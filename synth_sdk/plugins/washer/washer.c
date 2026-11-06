/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* "washer": chorus / ensemble / flanger. Three taps of one short delay line, swept by a triangle LFO a third
 * of a turn apart (ensemble); more foam moves the taps closer and feeds them back (flanger).
 * blue   (knob 0): spin speed, LFO rate 0.05 .. 8 Hz (exponential)
 * green  (knob 1): depth of the sweep
 * white  (knob 2): foam: 0 = chorus (12 ms, no feedback) .. full = flanger (1 ms, 85 % feedback)
 * red    (knob 3): door: mix, dry 1 -> 0.5 while the wet rises 0 -> 1 (chorus adds, it does not replace)
 * Per sample: 3 interpolated reads, one write, a few multiplies.
 */
#include "hk/fx.h"

#define LEN 2048   /* 46 ms */

typedef struct {
    int16_t buf[LEN];
    uint32_t w, ph;
} state_t;

static FX_BUFFER state_t st[FX_INSTANCES];
volatile uint32_t washer_phase;   /* LFO phase and blocks processed, for the screen */
volatile uint32_t washer_blocks;
volatile int32_t washer_level;    /* wet level, Q15 */

void fx_reset(int inst)
{
    state_t *s = &st[inst];
    for (int i = 0; i < LEN; i++) s->buf[i] = 0;
    s->w = 0;
    s->ph = 0;
}

/* 2^(x / 4096) for x in 0..32767, Q12 result scaled by 4096 (so 4096 .. 4096 * 256) */
static int32_t exp2q(int32_t x)
{
    const int32_t e = x >> 12, f = x & 4095;
    return (4096 + ((f * (2839 + ((f * 1257) >> 12))) >> 12)) << e;   /* 1 + f (0.693 + 0.24 f) */
}

static inline int32_t tap(const int16_t *buf, uint32_t w, int32_t d8)
{
    const uint32_t pos = (w << 8) - (uint32_t)d8;            /* Q8 position */
    const uint32_t i = (pos >> 8) & (LEN - 1);
    const int32_t f = (int32_t)(pos & 255);
    const int32_t a = buf[i], b = buf[(i + 1) & (LEN - 1)];
    return a + (((b - a) * f) >> 8);
}

/* x / 3 (C truncation) for |x| < 2^17 without libgcc's __divsi3 (a 32-step loop on the Blackfin);
   43691 = ceil(2^17 / 3), split so no product exceeds 32 bits. Same as tools/engines/elg/elg.c. */
static inline int32_t div3(int32_t x)
{
    const uint32_t a = (uint32_t)(x < 0 ? -x : x);
    const int32_t q = (int32_t)(((a >> 8) * 43691u + (((a & 255) * 43691u) >> 8)) >> 9);
    return x < 0 ? -q : q;
}

/* the params as each instance hears them (after the LFO), for the screen */
volatile int16_t washer_knobs[FX_INSTANCES][4];
volatile uint32_t washer_kblocks[FX_INSTANCES];

void fx_process(int inst, int16_t *io, int n, const int16_t *knobs)
{
    for (int i = 0; i < 4; i++) washer_knobs[inst][i] = knobs[i];
    washer_kblocks[inst]++;
    state_t *s = &st[inst];
    const int32_t k0 = knobs[0] < 0 ? 0 : knobs[0], k1 = knobs[1] < 0 ? 0 : knobs[1];
    const int32_t k2 = knobs[2] < 0 ? 0 : knobs[2], k3 = knobs[3] < 0 ? 0 : knobs[3];

    /* 0.05 Hz .. 8 Hz: increment 4870 .. 779000 (2^32 f / 44100), 7.3 octaves over the knob */
    const uint32_t inc = (uint32_t)((4870 * (int64_t)exp2q((k0 * 29900) >> 15)) >> 12);
    const int32_t base = (529 << 8) - ((k2 * 485) >> 7);              /* Q8 samples, 12 ms .. 1 ms */
    int32_t depth = (k1 * (176 << 8)) >> 15;                          /* up to 4 ms */
    if (depth > base - (2 << 8)) depth = base - (2 << 8);
    const int32_t fb = (k2 * 27853) >> 15;                            /* feedback up to 0.85, Q15 */
    int32_t wet_sum = 0;

    uint32_t w = s->w, ph = s->ph;
    for (int i = 0; i < n; i++) {
        int32_t wet = 0, first = 0;
        for (int t = 0; t < 3; t++) {
            const uint32_t p = ph + (uint32_t)t * 0x55555555u;
            const int32_t tri = (int32_t)(p >> 16) - 32768;              /* saw +-32768 */
            const int32_t lfo = (tri < 0 ? -tri : tri) * 2 - 32768;     /* triangle +-32768 */
            /* 32-bit is exact: 0 <= depth <= 45054, |lfo| <= 32768, so |depth * lfo| < 2^31 */
            const int32_t y = tap(s->buf, w, base + ((depth * lfo) >> 15));
            if (!t) first = y;
            wet += y;
        }
        wet = div3(wet);                                               /* 3 taps of int16: |wet| < 2^17 */
        const int32_t x = io[i];
        s->buf[w & (LEN - 1)] = fx_sat16(x + ((first * fb) >> 15));
        w++;
        ph += inc;
        io[i] = fx_sat16(x - ((x * k3) >> 16) + ((wet * k3) >> 15));   /* dry 1 .. 0.5, wet 0 .. 1 */
        wet_sum += wet < 0 ? -wet : wet;
    }
    s->w = w;
    s->ph = ph;
    if (inst != FX_MASTER) {
        washer_phase = ph;
        washer_blocks++;
        washer_level = wet_sum / (n ? n : 1);
    }
}
