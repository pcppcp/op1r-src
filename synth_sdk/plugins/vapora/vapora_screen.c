/* Copyright (C) 2026 AiAAAA. GPL-3.0-or-later with the OP-1 firmware permission. */
/* Original procedural ocean, 66 vertices / 165 unique edges.
 * GPL-3.0-or-later with OP-1 firmware permission. GUI thread only. */
#ifdef VAPORA_HOST
#include "tests/native_mock.h"
#else
#include "op1_synth.h"
#endif
#include "seq/seq_text.h"
#include "vapora_mailbox.h"
#include "vapora_visual.h"

#if defined(OP1_HOST) && !defined(VAPORA_HOST)
/* Standard op1r preview harness: emulate the cycle clock; the audio-only
 * harness does not supply text drawing. The screen harness overrides these
 * weak no-ops with its stock-font preview implementation. No device code. */
static uint32_t preview_cycles;
__attribute__((weak)) void *host_text_ctor(void *obj,int32_t x,int32_t y,
                                         int32_t size,const uint8_t *rgba)
{ (void)x;(void)y;(void)size;(void)rgba;return obj; }
__attribute__((weak)) void host_text_set(void *obj,const char *text)
{ (void)obj;(void)text; }
__attribute__((weak)) void host_text_draw(void *obj,void *r,const int32_t *m)
{ (void)obj;(void)r;(void)m; }
#endif

static op1_pt vertices[PS_ROWS*PS_COLS];
static uint8_t edges[PS_EDGES][2];
static uint32_t sequence[PS_VOICES], clock_last, elapsed, ocean_phase, idle_phase;
static uint32_t sun_phase;
static int view_slot=-1,view_selector=-1,ready;
static uint32_t particle_phase,particle_slow_phase;
static int active_knob, active_sun, control_ready;
static int footer_visible,footer_shift_previous;
static int shift_held(void)
{
#ifdef VAPORA_HOST
    return ps_host_shift!=0;
#elif defined(OP1_HOST)
    return 0; /* Standard preview has no Shift input; local tests exercise it. */
#else
    /* R246 Shift state, shared by op1r and the Hundekraft 3.0.2 baseline. */
    return *(volatile uint8_t *)0x01DD7F78!=0;
#endif
}
static int sun_design; /* 01 Solid is the power-on default. */
static int16_t control_words[8];
static uint8_t control_pages[4];
static uint8_t text_objects[4][OP1_TEXT_SIZE] __attribute__((aligned(4)));
static uint8_t text_made[4];
static char text_cache[4][48];

static uint32_t cycles(void)
{
#ifdef VAPORA_HOST
    return ps_host_cycles;
#elif defined(OP1_HOST)
    return preview_cycles;
#else
    uint32_t c; __asm__ volatile("%0 = CYCLES;" : "=d"(c)); return c;
#endif
}
#include "vapora_q.h"
static int32_t rounded_wave(uint32_t phase)
{
    int32_t t=(int32_t)((phase>>16)&65535);
    t=t<32768?t*2-32768:98303-t*2;
    return (t*(65536-(t<0?-t:t)))>>15;
}
static int gradient(unsigned x,unsigned y,int dx,int dy)
{
    x&=7u;y&=7u; /* periodic tile: phase wrap cannot jump the noise field */
    uint32_t h=x*0x9e3779b9u+y*0x85ebca6bu+0x4f434541u;
    h^=h>>16;h*=0x7feb352du;h^=h>>15;
    switch(h&7){
        case 0:return dx+dy; case 1:return dx-dy;
        case 2:return -dx+dy; case 3:return -dx-dy;
        case 4:return dx; case 5:return -dx;
        case 6:return dy; default:return -dy;
    }
}
static int fade(int t)
{
    int t2=(t*t)>>8,t3=(t2*t)>>8;
    return (t3*(6*t2-15*t+2560))>>8;
}
static int coherent(unsigned x,unsigned y)
{
    int dx=x&255,dy=y&255; x>>=8;y>>=8;
    int fx=fade(dx),fy=fade(dy);
    int a=gradient(x,y,dx,dy),b=gradient(x+1,y,dx-256,dy);
    int c=gradient(x,y+1,dx,dy-256),d=gradient(x+1,y+1,dx-256,dy-256);
    a+=((b-a)*fx)>>8;c+=((d-c)*fx)>>8;
    return (a+(((c-a)*fy)>>8))*4;
}
static void line(void *r,int x,int y,int xx,int yy,int w,const uint8_t *c)
{
    op1_pt a={x,y},b={xx,yy};
    if(x==xx&&y==yy)return;
    op1_raster_line(r,&a,&b,w,c);
}
/* Clip at 1/64 pixel precision with bounded int32 products. Avoid the R1
 * software 64-bit division helper for every clipped edge. Raster uses Q10. */
static int region(int x,int y)
{
    return (x<PS_LEFT*64?1:x>PS_RIGHT*64?2:0)|
           (y<PS_TOP*64?4:y>PS_WATER_BOTTOM*64?8:0);
}
static void waterline(void *r,op1_pt a,op1_pt b,const uint8_t *c)
{
    a.x>>=4;a.y>>=4;b.x>>=4;b.y>>=4;
    for(int tries=0;tries<5;tries++){
        int ca=region(a.x,a.y),cb=region(b.x,b.y),v=ca?ca:cb;
        if(!(ca|cb)){line(r,a.x*16,a.y*16,b.x*16,b.y*16,PS_MESH_STROKE,c);return;}
        if(ca&cb)return;
        int x,y;
        if(v&12){
            y=(v&4?PS_TOP:PS_WATER_BOTTOM)*64;
            if(a.y==b.y)return;
            x=a.x+(b.x-a.x)*(y-a.y)/(b.y-a.y);
        }else{
            x=(v&1?PS_LEFT:PS_RIGHT)*64;
            if(a.x==b.x)return;
            y=a.y+(b.y-a.y)*(x-a.x)/(b.x-a.x);
        }
        if(ca){a.x=x;a.y=y;}else{b.x=x;b.y=y;}
    }
}
#include "vapora_suns.h"
#include "vapora_particles.h"
#include "vapora_wave_modes.h"
static void init(void *scr)
{
    sun_init(scr);footer_shift_previous=shift_held();
    int e=0;
    for(int row=0;row<PS_ROWS;row++)for(int col=0;col<PS_COLS;col++){
        int i=row*PS_COLS+col;
        if(col<PS_COLS-1){edges[e][0]=i;edges[e++][1]=i+1;}
        if(row<PS_ROWS-1){edges[e][0]=i;edges[e++][1]=i+PS_COLS;}
        if(row<PS_ROWS-1&&col<PS_COLS-1){
            int flip=col<2?0:col>=PS_COLS-3?1:((row+col)&1);
            edges[e][0]=i+flip;
            edges[e++][1]=i+PS_COLS+1-flip;
        }
    }
    view_slot=-1; control_ready=0; q(4,0); ready=1; clock_last=cycles(); elapsed=25000000u;
}
static void reset_view(int slot,int selector)
{
    view_slot=slot; view_selector=selector;
    for(int i=0;i<PS_VOICES;i++){
        sequence[i]=ps_mail[i].sequence; /* discard all pre-entry telemetry */
        note_serial[i]=ps_mail[i].snapshot.serial;
    }
    impacts_reset();
}
/* Move one side vertex as far as adjacent triangles allow. Q6 areas fit
 * int32; retain an area margin so shared edges cannot cross at high Morph. */
static void move_side_vertex(int index,int requested)
{
    int row=index/PS_COLS,col=index%PS_COLS,low=-32*64,high=32*64;
    for(int rr=row-1;rr<=row;rr++)for(int cc=col-1;cc<=col;cc++){
        if(rr<0||rr>=PS_ROWS-1||cc<0||cc>=PS_COLS-1)continue;
        int a=rr*PS_COLS+cc,b=a+1,c=a+PS_COLS,d=c+1;
        int flip=cc<2?0:cc>=PS_COLS-3?1:((rr+cc)&1);
        int triangles[2][3]={{a,b,flip?c:d},{flip?b:a,d,c}};
        for(int t=0;t<2;t++)for(int v=0;v<3;v++)if(triangles[t][v]==index){
            op1_pt p=vertices[index],q=vertices[triangles[t][(v+1)%3]],r=vertices[triangles[t][(v+2)%3]];
            int px=p.x>>4,py=p.y>>4,qx=q.x>>4,qy=q.y>>4,rx=r.x>>4,ry=r.y>>4;
            int area=(qx-px)*(ry-py)-(qy-py)*(rx-px),slope=rx-qx;
            int margin=area/8+64;
            if(slope>0){int bound=(margin-area)/slope+1;if(bound>low)low=bound;}
            if(slope<0){int bound=(area-margin)/(-slope)-1;if(bound<high)high=bound;}
        }
    }
    if(low<=high)vertices[index].y+=ps_clamp(requested/16,low,high)*16;
}
static void geometry(const ps_params *p)
{
    wave_mode=ps_clamp(p->character,0,PS_WAVE_MODES-1);
    uint32_t phase=ocean_phase,quiet=idle_phase>>21;
    int shoulder_extra[PS_ROWS*2];
    /* Keep default ocean spacing; Thickness owns signature-star rotation. */
    int32_t morph=p->q[PS_MORPH], motion=16384+(12500>>1), thick=26000;
    uint32_t space=0x10000000u+(uint32_t)thick*4096u;
    uint32_t column_step=(space*4u)/5u; /* 8/10 original columns; bounded uint32 */
    for(int row=0;row<PS_ROWS;row++)for(int col=0;col<PS_COLS;col++){
        int i=row*PS_COLS+col;
        unsigned cx=(unsigned)col*112u;
        uint32_t pos=(uint32_t)col*column_step+(uint32_t)row*0x15000000u;
        uint32_t drift=phase;
        int height;
        if(!wave_mode){
            int base=rounded_wave(pos)>>5;
            int broad=base+((((rounded_wave(pos+drift)>>5)-base)*motion)>>15);
            base=rounded_wave(pos*2)>>5;
            int ridge=base+((((rounded_wave(pos*2+drift)>>5)-base)*motion)>>15);
            int swell=broad+(((ridge-broad)*morph)>>15);
            uint32_t paired=pos; /* Interval now selects the star field. */
            int pairbase=rounded_wave(paired)>>6;
            int pair=pairbase+((((rounded_wave(paired-drift)>>6)-pairbase)*motion)>>15);
            swell+=(pair*thick)>>15;
            int noise=coherent(cx+(phase>>21),row*170+(phase>>21));
            swell+=(noise*motion)>>15;
            /* Mode 01 retains the prior ambient surface for comparison. */
            int idle=coherent(cx+quiet,row*170+quiet);
            height=(idle*(row+3)*3)>>2;
            int activity=(motion*motion)>>15;
            int waveamp=(activity*(row+3))>>13;
            height+=swell*waveamp;
        }else{
            int amplitude=2048+((morph*14336)>>15);
            height=(wave_field(wave_mode,col,row,phase)*amplitude)>>10;
            height=height*(row+5)/8;
        }
        height=ps_clamp(height,-24*1024,24*1024);
        if(wave_mode&&(col==1||col==PS_COLS-2))height=ps_clamp(height,-12*1024,12*1024);
        height=(height*ps_response[col])>>15;
        int side=col<PS_COLS/2?col:PS_COLS-1-col;
        if(row==0&&side==2)height=height*4/3;
        int x=160*1024+((2*col-(PS_COLS-1))*ps_width[row]*512)/(PS_COLS-1);
        int y=ps_depth[row]*1024-height;
        if(side<2){
            int edge_x=side?ps_shoulder_x[row]:ps_side_x[row];
            x=(col<PS_COLS/2?edge_x:320-edge_x-(side?0:8))*1024;
            /* Start from the proven side silhouette, then free the lower
             * shoulders to follow 100% of the swell within triangle limits. */
            int response=row==0?(side?5243:655):side?(row==1?2621:8192):1311;
            if(side&&row==PS_ROWS-1&&height<0)response=2730;
            y=(side?ps_shoulder_y[row]:ps_side_y[row])*1024-
                (height*response)/(ps_response[col]);
            if(side&&row>=2)shoulder_extra[row*2+(col>PS_COLS/2)]=
                -(height*(32768-response))/ps_response[col];
        }else{
            x+=((((rounded_wave(pos)>>7)*(thick>>7))>>6)*ps_response[col])>>15;
        }
        if(row==0&&side==2)y-=3*1024;
        vertices[i].x=x;vertices[i].y=y;
        if((side==2||(wave_mode&&side>2))&&row){
            int min_y=vertices[i-PS_COLS].y+6*1024;
            if(vertices[i].y<min_y)vertices[i].y=min_y;
        }
    }
    for(int row=2;row<PS_ROWS;row++)for(int right=0;right<2;right++)
        move_side_vertex(row*PS_COLS+(right?PS_COLS-2:1),shoulder_extra[row*2+right]);
    /* Deform shared vertices; never bend individual edges from audio traces. */
    int offsets[PS_ROWS*PS_COLS];
    for(int row=1;row<PS_ROWS-1;row++)for(int col=2;col<PS_COLS-2;col++){
        int i=row*PS_COLS+col;
        int up=(vertices[i].y-vertices[i-PS_COLS].y)/2-1024;
        int down=(vertices[i+PS_COLS].y-vertices[i].y)/2-1024;
        offsets[i]=ps_clamp(impact_displacement(col,row),-(up>0?up:0),down>0?down:0);
    }
    for(int row=1;row<PS_ROWS-1;row++)for(int col=2;col<PS_COLS-2;col++){
        int i=row*PS_COLS+col;
        if(col==2||col==PS_COLS-3)move_side_vertex(i,offsets[i]);
        else vertices[i].y+=offsets[i];
    }
}

#ifdef VAPORA_HOST
void ps_test_audition_reset(int mode)
{
    wave_mode=mode;ocean_phase=idle_phase=particle_phase=particle_slow_phase=sun_phase=0;
    impact_rng=0x7472616du;impact_count=0;impacts_reset();active_knob=active_sun=0;
}
int ps_test_footer(void){return footer_visible;}
void ps_test_footer_set(int visible){footer_visible=visible;}
int ps_test_wave_mode(void){return wave_mode;}
void ps_test_wave_mode_set(int mode){wave_mode=mode;impacts_reset();}
uint32_t ps_test_impact_count(void){return impact_count;}
void ps_test_impact_last(op1_pt *p)
{int i=(impact_next+PS_IMPACTS-1)%PS_IMPACTS;p->x=impacts[i].x;p->y=impacts[i].y;}
void ps_test_impact_tick(uint32_t step){impacts_tick(step);}
int ps_test_impact_profile(int offset,uint32_t extension)
{return impact_profile(offset,12288,extension);}
void ps_test_impact_last_state(uint32_t *age,uint32_t *extension)
{int i=(impact_next+PS_IMPACTS-1)%PS_IMPACTS;*age=impacts[i].age;*extension=impacts[i].extension;}
void ps_test_geometry_phase(uint32_t ocean,uint32_t idle)
{ocean_phase=ocean;idle_phase=idle;}
/* Inspect production geometry at a fixed phase, without advancing the clock. */
void ps_test_geometry(const ps_params *p,op1_pt *points,int16_t *curves)
{
    geometry(p);
    for(int k=0;k<PS_ROWS*PS_COLS;k++)points[k]=vertices[k];
    for(int k=0;k<PS_EDGES;k++)curves[k]=0;
}
#endif
/* Raw words detect physical edits; live modulation does not steal the readout.
 * Synth API 3.0.2 has page toggles but no press callback. Consume white's
 * page flag at the next GUI update, before reading any parameter values. */
static void track_controls(const int16_t *raw,int new_view,int white_press)
{
    if(new_view||!control_ready){
        q(4,0);
        active_knob=0;active_sun=0;
        for(int k=0;k<4;k++)if(k!=2&&op1_eng_page[k])active_knob=k;
        for(int k=0;k<8;k++)control_words[k]=raw[k];
        for(int k=0;k<4;k++)control_pages[k]=op1_eng_page[k];
        control_ready=1;
    }else{
        for(int k=0;k<4;k++){
            int word=k==0?5:k==1?6:7;
            if(raw[k]!=control_words[k]||(k!=2&&raw[word]!=control_words[word])){
                q(4,0);active_knob=k;active_sun=0;
            }
            if(k!=2&&op1_eng_page[k]!=control_pages[k]){
                q(k,op1_eng_page[k]);active_knob=k;active_sun=0;
            }
        }
        for(int k=0;k<8;k++)control_words[k]=raw[k];
        for(int k=0;k<4;k++)control_pages[k]=op1_eng_page[k];
    }
    if(white_press){q(2,1);sun_design=(sun_design+1)%PS_SUN_DESIGNS;active_knob=2;active_sun=1;}
}
#ifdef VAPORA_HOST
void ps_test_draw_sun_at(int cy,int design,int opening)
{sun_color(0,0);sun_prepare(cy,design,opening,0);op1_screen_draw(0,0);draw_sun(op1_raster(),cy,design,opening);}
void ps_test_draw_sun(int cy,int design){ps_test_draw_sun_at(cy,design,(56-cy)*32767/26);}
#endif
static unsigned dial_phase(int q)
{return 12u*1024u+(unsigned)ps_clamp(q,0,32767)*24u*1024u/32767u;}
static void controls(void *r,const ps_params *p,const int16_t *raw)
{
    static const char *const normal[4]={"MORPH","THICKNESS","BRIGHTNESS","MOTION"};
    int k=active_knob,q=p->q[k];
    char value[32],caption[48],*o=value;const char *label=normal[k],*s=0;
    if(active_sun){
        label="SUN";o=sq_num(o,(unsigned)ps_sun_ids[sun_design],2);o=sq_cat(o," ");sq_cat(o,ps_sun_names[sun_design]);
        q=sun_design*32767/(PS_SUN_DESIGNS-1);
    }else if(k==0&&op1_eng_page[0]){
        label="CHARACTER";o=sq_num(o,(unsigned)p->character+1,2);o=sq_cat(o," ");
        sq_cat(o,ps_character_names[p->character]);
        q=(ps_secondary(raw[5])-1)*32767/32766;
    }
    else if(k==1&&op1_eng_page[k]){label="INTERVAL";s=ps_interval_names[p->interval];q=p->interval*32767/(PS_INTERVALS-1);}
    else if(k==3&&op1_eng_page[k]){
        label="SPEED";q=p->q[PS_SPEED];int hz=ps_speed_millihz(q);
        o=sq_num(o,(unsigned)(hz/1000),1);*o++='.';
        o=sq_num(o,(unsigned)(hz%1000/10),2);sq_cat(o," HZ");
    }else {o=sq_num(o,(unsigned)((q*100+16383)/32767),1);sq_cat(o,"%");}
    if(s)sq_cat(value,s);
    o=sq_cat(caption,label);o=sq_cat(o,"  ");sq_cat(o,value);
    for(int n=0;caption[n];n++)if(caption[n]>='A'&&caption[n]<='Z')caption[n]+='a'-'A';
    /* The stock font uses lowercase codes for letters; uppercase codes are icons. */
    static const int32_t identity[9]={1024,0,0,0,1024,0,0,0,1024};
    if(!text_made[k]){op1_text_ctor(text_objects[k],34*1024,PS_LABEL_Y,PS_TEXT_SIZE,k<2?ps_text_colors[k]:ps_colors[k]);text_made[k]=1;}
    int different=0,i=0;
    do {if(text_cache[k][i]!=caption[i])different=1;i++;}while(caption[i-1]&&i<48);
    if(different){for(i=0;i<48;i++){text_cache[k][i]=caption[i];if(!caption[i])break;}op1_text_set(text_objects[k],text_cache[k]);}
    op1_text_draw(text_objects[k],r,identity);
    /* FMbell-style 270-degree dial, coloured ring and moving indicator. */
    for(int j=4;j<28;j++){
        int a=(j+8)&31,b=(j+9)&31;
        line(r,16*1024+ps_circle[a][0]*10,PS_DIAL_Y*1024+ps_circle[a][1]*10,
             16*1024+ps_circle[b][0]*10,PS_DIAL_Y*1024+ps_circle[b][1]*10,PS_MESH_STROKE,ps_colors[k]);
    }
    unsigned phase=dial_phase(q);
    int a=(phase>>10)&31,b=(a+1)&31,f=phase&1023;
    int dx=ps_circle[a][0]+((ps_circle[b][0]-ps_circle[a][0])*f>>10);
    int dy=ps_circle[a][1]+((ps_circle[b][1]-ps_circle[a][1])*f>>10);
    line(r,16*1024+dx*3,PS_DIAL_Y*1024+dy*3,16*1024+dx*8,PS_DIAL_Y*1024+dy*8,PS_MESH_STROKE,ps_colors[k]);
}
#ifdef VAPORA_HOST
int ps_test_secret(void){return z==2;}
uint32_t ps_test_particle_phase(void){return particle_phase;}
uint32_t ps_test_particle_slow_phase(void){return particle_slow_phase;}
uint32_t ps_test_ocean_phase(void){return ocean_phase;}
uint32_t ps_test_sun_phase(void){return sun_phase;}
void ps_test_sun_color(uint32_t phase,int depth,uint8_t *out)
{sun_color(phase,depth);for(int k=0;k<4;k++)out[k]=ps_sun_color[k];}
void ps_test_sun_frame(int design,uint32_t phase,int depth)
{sun_color(phase,depth);sun_prepare(30,design,32767,0);op1_screen_draw(0,0);draw_sun(op1_raster(),30,design,32767);}
void ps_test_particle_frame(int interval,uint32_t phase)
{
    ps_params p={0};p.interval=interval;
    uint32_t old_phase=particle_phase,old_slow=particle_slow_phase;int old_sun=sun_design;
    particle_phase=particle_slow_phase=phase;sun_design=2; /* Halo: no solid disk exclusion. */
    draw_particles(op1_raster(),&p,56);
    particle_phase=old_phase;particle_slow_phase=old_slow;sun_design=old_sun;
}
void ps_test_small_solid_frame(int interval,uint32_t phase)
{
    ps_params p={0};p.interval=interval;p.q[PS_BRIGHT]=32767;
    uint32_t old_phase=particle_phase,old_slow=particle_slow_phase;int old_sun=sun_design;
    particle_phase=particle_slow_phase=phase;sun_design=0;draw_particles(op1_raster(),&p,36);
    particle_phase=old_phase;particle_slow_phase=old_slow;sun_design=old_sun;
}
void ps_test_logo_vertex(int interval,int thick,op1_pt *out)
{*out=star_logo_vertex(&ps_star_logos[interval],0,0,0,(unsigned)thick*32768u/32767u,thick);}
void ps_test_signature_path(int interval,uint32_t progress)
{particle_phase=progress-star_logo_seed(interval);}
void ps_test_signature_position(int interval,uint32_t progress,op1_pt *out)
{
    uint32_t old=particle_phase;particle_phase=progress-star_logo_seed(interval);
    int x,y;star_logo_position(interval,&x,&y);out->x=x;out->y=y;particle_phase=old;
}
void ps_test_signature_icon(int interval)
{
    uint32_t old=particle_phase;int design=sun_design;sun_design=2;
    ps_params p={0};p.interval=interval;p.q[PS_THICK]=32767;p.q[PS_BRIGHT]=32767;
    particle_phase=0x40000000u-star_logo_seed(interval);draw_star_logo(op1_raster(),&p,36);
    particle_phase=old;sun_design=design;ps_host_particle_layer=-1;
}
int ps_test_sun_design(void){return sun_design;}
int ps_test_active_control(void){return active_sun?4:active_knob;}
void ps_test_character_dial(int16_t word)
{
    int knob=active_knob,sun=active_sun,page=op1_eng_page[0];
    int16_t raw[8]={0};raw[5]=word;ps_params p=ps_decode(raw);
    active_knob=0;active_sun=0;op1_eng_page[0]=1;
    controls(op1_raster(),&p,raw);
    active_knob=knob;active_sun=sun;op1_eng_page[0]=page;
}
#endif
static void update(void *scr,int dt)
{
#if defined(OP1_HOST) && !defined(VAPORA_HOST)
    preview_cycles+=(uint32_t)(dt>0?dt:16)*400000u;
#endif
    if(!ready)init(scr);
    int white_press=op1_eng_page[2]!=0;
    op1_eng_page[2]=0; /* acknowledge one-shot; white turns return to Brightness */
    int slot=OP1_SCREEN_SLOT&7;
    const int16_t *raw=op1_synth_knobs(slot);
    track_controls(raw,view_slot!=slot||view_selector!=raw[4],white_press);
    int16_t words[8];for(int i=0;i<8;i++)words[i]=raw[i];
    op1_synth_knobs_live(slot,words);
    ps_params p=ps_decode(words);
    if(view_slot!=slot||view_selector!=words[4])reset_view(slot,words[4]);
    wave_mode=p.character;
    uint32_t now=cycles(),delta=now-clock_last;clock_last=now;
    int shift=shift_held();
    if(delta<=100000000u&&shift&&!footer_shift_previous)footer_visible^=1;
    footer_shift_previous=shift;
    int secret=j(now,delta),tick=0;
    if(delta>100000000u){
        impacts_reset();
        for(int i=0;i<PS_VOICES;i++){sequence[i]=ps_mail[i].sequence;note_serial[i]=ps_mail[i].snapshot.serial;}
        delta=100000000u;
    }
    sun_phase+=(uint32_t)(((uint64_t)delta*ps_speed_millihz(p.q[PS_SPEED])*10737u)/1000000u);
    sun_color(sun_phase,p.q[PS_MOTION]);
    elapsed+=delta;
    if(elapsed>=25000000u){ /* 16 geometry updates/sec at the core's 400 MHz */
        uint32_t step=elapsed;elapsed=0;tick=1;
        idle_phase+=(uint32_t)(((uint64_t)step*429u)/1000u); /* 0.040 Hz */
        ocean_phase+=(uint32_t)(((uint64_t)step*ps_visual_speed_millihz(8192)*10737u)/1000000u);
        /* High sun: half the former daytime drift; nighttime speed unchanged. */
        int night=32767-p.q[PS_BRIGHT];
        uint32_t drift=(uint32_t)(((uint64_t)step*(160u+((unsigned)night*320u>>15)))/1000u);
        particle_phase+=drift;
        /* Separate clock stays continuous when either layer wraps. */
        particle_slow_phase+=(uint32_t)((uint64_t)drift*3u/8u);
        if(!secret){impacts_tick(step);geometry(&p);}else impacts_reset();
    }
    int cy=ps_sun_y(p.q[PS_BRIGHT]);
    sun_prepare(cy,sun_design,p.q[PS_BRIGHT],secret);
    op1_screen_draw(scr,dt);
    void *r=op1_raster();if(!r)return;
    if(secret){v(r,tick);return;}
    draw_particles(r,&p,cy);
    draw_sun(r,cy,sun_design,p.q[PS_BRIGHT]);
    for(int e=0;e<PS_EDGES;e++){
        int row=edges[e][0]/PS_COLS,crest=vertices[edges[e][0]].y<ps_depth[row]*1024;
        int b=p.q[PS_BRIGHT]>>8;
        uint8_t color[4]={(uint8_t)(8+crest*(15+(b>>2))),
            (uint8_t)(70+row*10+(b>>1)+crest*35),
            (uint8_t)(165+row*7+(b>>2)),255};
        op1_pt a=vertices[edges[e][0]],z=vertices[edges[e][1]];
        waterline(r,a,z,color);
    }
    if(footer_visible){
        static const uint8_t black[4]={0,0,0,255};
        /* The readout owns its backing only while visible. */
        for(int y=PS_BOTTOM+1;y<=152;y++)line(r,0,y*1024,319*1024,y*1024,1024,black);
        controls(r,&p,raw);
    }
}
const op1_screen_t SCREEN_vapora={init,update};
