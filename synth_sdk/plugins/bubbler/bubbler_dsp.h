/* SPDX-License-Identifier: GPL-3.0-or-later
 * ESC_CODE / Volker Junginger's Delay Trio (github.com/VolkerJunginger/OP-1, Delay-Trio, e88427c); op1r: BB_SAMPLES 65536 (the FX scratch holds 176 KB; 1.49 s, the base delay is at most 1 s).
 * Fixed-point adaptation of Tom Erbe's +bubbler~ (SoundHack 0.0.8).
 * Original BSD terms and original source accompany this distribution.
 */
#ifndef BUBBLER_DSP_H
#define BUBBLER_DSP_H
#include "bubbler_params.h"
#define BB_SAMPLES 65536u
#define BB_STORAGE (BB_SAMPLES + 4u)
#define BB_MASK (BB_SAMPLES - 1u)
#define BB_PHASE_MASK ((BB_SAMPLES << 12) - 1u)
#define BB_GRAINS 8
struct bb_grain {
    unsigned pos, age, length, wait, fade, env, env_step;
    int step, active;
} ;
struct bb_state {
    int pitch_mode;
    struct bb_grain grain[BB_GRAINS];
    unsigned write, filled, seed, countdown;
    int mixbuf[64];
    struct bb_params decoded;
    short saved[5];
    int params_ready;
    int prep_ready,size_control,size_value,prep_delay,prep_density;
    unsigned length,spacing;
    int res_x1,res_x2,res_y1,res_y2,res_active;
    int delay, feedback, mix, filter_a, filter_z, dc_in, dc_out, ready;
} ;
void bb_init(struct bb_state *s);
void bb_render(struct bb_state *s, short *ring, short *left, short *right,
int frames, const short *par);
void bb_render_work(struct bb_state *s, short *storage, short *left, short *right,
int frames, const short *par, int *mixbuf);
void bb_fast_tables(void *memory);
void bb_fast_clip(void *memory);
#define BB_FAST_CLIP_BYTES (257u * sizeof(short))
#define BB_FAST_TABLE_BYTES ((257u + 2048u) * sizeof(short))
int bb_time_samples(int p);
int bb_cutoff(int p);
void bb_note(int note);
#endif
