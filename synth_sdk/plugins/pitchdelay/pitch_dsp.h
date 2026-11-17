/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ESC_PITCH_DSP_H
#define ESC_PITCH_DSP_H
#define PD_MAX_TIME_MS 1000
#define PD_SAMPLES 88208u

struct pd_state {
 unsigned write,filled,phase,increment;
 int ready,time,time_target,time_step,time_left;
 int depth,depth_target,depth_step,depth_left;
 int rate_cache,window_cache,feedback,mix,dc_in,dc_out;
};
struct pd_buffer {const struct pd_state *owner;short ring[PD_SAMPLES];};
void pd_init(struct pd_state *s);
void pd_render(struct pd_state *s,short *ring,short *l,short *r,const short p[8],int octave,int depth);
/* Native encoder values use a full-sized parameter scale: 896 units/interval. */
#define PD_PITCH_STEP 896
#define PD_PITCH_MAX (34*PD_PITCH_STEP)
int pd_pitch_index(int value);
int pd_time(int p);
int pd_cents(int index);
const char *pd_pitch_label(int index);
int pd_rate(int index,int octave);
#endif
