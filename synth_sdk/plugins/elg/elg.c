/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* "elg": a drone synth (the moose). Three saws per voice (root, a detuned root, and a third voice at an
 * interval above), a growl drive and a resonant two-pole low-pass, all slowly moving.
 * blue   (knob 0): pitch: the third voice's interval, in steps: unison, fifth, octave, octave + fifth, two octaves
 * green  (knob 1): detune: beating between the voices, up to about +-20 cents
 * white  (knob 2): timbre: low-pass cutoff (40 Hz .. 6 kHz) and how hard the growl drive is pushed
 * red    (knob 3): movement: an LFO (0.03 .. 2 Hz) sweeping the cutoff (up to +-1.5 octaves) and swelling the
 *                  level, plus a slow pitch drift per voice
 * Follows note velocity. Int-only; per sample: 3 saws, a soft clip, a Chamberlin state-variable filter.
 */
#include "hk/engine.h"
#include "elg_level.h"

typedef struct {
    uint32_t ph[3], lfo;
    int32_t low, band;
    int32_t drift, drift_v;   /* pitch drift, Q15 of +-6 cents */
    uint32_t rnd, tick;       /* tick: call parity, the drift moves every other call (designed for 344 calls/s) */
    int32_t control[5]; /* previous detune/drift/filter/drive/gain targets */
    int control_ready;
} voice_t;

static voice_t voices[ENGINE_VOICES];
volatile uint32_t elg_blocks;   /* for the screen */
volatile int32_t elg_level, elg_lfo;

/* the third voice's interval, Q12: 1, 3/2, 2, 3, 4 */
/* Repeating 512-unit selector, also used by the antler animation. */
int elg_interval(int32_t value)
{
    const unsigned x = (unsigned)(value < 0 ? 0 : value) >> 9;
    return x < 64 ? (int)(x - 5 * ((x * 13) >> 6)) : (int)(x % 5u);   /* a knob (x <= 63): mod 5 without libgcc's division */
}
static const int32_t ratio[5] = {4096, 6144, 8192, 12288, 16384};

/* 2^(x / 4096), Q12 (4096 .. 4096 * 2^7) for x in 0..32767 */
static int32_t exp2q(int32_t x)
{
    if (x < 0) x = 0;
    const int32_t e = x >> 12, f = x & 4095;
    return (4096 + ((f * (2839 + ((f * 1257) >> 12))) >> 12)) << e;
}

/* Exact replacements for per-sample 64-bit products and a divide: the Blackfin does them in libgcc
   (__muldi3, __divsi3 through a 32-step loop). Bit-identical to the 64-bit / C expressions in their ranges. */

/* (int64_t)u * m >> s for s = 15 or 16, cast to uint32_t; |m| < 2^15 (here |dr| <= 115, 0 <= det <= 392) */
static inline uint32_t mul_u32_s(uint32_t u, int32_t m, int s)
{
    const int32_t hi = (int32_t)(u >> 16) * m, lo = (int32_t)(u & 0xFFFF) * m;
    return (uint32_t)(hi * (1 << (16 - s)) + (lo >> s));
}

/* (int64_t)a * b >> 15 for 0 <= b < 2^15 and |a| < 2^30, in 32 bits: a = ah 2^15 + al */
static inline int32_t mul_q15(int32_t a, int32_t b)
{
    return (a >> 15) * b + (((a & 0x7FFF) * b) >> 15);
}

/* x / 3 (C truncation) for |x| < 2^17; 43691 = ceil(2^17 / 3), split so no product exceeds 32 bits */
static inline int32_t div3(int32_t x)
{
    const uint32_t a = (uint32_t)(x < 0 ? -x : x);
    const int32_t q = (int32_t)(((a >> 8) * 43691u + (((a & 255) * 43691u) >> 8)) >> 9);
    return x < 0 ? -q : q;
}

#ifdef __bfin__
/* elg_dsp.S: the sample loop in five hand-scheduled passes (see its header) */
void elg_inc(uint32_t *t, const uint32_t *pitch, const int32_t *c);
void elg_mix(uint32_t *ph, const uint32_t *t, int16_t *out);
void elg_shape(int16_t *out, int32_t wdrive, int32_t ddrive);
void elg_svf(int16_t *out, int32_t *st, const int32_t *c);
int32_t elg_peak(const int16_t *out);
static uint32_t elg_t[3 * ENGINE_BLOCK + 1];   /* the increments per sample (elg_mix reads one word ahead) */
#endif

static inline int32_t sine_lfo(uint32_t ph)   /* parabolic sine, +-32767 */
{
    const int32_t x = (int32_t)(ph >> 16) - 32768;          /* -32768 .. 32767 = -pi .. pi */
    const int32_t ax = x < 0 ? -x : x;
    const int32_t y = (x * (32768 - ax)) >> 13;              /* 4 x (1 - |x|): 0 at 0 and +-pi, peak at +-pi/2 */
    return y > 32767 ? 32767 : y;
}

void engine_note_on(void *self, const int16_t *knobs)
{
    const int vi = ENGINE_VOICE(self);
    voice_t *v = &voices[vi];
    (void)knobs;
    v->control_ready = 0;
    v->rnd = v->rnd * 1664525u + 1013904223u + (uint32_t)vi * 0x9E3779B9u;
    v->ph[0] = 0;
    v->ph[1] = v->rnd;
    v->ph[2] = v->rnd << 7;
    v->lfo = v->rnd << 3;   /* each voice moves on its own */
    v->low = v->band = 0;
}

void engine_note_off(void *self, const int16_t *knobs)
{
    (void)self;
    (void)knobs;
}

/* the knobs as this block hears them (after the slot LFO), for the screen */
volatile int16_t elg_knobs[4];

void engine_render(void *self, int16_t *out, const int16_t *knobs)
{
    for (int i = 0; i < 4; i++) elg_knobs[i] = knobs[i];
    const int vi = ENGINE_VOICE(self);
    voice_t *v = &voices[vi];
    int vel = ENGINE_VELOCITY(vi);
    if (vel < 1 || vel > 127) vel = 100;
    const int32_t k0 = knobs[0] < 0 ? 0 : knobs[0], k1 = knobs[1] < 0 ? 0 : knobs[1];
    const int32_t k2 = knobs[2] < 0 ? 0 : knobs[2], k3 = knobs[3] < 0 ? 0 : knobs[3];

    /* movement: LFO 0.03 .. 2 Hz (per call: 2^32 f / 689.06, R246's 128 samples at 88.2 kHz), and a slow random walk on
     * the pitch (it was written for 344 calls/s, so it steps every other call: rate fix 2026-11-03) */
    /* the products below fit 32 bits (knobs 0..32767, |lfo|, |drift| <= 32768, vel <= 127), so they are the 64-bit
     * expressions they replaced, without libgcc's __muldi3; 187000 * e >> 12 is split at e's 12 low bits */
    {
        const int32_t e = exp2q((k3 * 24800) >> 15);                       /* 4096 .. < 2^19 */
        v->lfo += (uint32_t)(187000 * (e >> 12) + ((187000 * (e & 4095)) >> 12));
    }
    const int32_t lfo = sine_lfo(v->lfo);
    if (v->tick++ & 1) {
        v->rnd = v->rnd * 1664525u + 1013904223u;
        v->drift_v += ((int32_t)(v->rnd >> 20) - 2048) - (v->drift >> 6);
        v->drift_v -= v->drift_v >> 4;
        v->drift += v->drift_v >> 4;
        if (v->drift > 32767) v->drift = 32767;
        if (v->drift < -32767) v->drift = -32767;
    }

    /* pitches: detune up to +-1.2 % (~20 cents), drift up to +-0.35 % (~6 cents) times movement */
    const int32_t det = (k1 * 393) >> 15;                                   /* Q15 fraction */
    const int32_t dr = ((v->drift * k3) >> 15) * 115 >> 15;
    int step = elg_interval(k0);
    if (step > 4) step = 4;

    /* cutoff: 40 Hz .. 6 kHz plus +-1.5 octaves of LFO; f = 2 sin(pi fc / fs), Q15, fs = 88.2 kHz: 93.5 .. ~13500 */
    int32_t cx = (k2 * 29500) >> 15;
    cx += ((lfo * k3) >> 15) * 6144 >> 15;
    int32_t f = (187 * exp2q(cx)) >> 13;                                    /* 2 sin(pi 40 / 88200) in Q15 is 93.5 */
    if (f > 13500) f = 13500;
    if (f < 60) f = 60;
    const int32_t q = 20000;                                                /* damping, mild resonance (elg_dsp.S: 40000 = 2 q) */
    (void)q;
    const int32_t drive = 4096 + ((k2 * 12288) >> 15);                      /* Q12, 1 .. 4 */
    /* level swell: down to 70 % at the LFO's peak, with movement */
    const int32_t swell = 32767 - (((((lfo + 32767) >> 1) * k3) >> 15) * 9830 >> 15);
    /* Static trim follows base timbre/interval, never the moving cutoff or
     * audio level. Bound -6.02..+3dB: retain dark drones, swell and velocity. */
#ifdef KENNEL_LEVEL_CALIBRATION
    const int32_t trim=4096; /* offline generator: original production gain */
#else
    const int32_t trim_pos=k2*16,trim_index=trim_pos>>15;
    const int32_t trim_a=elg_level_trim[step*17+trim_index];
    const int32_t trim=trim_a+(((elg_level_trim[step*17+trim_index+1]-trim_a)*(trim_pos&32767))>>15);
#endif
    const int32_t voice_gain=(swell * ((15180 * vel) >> 7)) >> 15;   /* 15180: +2.8 dB (level pass 2026-10-29, was 11000) */
    int32_t gain=(((voice_gain*trim)>>12)*(knobs[7] > 0 ? knobs[7] : 8192))>>13;   /* and the preset trim (word 7) */
    if (gain>32767) gain=32767;

    /* Preserve the intentionally stepped interval selector; smooth continuous coefficients. */
    const int32_t target[5] = {det, dr, f, drive, gain};
    int32_t control[5], delta[5];
    for (int j = 0; j < 5; j++) {
        const int32_t previous = v->control_ready ? v->control[j] : target[j];
        control[j] = previous * ENGINE_BLOCK;
        delta[j] = target[j] - previous;
        v->control[j] = target[j];
    }
    v->control_ready = 1;
#ifdef __bfin__
    /* elg_dsp.S: the loop below in five passes, bit for bit (the ramps as control << 9: the high half is
     * control >> 7, |control| < 2^22); L0..L3 come back as the caller had them, as mbira.c does */
    int32_t peak;
    {
        uint32_t l0, l1, l2, l3;
        __asm__ volatile("%0 = L0;\n\t%1 = L1;\n\t%2 = L2;\n\t%3 = L3;" : "=d"(l0), "=d"(l1), "=d"(l2), "=d"(l3));
        const int32_t ci[5] = { control[1] * 512, delta[1] * 512, control[0] * 512, delta[0] * 512, step };
        elg_inc(elg_t, (const uint32_t *)ENGINE_PITCH, ci);
        uint32_t ph[3] = { v->ph[0] ^ 0x80000000u, v->ph[1] ^ 0x80000000u, v->ph[2] ^ 0x80000000u };
        elg_mix(ph, elg_t, out);
        elg_shape(out, control[3] * 512, delta[3] * 512);
        int32_t st[2] = { v->low, v->band };
        const int32_t cs[5] = { control[2] * 512, delta[2] * 512, control[4] * 512, delta[4] * 512, 40000 };
        elg_svf(out, st, cs);
        peak = elg_peak(out);
        __asm__ volatile("L0 = %0;\n\tL1 = %1;\n\tL2 = %2;\n\tL3 = %3;" : : "d"(l0), "d"(l1), "d"(l2), "d"(l3));
        v->ph[0] = ph[0] ^ 0x80000000u;
        v->ph[1] = ph[1] ^ 0x80000000u;
        v->ph[2] = ph[2] ^ 0x80000000u;
        v->low = st[0];
        v->band = st[1];
    }
#else
    uint32_t p0 = v->ph[0], p1 = v->ph[1], p2 = v->ph[2];
    int32_t low = v->low, band = v->band, peak = 0;
    for (int i = 0; i < ENGINE_BLOCK; i++) {
        const int32_t det = control[0] >> 7, dr = control[1] >> 7, f = control[2] >> 7;
        const int32_t drive = control[3] >> 7, gain = control[4] >> 7;
        const uint32_t inc = ENGINE_PITCH[i];
        const uint32_t base = inc + mul_u32_s(inc, dr, 15);
        p0 += base;
        p1 += base + mul_u32_s(base, det, 15);
        /* base * ratio >> 12: ratio is 2048 * {2, 3, 4, 6, 8}, so base * (ratio >> 11) >> 1, mod 2^32 */
        p2 += ((base >> 1) * (uint32_t)(ratio[step] >> 11) + ((base & 1) * (uint32_t)(ratio[step] >> 11) >> 1))
              - mul_u32_s(base, det, 16);
        int32_t x = div3(((int32_t)(p0 >> 16) - 32768) + ((int32_t)(p1 >> 16) - 32768)
                         + ((int32_t)(p2 >> 16) - 32768));
        /* growl: drive into a soft clip (1.5x - 0.5x^3) */
        x = (x * drive) >> 12;
        if (x > 32767) x = 32767;
        if (x < -32767) x = -32767;
        x = (x * (98304 - ((x * x) >> 15))) >> 16;
        /* Chamberlin state-variable low-pass */
        low += (f * band) >> 15;
        const int32_t high = x - low - ((q * band) >> 15);
        /* at the top cutoff on high notes |high| passes 80000 and f * high (f <= 27000) overflowed 32 bits */
        band += mul_q15(high, f);
        if (band > 65535) band = 65535;
        if (band < -65535) band = -65535;
        const int32_t y = engine_sat16(mul_q15(low, gain));            /* same for low * gain */
        out[i] = (int16_t)y;
        for (int j = 0; j < 5; j++) control[j] += delta[j];
        peak = y > peak ? y : -y > peak ? -y : peak;
    }
    v->ph[0] = p0;
    v->ph[1] = p1;
    v->ph[2] = p2;
    v->low = low;
    v->band = band;
#endif
    elg_blocks++;
    elg_level = peak;
    elg_lfo = (lfo * k3) >> 15;
}
