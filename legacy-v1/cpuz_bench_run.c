// Bench driver v2: persistent workers + barrier (mirrors 0x140011328 waiters +
// supervisor 0x140004A58), exact scoring + display layer (0x140005DAC+ formula).
#include "cpuz_bench_neon.h"
#include "cpuz_bench_ref.h"
#include <pthread.h>
#if __APPLE__
#include <sys/qos.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// portable barrier (macOS lacks pthread_barrier)
typedef struct { pthread_mutex_t m; pthread_cond_t c; int count, total, gen; } xbarrier_t;
static void xbarrier_init(xbarrier_t *b, int total) {
  pthread_mutex_init(&b->m, 0); pthread_cond_init(&b->c, 0);
  b->count = 0; b->total = total; b->gen = 0;
}
static void xbarrier_wait(xbarrier_t *b) {
  pthread_mutex_lock(&b->m);
  int g = b->gen;
  if (++b->count == b->total) { b->count = 0; b->gen++; pthread_cond_broadcast(&b->c); }
  else while (b->gen == g) pthread_cond_wait(&b->c, &b->m);
  pthread_mutex_unlock(&b->m);
}


// Dynamic row dispatch, mirroring the original APC model (0x1400116B0: QueueUserAPC
// per row with w23 counter; workers grab work as they finish). Static ranges
// + per-pass barrier starve P-cores behind E-core stragglers on heterogeneous
// chips; dynamic dispatch restores proper summation.
// Dispatch granularity: original APC model dispatches 1 row per APC (w23+1).
// 2 rows amortizes call overhead while keeping straggler tail small on
// heterogeneous chips (tail <= one slow chunk per pass). Pixel-identical.
#define CPUZ_CHUNK_ROWS 2
typedef struct {
  const float *tab; uint8_t *buf; int w, h;
  float ox, oy, scale;
  xbarrier_t *b_start, *b_done;
  volatile int *stop; int *out_rows;
  volatile int *next_row; // shared atomic row cursor (w23 equivalent)
} worker_t;

static void *worker_fn(void *p) {
#if __APPLE__
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  worker_t *a = (worker_t*)p;
  for (;;) {
    xbarrier_wait(a->b_start);
    if (*a->stop) break;
    int done = 0;
    for (;;) {
      int y0 = __atomic_fetch_add(a->next_row, CPUZ_CHUNK_ROWS, __ATOMIC_RELAXED);
      if (y0 >= a->h) break;
      int y1 = y0 + CPUZ_CHUNK_ROWS;
      if (y1 > a->h) y1 = a->h;
      uint8_t *dst = a->buf + (size_t)y0 * a->w * 4;
#ifdef __ARM_NEON
      cpuz_tile_rgba_neon(a->tab, dst, a->w, y1 - y0, a->w*4, a->ox, a->oy + y0*a->scale, a->scale);
#else
      cpuz_tile_rgba_scalar(a->tab, dst, a->w, y1 - y0, a->w*4, a->ox, a->oy + y0*a->scale, a->scale);
#endif
      done += y1 - y0;
    }
    *a->out_rows = done;
    xbarrier_wait(a->b_done);
  }
  return NULL;
}

// One kernel call = exactly ONE full map (fixed work), then event-exit.
// Mirrors 0x1400115A0: workers render dispatched rows (w23/w27=1024), signal
// done, main collects and returns. w5 (timeout_ms) is only a safety cap
// (0x1400116E8: ms>=w28 -> abort path); <=0 (w5=-1) disables the cap.
// NOTE: an earlier revision looped maps until timeout (10s x2 = 20s+); that
// matches the score (rate-based) but NOT the runtime. Fixed-work matches the
// original observed runtime (sub-second per mode).
// Returns total work rows and elapsed ns.
static int g_quick = 0; // --quick: fixed-work fast path (warmup + timed maps, <2s, same rate)
static volatile int *g_cursor = 0; // set by main: shared row cursor, reset each dispatch
static void run_one_kernel(const float *tab, uint8_t *buf, int nthreads,
  int32_t timeout_ms, int64_t *out_work, uint64_t *out_ns,
  xbarrier_t *bs, xbarrier_t *bd, volatile int *stop,
  worker_t *ws, pthread_t *ths, int *rowslots) {
  (void)ws; (void)ths;
  memset(buf, 0, CPUZ_BENCH_BUF_BYTES); // 0x140220FE0 NEON memset before kernel
  uint64_t t0 = cpuz_qpc_now_ns();
  int64_t work = 0;
  if (g_cursor) *g_cursor = 0; // reset dispatch cursor (w23=0) each kernel call
  if (timeout_ms <= 0 || g_quick) { // w5=-1: no timeout cap, single map (warmup path)
    xbarrier_wait(bs); // dispatch one full map (1024 rows across workers)
    xbarrier_wait(bd); // event: all workers done
    for (int i = 0; i < nthreads; i++) work += rowslots[i];
  } else {
    // faithful: fill until timeout, like the original loop (event exit = abort only)
    for (int pass = 0;; pass++) {
      if (g_cursor) *g_cursor = 0;
      xbarrier_wait(bs);
      xbarrier_wait(bd);
      for (int i = 0; i < nthreads; i++) work += rowslots[i];
      uint64_t now = cpuz_qpc_now_ns();
      { // live score, like the dialog timer reading intermediate results
        double el = (double)(now - t0) / 1e9;
        double rate = (double)work / ((double)(now - t0) / 1e6);
        int live = (int)(rate * 256.0);
        fprintf(stderr, "\r live pass %d t=%.1fs rate=%.2f rows/ms headline=%d   ",
                pass, el, rate, live);
        fflush(stderr);
        if ((pass % 10) == 0) fprintf(stderr, "\n");
      }
      if (cpuz_timeout_hit(cpuz_qpc_freq(), (int64_t)(now - t0), timeout_ms)) break;
    }
  }
  uint64_t t1 = cpuz_qpc_now_ns();
  if (timeout_ms > 0 && cpuz_timeout_hit(cpuz_qpc_freq(), (int64_t)(t1 - t0), timeout_ms)) {
    fprintf(stderr, "note: kernel call hit timeout cap (%dms)\n", timeout_ms);
  }
  *out_work = work; *out_ns = t1 - t0;
}

int main(int argc, char **argv) {
  int nthreads = argc > 1 ? atoi(argv[1]) : 4;
  int refidx = argc > 2 ? atoi(argv[2]) : CPUZ_REF_DEFAULT;
  int stress = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--quick")) g_quick = 1;
    if (!strcmp(argv[i], "--stress")) stress = 1;
  }
  if (refidx < 0 || refidx >= CPUZ_REF_COUNT) refidx = CPUZ_REF_DEFAULT;
  static float tab[4096];
  cpuz_sin_table_init(tab);
  uint8_t *buf = malloc(CPUZ_BENCH_BUF_BYTES);
  // spawn persistent workers (CREATE_SUSPENDED+Resume equivalent: barrier start)
  xbarrier_t bs, bd;
  xbarrier_init(&bs, nthreads + 1);
  xbarrier_init(&bd, nthreads + 1);
  volatile int stop = 0;
  worker_t *ws = calloc(nthreads, sizeof(*ws));
  pthread_t *ths = calloc(nthreads, sizeof(*ths));
  int *rowslots = calloc(nthreads, sizeof(*rowslots));
  float scale = CPUZ_F_INV1024 * CPUZ_F_40;
  static volatile int next_row_storage = 0;
  volatile int *next_row = &next_row_storage;
  g_cursor = next_row;
  for (int i = 0; i < nthreads; i++) {
    ws[i] = (worker_t){tab, buf, CPUZ_BENCH_W, CPUZ_BENCH_H, 0, 0, scale, &bs, &bd, &stop, &rowslots[i], next_row};
    pthread_create(&ths[i], 0, worker_fn, &ws[i]);
  }
  int64_t freq = cpuz_qpc_freq();
  // warmup (mirrors supervisor mode1 single-shot, w5=-1: same kernel, result discarded;
  // warms i-cache/TLB/worker barrier before the two scored passes).
  // In --quick mode run 3 warmup maps to reach steady-state clocks.
  { int64_t ww; uint64_t nn;
    int nwarm = g_quick ? 3 : 1;
    for (int k = 0; k < nwarm; k++)
      run_one_kernel(tab, buf, nthreads, -1, &ww, &nn, &bs, &bd,
                     (volatile int*)&stop, ws, ths, rowslots); }
  // pass 1 (vtable+0x28) -> score+0x14
  int64_t w1; uint64_t ns1;
  run_one_kernel(tab, buf, nthreads, CPUZ_BENCH_ITERS, &w1, &ns1, &bs, &bd, (volatile int*)&stop, ws, ths, rowslots);
  float raw1 = cpuz_score_by_ms(freq, w1, (int64_t)ns1, 0);
  float raw1b = cpuz_score_raw(freq, w1, (int64_t)ns1);
  usleep(CPUZ_SLEEP_MS * 1000);
  // pass 2 (vtable+0x20) -> score+0x10
  int64_t w2; uint64_t ns2;
  run_one_kernel(tab, buf, nthreads, CPUZ_BENCH_ITERS, &w2, &ns2, &bs, &bd, (volatile int*)&stop, ws, ths, rowslots);
  float raw2 = cpuz_score_by_ms(freq, w2, (int64_t)ns2, 0);
  float raw2b = cpuz_score_raw(freq, w2, (int64_t)ns2);
  if (stress) {
    printf("--stress: one-map passes until Ctrl-C (orig stops on user Stop)...\n");
    for (;;) {
      int64_t w; uint64_t ns;
      run_one_kernel(tab, buf, nthreads, CPUZ_STRESS_ITERS, &w, &ns, &bs, &bd,
                     (volatile int*)&stop, ws, ths, rowslots);
      float r = cpuz_score_by_ms(freq, w, (int64_t)ns, 0);
      printf("stress pass: raw=%.4f headline=%d\n", r, (int)(r * 256.0f));
      fflush(stdout);
    }
  }
  stop = 1;
  xbarrier_wait(&bs); // release workers to exit
  for (int i = 0; i < nthreads; i++) pthread_join(ths[i], 0);

  const cpuz_ref_t *R = &CPUZ_REFS[refidx];
  // display layer:
  // Native work unit of the original driver is 256x finer than rows
  // (256 = width/4; i.e. headline = pixels/ms / 4; the asm carries the /4 in
  // w13 setup, mechanism flagged for dynamic confirmation; rows*256 converts).
  // (a) Headline This-Processor scores: raw_rows*256, fcvtzs trunc (like asm).
  //     Validated by ratio cancellation: 9729/877=11.09 vs X Elite 9165/820=11.18.
  // (b) EXACT per 0x140005DAC+ (compare controls): s17=measured*100.0f;
  //     s16=s17/ref; w8=(int)s16 (fcvtzs trunc toward zero) — applied here on
  //     headline-scale values, matching how the dialog feeds display-scale refs.
  float m = (raw1 + raw2) * 0.5f;
  const float K = 256.0f; // rows -> native units (width/4)
  int headline = (int)(m * K); // fcvtzs trunc, like asm
  int d_pct = (int)((float)headline * 100.0f / (nthreads == 1 ? R->single_ref : R->multi_ref));
  // chart max per 0x140006910: max(8000.0, candidates+10)
  float cmax = 8000.0f;
  float cand[] = {raw1 + 10.0f, raw2 + 10.0f, R->single_ref + 10.0f, R->multi_ref + 10.0f};
  for (int i = 0; i < 4; i++) if (cand[i] > cmax) cmax = cand[i];

  printf("threads=%d ref='%s' (%.1f/%.1f)\n", nthreads, R->name, R->single_ref, R->multi_ref);
  printf("raw pass1 ms_score=%.4f raw_path=%.4f work=%lld ns=%llu\n", raw1, raw1b, (long long)w1, (unsigned long long)ns1);
  printf("raw pass2 ms_score=%.4f raw_path=%.4f work=%lld ns=%llu\n", raw2, raw2b, (long long)w2, (unsigned long long)ns2);
  printf("raw mean=%.4f\n", m);
  printf("headline (raw*%.0f=rows*width/4 per ms, trunc): %d\n", (double)K, headline);
  printf("vs %s %s (%.1f): %d%%  (headline*100/ref, trunc, exact asm formula)\n",
         nthreads == 1 ? "single-ref" : "multi-ref", R->name,
         (double)(nthreads == 1 ? R->single_ref : R->multi_ref), d_pct);
  printf("chart_max=%.1f\n", cmax);
  return 0;
}
