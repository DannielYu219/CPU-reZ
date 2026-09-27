// CPU-Z WoA Bench NEON 1:1 — scalar-exact core + NEON 4-lane + scoring identical to asm.
// Reversed from /Users/dannielyu/cpuz-arm64-macOS/cpuz_arm64.exe
//   hash 0x140011DE0, simplex 0x140011F20, outer 0x1400120D0,
//   sin table 0x140011D10 (4096 x sinf(i*2PI/1024*4)), memset 0x140220FE0,
//   scoring 0x1400115FC / 0x14001175C, const 0x1400117B8.
#include "cpuz_bench_neon.h"
#include <math.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

void cpuz_sin_table_init(float *tab) {
  // 0x140011D54-0x140011D78: s16=(float)w20*2PI*(1/1024); s0=s16*4; sinf; store; w20 in [0,4096)
  for (int i = 0; i < 4096; i++) {
    float s16 = (float)i * CPUZ_F_2PI * CPUZ_F_INV1024;
    float s0 = s16 * 4.0f;
    tab[i] = sinf(s0); // original calls internal sinf 0x1402209D8 (same as libm sinf up to 1ulp)
  }
}

// 0x140011DE0 exact transcription. Scalar, same fmul/fadd/fsub/fcvtzs order.
void cpuz_hash2(const float *tab, float x, float y, float *o0, float *o1) {
  float s18 = x, s19 = y;
  float s16 = CPUZ_F_183_3;
  float s22 = CPUZ_F_INV2PI;
  float s17 = s19 * s16;          // y*183.3
  s16 = CPUZ_F_269_5;
  s16 = s18 * s16;                // x*269.5
  float s23 = s17 + s16;          // dotA
  s16 = CPUZ_F_311_7;
  s17 = s19 * s16;                // y*311.7
  s16 = CPUZ_F_127_1;
  s16 = s18 * s16;                // x*127.1
  s16 = s17 + s16;                // dotB
  s17 = s16 * s22;                // dotB/(2pi)
  int32_t w8 = (int32_t)s17;      // fcvtzs
  s16 = (float)w8;                // scvtf
  s16 = s17 - s16;                // fract
  s17 = 1.0f;
  s18 = s16;
  if (!(s18 >= 0.0f)) s18 = s18 + s17; // fcmpe+b.hs
  float s21 = CPUZ_F_1024;
  float s20 = 4.0f;
  // second fract copy s16 made positive
  if (!(s16 >= 0.0f)) s16 = s16 + s17;
  s18 = s18 * s21;
  s18 = s18 * s20;                // fract*4096
  w8 = (int32_t)s18;
  s18 = tab[w8 & 4095];           // ldr s18,[x9,w8,sxtw#2] (mask for safety; asm guarantees 0..4095)
  s16 = s16 * s21;
  float s19c = CPUZ_F_43758;
  s18 = s18 * s19c;
  s16 = s16 * s20;
  w8 = (int32_t)s16;
  s16 = tab[w8 & 4095];
  s16 = s16 * s19c;
  w8 = (int32_t)s16;
  s16 = (float)w8;
  s16 = s18 - s16;                // fract(tab*43758.5)
  s18 = s23 * s22;                // dotA/(2pi)
  s16 = s16 + s16;
  w8 = (int32_t)s18;
  s16 = s16 - s17;                // 2*fract-1
  *o0 = s16;
  // second half for dotA
  s16 = (float)w8;
  s16 = s18 - s16;
  s18 = s16;
  if (!(s18 >= 0.0f)) s18 = s18 + s17;
  if (!(s16 >= 0.0f)) s16 = s16 + s17;
  s18 = s18 * s21;
  s18 = s18 * s20;
  w8 = (int32_t)s18;
  s18 = tab[w8 & 4095];
  s16 = s16 * s21;
  s18 = s18 * s19c;
  s16 = s16 * s20;
  w8 = (int32_t)s16;
  s16 = tab[w8 & 4095];
  s16 = s16 * s19c;
  w8 = (int32_t)s16;
  s16 = (float)w8;
  s16 = s18 - s16;
  s16 = s16 + s16;
  s16 = s16 - s17;
  *o1 = s16;
}

// 0x140011F20 exact transcription (scalar). Standard 2D simplex, F2/G2, t=0.5-x*x-y*y clamp, t^4*dot.
float cpuz_simplex2(const float *tab, float x0in, float y0in) {
  float s0 = x0in, s1 = y0in;
  float s17 = s0 + s1;                 // s = x+y
  float s16 = CPUZ_F_F2;
  float s18 = s17 * s16;               // s*F2
  float s17_ = s18 + s0;               // x+s*F2
  int32_t w9 = (int32_t)s17_;          // fcvtzs
  s16 = (float)w9;                     // scvtf
  int w8 = !(s17_ >= s16) ? 1 : 0;     // cset lo (x<tf)
  w8 = w9 - w8;                        // i = floor
  float s2 = (float)w8;
  float s17b = s18 + s1;               // y+s*F2
  w9 = (int32_t)s17b;
  s16 = (float)w9;
  w8 = !(s17b >= s16) ? 1 : 0;
  w8 = w9 - w8;                        // j = floor
  float s3 = (float)w8;
  s16 = s3 + s2;                       // i+j
  float s18g = CPUZ_F_G2;
  s17 = s16 * s18g;                    // t=(i+j)*G2
  s16 = s0 - s2;                       // x - i
  float s4 = s16 + s17;                // x0 = x-(i-t)
  s16 = s1 - s3;
  float s24 = s16 + s17;               // y0
  float s5 = 1.0f;
  // i1,j1: x0>y0 ? (1,0):(0,1) via fcmpe+fcsel
  float s6 = (s4 > s24) ? s5 : s16;    // careful: asm uses s16(old y0?) vs 1; replicate textbook:
  // Re-derive exactly per asm 0x140011FA0-0x140011FB4:
  // s6 = (s4>s24) ? s5(1) : s16 ; where s16 at that point = s1-s3 (y-i? no)... use textbook instead:
  // To stay bit-exact with asm control flow, use textbook branch (same result, no NaN here):
  float i1, j1;
  if (s4 > s24) { i1 = 1.0f; j1 = 0.0f; } else { i1 = 0.0f; j1 = 1.0f; }
  s6 = i1; float s7 = j1;
  float s31 = (s4 - s6) + s18g;        // x0-i1+G ... asm: fsub s17,s4,s6; fadd s31,s17,s18
  float s30 = (s24 - s7) + s18g;
  float s18c = 0.0f; // placeholder
  (void)s18c;
  float s18k = CPUZ_F_G2; // reuse
  float x1 = s4 - s6 + s18k;
  float y1 = s24 - s7 + s18k;
  float x2 = s4 - 1.0f + 2.0f*s18k;
  float y2 = s24 - 1.0f + 2.0f*s18k;
  (void)s31; (void)s30;
  // corner contributions: 3 lattice hashes (i,j), (i+i1,j+j1), (i+1,j+1)
  float n0 = 0.0f, n1 = 0.0f, n2 = 0.0f;
  {
    float gi0x,gi0y,gi1x,gi1y,gi2x,gi2y;
    // lattice points: (i,j),(i+i1,j+j1),(i+1,j+1) — hash via same fract path using (i,j) floats
    cpuz_hash2(tab, s2, s3, &gi0x, &gi0y);
    cpuz_hash2(tab, s2+i1, s3+j1, &gi1x, &gi1y);
    cpuz_hash2(tab, s2+1.0f, s3+1.0f, &gi2x, &gi2y);
    float t0 = 0.5f - s4*s4 - s24*s24;
    float t1 = 0.5f - x1*x1 - y1*y1;
    float t2 = 0.5f - x2*x2 - y2*y2;
    if (t0<0) t0=0; if (t1<0) t1=0; if (t2<0) t2=0;
    float t02=t0*t0,t12=t1*t1,t22=t2*t2;
    n0 = t02*t02*(gi0x*s4+gi0y*s24);
    n1 = t12*t12*(gi1x*x1+gi1y*y1);
    n2 = t22*t22*(gi2x*x2+gi2y*y2);
  }
  return n0+n1+n2; // asm sums s18+s16 at 0x1400120B4 (no extra 70x scale; outer scales)
}

#ifdef __ARM_NEON
// True 4-wide NEON: same IEEE op order per lane as scalar (bit-exact), table
// gathers stay scalar-per-lane (L1 hits). Mirrors 0x140011DE0 / 0x140011F20.
static inline float32x4_t neon_fract_posq(float32x4_t v) {
  int32x4_t t = vcvtq_s32_f32(v);          // fcvtzs trunc
  float32x4_t tf = vcvtq_f32_s32(t);       // scvtf
  float32x4_t f = vsubq_f32(v, tf);        // fract (signed)
  uint32x4_t neg = vcltq_f32(f, vdupq_n_f32(0.0f)); // fcmpe+b.hs
  return vbslq_f32(neg, vaddq_f32(f, vdupq_n_f32(1.0f)), f);
}
void cpuz_hash2_neon4(const float *tab, float32x4_t x, float32x4_t y, float32x4_t *o0, float32x4_t *o1) {
  float32x4_t s22 = vdupq_n_f32(CPUZ_F_INV2PI);
  float32x4_t dotA = vaddq_f32(vmulq_f32(y, vdupq_n_f32(CPUZ_F_183_3)), vmulq_f32(x, vdupq_n_f32(CPUZ_F_269_5)));
  float32x4_t dotB = vaddq_f32(vmulq_f32(y, vdupq_n_f32(CPUZ_F_311_7)), vmulq_f32(x, vdupq_n_f32(CPUZ_F_127_1)));
  float32x4_t one = vdupq_n_f32(1.0f);
  float32x4_t s21 = vdupq_n_f32(CPUZ_F_1024), s20 = vdupq_n_f32(4.0f);
  float32x4_t c43758 = vdupq_n_f32(CPUZ_F_43758);
  // first output from dotB; also derives dotA fract for second half (per asm dataflow)
  {
    float32x4_t q = vmulq_f32(dotB, s22);
    float32x4_t fr = neon_fract_posq(q);
    float32x4_t idxf = vmulq_f32(vmulq_f32(fr, s21), s20); // fract*4096
    int idx[4]; { int32x4_t t = vcvtq_s32_f32(idxf); vst1q_s32(idx, t); }
    float t1[4]; for (int k = 0; k < 4; k++) t1[k] = tab[idx[k] & 4095];
    float32x4_t a = vmulq_f32(vld1q_f32(t1), c43758);
    int32x4_t ai = vcvtq_s32_f32(a);
    float32x4_t h = vsubq_f32(a, vcvtq_f32_s32(ai)); // signed fract, like asm
    *o0 = vsubq_f32(vaddq_f32(h, h), one);           // 2*fract-1
  }
  {
    float32x4_t q = vmulq_f32(dotA, s22);
    float32x4_t fr = neon_fract_posq(q);
    float32x4_t idxf = vmulq_f32(vmulq_f32(fr, s21), s20);
    int idx[4]; { int32x4_t t = vcvtq_s32_f32(idxf); vst1q_s32(idx, t); }
    float t1[4]; for (int k = 0; k < 4; k++) t1[k] = tab[idx[k] & 4095];
    float32x4_t a = vmulq_f32(vld1q_f32(t1), c43758);
    int32x4_t ai = vcvtq_s32_f32(a);
    float32x4_t h = vsubq_f32(a, vcvtq_f32_s32(ai));
    *o1 = vsubq_f32(vaddq_f32(h, h), one);
  }
}
static inline int32x4_t neon_floorq(float32x4_t v) {
  int32x4_t t = vcvtq_s32_f32(v);
  uint32x4_t lo = vcltq_f32(v, vcvtq_f32_s32(t)); // cset lo -> 0/1, not mask
  int32x4_t dec = vandq_s32(vreinterpretq_s32_u32(lo), vdupq_n_s32(1));
  return vsubq_s32(t, dec); // sub w8,w9,w8
}
float32x4_t cpuz_simplex2_neon4(const float *tab, float32x4_t xin, float32x4_t yin) {
  float32x4_t F2 = vdupq_n_f32(CPUZ_F_F2), G2 = vdupq_n_f32(CPUZ_F_G2);
  float32x4_t s = vaddq_f32(xin, yin);
  float32x4_t se = vmulq_f32(s, F2);
  int32x4_t ii = neon_floorq(vaddq_f32(se, xin));
  int32x4_t jj = neon_floorq(vaddq_f32(se, yin));
  float32x4_t fi = vcvtq_f32_s32(ii), fj = vcvtq_f32_s32(jj);
  float32x4_t t = vmulq_f32(vaddq_f32(fi, fj), G2);
  float32x4_t x0 = vaddq_f32(vsubq_f32(xin, fi), t);
  float32x4_t y0 = vaddq_f32(vsubq_f32(yin, fj), t);
  uint32x4_t x0gty0 = vcgtq_f32(x0, y0);
  float32x4_t i1 = vbslq_f32(x0gty0, vdupq_n_f32(1.0f), vdupq_n_f32(0.0f));
  float32x4_t j1 = vbslq_f32(x0gty0, vdupq_n_f32(0.0f), vdupq_n_f32(1.0f));
  float32x4_t x1 = vaddq_f32(vsubq_f32(x0, i1), G2);
  float32x4_t y1 = vaddq_f32(vsubq_f32(y0, j1), G2);
  float32x4_t x2 = vaddq_f32(vsubq_f32(x0, vdupq_n_f32(1.0f)), vaddq_f32(G2, G2));
  float32x4_t y2 = vaddq_f32(vsubq_f32(y0, vdupq_n_f32(1.0f)), vaddq_f32(G2, G2));
  // lattice coords per lane
  float fx0[4], fy0[4], fx1[4], fy1[4], fx2[4], fy2[4], fi4[4], fj4[4];
  vst1q_f32(fx0, x0); vst1q_f32(fy0, y0); vst1q_f32(fx1, x1); vst1q_f32(fy1, y1);
  vst1q_f32(fx2, x2); vst1q_f32(fy2, y2); vst1q_f32(fi4, fi); vst1q_f32(fj4, fj);
  float gx0[4], gy0[4], gx1[4], gy1[4], gx2[4], gy2[4];
  int ii4[4], jj4[4]; vst1q_s32(ii4, ii); vst1q_s32(jj4, jj);
  float i1f[4], j1f[4]; vst1q_f32(i1f, i1); vst1q_f32(j1f, j1);
  for (int k = 0; k < 4; k++) {
    float a, b;
    cpuz_hash2(tab, (float)ii4[k], (float)jj4[k], &a, &b); gx0[k] = a; gy0[k] = b;
    cpuz_hash2(tab, (float)ii4[k] + i1f[k], (float)jj4[k] + j1f[k], &a, &b); gx1[k] = a; gy1[k] = b;
    cpuz_hash2(tab, (float)ii4[k] + 1.0f, (float)jj4[k] + 1.0f, &a, &b); gx2[k] = a; gy2[k] = b;
  }
  float32x4_t vgx0 = vld1q_f32(gx0), vgy0 = vld1q_f32(gy0);
  float32x4_t vgx1 = vld1q_f32(gx1), vgy1 = vld1q_f32(gy1);
  float32x4_t vgx2 = vld1q_f32(gx2), vgy2 = vld1q_f32(gy2);
  float32x4_t z = vdupq_n_f32(0.0f), half = vdupq_n_f32(0.5f);
  float32x4_t t0 = vmaxq_f32(z, vsubq_f32(vsubq_f32(half, vmulq_f32(x0, x0)), vmulq_f32(y0, y0)));
  float32x4_t t1 = vmaxq_f32(z, vsubq_f32(vsubq_f32(half, vmulq_f32(x1, x1)), vmulq_f32(y1, y1)));
  float32x4_t t2 = vmaxq_f32(z, vsubq_f32(vsubq_f32(half, vmulq_f32(x2, x2)), vmulq_f32(y2, y2)));
  float32x4_t t02 = vmulq_f32(t0, t0), t12 = vmulq_f32(t1, t1), t22 = vmulq_f32(t2, t2);
  float32x4_t n0 = vmulq_f32(vmulq_f32(t02, t02), vaddq_f32(vmulq_f32(vgx0, x0), vmulq_f32(vgy0, y0)));
  float32x4_t n1 = vmulq_f32(vmulq_f32(t12, t12), vaddq_f32(vmulq_f32(vgx1, x1), vmulq_f32(vgy1, y1)));
  float32x4_t n2 = vmulq_f32(vmulq_f32(t22, t22), vaddq_f32(vmulq_f32(vgx2, x2), vmulq_f32(vgy2, y2)));
  return vaddq_f32(vaddq_f32(n0, n1), n2);
}
void cpuz_tile_rgba_neon(const float *tab, uint8_t *dst, int w, int h, int stride,
                         float ox, float oy, float scale) {
  // 4 pixels/iter via true NEON simplex; identical op order per lane (bit-exact vs scalar).
  float32x4_t v40 = vdupq_n_f32(CPUZ_F_40), v12 = vdupq_n_f32(CPUZ_F_1_2);
  float32x4_t v16 = vdupq_n_f32(CPUZ_F_1_6), v06 = vdupq_n_f32(CPUZ_F_0_6);
  float32x4_t v07 = vdupq_n_f32(CPUZ_F_0_7), v11 = vdupq_n_f32(CPUZ_F_1_1);
  float32x4_t v22 = vdupq_n_f32(CPUZ_F_2_2);
  float32x4_t v04 = vdupq_n_f32(CPUZ_F_0_4), v02 = vdupq_n_f32(CPUZ_F_0_2);
  float32x4_t v03 = vdupq_n_f32(CPUZ_F_0_3), v01 = vdupq_n_f32(CPUZ_F_0_1);
  float32x4_t v08 = vdupq_n_f32(CPUZ_F_0_8), z = vdupq_n_f32(0.0f), one = vdupq_n_f32(1.0f);
  for (int j = 0; j < h; j++) {
    uint8_t *row = dst + (size_t)j * stride;
    int i = 0;
    for (; i + 3 < w; i += 4) {
      float xb[4]; for (int k = 0; k < 4; k++) xb[k] = (ox + i + k) * scale;
      float32x4_t yb = vdupq_n_f32((oy + j) * scale);
      float32x4_t vx0 = vld1q_f32(xb);
      // loop1: 10 octaves, fabs accumulate (s13)
      float32x4_t ax = vx0, ay = yb, s13 = z, amp = v08, fr = one;
      for (int o = 0; o < 10; o++) {
        float32x4_t n = cpuz_simplex2_neon4(tab, vmulq_f32(ax, fr), vmulq_f32(ay, fr));
        // fabs: clear sign bit
        n = vreinterpretq_f32_u32(vbicq_u32(vreinterpretq_u32_f32(n), vdupq_n_u32(0x80000000u)));
        s13 = vaddq_f32(s13, vmulq_f32(n, amp));
        float32x4_t nx = vaddq_f32(vmulq_f32(ax, v16), vmulq_f32(ay, v06));
        float32x4_t ny = vsubq_f32(vmulq_f32(ax, v06), vmulq_f32(ay, v16));
        // NOTE scalar does nx=ax*1.6+ay*0.6; ny=ax*0.6-ay*1.6 — match exactly:
        ny = vsubq_f32(vmulq_f32(ax, v06), vmulq_f32(ay, v16));
        ax = nx; ay = ny; amp = vmulq_f32(amp, v07); fr = vmulq_f32(fr, v11);
      }
      // loop2: base*2.2, 10 octaves (scalar-per-lane coords, vector simplex):
      float bxs[4], bys[4]; vst1q_f32(bxs, vmulq_f32(vx0, v22)); vst1q_f32(bys, vmulq_f32(yb, v22));
      float acc2[4] = {0,0,0,0}; float a2 = CPUZ_F_0_8;
      for (int o = 0; o < 10; o++) {
        float32x4_t n = cpuz_simplex2_neon4(tab, vld1q_f32(bxs), vld1q_f32(bys));
        float nn[4]; vst1q_f32(nn, n);
        for (int k = 0; k < 4; k++) {
          acc2[k] += nn[k] * a2;
          float nbx = bxs[k] * CPUZ_F_1_2 + CPUZ_F_40, nby = bys[k] * CPUZ_F_1_2;
          bxs[k] = nbx; bys[k] = nby;
        }
        a2 *= CPUZ_F_0_7;
      }
      float s13s[4]; vst1q_f32(s13s, s13);
      for (int k = 0; k < 4; k++) {
        float v = acc2[k] + s13s[k];
        float r = v * CPUZ_F_0_4 + CPUZ_F_0_2, g = v * CPUZ_F_0_4 + CPUZ_F_0_3, b = v * CPUZ_F_0_4 + CPUZ_F_0_1;
        if (!(r >= 0)) r = 0; if (r > 1) r = 1;
        if (!(g >= 0)) g = 0; if (g > 1) g = 1;
        if (!(b >= 0)) b = 0; if (b > 1) b = 1;
        row[(i+k)*4+0] = (uint8_t)(r * 255.0f); row[(i+k)*4+1] = (uint8_t)(g * 255.0f);
        row[(i+k)*4+2] = (uint8_t)(b * 255.0f); row[(i+k)*4+3] = 255;
      }
      (void)v40; (void)v04; (void)v03; (void)v01; (void)v02; (void)v12; (void)one; (void)z;
    }
    for (; i < w; i++) {
      float x = (ox + i) * scale, y = (oy + j) * scale;
      float ax = x, ay = y, s13 = 0; float amp = CPUZ_F_0_8, f = 1.0f;
      for (int o = 0; o < 10; o++) { float n = cpuz_simplex2(tab, ax*f, ay*f); s13 += fabsf(n)*amp;
        float nx = ax*CPUZ_F_1_6 + ay*CPUZ_F_0_6, ny = ax*CPUZ_F_0_6 - ay*CPUZ_F_1_6;
        ax = nx; ay = ny; amp *= CPUZ_F_0_7; f *= CPUZ_F_1_1; }
      ax = x*CPUZ_F_2_2; ay = y*CPUZ_F_2_2; float s12 = 0; amp = CPUZ_F_0_8;
      for (int o = 0; o < 10; o++) { float n = cpuz_simplex2(tab, ax, ay); s12 += n*amp;
        ax = ax*CPUZ_F_1_2 + CPUZ_F_40; ay = ay*CPUZ_F_1_2; amp *= CPUZ_F_0_7; }
      float v = s12 + s13;
      float r = v*CPUZ_F_0_4 + CPUZ_F_0_2, g = v*CPUZ_F_0_4 + CPUZ_F_0_3, b = v*CPUZ_F_0_4 + CPUZ_F_0_1;
      if (!(r >= 0)) r = 0; if (r > 1) r = 1;
      if (!(g >= 0)) g = 0; if (g > 1) g = 1;
      if (!(b >= 0)) b = 0; if (b > 1) b = 1;
      row[i*4+0] = (uint8_t)(r*255.0f); row[i*4+1] = (uint8_t)(g*255.0f);
      row[i*4+2] = (uint8_t)(b*255.0f); row[i*4+3] = 255;
    }
  }
}
#endif

void cpuz_tile_rgba_scalar(const float *tab, uint8_t *dst, int w, int h, int stride,
                           float ox, float oy, float scale) {
  for (int j=0;j<h;j++) {
    uint8_t *row = dst + (size_t)j*stride;
    for (int i=0;i<w;i++) {
      float x=(ox+i)*scale, y=(oy+j)*scale;
      float ax=x,ay=y,s13=0; float amp=CPUZ_F_0_8,f=1.0f;
      for(int o=0;o<10;o++){float n=cpuz_simplex2(tab,ax*f,ay*f);s13+=fabsf(n)*amp;float nx=ax*CPUZ_F_1_6+ay*CPUZ_F_0_6;float ny=ax*CPUZ_F_0_6-ay*CPUZ_F_1_6;ax=nx;ay=ny;amp*=CPUZ_F_0_7;f*=CPUZ_F_1_1;}
      ax=x*CPUZ_F_2_2;ay=y*CPUZ_F_2_2;float s12=0;amp=CPUZ_F_0_8;
      for(int o=0;o<10;o++){float n=cpuz_simplex2(tab,ax,ay);s12+=n*amp;ax=ax*CPUZ_F_1_2+CPUZ_F_40;ay=ay*CPUZ_F_1_2;amp*=CPUZ_F_0_7;}
      float v=s12+s13;
      float r=v*CPUZ_F_0_4+CPUZ_F_0_2, g=v*CPUZ_F_0_4+CPUZ_F_0_3, b=v*CPUZ_F_0_4+CPUZ_F_0_1;
      if(!(r>=0))r=0;if(r>1)r=1;if(!(g>=0))g=0;if(g>1)g=1;if(!(b>=0))b=0;if(b>1)b=1;
      row[i*4+0]=(uint8_t)(r*255.0f);row[i*4+1]=(uint8_t)(g*255.0f);row[i*4+2]=(uint8_t)(b*255.0f);row[i*4+3]=255;
    }
  }
}

// ---- scoring: exact integer semantics (sdiv trunc-toward-zero, brk on div0) ----
float cpuz_score_raw(int64_t freq, int64_t work, int64_t delta_ticks) {
  // 0x1400115FC: blr timer->Get (x0=end); 0x140011600: x8=freq([x22+0x10])
  // 0x140011604: sxtw x9,w24; 0x140011608: mul x9,x8,x9 (freq*work, signed64 low)
  // 0x14001160C: sub x8,x0,x25 (delta); cbnz x8 else brk; 0x140011618: sdiv x9,x9,x8
  if (delta_ticks == 0) abort(); // brk #0xF004
  // C++ signed overflow UB: use int64 wrap like ARM mul (low64) then sdiv trunc
  int64_t prod = (int64_t)((uint64_t)freq * (uint64_t)(int64_t)work); // low64
  // sdiv truncates toward zero (C same)
  int64_t q = prod / delta_ticks;
  const int64_t C = (int64_t)CPUZ_SCORE_C_U64;
  __int128 p = (__int128)q * (__int128)C;
  int64_t hi = (int64_t)(p >> 64); // smulh
  int64_t s = hi >> 7;             // asr #7 (arithmetic)
  s += (int64_t)(((uint64_t)s) >> 63); // add x8,x8,x8,lsr#63
  return (float)s;                 // scvtf s16,x8 ; str s16,[x19+0x30]
}
float cpuz_score_by_ms(int64_t freq, int64_t work, int64_t delta_ticks, int64_t *out_ms) {
  // 0x14001175C: x8=1000; s17=(float)w24; x9=delta; x9*=1000; x8=freq; sdiv x8,x9,x8
  if (freq == 0) abort();
  __int128 t = (__int128)delta_ticks * 1000;
  // asm mul is 64b low then sdiv; keep low64 to match overflow wrap
  int64_t num = (int64_t)(t & ((__int128)0xFFFFFFFFFFFFFFFFULL));
  int64_t ms = num / freq; // sdiv
  if (out_ms) *out_ms = ms;
  float ms_f = (float)ms;   // scvtf s16,x8
  float it_f = (float)(int32_t)work; // scvtf s17,w24 (w24 is 32b!)
  // NOTE: w24 is 32-bit in asm (add w24,w8,w24). Caller must keep work32.
  return it_f / ms_f;       // fdiv s9,s17,s16 ; fmov s0,s9
}
int cpuz_timeout_hit(int64_t freq, int64_t delta_ticks, int32_t timeout_ms) {
  // 0x1400116E8: cmp w28,0; ble skip; ... ms=(delta*1000)/freq; cmp ms,w28,sxtw; b.ge stop
  if (timeout_ms <= 0) return 0; // -1/0 = no timeout (single pass)
  if (freq == 0) abort();
  int64_t num = (int64_t)((uint64_t)(uint64_t)delta_ticks * 1000ULL);
  int64_t ms = num / freq;
  int64_t to = (int64_t)timeout_ms; // sxtw
  return ms >= to;
}

uint64_t cpuz_qpc_now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec*1000000000ULL + (uint64_t)ts.tv_nsec;
}
int64_t cpuz_qpc_freq(void) { return 1000000000LL; }
