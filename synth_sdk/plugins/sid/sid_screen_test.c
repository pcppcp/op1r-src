#define OP1_HOST
#include "../../op1_synth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int16_t host_knobs[8][8];int host_slot;static int first=1;
typedef struct{int x,y,size;uint8_t color[4];char str[32];} txt;
void *host_text_ctor(void *obj,int32_t x,int32_t y,int32_t size,const uint8_t *c){txt *t=obj;t->x=x;t->y=y;t->size=size;memcpy(t->color,c,4);return obj;}
void host_text_set(void *obj,const char *s){snprintf(((txt*)obj)->str,32,"%s",s);}
void host_text_draw(void *obj,void *r,const int32_t *m){(void)r;(void)m;txt *t=obj;printf("%s[\"text\",%d,%d,%d,0,[%u,%u,%u,%u],\"%s\"]",first?"":",",t->x,t->y,t->size,t->color[0],t->color[1],t->color[2],t->color[3],t->str);first=0;}
void host_screen_draw(void *s,int dt){(void)s;(void)dt;}
void *host_raster(void){return 0;}
#include "sid_screen.c"
int main(int argc,char **argv){for(int i=0;i<4;i++)host_knobs[0][i]=argc>i+1?atoi(argv[i+1]):16384;puts("{\"ops\":[");SCREEN_sid.init(0);SCREEN_sid.update(0,16);puts("]}");return 0;}
