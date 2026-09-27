# CPU-reZ：CPU-Z WoA Bench 逆向 → NEON/SME 可移植重写

[English summary below](#english-summary)

CPU-Z for Windows ARM64（`cpuz_arm64.exe` v1.06，Bench v17.01.64）的 Benchmark 全链路逆向工程与
跨平台重写：2D simplex procedural-map fBm 内核、多核调度、计时计分、参考表，以及
NEON 4-wide 与 SME streaming-SVE 双后端。

## 背景

- 原机 Bench = 1024×1024 RGBA procedural map，每像素 2×10-octave simplex fBm；
  两层线程（waiter + supervisor）+ `GROUP_AFFINITY` 硬绑核 + barrier；
  `QPC/QPF` 计时；`measured×100/ref` 显示公式；13 项参考表（含 X Elite 820/9165）。
- 逆向规格全文见 [`bench_spec.md`](legacy-v1/bench_spec.md)（如一并归档）——本仓库聚焦 **CPU-reZ 重写层**。

> 注：本仓库 `legacy-v1/` 为逆向定版快照（含规格、成绩单、参照二进制信息），`CPU-reZ/` 为重写实现。
> （若只想要重写代码，看 `CPU-reZ/` 即可。）

## 目录结构

```text
CPU-reZ/
├── cpuz_bench_neon.h/.c    # 常量、标量参考实现、NEON 4-wide（生产默认）
├── cpuz_bench_sme.h        # SME 前端：存在性检查、每线程 probe、分发
├── cpuz_bench_sme_core.c   # SME streaming-SVE 核心（hash/simplex/tile，naked 入口）
├── cpuz_bench_run.c        # legacy 驱动（保留对照）
├── cpuz_bench_driver.c     # 自检小驱动（exactness/单测）
├── cpuz_bench_ref.h        # 参考表（13 CPU，静态数组 1:1）
├── rez_bench.c             # 端到端 bench（--auto/--neon/--sme/--quick/--stress）
├── sme_test.c / sme_bench.c# SME 自检与吞吐 harness
├── SME_AMX_AVX_MAPPING.md  # 稀疏→SME/AMX 映射技术文档 + AVX 对应表
└── build.sh                # 一键构建
```

## 构建（Apple Silicon Mac，Xcode clang）

```sh
./build.sh
# 细则：core TU 必须 -march=armv9-a+sme2 -ffp-contract=off -O3 -fno-vectorize -fno-slp-vectorize；
# 普通 TU 用 -mcpu=native（绝不能开 SVE，否则普通函数里会出现非 streaming SVE 而 fault，见文档 §4）。
```

## 运行

```sh
./rez_bench 1 11 --neon     # 单核 NEON
./rez_bench 18 11 --auto    # 多核，按 probe 自动选后端
./rez_bench 1 11 --sme      # 单核 SME（功能对、当前偏慢，见下）
./rez_bench 18 11 --stress  # Stress（Ctrl-C 停）
# 参考索引 0-12（11 = Snapdragon X Elite，820/9165）
```

## 实测（Apple M5 Pro，18 核；原机 ARM64 VM 16 核：1560.9 / 19401.7 / ratio 12.4）

| 后端 | 单核 | 多核 18T | ratio |
|---|---|---|---|
| NEON（生产默认） | 1554（+0.5%）| 19647（+1.3%）| 12.9 |
| SME streaming | 212（同 work，诚实低分） | — | — |

- exactness：生产形态 4 线程×50 tile **200/200 exact**（`memcmp` 整 tile）。
- SME 在此机此态偏慢（gather-bound + streaming 开销），故默认 NEON、SME opt-in；计分链不受影响。

## 关键技术点（详见 `SME_AMX_AVX_MAPPING.md`）

1. 稀疏 gather（hash 查表）保持标量（SME 无 gather，本机缺 LUTv2）；稠密算术 16-wide。
2. ZA 外积/FMOPA/AMX-GEMM **不适用**（无 matmul 结构，flop 证明在文档）。
3. streaming 区禁 AdvSIMD、普通 TU 禁 SVE、naked 手写入口、per-thread SIGILL-guard probe、交错会话约束。
4. AVX 对应表：AVX2 8-wide ↔ SVL512 半条，AVX-512 ↔ 1:1，FMA/掩码两坑两边通用。

## 许可

AGPL-3.0（见 `LICENSE`）。逆向手法与数值仅用于互操作/研究目的。

---

## English summary

Full-pipeline reverse engineering and portable rewrite of the CPU-Z for Windows ARM64
benchmark (2D simplex fBm kernel, thread scheduling, QPC timing/scoring, reference table),
with NEON and SME streaming-SVE backends. NEON is the production default (single-core 1554
vs 1545.6 reference, +0.5%); SME is opt-in, bit-exact in production shape. See
`SME_AMX_AVX_MAPPING.md` for the sparse→SME/AMX mapping and the AVX correspondence table.
