/* Fixed-point port of Mutable Instruments Clouds (granular processor), mono.
 * Original: https://github.com/pichenettes/eurorack/tree/master/clouds, (c) 2014 Emilie Gillet, MIT license.
 * Port: no floating point (Blackfin has no FPU). Modes: granular, looping delay. Stretch / spectral are not
 * ported. Reference build + comparison: harness/clouds_compare.py (original vendored in third_party/).
 *
 * Audio: Q15 (int16 / int32 with headroom). Parameters: Q15 0..32767 = 0.0..1.0, pitch in 1/256 semitones.
 * CLOUDS_SR: 32000 = identical timing to the original (used for verification), 44100 on the OP-1.
 * OP-1 speed options (off in the verification build): CLOUDS_LINEAR_READ (grains read with linear instead of
 * Hermite interpolation: no 64-bit multiplies per grain sample), CLOUDS_SKIP_IDLE (diffuser / reverb /
 * feedback filter not run while their amount is 0), CLOUDS_MAX_GRAINS (default 40). */
#ifndef CLOUDS_DSP_H
#define CLOUDS_DSP_H
#include <stdint.h>
#include <stddef.h>

#ifndef CLOUDS_SR
#define CLOUDS_SR 44100
#endif
#define CLOUDS_MAX_BLOCK   64
#ifndef CLOUDS_MAX_GRAINS
#define CLOUDS_MAX_GRAINS  40          /* original: 40 in mono hi-fi mode */
#endif
#define CLOUDS_BUFFER_BYTES 118784     /* recording buffer (as the original's large buffer) */
#define CLOUDS_FX_BYTES    ((2048 * 4) + (16384 * 2) + (4096 * 2))   /* diffuser + reverb + pitch shifter */
#define CLOUDS_MEM_BYTES   (CLOUDS_BUFFER_BYTES + CLOUDS_FX_BYTES)    /* 167936, fits the OP-1 FX scratch */

enum { CLOUDS_GRANULAR = 0, CLOUDS_LOOPING = 1 };

typedef struct {
    int32_t position, size, density, texture, dry_wet, feedback, reverb;   /* Q15 0..32767 */
    int32_t pitch;                                                         /* semitones * 256 */
    uint8_t freeze, trigger;
} clouds_params_t;

typedef struct {
    int32_t first_sample, width, phase, phase_increment, pre_delay;
    uint32_t env_phase, env_increment;          /* Q30: 2.0 = 2^31 */
    int32_t env_smoothness, env_slope;          /* Q15, Q12 */
    int32_t env_slope_limit;                    /* gain above which gain * slope clips (no per-sample divide) */
    uint8_t active, quality;
} clouds_grain_t;

typedef struct { int32_t g, r, h, s1, s2; } clouds_svf_t;   /* coefficients Q24, state Q15 */

/* what schedule_grain works out from the parameters alone (clouds_dsp.c grain_tpl), kept per block */
typedef struct {
    int64_t gs;                                         /* grain size, Q8 samples */
    int32_t width, offset, inc;                         /* samples; start = head - offset; Q16 */
    uint32_t env_increment;
    int32_t env_smoothness, env_slope, env_slope_limit;
} clouds_tpl_t;

typedef struct clouds {
    int mode, prev_mode;
    /* recording buffer */
    int16_t *s16;
    int32_t size, head, crossfade;
    int16_t tail[257];
    /* granular player */
    clouds_grain_t grains[CLOUDS_MAX_GRAINS];
    int32_t num_grains_q16, gain_norm_q15, hint_q8, phasor;
    /* looping player (Q12 sample positions) */
    int64_t lp_current_delay;                           /* Q28 */
    int32_t lp_phase, lp_loop_point, lp_loop_duration, lp_tail_start, lp_tail_duration,
            lp_loop_reset, lp_inv_tail;
    /* fx engines */
    int32_t *diff_buf;  int32_t diff_wp;
    int16_t *rev_buf;   int32_t rev_wp, rev_lp1, rev_lp2;  int32_t rev_lfo[2], lfo_y0[2], lfo_y1[2], lfo_iir[2];
    int16_t *ps_buf;    int32_t ps_wp;  uint32_t ps_phase; int32_t ps_size_q16;
    /* processor */
    clouds_svf_t fb_filter, lp_filter, hp_filter;
    int32_t freeze_lp_q30, dry_wet_q15;
    int32_t fb[CLOUDS_MAX_BLOCK];
    uint32_t rng;
    int64_t sd_p, sd_space;                             /* the seeding divisions and their inputs (play_granular) */
    int32_t sd_target, sd_hint;
    uint8_t sd_ok;
    clouds_tpl_t tpl;                                   /* the grain set-up of the last block and its inputs */
    int32_t tpl_key[4];
    uint8_t tpl_ok;
} clouds_t;

void clouds_init(clouds_t *c, void *mem, int mode);            /* mem: CLOUDS_MEM_BYTES */
void clouds_process(clouds_t *c, const clouds_params_t *p, const int16_t *in, int16_t *out, int n);
#endif
