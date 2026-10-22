#include "native_mock.h"
#include "../vapora_dsp.h"
#include "../vapora_mailbox.h"
#include "../vapora_visual.h"
#include "preset_words.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <limits.h>

uint32_t ps_host_voices[6][9],ps_host_cycles;
int32_t ps_host_pitch[128];
int16_t ps_host_env[128],ps_host_words[8][8],ps_host_velocity[6];
int ps_host_slot,ps_host_shift;
volatile uint8_t op1_eng_page[4];
static FILE *drawing;
int ps_host_particle_layer=-1;
static unsigned drawn_lines,particle_light;
static int record_particles,particle_counts[8],particle_green,particle_colored;
static uint32_t particle_base_hash,particle_overlay_hash;
static op1_pt recorded_base[PS_PARTICLES];
static int recorded_base_count,logo_lines;
static op1_pt logo_first,logo_previous;
static uint32_t logo_shape_hash;
static op1_pt last_line_b;
static char last_drawn_text[48];
static const int16_t defaults[8]={9000,26000,18500,12500,0x5320,0,0,0};
#include "native_scene.h"
void *op1_raster(void){return (void*)1;}
void op1_raster_line(void *r,const op1_pt *a,const op1_pt *b,int32_t w,const uint8_t *c)
{
    (void)r;
    assert(a->x>=0&&a->x<320*1024&&a->y>=0&&a->y<153*1024);
    assert(b->x>=0&&b->x<320*1024&&b->y>=0&&b->y<153*1024);
    assert(w>0&&w<=PS_BIG_DOT_STROKE*2);drawn_lines++;last_line_b=*b;
    if(w==PS_DOT_STROKE||w==PS_MED_DOT_STROKE||w==PS_BIG_DOT_STROKE)particle_light+=c[2];
    if(record_particles&&ps_host_particle_layer==2){
        assert(w==1536);
        if(!logo_lines){logo_first=*a;logo_shape_hash=2166136261u;}
        else assert(a->x==logo_previous.x&&a->y==logo_previous.y);
        logo_previous=*b;logo_lines++;
        logo_shape_hash=(logo_shape_hash^(uint32_t)(a->x-logo_first.x))*16777619u;
        logo_shape_hash=(logo_shape_hash^(uint32_t)(a->y-logo_first.y))*16777619u;
        if(!particle_green)assert(c[0]==0&&abs(c[1]*149-c[2]*237)<=193);
        else assert(c[2]>=c[1]&&c[1]>=c[0]);
    }
    if(record_particles&&ps_host_particle_layer!=2){
        int overlay=ps_host_particle_layer;assert(overlay==0||overlay==1);
        int width=w;
        int size=width==PS_BIG_DOT_STROKE?0:width==PS_MED_DOT_STROKE?1:width==PS_DOT_STROKE?2:3;
        assert(width==PS_BIG_DOT_STROKE||width==PS_MED_DOT_STROKE||width==PS_DOT_STROKE||width==PS_TINY_DOT_STROKE);
        assert(b->x-a->x==(size==0?1200:size==1?800:650));
        particle_counts[size+overlay*4]++;
        if(!overlay){
            recorded_base[recorded_base_count++]=*a;
            const uint32_t values[]={(uint32_t)a->x,(uint32_t)a->y,(uint32_t)b->x,(uint32_t)b->y,(uint32_t)w,c[0],c[1],c[2],c[3]};
            for(unsigned j=0;j<sizeof(values)/sizeof(values[0]);j++)particle_base_hash=(particle_base_hash^values[j])*16777619u;
        }
        if(overlay){
            for(int b=0;b<recorded_base_count;b++){
                int dx=abs(a->x-recorded_base[b].x),dy=abs(a->y-recorded_base[b].y);
                assert(dx>=9*1024||dy>=6*1024);
            }
            particle_overlay_hash=(particle_overlay_hash^(uint32_t)a->x)*16777619u;
            particle_overlay_hash=(particle_overlay_hash^(uint32_t)a->y)*16777619u;
        }
        if(particle_green&&overlay){assert(c[0]==0&&abs(c[1]*149-c[2]*237)<=193);}
        else if(c[2])assert(c[2]>=c[1]&&c[1]>=c[0]);
        particle_colored+=c[1]>0;
    }
    if(drawing)fprintf(drawing,"<line data-particle-layer=\"%d\" x1=\"%.4f\" y1=\"%.4f\" x2=\"%.4f\" y2=\"%.4f\" stroke=\"#%02x%02x%02x\" stroke-width=\"%.4f\"/>\n",
        ps_host_particle_layer,a->x/1024.,a->y/1024.,b->x/1024.,b->y/1024.,c[0],c[1],c[2],w/1024.);
}
/* No TE font is extracted. Record stock text API arguments for host preview. */
typedef struct {int x,y,size;uint8_t c[4];char text[48];} host_text;
void op1_text_ctor(void *obj,int32_t x,int y,int size,const uint8_t *c)
{
    host_text *t=obj; t->x=x;t->y=y;t->size=size;memcpy(t->c,c,4);t->text[0]=0;
    assert(sizeof(*t)<=OP1_TEXT_SIZE&&(x==34*1024||x==0)&&size==12);
}
void op1_text_set(void *obj,const char *s)
{host_text *t=obj;assert(strlen(s)<sizeof t->text);for(int i=0;s[i];i++)assert(s[i]<'A'||s[i]>'Z');strcpy(t->text,s);}
void op1_text_draw(void *obj,void *r,const int32_t *m)
{
    (void)r;host_text *t=obj;int x=t->x+m[6],y=t->y+(m[7]>>10);
    strcpy(last_drawn_text,t->text);
    assert(x>=0&&x/1024+(int)strlen(t->text)*8<320&&y>=0&&y+t->size<153);
    if(drawing){
        fprintf(drawing,"<text x=\"%.4f\" y=\"%d\" font-size=\"%d\" fill=\"#%02x%02x%02x\">",x/1024.,y+t->size,t->size,t->c[0],t->c[1],t->c[2]);
        for(const char *c=t->text;*c;c++){
            if(*c=='<')fputs("&lt;",drawing);else if(*c=='&')fputs("&amp;",drawing);
            else fputc(*c>='a'&&*c<='z'?*c-'a'+'A':*c,drawing);
        }
        fputs("</text>\n",drawing);
    }
}
static uint32_t rng=12345;
static uint32_t random32(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static int32_t pitch_hz(double hz){return (int32_t)(hz*4294967296./44100.);}
static void set_pitch(double hz)
{for(int n=0;n<128;n++)ps_host_pitch[n]=pitch_hz(hz);}
static void reset(void)
{
    memcpy(ps_host_words[ps_host_slot],defaults,sizeof defaults);
    for(int i=0;i<6;i++){ps_host_voices[i][0]=0xdeadbeef;ps_host_velocity[i]=32767;}
    for(int i=0;i<128;i++)ps_host_env[i]=32767;
}
static void params_test(void)
{
    int16_t p[8];memcpy(p,defaults,sizeof p);
    ps_params a=ps_decode(p);
    assert(a.character==0&&a.interval==PS_UNISON&&ps_speed_millihz(a.q[PS_SPEED])==143);
    for(int w=5;w<8;w++)p[w]=8192;
    ps_params b=ps_decode(p);
    for(int k=0;k<PS_CONTROLS;k++)assert(a.q[k]==b.q[k]);
    assert(a.character==b.character&&a.interval==b.interval);
    for(int d=-1;d<=1;d+=2){
        for(int k=5;k<8;k++)p[k]=8192+d; /* actual handler delta +/-4 */
        b=ps_decode(p);assert(b.character==0&&b.interval==PS_UNISON);
        assert(abs(ps_speed_millihz(b.q[PS_SPEED])-143)<=1);
    }
    for(int k=5;k<8;k++)p[k]=1;
    b=ps_decode(p);assert(b.q[PS_SPEED]==1&&b.character==(PS_CHARACTERS-32%PS_CHARACTERS)%PS_CHARACTERS&&b.interval==0);
    for(int k=0;k<8;k++)p[k]=INT16_MIN;
    b=ps_decode(p);assert(b.q[0]==0&&b.q[PS_SPEED]==8192);
    assert(ps_interval_ratio(PS_UNISON)==32768&&ps_interval_ratio(PS_OCTAVE)==65536);
    assert(fabs(ps_interval_ratio(PS_FIFTH)/32768.-pow(2,7./12))<.00002);
    for(int i=0;i<PS_INTERVALS;i++){
        assert(fabs(ps_interval_ratio(i)/32768.-pow(2,ps_semitones[i]/12.))<.00002);
        p[6]=(int16_t)((i*32768+16384)/PS_INTERVALS);
        assert(ps_decode(p).interval==i);
    }
    p[6]=16384;assert(ps_decode(p).interval==PS_FIFTH);
    assert(ps_visual_speed_millihz(32767)<=540&&ps_visual_speed_millihz(32767)>=538);
    for(double hz=8.1758;hz<16000;hz*=1.05946){
        int32_t pi=pitch_hz(hz);
        double decoded=ps_loop_increment(pi)*256.*44100./4294967296.;
        assert(fabs(decoded-hz)<.0027);
    }
    assert(ps_loop_increment(INT32_MIN)==0&&ps_loop_increment(-1)==0);
    for(int l=0;l<7;l++){
        uint32_t edge=1u<<(18+l);int32_t m;
        assert(ps_level_for(edge-1,&m)==l&&m>=32760);
        assert(ps_level_for(edge,&m)==l+1&&m==0);
    }
}
static void stress(void)
{
    struct {uint32_t a;int16_t samples[128];uint32_t b;} guard={0x1234,{0},0x5678};
    reset();set_pitch(440);
    for(int v=0;v<6;v++)ENGINE_vapora.note_on(ps_host_voices[v],ps_host_words[0]);
    clock_t t=clock();
    for(int block=0;block<3000;block++){
        int16_t *w=ps_host_words[0];
        if(block%11==0)for(int k=0;k<8;k++)if(k!=4)w[k]=(int16_t)(random32()&32767);
        if(block%17==0)ENGINE_vapora.note_on(ps_host_voices[block%6],w);
        for(int n=0;n<128;n++){
            ps_host_pitch[n]=block%97==0?INT32_MAX:block%101==0?INT32_MIN:
                pitch_hz(20+((block*37+n)%11000));
            ps_host_env[n]=(int16_t)(random32()&32767);
        }
        for(int v=0;v<6;v++){
            ENGINE_vapora.render(ps_host_voices[v],guard.samples,w);
            assert(ps_host_voices[v][0]==0xdeadbeef);
            for(int n=0;n<128;n++)assert(abs(guard.samples[n])<15000);
        }
        assert(guard.a==0x1234&&guard.b==0x5678);
        if(block%19==0){
            ps_host_cycles+=25000000; SCREEN_vapora.update(0,16);
        }
    }
    double seconds=(clock()-t)/(double)CLOCKS_PER_SEC;
    printf("Stress: 3000 six-voice blocks, randomized controls/pitch/retriggers, %.3f host CPU seconds (not Blackfin timing)\n",seconds);
    ENGINE_vapora.render((void*)(OP1_VOICE_BASE+1),guard.samples,ps_host_words[0]);
    for(int n=0;n<128;n++)assert(!guard.samples[n]);
    ps_host_voices[0][1]=0;ENGINE_vapora.render(ps_host_voices[0],guard.samples,ps_host_words[0]);
    for(int n=0;n<128;n++)assert(!guard.samples[n]);
    /* Reader never consumes an incomplete or foreign publication. */
    ps_snapshot s;uint32_t seq=0;
    ps_mail[0].sequence=3;assert(!ps_read_mail(0,0,0x5320,&s,&seq));
    ps_mail[0].sequence=4;ps_mail[0].slot=1;assert(!ps_read_mail(0,0,0x5320,&s,&seq));
    ps_mail[0].slot=0;ps_mail[0].selector=0x5321;assert(!ps_read_mail(0,0,0x5320,&s,&seq));
    ps_mail[0].selector=0x5320;assert(ps_read_mail(0,0,0x5320,&s,&seq));
}
static void sound_contract(void)
{
    int16_t words[8];memcpy(words,defaults,sizeof words);
    words[3]=0;words[7]=1;
    ps_params p=ps_decode(words);ps_voice a,b;
    ps_start(&a,&p,123,1);ps_start(&b,&p,123,1);
    int16_t x[128],y[128];set_pitch(440);
    /* Speed must be inaudible when Motion is zero. */
    ps_params fast=p;fast.q[PS_SPEED]=32767;
    for(int i=0;i<200;i++){
        ps_render(&a,x,ps_host_pitch,32767,&p);ps_render(&b,y,ps_host_pitch,32767,&fast);
        assert(!memcmp(x,y,sizeof x));
        for(int n=0;n<128;n++)assert(abs(x[n])<15000);
    }
    /* Character crossfade and Interval gating remain bounded under reversals. */
    int prev=0,largest=0,flips=0;
    for(int block=0;block<1000;block++){
        p.character=(block/9)%PS_CHARACTERS;p.interval=(block/13)%PS_INTERVALS;
        p.q[PS_MORPH]=(block&1)?32767:0;
        p.q[PS_BRIGHT]=32767;p.q[PS_THICK]=32767;
        int before=a.interval, beforegain=a.aux_gain;
        ps_render(&a,x,ps_host_pitch,32767,&p);
        if(a.interval!=before){assert(beforegain==0);flips++;}
        for(int n=0;n<128;n++){
            int d=abs(x[n]-prev);if(d>largest)largest=d;prev=x[n];
            assert(abs(x[n])<15000);
        }
    }
    assert(flips>20&&largest<9000);
    printf("Sound contract: Motion=0 speed invariance; bounded voice output; %d gated interval changes; largest sample step %d\n",flips,largest);
    /* Sweep level boundaries in both directions, with an audible octave head.
     * Guarded old and new banks must remain safe throughout each normal fade. */
    p=ps_decode(defaults);p.interval=PS_OCTAVE;p.q[PS_BRIGHT]=32767;
    ps_start(&a,&p,100,1);largest=prev=0;int fades=0;
    for(int block=0;block<3000;block++){
        double semitones=60.*(block<1500?block:3000-block)/1500.;
        set_pitch(65.4064*pow(2,semitones/12.));
        ps_render(&a,x,ps_host_pitch,32767,&p);
        uint32_t inc=ps_loop_increment(ps_host_pitch[0]);
        uint32_t bound=inc*4+(inc>>4);
        assert(bound<=(1u<<(18+a.bank_lod)));
        if(a.bank_mix<32768){
            assert(bound<=(1u<<(18+a.old_lod)));fades++;
        }
        for(int n=0;n<128;n++){
            int step=abs(x[n]-prev);if(step>largest)largest=step;prev=x[n];
        }
    }
    assert(fades>100&&largest<9000);
    printf("Pitch sweep: %d safe crossfade blocks; largest sample step %d\n",fades,largest);
    /* Three-minute held chord on exactly the production DSP; no huge fixture. */
    ps_voice held[6];ps_params slow=ps_decode(defaults);
    int32_t chordpitch[6][128];
    for(int v=0;v<6;v++){
        ps_start(&held[v],&slow,100u+(unsigned)v,(unsigned)v+1);
        for(int n=0;n<128;n++)chordpitch[v][n]=pitch_hz(130.8128*pow(2,v*3./12));
    }
    /* Check actual independently phased voices, not six copies of one sample.
     * output_level.c also covers dense clusters and repeated-note unisons. */
    for(int block=0;block<62016;block++){
        int32_t sum[128]={0};
        for(int v=0;v<6;v++){
            ps_render(&held[v],x,chordpitch[v],32767,&slow);
            assert(abs(held[v].lp1)<30000&&abs(held[v].lp2)<30000);
            for(int n=0;n<128;n++)sum[n]+=x[n];
        }
        for(int n=0;n<128;n++)assert(sum[n]>=-32768&&sum[n]<=32767);
    }
    puts("Three-minute six-voice sustained-state check: PASS (host, no FX)");
    int16_t released[128]={0};ps_snapshot tail;
    ps_analyze(&held[0],x,released,(uint32_t)chordpitch[0][0],&tail);
    assert(tail.level==0);
    for(int n=0;n<PS_TRACE;n++)assert(tail.trace[n]==0);
}
static void fixture(const char *dir,const char *name,int which)
{
    char path[1024];snprintf(path,sizeof path,"%s/%s.pcm",dir,name);
    FILE *f=fopen(path,"wb");assert(f);
    reset();
    int16_t *w=ps_host_words[0];
    int count=which==1?6:1;
    double length=which==0?16:which==1?18:12;
    const int notes[6]={48,55,60,64,67,72};
    if(which==3){w[5]=24000;w[6]=30000;w[2]=32767;}
    if(which>=4){memcpy(w,preset_words[which-4],sizeof defaults);length=16;}
    for(int v=0;v<count;v++)ENGINE_vapora.note_on(ps_host_voices[v],w);
    int clips=0,peak=0;double sq=0;long frames=0;
    for(int b=0;b<(int)(length*44100/128);b++){
        double sec=b*128./44100.;
        if(which==2){
            double t=sec/length;
            w[0]=(int16_t)(32767*t);w[1]=(int16_t)(32767*(1-t));
            w[2]=(int16_t)(32767*t);w[3]=(int16_t)(32767*t);
            w[5]=sec<4?8192:sec<8?24576:8192;w[6]=sec<3?8192:sec<6?16384:30000;
            w[7]=(int16_t)(1+32766*t);
        }
        int32_t sum[128]={0};
        for(int v=0;v<count;v++){
            int note=which==1?notes[v]:which==3?96:60;
            if(which>=4)note+=12*preset_octave[which-4];
            set_pitch(440*pow(2,(note-69)/12.));
            for(int n=0;n<128;n++){
                double t=(b*128+n)/44100.;
                double env=fmin(1,t/1.5)*fmin(1,fmax(0,(length-t)/2.5));
                if(which>=4){
                    const int32_t *e=preset_preview[which-4];
                    double at=e[0]/1000.,de=e[1]/1000.,su=e[2]/32767.;
                    env=t<at?t/at:t<at+de?1-(1-su)*(t-at)/de:su;
                    env*=fmin(1,fmax(0,(length-t)/(e[3]/1000.)));
                }
                ps_host_env[n]=(int16_t)(env*32767);
            }
            int16_t out[128];ENGINE_vapora.render(ps_host_voices[v],out,w);
            for(int n=0;n<128;n++)sum[n]+=(out[n]*ps_host_env[n])>>15;
        }
        int16_t out[128];
        for(int n=0;n<128;n++){
            if(sum[n]>32767||sum[n]<-32768)clips++;
            out[n]=(int16_t)ps_clamp(sum[n],-32768,32767);
            if(abs(out[n])>peak)peak=abs(out[n]);
            sq+=(double)out[n]*out[n];frames++;
        }
        assert(fwrite(out,sizeof *out,128,f)==128);
    }
    fclose(f);assert(!clips);
    assert(peak>100&&sq/frames>100); /* Reject accidental silent/near-silent banks. */
    printf("Fixture %s: peak %d, RMS %.1f, clipping %d\n",name,peak,sqrt(sq/frames),clips);
}
static void previews(const char *dir)
{
    const char *names[]={"minimum","default","maximum","character","interval","speed","all-alternates","voice-onset","single-low","single-semitone","single-high"};
    for(int s=0;s<11;s++){
        reset();for(int k=0;k<4;k++)op1_eng_page[k]=0;
        if(s==0)for(int k=0;k<8;k++)if(k!=4)ps_host_words[0][k]=k<4?0:1;
        if(s==2||s==7)for(int k=0;k<8;k++)if(k!=4)ps_host_words[0][k]=32767;
        if(s==3||s==6)op1_eng_page[0]=1;
        if(s==4||s==6)op1_eng_page[1]=1;
        if(s==5||s==6)op1_eng_page[3]=1;
        if(s==6){ps_host_words[0][5]=25000;ps_host_words[0][6]=30000;ps_host_words[0][7]=32767;}
        SCREEN_vapora.init(0);
        ps_host_cycles+=25000000;SCREEN_vapora.update(0,16);
        for(int frame=0;frame<32;frame++){
            if(s==7||s>=8){
                int voices=s==7?6:1;
                set_pitch(s==8?130.8128:s==9?138.5913:1046.5);int16_t out[128];
                if(!frame)for(int v=0;v<voices;v++)ENGINE_vapora.note_on(ps_host_voices[v],ps_host_words[0]);
                for(int b=0;b<22;b++)for(int v=0;v<voices;v++)ENGINE_vapora.render(ps_host_voices[v],out,ps_host_words[0]);
            }
            if((s==7||s==0||s==1||s>=8)&&frame<24){
                const char *prefix=s==0?"idle":s==1?"default-idle":s>=8?names[s]:"animation";
                char path[1024];snprintf(path,sizeof path,"%s/%s-%02d.svg",dir,prefix,frame);
                drawing=fopen(path,"w");assert(drawing);
                fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"160\" viewBox=\"0 0 320 160\">\n",drawing);
            }
            ps_host_cycles+=25000000;SCREEN_vapora.update(0,16);
            if(drawing){fputs("</svg>\n",drawing);fclose(drawing);drawing=0;}
        }
        char path[1024];snprintf(path,sizeof path,"%s/%s.svg",dir,names[s]);
        drawing=fopen(path,"w");assert(drawing);
        fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"160\" viewBox=\"0 0 320 160\">\n",drawing);
        drawn_lines=0;
        ps_host_cycles+=25000000;SCREEN_vapora.update(0,16);
        fputs("</svg>\n",drawing);fclose(drawing);drawing=0;
        assert(drawn_lines<650);
        printf("Preview %s: %u overlay strokes, all inside 320x153\n",names[s],drawn_lines);
    }
    /* Animated extremes + every independent page combination, slot returns. */
    for(int n=0;n<500;n++){
        ps_host_slot=n%8;reset();
        for(int k=0;k<8;k++)if(k!=4)ps_host_words[ps_host_slot][k]=(int16_t)(random32()&32767);
        for(int k=0;k<4;k++)op1_eng_page[k]=(n>>k)&1;
        ps_host_cycles+=25000000;SCREEN_vapora.update(0,16);
    }
    ps_host_slot=0;
}
extern int ps_test_sun_design(void),ps_test_active_control(void);
extern uint32_t ps_test_particle_phase(void);
extern void ps_test_draw_sun(int cy,int design);
extern void ps_test_draw_sun_at(int cy,int design,int opening);
static void sun_motion_contract(const char *dir)
{
    assert(ps_sun_y(0)==56&&ps_sun_y(32767)==36);
    int previous=56;
    for(int q=0;q<=32767;q++){
        int y=ps_sun_y(q);
        assert(y<=previous&&y>=36&&y<=56);
        /* Exactly 75% travel before whole-pixel rounding. */
        assert(abs((56-y)*65534-q*39)<=32767);
        previous=y;
    }
    for(int design=0;design<PS_SUN_DESIGNS;design++)for(int cy=30;cy<=56;cy++){
        char path[1024];snprintf(path,sizeof path,"%s/sunmove-%02d-%02d.svg",dir,ps_sun_ids[design],cy);
        drawing=fopen(path,"w");assert(drawing);
        fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"160\">\n",drawing);
        drawn_lines=0;ps_test_draw_sun(cy,design);assert(drawn_lines<=100);
        fputs("</svg>\n",drawing);fclose(drawing);drawing=0;
        snprintf(path,sizeof path,"%s/sunfixed-%02d-%02d.svg",dir,ps_sun_ids[design],cy);
        drawing=fopen(path,"w");assert(drawing);
        fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"160\">\n",drawing);
        drawn_lines=0;ps_test_draw_sun_at(cy,design,32767);assert(drawn_lines<=100);
        fputs("</svg>\n",drawing);fclose(drawing);drawing=0;
    }
}
static void approved_suns_contract(const char *dir)
{
    for(int design=0;design<2;design++){
        for(int q=0;q<=32767;q++){
            drawn_lines=0;ps_test_draw_sun_at(ps_sun_y(q),design,q);
            assert(ps_native_nodes[0].visible==(design==0&&q>0));
            assert(drawn_lines<8);
            if(q==32767)assert(drawn_lines==(design==1?7u:0u));
        }
        for(int step=0;step<=80;step++){
            char path[1024];snprintf(path,sizeof path,"%s/approved-%02d-%02d.svg",dir,design+1,step);
            drawing=fopen(path,"w");assert(drawing);
            fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"160\">\n",drawing);
            int q=(80-step)*32767/80;ps_test_draw_sun_at(ps_sun_y(q),design,q);
            fputs("</svg>\n",drawing);fclose(drawing);drawing=0;
        }
    }
    puts("Approved suns: full 32768-value sweeps; Solid visibility and seven Lines strokes checked");
}
static void controls_contract(const char *dir)
{
    reset();for(int k=0;k<4;k++)op1_eng_page[k]=0;
    SCREEN_vapora.init(0);SCREEN_vapora.update(0,16);
    int initial=ps_test_sun_design();int16_t before[8];memcpy(before,ps_host_words[0],sizeof before);
    for(int i=0;i<PS_SUN_DESIGNS;i++){
        int previous=ps_test_sun_design();op1_eng_page[2]=1;SCREEN_vapora.update(0,16);
        assert(op1_eng_page[2]==0&&ps_test_sun_design()==(previous+1)%PS_SUN_DESIGNS&&ps_test_active_control()==4);
        assert(!memcmp(before,ps_host_words[0],sizeof before));
        int style=ps_test_sun_design();char path[1024];snprintf(path,sizeof path,"%s/sun-%02d.svg",dir,ps_sun_ids[style]);
        drawing=fopen(path,"w");assert(drawing);
        fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"160\">\n",drawing);
        drawn_lines=0;SCREEN_vapora.update(0,16);assert(drawn_lines<550);
        fputs("</svg>\n",drawing);fclose(drawing);drawing=0;
    }
    assert(ps_test_sun_design()==initial);
    for(int k=0;k<4;k++){
        ps_host_words[0][k]+=256;SCREEN_vapora.update(0,16);assert(ps_test_active_control()==k);
        if(k!=2){op1_eng_page[k]^=1;SCREEN_vapora.update(0,16);assert(ps_test_active_control()==k);}
    }
    for(int family=0;family<PS_CHARACTERS;family++)for(int direction=-1;direction<=1;direction+=2){
        int16_t w[8];memcpy(w,defaults,sizeof w);w[5]=(int16_t)(8192+family*256);
        assert(ps_decode(w).character==family);w[5]+=direction*256;
        assert(ps_decode(w).character==(family+direction+PS_CHARACTERS)%PS_CHARACTERS);
    }
    for(int n=0;n<PRESET_COUNT;n++)assert(ps_decode(preset_words[n]).character==(preset_words[n][5]-8192)/256);
    uint32_t phase=ps_test_particle_phase();ps_host_words[0][2]=0;ps_host_cycles+=25000000;
    particle_light=0;SCREEN_vapora.update(0,16);
    uint32_t night_step=ps_test_particle_phase()-phase;unsigned night_light=particle_light;
    phase=ps_test_particle_phase();ps_host_words[0][2]=32767;ps_host_cycles+=25000000;
    particle_light=0;SCREEN_vapora.update(0,16);
    uint32_t day_step=ps_test_particle_phase()-phase;
    assert(night_step>day_step*2&&night_light>particle_light);
    printf("Particles: low-sun step %u / high-sun step %u; sky brightness %u / %u\n",night_step,day_step,night_light,particle_light);
    puts("Controls: 7 sun steps/wrap, release IDs 01/02/03/04/05/06/07; saved words unchanged by presses; active turn/page; Character changes in one detent; core preset Character mappings");
}
extern int ps_test_secret(void);
static void secret_setup(void)
{
    ps_host_slot=0;reset();for(int k=0;k<4;k++)op1_eng_page[k]=0;
    SCREEN_vapora.init(0);SCREEN_vapora.update(0,16);
}
static void secret_press(int k)
{op1_eng_page[k]^=1;ps_host_cycles+=25000000;SCREEN_vapora.update(0,16);}
static void secret_arm(void)
{secret_press(0);secret_press(1);secret_press(3);secret_press(2);}
static void secret_wait(int frames)
{for(int i=0;i<frames;i++){ps_host_cycles+=25000000;SCREEN_vapora.update(0,16);}}
static void secret_contract(const char *dir)
{
    secret_setup();secret_arm();secret_wait(111);assert(!ps_test_secret());
    secret_wait(1);assert(ps_test_secret());
    for(int i=0;i<96;i++){
        char path[1024];snprintf(path,sizeof path,"%s/secret-%02d.svg",dir,i);
        drawing=fopen(path,"w");assert(drawing);
        fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"160\">\n",drawing);
        drawn_lines=0;secret_wait(1);assert(ps_test_secret()&&!drawn_lines);
        fputs("</svg>\n",drawing);fclose(drawing);drawing=0;
    }
    ps_host_words[0][1]++;secret_wait(1);assert(!ps_test_secret());
    secret_setup();secret_arm();secret_wait(110);ps_host_words[0][0]++;secret_wait(10);assert(!ps_test_secret());
    secret_setup();secret_press(1);secret_press(0);secret_press(3);secret_press(2);secret_wait(120);assert(!ps_test_secret());
    ps_host_cycles=UINT32_MAX-500000000u;secret_setup();secret_arm();secret_wait(112);assert(ps_test_secret());
    secret_press(0);assert(!ps_test_secret());
    secret_setup();secret_arm();ps_host_cycles+=2800000000u;SCREEN_vapora.update(0,16);assert(!ps_test_secret());
    puts("Secret: ordered four-click sequence, exact 7-second boundary, movement cancellation, exit, cycle wrap, and hidden-screen gap; 96 frames of three bounded native text objects");
}
extern void ps_test_geometry(const ps_params *,op1_pt *,int16_t *);
extern void ps_test_geometry_phase(uint32_t,uint32_t);
extern int ps_test_wave_mode(void);
static void side_cells_contract(const op1_pt *points)
{
    for(int row=0;row<PS_ROWS-1;row++)for(int col=0;col<PS_COLS-1;col++){
        if(col>1&&col<PS_COLS-3)continue;
        int i=row*PS_COLS+col;
        op1_pt a=points[i],b=points[i+1],c=points[i+PS_COLS],d=points[i+PS_COLS+1];
        int flip=col<2?0:1;
        int64_t area1=flip?(int64_t)(b.x-a.x)*(c.y-a.y)-(int64_t)(b.y-a.y)*(c.x-a.x):
            (int64_t)(b.x-a.x)*(d.y-a.y)-(int64_t)(b.y-a.y)*(d.x-a.x);
        int64_t area2=flip?(int64_t)(d.x-b.x)*(c.y-b.y)-(int64_t)(d.y-b.y)*(c.x-b.x):
            (int64_t)(d.x-a.x)*(c.y-a.y)-(int64_t)(d.y-a.y)*(c.x-a.x);
        if(area1<=0||area2<=0)fprintf(stderr,"Fold mode=%d row=%d col=%d area=%lld/%lld\n",ps_test_wave_mode(),row,col,(long long)area1,(long long)area2);
        assert(area1>0&&area2>0);
    }
}
static void wave_edges_contract(void)
{
    ps_params p=ps_decode(defaults);op1_pt points[PS_ROWS*PS_COLS];int16_t curves[PS_EDGES];
    int low[4]={INT_MAX,INT_MAX,INT_MAX,INT_MAX},high[4]={INT_MIN,INT_MIN,INT_MIN,INT_MIN};
    const int joints[4]={4*PS_COLS+1,5*PS_COLS+1,4*PS_COLS+PS_COLS-2,5*PS_COLS+PS_COLS-2};
    for(int frame=0;frame<16384;frame++){
        ps_test_geometry_phase(random32(),random32());
        p.q[PS_MORPH]=(frame&1)?32767:0;p.character=(frame>>1)%14;
        ps_test_geometry(&p,points,curves);side_cells_contract(points);
        for(int row=0;row<PS_ROWS;row++){
            op1_pt right=points[row*PS_COLS+PS_COLS-1];
            assert(right.x==(320-ps_side_x[row]-8)*1024);
            assert(right.x+PS_MESH_STROKE/2<312*1024);
        }
        for(int j=0;j<4;j++){
            int y=points[joints[j]].y;
            assert(y+PS_MESH_STROKE/2<=PS_WATER_BOTTOM*1024);
            if(y<low[j])low[j]=y;if(y>high[j])high[j]=y;
        }
    }
    for(int j=0;j<4;j++)assert(high[j]-low[j]>12*1024);
    ps_test_geometry_phase(0,0);
    puts("Wave edges: 16384 phase/Character/Morph samples; side triangles unfolded; right outline inset 8 px; lower joints move >12 px within the screen");
}
extern int ps_test_wave_mode(void);
extern void ps_test_wave_mode_set(int);
extern uint32_t ps_test_impact_count(void);
extern void ps_test_impact_last(op1_pt *);
extern void ps_test_impact_tick(uint32_t);
static void test_note_mail(int voice,uint32_t serial)
{
    ps_mailbox *m=&ps_mail[voice];m->sequence=(m->sequence+2)&~1u;
    m->slot=0;m->selector=defaults[4];m->snapshot.serial=serial;
    m->snapshot.level=2000;m->snapshot.pitch=(uint32_t)pitch_hz(261.6);
    for(int k=0;k<PS_TRACE;k++)m->snapshot.trace[k]=(k-8)*200;
}
extern int ps_test_footer(void);
extern void ps_test_footer_set(int);
static void footer_contract(const char *dir)
{
    assert(!ps_test_footer());reset();ps_host_shift=0;SCREEN_vapora.init(0);SCREEN_vapora.update(0,16);
    int16_t words[8];memcpy(words,ps_host_words[0],sizeof words);
    for(int state=0;state<2;state++){
        if(state){ps_host_shift=1;SCREEN_vapora.update(0,16);}
        assert(ps_test_footer()==state);
        char path[1024];snprintf(path,sizeof path,"%s/footer-%s.svg",dir,state?"on":"off");
        drawing=fopen(path,"w");assert(drawing);
        fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"160\">\n",drawing);
        SCREEN_vapora.update(0,16);fputs("</svg>\n",drawing);fclose(drawing);drawing=0;
    }
    for(int f=0;f<20;f++){ps_host_cycles+=25000000;SCREEN_vapora.update(0,16);assert(ps_test_footer());}
    ps_host_shift=0;SCREEN_vapora.update(0,16);assert(ps_test_footer());
    SCREEN_vapora.init(0);SCREEN_vapora.update(0,16);assert(ps_test_footer());
    ps_host_shift=1;SCREEN_vapora.update(0,16);assert(!ps_test_footer());
    ps_host_shift=0;SCREEN_vapora.update(0,16);assert(!ps_test_footer());
    assert(!memcmp(words,ps_host_words[0],sizeof words));
    ps_host_cycles+=800000000u;ps_host_shift=1;SCREEN_vapora.update(0,16);assert(!ps_test_footer());
    ps_host_shift=0;SCREEN_vapora.update(0,16);ps_host_shift=1;SCREEN_vapora.update(0,16);assert(ps_test_footer());
    ps_host_shift=0;SCREEN_vapora.update(0,16);
    puts("Footer: initially off; Shift rising edge toggles once; holds/releases/scene re-entry preserve choice; no saved-word edits");
}
static void geometry_contract(void)
{
    reset();for(int k=0;k<4;k++)op1_eng_page[k]=0;
    SCREEN_vapora.init(0);SCREEN_vapora.update(0,16);ps_test_wave_mode_set(0);
    ps_test_geometry_phase(0,0);
    ps_params p=ps_decode(defaults);op1_pt quiet[PS_ROWS*PS_COLS],playing[PS_ROWS*PS_COLS];
    int16_t curves[PS_EDGES];ps_test_geometry(&p,quiet,curves);
    uint32_t count=ps_test_impact_count();
    for(int v=0;v<3;v++)test_note_mail(v,0x70000001u+v);
    ps_test_impact_tick(25000000);assert(ps_test_impact_count()==count);
    for(int v=3;v<6;v++)test_note_mail(v,0x70000001u+v);
    ps_test_impact_tick(25000000);assert(ps_test_impact_count()==count+1);
    int dip=0,rebound=0;
    for(int frame=0;frame<32;frame++){
        /* Held notes publish changing waveforms/pitch, but no new serials. */
        for(int v=0;v<6;v++)test_note_mail(v,0x70000001u+v);
        ps_mail[0].snapshot.pitch=(uint32_t)pitch_hz(frame&1?130.8:1046.5);
        ps_test_impact_tick(25000000);ps_test_geometry(&p,playing,curves);
        assert(ps_test_impact_count()==count+1);side_cells_contract(playing);
        for(int e=0;e<PS_EDGES;e++)assert(curves[e]==0);
        for(int i=0;i<PS_ROWS*PS_COLS;i++){
            int d=playing[i].y-quiet[i].y;
            if(d>dip)dip=d;if(d<rebound)rebound=d;
            assert(playing[i].x==quiet[i].x&&abs(d)<=8*1024);
            if(i%PS_COLS<2||i%PS_COLS>8||i/PS_COLS==0||i/PS_COLS==5)assert(!d);
        }
        if(frame>=23)assert(!memcmp(quiet,playing,sizeof quiet));
    }
    assert(dip>2*1024&&rebound> -1024);
    op1_pt previous={0},location;int changed=0;
    for(int n=0;n<12;n++){
        test_note_mail(n%6,0x71000000u+n);ps_test_impact_tick(25000000);ps_test_impact_tick(25000000);
        ps_test_impact_last(&location);changed+=memcmp(&location,&previous,sizeof location)!=0;previous=location;
        ps_test_geometry(&p,playing,curves);
        for(int i=0;i<PS_ROWS*PS_COLS;i++)assert(abs(playing[i].y-quiet[i].y)<=8*1024);
    }
    assert(changed>=10&&ps_test_impact_count()==count+13);
    for(int n=0;n<24;n++)ps_test_impact_tick(25000000);
    ps_test_geometry(&p,playing,curves);assert(!memcmp(quiet,playing,sizeof quiet));
    /* A real six-voice adapter chord also generates exactly one impact. */
    count=ps_test_impact_count();set_pitch(261.625565);int16_t out[128];
    for(int v=0;v<6;v++)ENGINE_vapora.note_on(ps_host_voices[v],ps_host_words[0]);
    for(int frame=0;frame<32;frame++){
        for(int block=0;block<22;block++)for(int v=0;v<6;v++)ENGINE_vapora.render(ps_host_voices[v],out,ps_host_words[0]);
        ps_test_impact_tick(25000000);
    }
    assert(ps_test_impact_count()==count+1);
    ps_test_geometry(&p,playing,curves);assert(!memcmp(quiet,playing,sizeof quiet));
    puts("Trampoline: one outward pulse per note or grouped chord; random positions, bounded overlap, pinned rim, exact settling despite held audio; real six-voice chord checked");
}
extern void ps_test_audition_reset(int);
extern void ps_test_impact_last_state(uint32_t*,uint32_t*);
extern int ps_test_impact_profile(int,uint32_t);
static void hold_return_contract(void)
{
    int tap_previous=ps_test_impact_profile(0,0),held_previous=ps_test_impact_profile(0,150000000u);
    int tap_drop=0,held_drop=0,tap_rebound=0,held_rebound=0;
    assert(tap_previous==held_previous&&tap_previous>0);
    for(int x=0;x<=1800;x++){
        /* Incoming motion and the sharp peak must be identical. */
        assert(ps_test_impact_profile(x,0)==ps_test_impact_profile(x,150000000u));
        int tap=ps_test_impact_profile(-x,0),held=ps_test_impact_profile(-x,150000000u);
        if(tap_previous-tap>tap_drop)tap_drop=tap_previous-tap;
        if(held_previous-held>held_drop)held_drop=held_previous-held;
        if(tap<tap_rebound)tap_rebound=tap;if(held<held_rebound)held_rebound=held;
        tap_previous=tap;held_previous=held;
    }
    assert(held_drop<tap_drop*3/4);
    assert(held_rebound>tap_rebound*3/4&&held_rebound<0);
    assert(ps_test_impact_profile(-480,150000000u)>0);
    assert(ps_test_impact_profile(-480,0)<=0);
    assert(tap_previous==0&&held_previous==0);
    puts("Held return: identical sharp front; trailing descent slope reduced >25%; rebound reduced >25%; both spatial tails finite");
}
static void hold_ripple_contract(void)
{
    /* Same start/geometry for tap, short hold, long hold, chord and reused voice. */
    uint32_t credit[5];int settled[5];int64_t energy[5];
    for(int scenario=0;scenario<5;scenario++){
        reset();for(int k=0;k<4;k++)op1_eng_page[k]=0;
        for(int v=0;v<PS_VOICES;v++)ps_held_serial[v]=0;
        SCREEN_vapora.init(0);SCREEN_vapora.update(0,16);ps_test_audition_reset(0);
        ps_test_geometry_phase(0,0);ps_params p=ps_decode(defaults);
        op1_pt rest[PS_ROWS*PS_COLS],points[PS_ROWS*PS_COLS];int16_t curves[PS_EDGES];
        ps_test_geometry(&p,rest,curves);
        for(int v=0;v<(scenario==3?3:1);v++){
            uint32_t serial=0x7e000000u+(unsigned)scenario*8u+(unsigned)v;
            test_note_mail(v,serial);ps_held_serial[v]=scenario?serial:0;
        }
        ps_test_impact_tick(25000000);ps_test_impact_tick(25000000);
        assert(ps_test_impact_count()==1);settled[scenario]=0;energy[scenario]=0;
        for(int f=0;f<48;f++){
            if(scenario==1&&f==8)ps_held_serial[0]=0;
            if(scenario==3&&f==8){ps_held_serial[0]=0;ps_held_serial[1]=0;}
            /* A different held serial on the same voice must not feed this hit. */
            if(scenario==4&&f==8)ps_held_serial[0]++;
            ps_test_impact_tick(25000000);ps_test_geometry(&p,points,curves);
            uint32_t age,extension;ps_test_impact_last_state(&age,&extension);
            credit[scenario]=extension;
            assert(extension<=6*25000000u&&ps_test_impact_count()==1);
            if(!settled[scenario]&&age-extension>=22*25000000u)settled[scenario]=f+1;
            side_cells_contract(points);
            for(int i=0;i<PS_ROWS*PS_COLS;i++){
                int d=abs(points[i].y-rest[i].y);assert(d<=8*1024);
                if(f>=16)energy[scenario]+=d;
            }
            if(f>=27)assert(!memcmp(rest,points,sizeof rest));
        }
    }
    assert(credit[0]==0&&credit[1]>0&&credit[1]<credit[2]);
    assert(credit[2]==6*25000000u&&credit[3]==credit[2]&&credit[4]==credit[1]);
    assert(settled[0]==22&&settled[1]>22&&settled[1]<28&&settled[2]==28);
    assert(energy[2]>energy[1]&&energy[1]>energy[0]);
    /* Real note_off only updates the visual gate, never the audio render state. */
    reset();set_pitch(261.625565);ENGINE_vapora.note_on(ps_host_voices[0],ps_host_words[0]);
    assert(ps_held_serial[0]);uint32_t serial=ps_held_serial[0];
    ENGINE_vapora.note_off((void*)(OP1_VOICE_BASE+1));assert(ps_held_serial[0]==serial);
    ENGINE_vapora.note_off(ps_host_voices[0]);assert(!ps_held_serial[0]);
    ENGINE_vapora.note_on(ps_host_voices[0],ps_host_words[0]);assert(ps_held_serial[0]&&ps_held_serial[0]!=serial);
    for(int v=0;v<PS_VOICES;v++)ENGINE_vapora.note_off(ps_host_voices[v]);
    printf("Hold ripple: tap %d frames, short hold %d, long hold %d; 375ms credit cap; partial chord release and reused voice checked; late energy %lld < %lld < %lld\n",
        settled[0],settled[1],settled[2],(long long)energy[0],(long long)energy[1],(long long)energy[2]);
}
static void wave_modes_contract(void)
{
    reset();for(int k=0;k<4;k++)op1_eng_page[k]=0;
    SCREEN_vapora.init(0);SCREEN_vapora.update(0,16);ps_test_wave_mode_set(0);
    int16_t saved[8];memcpy(saved,ps_host_words[0],sizeof saved);
    for(int press=0;press<4;press++){
        op1_eng_page[0]^=1;SCREEN_vapora.update(0,16);
        assert(op1_eng_page[0]==((press+1)&1));
        assert(!memcmp(saved,ps_host_words[0],sizeof saved));
    }
    for(int character=0;character<14;character++){
        ps_host_words[0][5]=(int16_t)(8192+character*256);
        SCREEN_vapora.update(0,16);assert(ps_test_wave_mode()==character);
    }
    ps_params p=ps_decode(defaults);op1_pt low[PS_ROWS*PS_COLS],high[PS_ROWS*PS_COLS];int16_t curves[PS_EDGES];
    uint32_t hashes[14];
    for(int mode=0;mode<14;mode++){
        p.character=mode;ps_test_wave_mode_set(mode);uint32_t hash=2166136261u;int morph_changes=0;
        for(int frame=0;frame<512;frame++){
            ps_test_geometry_phase((uint32_t)frame*0x019837abu,(uint32_t)frame*0x0ab87125u);
            p.q[PS_MORPH]=0;ps_test_geometry(&p,low,curves);
            p.q[PS_MORPH]=32767;ps_test_geometry(&p,high,curves);
            side_cells_contract(low);side_cells_contract(high);
            morph_changes+=memcmp(low,high,sizeof low)!=0;
            for(int i=0;i<PS_ROWS*PS_COLS;i++)hash=(hash^(uint32_t)high[i].y)*16777619u;
        }
        assert(morph_changes>400);hashes[mode]=hash;
        for(int m=0;m<mode;m++)assert(hashes[m]!=hash);
        p.q[PS_MORPH]=32767;
        for(int frame=0;frame<256;frame++){
            ps_test_geometry_phase((uint32_t)frame*0x032871abu,(uint32_t)frame*0x0ab87125u);
            if(!(frame%2))test_note_mail(frame%6,0x72000000u+mode*256u+frame);
            ps_test_impact_tick(25000000);ps_test_geometry(&p,high,curves);side_cells_contract(high);
        }
    }
    ps_test_wave_mode_set(0);ps_test_geometry_phase(0,0);
    puts("Character waves: blue toggles Morph/Character without edits; 14 retained styles track saved Character; Morph changes each style; phase/overlapping-impact checks preserve side cells");
}
extern uint32_t ps_test_ocean_phase(void),ps_test_sun_phase(void);
extern void ps_test_sun_color(uint32_t,int,uint8_t*);
extern void ps_test_sun_frame(int,uint32_t,int);
static void sun_pulse_contract(const char *dir)
{
    uint8_t color[4];
    for(int depth=0;depth<=32767;depth++){
        ps_test_sun_color(0,depth,color);
        assert(color[0]==255&&color[1]==255&&color[2]==255&&color[3]==255);
        ps_test_sun_color(0x80000000u,depth,color);
        assert(color[0]==255&&color[1]>=66&&color[2]>=63&&color[3]==255);
        assert(!memcmp(color,ps_host_sun_fill,4));
    }
    assert(color[1]==66&&color[2]==63);
    for(int i=0;i<256;i++){
        ps_test_sun_color((uint32_t)i<<24,0,color);
        assert(color[0]==255&&color[1]==255&&color[2]==255);
    }
    /* Capture every sun at white, half depth and full orange; same geometry. */
    for(int design=0;design<7;design++)for(int step=0;step<3;step++){
        char path[1024];snprintf(path,sizeof path,"%s/pulse-%02d-%d.svg",dir,design+1,step);
        drawing=fopen(path,"w");assert(drawing);
        fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"160\">\n",drawing);
        ps_test_sun_frame(design,0x80000000u,step==2?32767:step*16384);
        fputs("</svg>\n",drawing);fclose(drawing);drawing=0;
    }
    reset();for(int k=0;k<4;k++)op1_eng_page[k]=0;
    SCREEN_vapora.init(0);SCREEN_vapora.update(0,16);
    op1_pt base[PS_ROWS*PS_COLS],other[PS_ROWS*PS_COLS];int16_t curves[PS_EDGES];
    ps_params p=ps_decode(ps_host_words[0]);ps_test_geometry(&p,base,curves);
    for(int depth=0;depth<2;depth++)for(int speed=0;speed<2;speed++){
        p.q[PS_MOTION]=depth?32767:0;p.q[PS_SPEED]=speed?32767:1;
        ps_test_geometry(&p,other,curves);assert(!memcmp(base,other,sizeof base));
    }
    uint32_t ocean_steps[2],sun_steps[2];
    for(int speed=0;speed<2;speed++){
        ps_host_words[0][3]=speed?32767:0;ps_host_words[0][7]=speed?32767:1;
        uint32_t ocean=ps_test_ocean_phase(),sun=ps_test_sun_phase();
        ps_host_cycles+=25000000u;SCREEN_vapora.update(0,16);
        ocean_steps[speed]=ps_test_ocean_phase()-ocean;sun_steps[speed]=ps_test_sun_phase()-sun;
        double expected=4294967296.0*.0625*ps_speed_millihz(ps_host_words[0][7])/1000.;
        assert(fabs(sun_steps[speed]-expected)/expected<.0001);
    }
    assert(ocean_steps[0]==ocean_steps[1]&&sun_steps[1]>sun_steps[0]*90u);
    puts("Sun pulse: zero Motion white at every phase; full depth reaches shared red-orange; all seven shapes; Speed follows audio rate; ocean shape/rate independent of Orange");
}
extern void ps_test_particle_frame(int,uint32_t);
static void interval_particles_contract(const char *dir)
{
    static const int expected[5][4]={{8,4,0,0},{8,8,16,0},{0,18,6,0},{0,8,32,0},{0,0,0,48}};
    static const int base_counts[4]={8,16,24,0};uint32_t base_hash[64],overlay_hash[10][64],logo_hash[10];
    reset();for(int k=0;k<4;k++)op1_eng_page[k]=0;
    SCREEN_vapora.init(0);SCREEN_vapora.update(0,16);
    ps_params p=ps_decode(defaults);op1_pt base[PS_ROWS*PS_COLS],other[PS_ROWS*PS_COLS];int16_t curves[PS_EDGES];
    ps_test_geometry(&p,base,curves);
    for(int i=0;i<10;i++){
        p.interval=i;ps_test_geometry(&p,other,curves);assert(!memcmp(base,other,sizeof base));
        for(int phase=0;phase<64;phase++){
            memset(particle_counts,0,sizeof particle_counts);particle_colored=0;
            particle_green=i&1;record_particles=1;recorded_base_count=0;logo_lines=0;
            particle_base_hash=particle_overlay_hash=2166136261u;
            ps_test_particle_frame(i,(uint32_t)phase*0x04000000u);
            record_particles=0;
            assert(!memcmp(particle_counts,base_counts,sizeof base_counts));
            for(int size=0;size<4;size++)assert(particle_counts[4+size]==expected[i/2][size]);
            if(i==0)base_hash[phase]=particle_base_hash;
            else assert(base_hash[phase]==particle_base_hash);
            overlay_hash[i][phase]=particle_overlay_hash;
            for(int previous=0;previous<i;previous++)assert(overlay_hash[previous][phase]!=particle_overlay_hash);
            assert(particle_colored>=8);
            assert(logo_lines==2*(4+i/2));
            assert(logo_previous.x==logo_first.x&&logo_previous.y==logo_first.y);
            if(!phase){
                logo_hash[i]=logo_shape_hash;
                for(int previous=0;previous<i;previous++)assert(logo_hash[previous]!=logo_hash[i]);
            }else assert(logo_hash[i]==logo_shape_hash);
        }
    }
    /* Same-clock screenshots: each step snaps immediately, with fixed ocean. */
    reset();ps_host_words[0][2]=32767;ps_host_words[0][3]=0;
    op1_eng_page[1]=1;SCREEN_vapora.init(0);SCREEN_vapora.update(0,16);
    while(ps_test_sun_design()!=0){op1_eng_page[2]=1;SCREEN_vapora.update(0,16);}
    for(int i=0;i<10;i++){
        ps_host_words[0][6]=(int16_t)((i*32768+16384)/10);
        char path[1024];snprintf(path,sizeof path,"%s/interval-stars-%02d.svg",dir,i);
        drawing=fopen(path,"w");assert(drawing);
        fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"160\">\n",drawing);
        SCREEN_vapora.update(0,16);
        assert(ps_decode(ps_host_words[0]).interval==i&&ps_test_active_control()==1);
        fputs("</svg>\n",drawing);fclose(drawing);drawing=0;
    }
    puts("Interval stars: original 48-star base invariant in all 10 states at 64 phases; overlay count/width/length at stock scale; all ten positions distinct; no base overpaint; readout-green tint; <=96 small stars plus one distinct moving signature; ocean independent; stock interval encoding retained");
}
extern void ps_test_logo_vertex(int,int,op1_pt*);
extern void ps_test_signature_position(int,uint32_t,op1_pt*);
static void logo_rotation_contract(void)
{
    ps_params p=ps_decode(defaults);op1_pt base[PS_ROWS*PS_COLS],other[PS_ROWS*PS_COLS];
    int16_t curves[PS_EDGES];ps_test_geometry(&p,base,curves);
    for(int interval=0;interval<10;interval++){
        double first_angle=0,previous=0;op1_pt first={0},point;
        for(int q=0;q<=32767;q++){
            ps_test_logo_vertex(interval,q,&point);
            double angle=atan2(point.y,point.x);
            if(q==0){first=point;first_angle=angle;}
            else {while(angle<previous-3.141592653589793)angle+=6.283185307179586;assert(angle>=previous-.0003);}
            previous=angle;
        }
        assert(abs(point.x*10-first.x*16)<=15&&abs(point.y*10-first.y*16)<=15);
        assert(fabs(previous-first_angle-6.283185307179586)<.0003);
        op1_pt pos;int previous_x=0;
        for(int step=0;step<1024;step++){
            ps_test_signature_position(interval,(uint32_t)step*0x00400000u,&pos);
            assert(pos.x>=previous_x&&pos.x>=16&&pos.x<=303);previous_x=pos.x;
        }
        assert(pos.x==303);ps_test_signature_position(interval,0,&pos);assert(pos.x==16);
        for(int q=0;q<=32767;q+=1024){
            p.q[PS_THICK]=q;p.interval=interval;ps_test_geometry(&p,other,curves);
            assert(!memcmp(base,other,sizeof base));
        }
    }
    puts("Signature rotation: all ten stars make one full turn over all 32768 Thickness values; 60% endpoint growth; Thickness and Interval do not change wave geometry.");
}
extern void ps_test_character_dial(int16_t);
static void character_dial_contract(const char *dir)
{
    double previous=0,first=0;
    for(int word=1;word<=32767;word++){
        ps_test_character_dial((int16_t)word);
        int16_t words[8]={0};words[5]=(int16_t)word;
        ps_params decoded=ps_decode(words);char expected[48];
        snprintf(expected,sizeof expected,"character  %02d %s",decoded.character+1,ps_character_names[decoded.character]);
        assert(!strcmp(last_drawn_text,expected));
        double angle=atan2(last_line_b.y-PS_DIAL_Y*1024,last_line_b.x-16*1024);
        if(word==1)first=angle;
        else while(angle<previous-.000001)angle+=2*3.141592653589793;
        assert(word==1||angle>=previous-.000001);
        assert(angle-first<=1.5*3.141592653589793+.001);previous=angle;
    }
    assert(fabs(first-3.141592653589793*.75)<.001);
    assert(fabs(previous-first-3.141592653589793*1.5)<.001);
    for(int i=0;i<5;i++){
        char path[1024];snprintf(path,sizeof path,"%s/character-dial-%d.svg",dir,i);
        drawing=fopen(path,"w");assert(drawing);
        fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"160\">\n",drawing);
        ps_test_character_dial((int16_t)(1+i*32766/4));
        fputs("</svg>\n",drawing);fclose(drawing);drawing=0;
    }
    puts("Character dial: all 32767 encoder positions stay monotonic on one 270-degree arc, bottom-left to bottom-right; readout matches the audio Character name at every position; sound/preset decoding unchanged");
}
int main(int argc,char **argv)
{
    assert(argc==2);reset();params_test();sound_contract();SCREEN_vapora.init(0);stress();
    fixture(argv[1],"held-c4",0);fixture(argv[1],"six-note-pad",1);
    fixture(argv[1],"macro-sweep",2);fixture(argv[1],"high-octave",3);
    for(int i=0;i<PRESET_COUNT;i++){
        char name[100];snprintf(name,sizeof name,"preset-%02d-%s",i+1,preset_names[i]);
        fixture(argv[1],name,i+4);
    }
    footer_contract(argv[1]);geometry_contract();hold_return_contract();hold_ripple_contract();previews(argv[1]);controls_contract(argv[1]);sun_motion_contract(argv[1]);approved_suns_contract(argv[1]);secret_contract(argv[1]);sun_pulse_contract(argv[1]);interval_particles_contract(argv[1]);logo_rotation_contract();character_dial_contract(argv[1]);wave_modes_contract();wave_edges_contract();puts("PASS: host DSP/adapter/mailbox/screen checks");
    return 0;
}
