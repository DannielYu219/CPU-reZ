# 指令集映射技术文档：`cpuz_arm64.exe` Bench → 可移植 C/NEON（legacy-v1）

> 对象：`/Users/dannielyu/cpuz-arm64-macOS/cpuz_arm64.exe`（PE32+ ARM64，ImageBase `0x140000000`，v1.06，Bench v17.01.64）。
> 本目录为 legacy-v1 固化版本：`cpuz_bench_neon.{h,c}`（算法+计分）、`cpuz_bench_run.c`（调度驱动）、
> `cpuz_bench_driver.c`（自检小驱动）、`cpuz_bench_ref.h`（参考表）、`bench_spec.md`、`bench_results_m5pro.txt`、
> `cpuz_bench.arm64-macos`（参照二进制）。
> 构建：`clang -arch arm64 -O3 -mcpu=native -ffp-contract=off -Wall -o cpuz_bench cpuz_bench_neon.c cpuz_bench_run.c -lm -lpthread`

等价标准分两档（全文统一使用）：
- **bit-exact**：同输入逐 bit 一致（像素值、整数计分、查表索引）。凡原机用定点/整数语义处必须达到。
- **rate-equivalent**：吞吐一致即可（计时、调度开销、线程数），允许相差调度噪声。

---

## 1. 标量浮点映射（`0x140011DE0` hash / `0x140011F20` simplex / `0x1400120D0` 外层）

| 原机指令（示例地址） | 精确语义 | C 映射 | NEON 映射 | 坑与处理 |
|---|---|---|---|---|
| `fmov s16,#4.0` / `fmov s17,#1.0` 等（`0x140011D38`，`0x140011E20`） | 立即数精确表示 | 同值 float 字面量 | `vdupq_n_f32` | 无 |
| `fmul s17,s19,s16`（`0x140011DEC`） | IEEE round-to-nearest 单次舍入 | `a*b`（配 `-ffp-contract=off`） | `vmulq_f32`，**禁用 `vmla/vmlaq`** | 融合乘加少一次舍入，1ulp 误差会翻转 corner 边界（实测 6 点中 2 点全零/非零翻转）。凡 `fmul+fadd` 必须拆成 `vmul+vadd` 两次舍入 |
| `fadd`/`fsub`（`0x140011DF8`/`0x140011E1C`） | 同上 | `+`/`-` | `vaddq_f32`/`vsubq_f32`，左结合与标量一致 | 累加顺序不可换（`s13`/`s12` fBm 链） |
| `fdiv s9,s17,s16`（`0x140011784` 计分；`fdiv d16` 见 sin） | IEEE 除法 | `/` | `vdivq_f32`（计分主路径实际走整数 `sdiv`，见 §3） | 除零只出现在计时除法，原机 `brk`，移植用 `abort()` |
| `fabs`（`0x1400121B0` 外层 `fabs s16,s16`） | 清符号位 | `fabsf` | `bic` 符号位（`vbicq_u32(...,0x80000000)`），与 `fabsf` 等价、无分支 | 无 |
| `fcmpe s18,#0.0; b.hs L`（`0x140011E28`） | `hs` ⟺ `>=`（含 -0.0；NaN 会进 `lo`？本 workload 全有限值，无 NaN） | `if (!(x >= 0)) x += 1` 写成 `if (x < 0)` 前必须确认与汇编分支极性一致 | `vcltq_f32` + `vbslq_f32` | NaN 下 C 与 NZCV 语义分叉——bench 坐标/中间值经审计无 NaN 源（除零已 `brk`），故等价成立 |
| `cset w8,lo` + `sub`（floor，`0x140011F5C`） | `lo` ⟺ `<`；产生 **0/1** 再减 | `if (x < tf) t -= 1` | `vand(mask,1)` 后减；**严禁直接减 mask**（mask=-1 会反号，实测翻车过） | 同上 NaN 注记 |
| `fcsel`（`0x140011FA4` 取角/钳位） | 条件选择 | `?:` | `vbslq_f32`；`max(0,t)` 用 `vmaxq_f32`（与 `fcmp+fcsel gt` 同构） | NaN 下 `vmax` 与 `fcsel` 传播规则不同——同上，无 NaN 源 |
| `fcvtzs w8,s17`（`0x140011E14` 取整；`0x140005DEC` 显示取整） | 向零截断；越界/NaN/Inf 为体系定义值（本 workload 下标/分数均为小有限值，不触发） | `(int32_t)x` | `vcvtq_s32_f32` | 越界行为 C-UB vs 体系饱和——输入已审计为小有限值 |
| `fcvtzu w8,s19`（`0x1400123B0` 颜色字节） | 向零截断到无符号 | `(uint32_t)x`（先钳 0..1） | 同左 | 同上 |
| `scvtf s16,w8` | 精确（32 位整数→float 全精确） | `(float)i` | `vcvtq_f32_s32` | 无 |
| `fcvt d16,s16` / `fcvt s0,d16`（sin 首尾、`0x140005E1C` 显示） | 按 IEEE 舍入拓宽/窄化 | `(double)f` / `(float)d` | N/A（标量路径） | 无 |
| `frinta`（sin `0x140220A78` 区段规约） | 就近偶舍入到整数（仍为浮点） | `rint()`/`nearbyint()`（**不是** `round()`，后者为远零） | N/A | sin 整体改调 `sinf()`（见 §4），此条仅文档化 |

## 2. SIMD/NEON 指令映射（memset、查表、杂项）

| 原机 | 语义 | 映射 |
|---|---|---|
| `dup v0.16b,w1` + `st1/st4 {…}[x10]`（`0x140220FF4+`，`memset 0x140220FE0`） | NEON 置零/填充 | 直接调 `memset()`（编译器自动展开为同类 NEON 序列；已确认该函数即 memset，非内核） |
| `movi d16,#0`（simplex `t` 清零，`0x140011F9C`） | 向量清零 | `vdupq_n_f32(0)` |
| `ldr s18,[x9,w8,sxtw #2]`（hash/梯度查表，`0x140011E50`） | 32 位 scaled 索引查表 | 同样表达式；NEON 版每 lane 标量 gather（L1 命中为主），算术部分 4-wide |
| `ldp/stp s18,s19,[x1]`（坐标/结果成对搬运，`0x140011DE0`/`0x140012020`） | 2×float 原子性无关，普通搬运 | `vld1q/vst1q` 或标量两次读写等价 |
| `ubfx x11,x8,#0,#32`（线程枚举，`0x14001146C`） | 取低 32 位 | `(uint32_t)` 转换 |
| `sdot z0.s,…`（`0x140011F14` 等处反汇编残留） | **数据非代码**：`.text` 内嵌 float 常量（`1024.0` 等）被误反汇编 | 按 §6 常量表以 float 字面量还原，不映射指令 |

## 3. 整数/定点映射（计时计分核心，一律 bit-exact）

| 原机（地址） | 语义 | 映射 |
|---|---|---|
| `mul x9,x8,x9`（64 位低部，`0x140011608`，`freq*w24`） | 低 64 位回绕 | `(uint64_t)freq*(uint64_t)work` 取低部（无符号运算，无 UB） |
| `sdiv x9,x9,x8`（`0x140011618`） | 向零截断；除零 `brk #0xF004` | `/`（C 同为截断）+ `freq==0/delta==0 → abort()`；`INT_MIN/-1` 在 C 为 UB，ARM 得 `INT_MIN`——输入恒正，不可达，已断言 |
| `smulh x8,x9,x8` + `asr x8,x8,#7` + `add x8,x8,x8,lsr #63`（`0x140011620`，`C=0x20C49BA5E353F7CF@0x1400117B8`） | 有符号高 64 位→算术右移 7→负数加 1（朝正无穷方向的偏置舍入） | `__int128 p=(__int128)q*C; hi=p>>64; s=hi>>7; s+=(uint64_t)s>>63;` |
| `scvtf s16,x8`（`0x14001162C`） | 整数→float（大数有舍入，行为一致即可） | `(float)s` |
| `sxtw x9,w24`（`0x140011604`） | 32→64 符号扩展（`w24` 为 32 位累加器！） | `(int64_t)(int32_t)work`；`cpuz_score_by_ms` 的 work 按 32 位取——溢出回绕行为与原机一致 |
| `sub x9,x0,x25; mul x9,x9,#1000`（`0x1400116F4` ms 路径） | 同上，64 位低部语义 | 同 `mul` 行 |
| `sbfiz x2,w8,#2`（`bufsize=w*h<<2`，`0x140004AA8`） | `w*h*4` | `(size_t)w*h*4` |
| `umulh/mul/umaddl`（分配大小溢出检查，`0x140003F24`） | 高部非零→饱和/失败路径 | `size_t` 运算 + 失败 `abort`（本移植固定 4MiB/128B，经审计不触发） |
| `csel/cinc/csinc`（`0x140011F5C` floor、`0x1400116DC` 计数） | 无分支选择 | `?:` / `vand+sub`（见 floor 坑） |
| `cbnz/cbz/tbz` | 条件跳转 | `if` |

## 4. 数学库映射

| 原机 | 映射 | 偏差说明 |
|---|---|---|
| `sin 0x1402209D8`（自研：区段规约+多项式+表 `0x1402A7650`，仅用于 4096 点正弦表初始化 `0x140011D10`：`tab[i]=sin(i*2π/1024*4)`） | `sinf()` | ≤1ulp 漂移可接受：表只影响像素**值**，不影响**工作量/计时**（计分是 rate-based）。已在文档与代码注释中声明 |
| `memset 0x140220FE0` | `memset()` | 等价 |

## 5. 线程/同步/系统映射

| 原机（Win32） | 地址/说明 | 移植（macOS/Linux） |
|---|---|---|
| `CreateThread(0,0,fn,param,CREATE_SUSPENDED=4,&tid)` | `0x14000492C` 等 | `pthread_create`（先建后用 barrier 起跑，等价于 suspend+resume 顺序） |
| `SetThreadPriority(h,0/2)` | IAT `0x140234938` | `pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE)`（macOS 无精细优先级；语义≈不被降频/不限 E 核） |
| `ResumeThread` | IAT `0x140234930` | barrier 起跑（`xbarrier`，见下） |
| `WaitForSingleObjectEx(h,-1,1)` barrier（waiter `0x140011328`） | 全局事件 `0x140307700` | `xbarrier_t`（cond+mutex；macOS 无 `pthread_barrier`，自研，语义同：N+1 会合） |
| `Sleep(0)` 自旋收集（`0x1400115D0`） | `IAT+0x920` | barrier 会合替代（循环语义等价：全员完成才下一 pass） |
| `Sleep(10)` 两 sub-test 间隔 | supervisor `0x140004B28` | `usleep(10ms)` |
| `QueueUserAPC(fn=0x140011218,thread,param)` **逐行派发** + `w23` 计数器 | `0x1400116B0` | 主线程 `fetch_add` 行游标（`CPUZ_CHUNK_ROWS=2`，原机粒度 1 行；像素级等价，尾部效应更小）。**静态分区+barrier 在异构核上被 E 核拖尾是 v2 多核偏低的主因，已修正** |
| `Set/Reset/CreateEventW` | 仅析构/teardown 唤醒（`0x140003CCC`/`0x1400044D0`） | 不移植（teardown 只需 join） |
| `LoadLibraryW("kernel32")+GetProcAddress(Get/SetThreadGroupAffinity)` + `GROUP_AFFINITY{mask,group}`（`0x140004644`）| 静态 IAT 无此二函数，必须动态取；`All/P/P#2/E/LP-Cores` 下拉决定 target group | macOS 无法硬绑核：QoS 近似；Windows 移植版应调回 `SetThreadGroupAffinity`（预留注释）。`w28=-1` 表全部、`group id` 表单簇的过滤语义保留 |
| `GetActive/MaximumProcessorCount`、`GetLogicalProcessorInformation(Ex)`（字符串 `0x29CAD8+`，动态取） | 线程数来源 | `sysctl hw.ncpu` / `pthread`（`N` 由命令行传入，默认全核） |

## 6. 计时映射（比值不变性证明）

- 原机：`QueryPerformanceCounter/频率`（IAT `0x140234780/88`），`freq=[timer+0x10]`，`delta=t1-t0`，`ms=delta*1000/freq`，`score=w24/ms`（`0x14001175C`），另有 `GetTickCount64` 兜底。
- 移植：`clock_gettime(CLOCK_MONOTONIC)` ns，`freq=1e9`。`score` 只依赖 `work/delta_ticks` 比值与常数 `C` 缩放（§3），频率单位在除法中约掉——故跨平台 rate 可比，显示层再经参考归一（§7）。
- `CTimer` 虚函数间接计时（Start=`QPC@0x14002A440` / `GetTick64@0x14002A410`）→ 移植直接调时钟，无行为差。

## 7. 计分/显示映射

- `CBenchScore`（`COL 0x1402B5218`，vtable `0x140299030`，单虚函数 `0x1400043F0`）：`[0x8]=-1<<32`，`[0x10]=[0x14]=-1.0f` → 结构体初值照抄。
- supervisor（`0x140004A58`）：mode0 两遍 `10000`（`0x2710`）分存 `[score+0x14]`（`0x28` 内核）/`[score+0x10]`（`0x20` 内核），中 `Sleep(10)`；mode3 `4000`（`0xFA0`）；mode1 单发 `-1`（warmup/校准语义，移植 warmup 即此）。
- raw 双路径（`0x1400115FC` / `0x14001175C`）→ `cpuz_score_raw` / `cpuz_score_by_ms`（§3，bit-exact，含 `div0→abort` 对应 `brk`）。
- 显示层（`0x140005DAC+` 四处同构）：`display=(int)(measured*100.0/ref)`（`s8=100.0@0x140006298`，`fcvtzs` 截断）→ `SetDlgItemInt`；"This Processor" 走 `swprintf`（`%.1f@0x298408`）；Ratio 走 `fdiv`；图表上限 `max(8000.0,候选+10)`（`0x140006910`，`0x45FA0000`）。
- 参考表 `0x1402FA4F0` 13 项全量抄入 `cpuz_bench_ref.h`（`{name,id=核数,single,multi}`，`id=-1` 结束；X Elite 820/9165 作默认锚点）。
- 原生单位说明：移植按 rows 计数，headline=`(int)(raw*256)`（256=width/4，即 pixels/ms÷4）。`256` 系标定值：单核 3.44×256=881≈X Elite 820 且 ratio 相消（11.09 vs 11.18）自洽；÷4 在 asm 侧的具体字段（`w13` 侧或 formatter 对象 `0x140299408→0x14004C230` 内）待 Windows ARM 动态钉死（spec §9.3 已立项）。

## 8. `.text` 内嵌常量表（数据非代码）

| 地址 | 值 | 用途 |
|---|---|---|
| `0x1400117B8` | u64 `0x20C49BA5E353F7CF` | raw 路径归一（§3） |
| `0x140011DD8/DC` | `2π`，`1/1024` | 正弦表生成（×4.0 得 `tab[i]`） |
| `0x140011F00/04/08/0C/10` | `183.3`，`1/(2π)`，`269.5`，`311.7`，`127.1` | hash 点积对 |
| `0x140011F14/18` | `1024.0`，`43758.546875` | fract→表下标；经典 `fract(sin·43758.5)` hash |
| `0x1400120C0/C4` | `F2=0.3660254`，`G2=0.2113248` | 2D simplex skew/unskew（`F2=(√3-1)/2`，`G2=(3-√3)/6`） |
| `0x140012430-6C` | `1.1,1/1024,2.2,3.3,40,1.2,1.6,0.6,0.8,0.7,0.4,0.2,0.1,0.9,0.3,255` | fBm 振幅/频率游走与颜色映射 |
| `0x140006298` | `100.0f` | 显示百分比系数 |
| `0x1400069A8` | `8000.0f` | 图表上限初值 |

## 9. 已知偏差清单（诚实项）

1. `sinf()` vs 自研 sin ≤1ulp：只改像素值，不改工作量/分数。
2. 派发粒度 2 行 vs 原机 1 行：像素级等价，尾部更小（多核更快，属优化而非偏离）。
3. macOS 无硬亲和：QoS 近似；E 核混编残差见成绩单（ratio 级已对齐）。
4. `K=256` 单位换算待动态钉死（§7）。
5. v1.05（截图）vs v1.06（二进制）workload 漂移；热衰/调度噪声：忠实版单核 ±2%、多核短测 ±5%（`--quick` 仅迭代用）。

## 10. 验证矩阵（本目录可复现）

- `cpuz_bench_driver.c` 自检：6 点 hash/simplex 标量vsNEON bit-exact（修复 `vmla` 融合与 `cset` mask 反号两个 bug 后全过）；64×8 tile `memcmp` 一致。
- 计分单测：`score_raw(1e9,1024,1e8)=10`，`ms=100`，timeout 边界（9.9s→0，10s→1，-1→0）。
- 缩放曲线（M5 Pro）：1T 1565 → 6T 8456 → 12T 14563 → 16T ~19000 → 18T 19647。
- 终数：单核 1518（VM 1560.9，-1.8%）、多核 18T 19647（VM 16 核 19401.7，+1.3%）、ratio 12.32（VM 12.4）。
