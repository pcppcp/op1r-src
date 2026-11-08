/* Optional Amp input instrument, original design inspired by Field's workflow.
 * GPL-3.0-or-later with OP-1 firmware permission. */
#include "../../op1_input.h"
#include "st_pitch.h"
#include "../../seq/seq_text.h"
#include "amp_dsp.h"
/* R246's PmSynth keeps its frame pointer in M1 across the call this hook replaces (0xFFA02896: P1 = M1), and
 * GCC treats M1 as scratch. Reserve it for this whole file, so the audio path (reset, process and everything they
 * call) never writes it. A linear scan of the caller up to its RTS finds no other I/M/B/L register read first. */
#ifdef __bfin__
register int32_t amp_keep_m1 __asm__("M1");
#endif
#define BARRIER() __asm__ volatile("" ::: "memory")
static amp_dsp dsp;
static struct { int16_t x[ST_PITCH_N];volatile uint32_t state,epoch,rate; } capture[2];
static volatile uint32_t epoch,enabled,result_epoch,display;
static volatile int32_t input_peak,output_peak;
static uint32_t capture_pos,capture_which;
static st_pitch_work work;
static int job=-1;
static int tuning;
static uint32_t job_epoch;
static void reset(int slot,uint32_t rate){
 enabled=slot>=0;epoch++;capture_pos=0;input_peak=output_peak=0;
 /* Only audio writes FREE buffers; never reset a READY/BUSY worker-owned window. */
 amp_dsp_reset(&dsp,rate);
}
static void process(const int16_t *in,int16_t *out,const int16_t *p,uint32_t rate){
 if(dsp.rate!=rate)reset(0,rate);
 amp_dsp_process(&dsp,in,out,p);
 int32_t ip=0,op=0;for(int i=0;i<64;i++){int32_t a=amp_abs(in[i]),b=amp_abs(out[i]);if(a>ip)ip=a;if(b>op)op=b;}
 input_peak=ip;output_peak=op;
 int requested=op1_input_tuner_active();
 if(tuning!=requested){tuning=requested;epoch++;capture_pos=0;}
 if(!tuning)return;
 unsigned b=capture_which;
 if(capture[b].state){b^=1;capture_which=b;capture_pos=0;}
 if(!capture[b].state){
  if(!capture_pos){capture[b].epoch=epoch;capture[b].rate=rate;}
  for(unsigned i=0;i<64;i++)capture[b].x[capture_pos+i]=in[i];
  capture_pos+=64;
  if(capture_pos==ST_PITCH_N){BARRIER();capture[b].state=1;capture_which^=1;capture_pos=0;}
 }
}
static void service(void){
 if(job<0){
  for(int b=0;b<2;b++)if(capture[b].state==1){
   BARRIER();if(!enabled||capture[b].epoch!=epoch){capture[b].state=0;continue;}
   capture[b].state=2;job=b;job_epoch=capture[b].epoch;
   st_pitch_begin_rate(&work,capture[b].x,0,ST_PITCH_N,capture[b].rate);break;
  }
 }
 if(job<0)return;
 if(!st_pitch_step(&work,4)){
  st_pitch_result r;st_pitch_result_get(&work,&r);
  if(enabled&&job_epoch==epoch){display=(r.valid?0x10000u:0)|((uint32_t)(uint8_t)r.cents<<8)|r.note;BARRIER();result_epoch=job_epoch;}
  BARRIER();capture[job].state=0;job=-1;
 }
}
static void voice_on(void *self,const int16_t *p){(void)self;(void)p;}
static void voice_render(void *self,int16_t *out,const int16_t *p){(void)self;(void)p;for(int i=0;i<128;i++)out[i]=0;}
static void voice_off(void *self){(void)self;}
static const uint8_t colors[4][4]={{105,142,255,255},{0,237,149,255},{245,245,245,255},{255,82,75,255}};
static const uint8_t grey[4]={160,166,174,255};
static void *dials[4],*wave,*box_n,*box_h,*icons[AMP_VOICES];
static void init(void *s){
 const char *names[4]={"dial0","dial1","dial2","dial3"};
 for(int i=0;i<4;i++)dials[i]=op1_find_node(s,names[i]);
 wave=op1_find_node(s,"input_wave");box_n=op1_find_node(s,"box_n");box_h=op1_find_node(s,"box_h");
 for(int i=0;i<AMP_VOICES;i++){char id[7]={'i','c','o','n',(char)('0'+i),0};icons[i]=op1_find_node(s,id);}
}
static void text(void *r,int x,int y,int scale,const char *s,const uint8_t *c){sq_text(r,x<<10,y<<10,scale, s,900,c);}
static int32_t text_width(const char *t,int scale){int32_t w=0;for(;*t;t++)w+=(*t=='1'||*t=='.'?4:6)*scale;return w-2*scale;}
static void update(void *s,int dt){
 int16_t k[4];op1_synth_knobs_live(OP1_SCREEN_SLOT,k);
 /* Blue pressed: page 2, blue picks the voice (word 5, saved with the sound). */
 int voice_page=op1_eng_page[0];int16_t vw=op1_synth_knobs(OP1_SCREEN_SLOT)[5];
 if(voice_page)k[0]=vw<0?0:vw;
 for(int i=0;i<4;i++)if(dials[i]){int a=-3072+(k[i]*6144>>15);op1_node_rotate(dials[i],(112+31*i)<<10,43<<10,OP1_SINE_TABLE[a&8191],OP1_SINE_TABLE[(a+2048)&8191]);}
 if(wave){int scale=256+(input_peak*768>>15);int32_t m[9]={1024,0,0,0,scale,0,0,43*(1024-scale),1024};op1_node_set_transform(wave,m);}
 /* Voice box (left, below the cable): only on page 2 (blue pushed), the voice's icon and name; blue steps the voice. */
 int vi=amp_voice_index(vw);
 if(box_n)op1_node_set_visible(box_n,0);if(box_h)op1_node_set_visible(box_h,voice_page);
 for(int i=0;i<AMP_VOICES;i++)if(icons[i])op1_node_set_visible(icons[i],voice_page&&i==vi);
 op1_screen_draw(s,dt);void *r=op1_raster();
 /* Labels only (nothing below y 138, the OLED's safe edge): the dials show the values. */
 const char *labels[4]={voice_page?"VOICE":"VOL","COMP","TONE","DRIVE"};
 for(int i=0;i<4;i++)text(r,103+31*i,26,768,labels[i],colors[i]);
 if(voice_page){const char *nm=amp_voice_names[vi];sq_text(r,(42<<10)-text_width(nm,1024)/2,128<<10,1024,nm,900,colors[0]);}
 text(r,238,12,896,"T2 TUNER",grey);
}
/* Needle-arc tuner (T2): a flattened half ring of ticks from -50 to +50 cents around a pivot at the bottom centre,
 * red flat side, green centre (+-6), blue sharp side, a white needle eased towards the reading once per frame.
 * Layout inside y 14..138 (screen-art.md): title y14..23, ring top y29, pivot ring to y129, end labels to y136. */
#define TU_PX 160
#define TU_PY 124
#define TU_RX 132
#define TU_RY 94
static const uint8_t tu_red[4]={255,58,93,255},tu_green[4]={0,237,149,255},tu_blue[4]={105,142,255,255},
 tu_grey[4]={101,101,121,255},tu_white[4]={245,245,245,255};
static const int8_t tu_ring[9][2]={{4,0},{3,3},{0,4},{-3,3},{-4,0},{-3,-3},{0,-4},{3,-3},{4,0}};   /* pivot ring, r 4 */
static int16_t tu_dir[101][2];   /* per cent: unit direction on the ring, Q14 (x = sin, y = -cos), from the firmware sine table */
static int32_t tu_needle;        /* cents, Q8, eased */
static void tuner_init(void *s){
 (void)s;
 for(int c=-50;c<=50;c++){int a=(c*2048+(c<0?-25:25))/50;tu_dir[c+50][0]=(int16_t)(OP1_SINE_TABLE[a&8191]>>1);tu_dir[c+50][1]=(int16_t)(-(OP1_SINE_TABLE[(a+2048)&8191]>>1));}
}
/* ring point at fraction f (Q10) of the radius along direction d (Q14) -> Q10 screen coordinates */
static int32_t tu_x(const int16_t *d,int32_t f){return (TU_PX<<10)+((((int32_t)d[0]*TU_RX)>>4)*f>>10);}
static int32_t tu_y(const int16_t *d,int32_t f){return (TU_PY<<10)+((((int32_t)d[1]*TU_RY)>>4)*f>>10);}
static void tuner_update(void *s,int dt){
 op1_screen_draw(s,dt);void *r=op1_raster();text(r,18,14,1536,"AMP TUNER",tu_white);
 /* INPUT GAIN (blue turns it, kennel/kn_input.c; per source, not saved with the sound): top right, clear of the ring */
 {char g[12],*o=g;int db=op1_input_gain_db();if(db>0)*o++='+';if(db<0){*o++='-';db=-db;}sq_cat(sq_num(o,(uint32_t)db,1)," DB");
  int32_t x=(302<<10)-text_width(g,1280);
  sq_text(r,x,15<<10,1280,g,1000,tu_white);sq_text(r,x-9*1280-text_width("GAIN",1280),15<<10,1280,"GAIN",1000,tu_blue);}
 for(int c=-50;c<=50;c+=2){
  const int16_t *d=tu_dir[c+50];int big=c%10==0;int32_t in=big?840:922;
  const uint8_t *col=c<-6?tu_red:c>6?tu_blue:tu_green;
  sq_line(r,tu_x(d,in),tu_y(d,in),tu_x(d,1024),tu_y(d,1024),big?2560:1536,col);
 }
 sq_text(r,24<<10,128<<10,1280,"-50",1000,tu_red);sq_text(r,(296<<10)-text_width("+50",1280),128<<10,1280,"+50",1000,tu_blue);
 uint32_t d=display;int valid=enabled&&result_epoch==epoch&&(d&0x10000);int c=valid?(int8_t)(d>>8):0;
 if(c<-50)c=-50;if(c>50)c=50;
 tu_needle+=((c<<8)-tu_needle)>>2;
 /* needle: the per-cent directions interpolated at the eased Q8 position (it eases back to centre without a note) */
 int32_t p=tu_needle+(50<<8);if(p<0)p=0;if(p>100<<8)p=100<<8;int i=p>>8,f=p&255;if(i==100){i=99;f=256;}
 int16_t nd[2];for(int k=0;k<2;k++)nd[k]=(int16_t)(tu_dir[i][k]+(((tu_dir[i+1][k]-tu_dir[i][k])*f)>>8));
 if(valid)sq_line(r,tu_x(nd,62),tu_y(nd,62),tu_x(nd,800),tu_y(nd,800),1280,tu_white);   /* no signal: no needle, grey pivot */
 for(int k=0;k<8;k++)sq_line(r,(TU_PX+tu_ring[k][0])<<10,(TU_PY+tu_ring[k][1])<<10,(TU_PX+tu_ring[k+1][0])<<10,(TU_PY+tu_ring[k+1][1])<<10,1280,valid?tu_white:tu_grey);
 char name[8],cents[12];
 if(valid){
  st_pitch_name(d&255,name);if(name[0]>='a'&&name[0]<='g')name[0]-='a'-'A';
  char *o=cents;*o++=c<0?'-':'+';sq_num(o,c<0?-c:c,1);
  sq_text(r,(160<<10)-text_width(name,4096)/2,56<<10,4096,name,1400,tu_green);
  int32_t w=text_width(cents,1536)+7*1024+text_width("CENTS",1024),x=(160<<10)-w/2;
  /* the readout sits on a black bar, so the needle never runs through its digits */
  static const uint8_t black[4]={0,0,0,255};sq_line(r,x-3*1024,(94<<10)+512,x+w+3*1024,(94<<10)+512,13*1024,black);
  x=sq_text(r,x,90<<10,1536,cents,1000,tu_white);sq_text(r,x+4*1024,(90<<10)+3*1024,1024,"CENTS",900,tu_grey);
 }else{
  sq_text(r,(160<<10)-text_width("--",4096)/2,56<<10,4096,"--",1400,tu_grey);
  sq_text(r,(160<<10)-text_width("PLAY ONE NOTE",1024)/2,92<<10,1024,"PLAY ONE NOTE",900,tu_grey);
 }
}
const op1_screen_t SCREEN_amp={init,update};
static const op1_screen_t tuner={tuner_init,tuner_update};
const op1_input_engine_t ENGINE_amp={{voice_on,voice_render,voice_off,{OP1_PRESS_PAGE_STEPS(AMP_VOICES),0,0,0}},reset,process,service,&tuner};
