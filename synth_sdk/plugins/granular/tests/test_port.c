#define L1POOL_IMPL
/* Host audio render of one plugin -> raw int16 mono 44.1 kHz on stdout (harness/audio.py writes the WAV).
 *   synth: -DENGINE=<symbol>: a phrase on voice 0 (C3 E3 G3 C4 G3 E3, 0.4 s each, velocities 127 / 64 alternating)
 *   fx:    -DFX=<symbol>: plucked saw notes through the FX at position 0, then 1.5 s of silence (tails)
 * args: knob0..3 (0..32767). Reports a peak / NaN-free summary on stderr; exit 1 if the output is silent. */
#include "op1_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6]; int16_t host_voice_env[128]; int16_t host_accel[3] = { 0, 0, 16384 };
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
uint8_t *host_node_style_rgba(void *s, const char *id) { static uint8_t c[4]; (void)s; (void)id; return c; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
void *host_raster(void) { return 0; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c) { (void)r; (void)a; (void)b; (void)w; (void)c; }
void *read_fx_param_safe(int slot, int pos) { (void)slot; (void)pos; return 0; }
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t sz, const uint8_t *c) { (void)x; (void)y; (void)sz; (void)c; return o; }
void host_text_set(void *t, const char *s) { (void)t; (void)s; }
void host_text_draw(void *t, void *r, const int32_t *m) { (void)t; (void)r; (void)m; }
int host_shift;
int op1_sc_source(int src, int16_t *out) { (void)src; (void)out; return 0; }   /* ENTE: no sidechain on the host */


#include <assert.h>
#include <string.h>
static _Alignas(32) uint8_t test_l1[8192];
static int pool_enabled,pool_fresh=1;
void *op1_l1_claim(int part,const void *owner,uint32_t bytes,int *fresh){
    (void)owner;assert(part==OP1_L1_SYNTH&&bytes<=sizeof test_l1);
    if(!pool_enabled){*fresh=0;pool_fresh=1;return 0;}
    *fresh=pool_fresh;pool_fresh=0;return test_l1;
}
#include "../granular.c"
int gr_host_mode,gr_host_screen=0x14,gr_host_sync[8];
uintptr_t gr_host_sound[8]={1,2,3,4,5,6,7,8};
const op1_engine_t *op1_engines[64];int op1_engine_count=64;
static int browser,editor,queue_full;
static int queue[256][2],qn,delivered[64][2],dn;
int sb_is_open(int n){(void)n;return browser;}
int se_is_open(void){return editor;}
int gr_host_post(int c,int d){if(queue_full)return 0;assert(qn<256);queue[qn][0]=c;queue[qn++][1]=d;return 1;}
#define GR_TEST_HOOKS
#include "../keys.c"
static void key(int c,int d){if(!control_key(c,d)){assert(dn<64);delivered[dn][0]=c;delivered[dn++][1]=d;}}
static void drain(void){for(int j=0;j<qn;j++)key(queue[j][0],queue[j][1]);qn=0;}
static uint32_t seed=42;
static uint32_t random32(void){seed=seed*1664525u+1013904223u;return seed;}
static short samples[2][OP1_SAMPLE_MAX+OP1_SAMPLE_GUARD];
static struct{short *d;short peaks[OP1_PEAK_BUCKETS];} buffers[2];
static void setup(void){
    for(int i=0;i<8192;i++)host_sine[i]=(short)lrint(32767*sin(2*M_PI*i/8192));
    for(int s=0;s<2;s++){
        buffers[s].d=samples[s];
        for(int i=0;i<OP1_SAMPLE_MAX;i++)samples[s][i+8]=(short)(random32()>>16);
        for(int i=0;i<OP1_PEAK_BUCKETS;i++)buffers[s].peaks[i]=i<1034?20000:0;
        host_sample_buffer[s]=(const uint8_t *)&buffers[s];host_knobs[s][4]=0x5303;
    }
    for(int i=0;i<6;i++)host_voice_vel[i]=127<<8;
    host_pitch_inc[0]=INC_C4;
    op1_engines[3]=&ENGINE_granular.x.engine;control_service();
}
static void test_grain_reference(void){
    for(int sh=0;sh<6;sh++)for(int n=1;n<=128;n++)for(int r=0;r<8;r++){
        grain_t g={random32()%1024,16+random32()%20000,0,0,128,sh};
        g.winc=0x80000000u/(128+random32()%50000);
        grain_t ref=g;int32_t a[129]={0},b[129]={0};
        grain_c(&g,samples[0]+8,a+1,n,WIN[sh]);
        for(int i=0;i<n;i++){
            uint32_t idx=ref.pos>>10;int32_t x=samples[0][8+idx],y=samples[0][9+idx];
            int32_t z=x+(((y-x)*(int32_t)(ref.pos&1023))>>10);
            b[i+1]=(z*WIN[sh][ref.wph>>20])>>15;
            ref.pos+=ref.rate;ref.wph+=ref.winc;
        }
        assert(!memcmp(a,b,sizeof a));assert(g.pos==ref.pos&&g.wph==ref.wph);
    }
    puts("PASS: 6,144 grain-loop cases match independent per-sample reference");
}
static void test_audio(void){
    short p[8]={4000,8000,3000,12000,0x5303,0,0,0},out[128];long nonzero=0;
    for(int sh=0;sh<6;sh++)for(int syn=0;syn<2;syn++){
        ENGINE_granular.sync[0]=(uint8_t)syn;host_beat()->len=22050<<14;
        for(int v=0;v<6;v++)native_note_on(OP1_VOICE(v),p);
        for(int b=0;b<500;b++){
            for(int k=0;k<4;k++)p[k]=(short)(random32()&32767);
            p[5]=(short)(2048+(random32()%49)*256);p[6]=(short)((sh*2+1)*32768/12);p[7]=(short)(random32()&32767);
            host_pitch_inc[0]=(int32_t)(INC_C4/4+(random32()%16)*INC_C4);
            for(int v=0;v<6;v++){
                native_render(OP1_VOICE(v),out,p);
                for(int n=0;n<128;n++)nonzero+=out[n]!=0;
                for(int i=0;i<4;i++)if(native_voices[v].g[i].winc){grain_t *g=&native_voices[v].g[i];assert(g->left>0&&g->sh>=0&&g->sh<6);}
            }
        }
    }
    assert(nonzero>10000);
    puts("PASS: 36,000 six-voice render calls, all shapes, sync, extreme controls and gain");
    for(int step=0;step<15;step++)for(unsigned bpm=20000;bpm<=400000;bpm+=5000){
        unsigned long long n=(unsigned long long)SYNC_TICKS[step]*27562500u/bpm;
        assert(sync_size(step,bpm)==(int)(n<64?64:n>529200?529200:n));
    }
    puts("PASS: all 15 clock divisions at 20–400 BPM");
    p[0]=0;p[1]=32767;p[2]=0;p[3]=32767;p[5]=2048;p[6]=32767;p[7]=32767;
    host_pitch_inc[0]=INC_C4/4;host_beat()->len=INT32_MAX;ENGINE_granular.sync[0]=1;
    native_note_on(OP1_VOICE(0),p);
    for(int b=0;b<12000;b++)native_render(OP1_VOICE(0),out,p);
    puts("PASS: 34-second sustained run crosses long synchronized grain lifetimes");
}
static void test_l1_fallback(void){
    short p[8]={4000,8000,10000,32767,0x5303,8192,0,8192},a[128],b[128];
    host_slot=0;ENGINE_granular.sync[0]=0;native_note_on(OP1_VOICE(0),p);
    for(int j=0;j<1000;j++){
        p[6]=(short)(random32()&32767);
        gvoice_t saved=native_voices[0];
        pool_enabled=1;native_render(OP1_VOICE(0),a,p);
        native_voices[0]=saved;pool_enabled=0;native_render(OP1_VOICE(0),b,p);
        assert(!memcmp(a,b,sizeof a));
    }
    puts("PASS: 1,000 L1 acquisition/loss/shape-change cases match ordinary-RAM output exactly");
}
static void test_samples(void){
    short p[8]={32767,32767,32767,32767,0x5303,32767,32767,32767},out[128];
    host_slot=0;host_sample_buffer[0]=0;scan();native_note_on(OP1_VOICE(0),p);native_render(OP1_VOICE(0),out,p);
    for(int i=0;i<128;i++)assert(out[i]==0);
    host_sample_buffer[0]=(const uint8_t *)&buffers[0];
    memset(buffers[0].peaks,0,sizeof buffers[0].peaks);buffers[0].peaks[1]=1000;
    scan();native_note_on(OP1_VOICE(0),p);assert(native_voices[0].len==512);
    for(int i=0;i<100;i++)native_render(OP1_VOICE(0),out,p);
    buffers[0].peaks[500]=10000;scan();native_render(OP1_VOICE(0),out,p);assert(native_voices[0].len==128256);
    host_slot=1;scan();native_note_on(OP1_VOICE(0),p);assert(native_voices[0].len==264600);
    assert(op1_sample_data(host_sample_buffer[1])==samples[1]+8);
    host_slot=0;scan();assert(sample_len(1)==0);
    puts("PASS: empty, short, replaced, maximum-length and per-slot sample buffers");
}
static void press(int c){key(c,1);key(c,0);}
static void test_controls(void){
    dn=qn=0;gr_host_mode=0;host_slot=0;context();
    press(0x3E);assert(ENGINE_granular.frozen);
    key(0x0D,1);key(0x0D,0);assert(dn==1&&os_down[0]&&!finger[0]);
    key(0x0D,1);assert(dn==1);key(0x0D,0);assert(dn==1);
    press(0x3E);drain();assert(!ENGINE_granular.frozen&&dn==2&&!os_down[0]);
    /* A queued thaw must not lift a note physically pressed again meanwhile. */
    press(0x3E);key(0x0D,1);key(0x0D,0);press(0x3E);key(0x0D,1);drain();assert(os_down[0]);key(0x0D,0);assert(!os_down[0]);
    /* Full key queue retries on the main pass. */
    dn=0;press(0x3E);key(0x0D,1);key(0x0D,0);queue_full=1;press(0x3E);assert(os_down[0]&&!sent[0]);queue_full=0;control_service();drain();assert(!os_down[0]);
    /* Plain white reaches native sample-browser dispatch unchanged. */
    dn=0;press(0x3D);assert(dn==2&&delivered[0][0]==0x3D&&delivered[1][1]==0);
    host_shift=1;dn=0;press(0x3D);assert(dn==0&&gr_host_sync[0]&&ENGINE_granular.sync[0]);host_shift=0;
    browser=1;dn=0;press(0x3D);press(0x3E);assert(dn==4&&!ENGINE_granular.frozen);browser=0;
    dn=0;key(0x26,1);host_shift=1;assert(host_knobs[0][6]==2730);key(0x26,0);host_shift=0;   /* pages: the core (shift_page) */
    assert(ENGINE_granular.x.key==gr_key&&ENGINE_granular.x.shift_page==((1<<1)|(1<<3)));
    assert(pitch_st(0)==0&&pitch_st(8192)==0&&pitch_st(8448)==1&&pitch_st(7936)==-1);
    assert(pitch_st(2048)==-24&&pitch_st(14336)==24);
    dn=0;press(0x3E);key(0x0D,1);key(0x0D,0);host_slot=1;control_service();drain();assert(!ENGINE_granular.frozen&&!os_down[0]);
    host_slot=0;context();press(0x3E);key(0x0D,1);key(0x0D,0);gr_host_sound[0]++;control_service();drain();assert(!ENGINE_granular.frozen&&!os_down[0]);
    dn=0;press(0x3E);key(0x0D,1);key(0x0D,0);gr_host_mode=1;control_service();drain();assert(!ENGINE_granular.frozen&&!os_down[0]);
    puts("PASS: freeze, retrigger, queue races/retries, browser passthrough, sync persistence, slot/preset/mode cleanup");
}
int main(void){setup();test_grain_reference();test_audio();test_l1_fallback();test_samples();test_controls();return 0;}
