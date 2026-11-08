/* Original fixed-point input strip; no TE algorithm or artwork. GPL-3.0-or-later with OP-1 permission. */
#ifndef AMP_DSP_H
#define AMP_DSP_H
#include <stdint.h>
#include "amp_voices.h"
/* xh: pre-drive input ring, clipped (2x interpolation); dh: the same unclipped (dry delay); wo/we: shaped odd/even 2x samples;
 * lx/ly/le, bx/by/be: the voice's low cuts and biquads, int16 history plus the rounding remainder (error feedback). */
typedef struct { int32_t dc_x,dc_y,low,env,gain,volume,tilt,drive;uint32_t rate;int fade;
 int32_t gate,hold,open,sag;unsigned pos;int32_t blk[64],dh[8];int16_t xh[8],wo[8],we[4];
 int16_t lx[2],ly[2];uint16_t le[2];int16_t bx[3][2],by[3][2];uint16_t be[3]; } amp_dsp;
static inline int32_t amp_clip(int32_t x){return x>32767?32767:x< -32768?-32768:x;}
static inline int32_t amp_abs(int32_t x){return x<0?-x:x;}
/* Operands bounded to |a|<=131072 and nonnegative b<=8*32768. No 64-bit helper in audio. */
static inline int32_t amp_mul(int32_t a,int32_t b){return a*(b>>15)+(a>>15)*(b&32767)+(((a&32767)*(b&32767)+16384)>>15);}
/* One 16x16 multiply (a single Blackfin MAC). Sums use uint32 so partial sums may wrap; the final results fit. */
static inline uint32_t amp_m16(int16_t a,int16_t b){return (uint32_t)((int32_t)a*b);}
static inline int32_t amp_soft(int32_t x){x=amp_clip(x);int32_t x2=(x*x)>>15;return (3*x-((x2*x)>>15))>>1;}
static inline int amp_voice_index(int16_t w){return w<=0?1:(w*AMP_VOICES)>>15;}   /* unset word (old patches): CRUNCH */
static inline void amp_dsp_reset(amp_dsp *s,uint32_t rate){
 s->dc_x=s->dc_y=s->low=s->env=0;s->gain=32768;s->volume=32768;s->tilt=s->drive=0;s->rate=rate;s->fade=0;
 s->gate=32768;s->hold=s->open=0;s->sag=32768;s->pos=0;
 for(int i=0;i<8;i++){s->xh[i]=s->wo[i]=0;s->dh[i]=0;}
 for(int i=0;i<4;i++)s->we[i]=0;
 for(int i=0;i<2;i++){s->lx[i]=s->ly[i]=0;s->le[i]=0;}
 for(int i=0;i<3;i++){s->bx[i][0]=s->bx[i][1]=s->by[i][0]=s->by[i][1]=0;s->be[i]=0;}
}
/* Voice cascade: two first-order low cuts (1-z^-1)/(1-p z^-1) with Q15 poles (the first is the tone stack's lowest
 * pole with its zero at DC, the post-drive DC block), then three Q14 biquads (the rest of the Bassman stack, presence,
 * roll-off). DF1 with first-order error feedback, so silence decays to exact zero. */
static inline int32_t amp_cascade(amp_dsp *s,const amp_voice *v,int32_t x){
 for(int k=0;k<2;k++){
  uint32_t a=(uint32_t)(x-s->lx[k])*32768u+amp_m16(s->ly[k],v->low[k])+s->le[k];
  int32_t y=(int32_t)a>>15;s->le[k]=(uint16_t)(a&32767);s->lx[k]=(int16_t)x;
  y=amp_clip(y);s->ly[k]=(int16_t)y;x=y;
 }
 for(int k=0;k<3;k++){
  const int16_t *c=v->bq[k];
  uint32_t a=amp_m16(x,c[0])+amp_m16(s->bx[k][0],c[1])+amp_m16(s->bx[k][1],c[2])-amp_m16(s->by[k][0],c[3])-amp_m16(s->by[k][1],c[4])+s->be[k];
  int32_t y=(int32_t)a>>14;s->be[k]=(uint16_t)(a&16383);
  s->bx[k][1]=s->bx[k][0];s->bx[k][0]=(int16_t)x;y=amp_clip(y);s->by[k][1]=s->by[k][0];s->by[k][0]=(int16_t)y;x=y;
 }
 return x;
}
/* Half-band pair sums around a ring position: taps c[0..3] at distances 1, 3, 5, 7 (2x samples). */
static inline uint32_t amp_hb_pairs(const int16_t *r,unsigned n){
 const int16_t *c=amp_hb;
 return amp_m16(c[0],r[(n-4)&7])+amp_m16(c[0],r[(n-3)&7])+amp_m16(c[1],r[(n-5)&7])+amp_m16(c[1],r[(n-2)&7])
  +amp_m16(c[2],r[(n-6)&7])+amp_m16(c[2],r[(n-1)&7])+amp_m16(c[3],r[(n-7)&7])+amp_m16(c[3],r[n&7]);
}
static inline void amp_dsp_process(amp_dsp *s,const int16_t *in,int16_t *out,const int16_t *p){
 int32_t vol=2*(p[0]<0?0:p[0]),amt=p[1]<0?0:p[1],tilt=p[2]-16384,drive=p[3]<0?0:p[3];
 int32_t threshold=28000-((amt*24000)>>15),target=32768;
 if(amt&&s->env>threshold){int32_t desired=threshold+((s->env-threshold)>>2);target=(desired<<15)/s->env;}
 int32_t pole=s->rate==44100?32724:32680,lp=s->rate==44100?3353:6083,r=s->rate==44100;
 const amp_voice *v=(r?amp_voices_44100:amp_voices_22050)+amp_voice_index(p[5]);
 /* Gate only with Drive: opens about -60dBFS at low drive, -42dBFS at full, closes 6dB lower after a 25ms hold. */
 int32_t thr=33+((drive*227)>>15),hold=r?1102:551;
 /* Sag: loud playing lowers the drive gain by up to the voice's amount (follower at -6dBFS or above) and it
  * recovers with the follower's release. Set per block, smoothed. */
 int32_t e=s->env>16384?16384:s->env,sag=32768-((v->sag*e)>>14);
 s->sag+=(sag-s->sag)>>2;
 int32_t bias=v->bias,amp_on=drive||s->drive;
 for(int i=0;i<64;i++){
  s->tilt+=(tilt-s->tilt)>>6;s->gain+=(target-s->gain)>>4;
  int32_t x=in[i],dc=x-s->dc_x+((s->dc_y*pole+16384)>>15);s->dc_x=x;s->dc_y=amp_clip(dc);x=s->dc_y;
  int32_t a=amp_abs(x);s->env+=(a-s->env)>>(a>s->env?4+(s->rate==44100):11+(s->rate==44100));
  if(drive){if(s->env>thr){s->open=1;s->hold=hold;}else if(s->env<(thr>>1)){if(s->hold)s->hold--;else s->open=0;}}else s->open=1;
  if(s->open)s->gate+=(32768-s->gate+(7<<r)+r)>>(3+r);else s->gate+=(0-s->gate)>>(9+r);
  x=(x*s->gate)>>15;
  x=(x*s->gain)>>15;s->low+=((x-s->low)*lp)>>15;
  /* complementary low/high tilt, centre unity, bounded intermediate */
  int32_t gl=32768-s->tilt,gh=32768+s->tilt;
  s->blk[i]=amp_mul(s->low,gl)+amp_mul(x-s->low,gh);
 }
 /* Second pass over the block: fewer live values per loop keeps the audio-thread stack frame small. */
 for(int i=0;i<64;i++){
  s->volume+=(vol-s->volume)>>6;s->drive+=(drive-s->drive)>>6;
  int32_t x=s->blk[i];
  unsigned n=s->pos=(s->pos+1)&7;
  int16_t *h=s->xh;h[n]=(int16_t)amp_clip(x);s->dh[n]=x;
  int32_t dry=s->dh[(n+1)&7];   /* x[n-7], aligned with the 2x stage's output */
  if(amp_on){
   /* 2x: even sample x[n-4], odd sample the half-band midpoint of x[n-4] and x[n-3]; shape both, then decimate
    * around even x[n-7]. Bias makes the curve asymmetric (even harmonics); the offset it leaves is removed by the
    * cascade's zero at DC. */
   int32_t odd=(int32_t)amp_hb_pairs(h,n)>>14;
   int32_t g=256+(((v->gain-256)*s->drive)>>15);g=(g*s->sag)>>15;
   s->we[n&3]=(int16_t)amp_clip(amp_soft(((h[(n-4)&7]*g)>>8)+bias));
   s->wo[n]=(int16_t)amp_clip(amp_soft(((odd*g)>>8)+bias));
   int32_t wet=(int32_t)((uint32_t)s->we[(n-3)&3]*16384u+amp_hb_pairs(s->wo,n))>>15;
   /* Drive blends clean into shaped; the voice (stack and cabinet) fades in over the first quarter of Drive. */
   int32_t driven=amp_mul(dry,32768-s->drive)+amp_mul(wet,s->drive);
   /* The cascade runs 12dB down (headroom for the square edges of a hard-driven signal); its Q12 gain is applied x4. */
   int32_t y=amp_clip((amp_cascade(s,v,amp_clip(driven)>>2)*v->out)>>10);
   int32_t cm=s->drive<8192?4*s->drive:32768;
   x=dry+amp_mul(y-dry,cm);
  }else x=dry;
  x=amp_mul(x,s->volume);
  if(s->fade<64)s->fade++;
  out[i]=(int16_t)amp_clip((x*s->fade)>>6);
 }
}
#endif
