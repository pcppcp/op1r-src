/* Actual production engine wrapper: current-voice per-sample pitch array. */
#define OP1_HOST
#include "../../op1_plugin.h"
#include "sid_dsp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int32_t host_pitch_inc[128];uint8_t host_voice_mem[6*0x24];int16_t host_voice_vel[6];
#include "sid.c"
int main(void){
 int16_t q[4]={8192,16384,24576,8192},a[128],b[128];
 for(int i=0;i<128;i++)host_pitch_inc[i]=40000000+(i/32)*10000000;
 for(int voice=0;voice<6;voice++){
  void *self=host_voice_mem+voice*0x24;ENGINE_sid.note_on(self,q);ENGINE_sid.render(self,a,q);
  sid_state reference;sid_reset(&reference);
  for(int i=0;i<128;i++){if(!(i&31))sid_configure(&reference,(uint32_t)host_pitch_inc[i],q);b[i]=sid_sample(&reference);}
  assert(!memcmp(a,b,sizeof a));assert(voices[voice].cached_inc==70000000);
 }
 sid_state before=voices[1];ENGINE_sid.note_on(host_voice_mem,q);assert(!memcmp(&before,&voices[1],sizeof before));
 for(int i=0;i<128;i++)host_pitch_inc[i]=40000000;
 ENGINE_sid.render(host_voice_mem,b,q);assert(memcmp(a,b,sizeof a));
 puts("SID production render: six voices, 32-sample pitch modulation, retrigger isolation PASS");return 0;
}
