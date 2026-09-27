// CPU-reZ SME frontend: probe + dispatch. Compile with PLAIN flags
// (-mcpu=native, no +sme) so the compiler never emits SVE instructions in
// ordinary code (Apple Silicon faults non-streaming SVE, e.g. stray `cntd`).
// Streaming work lives in cpuz_bench_sme_core.c (+sme2 TU) and is entered via
// ordinary BL; SMSTART/SMSTOP in the callee prologue/epilogue, inside the guard.
#include "cpuz_bench_sme.h"
#include "cpuz_bench_neon.h"
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/sysctl.h>

// Plain declarations of core-TU streaming entry points (no SME attributes here
// on purpose: this TU must compile even without SME support).
uint64_t cpuz_sme_core_vl(void);
void cpuz_sme_core_tile(const float *tab, uint8_t *dst, int w, int h,
                         int stride, float ox, float oy, float scale);

int cpuz_sme_present(void) {
  int v = 0; size_t n = sizeof(v);
  if (sysctlbyname("hw.optional.arm.FEAT_SME", &v, &n, 0, 0) != 0) return 0;
  return v != 0;
}

static sigjmp_buf s_jb;
static void s_hdl(int sig) { (void)sig; siglongjmp(s_jb, 1); }
static void probe_guard_begin(struct sigaction *old) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = s_hdl;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGILL, &sa, old);
}
int cpuz_sme_available_on_this_thread(void) {
  if (!cpuz_sme_present()) return 0;
  struct sigaction old;
  probe_guard_begin(&old);
  int ok = 0;
  if (sigsetjmp(s_jb, 1) == 0) {
    uint64_t vl = cpuz_sme_core_vl(); // SMSTART faults here if unsupported -> caught
    ok = (vl >= 4);
  }
  sigaction(SIGILL, &old, 0);
  return ok;
}
uint64_t cpuz_sme_vl_words(void) {
  if (!cpuz_sme_present()) return 0;
  struct sigaction old;
  probe_guard_begin(&old);
  uint64_t vl = 0;
  if (sigsetjmp(s_jb, 1) == 0) vl = cpuz_sme_core_vl();
  sigaction(SIGILL, &old, 0);
  return vl;
}
void cpuz_tile_rgba_dispatch(const float *tab, uint8_t *dst, int w, int h, int stride,
                             float ox, float oy, float scale, int use_sme) {
  if (use_sme) {
    cpuz_sme_core_tile(tab, dst, w, h, stride, ox, oy, scale);
    return;
  }
#ifdef __ARM_NEON
  cpuz_tile_rgba_neon(tab, dst, w, h, stride, ox, oy, scale);
#else
  cpuz_tile_rgba_scalar(tab, dst, w, h, stride, ox, oy, scale);
#endif
}
