/* Cached FX normalization: exact output across positions, edits and re-init; dry while the state is cleared.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with OP-1 firmware permission. */
#define OP1_HOST
#define op1_fx_scratch unused_host_scratch
#define op1_fx_param_q15 unused_host_q15
#include "op1_plugin.h"
#undef op1_fx_scratch
#undef op1_fx_param_q15
#include "father_dsp.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static unsigned char scratch[3][sizeof(mw_state)+32];static mw_state refs[3];
static unsigned dry[3];   /* callbacks left that father.c spends clearing (dry output) */
static unsigned divisions;
static void *op1_fx_scratch(int p){return scratch[p]+1;}   /* misaligned on purpose: father.c aligns it */
static int32_t op1_fx_param_q15(const int16_t *p,int i){divisions++;return p[i]<0?0:p[i];}
#include "father.c"
void mw_ref_init(mw_state*);
void mw_ref_process(mw_state*,int16_t*,int16_t*,const int32_t*);
int main(void){
 unsigned rng=9,expected=0;int16_t params[3][8]={{1000,0,16000,12000},{18000,7000,24000,20000},{32000,0,30000,32000}};
 const unsigned clear=(sizeof(mw_state)/4+MW_CLEAR_WORDS-1)/MW_CLEAR_WORDS;
 for(int p=0;p<3;p++){FX_father.mono.init(p);mw_ref_init(&refs[p]);dry[p]=clear;}
 for(unsigned block=0;block<1500;block++)for(int p=0;p<3;p++){
  if(block==750 && p==2){FX_father.mono.init(p);mw_ref_init(&refs[p]);expected+=4;dry[p]=clear;}
  if(block%13==0){rng=rng*1664525u+1013904223u;unsigned k=(block/13)%4;int16_t v=rng>>17;if(v!=params[p][k]){params[p][k]=v;if(block)expected++;}}
  if(!block)expected+=4;
  int16_t l[64],r[64],a[64],b[64];int32_t q[4];for(int k=0;k<4;k++)q[k]=params[p][k]<0?0:params[p][k];
  for(int n=0;n<64;n++){rng=rng*1664525u+1013904223u;l[n]=a[n]=(int16_t)(rng>>16);rng=rng*1664525u+1013904223u;r[n]=b[n]=(int16_t)(rng>>16);}
  const int clearing=dry[p]!=0;if(clearing)dry[p]--;   /* dry while clearing: the reference starts after */
  if(p==2){FX_father.render_stereo(p,l,r,params[p]);if(!clearing)mw_ref_process(&refs[p],a,b,q);assert(!memcmp(r,b,sizeof r));}
  else{FX_father.mono.render(p,l,params[p]);if(!clearing)mw_ref_process(&refs[p],a,0,q);}
  assert(!memcmp(l,a,sizeof l));assert(divisions==expected);
  for(int k=0;k<4;k++)assert(father_live[p][k]==q[k]);
 }
 printf("4500 instrument/drum/master blocks exact; %u conversions instead of 18000\n",divisions);
}
