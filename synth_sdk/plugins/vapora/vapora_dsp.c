/* Copyright (C) 2026 AiAAAA. GPL-3.0-or-later with the OP-1 firmware permission. */
/* VAPORA R3. GPL-3.0-or-later with OP-1 firmware permission.
 * WARM preserves R2. Compact banks wrap the same 64-sample fundamental period.
 * Native pitch is Q32 at 44.1 kHz; two increments feed each 22.05 kHz step.
 * Controls run at block rate with sample ramps. Single guarded mip at rest;
 * one shared bank fade handles family/LOD changes without multiplying fades.
 */
#include "vapora_dsp.h"
#include "vapora_tables.h"

uint32_t ps_loop_increment(int32_t pitch)
{ return pitch<=0 ? 0 : (uint32_t)pitch>>8; }
int ps_level_for(uint32_t inc,int32_t *blend)
{
    int l=0;uint32_t limit=1u<<18;
    while(l<7&&inc>=limit){++l;limit<<=1;}
    *blend=l==7?0:ps_clamp((int32_t)(inc>>(2+l))-32768,0,32768);
    return l;
}
#define PS_INLINE static inline __attribute__((always_inline))
PS_INLINE int32_t lerp(int32_t a,int32_t b,int32_t t)
{ return a+(((b-a)*t)>>15); }
typedef struct {const int16_t *a,*b;unsigned shift,mask;} ps_bank;
PS_INLINE ps_bank bank(int family,int lod)
{
    ps_bank b={ps_tables[ps_family_tables[family][0]][lod],ps_tables[ps_family_tables[family][1]][lod],
               18u+(unsigned)lod,(1u<<(ps_family_bits[family]-lod))-1};return b;
}
PS_INLINE int32_t head(ps_bank b,uint32_t phase,int32_t morph)
{
    unsigned i=(phase>>b.shift)&b.mask,j=(i+1)&b.mask;
    int32_t f=(phase>>(b.shift-15))&32767;
    return lerp(lerp(b.a[i],b.a[j],f),lerp(b.b[i],b.b[j],f),morph);
}
static uint32_t random32(uint32_t *x)
{*x^=*x<<13;*x^=*x>>17;*x^=*x<<5;return *x;}
static int32_t wave(uint32_t ph)
{
    int32_t t=(int32_t)(ph>>16);
    t=t<32768?t*2-32768:98303-t*2;
    return (t*(65536-(t<0?-t:t)))>>15;
}
static int32_t cutoff(int bright,int depth)
{
    bright=ps_clamp(bright+(depth>>2),0,32767);
    int c=700+((bright*bright)>>15);if(c>30000)c=30000;
    return 32768-(((32768-c)*(32768-c))>>15);
}
PS_INLINE uint32_t auxiliary(uint32_t inc,int ratio,int detune)
{
    uint32_t a=(inc>>15)*(unsigned)ratio+(((inc&32767)*(unsigned)ratio)>>15);
    return a+(uint32_t)(((int32_t)(a>>8)*detune)>>7);
}
void ps_start(ps_voice *v,const ps_params *p,uint32_t seed,uint32_t serial)
{
    if(!seed)seed=1;
    v->phase[0]=random32(&seed);v->phase[1]=random32(&seed);
    v->lfo=random32(&seed);v->serial=serial;
    for(int k=0;k<PS_CONTROLS;k++)v->smooth[k]=p->q[k];
    v->lp1=v->lp2=0;v->aux_gain=32768;v->interval=p->interval;
    v->ratio=ps_interval_ratio(p->interval);v->ready=1;
    v->bank_ready=0;v->bank_family=v->old_family=p->character;
    v->bank_lod=v->old_lod=0;v->bank_mix=32768;
    for(int k=0;k<8;k++)v->up[k]=0;
    v->up_pos=0;
}
void ps_render(ps_voice *v,int16_t out[PS_BLOCK],const int32_t pitch[PS_BLOCK],
               int32_t velocity,const ps_params *p)
{
    int start[PS_CONTROLS];
    for(int k=0;k<PS_CONTROLS;k++){
        start[k]=v->smooth[k];int d=p->q[k]-start[k],s=d/8;
        if(d&&!s)s=d;
        v->smooth[k]+=s;
    }
    int aux0=v->aux_gain;
    if(v->interval!=p->interval){
        if(!aux0){v->interval=p->interval;v->ratio=ps_interval_ratio(p->interval);v->aux_gain=8192;}
        else v->aux_gain=ps_clamp(aux0-8192,0,32768);
    }else v->aux_gain=ps_clamp(aux0+8192,0,32768);

    uint32_t maxinc=0;int flat=1;
    for(int n=0;n<PS_BLOCK;n++){
        uint32_t inc=ps_loop_increment(pitch[n]);if(inc>maxinc)maxinc=inc;
        if(pitch[n]!=pitch[0])flat=0;
    }
    uint32_t bound=maxinc*4+(maxinc>>4); /* half rate, worst octave + drift */
    int32_t unused;int target=ps_level_for(bound+(bound>>3),&unused);
    /* 12.5% guard permits ordinary bends without sacrificing an entire
     * octave of upper-register bandwidth to the transition margin. */
    if(!v->bank_ready){v->bank_lod=target;v->bank_family=p->character;v->bank_ready=1;}
    /* Never retain an unsafe bank across an abrupt external pitch jump.
     * Both tables in every normal fade satisfy the <=0.4 Fs stored limit. */
    if(bound>(1u<<(18+v->bank_lod))){
        v->bank_lod=target;v->bank_family=p->character;v->bank_mix=32768;
    }
    if(v->bank_mix<32768&&bound>(1u<<(18+v->old_lod)))v->bank_mix=32768;
    if(v->bank_mix==32768&&(target!=v->bank_lod||p->character!=v->bank_family)){
        v->old_lod=v->bank_lod;v->old_family=v->bank_family;
        v->bank_lod=target;v->bank_family=p->character;v->bank_mix=0;
    }
    int bank0=v->bank_mix;
    v->bank_mix=ps_clamp(bank0+2048,0,32768); /* 46 ms, one fade at a time */
    int bank_step=v->bank_mix-bank0;
    ps_bank current=bank(v->bank_family,v->bank_lod);
    ps_bank previous=bank(v->old_family,v->old_lod);

    uint32_t lstep=((uint32_t)ps_speed_millihz(v->smooth[PS_SPEED])*24932u)>>8;
    int d0=(wave(v->lfo)*start[PS_MOTION])>>15;
    v->lfo+=lstep*PS_BLOCK;
    int d1=(wave(v->lfo)*v->smooth[PS_MOTION])>>15;
    int m0=ps_clamp(start[PS_MORPH]+(d0>>3),0,32767);
    int m1=ps_clamp(v->smooth[PS_MORPH]+(d1>>3),0,32767);
    int c0=cutoff(start[PS_BRIGHT],d0),c1=cutoff(v->smooth[PS_BRIGHT],d1);
    int g0=((8192+(start[PS_THICK]>>2))*aux0)>>15;
    int g1=((8192+(v->smooth[PS_THICK]>>2))*v->aux_gain)>>15;
    /* Q6 accumulators give exact linear ramps with additions, not multiplies. */
    int morph=m0*64,cut=c0*64,mix=g0*64,cross=bank0*64;
    int dm=m1-m0,dc=c1-c0,dg=g1-g0;
    int drift=(d0+d1)>>9;
    int detune=((start[PS_THICK]+v->smooth[PS_THICK])>>8)-drift;
    uint32_t base=ps_loop_increment(pitch[0])*2;
    uint32_t maininc=base+(uint32_t)(((int32_t)(base>>8)*drift)>>7);
    uint32_t auxinc=auxiliary(base,v->ratio,detune);
    /* 1.2.3: +9.54 dB relative to 1.2.2. Fixed gain preserves dynamics;
     * six-voice dry sums are checked in tests/output_level.c. */
    int gain=(ps_clamp(velocity,0,32767)*11520)>>15;
    uint32_t ph0=v->phase[0],ph1=v->phase[1];
    int lp1=v->lp1,lp2=v->lp2;
    for(int n=0;n<PS_BLOCK;n+=2){
        if(!flat){
            base=ps_loop_increment(pitch[n])+ps_loop_increment(pitch[n+1]);
            maininc=base+(uint32_t)(((int32_t)(base>>8)*drift)>>7);
            auxinc=auxiliary(base,v->ratio,detune);
        }
        ph0+=maininc;ph1+=auxinc;
        int a=head(current,ph0,morph>>6),b=head(current,ph1,morph>>6);
        if(bank_step){
            a=lerp(head(previous,ph0,morph>>6),a,cross>>6);
            b=lerp(head(previous,ph1,morph>>6),b,cross>>6);
        }
        int x=(a*24576+b*(mix>>6))>>15;
        lp1+=((x-lp1)*(cut>>6))>>15;
        lp2+=((lp1-lp2)*(cut>>6))>>15;
        unsigned pos=v->up_pos;
        v->up[pos]=(lp2*gain)>>15;
        out[n]=(int16_t)v->up[(pos+4)&7];
        /* Symmetric FIR: four multiplies, no eight-iteration inner loop. */
        int s0=v->up[(pos+1)&7]+v->up[pos];
        int s1=v->up[(pos+2)&7]+v->up[(pos+7)&7];
        int s2=v->up[(pos+3)&7]+v->up[(pos+6)&7];
        int s3=v->up[(pos+4)&7]+v->up[(pos+5)&7];
        out[n+1]=(int16_t)((-109*s0+1127*s1-4523*s2+19889*s3)>>15);
        v->up_pos=(pos+1)&7;
        morph+=dm;cut+=dc;mix+=dg;cross+=bank_step;
    }
    v->phase[0]=ph0;v->phase[1]=ph1;v->lp1=lp1;v->lp2=lp2;
}
void ps_analyze(const ps_voice *v,const int16_t out[PS_BLOCK],
                const int16_t env[PS_BLOCK],uint32_t pitch,ps_snapshot *s)
{
    s->serial=v->serial;s->pitch=pitch;s->level=0;
    for(int k=0;k<PS_TRACE;k++){
        int32_t sum=0;
        for(int j=0;j<8;j++){
            int n=k*8+j,e=ps_clamp(env[n],0,32767),x=(out[n]*e)>>15;
            if((x<0?-x:x)>s->level)s->level=x<0?-x:x;
            sum+=x;
        }
        s->trace[k]=(int16_t)(sum>>3);
    }
    for(int k=0;k<4;k++)s->primary[k]=(int16_t)v->smooth[k];
}
