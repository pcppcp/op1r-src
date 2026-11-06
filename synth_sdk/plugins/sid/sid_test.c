#include "sid_dsp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 sid_state a,b;int16_t q[4]={0,16384,24576,8192};
 for(int mode=0;mode<6;mode++)for(int pitch=0;pitch<5;pitch++)for(int edge=0;edge<4;edge++){
  q[0]=mode*5462;q[1]=(edge&1)?32767:0;q[2]=(edge&2)?32767:0;q[3]=(edge&1)?32767:0;
  sid_reset(&a);sid_reset(&b);uint32_t inc=(uint32_t)1000000u<<(pitch*2);
  sid_configure(&a,inc,q);sid_configure(&b,inc,q);
  for(int i=0;i<4096;i++){int16_t v=sid_sample(&a);assert(v==sid_sample(&b));for(int j=0;j<3;j++)assert(a.phase[j]<=0xfffffff);}
 }
 sid_reset(&a);sid_reset(&b);q[0]=8192;q[2]=24576;q[3]=8192;sid_configure(&a,42852281,q);
 int nonzero=0;for(int i=0;i<1000;i++)nonzero|=sid_sample(&a)!=0;assert(nonzero);assert(!memcmp(&b,&(sid_state){.noise={0x7fffff,0x7fffff,0x7fffff}},sizeof b));
 for(int mode=0;mode<6;mode++)for(int knob=1;knob<4;knob++){
  int16_t x[4]={mode*5462,12000,20000,10000},y[4];memcpy(y,x,sizeof x);y[knob]=30000;
  sid_reset(&a);sid_reset(&b);sid_configure(&a,42852281,x);sid_configure(&b,42852281,y);
  unsigned changes=0;for(int i=0;i<4096;i++)changes+=sid_sample(&a)!=sid_sample(&b);assert(changes>100);
 }
 for(int mode=0;mode<6;mode++)for(int edge=0;edge<4;edge++){
  int16_t q[4]={mode*5462,(edge&1)?32767:0,(edge&2)?32767:0,(edge&1)?32767:0};sid_reset(&a);sid_reset(&b);
  for(int block=0;block<32;block++){
   uint32_t inc=1000000u+(unsigned)block*10000000u;sid_configure(&a,inc,q);sid_configure(&b,inc,q);
   int16_t actual[32];sid_render_samples(&a,actual,32);
   for(int i=0;i<32;i++)assert(actual[i]==sid_sample(&b));assert(!memcmp(&a,&b,sizeof a));
  }
 }
 puts("SID six-mode endpoint/pitch determinism, bounds and independent-state checks PASS");return 0;
}
