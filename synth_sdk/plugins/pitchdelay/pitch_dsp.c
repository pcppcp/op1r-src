/* SPDX-License-Identifier: GPL-3.0-or-later
 * ESC_CODE / Volker Junginger's Delay Trio (github.com/VolkerJunginger/OP-1, Delay-Trio, e88427c), unchanged.
 * Fixed-point adaptation of Tom Erbe / SoundHack +pitchdelay~.
 * Two crossfaded Hermite heads, moving ramps, DC block and atan feedback.
 */
#include "pitch_dsp.h"
#include "tables.h"
/* Tight six-second history plus interpolation guard, without power-of-two
 * rounding. Inputs to wrap are strictly inside (-SAMPLES,2*SAMPLES). */
static unsigned wrap(int i)
{if(i<0)i+=(int)PD_SAMPLES;else if(i>=(int)PD_SAMPLES)i-=(int)PD_SAMPLES;return (unsigned)i;}
static int clamp(int x,int a,int b){return x<a?a:x>b?b:x;}
static int q15(int a,int b){return (a*b)>>15;}
static int product(int a,int b){return (a>>15)*b+(((a&32767)*b)>>15);}
int pd_pitch_index(int value){return clamp((clamp(value,0,PD_PITCH_MAX)+PD_PITCH_STEP/2)/PD_PITCH_STEP,0,34);}
int pd_time(int p){return PD_TIME[clamp(p,0,32767)>>7];}
int pd_cents(int i){return PD_CENTS[clamp(i,0,34)];}
const char *pd_pitch_label(int i){return PD_LABELS[clamp(i,0,34)];}
int pd_rate(int i,int octave){return PD_RATE[(clamp(octave,-3,3)+3)*35+clamp(i,0,34)];}
void pd_init(struct pd_state *s)
{
 unsigned char *p=(unsigned char *)s;
 for(unsigned i=0;i<sizeof *s;++i)p[i]=0;
 s->phase=0x80000000u;
}
static int clip(int x)
{
 int negative=x<0;if(negative)x=-x;x=clamp(x,0,131071);
 unsigned i=(unsigned)x>>9,f=(unsigned)x&511;
 int out=PD_CLIP[i]+((short)(PD_CLIP[i+1]-PD_CLIP[i])*(short)f>>9);
 return negative?-out:out;
}
static int history(const short *r,unsigned i,unsigned write,unsigned filled)
{unsigned age=wrap((int)write-(int)i);return age && age<=filled?r[i]:0;}
static inline int tap(const short *r,unsigned write,unsigned lag,unsigned filled)
{
 int position=(int)(write<<12)-(int)lag;
 if(position<0)position+=(int)(PD_SAMPLES<<12);
 unsigned pos=(unsigned)position,i=pos>>12,f=(pos>>4)&255u;
 if(!f)return history(r,i,write,filled);
 const short *w=PD_CUBIC+4*f;short a,b,c,d;
 if(filled>=(lag>>12)+3u){a=r[wrap((int)i-1)];b=r[i];c=r[wrap((int)i+1)];d=r[wrap((int)i+2)];}
 else {a=history(r,wrap((int)i-1),write,filled);b=history(r,i,write,filled);c=history(r,wrap((int)i+1),write,filled);d=history(r,wrap((int)i+2),write,filled);}
 return (a*w[0]+b*w[1]+c*w[2]+d*w[3])>>14;
}
/* Exact neutral-pitch path: settled controls, centered phase and valid
 * integer history. No interpolation, phase/window math or history checks. */
static void neutral(struct pd_state *s,short *ring,short *l,short *r,unsigned lag,int fb,int mix)
{
 unsigned write=s->write;int dc=s->dc_out,dc_in=s->dc_in;
 for(int i=0;i<64;++i) {
  int wet=ring[wrap((int)write-(int)lag)];
  int dry=l[i],dr=r?r[i]:dry;
  int input=r?(dry+dr)/4:dry/2;
  int pre=input+(((short)wet*(int)(unsigned short)fb)>>15);
  dc=clamp(pre-dc_in+dc-(dc*94)/32768,-262143,262143);dc_in=pre;
  ring[write]=(short)clip(dc);if(++write==PD_SAMPLES)write=0;
  wet=clamp(wet*2,-32768,32767);
  if(mix==16384){l[i]=(short)((dry+wet)>>1);if(r)r[i]=(short)((dr+wet)>>1);}
  else {l[i]=(short)(mix==32767?wet:clamp(dry+q15(wet-dry,mix),-32768,32767));if(r)r[i]=(short)(mix==32767?wet:clamp(dr+q15(wet-dr,mix),-32768,32767));}
 }
 s->write=write;s->filled=s->filled>PD_SAMPLES-64u?PD_SAMPLES:s->filled+64u;
 s->dc_in=dc_in;s->dc_out=dc;
}
/* Settled shifted pitch: valid history allows direct, inlined Hermite
 * reads and fixed controls, without per-sample history/ramp decisions. */
static inline __attribute__((always_inline)) int valid_tap(const short *ring,unsigned write,unsigned lag)
{
 int position=(int)(write<<12)-(int)lag;
 if(position<0)position+=(int)(PD_SAMPLES<<12);
 unsigned pos=(unsigned)position,i=pos>>12,f=(pos>>4)&255u;
 if(!f)return ring[i];
 const short *w=PD_CUBIC+4*f;
 short a=ring[wrap((int)i-1)],b=ring[i],c=ring[wrap((int)i+1)],d=ring[wrap((int)i+2)];
 return (a*w[0]+b*w[1]+c*w[2]+d*w[3])>>14;
}
static void shifted(struct pd_state *s,short *ring,short *l,int width,int fb,int mix)
{
 unsigned write=s->write,phase=s->phase,inc=s->increment;
 int time=s->time,dc=s->dc_out,dc_in=s->dc_in;
 for(int i=0;i<64;++i) {
  phase+=inc;
  int q=(int)(phase>>16)-32768,weight=q<0?-q:q,qb=q>=0?q-32768:q+32768;
  int la=clamp(time-product(width,q)+8192,16384,(PD_SAMPLES-4u)*4096u);
  int lb=clamp(time-product(width,qb)+8192,16384,(PD_SAMPLES-4u)*4096u);
  int a=valid_tap(ring,write,(unsigned)la),b=valid_tap(ring,write,(unsigned)lb);
  int wet=weight==32768?b:a+q15(b-a,weight);
  int dry=l[i],pre=dry/2+(((short)wet*(int)(unsigned short)fb)>>15);
  dc=clamp(pre-dc_in+dc-(dc*94)/32768,-262143,262143);dc_in=pre;
  ring[write]=(short)clip(dc);if(++write==PD_SAMPLES)write=0;
  wet=clamp(wet*2,-32768,32767);
  l[i]=(short)(mix==16384?(dry+wet)>>1:mix==32767?wet:clamp(dry+q15(wet-dry,mix),-32768,32767));
 }
 s->phase=phase;s->write=write;s->dc_in=dc_in;s->dc_out=dc;
 s->filled=s->filled>PD_SAMPLES-64u?PD_SAMPLES:s->filled+64u;
}
void pd_render(struct pd_state *s,short *ring,short *l,short *r,const short p[8],int octave,int depth)
{
 int target=pd_time(p[0])*4096,dt=clamp(depth,10,100)*16777216/100;
 int rate=pd_rate(pd_pitch_index(p[2]),octave),fb=clamp(p[1],0,32767)*2,mix=clamp(p[3],0,32767);
 if(!s->ready){s->time=s->time_target=target;s->depth=s->depth_target=dt;s->feedback=fb;s->mix=mix;s->ready=1;}
 if(target!=s->time_target){s->time_target=target;s->time_step=(target-s->time)/22050;s->time_left=22050;}
 if(dt!=s->depth_target){s->depth_target=dt;s->depth_step=(dt-s->depth)/44100;s->depth_left=44100;}
 int middle_time=s->time+(s->time_left>32?s->time_step*32:0);
 int middle_depth=s->depth+(s->depth_left>32?s->depth_step*32:0);
 int window=product(middle_time,middle_depth>>9);window=clamp(window,4096,PD_MAX_TIME_MS*441/10*4096);
 if(rate!=s->rate_cache || window!=s->window_cache) {
  int step=rate-1048576;
  s->increment=step?(unsigned)(((long long)step*8388608ll)/window):0;
  s->rate_cache=rate;s->window_cache=window;
 }
 if(!s->time_left && !s->depth_left && !s->increment && s->phase==0x80000000u &&
  s->feedback==fb && s->mix==mix && s->filled>=(unsigned)((s->time>>12)+2)) {
  neutral(s,ring,l,r,(unsigned)((s->time>>12)+2),fb,mix);return;
 }
 /* Constant while Time and Loop Depth are settled, including nonzero pitch. */
 int fixed_width=(!s->time_left && !s->depth_left)?product(s->time,s->depth>>9):-1;
 if(!r && fixed_width>=0 && (s->increment || s->phase!=0x80000000u) && s->feedback==fb && s->mix==mix &&
  s->filled>=(unsigned)(2*(s->time>>12)+6)) {
  shifted(s,ring,l,fixed_width,fb,mix);return;
 }
 unsigned write=s->write,filled=s->filled,phase=s->phase;
 int dc=s->dc_out,dc_in=s->dc_in,fb0=s->feedback,mix0=s->mix;
 int ramp=fb!=fb0 || mix!=mix0;
 for(int i=0;i<64;++i) {
  if(s->time_left && !--s->time_left)s->time=s->time_target;
  else if(s->time_left)s->time+=s->time_step;
  if(s->depth_left && !--s->depth_left)s->depth=s->depth_target;
  else if(s->depth_left)s->depth+=s->depth_step;
  phase+=s->increment;
  int q=(int)(phase>>16)-32768,weight=q<0?-q:q;
  int width=weight?(fixed_width>=0?fixed_width:product(s->time,s->depth>>9)):0;
  int lag_a=s->time-(weight?product(width,q):0)+8192;
  lag_a=clamp(lag_a,16384,(PD_SAMPLES-4u)*4096u);
  int wet;
  if(weight==0)wet=tap(ring,write,(unsigned)lag_a,filled);
  else {
   int qb=q>=0?q-32768:q+32768;
   int lag_b=s->time-product(width,qb)+8192;
   lag_b=clamp(lag_b,16384,(PD_SAMPLES-4u)*4096u);
   int b=tap(ring,write,(unsigned)lag_b,filled);
   if(weight==32768)wet=b;
   else {int a=tap(ring,write,(unsigned)lag_a,filled);wet=a+q15(b-a,weight);}
  }
  int feedback=ramp?fb0+(fb-fb0)*(i+1)/64:fb;
  int blend=ramp?mix0+(mix-mix0)*(i+1)/64:mix;
  int dry=l[i],dr=r?r[i]:dry;
  int input=r?(dry+dr)/4:dry/2;
  int pre=input+(((short)wet*(int)(unsigned short)feedback)>>15);
  dc=clamp(pre-dc_in+dc-(dc*94)/32768,-262143,262143);dc_in=pre;
  ring[write]=(short)clip(dc);
  if(++write==PD_SAMPLES)write=0;
  if(filled<PD_SAMPLES)++filled;
  wet=clamp(wet*2,-32768,32767);
  if(blend==16384){l[i]=(short)((dry+wet)>>1);if(r)r[i]=(short)((dr+wet)>>1);}
  else {l[i]=(short)(blend==32767?wet:clamp(dry+q15(wet-dry,blend),-32768,32767));if(r)r[i]=(short)(blend==32767?wet:clamp(dr+q15(wet-dr,blend),-32768,32767));}
 }
 s->phase=phase;s->write=write;s->filled=filled;s->dc_in=dc_in;s->dc_out=dc;s->feedback=fb;s->mix=mix;
}
