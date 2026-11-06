/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with OP-1 firmware permission. */
#define OP1_HOST
#include "../../op1_synth.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int16_t host_fx_knobs[3][8];volatile uint32_t father_blocks[3];volatile int16_t father_gate[3]={32767,32767,32767};
int father_test_enabled=1;static int capture,first,sets;
static int32_t person_transform[9];
typedef struct{int x,y,size;uint8_t color[4];char str[32];} txt;
void *host_text_ctor(void *o,int32_t x,int32_t y,int32_t z,const uint8_t *c){txt*t=o;t->x=x;t->y=y;t->size=z;memcpy(t->color,c,4);return o;}
void host_text_set(void *o,const char*s){snprintf(((txt*)o)->str,32,"%s",s);sets++;}
void host_text_draw(void *o,void*r,const int32_t*m){(void)r;(void)m;if(!capture)return;txt*t=o;printf("%s[\"text\",%d,%d,%d,0,[%u,%u,%u,%u],\"%s\"]",first?"":",",t->x,t->y,t->size,t->color[0],t->color[1],t->color[2],t->color[3],t->str);first=0;}
void *host_find_node(void*s,const char*id){(void)s;if(!strcmp(id,"gate_person"))return (void*)2;assert(!strcmp(id,"face"));return (void*)1;}
void host_node_set_transform(void*n,const int32_t*m){assert(n==(void*)2);memcpy(person_transform,m,sizeof person_transform);}
void host_node_set_visible(void*n,int on){assert(n==(void*)1&&!on);}
void host_screen_draw(void*s,int d){(void)s;(void)d;first=1;}
void *host_raster(void){return 0;}
void host_raster_line(void*r,const void*aa,const void*bb,int32_t w,const uint8_t*c){
 (void)r;const op1_pt*a=aa,*b=bb;
 assert(a->x>=0&&a->x<320*1024&&b->x>=0&&b->x<320*1024);
 assert(a->y>=0&&a->y<160*1024&&b->y>=0&&b->y<160*1024);
 if(capture){printf("%s[\"line\",%d,%d,%d,%d,%d,[%u,%u,%u,%u]]",first?"":",",a->x,a->y,b->x,b->y,w,c[0],c[1],c[2],c[3]);first=0;}
}
#include "father_screen.c"
int main(int argc,char**argv){
 int distance=argc>1?atoi(argv[1]):49,off=argc>2?atoi(argv[2]):0;
 host_fx_knobs[0][0]=(int16_t)((distance*32767+49)/99);host_fx_knobs[0][3]=(int16_t)(argc>3?atoi(argv[3]):9830);
 host_fx_knobs[0][1]=(int16_t)(argc>4?atoi(argv[4]):8192);
 host_fx_knobs[0][2]=(int16_t)(argc>5?atoi(argv[5]):18022);
 int frames=argc>6?atoi(argv[6]):6;
 father_test_enabled=!off;SCREEN_father.init(0);for(int f=0;f<frames;f++){if(!off)father_blocks[0]++;capture=f==frames-1;if(capture)printf("{\"hidden\":[\"face\"],\"ops\":[");SCREEN_father.update(0,25);}
 printf("],\"nodes\":{\"gate_person\":[");for(int i=0;i<9;i++)printf("%s%d",i?",":"",person_transform[i]);puts("]}}");assert(sets==5);return 0;
}
