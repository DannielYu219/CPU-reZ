// CPU-reZ verification: SME-vs-scalar bit-exactness, SVL report, speedup.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cpuz_bench_neon.h"
#include "cpuz_bench_sme.h"
static uint64_t ns_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000000000ull+t.tv_nsec;}
int main(void){
  printf("sme_present(sysctl)=%d main_thread_sme=%d svl_words=%llu\n",
    cpuz_sme_present(), cpuz_sme_available_on_this_thread(),
    (unsigned long long)cpuz_sme_vl_words());
  static float tab[4096]; cpuz_sin_table_init(tab);
  const int W=256,H=64;
  static uint8_t a[256*64*4],b[256*64*4];
  cpuz_tile_rgba_scalar(tab,a,W,H,W*4,0,0,CPUZ_F_INV1024*CPUZ_F_40);
  int use_sme = cpuz_sme_available_on_this_thread();
  cpuz_tile_rgba_dispatch(tab,b,W,H,W*4,0,0,CPUZ_F_INV1024*CPUZ_F_40,use_sme);
  int nd=0,first=-1;
  for(int i=0;i<(int)sizeof(a);i++) if(a[i]!=b[i]){ if(first<0)first=i; nd++; }
  printf("dispatch(use_sme=%d) vs scalar: %s diff=%d first=%d\n", use_sme, nd==0?"BIT-EXACT":"MISMATCH", nd, first);
  if(first>=0){int px=first/4; printf("px %d x=%d y=%d s=%d,%d,%d m=%d,%d,%d\n",px,px%W,px/W,
    a[first&~3],a[first&~3|1],a[first&~3|2],b[first&~3],b[first&~3|1],b[first&~3|2]);}
  // timing: 1024x1024 single map, scalar vs dispatch
  uint8_t *big=malloc(1024*1024*4);
  uint64_t t0=ns_now();
  cpuz_tile_rgba_scalar(tab,big,1024,1024,1024*4,0,0,CPUZ_F_INV1024*CPUZ_F_40);
  uint64_t t1=ns_now();
  printf("scalar 1024x1024: %.1f ms\n",(double)(t1-t0)/1e6);
  t0=ns_now();
  cpuz_tile_rgba_dispatch(tab,big,1024,1024,1024*4,0,0,CPUZ_F_INV1024*CPUZ_F_40,use_sme);
  t1=ns_now();
  printf("dispatch(use_sme=%d) 1024x1024: %.1f ms\n",use_sme,(double)(t1-t0)/1e6);
  return 0;
}
