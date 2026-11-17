/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ESC_DELAY_DSP_H
#define ESC_DELAY_DSP_H
#define DL_SAMPLES 88208u

struct dl_state {
 unsigned write,phase,seed,freeze_length,filled,filter_fraction;
 int ready,time_q8,feedback,mix,depth,x1,x2,y1,y2,dc_in,dc_out;
 int random_from,random_to;
 unsigned smooth_phase;
 int smooth_ready,smooth_shape,smooth_gain,smooth_from,smooth_to;
};
struct dl_buffer { const struct dl_state *owner; short ring[DL_SAMPLES]; };
void dl_init(struct dl_state *s);
void dl_set_smooth(int enabled);
void dl_render(struct dl_state *s,short *ring,short *left,short *right,int n,const short par[8],int resonance,int freeze);
int dl_time(int p);
int dl_freq(int p);
int dl_rate_milli(int p);
int dl_wave(unsigned phase,int shape,int a,int b);
#endif
