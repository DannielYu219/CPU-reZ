#pragma once
// CPU-reZ: sparse-vector bench kernels mapped to SME streaming-SVE (+AMX/AVX notes).
// - Dense per-pixel arithmetic (hash fract, simplex skew, fBm) -> streaming SVE
//   (SVL-agnostic via svcntw; Apple M4/M5 SVL = 512 bit = 16x f32).
// - Sparse gathers (sin/grad table lookups; SME LUTv2 unavailable per sysctl)
//   stay scalar-per-lane (L1 hits), like the NEON version.
// - ZA outer-product / FMOPA / AMX GEMM: NOT applicable (no matmul in this
//   workload; flop analysis in SME_AMX_AVX_MAPPING.md). AMX reachable only via
//   Accelerate (documented, not used: op-reorder breaks bit-exactness).
// - SME exists only on some cores (P-cores); per-thread SIGILL-guarded probe
//   selects SME vs NEON fallback at worker startup.
// Build the SME TU with: -march=armv9-a+sme2 -ffp-contract=off
// (Apple clang 21 OK; -mcpu=native also defines __ARM_FEATURE_SME/SME2).
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Process-wide SME presence (sysctl hw.optional.arm.FEAT_SME). 0 => all NEON.
int cpuz_sme_present(void);
// Per-thread probe: 1 if streaming SVE executes here, else 0 (NEON fallback).
int cpuz_sme_available_on_this_thread(void);
// Streaming vector length in 32-bit lanes (svcntw); 0 if unavailable.
uint64_t cpuz_sme_vl_words(void);

// Dispatch tile renderer: uses SME streaming path when use_sme != 0,
// else the legacy NEON/scalar path. Pixel-identical in both.
void cpuz_tile_rgba_dispatch(const float *tab, uint8_t *dst, int w, int h, int stride,
                             float ox, float oy, float scale, int use_sme);

#ifdef __cplusplus
}
#endif
