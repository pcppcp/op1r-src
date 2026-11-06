/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission. */
#include "op1_plugin.h"
#include "father_dsp.h"
_Static_assert(sizeof(mw_state)+31<=OP1_FX_SCRATCH_BYTES,"Father scratch overflow");
volatile uint32_t father_blocks[3];
volatile int16_t father_live[3][4];
volatile int32_t father_peak[3];
volatile int16_t father_gate[3];
static int16_t raw_params[3][4];
static int32_t normalized[3][4];
static uint8_t params_valid[3];
/* the state on a 32-byte line, so every ring starts on one (father_dsp.h) */
static mw_state *state(int p){uintptr_t a=(uintptr_t)op1_fx_scratch(p);return a?(mw_state *)((a+31)&~(uintptr_t)31):0;}
/* The state is cleared MW_CLEAR_WORDS words per callback after a select, dry meanwhile: a whole clear is ~5.7k
   single SDRAM stores, too many for one block (fx_spread.h). */
#define FX_SPREAD_WORDS MW_CLEAR_WORDS
#include "fx_spread.h"
static fx_spread_t spread[3];
static uint8_t fields_due[3];
static void init(int p){mw_state *s=state(p);if(s){fx_spread_start(&spread[p%3],s,sizeof *s/4);fields_due[p%3]=1;}params_valid[p]=0;}
static void stereo(int p,int16_t *l,int16_t *r,const int16_t *params){
 mw_state *s=state(p);if(!s)return;int32_t q[4];
 /* Range conversion divides; cache each unchanged word, including master FX.
    LFO/encoder changes still normalize on their first affected callback. */
 for(unsigned i=0;i<4;i++){
  if(!params_valid[p]||raw_params[p][i]!=params[i]){
   raw_params[p][i]=params[i];normalized[p][i]=op1_fx_param_q15(params,i);
  }
  father_live[p][i]=(int16_t)(q[i]=normalized[p][i]);
 }
 params_valid[p]=1;
 if(fx_spread_step(&spread[p%3]))return;
 if(fields_due[p%3]){fields_due[p%3]=0;mw_init_fields(s);}
#ifdef __bfin__
 /* father_kern.S zeroes L0..L3; the caller's values come back on return, as MBIRA and Gin do */
 uint32_t l0,l1,l2,l3;
 __asm__ volatile("%0 = L0;\n\t%1 = L1;\n\t%2 = L2;\n\t%3 = L3;":"=d"(l0),"=d"(l1),"=d"(l2),"=d"(l3));
 mw_process(s,l,r,q);
 __asm__ volatile("L0 = %0;\n\tL1 = %1;\n\tL2 = %2;\n\tL3 = %3;"::"d"(l0),"d"(l1),"d"(l2),"d"(l3));
#else
 mw_process(s,l,r,q);
#endif
 father_peak[p]=s->peak;father_gate[p]=(int16_t)s->gate_gain;father_blocks[p]++;
}
static void mono(int p,int16_t *b,const int16_t *q){stereo(p,b,0,q);}
const op1_fx_stereo_t FX_father={{init,mono,{0,0,0,0}},stereo};
