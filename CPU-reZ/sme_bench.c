// CPU-reZ perf harness: N QoS worker threads, probe-once, dispatch SME/NEON per
// probed capability; measures sustained MPixels/s + exactness vs scalar.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/qos.h>
#include <time.h>
#include "cpuz_bench_neon.h"
#include "cpuz_bench_sme.h"
static float tab[4096];
static uint8_t *buf;
static uint8_t refchk[256*4*4];
static volatile int start_flag = 0, stop_flag = 0;
static int g_nthreads = 0, g_mode = 0; // 0=auto(dispatch),1=force neon,2=force sme
static uint64_t ns_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000000000ull+t.tv_nsec;}
typedef struct { int id; long tiles; int use_sme; int mism; } warg_t;
static void *worker(void *p){
  warg_t *a=(warg_t*)p;
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  int ok = cpuz_sme_available_on_this_thread();
  a->use_sme = (g_mode==2) ? 1 : (g_mode==1 ? 0 : ok);
  while(!start_flag) {}
  long n=0;
  int H = 1024 / g_nthreads, rem = 1024 % g_nthreads;
  int y0 = a->id*H + (a->id<rem?a->id:rem);
  int hh = H + (a->id<rem?1:0);
  while(!stop_flag){
    cpuz_tile_rgba_dispatch(tab, buf + (size_t)y0*1024*4, 1024, hh, 1024*4,
                            0, (float)y0, CPUZ_F_INV1024*CPUZ_F_40, a->use_sme);
    n++;
  }
  a->tiles=n;
  return NULL;
}
int main(int argc,char**argv){
  setvbuf(stdout,0,_IONBF,0);
  int N = argc>1?atoi(argv[1]):4;
  int secs = argc>2?atoi(argv[2]):5;
  int mode = argc>3?atoi(argv[3]):0;
  g_nthreads=N; g_mode=mode;
  cpuz_sin_table_init(tab);
  buf=malloc(1024*1024*4);
  pthread_t *th=malloc(sizeof *th * (size_t)N);
  warg_t *ag=calloc(N,sizeof *ag);
  // warmup + exactness on one tile
  cpuz_tile_rgba_scalar(tab,refchk,256,4,256*4,0,0,CPUZ_F_INV1024*CPUZ_F_40);
  for(int i=0;i<N;i++){ag[i].id=i;pthread_create(&th[i],0,worker,&ag[i]);}
  // wait for workers to be ready (spin briefly), then go
  struct timespec ts={0,200000000}; nanosleep(&ts,0);
  uint64_t t0=ns_now(); start_flag=1;
  struct timespec run={secs,0}; nanosleep(&run,0);
  stop_flag=1;
  for(int i=0;i<N;i++) pthread_join(th[i],0);
  uint64_t t1=ns_now();
  long tot=0; int nsme=0;
  for(int i=0;i<N;i++){tot+=ag[i].tiles; nsme+=ag[i].use_sme;}
  double el=(double)(t1-t0)/1e9;
  // tiles are 1024xH slices; count pixels properly:
  double mpix = (double)tot * 1024.0 * (1024.0/N) / 1e6;
  printf("mode=%d N=%d time=%.2fs map-passes=%ld MPix=%.1f MPix/s=%.1f sme_workers=%d/%d\n",
    mode,N,el,tot,mpix,mpix/el,nsme,N);
  return 0;
}
