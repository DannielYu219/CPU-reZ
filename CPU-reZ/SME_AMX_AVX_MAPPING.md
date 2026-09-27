# CPU-reZ：稀疏向量算法 → SME/AMX 映射技术文档（含 AVX 对应表）

> 目标：把 legacy-v1 中已映射到 NEON 的 bench 向量部分，进一步映射到矩阵/宽向量单元
> （SME streaming-SVE；AMX；x86 AVX 对应关系），给出可编译、可验证的代码与诚实结论。
> 本目录文件：`cpuz_bench_sme.{h,c}`（前端：探测+分发）、`cpuz_bench_sme_core.c`（streaming 核心）、
> `sme_test.c`（exactness/性能自检）、`sme_bench.c`（多线程吞吐）、`rez_bench.c`（端到端 bench，
> `--auto/--neon/--sme`）、`cpuz_bench_neon.h`/`cpuz_bench_ref.h`（自包含拷贝，同步自 legacy-v1）。

构建（TU 必须分开 flag，原因见 §4）：

```sh
clang -arch arm64 -O3 -mcpu=native -ffp-contract=off -Wall -c ../legacy-v1/cpuz_bench_neon.c -o neon_tu.o
clang -arch arm64 -march=armv9-a+sme2 -ffp-contract=off -O3 -fno-vectorize -fno-slp-vectorize -Wall -c cpuz_bench_sme_core.c -o sme_core.o
clang -arch arm64 -O2 -Wall -c cpuz_bench_sme.c -o sme_fe.o
clang -arch arm64 -O3 -mcpu=native -ffp-contract=off -Wall -I. -o rez_bench rez_bench.c neon_tu.o sme_core.o sme_fe.o -lm -lpthread
```

---

## 1. 稀疏性分析：bench 里什么是稀疏、什么是稠密

逐像素管线（`0x140011DE0 → 0x140011F20 → 0x1400120D0`，1024×1024，double-fBm 共 20 simplex/像素）：

| 阶段 | 形态 | flop/像素量级 | 可向量化性 |
|---|---|---|---|
| hash fract/点积（`dot/(2π)`、fract、×4096） | **稠密** elementwise | ~20 | streaming-SVE 16-wide，完美 |
| sin/梯度**查表**（每 hash 2 次 ×3 hash/simplex ×20 simplex ≈ 120 次/像素） | **稀疏 gather** | 地址随机（hash 索引） | SME 无 gather（且本机 `FEAT_SME_LUTv2=0`，连 LUT 指令都没有）→ 标量 gather，L1 命中为主 |
| simplex skew/floor/角点/t⁴·dot | **稠密** elementwise | ~60 | streaming-SVE，完美 |
| fBm 振幅/频率游走、颜色 clamp/scale | **稠密** | ~40 | streaming-SVE，完美 |
| 跨像素归约 | 无（像素独立） | 0 | 不需要 horizontal（省了一大类麻烦） |

结论：算术部分（~70% flops）可 16-wide，查表部分（~30% 时间，主导访存）只能 scalar。
没有 GEMM/卷积/外积结构（见 §2），所以矩阵单元无用武之地——映射目标是 **SME streaming-SVE 向量模式**，不是 ZA。

## 2. ZA 外积 / FMOPA / AMX：明确 ruled out（附 flop 证明）

- **FMOPA（SME 外积）/ AMX（Apple 矩阵协处理器 GEMM）**：只加速 `C += A·B` 型矩阵乘。本 workload 的"矩阵"只存在于名字里——
  实际是逐像素独立的多项式/hash，无任何 `(M,K)×(K,N)` 复用结构，算术强度 ≈ 3 flop/byte，roofline 落在访存/延迟区而非计算区。
  强行改写成外积形式 = 改算法 ≠ 1:1 移植。**结论：不用。**
- **AMX 可达路径**：Apple 不开放 AMX 指令，只能经 Accelerate（vDSP/BNNS，底层确实用 AMX/SME）。
  可映射点仅有 fBm 加权求和（`vDSP_vma` 类），但 Accelerate 会重排结合顺序 → 破坏 bit-exact，
  且小向量 dispatch 开销 > 收益。**结论：默认不用，留作 rate-only 路径备选（§6）。**

## 3. SME streaming-SVE 实现（`cpuz_bench_sme_core.c`）

- SVL-agnostic：`svcntw()` + `svwhilelt` 行循环（本机 SVL=512b=16×f32，运行时探得）。
- 逐 lane 操作顺序与标量**完全一致**（显式 `svmul+svadd`，永不 `svmla`；floor 用 `cmplt→sel 0/1→sub`，cset 语义；
  clamp 用 `svmax/svmin`，有限输入下等价于 `fcmpe+fcsel`）。
- 查表保持标量 gather（spill idx → 标量查 → reload），与 NEON 版同构。
- 整 tile 包在一个 streaming 区内（摊销进出成本）；区内无 libc/原子/系统调用。
- 验证：64×8/256×64 tile 与标量 `memcmp` **0 diff**；4 线程×50 tile 生产形态 **200/200 exact**。

## 4. 血泪换来的 streaming 规则（逐条实测验证过）

1. **streaming 区内禁 AdvSIMD**：`mov.16b`/`movi.2d`/q-reg 访存等一律 UNDEFINED（lldb 实锤两次）。
   编译器会自作聪明（float 参数传递用 `mov.16b`、零初始化用 `movi.2d`、标量 `(float)int` 循环降成 AdvSIMD 形态）。
   对策：worker 参数改结构体指针；`svdup(0.0)` 改自减清零；删 int 中转循环；**以反汇编扫描为准**（`movi.`/q-reg/vN.old 零命中）而不是"想当然"。
2. **`+sme` TU 里普通函数也会中招**：`+sme2` 隐含开 SVE，编译器在**非 streaming**函数序言都敢放 `cntd`（lldb 实锤），
   Apple Silicon 无非 streaming SVE，直接 fault。对策：**拆两个 TU**——core 只放 streaming 纯计算（`+sme2`），
   probe/分发放在普通 TU（plain `-mcpu=native`，绝无杂散 SVE），后者以普通 BL 调用前者（模式切换在被调方）。
3. **`__arm_streaming` 属性不可靠**：实测出现"trivial 函数不插 SMSTART"和"SMSTART 插在自家 SVE 序言之后"两种情况。
   对策：**naked 手写入口**（纯标量现场保存 → `smstart sm` 第一执行 → `bl worker` → `smstop sm` → 恢复返回），反汇编逐字节确认。
4. **交错会话约束**（未完全定根，行为确定）：`SME会话 → 标量FP工作 → SME会话` 的测试形态曾稳定复现偏差；
   而"启动 probe 一次、之后纯计算"的生产形态 200/200 exact。缓解：worker 启动 probe 一次，之后不再进出多余 SME 会话；
   该约束已写入驱动。机理疑为内核 SME 懒状态交互，待深挖（不影响生产路径）。
5. **E 核/异构**：SME 只在部分核可用 → 每线程 SIGILL-guard probe（`sigaction+sigsetjmp` 包 `smstart/cntw/smstop`），
   不可用则回 NEON。QoS 仍设 `USER_INTERACTIVE`。

## 5. AVX 对应表（x86 lineage / 未来 x64-AVX 化时用）

公开资料中 x64 bench 为标量 SSE2（无 AVX），下表为跨 ISA 等价映射（语义逐条对齐，FMA/掩码两处大坑标出）：

| x86（SSE/AVX） | ARM SME streaming-SVE | 说明 |
|---|---|---|
| 标量 `ADDSS/MULSS/DIVSS` | `svadd/svmul/svdiv + _x` 后缀逐 lane | 同舍入；`-ffp-contract=off` 两边都要 |
| `VFMADD132PS`（融合） | **必须拆** `svmul+svadd`（contract OFF） | 融合少一次舍入，1ulp 翻转 corner 边界（NEON 篇已实锤，同理） |
| `VCMPPS + VBLENDVPS` | `svcmplt + svsel` | 两边注意：x86 blend 用全 1 mask，ARM `cset` 系产 0/1——经 `vand(...,1)` 归一（floor 处实锤） |
| `VCVTTPS2DQ`（截断） | `svcvt_s32`（`fcvtzs`） | 同为向零截断；越界行为两边各自体系定义，bench 输入皆小有限值 |
| `VMAXPS/VMINPS`（钳位） | `svmax/svmin` | 有限输入下等价于 `fcmpe+fcsel`；NaN 传播不同（bench 无 NaN 源） |
| `VGATHERDPS`（稀疏查表） | 标量 gather（SME 无 gather；LUTv2 本机缺席） | 两边都是 gather 瓶颈，优化方向一致：查表合并/量化 |
| AVX2 256b（8×f32） | SME SVL512（16×f32）≈ 2 条 AVX2 | 宽度换算基准 |
| AVX-512 512b（16×f32） | SME SVL512 **1:1** | 本机 SVL=512，lane 级逐条可对 |
| MXCSR（FTZ/DAZ/舍入） | FPCR（streaming 共用同一寄存器，已验证=0） | 两边跑前建议显式读一次确认（本移植已做） |
| `VZEROUPPER`（AVX-SSE 转换惩罚） | 对应物：`smstart/smstop` 进出 + predicate 初始化 | 跨区调用摊销：整 tile 一个区（本实现），忌逐像素进出 |

## 6. 实测结果（M5 Pro，热机状态，同条件交替，诚实数据）

| 路径 | 单 tile 1024² | 备注 |
|---|---|---|
| 标量 | ~260ms | 基准 |
| NEON 4-wide | ~170ms（6.1 MPix/s） | 生产默认 |
| SME streaming 16-wide | ~1234ms（0.8 MPix/s） | 功能对、速度败（见下） |
| 端到端（rez_bench，4T）：NEON 23.5 MPix/s vs SME 3.3 MPix/s | | |
| 纯算术 microbench：NEON 18–29 GFLOP/s vs SME 7–10 GFLOP/s | | （合成数仅看比值；绝对值受编译器折叠与热衰影响） |

解读（不回避）：该 workload 是 gather-bound + 标量controls-heavy，16-wide 算术收益被查表 roundtrip、
predicate 开销和 streaming 区标量段稀释；叠加热机 E 核/SME 单元状态，SME 在此机此态不赢。
**生产默认保持 NEON；SME 以 `--sme` opt-in + 运行时 per-thread probe 保留**（硬件/散热/调优变化时可翻盘；
要翻盘的方向：查表合并、更大 streaming 区、ZA blocking——但后者需改算法，不属 1:1）。
计分链不受影响：同 work → 同公式 → 诚实低分（SME 单核 headline 212 vs NEON 1565，恰好证明了"同分不同速"）。

## 7. 运行

```sh
# 构建（注意两个 TU 的 flag 不同，见顶）
clang -arch arm64 -O3 -mcpu=native -ffp-contract=off -Wall -c ../legacy-v1/cpuz_bench_neon.c -o neon_tu.o
clang -arch arm64 -march=armv9-a+sme2 -ffp-contract=off -O3 -fno-vectorize -fno-slp-vectorize -Wall -c cpuz_bench_sme_core.c -o sme_core.o
clang -arch arm64 -O2 -Wall -c cpuz_bench_sme.c -o sme_fe.o
clang -arch arm64 -O3 -mcpu=native -ffp-contract=off -Wall -I. -o rez_bench rez_bench.c neon_tu.o sme_core.o sme_fe.o -lm -lpthread
./rez_bench 1 11 --neon    # 单核 NEON
./rez_bench 1 11 --sme     # 单核 SME（慢，但分诚实）
./rez_bench 1 11 --auto    # 按 probe 自动选
```

## 8. 验证结论与使用约束（实测数据）

### exactness（bit-exact vs 标量，`memcmp` 整 tile）
- 生产形态（worker 线程：启动 probe 一次→之后纯计算 tiles）：**4 线程×50 tile = 200/200 exact**；
  64×8、256×64、1024×1024 多配置 exact；跨 runs 确定性一致。
- 约束：**不要在计算之间穿插 probe 会话**（`SME会话 → 标量FP工作 → SME会话` 的测试形态曾稳定复现偏差，
  约 47705/65536 字节、首差异像素 1，确定性可复现；生产形态从未出现）。
  根因未完全隔离（候选：内核 SME 懒状态与信号/交错交互；或异构核 SME 行为差异如 FTZ；
  已排除：FPCR 非 streaming 侧读数恒 0、ZA 零引用、栈数组已 TLS 化、FMA/掩码类已逐项对齐），
  缓解即约束本身——生产驱动天然满足（probe-once），并有 200/200 为证。

### 性能（M5 Pro，热机，同条件交替，QoS 已设）
| 路径 | 1024² tile | 备注 |
|---|---|---|
| 标量 | ~260ms | 基准 |
| NEON 4-wide | ~170ms（6.1 MPix/s） | **生产默认** |
| SME streaming 16-wide | ~1234ms（0.8 MPix/s） | 功能对、速度败；端到端 4T：NEON 23.5 vs SME 3.3 MPix/s |
| 纯算术 microbench | NEON ~18–29 vs SME ~7–10 GFLOP/s | 合成数仅看比值（编译器折叠干扰绝对值） |

解读：本 workload 是 gather-bound（每像素 ~120 次标量查表 roundtrip）+ 标量控制段，
16-wide 算术收益被稀释；叠加 streaming 进出/predicate 开销与热机 E 核因素。
**SME 不适合此 workload 做默认加速**（与"宽即快"的直觉相反，数据说话）；
保留 `--sme` opt-in + per-thread probe（硬件/散热/调优变化时可翻盘；翻盘方向：查表合并、更大区、稠密化——均需改算法，不属 1:1）。
计分链不受影响：同 work → 同公式 → SME 跑出诚实的低分（单核 headline 212 vs NEON 1565），
恰好证明了"同分不同速"的链路正确性。

### AVX 一句话
x86bench 为标量 SSE2（无 AVX），§5 的表是跨 ISA 等价映射；若 x64 日后上 AVX，
按"AVX2 8-wide ↔ SME-SVL 2 条、AVX-512 ↔ SVL512 1:1"直接套，FMA/掩码两坑同样适用。
