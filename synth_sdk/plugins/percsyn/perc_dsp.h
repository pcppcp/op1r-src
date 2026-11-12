/* perc: drum voice for the PO-32 patch model, integer only.
 * GPL-3.0-or-later with the OP1R firmware permission. The signal path and parameter maps follow libpo32's
 * documented reference renderer (MIT, Eric Lewis); this is an independent integer implementation. */
#ifndef PERC_DSP_H
#define PERC_DSP_H
#include <stdint.h>

#define PERC_RATE   44100u          /* voice rate inside perc; the engine interpolates to the 88.2 kHz voice rate */
#define PERC_NPARAM 21
#define PERC_CR     4               /* control rate: envelopes, pitch and gains every PERC_CR samples */

/* PO-32 patch words in transfer order, 0..65535 = 0..1 */
enum { P_OSCWAVE, P_OSCFREQ, P_OSCATK, P_OSCDCY, P_MODMODE, P_MODRATE, P_MODAMT, P_NFILMOD, P_NFILFRQ, P_NFILQ,
       P_NENVMOD, P_NENVATK, P_NENVDCY, P_MIX, P_DIST, P_EQFREQ, P_EQGAIN, P_LEVEL, P_OSCVEL, P_NVEL, P_MODVEL };

/* The four knobs and the Shift words, already turned into offsets (the engine does that). */
typedef struct {
    int32_t pitch;      /* octaves, Q16 (tone body) */
    int32_t length;     /* log2 of a decay time factor, Q16 */
    int32_t punch;      /* -1..1, Q16: pitch drop depth and attack sharpness */
    int32_t tone;       /* octaves, Q16 (noise filter and EQ) */
    int32_t dist;       /* added to the patch's distortion, 0..1 Q16 */
    int32_t level_db;   /* dB, Q16 */
} perc_macro_t;

typedef struct {
    uint8_t wave, mod_mode, nf_mode, nenv_mode, use_eq, dist_mode, mod_on, pad;
    int32_t osc_l2;                     /* log2 of the phase increment (Q32 per sample) at C4, Q16 */
    int32_t mod_scale;                  /* octaves at full modulation, Q16 */
    /* envelope and modulation steps below are per PERC_CR samples (perc_prepare converts them) */
    uint32_t osc_atk_n; int32_t osc_atk_step; int32_t osc_dcy_c;      /* samples, log2 Q24 per step, Q30 */
    int32_t mod_dcy_c; uint32_t mod_inc; int32_t mod_alpha;            /* Q30, Q32 phase, Q30 */
    uint32_t n_atk_n; int32_t n_atk_step; int32_t n_dcy_c;
    uint32_t lin_atk_n, lin_end_n; int32_t lin_up, lin_dn;             /* Q30 per sample */
    uint32_t ring_inc; int32_t ring_corr;                              /* Q32 phase, Q26 */
    int32_t nf_a1, nf_a2, nf_a3, nf_k;                                 /* Q30, k Q26 */
    int32_t eq_a1, eq_a2, eq_a3, eq_m1;                                /* Q30, m1 Q26 */
    int32_t osc_g, noise_g;                                            /* Q30 (mix x velocity) */
    int32_t level;                                                     /* Q28 */
    int32_t d_drive, d_gain, d_clip, d_dc;                             /* Q24 */
    uint32_t end_n;                                                    /* samples after which the voice is silent */
} perc_prep_t;

typedef struct {
    perc_prep_t p;
    uint32_t n, phase, mod_phase, ring_phase, rnd;
    int32_t osc_env, osc_lg, mod_env, mod_state, nenv, n_lg, lin;
    int32_t f1, f2, e1, e2;
    int32_t prev;                       /* last 44.1 kHz output (int16 range), for the 2x interpolation */
    int32_t ao, an;                     /* tone and noise gains of the last control step (ramped between) */
    uint8_t osync, nsync, ramp;
    uint32_t tail;                      /* samples since both envelopes ended (EQ ring-out) */
    uint8_t active;
} perc_voice_t;

/* velocity 1..127 */
void perc_prepare(perc_prep_t *o, const uint16_t *patch, const perc_macro_t *m, int velocity);
void perc_trigger(perc_voice_t *v);
/* n samples at PERC_RATE into out (int16 range in int32 storage is not needed: int16 out); key_l2 = log2 offset of
 * the played key against C4, Q16. Returns 0 once the voice has ended (out zeroed). */
int perc_render(perc_voice_t *v, int16_t *out, unsigned n, int32_t key_l2);

/* helpers shared with the engine */
int32_t perc_log2(uint32_t v);                  /* log2(v), Q16, v > 0 */
uint32_t perc_pow2(int32_t x, int frac_bits);   /* 2^(x / 65536) * 2^frac_bits, saturating */
#endif
