/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* "wavetable": two detuned wavetable oscillators into an analog-style 4-pole ladder filter.
 * blue   (knob 0): wave position, morphing sine -> warm -> triangle -> saw -> square -> pulse -> vocal -> digital
 * green  (knob 1): detune between the two oscillators, 0 to about +-14 cents
 * white  (knob 2): cutoff, 30 Hz to 16 kHz (exponential)
 * orange (knob 3): resonance up to self-oscillation, with the drive into the filter rising alongside
 * Follows note velocity like op1r's engines. Int-only; per sample: 2 x (2 interpolated table reads + crossfade),
 * 4 filter stages, 2 soft clips.
 */
#include "hk/engine.h"
#include "op1_l1.h"
#include "wavetable_dsp.h"

static WavetableState voices[ENGINE_VOICES];
volatile uint32_t wavetable_blocks; /* for the screen: voice calls rendered */
/* for the screen (wavetable_screen.c), per voice: wavetable_blocks when it last rendered, and its level: the peak of
   every 8th sample of the block times the stock envelope at the block's end (the ADSR is applied after render). Only
   read and written beside the samples: the audio is unchanged. */
volatile uint32_t wavetable_vblk[ENGINE_VOICES];
volatile int16_t wavetable_vlev[ENGINE_VOICES];
#ifdef ENGINE_HOST_TEST
#define WT_ENV_END 32767
#else
#define WT_ENV_END (((const volatile int16_t *)0xFF801D50)[ENGINE_BLOCK - 1])   /* op1_plugin.h OP1_VOICE_ENV */
#endif

void engine_note_on(void *self, const int16_t *knobs)
{
    (void)knobs;
    wavetable_dsp_on(&voices[ENGINE_VOICE(self)]);
}

void engine_note_off(void *self, const int16_t *knobs)
{
    (void)self;
    (void)knobs;
}

/* the knobs as this block hears them (after the slot LFO), for the screen */
volatile int16_t wavetable_knobs[4];

/* L1 pool (op1_plugin.h, kennel/kn_l1.h): while this engine holds the synth partition the kernel reads the whole table
   (8,192 B, both the held-knob path and the ramp) from on-chip L1, copied in when the partition is new to it; without
   it (no pool, another engine still rendering) from the file's copy in SDRAM. Same bytes either way. */
static void rows_place(void)
{
#if defined(__bfin__) && !defined(ENGINE_HOST_TEST)
    int fresh;
    uint32_t *t = op1_l1_claim(OP1_L1_SYNTH, voices, sizeof wt_pairs, &fresh);
    if (t && fresh) {
        const uint32_t *s = &wt_pairs[0][0];
        for (uint32_t i = 0; i < sizeof wt_pairs / 4; i++) t[i] = s[i];
    }
    wt_rows_at = t ? (const uint32_t (*)[WT_SIZE])t : wt_pairs;
#endif
}

void engine_render(void *self, int16_t *out, const int16_t *knobs)
{
    rows_place();
    for (int i = 0; i < 4; i++) wavetable_knobs[i] = knobs[i];
    const int vi = ENGINE_VOICE(self);
    wavetable_dsp_render(&voices[vi], out, knobs, ENGINE_PITCH, ENGINE_VELOCITY(vi));
    int32_t pk = 0;
    for (int i = 0; i < ENGINE_BLOCK; i += 8) {
        const int32_t a = out[i] < 0 ? -out[i] : out[i];
        if (a > pk) pk = a;
    }
    int32_t env = WT_ENV_END;
    if (env < 0) env = 0;
    wavetable_vlev[vi] = (int16_t)((pk * env) >> 15);
    wavetable_vblk[vi] = ++wavetable_blocks;
}

/* the two oscillators' phase difference of voice v (the detune's beating), for the screen */
uint32_t wavetable_drift(int v)
{
    return voices[v].ph2 - voices[v].ph1;
}

int32_t wavetable_shape(int32_t k0, uint32_t ph)
{
    return wavetable_dsp_shape(k0, ph);
}
