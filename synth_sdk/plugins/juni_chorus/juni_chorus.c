/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* "juni chorus": a stereo ensemble chorus in the manner of an early-80s polysynth BBD chorus (modes I, II, I+II).
 * Pairs with the JUNI engine; see README.md.
 *
 *   blue  (knob 0): MODE. I (slow) -> II (a bit faster) -> I+II (fast and shallow), smooth crossfades between
 *   green (knob 1): DEPTH of the delay sweep (never fully off)
 *   white (knob 2): TONE: darkness of the wet path (a fresh to a tired BBD)
 *   red   (knob 3): MIX: the wet rises 0 -> 0.7 on top of the whole dry (as the original: dry and wet together)
 *
 * One modulated delay per side, driven by one triangle LFO, the sides 180 degrees apart:
 *   stereo (master): L = dry + delay(L, centre + lfo), R = dry + delay(R, centre - lfo)
 *   mono (synth, drum): dry + delay(x, centre + lfo), the left side alone, as the original's mono output
 * Integers only. 16 x 16 products are one Blackfin fractional multiply (fmul). The state is tiny (two 256 sample
 * rings per position, mono uses one), so it lives in .bss and needs no scratch memory. Each 64 sample block runs in small passes.
 * Rate: R246 calls an FX with 64 samples at 44,100 Hz; every constant below is at that rate (2026-11-03: they were
 * written for 22,050 Hz, so the LFOs ran at twice their rates and the delays at half their times on the unit).
 */
#include "op1_plugin.h"

#define RING   256                       /* samples per ring: longest delay 234 + 1, read before the block is written */
#define CENTRE (154 << 8)                /* Q8 samples: 3.5 ms at 44.1 kHz */
#define SWEEP  (80 << 8)                 /* Q8: +-1.8 ms, so 1.7 .. 5.3 ms at full depth in modes I and II */
#define SWEEP3 (20 << 8)                 /* the same in I+II: shallow, +-0.45 ms */
/* the read pass runs before the write pass: the shortest delay must lie behind the whole block, the longest within the
 * ring (the interpolation reads one more sample) */
_Static_assert(CENTRE - SWEEP >= (OP1_FX_BLOCK + 2) << 8 && CENTRE + SWEEP + (1 << 8) < RING << 8, "ring");

/* LFO increments per sample at 44.1 kHz (2^32 f / 44100); the rates are approximations, see README.md */
#define INC_I   48696u                   /* 0.5 Hz */
#define INC_II  77913u                   /* 0.8 Hz */
#define INC_III 876524u                  /* 9 Hz */

typedef struct {
    int16_t ring[2][RING];
    int32_t lp[2][3];                    /* the wet path per side: two low-pass poles, the high-pass's state (Q8) */
    uint32_t w, ph;
    int32_t depth;                       /* Q16 of the Q8 sweep, ramped over each block */
} state_t;

static state_t st[3];
volatile uint32_t juni_chorus_blocks[3];
/* what the screen (juni_chorus_screen.c) reads, written once per block, never read back here: the LFO phase after the
 * block, its increment, the ramped sweep (Q8 samples) and the input's peak (every 8th sample) */
volatile uint32_t juni_chorus_ph[3], juni_chorus_inc[3];
volatile int32_t juni_chorus_depth[3];
volatile int16_t juni_chorus_level[3];

static void telemetry(int pos, const state_t *s, uint32_t inc, const int16_t *a, const int16_t *b)
{
    int32_t pk = 0;
    for (int i = 0; i < OP1_FX_BLOCK; i += 8) {
        int32_t v = a[i] < 0 ? -a[i] : a[i];
        if (v > pk) pk = v;
        if (b) { v = b[i] < 0 ? -b[i] : b[i]; if (v > pk) pk = v; }
    }
    juni_chorus_ph[pos] = s->ph;
    juni_chorus_inc[pos] = inc;
    juni_chorus_depth[pos] = s->depth >> 16;
    juni_chorus_level[pos] = (int16_t)(pk > 32767 ? 32767 : pk);
}

/* (a * b) << 1 for 16-bit a and b: one fractional multiply on Blackfin (a plain int product is a 32 x 32 one) */
#ifdef __bfin__
static inline __attribute__((always_inline)) int32_t fmul(int32_t a, int32_t b)
{ return __builtin_bfin_mult_fr1x32((short)a, (short)b); }
#else
static inline int32_t fmul(int32_t a, int32_t b)
{ int32_t p = (int32_t)(int16_t)a * (int16_t)b; return p == 0x40000000 ? 0x7FFFFFFF : p * 2; }
#endif

static void init(int pos)
{
    state_t *s = &st[(unsigned)pos > 2 ? 0 : pos];
    for (int i = 0; i < RING; i++) s->ring[0][i] = s->ring[1][i] = 0;
    for (int i = 0; i < 3; i++) s->lp[0][i] = s->lp[1][i] = 0;
    s->w = 0;
    s->ph = 0;
    s->depth = 0;
}

typedef struct { uint32_t inc; int32_t depth_q8; int32_t a, gd, gw; } block_t;

/* floor(sqrt(v)), once per block (TONE) */
static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* the four knobs (Q15) -> what this block needs */
static void block_params(block_t *b, const int16_t *p)
{
    const int32_t k0 = op1_fx_param_q15(p, 0), k1 = op1_fx_param_q15(p, 1);
    const int32_t k2 = op1_fx_param_q15(p, 2), k3 = op1_fx_param_q15(p, 3);
    /* MODE: I below 9216, II 13312..19456, I+II above 23552; linear ramps between (4096 wide) */
    int32_t t1 = (k0 - 9216) * 8, t2 = (k0 - 19456) * 8;
    t1 = t1 < 0 ? 0 : (t1 > 32767 ? 32767 : t1);
    t2 = t2 < 0 ? 0 : (t2 > 32767 ? 32767 : t2);
    const uint32_t r2 = INC_I + (uint32_t)(((int32_t)(INC_II - INC_I) * t1) >> 15);
    b->inc = r2 + (uint32_t)((((int32_t)(INC_III - INC_II) >> 6) * t2) >> 9);
    const int32_t dm = SWEEP - (((SWEEP - SWEEP3) * t2) >> 15);               /* 7680 * 32767 < 2^31 */
    const int32_t dk = 6554 + ((k1 * 26214) >> 15);                           /* 0.2 .. 1.0: never fully static */
    b->depth_q8 = fmul(dm, dk) >> 16;
    /* TONE: the design's one-pole coefficient at 22.05 kHz, 0.92 (about 8.6 kHz) .. 0.31 (about 1.3 kHz), two poles
     * in series; at 44.1 kHz the same corner needs 1 - a' = sqrt(1 - a) (Q15: 23,245 .. 5,575) */
    const int32_t a22 = 30000 - ((k2 * 19800) >> 15);
    b->a = 32768 - (int32_t)isqrt((uint32_t)(32768 - a22) << 15);
    b->gd = 0;                                                                /* the dry stays whole */
    b->gw = (k3 * 22938) >> 15;                                               /* wet 0 .. 0.7: the wet is high-passed, so the
                                                                                * level holds within 0.3 dB (native takes) */
}

/* pass 1: the LFO as a signed delay offset in Q8 samples (-depth .. +depth); the depth ramps to the new value */
static void lfo_pass(state_t *s, const block_t *b, uint32_t inc, int16_t *off)
{
    int32_t d = s->depth;
    const int32_t dd = ((b->depth_q8 << 16) - d) >> 6;
    uint32_t ph = s->ph;
    for (int i = 0; i < OP1_FX_BLOCK; i++) {
        const int32_t t = (int32_t)ph >> 16;                 /* saw -32768 .. 32767 */
        const int32_t tri = 2 * (t ^ (t >> 31)) - 32767;     /* triangle */
        off[i] = (int16_t)(fmul(d >> 16, tri) >> 16);
        d += dd;
        ph += inc;
    }
    s->depth = d;
    s->ph = ph;
}

static void write_pass(int16_t *ring, uint32_t w, const int16_t *in)
{
    for (int i = 0; i < OP1_FX_BLOCK; i++) ring[(w + (uint32_t)i) & (RING - 1)] = in[i];
}

/* pass 3: one linearly interpolated tap, CENTRE + sign * off behind the write position */
static void read_pass(const int16_t *ring, uint32_t w, const int16_t *off, int neg, int16_t *out)
{
    for (int i = 0; i < OP1_FX_BLOCK; i++) {
        const int32_t o = neg ? -off[i] : off[i];
        const uint32_t pos = ((w + (uint32_t)i) << 8) - (uint32_t)(CENTRE + o);
        const uint32_t k = (pos >> 8) & (RING - 1);
        const int32_t f = (int32_t)(pos & 255);
        const int32_t a = ring[k], c = ring[(k + 1) & (RING - 1)];
        out[i] = (int16_t)(a + (fmul((c - a) >> 1, f << 7) >> 15));
    }
}

/* pass 4: the wet path: a one-pole high-pass at ~250 Hz (the bass stays dry: a 3.5 ms delay would otherwise cancel
 * the low notes' fundamentals against the dry), then two one-pole low-passes, y += (x - y) * a (rounded) */
#define HP_A 1147                                /* 1 - exp(-2 pi 250 / 44100), Q15 */
static void lp_pass(int16_t *x, int32_t *lp, int32_t a)
{
    int32_t y1 = lp[0], y2 = lp[1], h = lp[2];
    for (int i = 0; i < OP1_FX_BLOCK; i++) {
        h += ((x[i] * 256 - h) >> 5) * HP_A >> 10;             /* Q8: no dead band (a rounded 16-bit pole stalls
                                                                * up to 7 LSB from zero at this corner) */
        y1 += (fmul((x[i] - (h >> 8) - y1) >> 1, a) + 16384) >> 15;
        y2 += (fmul((y1 - y2) >> 1, a) + 16384) >> 15;
        x[i] = (int16_t)y2;
    }
    lp[0] = y1;
    lp[1] = y2;
    lp[2] = h;
}

/* pass 5: very soft saturation of the wet signal (x - 0.12 x^3 at full scale) and the mix into io */
static void mix_pass(int16_t *io, const int16_t *wet, int32_t gd, int32_t gw)
{
    for (int i = 0; i < OP1_FX_BLOCK; i++) {
        const int32_t x = wet[i];
        const int32_t x2 = fmul(x, x) >> 16, x3 = fmul(x2, x) >> 16;
        const int32_t y = x - (fmul(x3, 3932) >> 16);
        io[i] = op1_sat16(io[i] - (fmul(io[i], gd) >> 16) + (fmul(y, gw) >> 16));   /* gd: dry loss */
    }
}

static void stereo(int pos, int16_t *l, int16_t *r, const int16_t *p)
{
    state_t *s = &st[(unsigned)pos > 2 ? 0 : pos];
    block_t b;
    int16_t off[OP1_FX_BLOCK], wl[OP1_FX_BLOCK], wr[OP1_FX_BLOCK];
    block_params(&b, p);
    const uint32_t w = s->w;
    lfo_pass(s, &b, b.inc, off);
    telemetry((unsigned)pos > 2 ? 0 : pos, s, b.inc, l, r);
    read_pass(s->ring[0], w, off, 0, wl);                   /* before the write: the static assert at RING */
    read_pass(s->ring[1], w, off, 1, wr);
    write_pass(s->ring[0], w, l);
    write_pass(s->ring[1], w, r);
    lp_pass(wl, s->lp[0], b.a);
    lp_pass(wr, s->lp[1], b.a);
    mix_pass(l, wl, b.gd, b.gw);
    mix_pass(r, wr, b.gd, b.gw);
    s->w = w + OP1_FX_BLOCK;
    juni_chorus_blocks[(unsigned)pos > 2 ? 0 : pos]++;
}

/* mono (synth, drum): one tap and the dry, as the original's mono output (one chorus side). The two opposite taps
 * summed would cancel each other's pitch movement and leave only a faint comb (host measure, 2026-10-28) */
static void mono(int pos, int16_t *io, const int16_t *p)
{
    state_t *s = &st[(unsigned)pos > 2 ? 0 : pos];
    block_t b;
    int16_t off[OP1_FX_BLOCK], wa[OP1_FX_BLOCK];
    block_params(&b, p);
    const uint32_t w = s->w;
    lfo_pass(s, &b, b.inc, off);
    telemetry((unsigned)pos > 2 ? 0 : pos, s, b.inc, io, 0);
    read_pass(s->ring[0], w, off, 0, wa);
    write_pass(s->ring[0], w, io);
    lp_pass(wa, s->lp[0], b.a);
    mix_pass(io, wa, b.gd, b.gw);
    s->w = w + OP1_FX_BLOCK;
    juni_chorus_blocks[(unsigned)pos > 2 ? 0 : pos]++;
}

const op1_fx_stereo_t FX_juni_chorus = {
    { init, mono, { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE } }, stereo };
