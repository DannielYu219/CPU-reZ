#!/bin/sh
# CPU-reZ build. NOTE: the two TUs need DIFFERENT flags (see SME_AMX_AVX_MAPPING.md §4).
set -e
cd "$(dirname "$0")"
clang -arch arm64 -O3 -mcpu=native -ffp-contract=off -Wall -c ../legacy-v1/cpuz_bench_neon.c -o neon_tu.o
clang -arch arm64 -march=armv9-a+sme2 -ffp-contract=off -O3 -fno-vectorize -fno-slp-vectorize -Wall -c cpuz_bench_sme_core.c -o sme_core.o
clang -arch arm64 -O2 -Wall -c cpuz_bench_sme.c -o sme_fe.o
clang -arch arm64 -O3 -mcpu=native -ffp-contract=off -Wall -I. -o rez_bench rez_bench.c neon_tu.o sme_core.o sme_fe.o -lm -lpthread
clang -arch arm64 -O2 -Wall -I. -o sme_test sme_test.c neon_tu.o sme_core.o sme_fe.o -lm
echo BUILD_OK
./sme_test 2>&1 | head -n 4
