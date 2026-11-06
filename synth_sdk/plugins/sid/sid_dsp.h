#ifndef SID_DSP_H
#define SID_DSP_H
#include <stdint.h>
/* Blue repeats the six waves in 512-unit bands: about 11x faster to select.
 * Shared by audio and screen; no core rotary hook or legacy value migration. */
static inline unsigned sid_wave_mode(int16_t value) {
 return ((unsigned)(uint16_t)value >> 9) % 6u;
}
typedef struct {
 uint32_t phase[3],previous[3],noise[3],step[3];
 uint32_t pulse_steep[3],saw_steep[3],saw_recip[3];
 int32_t band,low,dc;uint32_t ring_msb;unsigned sync_rise;
 int cutoff,resonance,width,mode,secondary;
 int32_t cutoff_live; /* Q12 coefficient with 12 extra fractional bits */
 uint32_t cached_inc;int16_t cached_q[4];unsigned configured;
} sid_state;
void sid_reset(sid_state *s);
void sid_configure(sid_state *s,uint32_t phase_increment,const int16_t *q);
void sid_render_samples(sid_state *s,int16_t *out,unsigned count);
int16_t sid_sample(sid_state *s);
#endif
