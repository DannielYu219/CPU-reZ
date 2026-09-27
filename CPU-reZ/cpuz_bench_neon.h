#pragma once
// CPU-Z WoA Bench NEON 1:1 rewrite — scoring strictly aligned to cpuz_arm64.exe
// Target: /Users/dannielyu/cpuz-arm64-macOS/cpuz_arm64.exe v1.06
// Reversed addrs: kernel hash 0x140011DE0, simplex 0x140011F20, outer 0x1400120D0,
//   driver 0x140011368/0x1400117C0, supervisor 0x140004A58, Run 0x1400045B8,
//   scoring 0x1400115FC-0x140011630 + 0x14001175C-0x140011788, const 0x1400117B8.
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CPUZ_BENCH_W 1024
#define CPUZ_BENCH_H 1024
#define CPUZ_BENCH_BUF_BYTES (1024*1024*4u) // RGBA8888, matches 0x400000 + memset w*h*4
#define CPUZ_BENCH_ITERS 10000   // w5=0x2710, timeout ms (10s)
#define CPUZ_STRESS_ITERS 4000   // w5=0xFA0, timeout ms (4s)
#define CPUZ_SINGLE_ITERS -1     // w5=-1 (0xFFFFFFFF), no timeout, single pass
#define CPUZ_SLEEP_MS 10

// .text 0x1400117B8 u64 (file 0x10BB8): scaling constant for raw score path
#define CPUZ_SCORE_C_U64 0x20c49ba5e353f7cfULL

// float constants (bit-exact, from .text)
#define CPUZ_F_2PI        6.283185005187988f   // 0x140011DD8
#define CPUZ_F_INV1024    0.0009765625f        // 0x140011DDC / 0x140012434
#define CPUZ_F_183_3      183.3000030517578f   // 0x140011F00
#define CPUZ_F_INV2PI     0.1591549515724182f  // 0x140011F04
#define CPUZ_F_269_5      269.5f               // 0x140011F08
#define CPUZ_F_311_7      311.70001220703125f  // 0x140011F0C
#define CPUZ_F_127_1      127.0999984741211f   // 0x140011F10
#define CPUZ_F_1024       1024.0f              // 0x140011F14
#define CPUZ_F_43758      43758.546875f        // 0x140011F18
#define CPUZ_F_F2         0.3660254180431366f  // (sqrt3-1)/2, 0x1400120C0 simplex skew
#define CPUZ_F_G2         0.21132487058639526f // (3-sqrt3)/6, 0x1400120C4 simplex unskew
// outer fBm/color constants 0x140012430..0x14001246C
#define CPUZ_F_1_1 1.100000023841858f
#define CPUZ_F_2_2 2.200000047683716f
#define CPUZ_F_3_3 3.3000001907348633f
#define CPUZ_F_40  40.0f
#define CPUZ_F_1_2 1.2000000476837158f
#define CPUZ_F_1_6 1.600000023841858f
#define CPUZ_F_0_6 0.6000000238418579f
#define CPUZ_F_0_8 0.800000011920929f
#define CPUZ_F_0_7 0.699999988079071f
#define CPUZ_F_0_4 0.4000000059604645f
#define CPUZ_F_0_2 0.20000000298023224f
#define CPUZ_F_0_1 0.10000000149011612f
#define CPUZ_F_0_9 0.8999999761581421f
#define CPUZ_F_0_3 0.30000001192092896f
#define CPUZ_F_255 255.0f

typedef struct {
  float *sin_table; // 4096 floats, table[i]=sinf(i*2PI/1024*4)
} cpuz_bench_ctx_t;

// exact floor as asm: fcvtzs trunc + (x<tf ? -1 : 0)
static inline int32_t cpuz_floor_s32(float x) {
  int32_t t = (int32_t)x; // fcvtzs, trunc toward zero
  float tf = (float)t;    // scvtf
  // fcmpe + cset lo: w8 = (x < tf) ? 1 : 0 ; sub w8,w9,w8
  // NOTE: NaN never occurs here (coords finite)
  if (x < tf) t -= 1;
  return t;
}
static inline float cpuz_fract_pos(float x) {
  // fract(x/(2PI)) made positive: f = x-floor? asm does trunc+conditional
  // exact sequence: w8=(int)x, s16=(float)w8, s16=x-s16, if s16<0 s16+=1
  int32_t t = (int32_t)x;
  float tf = (float)t;
  float f = x - tf;
  if (f < 0.0f) f += 1.0f;
  return f;
}

// API
void cpuz_sin_table_init(float *tab); // 4096
void cpuz_hash2(const float *tab, float x, float y, float *o0, float *o1); // 0x140011DE0 scalar exact
float cpuz_simplex2(const float *tab, float x, float y); // 0x140011F20 scalar exact
#ifdef __ARM_NEON
#include <arm_neon.h>
// NEON 4-lane versions: same op order per lane (bit-exact vs scalar)
void cpuz_hash2_neon4(const float *tab, float32x4_t x, float32x4_t y, float32x4_t *o0, float32x4_t *o1);
float32x4_t cpuz_simplex2_neon4(const float *tab, float32x4_t x, float32x4_t y);
void cpuz_tile_rgba_neon(const float *tab, uint8_t *dst, int w, int h, int stride,
                         float ox, float oy, float scale);
#endif
void cpuz_tile_rgba_scalar(const float *tab, uint8_t *dst, int w, int h, int stride,
                           float ox, float oy, float scale);

// scoring — integer ops bit-identical to asm (sdiv/sm ulh/asr/scvtf/fdiv)
// raw path 0x1400115FC-0x140011630: q=freq*work/delta ; scaled=(mulh(q,C)>>7)+carry ; f=(float)scaled
float cpuz_score_raw(int64_t freq, int64_t work, int64_t delta_ticks);
// ms path 0x14001175C-0x140011788: ms=(delta*1000)/freq ; return (float)work/(float)ms
float cpuz_score_by_ms(int64_t freq, int64_t work, int64_t delta_ticks, int64_t *out_ms);
// timeout predicate 0x1400116E8-0x140011710: if (timeout_ms>0 && ms>=timeout_ms) stop
int cpuz_timeout_hit(int64_t freq, int64_t delta_ticks, int32_t timeout_ms);

// driver helpers
uint64_t cpuz_qpc_now_ns(void); // clock_gettime MONOTONIC
int64_t cpuz_qpc_freq(void);    // 1e9

#ifdef __cplusplus
}
#endif
