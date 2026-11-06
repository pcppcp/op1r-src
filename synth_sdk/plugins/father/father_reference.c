/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission. */
/* Father's plain C reference: every ring index wrapped per sample, no fast paths, no kernels. father_dsp.c (C fast
   paths, and father_kern.S on the Blackfin) must match it bit for bit (father_test.c, kern_check.py). The design is
   docs/father-redesign.md candidate A: an eight-line Hadamard FDN at 11,025 Hz with full-rate predelay and early
   reflections. */
#include "father_dsp.h"
#include "father_tables.h"
#define ROW(i,n) s->rows[(i)>>2][n][(i)&3]
static int32_t sat(int32_t v){return v>32767?32767:v< -32768?-32768:v;}
/* Round toward zero: silence cannot sustain a negative one-LSB limit cycle. */
static int32_t mul(int32_t a,int32_t b){int32_t v=a*b;return (v+((v>>31)&32767))>>15;}
static int32_t half(int32_t v){return (v+(int32_t)((uint32_t)v>>31))>>1;}
static unsigned wrap(unsigned i,unsigned len){while(i>=len)i-=len;return i;}
void mw_ref_init(mw_state *s){
 uint32_t *p=(uint32_t *)(void *)s;for(unsigned i=0;i<sizeof *s/4;i++)p[i]=0;
 s->mix=-1;s->pre_samples=-1;s->gate_gain=32767;
 for(unsigned i=0;i<8;i++)s->phase[i]=0x9E3779B9u*(i+1u);
}
/* 2:1 half-band decimator (-1 0 9 16 9 0 -1)/32, two samples late */
static void decimate(int32_t h[5],const int16_t *ring,unsigned at,int16_t *y){
 for(unsigned k=0;k<MW_N;k++){
  int32_t a=ring[wrap(at+2*k,MW_PRE_LEN)],b=ring[wrap(at+2*k+1,MW_PRE_LEN)];
  y[k]=(int16_t)sat((9*(h[2]+h[4])+16*h[3]-h[0]-b)>>5);
  h[0]=h[2];h[1]=h[3];h[2]=h[4];h[3]=a;h[4]=b;
 }
}
/* x2 interpolator (-1 9 9 -1)/16, two samples late */
static void interpolate(int32_t h[3],int32_t x,int32_t *o0,int32_t *o1){
 *o0=h[1];*o1=(9*(h[1]+h[2])-(h[0]+x))>>4;h[0]=h[1];h[1]=h[2];h[2]=x;
}
void mw_ref_process(mw_state *s,int16_t *l,int16_t *r,const int32_t q[4]){
 const int stereo=r!=0;if(!r)r=l;
 unsigned ix=((unsigned)q[0]*128+16383)>>15;if(ix>128)ix=128;
 const int32_t damping=1800+mul(q[2],20000),target_pre=mul(q[0],MW_PRE_MAX);
 const int32_t keep=(int32_t)(((uint32_t)(32768-damping)*(uint32_t)(32768-damping))>>15);   /* the pole at half rate */
 const int gated=q[1]!=0;const int32_t free=32767-q[1];
 const uint32_t hold_samples=1764u+(uint32_t)mul(mul(free,free),37926);
 const int32_t close_rate=64+mul(q[1],192);
 const int32_t target_mix=q[3];if(s->mix<0)s->mix=target_mix;
 if(s->pre_samples<0)s->pre_samples=target_pre;
 if(s->pre_samples<target_pre)s->pre_samples++;else if(s->pre_samples>target_pre)s->pre_samples--;
 int32_t mix=s->mix*64;const int32_t step=target_mix-s->mix;
 /* input into the predelay ring(s), peak for the gate detector */
 const unsigned ph=s->pre_hd;int32_t input_peak=0;
 for(unsigned c=0;c<1u+(unsigned)stereo;c++){
  const int16_t *x=c?r:l;
  for(unsigned n=0;n<64;n++){int32_t v=x[n];s->pre[c][wrap(ph+n,MW_PRE_LEN)]=(int16_t)v;v=v<0?-v:v;if(v>input_peak)input_peak=v;}
 }
 const int16_t *pr=s->pre[stereo];
 /* gate detector, once per callback */
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
 const int32_t gate_target=(!gated||!s->gate_armed||s->gate_hold)?32767:0;
 const int apply_gate=gate_target!=32767||s->gate_gain!=32767;
 /* front: predelay tap decimated to the tank rate, two allpasses per channel */
 const unsigned at=wrap(ph+2*MW_PRE_LEN-(unsigned)s->pre_samples,MW_PRE_LEN);
 decimate(s->dec[0],s->pre[0],at,s->in[0]);
 if(stereo)decimate(s->dec[1],pr,at,s->in[1]);
 else for(unsigned k=0;k<MW_N;k++)s->in[1][k]=s->in[0][k];
 for(unsigned a=0;a<4;a++){
  int16_t *ring=s->ap+mw_ap_off[a],*x=s->in[a>>1];unsigned h=s->ap_hd[a];
  for(unsigned k=0;k<MW_N;k++){
   int32_t d=ring[h],v=sat(x[k]-half(d));ring[h]=(int16_t)v;x[k]=(int16_t)sat(d+half(v));
   if(++h==mw_ap_len[a])h=0;
  }
  s->ap_hd[a]=(uint16_t)h;
 }
 /* tank reads: each line's delay is held for the callback (the slow triangle moves it less than 1/100 sample per
    callback), linear interpolation, then the damping one-pole */
 for(unsigned i=0;i<8;i++){
  const int16_t *ring=s->tank+mw_off[i];const unsigned len=mw_len[i],h=s->hd[i];
  const uint32_t p=s->phase[i];s->phase[i]=p+mw_speed[i]*MW_N;
  const int32_t tri=(int32_t)(((p>>15)^(0u-(p>>31)))&65535)-32768;
  const int32_t dq=(int32_t)mw_delay[i]*256+mul(tri,mw_depth[i]),fr=dq&255;
  int32_t damp=s->damp[i];
  for(unsigned n=0;n<MW_N;n++){
   const unsigned k=wrap(h+n+len-((unsigned)dq>>8),len),j=k?k-1:len-1;
   int32_t v=ring[k];v+=((ring[j]-v)*fr)>>8;
   damp=v+mul(damp-v,keep);ROW(i,n)=(int16_t)damp;
  }
  s->damp[i]=damp;
 }
 /* early reflections, full rate, from the predelay ring(s): the right channel only for a stereo call */
 for(unsigned c=0;c<1u+(unsigned)stereo;c++){
  const int16_t *a=c?pr:s->pre[0],*b=c?s->pre[0]:pr;const uint16_t *t=mw_early[c];int32_t e=s->early[c];
  for(unsigned n=0;n<64;n++){
   const unsigned base=at+n+MW_PRE_LEN;
   int32_t x=(a[wrap(base-t[0],MW_PRE_LEN)]>>2)+(b[wrap(base-t[1],MW_PRE_LEN)]>>3)
            -(a[wrap(base-t[2],MW_PRE_LEN)]>>4)+(b[wrap(base-t[3],MW_PRE_LEN)]>>4);
   e+=mul(x-e,damping);s->erow[c][n]=(int16_t)e;
  }
  s->early[c]=e;
 }
 /* output: late sum at the tank rate, interpolated, plus early, gate, dry/wet */
 int32_t gate_gain=s->gate_gain,peak=0;
 for(unsigned k=0;k<MW_N;k++){
  const int32_t tl=ROW(0,k)+ROW(2,k)-ROW(4,k)-ROW(6,k);
  const int32_t tr=ROW(1,k)+ROW(3,k)-ROW(5,k)-ROW(7,k);
  int32_t ol[2],orr[2]={0,0};
  interpolate(s->up[0],half(tl+half(tl)),&ol[0],&ol[1]);
  if(stereo)interpolate(s->up[1],half(tr+half(tr)),&orr[0],&orr[1]);
  for(unsigned m=0;m<2;m++){
   const unsigned o=2*k+m;
   if(apply_gate){
    if(gate_target>gate_gain)gate_gain+=(32767-gate_gain+31)>>5;
    else if(gate_target<gate_gain)gate_gain-=(gate_gain*close_rate+32767)>>15;
   }
   mix+=step;const int32_t wet=mix>>6;
   int32_t xl=sat(ol[m]+s->erow[0][o]);if(apply_gate)xl=mul(xl,gate_gain);
   const int32_t a=l[o],out_l=sat(a+mul(xl-a,wet));l[o]=(int16_t)out_l;
   int32_t pk=out_l<0?-out_l:out_l;if(pk>peak)peak=pk;
   if(stereo){
    int32_t xr=sat(orr[m]+s->erow[1][o]);if(apply_gate)xr=mul(xr,gate_gain);
    const int32_t b=r[o],out_r=sat(b+mul(xr-b,wet));r[o]=(int16_t)out_r;
    pk=out_r<0?-out_r:out_r;if(pk>peak)peak=pk;
   }
  }
 }
 s->gate_gain=gate_gain;s->peak=peak;
 /* tank write-back: two interleaved four-way Hadamard groups (lines 0..3 feed 0,2,4,6; lines 4..7 feed 1,3,5,7),
    decay per destination line, the left / right input with alternating sign */
 const int16_t *k=mw_decay[ix];
 for(unsigned n=0;n<MW_N;n++){
  for(unsigned g=0;g<2;g++){
   const int32_t d0=ROW(g*4,n),d1=ROW(g*4+1,n),d2=ROW(g*4+2,n),d3=ROW(g*4+3,n);
   const int32_t a0=d0+d1,a1=d0-d1,a2=d2+d3,a3=d2-d3;
   const int32_t in=s->in[g][n]>>2,ni=(-s->in[g][n])>>2;
   const int32_t v[4]={sat(mul(sat(half(a0+a2)),k[g])+in),sat(mul(sat(half(a1+a3)),k[g+2])+ni),
                       sat(mul(sat(half(a0-a2)),k[g+4])+in),sat(mul(sat(half(a1-a3)),k[g+6])+ni)};
   for(unsigned j=0;j<4;j++){const unsigned L=g+2*j;s->tank[mw_off[L]+wrap(s->hd[L]+n,mw_len[L])]=(int16_t)v[j];}
  }
 }
 for(unsigned i=0;i<8;i++)s->hd[i]=(uint16_t)wrap(s->hd[i]+MW_N,mw_len[i]);
 s->pre_hd=(uint16_t)wrap(ph+64,MW_PRE_LEN);
 s->mix=target_mix;
}
