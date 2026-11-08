/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission. */
/* Father: an eight-line Hadamard FDN at 11,025 Hz with predelay and early reflections at 22,050 Hz
   (docs/father-redesign.md, candidate A), inside R246's 44,100 Hz FX call (father_dsp.h); s->classic runs 3.1.x's
   counts (MW_NC tank samples, 64 early, no decimation of the input or interpolation of the wet). The same samples as
   father_reference.c; this file splits ring accesses into contiguous pieces instead of wrapping every index. */
#include "father_dsp.h"
#include "father_tables.h"
_Static_assert(MW_N==16&&MW_NC==32,"father_dsp.c and father_kern.S assume 16 (CLASSIC 32) tank samples per callback");
_Static_assert(sizeof(((mw_state *)0)->rows)>=64*4,"out() keeps its 64 output pairs in rows");
_Static_assert(__builtin_offsetof(mw_state,ap)%32==0&&__builtin_offsetof(mw_state,pre)%32==0,"rings on 32-byte lines");
#define ROW(i,n) s->rows[(i)>>2][n][(i)&3]
static inline int32_t sat(int32_t v){return v>32767?32767:v< -32768?-32768:v;}
/* Round toward zero: silence cannot sustain a negative one-LSB limit cycle. */
static inline int32_t mul(int32_t a,int32_t b){int32_t v=a*b;return (v+((v>>31)&32767))>>15;}
static inline int32_t half(int32_t v){return (v+(int32_t)((uint32_t)v>>31))>>1;}
/* a load that brings a line into the data cache before the stores to it (no write-allocate) */
#define TOUCH(p) ((void)*(volatile const int16_t *)(p))
void mw_init_fields(mw_state *s){            /* the non-zero fields of a cleared state */
 s->mix=-1;s->pre_samples=-1;s->gate_gain=32767;
 for(unsigned i=0;i<8;i++)s->phase[i]=0x9E3779B9u*(i+1u);
}
void mw_init(mw_state *s){
 _Static_assert(sizeof *s%4==0,"mw_state size");
 uint32_t *p=(uint32_t *)(void *)s;for(unsigned i=0;i<sizeof *s/4;i++)p[i]=0;
 mw_init_fields(s);
}
#ifdef __bfin__
typedef struct{const int16_t *p,*b;uint32_t lb;int32_t *hist;int16_t *out;uint32_t quads;}fa_dc_t;   /* quads: outputs / 4 */
void fa_decim(const fa_dc_t *t);
typedef struct{int16_t *x,*ia,*ba;uint32_t lba;int16_t *ib,*bb;uint32_t lbb,pairs;}fa_ap_t;   /* pairs: samples / 2 */
void fa_ap2(const fa_ap_t *t);
#endif
#ifndef __bfin__
/* half-band 2:1, n outputs from 2 n contiguous samples */
static void decimate(int32_t hist[5],const int16_t *x,int16_t *y,unsigned n){
 int32_t s0=hist[0],s2=hist[2],s3=hist[3],s4=hist[4];
 for(unsigned k=0;k<n;k++){
  const int32_t a=x[2*k],b=x[2*k+1];
  y[k]=(int16_t)sat((9*(s2+s4)+16*s3-s0-b)>>5);
  s0=s2;s2=s4;s3=a;s4=b;
 }
 hist[0]=s0;hist[1]=x[2*n-4];hist[2]=s2;hist[3]=s3;hist[4]=s4;
}
static void tap(const int16_t *ring,unsigned at,int16_t *o,unsigned m){   /* m samples from at, the ring unrolled */
 unsigned n1=MW_PRE_LEN-at;if(n1>m)n1=m;
 for(unsigned n=0;n<n1;n++)o[n]=ring[at+n];
 for(unsigned n=n1;n<m;n++)o[n]=ring[n-n1];
}
static void allpass(int16_t *ring,unsigned len,uint16_t *hp,int16_t *x,unsigned m){
 unsigned h=*hp;
 for(unsigned k=0;k<m;k++){
  int32_t d=ring[h],v=sat(x[k]-half(d));ring[h]=(int16_t)v;x[k]=(int16_t)sat(d+half(v));
  if(++h==len)h=0;
 }
 *hp=(uint16_t)h;
}
/* one line, the delay held for the callback: row[4 n] from the ring at h - delay (+ 1 for the interpolation) */
#endif
/* the 64 input samples' peak, and the 2:1-decimated MW_IN into a predelay ring at h (a multiple of 16, the ring
   32-byte aligned and its length a multiple of 16); CLASSIC: the 64 as they are */
static int32_t input(int16_t *ring,unsigned h,int32_t hist[5],const int16_t *x,int32_t peak,int classic){
 if(classic){
  for(unsigned n=0;n<64;n+=16){
   int16_t *w=ring+h;TOUCH(w);
   for(unsigned j=0;j<16;j++){int32_t v=x[n+j];w[j]=(int16_t)v;v=v<0?-v:v;if(v>peak)peak=v;}
   h+=16;if(h==MW_PRE_LEN)h=0;
  }
  return peak;
 }
 for(unsigned n=0;n<64;n++){int32_t v=x[n];v=v<0?-v:v;if(v>peak)peak=v;}
 int16_t y[MW_IN];
#ifdef __bfin__
 const fa_dc_t d={x,x,128,hist,y,MW_IN/4};fa_decim(&d);
#else
 decimate(hist,x,y,MW_IN);
#endif
 for(unsigned n=0;n<MW_IN;n+=16){
  int16_t *w=ring+h;TOUCH(w);
  for(unsigned j=0;j<16;j++)w[j]=y[n+j];
  h+=16;if(h==MW_PRE_LEN)h=0;
 }
 return peak;
}
#ifdef __bfin__
typedef struct{const int16_t *p,*b;uint32_t lb;int16_t *row;uint32_t frs,keeps;int32_t damp;uint32_t pairs;}fa_tr_t;   /* pairs: samples / 2 */
int32_t fa_line_read(const fa_tr_t *t);
#endif
static void tank_read(const int16_t *ring,unsigned len,unsigned h,int16_t *row,uint32_t *phasep,int32_t *dampp,
                      unsigned delay,int32_t depth,uint32_t speed,int32_t keep,unsigned m){
 const uint32_t p=*phasep;*phasep=p+speed*m;
 const int32_t tri=(int32_t)(((p>>15)^(0u-(p>>31)))&65535)-32768;
 const int32_t dq=(int32_t)delay*256+mul(tri,depth),fr=dq&255;
 int32_t damp=*dampp;
 TOUCH(ring+h);                                 /* the write window's first line (the reads start 2+ samples later) */
 unsigned b=h+len-((unsigned)dq>>8)-1;if(b>=len)b-=len;      /* p[-1] of the first sample */
#ifdef __bfin__
 const fa_tr_t t={ring+b,ring,2*len,row,(uint32_t)fr<<16|(uint32_t)(256-fr),(uint32_t)keep<<16|(uint32_t)(32768-keep),damp,m/2};
 *dampp=fa_line_read(&t);
 return;
#endif
 const unsigned n1=len-b>m+1?m+1:len-b;        /* m + 1 samples from b, in at most two pieces */
 int16_t w[MW_NC+1];const int16_t *x=ring+b;
 if(n1<m+1){for(unsigned n=0;n<n1;n++)w[n]=ring[b+n];for(unsigned n=n1;n<=m;n++)w[n]=ring[n-n1];x=w;}
 int32_t prev=x[0];
 for(unsigned n=0;n<m;n++){
  int32_t v=x[n+1];v+=((prev-v)*fr)>>8;prev=x[n+1];
  damp=v+mul(damp-v,keep);row[4*n]=(int16_t)damp;
 }
 *dampp=damp;
}
#ifdef __bfin__
typedef struct{const int16_t *i[4],*a,*b;uint32_t lb;int16_t *out;uint32_t coef;int32_t e;uint32_t count;}fa_er_t;
int32_t fa_early(const fa_er_t *t);
#endif
static void early(const int16_t *a,const int16_t *b,unsigned at,const uint16_t *t,int32_t coef,int32_t *ep,int16_t *row,
                  unsigned m){
 int32_t e=*ep;unsigned i[4];int fast=1;
 for(unsigned k=0;k<4;k++){i[k]=at+MW_PRE_LEN-t[k];if(i[k]>=MW_PRE_LEN)i[k]-=MW_PRE_LEN;if(i[k]+m>MW_PRE_LEN)fast=0;}
#ifdef __bfin__
 (void)fast;
 const fa_er_t f={{a+i[0],b+i[1],a+i[2],b+i[3]},a,b,2*MW_PRE_LEN,row,(uint32_t)(32768-coef)<<16|(uint32_t)coef,e,m};
 *ep=fa_early(&f);
 return;
#endif
 if(fast){
  const int16_t *p0=a+i[0],*p1=b+i[1],*p2=a+i[2],*p3=b+i[3];
  for(unsigned n=0;n<m;n++){
   const int32_t x=(p0[n]>>2)+(p1[n]>>3)-(p2[n]>>4)+(p3[n]>>4);
   e+=mul(x-e,coef);row[n]=(int16_t)e;
  }
 }else for(unsigned n=0;n<m;n++){
  int32_t x=0;
  for(unsigned k=0;k<4;k++){
   unsigned j=i[k]+n;if(j>=MW_PRE_LEN)j-=MW_PRE_LEN;
   const int32_t v=(k&1?b:a)[j];x+=k==0?v>>2:k==1?v>>3:k==2?-(v>>4):v>>4;
  }
  e+=mul(x-e,coef);row[n]=(int16_t)e;
 }
 *ep=e;
}
#ifdef __bfin__
/* father_kern.S: the same loops, scheduled by hand (bit-exact against the C, kern_check.py) */
typedef struct{int16_t *i[4],*b[4];uint32_t lb[4];const int16_t *rows,*in;uint32_t k02,k46,count;}fa_tw_t;
void fa_tank_write(const fa_tw_t *t);
#endif
static void tank_write(mw_state *s,const int16_t *k,unsigned m){
#ifdef __bfin__
 for(unsigned g=0;g<2;g++){
  fa_tw_t t;
  for(unsigned j=0;j<4;j++){
   const unsigned L=g+2*j,h=s->hd[L],len=mw_len[L];
   t.b[j]=s->tank+mw_off[L];t.lb[j]=2*len;t.i[j]=t.b[j]+(j<2?h:h?h-1:len-1);
  }
  t.rows=s->rows[g][0];t.in=s->in[g];
  t.k02=(uint32_t)(uint16_t)k[g+2]<<16|(uint16_t)k[g];t.k46=(uint32_t)(uint16_t)k[g+6]<<16|(uint16_t)k[g+4];t.count=m;
  fa_tank_write(&t);
 }
 for(unsigned L=0;L<8;L++){unsigned h=s->hd[L]+m;s->hd[L]=(uint16_t)(h>=mw_len[L]?h-mw_len[L]:h);}
 return;
#endif
 int16_t *t[8];int wrap=0;
 for(unsigned L=0;L<8;L++){t[L]=s->tank+mw_off[L]+s->hd[L];wrap|=s->hd[L]+m>mw_len[L];}
 for(unsigned n=0;n<m;n++){
  if(wrap)for(unsigned L=0;L<8;L++){unsigned p=s->hd[L]+n;if(p>=mw_len[L])p-=mw_len[L];t[L]=s->tank+mw_off[L]+p-n;}
  for(unsigned g=0;g<2;g++){
   const int32_t d0=ROW(g*4,n),d1=ROW(g*4+1,n),d2=ROW(g*4+2,n),d3=ROW(g*4+3,n);
   const int32_t a0=d0+d1,a1=d0-d1,a2=d2+d3,a3=d2-d3;
   const int32_t in=s->in[g][n]>>2,ni=(-s->in[g][n])>>2;
   t[g][n]=(int16_t)sat(mul(sat(half(a0+a2)),k[g])+in);
   t[g+2][n]=(int16_t)sat(mul(sat(half(a1+a3)),k[g+2])+ni);
   t[g+4][n]=(int16_t)sat(mul(sat(half(a0-a2)),k[g+4])+in);
   t[g+6][n]=(int16_t)sat(mul(sat(half(a1-a3)),k[g+6])+ni);
  }
 }
 for(unsigned L=0;L<8;L++){unsigned h=s->hd[L]+m;s->hd[L]=(uint16_t)(h>=mw_len[L]?h-mw_len[L]:h);}
}
/* the left (c 0) or right late sum, m tank samples interpolated x2, added to e[] (saturated): erow[c][MW_IN..]
   at 22,050 Hz (CLASSIC: erow[c][0..63] at 44,100) */
#ifdef __bfin__
typedef struct{const int16_t *r0,*r1;int16_t *e;int32_t *up;uint32_t count;}fa_lt_t;
void fa_late(const fa_lt_t *t);
static void late(mw_state *s,unsigned c,int16_t *e,unsigned m){const fa_lt_t t={&s->rows[0][0][c],&s->rows[1][0][c],e,s->up[c],m};fa_late(&t);}
#else
static void late(mw_state *s,unsigned c,int16_t *e,unsigned m){
 int32_t u0=s->up[c][0],u1=s->up[c][1],u2=s->up[c][2];
 for(unsigned k=0;k<m;k++){
  const int32_t t=ROW(c,k)+ROW(c+2,k)-ROW(c+4,k)-ROW(c+6,k),x=half(t+half(t));
  e[2*k]=(int16_t)sat(u1+e[2*k]);e[2*k+1]=(int16_t)sat(((9*(u1+u2)-(u0+x))>>4)+e[2*k+1]);
  u0=u1;u1=u2;u2=x;
 }
 s->up[c][0]=u0;s->up[c][1]=u1;s->up[c][2]=u2;
}
#endif
/* the wet at 22,050 Hz (e[MW_IN..]) to 64 samples at 44,100 Hz in place (e[0..63]; output 2k, 2k + 1 never passes
   the input k + MW_IN still to be read), the same x2 interpolator (-1 9 9 -1)/16, saturated */
static void upsample(int32_t u[3],int16_t *e){
 int32_t u0=u[0],u1=u[1],u2=u[2];
 for(unsigned k=0;k<MW_IN;k++){
  const int32_t x=e[MW_IN+k];
  e[2*k]=(int16_t)u1;e[2*k+1]=(int16_t)sat((9*(u1+u2)-(u0+x))>>4);
  u0=u1;u1=u2;u2=x;
 }
 u[0]=u0;u[1]=u1;u[2]=u2;
}
/* the 44,100 Hz output: mode 0 no gate, 1 opening, 2 closing; r == l and wr == wl for a mono call. The gain's steps
   are per 44,100 Hz sample: half the design's 22,050 Hz steps, the same times (opening: open_add 32767 + 63,
   open_shift 6); CLASSIC takes the design's steps (32767 + 31, 5) */
typedef struct{int16_t *l,*r;const int16_t *wl,*wr;int32_t mix,step,peak,gain,mode,close_rate;uint32_t *pairs;
               int32_t open_add,open_shift;}out_t;
#ifdef __bfin__
void fa_out(out_t *m);   /* father_kern.S */
#define out fa_out
#else
static void out(out_t *m){
 int32_t mix=m->mix,gain=m->gain,peak=0;
 for(unsigned o=0;o<64;o++){
  if(m->mode==1)gain+=(m->open_add-gain)>>m->open_shift;
  else if(m->mode==2)gain-=(gain*m->close_rate+32767)>>15;
  mix+=m->step;const int32_t wet=mix>>6;
  int32_t wl=m->wl[o],wr=m->wr[o];if(m->mode){wl=mul(wl,gain);wr=mul(wr,gain);}
  const int32_t a=m->l[o],b=m->r[o],ol=sat(a+mul(wl-a,wet)),orr=sat(b+mul(wr-b,wet));
  m->r[o]=(int16_t)orr;m->l[o]=(int16_t)ol;   /* r first: a mono call (r == l) keeps out_l */
  const int32_t pl=ol<0?-ol:ol,pr=orr<0?-orr:orr;if(pl>peak)peak=pl;if(pr>peak)peak=pr;
 }
 m->mix=mix;m->gain=gain;m->peak=peak;
}
#endif
void mw_process(mw_state *s,int16_t *l,int16_t *r,const int32_t q[4]){
 const int stereo=r!=0;if(!r)r=l;
 const int classic=s->classic;const unsigned n=classic?MW_NC:MW_N,nin=2*n;   /* tank / predelay samples a call */
 unsigned ix=((unsigned)q[0]*128+16383)>>15;if(ix>128)ix=128;
 const int32_t damping=1800+mul(q[2],20000),target_pre=mul(q[0],MW_PRE_MAX);
 const int32_t keep=(int32_t)(((uint32_t)(32768-damping)*(uint32_t)(32768-damping))>>15);
 const int gated=q[1]!=0;const int32_t free=32767-q[1];
 const uint32_t hold_samples=1764u+(uint32_t)mul(mul(free,free),37926);
 const int32_t close_rate=classic?64+mul(q[1],192):32+mul(q[1],96);   /* per 44,100 Hz sample: 160 .. 40 ms to -60 dB */
 const int32_t target_mix=q[3];if(s->mix<0)s->mix=target_mix;
 int slow=1;                                     /* CLASSIC: every call */
 if(!classic){slow=s->tick;s->tick=(uint16_t)!slow;}   /* the second call of a pair: the design's 2.9 ms callback */
 if(s->pre_samples<0)s->pre_samples=target_pre;
 if(slow){if(s->pre_samples<target_pre)s->pre_samples++;else if(s->pre_samples>target_pre)s->pre_samples--;}
 const int32_t mix=s->mix*64,step=target_mix-s->mix;
 /* the per-callback rows are written before they are read: bring their lines in first */
 for(unsigned g=0;g<2;g++){
  for(unsigned i=0;i<4*n;i+=16)TOUCH(&s->rows[g][0][0]+i);
  for(unsigned i=0;i<n;i+=16)TOUCH(&s->in[g][i]);
 }
 for(unsigned c=0;c<1u+(unsigned)stereo;c++)for(unsigned i=0;i<64;i+=16)TOUCH(&s->erow[c][i]);
 const unsigned ph=s->pre_hd;
 int32_t input_peak=input(s->pre[0],ph,s->front[0],l,0,classic);
 if(stereo)input_peak=input(s->pre[1],ph,s->front[1],r,input_peak,classic);
 const int16_t *pr=s->pre[stereo];
 if(s->gate_peak>input_peak)input_peak=s->gate_peak;
 if(!slow)s->gate_peak=input_peak;
 else{
  s->gate_peak=0;
  /* gate detector, once per two calls on their peak */
  const int32_t previous_env=s->gate_env;const int was_active=s->gate_active;
  if(input_peak>s->gate_env)s->gate_env=input_peak;else s->gate_env-=(s->gate_env+3)>>2;
  if(!s->gate_active&&s->gate_env>=32)s->gate_active=1;else if(s->gate_active&&s->gate_env<=16)s->gate_active=0;
  const int onset=(!was_active&&s->gate_active)||(input_peak>=32&&input_peak>previous_env+(previous_env>>1)+32);
  if(!gated){s->gate_enabled=0;s->gate_armed=0;s->gate_hold=0;}
  else{
   if(!s->gate_enabled){s->gate_enabled=1;s->gate_armed=0;s->gate_hold=0;}
   if(onset)s->gate_armed=1;
   if(s->gate_armed){
    if(s->gate_active)s->gate_hold=hold_samples+(unsigned)s->pre_samples;
    else s->gate_hold=s->gate_hold>64?s->gate_hold-64:0;
   }
  }
 }
 const int32_t gate_target=(!gated||!s->gate_armed||s->gate_hold)?32767:0;
 const int apply_gate=gate_target!=32767||s->gate_gain!=32767;
 /* front: the predelay tap decimated to the tank rate, two allpasses per channel */
 unsigned at=ph+2*MW_PRE_LEN-(unsigned)s->pre_samples;while(at>=MW_PRE_LEN)at-=MW_PRE_LEN;
 for(unsigned c=0;c<1u+(unsigned)stereo;c++){
  const int16_t *ring=c?pr:s->pre[0];
#ifdef __bfin__
  const fa_dc_t d={ring+at,ring,2*MW_PRE_LEN,s->dec[c],s->in[c],n/4};fa_decim(&d);
#else
  if(at+nin<=MW_PRE_LEN)decimate(s->dec[c],ring+at,s->in[c],n);
  else{int16_t t[2*MW_NC];tap(ring,at,t,nin);decimate(s->dec[c],t,s->in[c],n);}
#endif
 }
 if(!stereo)for(unsigned k=0;k<n;k++)s->in[1][k]=s->in[0][k];
#ifdef __bfin__
 for(unsigned c=0;c<2;c++){
  int16_t *ra=s->ap+mw_ap_off[2*c],*rb=s->ap+mw_ap_off[2*c+1];
  const fa_ap_t t={s->in[c],ra+s->ap_hd[2*c],ra,2u*mw_ap_len[2*c],rb+s->ap_hd[2*c+1],rb,2u*mw_ap_len[2*c+1],n/2};fa_ap2(&t);
  for(unsigned a=2*c;a<2*c+2;a++){unsigned h=s->ap_hd[a]+n;while(h>=mw_ap_len[a])h-=mw_ap_len[a];s->ap_hd[a]=(uint16_t)h;}
 }
#else
 allpass(s->ap+MW_AP_OFF0,MW_AP_LEN0,&s->ap_hd[0],s->in[0],n);allpass(s->ap+MW_AP_OFF1,MW_AP_LEN1,&s->ap_hd[1],s->in[0],n);
 allpass(s->ap+MW_AP_OFF2,MW_AP_LEN2,&s->ap_hd[2],s->in[1],n);allpass(s->ap+MW_AP_OFF3,MW_AP_LEN3,&s->ap_hd[3],s->in[1],n);
#endif
 for(unsigned i=0;i<8;i++)
  tank_read(s->tank+mw_off[i],mw_len[i],s->hd[i],&s->rows[i>>2][0][i&3],s->phase+i,s->damp+i,mw_delay[i],mw_depth[i],mw_speed[i],keep,n);
 const unsigned eo=classic?0:MW_IN;             /* the wet's start in erow */
 early(s->pre[0],pr,at,mw_early[0],damping,&s->early[0],s->erow[0]+eo,nin);
 if(stereo)early(pr,s->pre[0],at,mw_early[1],damping,&s->early[1],s->erow[1]+eo,nin);
 /* the wet: the late sum at the tank rate interpolated plus the early reflections (into erow, in place), to 44,100 Hz */
 late(s,0,s->erow[0]+eo,n);if(stereo)late(s,1,s->erow[1]+eo,n);
 if(!classic){upsample(s->up2[0],s->erow[0]);if(stereo)upsample(s->up2[1],s->erow[1]);}
 tank_write(s,mw_decay[ix],n);                    /* the rows' last reader; out() keeps its pairs there */
 /* gate and dry/wet at 44,100 Hz */
 out_t m={l,r,s->erow[0],s->erow[stereo],mix,step,0,s->gate_gain,!apply_gate?0:gate_target?1:2,close_rate,
          (uint32_t *)(void *)s->rows,classic?32767+31:32767+63,classic?5:6};
 out(&m);
 s->gate_gain=m.gain;s->peak=m.peak;
 s->pre_hd=(uint16_t)(ph+nin>=MW_PRE_LEN?ph+nin-MW_PRE_LEN:ph+nin);
 s->mix=target_mix;
}
