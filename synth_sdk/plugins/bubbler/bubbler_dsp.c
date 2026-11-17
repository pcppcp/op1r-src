/* SPDX-License-Identifier: GPL-3.0-or-later
 * ESC_CODE / Volker Junginger's Delay Trio (github.com/VolkerJunginger/OP-1, Delay-Trio, e88427c), unchanged.
 * Based on +bubbler~.c, copyright Tom Erbe and others, BSD-3-Clause.
 * Eight-grain scheduler, Hermite interpolation, windowed grains, reversal,
 * random delay/start/length, filtered saturated feedback. See PORT-NOTES.md.
 */
#include "bubbler_dsp.h"
#include "tables.h"
#include "bubbler_params.h"

static const short *env_table = BB_ENV;
static const short *cubic_table = BB_CUBIC;
static const short *clip_table = BB_ATAN;
void bb_fast_clip(void *memory)
{
    if(!memory){clip_table=BB_ATAN;return;}
    short *p=memory;
    for(unsigned i=0;i<257u;++i)p[i]=BB_ATAN[i];
    clip_table=p;
}
void bb_fast_tables(void *memory)
{
    if (!memory) {env_table=BB_ENV;cubic_table=BB_CUBIC;return;}
    short *p = memory;
    for (unsigned i=0;i<257u;++i) p[i]=BB_ENV[i];
    for (unsigned i=0;i<2048u;++i) p[257u+i]=BB_CUBIC[i];
    env_table=p;cubic_table=p+257;
}

static volatile int last_note;
void bb_note(int note){if(note>=0&&note<128)last_note=note-60;}
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int q15(int a, int b) { return (a * b) >> 15; }
static unsigned random_word(struct bb_state *s)
{
    s->seed = s->seed * 196314165u + 907633515u;
    return s->seed;
}
int bb_time_samples(int p) { return BB_TIME[clamp(p, 0, 32767) >> 8]; }
int bb_cutoff(int p) { return BB_CUTOFF[clamp(p, 0, 32767) >> 8]; }

void bb_init(struct bb_state *s)
{
    s->pitch_mode=0;
    for (int i = 0; i < BB_GRAINS; ++i) {
        s->grain[i].active = 0;
        s->grain[i].pos = s->grain[i].age = s->grain[i].length = 0;
        s->grain[i].wait = s->grain[i].fade = s->grain[i].env = s->grain[i].env_step = 0;
        s->grain[i].step = 4096;
    }
    s->write = s->filled = s->countdown = 0;
    s->seed = 0x42554242u;
    s->delay = s->feedback = s->mix = s->filter_a = 0;
    s->filter_z = s->dc_in = s->dc_out = s->ready = s->params_ready = s->prep_ready = 0;
    s->res_x1=s->res_x2=s->res_y1=s->res_y2=s->res_active=0;
    /* Bulk is shared by synth/drum. Never clear 256 KiB in the audio interrupt.
     * filled gates every interpolation tap until it has been written anew. */
}

/* Each interpolation product is a signed 16x16 multiply, rather than the
 * original 32-bit polynomial. Ring samples are bounded by atan(8)*16384;
 * Hermite's <=1.25 absolute weight sum remains inside signed 16 bits. */
static inline int cubic(const short *ring, unsigned pos)
{
    unsigned i = (pos >> 12) & BB_MASK;
    const short *c = cubic_table + ((pos & 4095u) >> 3) * 4u;
    const short *p = ring + i;
    return (p[-1]*c[0] + p[0]*c[1] + p[1]*c[2] + p[2]*c[3]) >> 14;
}

/* Separate kernels keep the integer-position path free of interpolation work.
 * Grain state is loaded once per span, not eight times per output sample. */
static void __attribute__((noinline)) add_direct(const short * restrict ring,
    int * restrict out, unsigned pos, int step, unsigned env, int env_step, int n)
{
    const short *table=env_table;
    unsigned index=(pos>>12)&BB_MASK;
    /* The common +/-1 paths use halfword post-increment/decrement loads.
     * Wrapping remains in the uncommon boundary path, outside the tight loop. */
    if(step==4096 && index+(unsigned)n<=BB_SAMPLES) {
        const short *in=ring+index;
        for(int j=0;j<n;++j) {
            env=(unsigned)((int)env+env_step);
            out[j]+=(*in++ * table[env>>16])>>15;
        }
    } else if(step==-4096 && index+1u>=(unsigned)n) {
        const short *in=ring+index;
        for(int j=0;j<n;++j) {
            env=(unsigned)((int)env+env_step);
            out[j]+=(*in-- * table[env>>16])>>15;
        }
    } else {
        for(int j=0;j<n;++j) {
            env=(unsigned)((int)env+env_step);
            int sample=ring[(pos>>12)&BB_MASK];
            out[j]+=(sample*table[env>>16])>>15;
            pos+=(unsigned)step;
        }
    }
}
static void __attribute__((noinline)) add_cubic(const short * restrict ring,
    int * restrict out, unsigned pos, int step, unsigned env, int env_step, int n)
{
    const short *table=env_table;
    for(int j=0;j<n;++j) {
        env=(unsigned)((int)env+env_step);
        short sample=(short)cubic(ring,pos);
        out[j]+=(sample*table[env>>16])>>15;
        pos+=(unsigned)step;
    }
}
/* At exactly half speed the phase alternates integer/half-integer.
 * Catmull-Rom's half-phase Q14 weights are [-1024,9216,9216,-1024].
 * The expression below is the identical integer sum/shift, without four
 * coefficient products or a coefficient-table lookup on every sample. */
static inline int half_sample(const short *p)
{
    return (9*(p[0]+p[1])-(p[-1]+p[2]))>>4;
}
static void __attribute__((noinline)) add_half(const short * restrict ring,
    int * restrict out,unsigned pos,int step,unsigned env,int env_step,int n)
{
    const short *table=env_table;
    if(pos&2048u) {
        env=(unsigned)((int)env+env_step);
        short sample=(short)half_sample(ring+((pos>>12)&BB_MASK));
        *out+++=(sample*table[env>>16])>>15;
        pos+=(unsigned)step;--n;
    }
    /* Start every pair at an integer phase. Unrolling the phase cycle avoids
     * a branch on alternating phases for every individual sample. */
    if(step>0) {
        while(n>=2) {
            const short *p=ring+((pos>>12)&BB_MASK);
            short a=p[0],b=(short)half_sample(p);
            env=(unsigned)((int)env+env_step);out[0]+=(a*table[env>>16])>>15;
            env=(unsigned)((int)env+env_step);out[1]+=(b*table[env>>16])>>15;
            out+=2;pos+=4096u;n-=2;
        }
    } else {
        while(n>=2) {
            unsigned index=(pos>>12)&BB_MASK;
            short a=ring[index],b=(short)half_sample(ring+((index-1u)&BB_MASK));
            env=(unsigned)((int)env+env_step);out[0]+=(a*table[env>>16])>>15;
            env=(unsigned)((int)env+env_step);out[1]+=(b*table[env>>16])>>15;
            out+=2;pos-=4096u;n-=2;
        }
    }
    if(n) {
        env=(unsigned)((int)env+env_step);
        *out+=(ring[(pos>>12)&BB_MASK]*table[env>>16])>>15;
    }
}
static void add_checked(const short *ring,int *out,unsigned pos,int step,
    unsigned env,int env_step,int n,unsigned write,unsigned filled)
{
    int direct=((pos|(unsigned)step)&4095u)==0;
    for(int j=0;j<n;++j) {
        env=(unsigned)((int)env+env_step);
        unsigned distance=(write-(pos>>12))&BB_MASK;
        if(distance>=3u && distance+2u<=filled) {
            int sample=direct?ring[(pos>>12)&BB_MASK]:cubic(ring,pos);
            out[j]+=(sample*env_table[env>>16])>>15;
        }
        pos=(pos+(unsigned)step)&BB_PHASE_MASK;
    }
}
static void grain_span(struct bb_grain *g,const short *ring,int *out,int n,
                       unsigned write,unsigned filled)
{
    if(!g->active) return;
    unsigned skip=g->wait<(unsigned)n?g->wait:(unsigned)n;
    g->wait-=skip;out+=skip;n-=(int)skip;
    while(n>0 && g->active) {
        unsigned boundary;int env_step;
        if(g->age<g->fade) {boundary=g->fade;env_step=(int)g->env_step;}
        else if(g->age<g->length-g->fade) {
            boundary=g->length-g->fade;env_step=0;g->env=256u<<16;
        } else {boundary=g->length;env_step=-(int)g->env_step;}
        unsigned count=boundary-g->age;
        if(count>(unsigned)n) count=(unsigned)n;
        unsigned first=(write-(g->pos>>12))&BB_MASK;
        unsigned endpos=(g->pos+(unsigned)g->step*(count-1u))&BB_PHASE_MASK;
        unsigned last=(write-(endpos>>12))&BB_MASK;
        int valid=filled==BB_SAMPLES ||
            (first>=3u && last>=3u && first+2u<=filled && last+2u<=filled);
        if(!valid) add_checked(ring,out,g->pos,g->step,g->env,env_step,(int)count,write,filled);
        else if(((g->pos|(unsigned)g->step)&4095u)==0)
            add_direct(ring,out,g->pos,g->step,g->env,env_step,(int)count);
        else if((g->step==2048 || g->step==-2048) && !(g->pos&2047u))
            add_half(ring,out,g->pos,g->step,g->env,env_step,(int)count);
        else add_cubic(ring,out,g->pos,g->step,g->env,env_step,(int)count);
        g->pos=(g->pos+(unsigned)g->step*count)&BB_PHASE_MASK;
        g->env=(unsigned)((int)g->env+env_step*(int)count);
        g->age+=count;out+=count;n-=(int)count;
        if(g->age>=g->length) g->active=0;
    }
}

/* Called only with dc/256 after its +/-65535*256 clamp. Narrow operands
 * select a native signed 16x16 product; no output approximation changes. */
static inline int softclip(int v,const short *clip)
{
    unsigned x=(unsigned)__builtin_abs(v);
    unsigned i=x>>9;
    short base=clip[i];
    short slope=(short)(clip[i+1]-base);
    short fraction=(short)(x&511u);
    int y=base+((slope*fraction)>>9);
    return v<0?-y:y;
}

static int spawn(struct bb_state *s, unsigned base_length, const struct bb_params *p,
                 const unsigned *free_at,unsigned offset)
{
    for (int i = 0; i < BB_GRAINS; ++i) {
        struct bb_grain *g = &s->grain[i];
        if (free_at[i]>offset) continue;
        int jitter = (int)(random_word(s) >> 17) - 16384;
        int spread = q15(jitter, p->size_var); /* -0.5 .. +0.5 in Q15 */
        g->length = (unsigned)((int)base_length + q15((int)base_length, spread) / 2);
        if (g->length < 16u) g->length = 16u;
        static const int offsets[10]={0,0,-12,-7,7,12,-5,-3,3,5};
        int mode=s->pitch_mode;
        int pitch=mode==1?last_note:offsets[mode];
        if(p->pitch_var) {
            unsigned chance=random_word(s);
            if((chance&32767u)<(unsigned)p->pitch_var*32768u/100u)
                pitch+=(chance&0x80000000u)?12:-12;
        }
        pitch=clamp(pitch,-72,79);
        int step = BB_PITCH[pitch + 72];
        unsigned direction = random_word(s) >> 17;
        int backwards = p->reverse == 32767 || (int)direction < p->reverse;
        /* Whole MIDI range plus an independent ±1 octave variation. */
        if(step>8192) {
            unsigned factor=backwards?(unsigned)step+8192u:(unsigned)step;
            unsigned longest=(BB_SAMPLES-8u)*4096u/factor;
            if(g->length>longest)g->length=longest;
        }
        int lag=step<=32768?(s->delay*step)>>12:(int)(((long long)s->delay*step)>>12);
        jitter = (int)(random_word(s) >> 17) - 16384;
        lag += q15(s->delay, q15(jitter, p->scatter)) / 2;
        /* Keep the whole trajectory in the ring, including advancing writer. */
        int travel = backwards ? ((int)g->length * (step + 4096)) >> 12 : (int)g->length;
        int minimum=(int)g->length+4;
        if(!backwards && step>8192)minimum=((int)g->length*(step-4096)>>12)+4;
        if(minimum<256)minimum=256;
        lag=clamp(lag,minimum,(int)BB_SAMPLES-travel-4);
        g->wait = (unsigned)q15((int)(random_word(s) >> 17), q15((int)s->countdown, p->jitter));
        g->pos = ((s->write << 12) - ((unsigned)lag << 12) + (g->wait << 12)) & BB_PHASE_MASK;
        g->step = backwards ? -step : step;
        g->age = g->env = 0;
        g->fade = g->length / 2u;
        if (g->fade > 2205u) g->fade = 2205u;
        g->env_step = (256u << 16) / g->fade;
        g->active = 1;
        return i;
    }
    return -1;
}

/* All reachable states have dc_in == filter_z. Using the filter increment
 * directly preserves the DC blocker while removing redundant sample state.
 * Specialization keeps mono/stereo and settled/ramping branches out of loops. */
static inline __attribute__((always_inline)) void finish_impl(
    struct bb_state *s,short *ring,short *left,short *right,int frames,
    const int *mixbuf,int target_fb,int target_mix,int target_a,
    int stereo,int ramp,int half_mix,int resonant)
{
    const short *clip=clip_table;
    int z=s->filter_z,dc=s->dc_out;
    int c0=0,c1=0,c2=0,x1=0,x2=0,y1=0,y2=0;
    if(resonant) {
        const int *c=BB_RESONANT+(s->decoded.tone*64+s->decoded.resonance)*3;
        c0=c[0];c1=c[1];c2=c[2];
        x1=s->res_x1;x2=s->res_x2;y1=s->res_y1;y2=s->res_y2;
    }
    int fb0=s->feedback,mix0=s->mix,a0=s->filter_a;
    int fbr=fb0*64,mr=mix0*64,ar=a0*64;
    int fd=target_fb-fb0,md=target_mix-mix0,ad=target_a-a0;
    unsigned write0=s->write,write=write0;
    int off=0;
    while(off<frames) {
        int count=frames-off;
        if((unsigned)count>BB_SAMPLES-write) count=(int)(BB_SAMPLES-write);
        short *dest=ring+write;
        for(int j=0;j<count;++j) {
            int n=off+j;
            int fb=target_fb,mix=target_mix,a=target_a;
            if(ramp) {
                fbr+=fd;mr+=md;ar+=ad;
                fb=fbr>>6;mix=mr>>6;a=ar>>6;
            }
            short wet=(short)clamp(mixbuf[n]/2,-32768,32767);
            int dry=left[n],dry_r=stereo?right[n]:dry;
            int input=stereo?(dry+dry_r)/4:dry/2;
            int pre=input+((wet*(short)fb)>>15);
            int delta;
            if(resonant) {
                long long acc=(long long)c0*(pre+2*x1+x2)
                    -(long long)c1*y1-(long long)c2*y2;
                int out=(int)(acc>>28);out=clamp(out,-65535,65535);
                x2=x1;x1=pre;y2=y1;y1=out;
                delta=out-z;z=out;
            } else {delta=((pre-z)*a)>>14;z+=delta;}
            dc=clamp(delta*256+dc-(dc*94)/32768,-65535*256,65535*256);
            *dest++=(short)softclip(dc/256,clip);
            short wet_pcm=(short)clamp((int)wet*2,-32768,32767);
            if(half_mix) left[n]=(short)((dry+(int)wet_pcm)>>1);
            else left[n]=(short)(mix==32767?wet_pcm:clamp(dry+q15((int)wet_pcm-dry,mix),-32768,32767));
            if(stereo) {
                if(half_mix) right[n]=(short)((dry_r+(int)wet_pcm)>>1);
                else right[n]=(short)(mix==32767?wet_pcm:clamp(dry_r+q15((int)wet_pcm-dry_r,mix),-32768,32767));
            }
        }
        write=(write+(unsigned)count)&BB_MASK;off+=count;
    }
    /* Mirrored taps are consumed only in the preceding grain pass; refresh
     * them once after all feedback writes, rather than testing every sample. */
    if(write0<2u || write0+(unsigned)frames>BB_SAMPLES) {
        ring[BB_SAMPLES]=ring[0];ring[BB_SAMPLES+1u]=ring[1];
    }
    if(write0+(unsigned)frames>=BB_SAMPLES) ring[-1]=ring[BB_MASK];
    s->write=write;
    unsigned filled=s->filled+(unsigned)frames;
    s->filled=filled>BB_SAMPLES?BB_SAMPLES:filled;
    if(resonant) {s->res_x1=x1;s->res_x2=x2;s->res_y1=y1;s->res_y2=y2;}
    s->filter_z=s->dc_in=z;s->dc_out=dc;
    s->feedback=target_fb;s->mix=target_mix;s->filter_a=target_a;
}
#define FINISH_ARGS struct bb_state *s,short *ring,short *left,short *right,int frames,const int *mixbuf,int fb,int mix,int a
#define FINISH_PASS s,ring,left,right,frames,mixbuf,fb,mix,a
#define FINISH_FN(name,stereo,ramp,half) \
static void __attribute__((noinline)) name(FINISH_ARGS) { finish_impl(FINISH_PASS,stereo,ramp,half,0); }
FINISH_FN(finish_mono_half,0,0,1)
FINISH_FN(finish_mono_stable,0,0,0)
FINISH_FN(finish_mono_ramp,0,1,0)
FINISH_FN(finish_stereo_half,1,0,1)
FINISH_FN(finish_stereo_stable,1,0,0)
FINISH_FN(finish_stereo_ramp,1,1,0)
#define RES_FINISH(name,stereo,ramp,half) \
static void __attribute__((noinline)) name(FINISH_ARGS) { finish_impl(FINISH_PASS,stereo,ramp,half,1); }
RES_FINISH(finish_res_mono_half,0,0,1)
RES_FINISH(finish_res_mono_stable,0,0,0)
RES_FINISH(finish_res_mono_ramp,0,1,0)
RES_FINISH(finish_res_stereo_half,1,0,1)
RES_FINISH(finish_res_stereo_stable,1,0,0)
RES_FINISH(finish_res_stereo_ramp,1,1,0)
#undef RES_FINISH
#undef FINISH_FN
#undef FINISH_ARGS
#undef FINISH_PASS

void bb_render_work(struct bb_state *s, short *storage, short *left, short *right,
                    int frames, const short *par, int *mixbuf)
{
    short *ring=storage+2;
    if(!s->params_ready || par[2]!=s->saved[4] || par[4]!=s->saved[0] || par[5]!=s->saved[1] ||
       par[6]!=s->saved[2] || par[7]!=s->saved[3]) {
        bb_decode(par,&s->decoded);
        for(int k=0;k<4;++k) s->saved[k]=par[k+4];
        s->saved[4]=par[2];s->params_ready=1;
    }
    const struct bb_params *p=&s->decoded;
    int target_delay = bb_time_samples(par[0]);
    int target_feedback = (clamp(par[1], 0, 32767) * 31129) >> 15;
    int size_changed=!s->prep_ready || s->size_control!=p->size;
    if(size_changed) {
        s->size_control=p->size;
        s->size_value=2+(clamp(p->size,0,32767)*48)/32767;
    }
    int target_mix = clamp(par[3], 0, 32767);
    int target_a = BB_FILTER[p->tone];
    if (!s->ready) {
        s->delay = target_delay;
        s->feedback = target_feedback;
        s->mix = target_mix;
        s->filter_a = target_a;
        s->ready = 1;
    }
    /* ~90 ms delay slew; each grain captures its own delay and pitch. */
    int diff = target_delay - s->delay;
    s->delay += diff / 64;
    if (diff && diff / 64 == 0) s->delay += diff > 0 ? 1 : -1;
    if(size_changed || !s->prep_ready || s->prep_delay!=s->delay || s->prep_density!=p->density) {
        s->length=(unsigned)(s->delay*s->size_value/100);
        s->spacing=p->density?s->length*100u/(8u*(unsigned)p->density):0x7fffffffu;
        if(s->spacing<2u) s->spacing=2u;
        s->prep_delay=s->delay;s->prep_density=p->density;s->prep_ready=1;
    }
    unsigned length=s->length,spacing=s->spacing;
    /* Minimum read lag is >=256 samples, pitch limited to +/-12 st. Thus
     * all 64-sample reads refer to pre-block history: feedback can run after
     * summing, without adding a block of latency to the feedback loop. */
    unsigned write0=s->write,filled0=s->filled;
    for(int i=0;i<frames;++i) mixbuf[i]=0;
    if(p->density==0) s->countdown=0;
    else if(s->countdown>spacing) s->countdown=spacing;
    /* Finish each existing grain's full span once. Slot availability is kept
     * on the original sample timeline, independent of these advanced states.
     * This avoids splitting all eight kernels at each scheduling tick. */
    unsigned free_at[BB_GRAINS];
    for(int k=0;k<BB_GRAINS;++k) {
        struct bb_grain *g=&s->grain[k];
        free_at[k]=g->active?g->wait+g->length-g->age:0;
        grain_span(g,ring,mixbuf,frames,write0,filled0);
    }
    if(p->density) for(int off=0;off<frames;) {
        if(!s->countdown) {
            s->write=(write0+(unsigned)off)&BB_MASK;
            s->countdown=spacing;
            int slot=spawn(s,length,p,free_at,(unsigned)off);
            if(slot>=0) {
                struct bb_grain *g=&s->grain[slot];
                free_at[slot]=(unsigned)off+g->wait+g->length;
                grain_span(g,ring,mixbuf+off,frames-off,write0,filled0);
            }
        }
        unsigned count=(unsigned)(frames-off);
        if(count>s->countdown) count=s->countdown;
        s->countdown-=count;off+=(int)count;
    }
    s->write=write0;
    int stable=s->feedback==target_feedback && s->mix==target_mix && s->filter_a==target_a;
    if(p->resonance && !s->res_active) {
        s->res_x1=s->res_x2=s->res_y1=s->res_y2=s->filter_z;
    }
    s->res_active=p->resonance!=0;
    if(p->resonance) {
        int settled=s->feedback==target_feedback && s->mix==target_mix;
        if(right) {
            if(!settled)finish_res_stereo_ramp(s,ring,left,right,frames,mixbuf,target_feedback,target_mix,target_a);
            else if(target_mix==16384)finish_res_stereo_half(s,ring,left,right,frames,mixbuf,target_feedback,target_mix,target_a);
            else finish_res_stereo_stable(s,ring,left,right,frames,mixbuf,target_feedback,target_mix,target_a);
        } else {
            if(!settled)finish_res_mono_ramp(s,ring,left,0,frames,mixbuf,target_feedback,target_mix,target_a);
            else if(target_mix==16384)finish_res_mono_half(s,ring,left,0,frames,mixbuf,target_feedback,target_mix,target_a);
            else finish_res_mono_stable(s,ring,left,0,frames,mixbuf,target_feedback,target_mix,target_a);
        }
    }
    else if(right) {
        if(stable && target_mix==16384)finish_stereo_half(s,ring,left,right,frames,mixbuf,target_feedback,target_mix,target_a);
        else if(stable) finish_stereo_stable(s,ring,left,right,frames,mixbuf,target_feedback,target_mix,target_a);
        else finish_stereo_ramp(s,ring,left,right,frames,mixbuf,target_feedback,target_mix,target_a);
    } else if(!stable) finish_mono_ramp(s,ring,left,0,frames,mixbuf,target_feedback,target_mix,target_a);
    else if(target_mix==16384) finish_mono_half(s,ring,left,0,frames,mixbuf,target_feedback,target_mix,target_a);
    else finish_mono_stable(s,ring,left,0,frames,mixbuf,target_feedback,target_mix,target_a);
}

void bb_render(struct bb_state *s,short *storage,short *left,short *right,
               int frames,const short *par)
{
    bb_render_work(s,storage,left,right,frames,par,s->mixbuf);
}
