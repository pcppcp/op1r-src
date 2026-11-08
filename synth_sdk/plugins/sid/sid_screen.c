/* Owner hardware polish: artwork and native readouts move up four screen pixels. */
#include "op1_synth.h"
#include "sid_dsp.h"
static uint8_t texts[8][OP1_TEXT_SIZE] __attribute__((aligned(4)));
static uint8_t made[8];static char cache[8][16];static unsigned slot;
static const uint8_t colors[4][4]={{100,151,235,255},{2,209,160,255},{221,221,221,255},{235,69,75,255}};
static void text(void *r,int x,int y,const char *s,const uint8_t *c){
 unsigned n=slot++;if(n>=8)return;static const int32_t identity[9]={1024,0,0,0,1024,0,0,0,1024};
 if(!made[n]){op1_text_ctor(texts[n],x*1024,y-4,12,c);made[n]=1;}
 unsigned i=0;while(s[i]&&i<15&&cache[n][i]==s[i])i++;
 if(s[i]||cache[n][i]){i=0;do{cache[n][i]=s[i];}while(s[i]&&++i<15);cache[n][15]=0;op1_text_set(texts[n],cache[n]);}
 op1_text_draw(texts[n],r,identity);
}
static void number(void *r,int x,int y,unsigned n,const uint8_t *c){
 char b[12],rev[12];unsigned k=0,i=0;do{rev[k++]=(char)('0'+n%10);n/=10;}while(n);while(k)b[i++]=rev[--k];b[i]=0;text(r,x,y,b,c);
}
static void init(void *scr){(void)scr;}
static void update(void *scr,int dt){
 int16_t q[4];op1_synth_knobs_live(OP1_SCREEN_SLOT,q);slot=0;op1_screen_draw(scr,dt);void *r=op1_raster();
 unsigned mode=sid_wave_mode(q[0]);
 static const char *names[6]={"triangle","saw","pulse","noise","sync","ring"};
 text(r,238,18,"wave",colors[0]);text(r,238,32,names[mode],colors[0]);
 text(r,238,53,mode>=4?"interval":mode==2?"width":"voices",colors[1]);if(mode>=4){unsigned n=sid_interval_semi(q[1]);char b[7],*o=b;*o++='+';if(n>=10)*o++=(char)('0'+n/10);*o++=(char)('0'+n%10);*o++=' ';*o++='s';*o++='t';*o=0;text(r,238,67,b,colors[1]);}
 else number(r,238,67,mode==2?3+(((unsigned)q[1]*94+16383)>>15):(((unsigned)q[1]*99+16383)>>15),colors[1]);
 text(r,238,88,"cutoff",colors[2]);number(r,238,102,((unsigned)q[2]*99+16383)>>15,colors[2]);
 text(r,238,123,"res",colors[3]);number(r,238,137,((unsigned)q[3]*99+16383)>>15,colors[3]);
}
const op1_screen_t SCREEN_sid={init,update};
