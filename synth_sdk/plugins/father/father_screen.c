/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with OP-1 firmware permission.
 * Original vector reconstruction from the supplied Father demonstration video.
 * No original DSP, firmware artwork or font is included. See gen_svg.py. */
#include "op1_synth.h"
#include "father_geom.h"
/* Total OLED lift, shared by live geometry and native text. */
#define SCREEN_LIFT 12
static unsigned text_slot;
static void *gate_person;
extern volatile int16_t father_gate[3];
static uint8_t text_objects[5][OP1_TEXT_SIZE] __attribute__((aligned(4)));
static uint8_t made[5];static char cached[5][16];
static const uint8_t black[4]={1,1,1,255};
static void line(void *r,int x,int y,int xx,int yy,int width,const uint8_t *c){
 y-=SCREEN_LIFT*1024;yy-=SCREEN_LIFT*1024;
 /* Intentional viewport clipping for the unobserved near-distance extrapolation. */
 if((y<1024&&yy<1024)||(y>154*1024&&yy>154*1024))return;
 if(y<1024||y>154*1024){int bound=y<1024?1024:154*1024;x+=(int)((int64_t)(xx-x)*(bound-y)/(yy-y));y=bound;}
 if(yy<1024||yy>154*1024){int bound=yy<1024?1024:154*1024;xx+=(int)((int64_t)(x-xx)*(bound-yy)/(y-yy));yy=bound;}
 op1_pt a={x,y},b={xx,yy};op1_raster_line(r,&a,&b,width,c);
}
static void text(void *r,int x,int y,int size,const char *s,const uint8_t *c){
 unsigned n=text_slot++;if(n>=5)return;
 static const int32_t ident[9]={1024,0,0,0,1024,0,0,0,1024};
 if(!made[n]){op1_text_ctor(text_objects[n],OP1_Q10(x),y-SCREEN_LIFT,size,c);made[n]=1;}
 unsigned j=0;while(j<15&&s[j]&&cached[n][j]==s[j])j++;
 if(cached[n][j]!=s[j]){op1_text_set(text_objects[n],s);for(j=0;j<15&&s[j];j++)cached[n][j]=s[j];cached[n][j]=0;}
 op1_text_draw(text_objects[n],r,ident);
}
static void init(void *s){
 gate_person=op1_find_node(s,"gate_person");
 void *face=op1_find_node(s,"face");if(face)op1_node_set_visible(face,0);

}
static int scale_coord(int v,int center,int scale){return center*1024+((v-center*256)*scale>>8);}
static void update(void *s,int dt){
 int16_t k[4];op1_fx_knobs_live(k);
 /* Green moves the person with shorter gate time; the door follows the wet envelope. */
 int position=(int32_t)k[1]*12*1024/32767;
 if(gate_person){
  const int32_t transform[9]={1024,0,0,0,1024,0,position,0,1024};
  op1_node_set_transform(gate_person,transform);
 }
 uint8_t cyan[4];const uint8_t dark[3]={45,80,95},reference[3]={98,169,196},bright[3]={200,225,240};
 for(unsigned i=0;i<3;i++)cyan[i]=(uint8_t)(k[2]<=18022?
  dark[i]+(reference[i]-dark[i])*k[2]/18022:
  reference[i]+(bright[i]-reference[i])*(k[2]-18022)/14745);
 cyan[3]=255;
 unsigned value=((unsigned)k[0]*99u+16383u)>>15;
 /* Measured49/80/99 footprint ratios1/.63/.40 about fixed vanishing anchor156,94. */
 int scale=1626-(int)(value*1234u/100u);if(scale<404)scale=404;
 op1_screen_draw(s,dt);void *r=op1_raster();
 /* The lossless reference shows the grid through head/body; only lenses occlude it. */
 for(unsigned lens=3;lens<=4;lens++){
  const father_path *path=&father_paths[lens];int low=160*1024,high=0;
  for(unsigned i=0;i<path->count;i++){int y=scale_coord(path->p[i].y,94,scale);if(y<low)low=y;if(y>high)high=y;}
  for(int y=((low+1023)/1024)*1024;y<high;y+=1024){
   int cuts[16];unsigned count=0;
   for(unsigned i=0;i<path->count;i++){
    unsigned j=(i+1)%path->count;
    int ay=scale_coord(path->p[i].y,94,scale),by=scale_coord(path->p[j].y,94,scale);
    if((ay<=y&&y<by)||(by<=y&&y<ay)){
     int ax=scale_coord(path->p[i].x,156,scale),bx=scale_coord(path->p[j].x,156,scale);
     if(count<16)cuts[count++]=ax+(int)((int64_t)(bx-ax)*(y-ay)/(by-ay));
    }
   }
   for(unsigned i=1;i<count;i++){int v=cuts[i];unsigned j=i;while(j&&cuts[j-1]>v){cuts[j]=cuts[j-1];j--;}cuts[j]=v;}
   for(unsigned i=0;i+1<count;i+=2)line(r,cuts[i],y,cuts[i+1],y,1152,black);
  }
 }
 for(unsigned p=0;p<sizeof(father_paths)/sizeof(father_paths[0]);p++){
  const father_path *path=&father_paths[p];
  for(unsigned i=1;i<path->count;i++)line(r,scale_coord(path->p[i-1].x,156,scale),scale_coord(path->p[i-1].y,94,scale),scale_coord(path->p[i].x,156,scale),scale_coord(path->p[i].y,94,scale),1024,cyan);
  if(p==0)line(r,scale_coord(path->p[path->count-1].x,156,scale),scale_coord(path->p[path->count-1].y,94,scale),scale_coord(path->p[0].x,156,scale),scale_coord(path->p[0].y,94,scale),1024,cyan);
 }
 const uint8_t blue[4]={107,142,255,255},green[4]={0,239,148,255},white[4]={255,255,255,255},red[4]={255,58,93,255};
 char number[3]={(char)('0'+value/10),(char)('0'+value%10),0};text_slot=0;
 text(r,14,23,12,"distance",blue);text(r,14,39,30,number,white);
 text(r,14,138,12,"gate",green);text(r,260,138,12,"color",white);text(r,284,23,12,"mix",red);
 int height=((int)k[3]*46+16383)>>15;
 const uint8_t dim[4]={80,21,30,255};line(r,302*1024,48*1024,302*1024,94*1024,9*1024,dim);
 if(height)line(r,302*1024,(94-height)*1024,302*1024,94*1024,9*1024,red);
 /* Gate posts close around the small green person as the wet tail shuts. */
 if(k[1]){
  int x=104*1024+position;
  int closed=32767-father_gate[(unsigned)op1_fx_screen_pos()%3];
  int width=closed*10*1024/32767;
  line(r,x-12*1024,84*1024,x-12*1024,118*1024,1024,green);
  line(r,x+12*1024,84*1024,x+12*1024,118*1024,1024,green);
  line(r,x-12*1024,84*1024,x+12*1024,84*1024,1024,green);
  for(int y=88;y<=116;y+=7){
   line(r,x-12*1024,y*1024,x-12*1024+width,y*1024,1024,green);
   line(r,x+12*1024-width,y*1024,x+12*1024,y*1024,1024,green);
  }
 }
 /* The native stock FX overlay owns bypass dimming and its central FX OFF label. */
}
const op1_screen_t SCREEN_father={init,update};
