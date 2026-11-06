/* Six OP-1 voices, each a three-oscillator cRSID-derived mini-chip. */
#include "op1_plugin.h"
#include "sid_dsp.h"
#include "op1_l1.h"
#ifdef __bfin__
void sid_noise_l1(uint16_t *l1,int fresh);   /* sid_dsp.c: the noise tables (6,144 B) in the L1 partition, or 0 for SDRAM */
#endif
static sid_state voices[OP1_VOICES];
volatile int16_t sid_live[4];
static void note_on(void *self,const int16_t *q){(void)q;sid_reset(&voices[op1_voice_index(self)]);}
static void render(void *self,int16_t *out,const int16_t *q){
 int voice=op1_voice_index(self);sid_state *s=&voices[voice];
 for(int i=0;i<4;i++)sid_live[i]=q[i];
#ifdef __bfin__
 if(sid_wave_mode(q[0])==3){            /* only the noise wave reads the tables: lease the pool while it sounds */
  int fresh;uint16_t *t=op1_l1_claim(OP1_L1_SYNTH,voices,6144,&fresh);
  sid_noise_l1(t,fresh);
 }
#endif
 for(int i=0;i<OP1_BLOCK;i+=32){
  sid_configure(s,(uint32_t)OP1_PITCH_INC[i],q);
  sid_render_samples(s,out+i,32);
 }
 /* level pass 2026-10-29: the chip has no gain stage, so one per block here: +2.6 dB (Q13 11051) times the preset
    trim (word 7) */
 int32_t g=(11051*op1_preset_trim(q))>>13;
 for(int i=0;i<OP1_BLOCK;i++)out[i]=op1_sat16((out[i]*g)>>13);
}
const op1_engine_t ENGINE_sid={note_on,render,0,{OP1_PRESS_NONE,OP1_PRESS_NONE,OP1_PRESS_NONE,OP1_PRESS_NONE}};
