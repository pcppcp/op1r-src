/* BLOODMOON: gothic organ with a haunted bell strike.
 *   blue  (BLOOD)    : harmonic richness. Down = hollow, odd-harmonic organ; up = denser, darker stack (2nd-4th).
 *   green (ECLIPSE)  : bell strike at the attack: how loud it starts and how long it takes to sink into the organ.
 *                      Down = no bell; up = loud, long, dissonant bell.
 *   white (HAUNTING) : vibrato depth (fades in after the attack) plus a small steady pitch instability.
 *   red   (MOONRISE) : vibrato / instability speed, about 0.15 Hz to 9 Hz, squared curve (slow end gets most of the travel).
 *
 * Voice = fundamental + 2nd/3rd/4th harmonic (all from one phase) + two inharmonic bell partials at about 1.1875x
 * (a minor third, deliberately dissonant) and 2.40625x the pitch. Volume envelope and level come from the firmware.
 *
 * Structure follows synth_sdk/template/synth (pcppcp). The vibrato-fade-in idea is adapted from the Brassophone
 * plugin (nvmsocool). Everything is integer / Q15, 32-bit only per sample, no libc, as PLUGINS.md requires.
 * Copyright (C) 2026 Nola Drift. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"

/* ---- tuning constants ---- */
#define ORGAN_PEAK        18000      /* organ harmonics sum to at most this (Q15 amplitude) */
#define BELL_A_PEAK       5200       /* bell partial at ~1.1875x, at full strike level */
#define BELL_B_PEAK       3600       /* bell partial at ~2.40625x */
#define OUT_GAIN          27554      /* Q15 output gain, -1.5 dB from full scale (kennel level pass 2026-10-31: +1.5 dB, was 23170,
                                        -3 dB; upstream: +6 dB over 1.0 crackled the OP-1's speaker, so not all the way to 32767) */
                                     /* worst case 18000 + 5200 + 3600 = 26800 < 32767, so no clipping before the gain */

#define VIB_MAX_AMT       1400       /* white knob full: +-1400/32768 = +-4.3% frequency, about +-73 cents */
#define WOBBLE_MAX_AMT    350        /* steady instability at white full: about +-18 cents */
#define VIB_MIN_INC       7304       /* 0.15 Hz: inc = f * 2^32 / 88200 (R246's voice rate), 1 Hz = 48696 */
#define VIB_MAX_INC       438262     /* 9 Hz */
#define VIB_RANGE         (VIB_MAX_INC - VIB_MIN_INC)

#define VIB_DELAY_BLOCKS  80         /* about 116 ms of held note before vibrato starts (689 blocks/s) */
#define VIB_FADE_IN       450        /* depth gained per block while held (about 73 blocks, 105 ms, to full) */
#define VIB_FADE_OUT      2048       /* depth lost per block after note off (16 blocks, 23 ms) */

#define BELL_RATE_MIN     66         /* strike decay: fraction of level lost per block, Q16 (66 = tau about 1.4 s) */
#define BELL_RATE_SPAN    3200       /* added at green = 0 (tau about 29 ms) */

/* Voice state: the firmware gives 0x24 bytes (vtable + 32 bytes). */
typedef struct {
    uint32_t vtable;       /* firmware-owned: never write */
    uint32_t ph;           /* fundamental phase (2^32 = one cycle); harmonics are multiples of it */
    uint32_t ph_a;         /* bell partial A phase */
    uint32_t ph_b;         /* bell partial B phase */
    uint32_t vib_ph;       /* vibrato LFO phase */
    int32_t  vib_depth;    /* 0..32767 vibrato fade */
    int32_t  bell_lvl;     /* 0..65534 bell strike level (Q16), decays per block */
    int32_t  blood_s;      /* smoothed BLOOD knob */
    int16_t  held;         /* 1 from note_on until note_off */
    int16_t  age;          /* blocks since note_on (saturates) */
} bm_voice_t;
typedef char bm_voice_fits[sizeof(bm_voice_t) <= OP1_VOICE_SIZE ? 1 : -1];

#ifdef __bfin__
/* kennel: bloodmoon_dsp.S (ph, ph_a, ph_b must stay in this order in bm_voice_t) */
void bm_vib(int16_t *out, uint32_t *lfo, uint32_t vinc, uint32_t vinc3, uint32_t amts);
void bm_osc(int16_t *out, uint32_t *ph, const uint32_t *g, const int32_t *inc, const int16_t *tab);   /* tab: op1_sin_tab() */
#endif

static uint32_t bm_seed = 0x2545F491u;      /* gives each note a different vibrato start phase */

static inline int32_t clamp_q15(int32_t x) { return x < 0 ? 0 : (x > 32767 ? 32767 : x); }

static void bm_note_on(void *self, const int16_t *params)
{
    bm_voice_t *v = (bm_voice_t *)self;
    bm_seed = bm_seed * 1664525u + 1013904223u;
    v->ph = 0; v->ph_a = 0; v->ph_b = 0;       /* all partials start at a sine zero crossing: no click */
    v->vib_ph = bm_seed;
    v->vib_depth = 0;
    v->bell_lvl = clamp_q15(params[1]) << 1;
    v->blood_s = clamp_q15(params[0]);
    v->held = 1;
    v->age = 0;
}

static void bm_note_off(void *self)
{
    ((bm_voice_t *)self)->held = 0;
}

static void bm_render(void *self, int16_t *out, const int16_t *params)
{
    bm_voice_t *v = (bm_voice_t *)self;
    const int32_t *inc = OP1_PITCH_INC;
    const int16_t *sine = OP1_SINE_TABLE;
    int32_t vg = op1_voice_vel_gain(self);

    /* ---- per-block control values (divisions are fine here, not per sample) ---- */
    int32_t blood_t = clamp_q15(params[0]);
    int32_t d = blood_t - v->blood_s;
    v->blood_s += (d + (d > 0 ? 15 : 0)) >> 4;                     /* one-pole smoothing (23 ms), reaches the target exactly */
    int32_t b = v->blood_s;

    int32_t w1 = 20000;
    int32_t w2 = 1500 + ((b * 14000) >> 15);
    int32_t w3 = 7000 + ((b * 8000) >> 15);
    int32_t w4 = ((((b * b) >> 15)) * 10000) >> 15;                /* the 4th only arrives late in the travel */
    int32_t norm = (int32_t)(((int32_t)ORGAN_PEAK << 15) / (w1 + w2 + w3 + w4));
    int32_t g1 = (w1 * norm) >> 15, g2 = (w2 * norm) >> 15, g3 = (w3 * norm) >> 15, g4 = (w4 * norm) >> 15;

    int32_t inv_g = 32767 - clamp_q15(params[1]);
    int32_t bell_rate = BELL_RATE_MIN + ((((inv_g * inv_g) >> 15) * BELL_RATE_SPAN) >> 15);
    v->bell_lvl -= (v->bell_lvl * bell_rate) >> 16;
    if (v->bell_lvl < 32) v->bell_lvl = 0;
    int32_t ba = (v->bell_lvl * BELL_A_PEAK) >> 16;
    int32_t bb = (v->bell_lvl * BELL_B_PEAK) >> 16;

    if (v->held) {
        if (v->age < 32767) v->age++;
        if (v->age >= VIB_DELAY_BLOCKS) {
            v->vib_depth += VIB_FADE_IN;
            if (v->vib_depth > 32767) v->vib_depth = 32767;
        }
    } else {
        v->vib_depth -= VIB_FADE_OUT;
        if (v->vib_depth < 0) v->vib_depth = 0;
    }

    int32_t haunt = clamp_q15(params[2]);
    int32_t vib_amt = op1_mul16((haunt * VIB_MAX_AMT) >> 15, v->vib_depth);
    int32_t wob_amt = (haunt * WOBBLE_MAX_AMT) >> 15;

    int32_t rate = clamp_q15(params[3]);
    int32_t rate_sq = (rate * rate) >> 15;
    uint32_t vib_inc = (uint32_t)(VIB_MIN_INC + ((rate_sq * (VIB_RANGE >> 8)) >> 7));   /* 32-bit only */

#ifdef __bfin__
    /* kennel: the loop below in Blackfin assembly (bloodmoon_dsp.S), bit for bit, reading only the sine table's first
     * quadrant: the vibrato into out[], then the voice over it */
    {
        uint32_t lfo[2] = { v->vib_ph, v->vib_ph * 3u + 0x55555555u };
        bm_vib(out, lfo, vib_inc, vib_inc * 3u, (uint32_t)wob_amt << 16 | (uint32_t)vib_amt);
        v->vib_ph = lfo[0];
        const uint32_t g[4] = { (uint32_t)g2 << 16 | (uint32_t)g1, (uint32_t)g4 << 16 | (uint32_t)g3,
                                (uint32_t)bb << 16 | (uint32_t)ba, (uint32_t)OUT_GAIN << 16 | (uint32_t)vg };
        bm_osc(out, &v->ph, g, inc, op1_sin_tab());
        return;
    }
#endif
    uint32_t ph = v->ph, ph_a = v->ph_a, ph_b = v->ph_b, vib_ph = v->vib_ph;

    for (int n = 0; n < OP1_BLOCK; n++) {
        /* pitch modulation: vibrato LFO plus a faster-moving (3x) instability LFO, both from one phase */
        int32_t l1 = sine[vib_ph >> 19];
        int32_t l2 = sine[(vib_ph * 3u + 0x55555555u) >> 19];
        vib_ph += vib_inc;
        int32_t mod = (l1 * vib_amt + l2 * wob_amt) >> 15;          /* |mod| < 1750, far below the 2^12 limit */
        int32_t eff = inc[n] + (((inc[n] >> 12) * mod) >> 3);

        ph += (uint32_t)eff;
        uint32_t ue = (uint32_t)eff;
        ph_a += ue + (ue >> 3) + (ue >> 4);                           /* x1.1875 */
        ph_b += (ue << 1) + (ue >> 2) + (ue >> 3) + (ue >> 5);        /* x2.40625 */

        int32_t organ = (sine[ph >> 19] * g1 + sine[(ph << 1) >> 19] * g2
                       + sine[(ph * 3u) >> 19] * g3 + sine[(ph << 2) >> 19] * g4) >> 15;
        int32_t bell = (sine[ph_a >> 19] * ba + sine[ph_b >> 19] * bb) >> 15;

        out[n] = op1_sat16(((((organ + bell) * vg) >> 15) * OUT_GAIN) >> 15);   /* peak 26800 * 0.84 = 22500 per voice */
    }
    v->ph = ph; v->ph_a = ph_a; v->ph_b = ph_b; v->vib_ph = vib_ph;
}

const op1_engine_t ENGINE_bloodmoon = { bm_note_on, bm_render, bm_note_off,
                                        { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE } };
