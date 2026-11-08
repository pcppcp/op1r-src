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
 int32_t cutoff_live; /* Q12 coefficient with 14 extra fractional bits: 2 sin(pi fc / 88200) */
 uint32_t cached_inc;int16_t cached_q[4];unsigned configured;
} sid_state;
void sid_reset(sid_state *s);
void sid_configure(sid_state *s,uint32_t phase_increment,const int16_t *q);
void sid_render_samples(sid_state *s,int16_t *out,unsigned count);
int16_t sid_sample(sid_state *s);
/* sync / ring interval: green (0..32767) as semitones 0..24. Before 2026-11-07 the ratio was 1 + 3 * green / 32768
 * (1x..4x, free); now the nearest semitone to that ratio, so saved sounds keep their interval (to the nearest
 * semitone) and every stop is in tune. sid_semi_from[n] = green where semitone n + 1 starts. */
static const uint16_t sid_semi_from[24]={321,989,1697,2448,3243,4085,4977,5923,6925,7986,9110,10301,11563,12900,14317,15818,17408,19092,20877,22768,24771,26894,29142,31525};
static inline unsigned sid_interval_semi(int q1){unsigned n=0;while(n<24&&q1>=(int)sid_semi_from[n])n++;return n;}
#endif
