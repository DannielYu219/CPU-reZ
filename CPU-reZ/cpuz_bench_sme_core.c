// CPU-reZ SME streaming core. Compile ONLY this file with:
//   clang -arch arm64 -march=armv9-a+sme2 -ffp-contract=off -O3 -fno-vectorize -fno-slp-vectorize
// Contains ONLY __arm_streaming pure-compute functions (no libc calls, no signal
// handling): keeps ALL SVE instructions inside streaming mode. Apple Silicon has
// no non-streaming SVE, so any SVE emitted in ordinary code faults (seen: `cntd`
// in a plain prologue). The frontend TU (cpuz_bench_sme.c, plain -mcpu=native)
// holds probe/dispatch and calls in via ordinary BL; mode switch happens in the
// callee prologue, inside the SIGILL guard.
#include "cpuz_bench_neon.h"
#if defined(__ARM_FEATURE_SME)
#include <arm_sme.h>
#include <arm_sve.h>

// ---- streaming helpers (same op order per lane as scalar) ----
static inline svfloat32_t fract_pos_sme(svbool_t pg, svfloat32_t v) {
  svint32_t t = svcvt_s32_f32_x(pg, v);          // fcvtzs trunc
  svfloat32_t tf = svcvt_f32_s32_x(pg, t);       // scvtf
  svfloat32_t f = svsub_f32_x(pg, v, tf);        // signed fract
  svbool_t neg = svcmplt_f32(pg, f, svsub_f32_x(pg, v, v)); // +0.0 via self-sub (SVE)
  return svsel_f32(neg, svadd_f32_x(pg, f, svdup_n_f32(1.0f)), f);
}
static inline svint32_t floor_sme(svbool_t pg, svfloat32_t v) {
  svint32_t t = svcvt_s32_f32_x(pg, v);
  svbool_t lo = svcmplt_f32(pg, v, svcvt_f32_s32_x(pg, t)); // cset -> bool
  svint32_t dec = svsel_s32(lo, svdup_n_s32(1), svdup_n_s32(0)); // 0/1, NOT mask
  return svsub_s32_x(pg, t, dec);
}

// hash2 over a predicate-masked chunk: xs/ys input, o0/o1 output (inactive lanes untouched).
static inline void hash2_chunk(const float *tab, svbool_t pg,
                               svfloat32_t x, svfloat32_t y,
                               svfloat32_t *o0, svfloat32_t *o1) {
  svfloat32_t dotA = svadd_f32_x(pg, svmul_f32_x(pg, y, svdup_n_f32(CPUZ_F_183_3)),
                                       svmul_f32_x(pg, x, svdup_n_f32(CPUZ_F_269_5)));
  svfloat32_t dotB = svadd_f32_x(pg, svmul_f32_x(pg, y, svdup_n_f32(CPUZ_F_311_7)),
                                       svmul_f32_x(pg, x, svdup_n_f32(CPUZ_F_127_1)));
  svfloat32_t one = svdup_n_f32(1.0f);
  svfloat32_t c43758 = svdup_n_f32(CPUZ_F_43758);
  // first output from dotB
  {
    svfloat32_t q = svmul_f32_x(pg, dotB, svdup_n_f32(CPUZ_F_INV2PI));
    svfloat32_t fr = fract_pos_sme(pg, q);
    svfloat32_t idxf = svmul_f32_x(pg, svmul_f32_x(pg, fr, svdup_n_f32(CPUZ_F_1024)),
                                         svdup_n_f32(4.0f));
    // sparse gather: spill idx, scalar table loads, reload (SME has no gather; LUTv2 absent)
    uint64_t vl = svcntw();
    static _Thread_local int32_t idxb[64]; static _Thread_local float tb[64];
    svst1_s32(pg, idxb, svcvt_s32_f32_x(pg, idxf));
    for (uint64_t k = 0; k < vl; k++) tb[k] = tab[(uint32_t)idxb[k] & 4095];
    svfloat32_t a = svmul_f32_x(pg, svld1_f32(pg, tb), c43758);
    svfloat32_t h = svsub_f32_x(pg, a, svcvt_f32_s32_x(pg, svcvt_s32_f32_x(pg, a)));
    *o0 = svsub_f32_x(pg, svadd_f32_x(pg, h, h), one);
  }
  // second output from dotA
  {
    svfloat32_t q = svmul_f32_x(pg, dotA, svdup_n_f32(CPUZ_F_INV2PI));
    svfloat32_t fr = fract_pos_sme(pg, q);
    svfloat32_t idxf = svmul_f32_x(pg, svmul_f32_x(pg, fr, svdup_n_f32(CPUZ_F_1024)),
                                         svdup_n_f32(4.0f));
    uint64_t vl = svcntw();
    static _Thread_local int32_t idxb[64]; static _Thread_local float tb[64];
    svst1_s32(pg, idxb, svcvt_s32_f32_x(pg, idxf));
    for (uint64_t k = 0; k < vl; k++) tb[k] = tab[(uint32_t)idxb[k] & 4095];
    svfloat32_t a = svmul_f32_x(pg, svld1_f32(pg, tb), c43758);
    svfloat32_t h = svsub_f32_x(pg, a, svcvt_f32_s32_x(pg, svcvt_s32_f32_x(pg, a)));
    *o1 = svsub_f32_x(pg, svadd_f32_x(pg, h, h), one);
  }
}

static inline svfloat32_t simplex_chunk(const float *tab, svbool_t pg,
                                        svfloat32_t xin, svfloat32_t yin) {
  svfloat32_t F2 = svdup_n_f32(CPUZ_F_F2), G2 = svdup_n_f32(CPUZ_F_G2);
  svfloat32_t s = svadd_f32_x(pg, xin, yin);
  svfloat32_t se = svmul_f32_x(pg, s, F2);
  svint32_t ii = floor_sme(pg, svadd_f32_x(pg, se, xin));
  svint32_t jj = floor_sme(pg, svadd_f32_x(pg, se, yin));
  svfloat32_t fi = svcvt_f32_s32_x(pg, ii), fj = svcvt_f32_s32_x(pg, jj);
  svfloat32_t t = svmul_f32_x(pg, svadd_f32_x(pg, fi, fj), G2);
  svfloat32_t x0 = svadd_f32_x(pg, svsub_f32_x(pg, xin, fi), t);
  svfloat32_t y0 = svadd_f32_x(pg, svsub_f32_x(pg, yin, fj), t);
  svbool_t x0gty0 = svcmpgt_f32(pg, x0, y0);
  svfloat32_t z0 = svsub_f32_x(pg, xin, xin); // +0.0 (SVE; svdup(0) lowers to NEON movi)
  svfloat32_t i1 = svsel_f32(x0gty0, svdup_n_f32(1.0f), z0);
  svfloat32_t j1 = svsel_f32(x0gty0, z0, svdup_n_f32(1.0f));
  svfloat32_t x1 = svadd_f32_x(pg, svsub_f32_x(pg, x0, i1), G2);
  svfloat32_t y1 = svadd_f32_x(pg, svsub_f32_x(pg, y0, j1), G2);
  svfloat32_t two = svdup_n_f32(2.0f);
  svfloat32_t x2 = svadd_f32_x(pg, svsub_f32_x(pg, x0, svdup_n_f32(1.0f)), svmul_f32_x(pg, G2, two));
  svfloat32_t y2 = svadd_f32_x(pg, svsub_f32_x(pg, y0, svdup_n_f32(1.0f)), svmul_f32_x(pg, G2, two));
  // lattice coords straight from the float vectors (fi/fj hold (float)ii/jj already):
  // avoids scalar (float)int traffic that the compiler lowers to AdvSIMD forms.
  svfloat32_t one = svdup_n_f32(1.0f);
  svfloat32_t gx0, gy0, gx1, gy1, gx2, gy2;
  hash2_chunk(tab, pg, fi, fj, &gx0, &gy0);
  hash2_chunk(tab, pg, svadd_f32_x(pg, fi, i1), svadd_f32_x(pg, fj, j1), &gx1, &gy1);
  hash2_chunk(tab, pg, svadd_f32_x(pg, fi, one), svadd_f32_x(pg, fj, one), &gx2, &gy2);
  svfloat32_t z = svsub_f32_x(pg, xin, xin), half = svdup_n_f32(0.5f); // =0, SVE
  svfloat32_t t0 = svmax_f32_x(pg, z, svsub_f32_x(pg, svsub_f32_x(pg, half, svmul_f32_x(pg, x0, x0)), svmul_f32_x(pg, y0, y0)));
  svfloat32_t t1 = svmax_f32_x(pg, z, svsub_f32_x(pg, svsub_f32_x(pg, half, svmul_f32_x(pg, x1, x1)), svmul_f32_x(pg, y1, y1)));
  svfloat32_t t2 = svmax_f32_x(pg, z, svsub_f32_x(pg, svsub_f32_x(pg, half, svmul_f32_x(pg, x2, x2)), svmul_f32_x(pg, y2, y2)));
  svfloat32_t t02 = svmul_f32_x(pg, t0, t0), t12 = svmul_f32_x(pg, t1, t1), t22 = svmul_f32_x(pg, t2, t2);
  svfloat32_t n0 = svmul_f32_x(pg, svmul_f32_x(pg, t02, t02),
                               svadd_f32_x(pg, svmul_f32_x(pg, gx0, x0), svmul_f32_x(pg, gy0, y0)));
  svfloat32_t n1 = svmul_f32_x(pg, svmul_f32_x(pg, t12, t12),
                               svadd_f32_x(pg, svmul_f32_x(pg, gx1, x1), svmul_f32_x(pg, gy1, y1)));
  svfloat32_t n2 = svmul_f32_x(pg, svmul_f32_x(pg, t22, t22),
                               svadd_f32_x(pg, svmul_f32_x(pg, gx2, x2), svmul_f32_x(pg, gy2, y2)));
  return svadd_f32_x(pg, svadd_f32_x(pg, n0, n1), n2);
}

// One streaming tile: whole tile inside streaming mode (amortizes entry cost).
// SVL-agnostic row loop via svwhilelt.
// NOTE: deliberately NOT __arm_streaming: executes only inside the streaming
// window opened by cpuz_sme_core_tile (manual SMSTART/SMSTOP). The attribute
// codegen emitted SVE before any SMSTART, which faults.
typedef struct { float ox, oy, scale; } sme_tile_params_t;
void cpuz_sme_tile_worker(const float *tab, uint8_t *dst, int w, int h,
                          int stride, const sme_tile_params_t *prm);
void cpuz_sme_tile_worker(const float *tab, uint8_t *dst, int w, int h,
                          int stride, const sme_tile_params_t *prm) {
  float ox = prm->ox, oy = prm->oy, scale = prm->scale;
  for (int j = 0; j < h; j++) {
    uint8_t *row = dst + (size_t)j * stride;
    float yb = (oy + j) * scale;
    for (int i = 0; i < w;) {
      svbool_t pg = svwhilelt_b32((uint32_t)i, (uint32_t)w);
      uint64_t vl = svcntw();
      static _Thread_local float xb[64];
      for (uint64_t k = 0; k < vl; k++) xb[k] = (ox + i + (int)k) * scale;
      svfloat32_t vx0 = svld1_f32(pg, xb);
      svfloat32_t vy0 = svdup_n_f32(yb);
      // loop1: 10 octaves, fabs accumulate
      svfloat32_t ax = vx0, ay = vy0, s13 = svsub_f32_x(pg, vx0, vx0); // =0, SVE (svdup(0) lowers to NEON movi!)
      svfloat32_t amp = svdup_n_f32(CPUZ_F_0_8), fr = svdup_n_f32(1.0f);
      svfloat32_t c16 = svdup_n_f32(CPUZ_F_1_6), c06 = svdup_n_f32(CPUZ_F_0_6);
      svfloat32_t c07 = svdup_n_f32(CPUZ_F_0_7), c11 = svdup_n_f32(CPUZ_F_1_1);
      for (int o = 0; o < 10; o++) {
        svfloat32_t n = simplex_chunk(tab, pg, svmul_f32_x(pg, ax, fr), svmul_f32_x(pg, ay, fr));
        svfloat32_t an = svabs_f32_x(pg, n);
        s13 = svadd_f32_x(pg, s13, svmul_f32_x(pg, an, amp));
        svfloat32_t nx = svadd_f32_x(pg, svmul_f32_x(pg, ax, c16), svmul_f32_x(pg, ay, c06));
        svfloat32_t ny = svsub_f32_x(pg, svmul_f32_x(pg, ax, c06), svmul_f32_x(pg, ay, c16));
        ax = nx; ay = ny;
        amp = svmul_f32_x(pg, amp, c07); fr = svmul_f32_x(pg, fr, c11);
      }
      // loop2: base*2.2, 10 octaves
      svfloat32_t bx = svmul_f32_x(pg, vx0, svdup_n_f32(CPUZ_F_2_2));
      svfloat32_t by = svmul_f32_x(pg, vy0, svdup_n_f32(CPUZ_F_2_2));
      svfloat32_t s12 = svsub_f32_x(pg, vy0, vy0); // =0, SVE (see above)
      amp = svdup_n_f32(CPUZ_F_0_8);
      svfloat32_t c12 = svdup_n_f32(CPUZ_F_1_2), c40 = svdup_n_f32(CPUZ_F_40);
      for (int o = 0; o < 10; o++) {
        svfloat32_t n = simplex_chunk(tab, pg, bx, by);
        s12 = svadd_f32_x(pg, s12, svmul_f32_x(pg, n, amp));
        bx = svadd_f32_x(pg, svmul_f32_x(pg, bx, c12), c40);
        by = svmul_f32_x(pg, by, c12);
        amp = svmul_f32_x(pg, amp, c07);
      }
      // color map: clamp 0..1 (fcsel lo semantics), *255, fcvtzu, store RGBA
      svfloat32_t v = svadd_f32_x(pg, s12, s13);
      svfloat32_t fz = svsub_f32_x(pg, v, v); // =0 for clamps (SVE)
      svfloat32_t r = svadd_f32_x(pg, svmul_f32_x(pg, v, svdup_n_f32(CPUZ_F_0_4)), svdup_n_f32(CPUZ_F_0_2));
      svfloat32_t g = svadd_f32_x(pg, svmul_f32_x(pg, v, svdup_n_f32(CPUZ_F_0_4)), svdup_n_f32(CPUZ_F_0_3));
      svfloat32_t b = svadd_f32_x(pg, svmul_f32_x(pg, v, svdup_n_f32(CPUZ_F_0_4)), svdup_n_f32(CPUZ_F_0_1));
      r = svmax_f32_x(pg, fz, r); g = svmax_f32_x(pg, fz, g); b = svmax_f32_x(pg, fz, b);
      r = svmin_f32_x(pg, svdup_n_f32(1.0f), r); g = svmin_f32_x(pg, svdup_n_f32(1.0f), g); b = svmin_f32_x(pg, svdup_n_f32(1.0f), b);
      // NOTE scalar clamps via fcmpe+fcsel lo (x<0 -> 0) then upper compare; svmax/svmin
      // match for finite inputs (no NaN in this pipeline); inactive lanes ignored.
      static _Thread_local uint32_t ri[64], gi[64], bi[64];
      svst1_u32(pg, ri, svcvt_u32_f32_x(pg, svmul_f32_x(pg, r, svdup_n_f32(255.0f))));
      svst1_u32(pg, gi, svcvt_u32_f32_x(pg, svmul_f32_x(pg, g, svdup_n_f32(255.0f))));
      svst1_u32(pg, bi, svcvt_u32_f32_x(pg, svmul_f32_x(pg, b, svdup_n_f32(255.0f))));
      for (uint64_t k = 0; k < vl; k++) {
        // active-lane guard via predicate popcount-free check
        if (i + (int)k >= w) break;
        row[(i + k) * 4 + 0] = (uint8_t)ri[k];
        row[(i + k) * 4 + 1] = (uint8_t)gi[k];
        row[(i + k) * 4 + 2] = (uint8_t)bi[k];
        row[(i + k) * 4 + 3] = 255;
      }
      i += (int)svcntp_b32(svptrue_b32(), pg); // active lanes (exact for w%VL==0)
    }
  }
}


// Naked entries: hand-written scalar-only prologue, SMSTART executes before ANY
// compiler-generated code (which elsewhere emitted VL-dependent prologue ops like
// `addvl` ahead of the first SMSTART). Args pass through untouched (x0-x7/s0-s7).
__asm__(
".text\n"
".globl _cpuz_sme_core_vl\n"
"_cpuz_sme_core_vl:\n"
"  smstart sm\n"
"  cntw x0\n"
"  smstop sm\n"
"  ret\n"
".globl _cpuz_sme_core_tile\n"
"_cpuz_sme_core_tile:\n"
"  stp x29, x30, [sp, #-176]!\n"
"  stp x19, x20, [sp, #16]\n"
"  stp x21, x22, [sp, #32]\n"
"  stp x23, x24, [sp, #48]\n"
"  stp x25, x26, [sp, #64]\n"
"  stp x27, x28, [sp, #80]\n"
"  stp d8, d9, [sp, #96]\n"
"  stp d10, d11, [sp, #112]\n"
"  stp d12, d13, [sp, #128]\n"
"  stp d14, d15, [sp, #144]\n"
"  str s0, [sp, #160]\n"
"  str s1, [sp, #164]\n"
"  str s2, [sp, #168]\n"
"  add x5, sp, #160\n"
"  mov x29, sp\n"
"  smstart sm\n"
"  bl _cpuz_sme_tile_worker\n"
"  smstop sm\n"
"  ldp d14, d15, [sp, #144]\n"
"  ldp d12, d13, [sp, #128]\n"
"  ldp d10, d11, [sp, #112]\n"
"  ldp d8, d9, [sp, #96]\n"
"  ldp x27, x28, [sp, #80]\n"
"  ldp x25, x26, [sp, #64]\n"
"  ldp x23, x24, [sp, #48]\n"
"  ldp x21, x22, [sp, #32]\n"
"  ldp x19, x20, [sp, #16]\n"
"  ldp x29, x30, [sp], #176\n"
"  ret\n"
);
// Microbench entry: dense chained FMUL/FADD over streaming vectors (no gathers,
// no scalar work). Measures raw streaming-arithmetic throughput for the doc.
void cpuz_sme_ubench(long reps, float *sink);
__asm__(
".text\n"
".globl _cpuz_sme_ubench\n"
"_cpuz_sme_ubench:\n"
"  stp x29, x30, [sp, #-32]!\n"
"  mov x29, sp\n"
"  smstart sm\n"
"  bl _cpuz_sme_ubench_worker\n"
"  smstop sm\n"
"  ldp x29, x30, [sp], #32\n"
"  ret\n"
);
void cpuz_sme_ubench_worker(long reps, float *sink);
void cpuz_sme_ubench_worker(long reps, float *sink) {
  uint64_t vl = svcntw();
  (void)vl;
  svbool_t pg = svptrue_b32();
  svfloat32_t vx = svdup_n_f32(1.5f), vy = svdup_n_f32(0.0f);
  svfloat32_t a = svdup_n_f32(1.0001f), b = svdup_n_f32(0.9999f);
  for (long i = 0; i < reps; i++) {
    vy = svadd_f32_x(pg, svmul_f32_x(pg, svmul_f32_x(pg, vx, a), vx), svmul_f32_x(pg, vx, b));
    vy = svadd_f32_x(pg, svmul_f32_x(pg, svmul_f32_x(pg, vy, a), vy), svmul_f32_x(pg, vy, b));
    if ((i & 32767) == 0) { svst1_f32(pg, sink, vy); vx = svld1_f32(pg, sink); }
  }
  svst1_f32(pg, sink, vy);
}
#endif // __ARM_FEATURE_SME
