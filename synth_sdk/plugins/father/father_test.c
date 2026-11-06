/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission. */
#include "father_dsp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
static mw_state s,t;
void mw_ref_init(mw_state *);
void mw_ref_process(mw_state *,int16_t *,int16_t *,const int32_t *);
static void impulse(const int32_t q[4],int stereo,int16_t *out,unsigned blocks){
 mw_init(&s);for(unsigned k=0;k<blocks;k++){
  int16_t l[64]={0},r[64]={0};if(!k)l[0]=30000;
  mw_process(&s,l,stereo?r:0,q);
  if(out)for(unsigned i=0;i<64;i++){out[(k*64+i)*2]=l[i];out[(k*64+i)*2+1]=stereo?r[i]:l[i];}
 }
}
int main(int argc,char **argv){
 int32_t q[4]={16384,0,18022,32767};
 /* Exact dry endpoint, including independent right samples. */
 mw_init(&s);q[3]=0;int16_t l[64],r[64],a[64],b[64];
 for(unsigned i=0;i<64;i++){a[i]=l[i]=(int16_t)(i*991);b[i]=r[i]=(int16_t)(31000-i*777);}
 mw_process(&s,l,r,q);assert(!memcmp(l,a,sizeof l)&&!memcmp(r,b,sizeof r));
 /* Exact differential check against the plain C reference (father_reference.c): output and the whole state. */
 mw_init(&s);mw_ref_init(&t);unsigned rng=12345;
 double error2=0,signal2=0;int max_error=0;
 for(unsigned k=0;k<6000;k++){
  for(unsigned j=0;j<4;j++)q[j]=(int32_t)((k*(j+17)*193u)&32767);
  for(unsigned i=0;i<64;i++){
   rng=rng*1664525u+1013904223u;l[i]=a[i]=(int16_t)(rng>>16);
   rng=rng*1664525u+1013904223u;r[i]=b[i]=(int16_t)(rng>>16);
  }
  mw_process(&s,l,k&1?r:0,q);mw_ref_process(&t,a,k&1?b:0,q);
  for(unsigned i=0;i<64;i++){
   int dl=l[i]-a[i],dr=r[i]-b[i];if(abs(dl)>max_error)max_error=abs(dl);if(abs(dr)>max_error)max_error=abs(dr);
   error2+=(double)dl*dl+(double)dr*dr;signal2+=(double)a[i]*a[i]+(double)b[i]*b[i];
  }
  assert(MW_SAME_STATE(&s,&t));
 }
 assert(max_error==0&&error2==0);
 printf("Father fullscale automated reference: max_error=%d normalized_rms_error=%.9f relative_rms_error=%.9f\n",max_error,sqrt(error2/(6000.0*128))/32768,sqrt(error2/signal2));
 /* No input cannot create signal, even with changing parameters and modulation. */
 mw_init(&s);for(unsigned k=0;k<4000;k++){
  for(unsigned j=0;j<4;j++)q[j]=(int32_t)((k*(j+17)*193u)&32767);
  memset(l,0,sizeof l);memset(r,0,sizeof r);mw_process(&s,l,r,q);
  for(unsigned i=0;i<64;i++)assert(!l[i]&&!r[i]);
 }
 /* Per-instance state: processing another state cannot change this one. */
 mw_init(&s);t=s;q[0]=20000;q[1]=32767;q[2]=32767;q[3]=32767;
 memset(l,0,sizeof l);memset(r,0,sizeof r);l[0]=32767;mw_process(&s,l,r,q);
 assert(t.hd[0]==0&&t.tank[0]==0);
 /* Every endpoint: full-scale alternating excitation then a long tail; UBSan
    catches arithmetic errors and ASan checks every interpolated/diffusion tap. */
 for(unsigned e=0;e<16;e++){
  for(unsigned j=0;j<4;j++)q[j]=(e&(1u<<j))?32767:0;
  mw_init(&s);for(unsigned k=0;k<5000;k++){
   for(unsigned i=0;i<64;i++){l[i]=k<64?(i&1?32767:-32768):0;r[i]=k<64?(i&2?32767:-32768):0;}
   mw_process(&s,l,r,q);
  }
 }
 if(argc>1){unsigned blocks=4135;int16_t *o=malloc(blocks*64*4);assert(o);
  const char *names[6]={"default","near","far","gated","dark","bright"};
  for(unsigned j=0;j<6;j++){
   q[0]=j==1?0:j==2?32767:16384;q[1]=j==3?8192:0;q[2]=j==4?0:j==5?32767:18022;q[3]=32767;
   impulse(q,1,o,blocks);char path[1024];snprintf(path,sizeof path,"%s/%s.raw",argv[1],names[j]);
   FILE *f=fopen(path,"wb");assert(f);assert(fwrite(o,4,blocks*64,f)==blocks*64);fclose(f);
  }
  for(unsigned take=0;take<3;take++){
   mw_init(&s);mw_ref_init(&t);double chord_error=0,chord_signal=0;int chord_peak=0;
   q[0]=16384;q[1]=8192;q[2]=18022;q[3]=take==0?0:take==1?9830:32767;
   for(unsigned k=0;k<blocks;k++){
    int16_t l[64],r[64];for(unsigned i=0;i<64;i++){
     unsigned n=k*64+i;double time=n/22050.0,local=fmod(time,3.0);
     double amp=local<1.0?exp(-local*5.0):0;
     double a=sin(time*6.283185307179586*130.8128)+.5*sin(time*6.283185307179586*164.8138)+.3*sin(time*6.283185307179586*195.9977);
     double b=sin(time*6.283185307179586*130.8128)+.5*sin(time*6.283185307179586*164.8138+.2)+.3*sin(time*6.283185307179586*195.9977+.4);
     l[i]=(int16_t)(a*amp*9000);r[i]=(int16_t)(b*amp*9000);
    }
    memcpy(a,l,sizeof a);memcpy(b,r,sizeof b);mw_ref_process(&t,a,b,q);mw_process(&s,l,r,q);
    for(unsigned i=0;i<64;i++){
     int dl=l[i]-a[i],dr=r[i]-b[i];if(abs(dl)>chord_peak)chord_peak=abs(dl);if(abs(dr)>chord_peak)chord_peak=abs(dr);
     chord_error+=(double)dl*dl+(double)dr*dr;chord_signal+=(double)a[i]*a[i]+(double)b[i]*b[i];
    }
    for(unsigned i=0;i<64;i++){o[(k*64+i)*2]=l[i];o[(k*64+i)*2+1]=r[i];}
   }
   assert(chord_peak<=128&&sqrt(chord_error/(blocks*128.0))/32768<.0005);
   printf("Father chord take %u: max_error=%d normalized_rms_error=%.9f relative_rms_error=%.9f\n",take,chord_peak,sqrt(chord_error/(blocks*128.0))/32768,chord_signal?sqrt(chord_error/chord_signal):0);
   char path[1024];snprintf(path,sizeof path,"%s/chord-%s.raw",argv[1],take==0?"dry":take==1?"mix":"wet");FILE *f=fopen(path,"wb");assert(f);assert(fwrite(o,4,blocks*64,f)==blocks*64);fclose(f);
  }free(o);
 }
 puts("Father dry/silence/instance/endpoints PASS");return 0;
}
