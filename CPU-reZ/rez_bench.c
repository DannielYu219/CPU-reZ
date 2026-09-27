// CPU-reZ end-to-end bench: same driver model as legacy run.c (persistent workers,
// dynamic dispatch, QPC scoring, X Elite refs) but tile backend selectable:
// --auto (per-thread probe, default), --neon (force), --sme (force).
// Expected: identical DISPLAY scores across backends for same work (rate differs).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/qos.h>
#include <time.h>
#include <unistd.h>
#include "cpuz_bench_neon.h"
#include "cpuz_bench_sme.h"
#include "cpuz_bench_ref.h"

typedef struct { pthread_mutex_t m; pthread_cond_t c; int count, total, gen; } xb_t;
static void xb_init(xb_t *b, int t){pthread_mutex_init(&b->m,0);pthread_cond_init(&b->c,0);b->count=0;b->total=t;b->gen=0;}
static void xb_wait(xb_t *b){pthread_mutex_lock(&b->m);int g=b->gen;
  if(++b->count==b->total){b->count=0;b->gen++;pthread_cond_broadcast(&b->c);}
  else while(b->gen==g) pthread_cond_wait(&b->c,&b->m); pthread_mutex_unlock(&b->m);}
static uint64_t qnow(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000000000ull+t.tv_nsec;}

typedef struct { const float *tab; uint8_t *buf; int w,h; float ox,oy,scale;
  xb_t *bs,*bd; volatile int *stop; int *rows; volatile int *cur; int backend; } wq_t;
#define CHUNK 2
static void *wfn(void *p){
  wq_t *a=(wq_t*)p;
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  int sme = cpuz_sme_available_on_this_thread();
  int use = (a->backend==2) ? 1 : (a->backend==1 ? 0 : sme);
  for(;;){ xb_wait(a->bs); if(*a->stop) break;
    int done=0;
    for(;;){ int y0=__atomic_fetch_add(a->cur, CHUNK, __ATOMIC_RELAXED);
      if(y0>=a->h) break; int y1=y0+CHUNK; if(y1>a->h) y1=a->h;
      cpuz_tile_rgba_dispatch(a->tab, a->buf+(size_t)y0*a->w*4, a->w, y1-y0, a->w*4,
                              a->ox, a->oy+y0*a->scale, a->scale, use);
      done+=y1-y0; }
    *a->rows=done; xb_wait(a->bd); }
  return NULL;
}
int main(int argc,char**argv){
  setvbuf(stdout,0,_IONBF,0);
  int N=argc>1?atoi(argv[1]):4, ref=argc>2?atoi(argv[2]):11, backend=0;
  int quick=0;
  for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--neon"))backend=1;
    if(!strcmp(argv[i],"--sme"))backend=2; if(!strcmp(argv[i],"--auto"))backend=0;
    if(!strcmp(argv[i],"--quick"))quick=1; }
  if(ref<0||ref>=CPUZ_REF_COUNT) ref=CPUZ_REF_DEFAULT;
  static float tab[4096]; cpuz_sin_table_init(tab);
  uint8_t *buf=malloc(CPUZ_BENCH_BUF_BYTES);
  xb_t bs,bd; xb_init(&bs,N+1); xb_init(&bd,N+1);
  volatile int stop=0; static volatile int cur=0;
  wq_t *ws=calloc(N,sizeof *ws); pthread_t *th=calloc(N,sizeof *th); int *rs=calloc(N,sizeof *rs);
  float sc=CPUZ_F_INV1024*CPUZ_F_40;
  for(int i=0;i<N;i++){ws[i]=(wq_t){tab,buf,1024,1024,0,0,sc,&bs,&bd,&stop,&rs[i],&cur,backend};
    pthread_create(&th[i],0,wfn,&ws[i]);}
  int64_t freq=cpuz_qpc_freq();
  float raws[2];
  for(int pass=0;pass<2;pass++){
    if(!quick){ // warmup maps
      for(int k=0;k<3;k++){ cur=0; memset(buf,0,CPUZ_BENCH_BUF_BYTES); xb_wait(&bs); xb_wait(&bd); }
    }
    cur=0; memset(buf,0,CPUZ_BENCH_BUF_BYTES);
    uint64_t t0=qnow(); xb_wait(&bs); xb_wait(&bd); uint64_t t1=qnow();
    int64_t work=0; for(int i=0;i<N;i++) work+=rs[i];
    raws[pass]=cpuz_score_by_ms(freq,work,(int64_t)(t1-t0),0);
    printf("pass%d: work=%lld ns=%llu raw=%.4f\n",pass,(long long)work,(unsigned long long)(t1-t0),raws[pass]);
    usleep(10000);
  }
  stop=1; xb_wait(&bs); for(int i=0;i<N;i++) pthread_join(th[i],0);
  float m=(raws[0]+raws[1])*0.5f;
  int head=(int)(m*256.0f);
  const cpuz_ref_t *R=&CPUZ_REFS[ref];
  printf("backend=%d N=%d raw=%.4f headline=%d vs-ref=%d%%\n", backend, N, m, head,
    (int)((float)head*100.0f/(N==1?R->single_ref:R->multi_ref)));
  return 0;
}
