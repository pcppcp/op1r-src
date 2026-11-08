/* SID primitives adapted from cRSID 1.58, Hermit (Mihaly Horvath).
 * License is still WTF: Do what the frick you want with this code, but
 * it would be nice mentioning me as the original author.
 * Local changes: six independent plugin instances; native OP-1 envelope;
 * block-cached edge coefficients; selected pure waveforms; bounded 8580 LPF.
 * No CPU/player/register bus/digi emulation. */
#include "sid_dsp.h"
#include "sid_filter_tables.h"
#define MASK 0x0fffffffu
#define MSB 0x08000000u
static int clamp(int x,int lo,int hi){return x<lo?lo:x>hi?hi:x;}
/* x / 288 for x <= 0x600000 (step >> 4): (x >> 5) / 9 from a low estimate and its remainder (r < 36; checked for every
 * step 0 .. 0x06000000 against the division). GCC calls the library division for it on the Blackfin */
static inline unsigned div288(uint32_t x){uint32_t y=x>>5,q=(y*7281u)>>16,r=y-q*9;return q+(r>=9)+(r>=18)+(r>=27);}
#ifdef __bfin__
uint32_t sid_udiv(uint32_t n,uint32_t d,unsigned b);   /* sid_kernels.S: n / d when it is below 2^b */
#define UDIV(n,d,b) sid_udiv(n,d,b)
#else
#define UDIV(n,d,b) ((n)/(d))
#endif
/* Smooth in extended precision (Q26). R246 renders a voice at 88.2 kHz, so the 1/512 pole has the ~5.8 ms
 * time constant the 1/256 pole of 44.1 kHz had. Split products keep both filter multiplies in signed 32 bits:
 * |high| <= 170496 and |band| <= 65536 under the existing state bounds. */
static inline int cutoff_target(int q) {
 unsigned u=(unsigned)clamp(q,0,32767),i=u>>9,f=u&511;
 if(u==32767)return sid_cutoff_log[64];
 return sid_cutoff_log[i]+((sid_cutoff_log[i+1]-sid_cutoff_log[i])*(int)f>>9);
}
static inline int cutoff_smooth(int live,int target) { return live+((target-live+256)>>9); }
static inline int cutoff_mul(int x,int c) {
 return (x*(c>>12)+((x*(c&4095))>>12))>>14;
}

void sid_reset(sid_state *s) {
 volatile unsigned char *bytes=(volatile unsigned char *)s;
 for(unsigned i=0;i<sizeof(*s);i++)bytes[i]=0;
 for(int j=0;j<3;j++)s->noise[j]=0x7fffff;
}
void sid_configure(sid_state *s,uint32_t inc,const int16_t *q) {
 unsigned same=s->configured && s->cached_inc==inc;
 for(int j=0;j<4;j++)if(s->cached_q[j]!=q[j])same=0;
 if(same)return;
 unsigned first=!s->configured;
 s->configured=1;s->cached_inc=inc;for(int j=0;j<4;j++)s->cached_q[j]=q[j];
 s->mode=sid_wave_mode(q[0]);
 s->secondary=q[1]>>7;
 s->width=2048+((int)q[1]*61440>>15); /* 3..97% duty, before high-pitch protection */
 s->cutoff=cutoff_target(q[2]);
 if(first)s->cutoff_live=s->cutoff; /* a fresh note starts at the chosen tone */
 s->resonance=cRSID_Resonances8580[(unsigned)q[3]>>11];
 /* Pure modes use root, octave and fifth: a SID mini-chip, not unison padding.
  * Sync/ring modes set the source interval in semitones, 0..24 (1x..4x), keeping a root carrier
  * (2026-11-07, owner: the free 1.00..4.00 sweep was too fine to set; every stop is now in tune). */
 uint32_t root=inc>>4;if(root>0x06000000u)root=0x06000000u;
 s->step[0]=root;
 if(s->mode>=4) {
   static const uint16_t semi[25]={256,271,287,304,323,342,362,384,406,431,456,483,512,542,575,609,645,683,724,767,813,861,912,967,1024}; /* 256 * 2^(n/12) */
   unsigned ratio=semi[sid_interval_semi(q[1])];
   s->step[1]=(root>>8)*ratio;s->step[2]=root;
 } else {s->step[1]=root*2;s->step[2]=root+(root>>1);}
 for(int j=0;j<3;j++) {
  uint32_t step=s->step[j];if(step>0x06000000u)step=0x06000000u;s->step[j]=step;
  /* only the current wave's divisions (a mode change is a q[0] change, so it always comes back here first) */
  if(s->mode==2)s->pulse_steep[j]=step>=4096?UDIV(MASK,step,16):65535;          /* < 2^16 from step >= 4096 */
  if(s->mode==1||s->mode==4){
   unsigned steep=div288(step>>4);if(!steep)steep=1;s->saw_steep[j]=steep;
   s->saw_recip[j]=UDIV(1u<<20,steep,steep>=32?16:21);                         /* <= 2^15 from 32 on, else <= 2^20 */
  }
 }
}
int16_t sid_sample(sid_state *s) {
 int sum=0;
 for(int j=0;j<3;j++) {
  uint32_t step=s->step[j],old=s->phase[j],phase;
  if(s->mode==4 && j!=1 && s->sync_rise)phase=0;
  else phase=(old+step)&MASK;
  s->phase[j]=phase;
  unsigned msb=phase&MSB;s->sync_rise=msb>(s->previous[j]&MSB);
  uint32_t wave=0;int mode=s->mode;
  if(mode==3) {
   uint32_t n=s->noise[j];
   if(((phase^s->previous[j])&0x01000000u)||step>=0x01000000u) {
    unsigned feedback=((n&0x400000u)^((n&0x20000u)<<5))!=0;
    n=((n<<1)|feedback)&0x7fffff;s->noise[j]=n;
   }
   wave=((n&0x100000)>>5)|((n&0x40000)>>4)|((n&0x4000)>>1)|((n&0x800)<<1)
       |((n&0x200)<<2)|((n&0x20)<<5)|((n&4)<<7)|((n&1)<<8);
  } else if(mode==2) {
   uint32_t pw=(unsigned)s->width,limit=step>>13;
   if(pw && pw<limit)pw=limit;if(pw>(limit^65535u))pw=limit^65535u;
   uint32_t t=phase>>12,steep=s->pulse_steep[j];int v;
   if(t<pw){int peak=(65535-pw)*steep;if(peak>65535)peak=65535;v=peak-(pw-t)*steep;if(v<0)v=0;}
   else {int peak=pw*steep;if(peak>65535)peak=65535;v=(65535-t)*steep-peak;if(v>=0)v=65535;}
   wave=(unsigned)v&65535;
  } else if(mode==1 || mode==4) {
   wave=phase>>12;wave+=(wave*s->saw_steep[j])>>16;
   if(wave>65535)wave=65535-(((wave-65536)*s->saw_recip[j])>>4);
   wave&=65535;
  } else {
   uint32_t t=phase^(mode==5?s->ring_msb:0);
   wave=((t^((t&MSB)?MASK:0))>>11)&65535;
  }
  s->previous[j]=phase;s->ring_msb=msb;
  int sample=((int)wave-32768)/8;
  if(j && (mode==0 || mode==1 || mode==3))sample=(sample*s->secondary)/256;
  sum+=sample;
 }
 /* cRSID 8580 state-variable lowpass; bounded states prevent numeric runaway
  * under extreme live cutoff/resonance changes. Extended-precision cutoff. */
 int high=sum+(s->band*s->resonance)/4096+s->low;
 s->cutoff_live=cutoff_smooth(s->cutoff_live,s->cutoff);
 s->band=clamp(s->band-cutoff_mul(high,s->cutoff_live),-65536,65536);
 s->low=clamp(s->low+cutoff_mul(s->band,s->cutoff_live),-65536,65536);
 /* Board output coupling: remove pulse/sync DC before native ADSR. */
 int output=s->low;s->dc+=(output*512-s->dc)/2048;
 return (int16_t)clamp(output-s->dc/512,-32768,32767);
}

/* Full-rate block path: constant waveform per loop, scalar local oscillator state.
 * sid_sample above remains a numerical reference for the host equivalence check. */
#include "sid_noise_tables.h"
typedef struct {uint32_t phase,previous,noise,step,pulse,saw,recip;} osc;
#define INLINE static inline __attribute__((always_inline))
/* x / 2^k as C rounds it (toward zero), without the branch gcc emits for it on the Blackfin */
#define SDIV(x,k) ((int)((x)+(int)((unsigned)((int)(x)>>31)>>(32-(k))))>>(k))
INLINE int oscillator(osc *o,unsigned mode,unsigned sync_enabled,unsigned *rise,uint32_t *ring) {
 uint32_t p=sync_enabled && *rise?0:(o->phase+o->step)&MASK;o->phase=p;
 uint32_t msb=p&MSB;*rise=msb>(o->previous&MSB);uint32_t w;
 if(mode==3){
  uint32_t n=o->noise;
  if(((p^o->previous)&0x1000000u)||o->step>=0x1000000u){unsigned b=((n&0x400000u)^((n&0x20000u)<<5))!=0;n=((n<<1)|b)&0x7fffff;o->noise=n;}
  w=noise_low[n&1023]|noise_high[(n>>10)&2047];
  o->previous=p;*ring=msb;return ((int)w-32768)>>3; /* noise sets bits 8..15 only: the division is exact */
 }else if(mode==2){
  /* Width is passed through ring's high-independent companion below. */
  w=0;
 }else if(mode==1||mode==4){
  w=p>>12;w+=(w*o->saw)>>16;
  if(w>65535)w=65535-(((w-65536)*o->recip)>>4);w&=65535;
 }else {uint32_t t=p^(mode==5?*ring:0);w=((t^((t&MSB)?MASK:0))>>11)&65535;}
 o->previous=p;*ring=msb;return SDIV((int)w-32768,3);
}
INLINE int pulse_oscillator(osc *o,unsigned width,unsigned *rise,uint32_t *ring){
 uint32_t p=(o->phase+o->step)&MASK;o->phase=p;uint32_t msb=p&MSB;*rise=msb>(o->previous&MSB);
 uint32_t pw=width,limit=o->step>>13;if(pw&&pw<limit)pw=limit;if(pw>(limit^65535u))pw=limit^65535u;
 uint32_t t=p>>12;int v;
 if(t<pw){int peak=(65535-pw)*o->pulse;if(peak>65535)peak=65535;v=peak-(pw-t)*o->pulse;if(v<0)v=0;}
 else {int peak=pw*o->pulse;if(peak>65535)peak=65535;v=(65535-t)*o->pulse-peak;if(v>=0)v=65535;}
 o->previous=p;*ring=msb;return SDIV((int)((unsigned)v&65535)-32768,3);
}
#define LOAD(j) (osc){s->phase[j],s->previous[j],s->noise[j],s->step[j],s->pulse_steep[j],s->saw_steep[j],s->saw_recip[j]}
#define SAVE(o,j) s->phase[j]=o.phase;s->previous[j]=o.previous;s->noise[j]=o.noise
/* C: the cutoff has reached its target (the smoothing is then the identity for the whole call, so the filter
 * coefficient and its two halves stay loop constants); a second copy of each loop smooths per sample */
#define DEFINE_RENDER(M,C) \
static void render_##M##_##C(sid_state *s,int16_t *out,unsigned count){ \
 osc a=LOAD(0),b=LOAD(1),c=LOAD(2); \
 unsigned rise=s->sync_rise;uint32_t ring=s->ring_msb; \
 int band=s->band,low=s->low,dc=s->dc,cutoff=s->cutoff_live,target=s->cutoff,res=s->resonance,gain=s->secondary;unsigned width=s->width; \
 for(unsigned i=0;i<count;i++){ \
  int x,y,z; \
  if(M==2){x=pulse_oscillator(&a,width,&rise,&ring);y=pulse_oscillator(&b,width,&rise,&ring);z=pulse_oscillator(&c,width,&rise,&ring);} \
  else{x=oscillator(&a,M,M==4,&rise,&ring);y=oscillator(&b,M,0,&rise,&ring);z=oscillator(&c,M,M==4,&rise,&ring);} \
  if(M==0||M==1||M==3){y=SDIV(y*gain,8);z=SDIV(z*gain,8);} \
  if(!C)cutoff=cutoff_smooth(cutoff,target); \
  int high=x+y+z+SDIV(band*res,12)+low;band=clamp(band-cutoff_mul(high,cutoff),-65536,65536);low=clamp(low+cutoff_mul(band,cutoff),-65536,65536); \
  dc+=SDIV(low*512-dc,11);out[i]=(int16_t)clamp(low-SDIV(dc,9),-32768,32767); \
 } \
 SAVE(a,0);SAVE(b,1);SAVE(c,2);s->sync_rise=rise;s->ring_msb=ring;s->band=band;s->low=low;s->dc=dc;s->cutoff_live=cutoff; \
}
DEFINE_RENDER(0,0) DEFINE_RENDER(1,0) DEFINE_RENDER(2,0) DEFINE_RENDER(3,0) DEFINE_RENDER(4,0) DEFINE_RENDER(5,0)
DEFINE_RENDER(0,1) DEFINE_RENDER(1,1) DEFINE_RENDER(2,1) DEFINE_RENDER(3,1) DEFINE_RENDER(4,1) DEFINE_RENDER(5,1)
#ifdef __bfin__
/* Blackfin: sid_kernels.S's passes, one oscillator over the call and then the filter, the same samples as the loops above
 * (which stay the host's path and the reference, and the fallback for a negative gain or a call over 32 samples).
 * They read the previous phase as the phase (render keeps previous == phase between calls) and the ring source's
 * MSB from the third oscillator's phase (ring_msb is always that MSB). */
typedef struct {uint32_t phase,step;int32_t gain;uint32_t mask,k1,k2,k3,k4,scratch[8];} sid_pass;
typedef struct {int32_t band,low,dc,cutoff,target,res,glide,pad,scratch[10];} sid_filt;
void sid_tri(int32_t *acc,sid_pass *p,unsigned n);
void sid_trig(int32_t *acc,sid_pass *p,unsigned n);
void sid_ring(int32_t *acc,sid_pass *p,unsigned n);
void sid_saw(int32_t *acc,sid_pass *p,unsigned n);
void sid_sawg(int32_t *acc,sid_pass *p,unsigned n);
uint32_t sid_sync(int32_t *acc,sid_pass *p,unsigned n);
void sid_pulse(int32_t *acc,sid_pass *p,unsigned n);
void sid_noise(int32_t *acc,sid_pass *p,unsigned n);
void sid_noiseg(int32_t *acc,sid_pass *p,unsigned n);
void sid_filter(int16_t *out,int32_t *acc,sid_filt *f,unsigned n);
static int32_t sid_acc[33];       /* [0]: the word in front of the block (the passes' lagging store writes it back) */
static sid_pass sid_p[3];
static sid_filt sid_f;
static int noise_sample(uint32_t n){return ((int)(noise_low[n&1023]|noise_high[(n>>10)&2047])-32768)>>3;}
/* L1 pool (sid.c): the noise tables the kernels read at the LFSR's random index, in the synth partition (noise_low at 0,
 * noise_high at 2,048; 6,144 B) or in SDRAM (the file's copies). Same values either way. l1 is the partition or 0. */
static const uint16_t *noise_lo_at=noise_low,*noise_hi_at=noise_high;
void sid_noise_l1(uint16_t *l1,int fresh){
 if(!l1){noise_lo_at=noise_low;noise_hi_at=noise_high;return;}
 if(fresh){
  for(unsigned i=0;i<1024;i++)l1[i]=noise_low[i];
  for(unsigned i=0;i<2048;i++)l1[1024+i]=noise_high[i];
 }
 noise_lo_at=l1;noise_hi_at=l1+1024;
}
static void render_blocks(sid_state *s,int16_t *out,unsigned n){
 int32_t *acc=sid_acc+1;unsigned mode=s->mode,shift=32-n;int gain=s->secondary;
 for(int j=0;j<3;j++){sid_p[j].phase=s->phase[j];sid_p[j].step=s->step[j];sid_p[j].gain=gain;}
 switch(mode){
 case 0:sid_tri(acc,&sid_p[0],n);sid_trig(acc,&sid_p[1],n);sid_trig(acc,&sid_p[2],n);break;
 case 1:
  for(int j=0;j<3;j++){sid_p[j].k1=s->saw_steep[j];sid_p[j].k2=s->saw_recip[j];}
  sid_saw(acc,&sid_p[0],n);sid_sawg(acc,&sid_p[1],n);sid_sawg(acc,&sid_p[2],n);break;
 case 2:
  for(int j=0;j<3;j++){
   uint32_t pw=(unsigned)s->width,limit=s->step[j]>>13,steep=s->pulse_steep[j];
   if(pw&&pw<limit)pw=limit;
   if(pw>(limit^65535u))pw=limit^65535u;
   int a=(65535-pw)*steep,b=pw*steep;if(a>65535)a=65535;if(b>65535)b=65535;
   sid_p[j].k1=steep;sid_p[j].k2=pw;sid_p[j].k3=(uint32_t)a;sid_p[j].k4=(uint32_t)b;
   sid_pulse(acc,&sid_p[j],n);
  }
  break;
 case 3:
  for(int j=0;j<3;j++){
   int v=noise_sample(s->noise[j]);if(j)v=SDIV(v*gain,8);
   sid_p[j].k1=s->noise[j];sid_p[j].k2=(uint32_t)v;sid_p[j].k3=s->step[j]>=0x1000000u;
   sid_p[j].k4=(uint32_t)noise_lo_at;sid_p[j].scratch[0]=(uint32_t)noise_hi_at;
  }
  sid_noise(acc,&sid_p[0],n);sid_noiseg(acc,&sid_p[1],n);sid_noiseg(acc,&sid_p[2],n);
  for(int j=0;j<3;j++)s->noise[j]=sid_p[j].k1;
  break;
 case 4:{
  for(int j=0;j<3;j++){sid_p[j].k1=s->saw_steep[j];sid_p[j].k2=s->saw_recip[j];}
  sid_p[1].mask=0;uint32_t rb=sid_sync(acc,&sid_p[1],n)<<shift;            /* the octave runs free */
  sid_p[2].mask=rb;uint32_t rc=sid_sync(acc,&sid_p[2],n)<<shift;            /* restarted by its rises */
  sid_p[0].mask=(s->sync_rise?MSB<<4:0)|rc>>1;sid_sync(acc,&sid_p[0],n);    /* by the third's, a sample later */
  s->sync_rise=(rc>>shift)&1;break;}
 default:
  sid_p[0].mask=s->phase[2]<<4;sid_p[0].k1=s->step[2]<<4;                   /* the third's MSB a sample before */
  sid_p[1].mask=(s->phase[0]+s->step[0])<<4;sid_p[1].k1=s->step[0]<<4;      /* the first's, the same sample */
  sid_p[2].mask=(s->phase[1]+s->step[1])<<4;sid_p[2].k1=s->step[1]<<4;
  sid_ring(acc,&sid_p[0],n);sid_ring(acc,&sid_p[1],n);sid_ring(acc,&sid_p[2],n);break;
 }
 for(int j=0;j<3;j++)s->phase[j]=s->previous[j]=sid_p[j].phase;
 uint32_t c=s->phase[2];s->ring_msb=c&MSB;
 if(mode!=4)s->sync_rise=(c&MSB)>(((c-s->step[2])&MASK)&MSB);
 sid_f.band=s->band;sid_f.low=s->low;sid_f.dc=s->dc;sid_f.cutoff=s->cutoff_live;sid_f.target=s->cutoff;
 sid_f.res=s->resonance;sid_f.glide=((s->cutoff-s->cutoff_live+256)>>9)!=0;
 sid_filter(out,acc,&sid_f,n);
 s->band=sid_f.band;s->low=sid_f.low;s->dc=sid_f.dc;s->cutoff_live=sid_f.cutoff;
}
#endif
void sid_render_samples(sid_state *s,int16_t *out,unsigned count){
#ifdef __bfin__
 if(count && count<=32 && (s->secondary>=0 || s->mode==2 || s->mode>=4)){render_blocks(s,out,count);return;}
#endif
 if(s->cutoff_live==s->cutoff)
  switch(s->mode){case 0:render_0_1(s,out,count);break;case 1:render_1_1(s,out,count);break;case 2:render_2_1(s,out,count);break;case 3:render_3_1(s,out,count);break;case 4:render_4_1(s,out,count);break;default:render_5_1(s,out,count);break;}
 else
  switch(s->mode){case 0:render_0_0(s,out,count);break;case 1:render_1_0(s,out,count);break;case 2:render_2_0(s,out,count);break;case 3:render_3_0(s,out,count);break;case 4:render_4_0(s,out,count);break;default:render_5_0(s,out,count);break;}
}
