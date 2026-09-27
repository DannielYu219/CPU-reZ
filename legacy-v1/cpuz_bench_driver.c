// Driver mirroring 0x1400045B8 + 0x140004A58 + 0x140011368 timeout/scoring.
// macOS/Linux portable: pthread + barrier, affinity best-effort.
#include "cpuz_bench_neon.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if __APPLE__
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif

typedef struct { const float *tab; uint8_t *buf; int y0,y1,w; float ox,oy,scale; int32_t *done_rows; } tile_arg_t;

static void *tile_thread(void *p) {
  tile_arg_t *a=(tile_arg_t*)p;
  int h=a->y1-a->y0;
  uint8_t *dst=a->buf+(size_t)a->y0*a->w*4;
#ifdef __ARM_NEON
  cpuz_tile_rgba_neon(a->tab,dst,a->w,h,a->w*4,a->ox,a->oy+a->y0*a->scale,a->scale);
#else
  cpuz_tile_rgba_scalar(a->tab,dst,a->w,h,a->w*4,a->ox,a->oy+a->y0*a->scale,a->scale);
#endif
  __atomic_store_n(a->done_rows,h,__ATOMIC_RELEASE);
  return NULL;
}

// One full-map pass, multithreaded. Returns rows done (1024) and fills buf.
static int run_map_once(const float *tab,uint8_t *buf,int nthreads,float ox,float oy,float scale){
  pthread_t *th=malloc(sizeof(*th)*nthreads);
  tile_arg_t *ag=malloc(sizeof(*ag)*nthreads);
  int32_t *done=malloc(sizeof(*done)*nthreads);
  int rows=CPUZ_BENCH_H/nthreads, rem=CPUZ_BENCH_H%nthreads, y=0;
  for(int i=0;i<nthreads;i++){int h=rows+(i<rem?1:0);ag[i]=(tile_arg_t){tab,buf,y,y+h,CPUZ_BENCH_W,ox,oy,scale};done[i]=0;ag[i].done_rows=&done[i];y+=h;}
  // memset like 0x140220FE0 (NEON) before kernel
  memset(buf,0,CPUZ_BENCH_BUF_BYTES);
  uint64_t t0=cpuz_qpc_now_ns();
  for(int i=0;i<nthreads;i++) pthread_create(&th[i],0,tile_thread,&ag[i]);
  for(int i=0;i<nthreads;i++) pthread_join(th[i],0);
  uint64_t t1=cpuz_qpc_now_ns();
  int total=0; for(int i=0;i<nthreads;i++) total+=done[i];
  free(th);free(ag);free(done);
  (void)t0;(void)t1;
  return total;
}

// Bench mode 0: two kernels (vtable 0x28 then 0x20) each timeout 10000ms, Sleep(10) between.
// We model both as same map workload (original differs only in vtable target; FLOPs class same).
// Returns s14 (first) via raw path and s10 via ms path to mirror str s0,[score+0x14]/[score+0x10].
int cpuz_bench_run(const float *tab,uint8_t *buf,int nthreads,float *out_first,float *out_second){
  int64_t freq=cpuz_qpc_freq();
  // pass 1
  memset(buf,0,CPUZ_BENCH_BUF_BYTES);
  uint64_t t0=cpuz_qpc_now_ns();
  int64_t work=0; uint64_t t1=t0;
  // repeat full-map passes until 10s timeout (w28=10000) like 0x1400116E8
  do {
    int r=run_map_once(tab,buf,nthreads,0,0,CPUZ_F_INV1024*CPUZ_F_40);
    work+=r;
    t1=cpuz_qpc_now_ns();
  } while(!cpuz_timeout_hit(freq,(int64_t)(t1-t0),CPUZ_BENCH_ITERS));
  int64_t delta=(int64_t)(t1-t0);
  float raw=cpuz_score_raw(freq,work,delta);
  float ms_score=cpuz_score_by_ms(freq,work,delta,0);
  (void)ms_score;
  *out_first=raw; // supervisor stores first kernel s0 to [score+0x14]
  usleep(CPUZ_SLEEP_MS*1000); // Sleep(10)
  // pass 2
  memset(buf,0,CPUZ_BENCH_BUF_BYTES);
  t0=cpuz_qpc_now_ns(); work=0;
  do {
    int r=run_map_once(tab,buf,nthreads,0,0,CPUZ_F_INV1024*CPUZ_F_40);
    work+=r; t1=cpuz_qpc_now_ns();
  } while(!cpuz_timeout_hit(freq,(int64_t)(t1-t0),CPUZ_BENCH_ITERS));
  delta=(int64_t)(t1-t0);
  float raw2=cpuz_score_raw(freq,work,delta);
  *out_second=raw2;
  return 0;
}

#ifndef CPUZ_BENCH_NO_MAIN
int main(int argc,char**argv){
  int nthreads=4; if(argc>1) nthreads=atoi(argv[1]);
  static float tab[4096]; cpuz_sin_table_init(tab);
  // self-test: scalar vs NEON bit-exact per pixel (same op order per lane)
  {
    static uint8_t a[64*4*8],b[64*4*8];
    cpuz_tile_rgba_scalar(tab,a,64,8,64*4,0,0,CPUZ_F_INV1024*CPUZ_F_40);
#ifdef __ARM_NEON
    cpuz_tile_rgba_neon(tab,b,64,8,64*4,0,0,CPUZ_F_INV1024*CPUZ_F_40);
    if(memcmp(a,b,sizeof(a))!=0){fprintf(stderr,"NEON mismatch\n");return 2;}
    printf("tile scalar==neon OK\n");
#endif
    // hash spot values
    float o0,o1; cpuz_hash2(tab,1.0f,2.0f,&o0,&o1);
    printf("hash(1,2)=%.9g %.9g\n",o0,o1);
    printf("simplex(0.5,0.5)=%.9g\n",cpuz_simplex2(tab,0.5f,0.5f));
  }
  // scoring vectors (must match asm integer semantics)
  {
    // tiny: freq=1e9, work=1024 rows, delta=100ms=1e8ns -> q=1e9*1024/1e8=10240
    float r=cpuz_score_raw(1000000000LL,1024,100000000LL);
    int64_t ms; float m=cpuz_score_by_ms(1000000000LL,1024,100000000LL,&ms);
    printf("score_raw(1e9,1024,1e8)=%.9g ms=%lld ms_score=%.9g\n",r,(long long)ms,m);
    printf("timeout(1e9,9.9s,10000)=%d timeout(1e9,10s,10000)=%d timeout(-1)=%d\n",
      cpuz_timeout_hit(1000000000LL,9900000000LL,10000),
      cpuz_timeout_hit(1000000000LL,10000000000LL,10000),
      cpuz_timeout_hit(1000000000LL,10000000000LL,-1));
  }
  uint8_t *buf=malloc(CPUZ_BENCH_BUF_BYTES);
  float f1=-1,s2=-1;
  // quick single-pass timing demo (not full 10s) if --quick
  if(argc>2&&!strcmp(argv[2],"--quick")){
    memset(buf,0,CPUZ_BENCH_BUF_BYTES);
    uint64_t t0=cpuz_qpc_now_ns();
    int r=run_map_once(tab,buf,nthreads,0,0,CPUZ_F_INV1024*CPUZ_F_40);
    uint64_t t1=cpuz_qpc_now_ns();
    float raw=cpuz_score_raw(cpuz_qpc_freq(),r,(int64_t)(t1-t0));
    printf("quick rows=%d dt_ns=%llu raw=%.9g\n",r,(unsigned long long)(t1-t0),raw);
    return 0;
  }
  cpuz_bench_run(tab,buf,nthreads,&f1,&s2);
  printf("bench nthreads=%d first=%.9g second=%.9g\n",nthreads,f1,s2);
  return 0;
}
#endif
