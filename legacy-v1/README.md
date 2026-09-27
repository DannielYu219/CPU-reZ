# legacy-v1（固化版本）

WoA CPU-Z Bench 逆向 + NEON 移植的第一版定稿。详见 `bench_spec.md`（逆向规格）与
`INSTR_SET_MAPPING.md`（指令集映射）。

构建 / 运行：

```sh
clang -arch arm64 -O3 -mcpu=native -ffp-contract=off -Wall \
  -o cpuz_bench cpuz_bench_neon.c cpuz_bench_run.c -lm -lpthread
./cpuz_bench 1 11            # 单核，约 20 秒
./cpuz_bench 18 11           # 多核，约 20 秒
./cpuz_bench 1 11 --quick    # 快速版（<2 秒）
./cpuz_bench 18 11 --stress  # Stress（Ctrl-C 停）
```

文件：`cpuz_bench_neon.{h,c}`、`cpuz_bench_run.c`、`cpuz_bench_driver.c`（自检）、
`cpuz_bench_ref.h`（参考表）、`cpuz_bench.arm64-macos`（参照二进制）、成绩单与规格。
