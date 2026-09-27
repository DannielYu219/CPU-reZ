# CPU-reZ：Bench 稀疏向量 → SME/AMX 映射（含 AVX 对应）

- `SME_AMX_AVX_MAPPING.md`：主技术文档（稀疏性分析、SME 实现、ZA/AMX ruling、AVX 表、实测结论）。
- `cpuz_bench_sme.h` / `cpuz_bench_sme.c`：前端（sysctl 存在性 + 每线程 SIGILL-guard probe + 分发）。
  普通 flag 编译，绝不进 SVE。
- `cpuz_bench_sme_core.c`：streaming 核心（hash/simplex/tile + naked 入口 + microbench entry）。
  **必须** `-march=armv9-a+sme2 -ffp-contract=off -O3 -fno-vectorize -fno-slp-vectorize` 编译，
  且编后用反汇编扫描确认 streaming 区外无 SVE、区内无 AdvSIMD（血泪教训见主文档 §4）。
- `sme_test.c`（exactness/自检）、`sme_bench.c`（多线程吞吐）、`rez_bench.c`（端到端 `--auto/--neon/--sme`）。
- `cpuz_bench_neon.h`、`cpuz_bench_ref.h`：自包含拷贝（同步自 legacy-v1）。

一键构建见 `build.sh`。生产默认 NEON；SME 为 opt-in（当前硬件/负载下 workload 级约慢 4–7 倍，
bit-exact 已验证，见主文档 §6）。
