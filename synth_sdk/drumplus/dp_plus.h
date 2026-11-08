/* drum+ voice: the stock drum voice (dp_voice.c, bit-exact) plus per-pad extras (docs/31). All extras off = the
 * stock render, unchanged. Integer only; per-sample work kept small (12 voices in the audio interrupt). */
#ifndef DP_PLUS_H
#define DP_PLUS_H
#include "dp_voice.h"

/* R246 renders a drum voice at 88,200 Hz (128 samples a call, decimated 2:1 with the synth voices). Times are set and
 * shown in 44.1 kHz samples (dp_time_samples, the labels) and counted in voice samples: DP_VOICE_X per 44.1 kHz
 * sample (2026-11-03; before, the voice counted them 1:1, so every time ran at half its label). */
#define DP_VOICE_X 2

/* per-pad extras (stored in the patch JSON "op1r" key; 0 = off / neutral everywhere) */
typedef struct {
    uint8_t env_on;                 /* per-pad envelope instead of the plain velocity */
    uint8_t attack, hold, decay;    /* 0..127 each, times on an exponential scale (dp_time_samples) */
    uint8_t release;                /* 0..127: after key-up (gate / loop) */
    uint8_t curve_exp;              /* 0 linear, 1 exponential decay / release */
    int8_t  pitch_env;              /* -48..48 semitones at the start, gliding to 0 */
    uint8_t pitch_time;             /* 0..127: glide time */
    int8_t  fine;                   /* -50..50 cents */
    uint8_t retrig;                 /* 0 off, 2..8 hits */
    uint8_t retrig_div;             /* 0..5: 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/64 */
    uint8_t filter;                 /* 0 off, 1 low pass, 2 high pass, 3 band pass */
    uint8_t cutoff;                 /* 0..127: 30 Hz .. 16 kHz */
    uint8_t resonance;              /* 0..127 */
    uint8_t drive;                  /* 0 off .. 127: gain 1..4 into a soft clip */
    uint8_t choke;                  /* 0 none, 1..4: a hit fades out the other pads of the group */
    uint8_t mono;                   /* 1: a new hit of this pad fades out its previous one */
    int8_t  vel_pitch;              /* -24..24 semitones at velocity 127 vs 64 (linear in velocity) */
    int8_t  vel_cutoff;             /* -127..127 cutoff steps at velocity 127 vs 64 */
    uint8_t vel_start;              /* 0..127: softer hits start later, up to vel_start * 32 samples at velocity 0 */
    uint8_t human;                  /* 0..127: random start delay up to human * 4 samples, level 0 .. -1.4 dB at 127 */
} dp_extra;

/* constants of a pad's extras, prepared when they change (dp_plus_prepare): no divisions in the render */
typedef struct {
    uint8_t active;
    uint32_t t_att, t_hold, t_dec, t_rel;               /* voice samples */
    int32_t a_step, d_step, r_step;                     /* linear envelope steps, Q30 */
    uint32_t kd, kr;                                    /* exponential envelope decrements per voice sample, 6.9 / T, Q22 */
    int32_t p_start, p_step, fine_u;                    /* pitch envelope (1/96 octave << 20), fine (<< 8) */
    int32_t f_k;                                        /* filter damping, Q14 (coefficients per hit) */
    int16_t f_k16;                                      /* k in Q16 for the per-hit coefficients */
    int32_t drive_q12;                                  /* drive gain, Q12 (0: off) */
} dp_pre;

/* per-voice state of the extras */
typedef struct {
    int32_t env;                    /* Q30 level 0..1 */
    uint8_t stage;                  /* 0 attack, 1 hold, 2 decay, 3 release, 4 done */
    uint32_t stage_n;               /* samples spent in the stage */
    int32_t pitch_u;                /* current pitch envelope offset, 1/96 octave << 20 */
    uint32_t until_retrig;          /* voice samples to the next retrig hit */
    uint8_t hits_left;
    int16_t hit_gain;               /* Q15 gain of the current hit (falls per retrig) */
    int32_t ic1, ic2;               /* filter integrators (signal << 8) */
    int32_t vel_u;                  /* velocity pitch offset, 1/96 octave << 8 */
    int32_t f_a1, f_a2, f_a3;       /* this hit's filter coefficients (velocity -> cutoff) */
    uint8_t pend_done, pend_state;  /* fast path: pre -> post */
    int16_t fade;                   /* choke / mono fade-out, Q15 (fading set) */
    uint8_t fading;
} dp_xvoice;

int  dp_extra_active(const dp_extra *x);
/* the saved form: field i (0..DP_NFIELDS-1, in struct order) clamped into its range; returns the stored value */
#define DP_NFIELDS 21
int  dp_extra_set(dp_extra *x, int i, int32_t v);
int  dp_extra_get(const dp_extra *x, int i);
uint32_t dp_time_samples(uint8_t t);                     /* 0..127 -> 0 .. ~4.6 s, in 44.1 kHz samples */
void dp_plus_prepare(const dp_extra *x, dp_pre *c);     /* when a pad's extras change (not in the audio loop) */
/* velocity 0..127 (the stock note_on's velocity, before << 8); samples_per_beat in 44.1 kHz output frames */
void dp_plus_note_on(dp_voice *v, dp_xvoice *s, const dp_pad *pad, const dp_extra *x, const dp_pre *c,
                     int velocity, uint32_t samples_per_beat);
void dp_plus_note_off(dp_voice *v, dp_xvoice *s, const dp_pad *pad, const dp_extra *x, const dp_pre *c);
/* before a note-on of pad new_pad: fade out the voices it chokes (same choke group, or the same pad if mono).
 * v[i]: voice i (the firmware's voices are not contiguous dp_voice), voice_pad[i]: its pad; x: extras of all pads. Returns the number of voices fading. */
int dp_plus_choke(dp_voice *const *v, dp_xvoice *s, const uint8_t *voice_pad, int nvoices, int new_pad, const dp_extra *x);
/* inc / vel: the stock buffers of this voice (vel = velocity << 8); like dp_stock_render otherwise */
void dp_plus_render(dp_voice *v, dp_xvoice *s, int16_t *out, const dp_pad *pad, const dp_extra *x, const dp_pre *c,
                    const int32_t *inc, const int16_t *vel, const int16_t *data, const int16_t *fir,
                    uint32_t samples_per_beat);
/* fast path (firmware: the stock render between pre and post on the stock buffers inc / vel, modified in place).
 * pre returns 0 without changing anything if a retrig falls inside the block: use dp_plus_render then. */
int  dp_plus_fast_pre(dp_voice *v, dp_xvoice *s, const dp_extra *x, const dp_pre *c, int32_t *inc, int16_t *vel);
void dp_plus_fast_post(dp_voice *v, dp_xvoice *s, const dp_extra *x, const dp_pre *c, int16_t *out);
#endif
