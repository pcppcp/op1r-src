/* "in a room" - Alvin Lucier, "I am sitting in a room" (1969/70). Lucier recorded a paragraph of speech, played
 * it back into a room and re-recorded it, again and again, until the words melted into the room's own
 * resonances. Here the room is a model, the tape is a loop in the FX scratch buffer (22.05 kHz, up to 3.8 s,
 * lo-fi on purpose; 11.025 kHz for longer loops, up to 7.6 s) and every pass of the loop is one generation:
 * playback -> room -> peak-normalise -> written back in place. You hear the loop (wet); the dry signal passes
 * underneath with the mix knob. (Source folder and symbols keep the old name "sitting".)
 *
 *   knob 0 (blue)  : room size     longest wall 0.8 .. 8 m (log; 6.1 m at 88 %); the other two scale with it
 *   knob 1 (green) : shape         4 rooms (L : W : H): CUBE 1:1:1, GOLDEN 2.618:1.618:1,
 *                                  BOLT 1.59:1.26:1, LUCIER 6.1 x 4.3 x 2.6 m (20' x 14' x 8.5': plausible
 *                                  for his Middletown living room, not measured)
 *   knob 2 (white) : erosion       how hard each generation is filtered: resonator Q 8 .. 80 and
 *                                  room share 12 .. 100 % (the rest is the direct sound + early reflections)
 *   knob 3 (orange): mix           dry .. wet (wet = the loop)
 * encoder press:
 *   blue press  -> page 2: loop length of the next capture (params[5]): the first 40 % FREE 0.5 .. 7.6 s (2.6 s at
 *                  the default 8100, as before; over 3.8 s at 11.025 kHz), then 6 tempo-synced lengths (screen
 *                  names): 1/16, 1/8, 1/4, 1/2 (= 1, 2, 4, 8 quarter-beat grid points), 1 BAR, 2 BARS (4/4)
 *   green press -> page 2: absorption, high-frequency loss per generation (none .. one-pole ~1.3 kHz; params[6])
 *   white press -> page 2: crossfade at the loop boundary of the next capture, off .. 93 ms (params[7]): the loop's
 *                  end blends into the input just before its start instead of fading both ends to silence
 *   orange press-> LATCH: every toggle starts a new capture (generation 0): the input is recorded for the loop
 *                  length, then the erosion starts. Before the first capture the wet signal is silence.
 *                  Synced length: the press ARMS the capture (the old loop keeps playing); it starts on the next
 *                  beat (bar lengths: the next bar line, 2 beats: beat 1 or 3), records exactly the musical length
 *                  on the beat clock and loops on it: every generation starts on the grid. Press again while
 *                  armed to cancel.
 *
 * Sync: the OP-1 beat clock (op1_beat_clock: the metronome's, read per block) gives the time to the next beat; the
 * quarter-beat grid points are found per block (at most one per 64 samples) and counted: capture and generations end
 * after exactly `lq` grid points, at the output sample of the grid point - so the loop length follows the clock to
 * the sample and never drifts. While the clock stands still (it follows the tape and the tape is stopped) a copy
 * runs on at the tempo. Bars come from the tape position while the clock follows the tape (as the accent click),
 * else the first synced capture defines beat 1. A loop that does not fit 3.8 s at 22.05 kHz is recorded at
 * 11.025 kHz (7.6 s, screen: LO-FI); one that does not fit that either is halved (screen: "1 BAR AS 1/2"). Tempo
 * or tape speed changes while a loop plays: the loop stays on the grid; faster cuts its end (de-clicked); slower
 * restarts it on the largest power-of-two part of its length that the recording still fills (within sqrt 2, e.g.
 * every beat for 1 BAR at 25 % tape speed), so a slowdown of less than that leaves a short gap before the next start.
 *
 * Room model (per loop sample): 3 early reflections (round trips across L, W, H, from a small delay line) + 6
 * resonant peaks at the room's axial modes f = n c / 2D (c = 343 m/s; the 6 lowest modes of the three dimensions at
 * or above 110 Hz, coincident modes merged into one stronger peak, as in a cube). Resonators: Chamberlin SVFs with
 * 32-bit state and Q15 coefficients (2 sin(pi f/fs) by a Taylor series, per knob change), stable down to the lowest
 * mode. Then a DC blocker, the absorption low-pass and the normalisation gain, updated per generation from the peaks
 * the last pass read and wrote (see next_generation), so the loop neither explodes nor dies (two divisions per
 * generation). Memory (scratch of this FX position): loop 84096 x int16 + early-reflection line 2048 x int16 +
 * crossfade pre-roll ring 2048 x int16.
 * Copyright (C) 2026 ultralazr. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"

#define NMAX     84096                 /* loop samples: 3.8 s at 22.05 kHz, 7.6 s at 11.025 kHz */
#define NMIN     11025                 /* 0.5 s */
#define ERLEN    2048                  /* early-reflection line (93 ms), power of two */
#define RING     2048                  /* input pre-roll at 22.05 kHz for the crossfade (93 ms), power of two */
#define NMODE    6
#define FADE     220                   /* free loop: 10 ms fade at both ends of a capture: seamless, no click */
#define SFADE    32                    /* synced loop: 1.5 ms (keeps the downbeat's attack) */
#define GRAMP_SH 8
#define GRAMP    (1 << GRAMP_SH)          /* loop samples a new generation's gain takes over (11.6 ms at 22.05 kHz) */
#define TARGET   16384                 /* normalised peak of a generation: -6 dBFS */
#define KNEE     24576                 /* soft knee above -2.5 dBFS when writing back */
#define WB       128                   /* waveform buckets for the screen */
#define FLO_Q8   (110 << 8)            /* lowest mode, Hz Q8 */
#define FREE_V   13107                 /* page-2 blue below this (40 %): free length, above: 6 synced lengths */
#define NSYNC    6
enum { S_IDLE = 0, S_REC, S_ERODE };
static const uint8_t sync_lq[NSYNC] = { 1, 2, 4, 8, 16, 32 };   /* length in quarter beats */
static inline int sync_sel(int32_t w5) { return w5 < FREE_V ? -1 : ((w5 - FREE_V) * 20) >> 16; }   /* 0..5 */
/* free length in samples at 22.05 kHz: 0.5 .. 2.6 s (the default, 8100) as before, then steeper up to 7.6 s; longer
 * than NMAX runs at 11.025 kHz */
static inline int32_t free_len(int32_t w5)
{
    if (w5 <= 8100) return NMIN + ((w5 * 46876) >> 13);
    int32_t n = 57374 + (((w5 - 8100) * 22664) >> 10);
    return n > 2 * NMAX ? 2 * NMAX : n;
}

/* ---------------- room geometry (shared by the audio and the screen thread) ---------------- */
typedef struct {
    int32_t dim[3];                    /* L, W, H in mm */
    int32_t f[NMODE];                  /* mode frequencies, Hz Q8 */
    uint8_t ax[NMODE];                 /* dimension bitmask (1 L, 2 W, 4 H): >1 bit = degenerate mode */
    uint8_t n[NMODE];                  /* order of the first dimension */
    uint8_t deg[NMODE];                /* how many axial modes coincide (1..3) */
    uint8_t shape;
} room_t;
static const int32_t shape_ratio[4][2] = {   /* W/L, H/L in Q15 */
    { 32767, 32767 },                  /* CUBE   1 : 1 : 1                         */
    { 20252, 12517 },                  /* GOLDEN 1 : 0.618 : 0.382                 */
    { 25967, 20608 },                  /* BOLT   1.59 : 1.26 : 1 (Bolt's area)     */
    { 23098, 13966 },                  /* LUCIER 6.1 x 4.3 x 2.6 m                 */
};

/* 2^(x / 32768) * 32768 for x >= 0 (quadratic fraction, 0.3 % error) */
static int32_t exp2_q15(int32_t x)
{
    int32_t i = x >> 15, f = x & 32767;
    int32_t m = 32768 + ((f * 21512) >> 15) + ((((f * f) >> 15) * 11256) >> 15);   /* 1 + .6565 f + .3435 f^2 */
    return m << i;
}

static void room_calc(room_t *r, int32_t ksize, int32_t kshape)
{
    int sh = kshape * 4 >> 15; if (sh > 3) sh = 3;
    r->shape = (uint8_t)sh;
    int32_t L = (800 * exp2_q15((ksize * 54426) >> 14)) >> 15;            /* 0.8 .. 8 m: 2^(k * log2 10) */
    r->dim[0] = L;
    r->dim[1] = (L * shape_ratio[sh][0]) >> 15;
    r->dim[2] = (L * shape_ratio[sh][1]) >> 15;
    int32_t f1[3], n[3], cur[3];
    for (int d = 0; d < 3; d++) {
        f1[d] = 43904000 / r->dim[d];                                        /* c / 2D, Hz Q8 */
        n[d] = (FLO_Q8 + f1[d] - 1) / f1[d];                                 /* first order >= 110 Hz */
        cur[d] = n[d] * f1[d];
    }
    int cnt = 0;
    for (int it = 0; it < 24 && cnt < NMODE; it++) {                         /* merge the three series */
        int d = cur[0] <= cur[1] ? 0 : 1;
        if (cur[2] < cur[d]) d = 2;
        int32_t f = cur[d];
        if (cnt && f <= r->f[cnt - 1] + (r->f[cnt - 1] >> 7)) {              /* within 0.8 %: degenerate */
            r->deg[cnt - 1]++; r->ax[cnt - 1] |= (uint8_t)(1 << d);
        } else {
            r->f[cnt] = f; r->ax[cnt] = (uint8_t)(1 << d); r->n[cnt] = (uint8_t)n[d]; r->deg[cnt] = 1; cnt++;
        }
        n[d]++; cur[d] += f1[d];
    }
}

/* 2 sin(pi f / 22050) in Q15 (f Hz Q8), Taylor to the 5th power in Q30; per knob change only */
static int32_t svf_coef(int32_t f_q8)
{
    if (f_q8 > (3400 << 8)) f_q8 = 3400 << 8;
    int64_t t = ((int64_t)f_q8 * 2447717) >> 12;                             /* pi f / fs, Q30 */
    int64_t t2 = (t * t) >> 30, t3 = (t2 * t) >> 30, t5 = (t3 * t2) >> 30;
    int32_t s = (int32_t)t - (int32_t)t3 / 6 + (int32_t)t5 / 120;            /* (32-bit divisions: cheap) */
    return (s + 8192) >> 14;                                                 /* 2 sin, Q15 (<= 0.95) */
}

/* ---------------- DSP ---------------- */
typedef struct { int32_t lp, bp, f, g; } res_t;   /* resonator: states (input x 8), 2 sin(pi f/fs), weight: Q15 */
typedef struct {
    res_t res[NMODE];
    int32_t q15, wroom, wdir, a15;     /* damping 1/Q, room / direct share, absorption LP coef: Q15 */
    int32_t tap[3];                    /* early-reflection delays (loop samples) */
    int32_t dcx, dcy, z, dcs;          /* DC blocker (pole shift), absorption LP */
    int32_t len, i, gen, mode, latch;
    int32_t gq12, gm, rs;              /* normalisation gain Q12 = mantissa gm (Q15) >> rs */
    int32_t gmo, rso, gk;              /* previous gain, ramp samples left: a new generation's gain fades in */
    int32_t gap;                       /* synced: the loop ran out before the grid point (waiting) */
    int32_t peak, ipeak, cpeak;        /* peak written (before the knee) / read this generation, capture peak */
    int32_t pw, x, ph, dsh, lw;        /* loop samples before / now, output sample in it (0 .. 2^dsh - 1), last wet */
    int32_t e1, e2, e3, f1, f2, f3;    /* decimator histories ([1 3 3 1] / 8 per 2:1 stage) */
    int32_t fade, fmul;                /* capture fade length (loop samples), 32768 / fade */
    int32_t xf, xfw;                   /* crossfade length (loop samples, 0 = fades), pre-roll end in the ring */
    int32_t xl, xs;                    /* first erosion pass: samples left blending raw -> room, 32768 / (x + 1) */
    int32_t rgw, r1, r2, r3;           /* pre-roll ring write index, its decimator history */
    int32_t goff;                      /* output sample of the capture start in this block (ring already filled to 64) */
    int16_t inb[OP1_FX_BLOCK + 3];     /* dry input: the last 3 of the previous block, then this block */
    int32_t erw, erc;                  /* early-reflection write index, entries cleared (during REC) */
    int32_t dkv, dkn;                  /* de-click offset when the state changes */
    int32_t rb, ba, cb, bhi, blo;      /* waveform bucket: 128 / len (Q16), position (Q16), current, max / min */
    int32_t kroom, kero, kabs, kdsh;   /* knob values (and rate) the coefficients were made from */
    int32_t env[NMODE];                /* resonator amplitudes for the screen */
    /* sync */
    int32_t lq, qleft, armed;          /* loop length in quarter beats (0 = free), grid points left, capture armed */
    int32_t qph;                       /* grid points since the capture start, mod lq (where the loop's bar is) */
    int32_t bp, blen, tape, tbeat;     /* own beat phase, beat length in real time (1/16384 samples), tape, beat no. */
    int32_t qbar;                      /* quarter in the bar 0..15 (-1: unknown) */
    int32_t plen;                      /* expected loop length (loop samples) of the capture: screen */
    room_t room;
} sit_t;

static sit_t st[3];
static int8_t wave_hi[3][WB], wave_lo[3][WB];   /* loop waveform per bucket (max / min >> 8), for the screen */

/* Blackfin has 16 x 16 multipliers (1 cycle); a 32 x 32 multiply is a multi-cycle instruction. So the 32-bit
 * signals are multiplied by Q15 coefficients in two 16 x 16 halves: a * b / 32768 (floor), |a| < 2^30, b 0..32767 */
static inline int32_t m16(int32_t a, int32_t b)          /* low halves, signed 16 x 16 -> 32 */
{
#ifdef __bfin__                                          /* gcc drops the casts of known-small values and emits a
                                                          * 32-bit multiply; force the 1-cycle MAC multiply */
    int32_t r;
    __asm__("%0 = %1.L * %2.L (IS);" : "=d"(r) : "d"(a), "d"(b));
    return r;
#else
    return (int32_t)(int16_t)a * (int32_t)(int16_t)b;
#endif
}
static inline int32_t mq15(int32_t a, int32_t b)
{ return m16(a >> 15, b) + (m16(a & 0x7fff, b) >> 15); }               /* both halves fit int16 */
static inline int32_t clampi(int32_t x, int32_t m) { return x > m ? m : (x < -m ? -m : x); }

static void set_gain(sit_t *s, int32_t g)          /* Q12, 1/16 .. 64 */
{
    if (g < 256) g = 256; else if (g > 262144) g = 262144;
    s->gq12 = g;
    int32_t m = g << 3, e = 0;                      /* Q15 */
    while (m > 32767) { m >>= 1; e++; }
    while (m < 16384) { m <<= 1; e--; }
    s->gm = m; s->rs = 11 - e;                      /* out = (v * 256) >> rs, v = z * gm / 32768 in Q3 */
}

static void wave_clear(int pos) { for (int b = 0; b < WB; b++) wave_hi[pos][b] = wave_lo[pos][b] = 0; }

static void sit_init(int pos)
{
    sit_t *s = &st[pos % 3];
    s->mode = S_IDLE; s->latch = op1_fx_page[pos % 3][3];
    s->gen = 0; s->i = 0; s->len = s->plen = NMIN; s->dkn = 0;
    s->pw = s->x = s->ph = s->lw = 0; s->dsh = 1;
    s->e1 = s->e2 = s->e3 = s->f1 = s->f2 = s->f3 = 0;
    s->xf = s->xl = 0; s->rgw = s->r1 = s->r2 = s->r3 = 0;
    s->lq = s->armed = 0; s->qbar = -1; s->bp = 0; s->blen = 22050 << 14; s->tape = 0; s->tbeat = -1;
    s->kroom = s->kero = s->kabs = s->kdsh = -1;
    for (int k = 0; k < NMODE; k++) s->env[k] = 0;
    set_gain(s, 4096); s->gk = 0;
    wave_clear(pos % 3);
}

/* synced capture: loop length in samples (44.1 kHz) of lq quarter beats at the current tempo; the rate (dsh) that
 * fits it, the length halved while even 11.025 kHz is too short. Shared with the screen. */
static int32_t sync_fit(int32_t blen, int32_t *lq, int32_t *dsh)
{
    int32_t spb = blen >> 14, lout = (spb * *lq) >> 2;
    while (*lq > 1 && (lout >> 2) + (lout >> 6) + 64 > NMAX) { *lq >>= 1; lout = (spb * *lq) >> 2; }
    *dsh = (lout >> 1) + (lout >> 5) + 64 <= NMAX ? 1 : 2;
    return lout;
}

/* a new capture (generation 0) from output sample n of this block: free (lq 0, length from the knob) or synced */
static void start_capture(sit_t *s, int pos, int32_t w5, int32_t lq, int32_t w7)
{
    s->dkv = s->lw; s->dkn = 256;                   /* the old wet level fades out (de-click) */
    s->mode = S_REC; s->i = 0; s->gen = 0; s->cpeak = 0; s->ph = 0; s->pw = s->x = 0; s->erc = 0;
    if (!lq) {
        int32_t n = free_len(w5);
        s->dsh = n > NMAX ? 2 : 1; s->len = s->plen = n >> (s->dsh - 1);
        s->fade = FADE >> (s->dsh - 1); s->fmul = s->dsh == 1 ? 149 : 297;
    } else {
        int32_t dsh, lout = sync_fit(s->blen, &lq, &dsh);
        int32_t cap = (lout >> dsh) + (lout >> (dsh + 4)) + 64;             /* room for a slower tempo meanwhile */
        s->dsh = dsh; s->len = cap > NMAX ? NMAX : cap; s->plen = lout >> dsh;
        s->fade = SFADE >> (dsh - 1); s->fmul = 32768 / s->fade;
        s->qleft = lq; s->qph = 0;
    }
    s->lq = lq;
    /* crossfade (white page 2): the loop's end is blended into the input just before its start (pre-roll ring), so
     * the wrap is seamless without fading to silence; 0 = the fades as before */
    s->xf = ((w7 < 0 ? 0 : w7) * (RING - 1) >> 15) >> (s->dsh - 1);
    s->xfw = (s->rgw - ((OP1_FX_BLOCK - s->goff) >> 1)) & (RING - 1);   /* the ring sample before loop[0] */
    /* decimator histories from the input just before the start (they were stale: loop[0] came out half) */
    s->e1 = s->inb[s->goff + 2]; s->e2 = s->inb[s->goff + 1]; s->e3 = s->inb[s->goff];
    const int16_t *ring = (const int16_t *)op1_fx_scratch(pos) + NMAX + ERLEN;
    s->f1 = ring[(s->xfw - 1) & (RING - 1)]; s->f2 = ring[(s->xfw - 2) & (RING - 1)]; s->f3 = ring[(s->xfw - 3) & (RING - 1)];
    if (s->xf) s->fade = 0;
    s->rb = (WB << 16) / s->plen;                   /* per capture */
    s->cb = 0; s->ba = 0; s->bhi = s->blo = 0;
    wave_clear(pos);
}

static void start_erode(sit_t *s, int16_t *er)
{
    for (int k = s->erc; k < ERLEN; k++) er[k] = 0; /* the rest (normally none: cleared while recording) */
    s->erc = ERLEN;
    for (int k = 0; k < NMODE; k++) s->res[k].lp = s->res[k].bp = 0;
    s->dcx = s->dcy = s->z = 0; s->erw = 0;
    s->mode = S_ERODE; s->i = 0; s->gen = 0; s->peak = 0; s->ipeak = 0; s->gk = 0; s->gap = 0;
    if (s->xf == 0) s->xl = 0;
    s->ph = 0; s->pw = s->x;                        /* playback starts from the last recorded sample */
    s->cb = 0; s->ba = 0; s->bhi = s->blo = 0;
    /* first pass: normalise from the capture's peak, with a guess of the room's loss (narrow resonators eat
     * more of the speech: x (1 + 3 e^2)); a silent capture stays silent (gain 1, never boosted) */
    int32_t e2 = (s->kero < 0 ? 16384 : s->kero) * (s->kero < 0 ? 16384 : s->kero) >> 15;
    set_gain(s, s->cpeak < 64 ? 4096 : ((((TARGET << 12) / s->cpeak) >> 4) * (4096 + (e2 * 3 >> 3))) >> 8);
}

/* end of a generation: the pass just written (peak S, made with gain G from a pass of peak P) goes through the
 * room next with about the same room gain S / (G P), so the gain that brings it to TARGET is G (T / S) (P / S).
 * Two divisions per generation, each ratio limited to x4 / x0.25; a (near) silent loop is never boosted. */
static void next_generation(sit_t *s)
{
    s->gen++;
    if (s->peak >= 16 && s->ipeak >= 16) {
        int32_t pk = s->peak > 0x7ffff ? 0x7ffff : s->peak;
        int32_t r1 = (TARGET << 12) / pk, r2 = (s->ipeak << 12) / pk;
        r1 = r1 < 1024 ? 1024 : (r1 > 16384 ? 16384 : r1);
        r2 = r2 < 1024 ? 1024 : (r2 > 16384 ? 16384 : r2);
        int32_t g = ((s->gq12 >> 2) * r1) >> 10;
        g = g > 262144 ? 262144 : g;
        set_gain(s, ((g >> 2) * r2) >> 10);
    }
    s->peak = 0; s->ipeak = 0;
}

/* coefficients for the loop's rate: 22.05 kHz (dsh 1) or 11.025 kHz (dsh 2) */
static void update_coefs(sit_t *s, int32_t kroom, int32_t kshape, int32_t kero, int32_t kabs)
{
    const int32_t sh = s->dsh - 1;
    if (s->kdsh != s->dsh) { s->kdsh = s->dsh; s->kroom = s->kabs = -1; s->dcs = 7 - sh; }   /* DC blocker ~27 Hz */
    int32_t key = kroom | ((kshape * 4 >> 15) << 16);
    if (key != s->kroom) {
        s->kroom = key;
        room_calc(&s->room, kroom, kshape);
        for (int k = 0; k < NMODE; k++) {
            s->res[k].f = svf_coef(s->room.f[k] << sh);                      /* 2 sin(pi f / fs) */
            s->res[k].g = 19661 + 6553 * (s->room.deg[k] - 1);             /* 0.6 / 0.8 / 1.0: degenerate = stronger */
        }
        for (int d = 0; d < 3; d++) {                                        /* round trip 2D / c */
            int32_t t = (s->room.dim[d] * 8436) >> (16 + sh);                /* mm * 0.12857 at 22.05 kHz */
            s->tap[d] = t < 1 ? 1 : (t > ERLEN - 1 ? ERLEN - 1 : t);
        }
    }
    if (kero != s->kero) {
        s->kero = kero;
        int32_t qq15 = 8 * exp2_q15((kero * 54426) >> 14);                 /* Q = 8 * 10^e, Q15 */
        s->q15 = (int32_t)(0x40000000u / (uint32_t)qq15);                   /* 1/Q, Q15 */
        s->wroom = 3932 + ((kero * 28835) >> 15);                            /* 12 .. 100 % */
        s->wdir = 32767 - s->wroom;
    }
    if (kabs != s->kabs) {
        s->kabs = kabs; s->a15 = 32767 - ((kabs * 22500) >> 15);
        if (sh) { int32_t a = 2 * s->a15 - ((s->a15 * s->a15) >> 15); s->a15 = a > 32767 ? 32767 : a; }   /* same cutoff */
    }
}

/* waveform bucket for the screen: max / min of the loop samples written into 1/128 of the loop */
static inline void bucket_flush(int pos, int32_t cb, int32_t hi, int32_t lo)
{
    if (cb < WB) { wave_hi[pos][cb] = (int8_t)(hi >> 8); wave_lo[pos][cb] = (int8_t)(lo >> 8); }
}

/* The erosion. Its hot state lives in locals of the caller (two small structs the compiler keeps in registers /
 * on the stack); erode1 runs one loop sample through the room and writes the next generation back in place. */
typedef struct { int32_t t0, t1, t2, q, wd, wr, ab, rb, len, dcs, fr; } cst_t;
typedef struct { int32_t i, erw, dcx, dcy, z, pk, ipk, ba, cb, bhi, blo, gm, rsh, gmo, rso, gk, gap, xl, xs; } hot_t;
#define ALWAYS_INLINE static inline __attribute__((always_inline))
ALWAYS_INLINE void hot_load(sit_t *s, cst_t *c, hot_t *h)
{
    c->t0 = s->tap[0]; c->t1 = s->tap[1]; c->t2 = s->tap[2]; c->q = s->q15; c->wd = s->wdir; c->wr = s->wroom;
    c->ab = s->a15; c->rb = s->rb; c->len = s->len; c->dcs = s->dcs; c->fr = !s->lq;
    h->i = s->i; h->erw = s->erw; h->dcx = s->dcx; h->dcy = s->dcy; h->z = s->z; h->pk = s->peak; h->ipk = s->ipeak;
    h->ba = s->ba; h->cb = s->cb; h->bhi = s->bhi; h->blo = s->blo; h->gm = s->gm; h->rsh = s->rs;
    h->gmo = s->gmo; h->rso = s->rso; h->gk = s->gk; h->gap = s->gap; h->xl = s->xl; h->xs = s->xs;
}
ALWAYS_INLINE void hot_store(sit_t *s, const hot_t *h)
{
    s->i = h->i; s->erw = h->erw; s->dcx = h->dcx; s->dcy = h->dcy; s->z = h->z; s->peak = h->pk; s->ipeak = h->ipk;
    s->ba = h->ba; s->cb = h->cb; s->bhi = h->bhi; s->blo = h->blo; s->gk = h->gk; s->gap = h->gap; s->xl = h->xl;
    s->gmo = h->gmo; s->rso = h->rso;                                        /* a free wrap's ramp spans blocks */
}
/* one loop sample: returns the heard sample (generation g) and writes generation g + 1 in its place; a synced
 * loop that ran out before its grid point fades from its last sample (prev) to silence until the grid point */
ALWAYS_INLINE int32_t erode1(sit_t *s, int pos, int16_t *loop, int16_t *er, const cst_t *c, hot_t *h, int32_t prev)
{
    if (h->i >= c->len) { h->gap = 1; return (prev * 3) >> 2; }
    int32_t x = loop[h->i];                                                  /* generation g (heard) */
    int32_t ax = x < 0 ? -x : x;
    if (ax > h->ipk) h->ipk = ax;
    /* room: direct + 3 early reflections (Q3) */
    int32_t erw = h->erw;
    er[erw] = (int16_t)x;
    int32_t t = er[(erw - c->t0) & (ERLEN - 1)] + er[(erw - c->t1) & (ERLEN - 1)] + er[(erw - c->t2) & (ERLEN - 1)];
    h->erw = (erw + 1) & (ERLEN - 1);
    int32_t e = x * 8 + t * 2 + (t >> 1) + (t >> 2) + (t >> 3);             /* + 3 x 0.36 */
    /* + 6 axial modes: Chamberlin SVFs, bandpass q * bp has peak gain 1 */
    int32_t room = 0;
    res_t *r = s->res;
    for (int k = 0; k < NMODE; k++, r++) {
        int32_t bp = r->bp, f = r->f, lp = r->lp + mq15(bp, f);
        int32_t qb = mq15(bp, c->q);
        r->lp = lp;
        r->bp = clampi(bp + mq15(e - lp - qb, f), 1 << 26);
        room += mq15(qb, r->g);
    }
    int32_t y = mq15(e, c->wd) + mq15(clampi(room, 1 << 22), c->wr);
    h->dcy = y - h->dcx + h->dcy - (h->dcy >> c->dcs); h->dcx = y;           /* DC blocker ~27 Hz */
    h->z += mq15(h->dcy - h->z, c->ab);                                      /* absorption */
    int32_t zc = clampi(h->z, (1 << 21) - 1);
    int32_t o = (mq15(zc, h->gm) * 256) >> h->rsh;                         /* normalisation gain, Q3 -> int16 */
    if (h->gk) {                                    /* a new generation: from the old gain to the new one (no step
                                                     * at the loop boundary, where the room's output runs on) */
        int32_t oo = (mq15(zc, h->gmo) * 256) >> h->rso;
        o += (clampi(oo - o, (1 << 22) - 1) * h->gk) >> GRAMP_SH;
        h->gk--;
    }
    if (h->xl > 0) {                                /* crossfade, first pass: the next generation starts as the raw
                                                     * recording (continuous with the baked end) and turns into the
                                                     * room's output over the crossfade */
        int32_t t = (h->i + 1) * h->xs;
        o = (x * (32768 - t) + clampi(o, 65535) * t) >> 15;
        h->xl--;
    }
    int32_t a = o < 0 ? -o : o;
    if (a > h->pk) h->pk = a;
    if (a > KNEE) {                                                          /* overshoot: 4:1, then 16:1 */
        int32_t d = a - KNEE;
        a = d < 16384 ? KNEE + (d >> 2) : 28672 + ((d - 16384) >> 4);
        o = o < 0 ? -a : a;
    }
    o = op1_sat16(o);
    loop[h->i] = (int16_t)o;                                                 /* generation g + 1 */
    int32_t b = h->ba >> 16;                                                 /* screen waveform */
    h->ba += c->rb;
    if (b != h->cb) { bucket_flush(pos, h->cb, h->bhi, h->blo); h->cb = b; h->bhi = h->blo = 0; }
    if (o > h->bhi) h->bhi = o; else if (o < h->blo) h->blo = o;
    if (++h->i >= c->len && c->fr) {                                         /* free loop: next generation */
        bucket_flush(pos, h->cb, h->bhi, h->blo);
        h->i = 0; h->ba = 0; h->cb = 0; h->bhi = h->blo = 0;
        s->peak = h->pk; s->ipeak = h->ipk;
        h->gmo = h->gm; h->rso = h->rsh; h->gk = GRAMP;
        next_generation(s);
        h->pk = h->ipk = 0; h->gm = s->gm; h->rsh = s->rs;
    }
    return x;
}
static inline int32_t wet(int16_t *buf, int n, int32_t v, int32_t mix)     /* dry .. wet, returns |out| */
{
    int32_t in = buf[n];
    int32_t o = op1_sat16(in + (m16(clampi(v - in, 32767), mix) >> 15));
    buf[n] = (int16_t)o;
    return o < 0 ? -o : o;
}

/* 22.05 kHz, from the start of a loop sample: both its output samples at once (linear 2x upsampling). Returns the
 * output peak; *pn = the first output sample not done (an odd tail is left for erode_run). */
static int32_t erode_pairs(sit_t *s, int pos, int16_t *loop, int16_t *er, int16_t *buf, int *pn, int end, int32_t mix)
{
    cst_t c; hot_t h;
    hot_load(s, &c, &h);
    int32_t pw = s->pw, x = s->x, dkv = s->dkv, dkn = s->dkn, opk = 0;
    int n = *pn;
    for (; n + 1 < end; n += 2) {
        x = erode1(s, pos, loop, er, &c, &h, x);
        int32_t v0 = (pw + x) >> 1, v1 = x;
        pw = x;
        if (dkn) { v0 += m16(dkv, dkn) >> 8; dkn--; v1 += m16(dkv, dkn) >> 8; if (dkn) dkn--; }
        int32_t o0 = wet(buf, n, v0, mix), o1 = wet(buf, n + 1, v1, mix);
        if (o0 > opk) opk = o0;
        if (o1 > opk) opk = o1;
    }
    hot_store(s, &h);
    s->pw = s->x = s->lw = x; s->dkn = dkn;
    *pn = n;
    return opk;
}

/* any rate and position: one output sample at a time (11.025 kHz loops, odd grid points) */
static int32_t erode_run(sit_t *s, int pos, int16_t *loop, int16_t *er, int16_t *buf, int n, int end, int32_t mix)
{
    int32_t opk = 0;
    if (s->dsh == 1 && !s->ph) opk = erode_pairs(s, pos, loop, er, buf, &n, end, mix);
    if (n >= end) return opk;
    cst_t c; hot_t h;
    hot_load(s, &c, &h);
    const int32_t dsh = s->dsh, dm = (1 << dsh) - 1;
    int32_t pw = s->pw, x = s->x, ph = s->ph, dkv = s->dkv, dkn = s->dkn, w = s->lw;
    for (; n < end; n++) {
        if (!ph) x = erode1(s, pos, loop, er, &c, &h, x);                   /* next loop sample */
        w = pw + (((x - pw) * (ph + 1)) >> dsh);                             /* linear upsampling */
        if (ph == dm) { ph = 0; pw = x; } else ph++;
        int32_t v = w;
        if (dkn) { v += m16(dkv, dkn) >> 8; dkn--; }
        int32_t o = wet(buf, n, v, mix);
        if (o > opk) opk = o;
    }
    hot_store(s, &h);
    s->pw = pw; s->x = x; s->ph = ph; s->dkn = dkn; s->lw = w;
    return opk;
}

/* end of a capture with a crossfade: the last xf loop samples move from the recording to the pre-roll (the input
 * just before loop[0]), so loop[len - 1] runs on into loop[0] (one division per capture) */
static void xfade_bake(sit_t *s, int16_t *loop, const int16_t *ring, int32_t len)
{
    int32_t x = s->xf < len / 2 ? s->xf : len / 2;
    s->xl = 0;
    if (x <= 0) return;
    int32_t step = 32768 / (x + 1), d = s->dsh - 1;
    s->xl = x; s->xs = step;                                                 /* and the raw -> room blend (erode1) */
    for (int32_t k = 0; k < x; k++) {
        int32_t t = (k + 1) * step, i = len - x + k;
        int32_t r = (s->xfw - ((x - k) << d)) & (RING - 1);
        int32_t pre = d ? (ring[r] + ring[(r + 1) & (RING - 1)]) >> 1 : ring[r];
        loop[i] = (int16_t)((loop[i] * (32768 - t) + pre * t) >> 15);
    }
}

/* one recorded loop sample (after the decimator) */
static void capture_sample(sit_t *s, int pos, int16_t *loop, int16_t *er, int32_t x)
{
    int32_t a = x < 0 ? -x : x, i = s->i;
    if (a > s->cpeak) s->cpeak = a;
    if (s->erc < ERLEN) {                                                    /* clear the reflection line, spread */
        int16_t *e = er + s->erc;
        e[0] = e[1] = e[2] = e[3] = 0; s->erc += 4;
    }
    if (i < s->len) {
        int32_t j = s->len - 1 - i;
        if (i < s->fade) x = (x * i * s->fmul) >> 15;
        else if (!s->lq && j < s->fade) x = (x * j * s->fmul) >> 15;      /* free: fade out now; synced: at the end */
        loop[i] = (int16_t)x;
        int32_t b = s->ba >> 16;                                             /* screen waveform */
        s->ba += s->rb;
        if (b != s->cb) { bucket_flush(pos, s->cb, s->bhi, s->blo); s->cb = b; s->bhi = s->blo = 0; }
        if (x > s->bhi) s->bhi = x; else if (x < s->blo) s->blo = x;
        s->i = i + 1;
    } else x = 0;                                                            /* synced: full, wait for the grid point */
    s->pw = s->x; s->x = x;
    if (s->i >= s->len && !s->lq) { bucket_flush(pos, s->cb, s->bhi, s->blo); xfade_bake(s, loop, er + ERLEN, s->len); start_erode(s, er); }
}

/* idle (silence) or recording, output samples n .. end - 1; hands over to the erosion when a free capture ends */
static int32_t rec_run(sit_t *s, int pos, int16_t *loop, int16_t *er, int16_t *buf, int n, int end, int32_t mix)
{
    int32_t opk = 0;
    for (; n < end; n++) {
        if (s->mode == S_ERODE) {
            int32_t pk = erode_run(s, pos, loop, er, buf, n, end, mix);
            return pk > opk ? pk : opk;
        }
        int32_t in = buf[n], ph = s->ph;
        int32_t w = s->pw + (((s->x - s->pw) * (ph + 1)) >> s->dsh);        /* monitor: the recording, 1 sample late */
        s->lw = w;
        if (s->mode == S_REC) {
            if (ph & 1) {
                int32_t y = (s->e3 + 3 * s->e2 + 3 * s->e1 + in) >> 3;         /* [1 3 3 1] / 8, then 2:1 */
                if (s->dsh == 1) capture_sample(s, pos, loop, er, y);
                else {                                                       /* 11.025 kHz: the same again */
                    if (ph == 3) capture_sample(s, pos, loop, er, (s->f3 + 3 * s->f2 + 3 * s->f1 + y) >> 3);
                    s->f3 = s->f2; s->f2 = s->f1; s->f1 = y;
                }
            }
            s->e3 = s->e2; s->e2 = s->e1; s->e1 = in;
            if (s->mode == S_REC) s->ph = ph == (1 << s->dsh) - 1 ? 0 : ph + 1;
        }
        if (s->dkn > 0) { w += m16(s->dkv, s->dkn) >> 8; s->dkn--; }         /* de-click a state change */
        int32_t o = op1_sat16(in + (m16(clampi(w - in, 32767), mix) >> 15));
        buf[n] = (int16_t)o;
        if (o < 0) o = -o;
        if (o > opk) opk = o;
    }
    return opk;
}

static int32_t run(sit_t *s, int pos, int16_t *loop, int16_t *er, int16_t *buf, int n, int end, int32_t mix)
{
    if (n >= end) return 0;
    return s->mode == S_ERODE ? erode_run(s, pos, loop, er, buf, n, end, mix) : rec_run(s, pos, loop, er, buf, n, end, mix);
}

/* quarter-beat grid point in this block (per block): its output sample 0..63 or -1, *qpos = quarter in the beat.
 * Time to the next beat from the OP-1 clock while it moves, else from our copy running on at the tempo. */
static int beat_grid(sit_t *s, int32_t *qpos)
{
    op1_beat_t c;
    op1_beat_clock(&c);
    int32_t len = c.len, p, step;
    int moving = len >= (256 << 14) && c.step > 0 && c.step <= 0x400000 && c.next >= 0 && c.next <= len;
    if (len < (256 << 14)) len = 22050 << 14;                                /* no clock: 120 BPM */
    if (moving) { p = c.next; step = c.step; }
    else { p = s->bp < 0 || s->bp > len ? len : s->bp; step = 0x100000; }
    int32_t np = p - step;
    s->bp = np < 0 ? np + len : np;
    int32_t sq = step >> 14, bl = sq > 0 && sq != 64 ? len / sq : len >> 6; /* beat in real time (tape speed) */
    s->blen = bl > 0x1ffffff ? 0x7fffffff : bl << 6;
    s->tape = moving && c.follows_tape;
    int32_t qq = len >> 2;
    for (int j = 3; j >= 0; j--) {                                           /* the earliest point in [0, step) */
        int32_t t = p - j * qq;
        if (t < 0 || t >= step) continue;
        int off = step == 0x100000 ? t >> 14 : (t * 64) / step;              /* (division: tape varispeed only) */
        *qpos = (4 - j) & 3;
        s->tbeat = -1;
        if (!*qpos && s->tape) { int32_t spb = len >> 14; s->tbeat = (c.tape_pos + off + (spb >> 1)) / spb; }
        return off;
    }
    return -1;
}

/* a grid point: start an armed capture, or count down the synced capture / generation */
static void grid_event(sit_t *s, int pos, int16_t *loop, int16_t *er, int32_t qpos, int32_t w5, int32_t w7)
{
    if (!qpos && s->tbeat >= 0) s->qbar = (s->tbeat & 3) << 2;               /* bar from the tape */
    else if (s->qbar >= 0) s->qbar = ((s->qbar & 12) + (qpos ? qpos : 4)) & 15;
    if (s->armed) {
        int sel = sync_sel(w5);
        int32_t lq = sel < 0 ? 0 : sync_lq[sel], a = lq > 16 ? 16 : lq;
        if (lq && s->qbar < 0 && !qpos) s->qbar = 0;                         /* no bar known: this beat is beat 1 */
        if (!lq || (a <= 4 ? (qpos & (a - 1)) == 0 : s->qbar >= 0 && (s->qbar & (a - 1)) == 0)) {
            s->armed = 0;
            start_capture(s, pos, w5, lq, w7);
            return;
        }
    }
    if (!s->lq) return;
    s->qph = s->qph + 1 == s->lq ? 0 : s->qph + 1;
    if (s->qbar >= 0) s->qph = s->lq > 16 ? (s->qph & 16) | s->qbar : s->qbar & (s->lq - 1);   /* the clock's bar */
    int early = s->mode == S_ERODE && s->gap;                                /* the recording ran out: slower clock */
    if (--s->qleft > 0 && !early) return;
    if (s->mode == S_REC) {                                                  /* capture complete: exactly lq quarters */
        s->qleft = s->lq;
        int32_t n = s->i;
        if (n < 64) { for (int32_t k = n; k < 64; k++) loop[k] = 0; n = 64; }   /* (the clock jumped) */
        s->len = s->plen = n;
        int32_t f = s->fade < n / 2 ? s->fade : n / 2;
        for (int32_t j = 0; j < f; j++) loop[n - 1 - j] = (int16_t)((loop[n - 1 - j] * j * s->fmul) >> 15);   /* fade out */
        xfade_bake(s, loop, er + ERLEN, n);
        bucket_flush(pos, s->cb, s->bhi, s->blo);
        s->rb = (WB << 16) / n;                                              /* per capture */
        start_erode(s, er);
    } else if (s->mode == S_ERODE) {                                         /* generation: back to the start */
        /* the clock slowed down (tape speed, tempo): the loop restarts on the largest power-of-two part of its length
         * that the recording still fills (within sqrt 2), on that part's grid within the bar - so at 25 % tape speed
         * a 1 BAR loop restarts every beat instead of playing once and then 3 beats of silence. Once the recording
         * has run out, the restart comes on the next such grid point instead of at the end of the count. */
        int32_t q = s->blen >> 16, k = s->lq;                               /* q: grid step in output samples */
        while (k > 1 && k * q * 29 > (s->len << s->dsh) * 41) k >>= 1;
        if (s->qleft > 0 && (s->qph & (k - 1))) return;                      /* ran out: wait for the part's grid */
        s->qleft = k - (s->qph & (k - 1));
        bucket_flush(pos, s->cb, s->bhi, s->blo);
        if (s->i >= s->len >> 3) {                                           /* a tiny pass (realigning, a clock jump)
                                                                              * is no generation: its peaks would jerk
                                                                              * the gain */
            s->gmo = s->gm; s->rso = s->rs; s->gk = GRAMP;
            next_generation(s);
        }
        int exact = s->i >= s->len && !s->gap;                              /* the loop ended on the grid point */
        s->gap = 0;
        s->i = 0; s->ph = 0; s->ba = 0; s->cb = 0; s->bhi = s->blo = 0;
        if (exact) s->pw = s->x;                                             /* the room's output runs on: interpolate
                                                                              * on from the last loop sample */
        else {                                                               /* cut short (tempo up) or a gap: */
            int32_t x0 = loop[0];                                            /* go on from the last output, */
            s->dkv = clampi(s->lw - x0, 32767); s->dkn = 256; s->pw = s->x = x0;   /* de-click the step */
        }
    }
}

static void sit_render(int pos, int16_t *buf, const int16_t *p)
{
    pos %= 3;
    sit_t *s = &st[pos];
    int16_t *loop = (int16_t *)op1_fx_scratch(pos);
    if (!loop) return;
    int16_t *er = loop + NMAX;
    int32_t mix = op1_fx_param_q15(p, 3), w5 = p[5] < 0 ? 0 : p[5];
    update_coefs(s, op1_fx_param_q15(p, 0), op1_fx_param_q15(p, 1), op1_fx_param_q15(p, 2), p[6] < 0 ? 0 : p[6]);

    int l = op1_fx_page[pos][3] != 0;
    if (l != s->latch) {                                                      /* orange press */
        s->latch = l;
        if (s->armed) s->armed = 0;                                          /* armed: cancel */
        else if (sync_sel(w5) < 0) { s->goff = OP1_FX_BLOCK; start_capture(s, pos, w5, 0, p[7]); }   /* ring: last block */            /* free: new capture now */
        else s->armed = 1;                                                   /* synced: on the grid */
    }

    s->inb[0] = s->inb[OP1_FX_BLOCK]; s->inb[1] = s->inb[OP1_FX_BLOCK + 1]; s->inb[2] = s->inb[OP1_FX_BLOCK + 2];
    for (int n = 0; n < OP1_FX_BLOCK; n++) s->inb[n + 3] = buf[n];
    if (s->mode != S_REC) {                                                  /* pre-roll ring (dry input, 22.05 kHz) */
        int16_t *ring = er + ERLEN;
        for (int n = 1; n < OP1_FX_BLOCK; n += 2) {
            int32_t a = buf[n - 1], b = buf[n];
            ring[s->rgw] = (int16_t)((s->r2 + 3 * s->r1 + 3 * a + b) >> 3);  /* [1 3 3 1] / 8, then 2:1 */
            s->rgw = (s->rgw + 1) & (RING - 1);
            s->r2 = a; s->r1 = b;
        }
    }
    int32_t qpos = 0;
    int off = beat_grid(s, &qpos);
    int32_t peak = run(s, pos, loop, er, buf, 0, off < 0 ? OP1_FX_BLOCK : off, mix);
    if (off >= 0) {
        s->goff = off;                                                   /* ring holds this whole block */
        grid_event(s, pos, loop, er, qpos, w5, p[7]);
        int32_t pk = run(s, pos, loop, er, buf, off, OP1_FX_BLOCK, mix);
        if (pk > peak) peak = pk;
    }
    if (s->mode == S_ERODE)                                                  /* resonator amplitudes, screen */
        for (int k = 0; k < NMODE; k++) {
            int32_t bp = s->res[k].bp, lp = s->res[k].lp;
            int32_t a = mq15((bp < 0 ? -bp : bp) + (lp < 0 ? -lp : lp), s->q15) >> 3;
            int32_t d = s->env[k] - (s->env[k] >> 5);
            s->env[k] = a > d ? a : d;
        }
    volatile int16_t *m = op1_fx_meter(pos);
    int32_t mv = (*m * 0xfe00) >> 16;
    *m = (int16_t)(peak > mv ? peak : mv);
}

const op1_fx_t FX_sitting = { sit_init, sit_render,
                              { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_LATCH } };


/* ======================= screen (GUI thread) =======================
 * lucierroom.svg (gen_svg.py, geometry in sitting_geom.h): only the tape lane ends and the knob tracks are static.
 * Drawn here: the generation counter "GEN 07" (stroke font; "REC" while capturing, "GEN --" before the first
 * capture), next to it the loop length (blue: "1 BAR", "1/16", "2.6 SEC") and ARMED / LO-FI, three info lines (shape in green,
 * L x W x H in blue, the loudest room mode in white - or the page-2 value being edited), the floor plan L x W (blue,
 * grows with the size knob) with the nodal lines of the active axial modes (L modes vertical, W modes horizontal)
 * lighting up with their resonator, a side section for the height modes, the loop waveform across the lane (min /
 * max per 1/128 of the loop: speech is spiky, the room's tones are a smooth band) with the play head (red) and the
 * beat / bar ticks of a synced loop (seconds of a free one), and the 4 knobs: label + bar (page 2: dashed). */
#include "sitting_geom.h"
#include "seq/seq_text.h"
#pragma GCC push_options
#pragma GCC optimize("Os")                             /* GUI code: size, not speed (code space is short) */

static const uint8_t C_BLUE[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, C_GREEN[4] = { 0x00, 0xED, 0x95, 0xFF },
                     C_WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, C_RED[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     C_GREY[4] = { 0x6F, 0x71, 0x73, 0xFF };
static const char *const shape_name[4] = { "CUBE", "GOLDEN", "BOLT", "LUCIER" };
static const char *const sync_name[NSYNC] = { "1/16", "1/8", "1/4", "1/2", "1 BAR", "2 BARS" };   /* note values, 4/4 */
static uint32_t blink;

static void dim(uint8_t *o, const uint8_t *c, int32_t lvl)                 /* lvl 0..256 */
{
    for (int i = 0; i < 3; i++) o[i] = (uint8_t)((c[i] * lvl) >> 8);
    o[3] = 0xFF;
}
#define put_num(p, v, digits) sq_num((p), (uint32_t)(v), (digits))   /* v >= 0 (seq/seq_text.h) */
#define put_str sq_cat
static char *put_m(char *p, int32_t mm)                                     /* metres with one decimal */
{
    int32_t dm = (mm + 50) / 100;
    p = put_num(p, dm / 10, 1); *p++ = '.';
    return put_num(p, dm % 10, 1);
}
static char *put_secs(char *p, int32_t n, int32_t rate)                    /* n samples at rate: "2.6 SEC" */
{
    int32_t ds = (n * 10 + rate / 2) / rate;
    p = put_num(p, ds / 10, 1); *p++ = '.'; p = put_num(p, ds % 10, 1);
    return put_str(p, " SEC");                                               /* (S alone looks like 5) */
}
/* the loop length tag: synced name or free seconds */
static char *put_len(char *p, int32_t lq, int32_t n, int32_t dsh)
{
    for (int k = 0; k < NSYNC; k++) if (lq && sync_lq[k] == lq) return put_str(p, sync_name[k]);
    return put_secs(p, n, 44100 >> dsh);
}
#define Q(v) ((int32_t)(v) * 1024)

static void sit_screen_init(void *scr) { (void)scr; }

static void sit_screen_update(void *scr, int dt)
{
    int pos = op1_fx_screen_pos() % 3;
    sit_t *s = &st[pos];
    int16_t k[4];
    sq_fx_knobs_live(k);
    int16_t wsh[8]; const int16_t *wd = op1_knobs_shown(1, pos, op1_fx_knobs(pos), wsh);   /* page 2 as modulated */
    int32_t w5 = wd[5] < 0 ? 0 : wd[5], w6 = wd[6] < 0 ? 0 : wd[6];
    int pg_blue = op1_fx_page[pos][0], pg_green = op1_fx_page[pos][1], pg_white = op1_fx_page[pos][2];
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    blink += (uint32_t)dt;
    uint8_t c[4];
    char t[24], *p;
    int mode = s->mode, sel = sync_sel(w5);

    /* ---- generation counter ---- */
    if (mode == S_REC) {
        sq_text(r, Q(SG_GEN_X), Q(SG_GEN_Y), SG_GEN_S, "REC", 0x800, C_RED);
        if ((blink >> 9) & 1) {                                              /* blinking dot after REC */
            int32_t x = Q(SG_GEN_X) + 18 * SG_GEN_S + Q(3), y = Q(SG_GEN_Y) + 3 * SG_GEN_S;
            sq_line(r, x - Q(2), y, x + Q(2), y, Q(4), C_RED);
        }
    } else {
        static const uint8_t C_LABEL[4] = { 0xAE, 0xB1, 0xDC, 0xFF };          /* stock purple-grey */
        int32_t x = sq_text(r, Q(SG_GEN_X), Q(SG_GEN_Y), SG_GEN_S, "GEN ", 0x800, mode == S_ERODE ? C_LABEL : C_GREY);
        if (mode == S_ERODE) put_num(t, s->gen > 99 ? 99 : s->gen, 2); else put_str(t, "--");
        sq_text(r, x, Q(SG_GEN_Y), SG_GEN_S, t, 0x800, mode == S_ERODE ? C_WHITE : C_GREY);
    }

    /* ---- tags: loop length (blue), ARMED (blinking) or LO-FI ---- */
    int32_t blen = s->blen > 0 ? s->blen : 22050 << 14;
    if (mode != S_IDLE) {
        put_len(t, s->lq, s->plen, s->dsh);
        sq_text(r, Q(SG_TAG_X), Q(sg_tag_y[0]), SG_TAG_S, t, 0x500, C_BLUE);
    } else if (sel >= 0) {
        sq_text(r, Q(SG_TAG_X), Q(sg_tag_y[0]), SG_TAG_S, sync_name[sel], 0x500, C_GREY);
    }
    if (s->armed) {
        if ((blink >> 8) & 1) sq_text(r, Q(SG_TAG_X), Q(sg_tag_y[1]), SG_TAG_S, "ARMED", 0x500, C_RED);
    } else if (mode != S_IDLE && s->dsh == 2) sq_text(r, Q(SG_TAG_X), Q(sg_tag_y[1]), SG_TAG_S, "LO-FI", 0x500, C_GREY);

    /* ---- info lines ---- */
    room_t rm;
    room_calc(&rm, k[0], k[1]);
    sq_text(r, Q(SG_INFO_X), Q(sg_info_y[0]), SG_INFO_S, shape_name[rm.shape], 0x500, C_GREEN);
    p = put_m(t, rm.dim[0]); *p++ = 'X'; p = put_m(p, rm.dim[1]); *p++ = 'X'; p = put_m(p, rm.dim[2]); put_str(p, "M");
    sq_text(r, Q(SG_INFO_X), Q(sg_info_y[1]), SG_INFO_S, t, 0x500, C_BLUE);
    int32_t emax = 0, kmax = 0;
    for (int i = 0; i < NMODE; i++) if (s->env[i] > emax) { emax = s->env[i]; kmax = i; }
    const uint8_t *ic = C_WHITE;
    if (pg_blue) {                                                           /* next capture length */
        if (sel < 0) {
            int32_t n = free_len(w5);
            p = put_str(t, "LOOP "); p = put_secs(p, n, 22050);
            if (n > NMAX) put_str(p, " LO-FI");                              /* 11.025 kHz */
        }
        else {
            int32_t lq = sync_lq[sel], dsh;
            sync_fit(blen, &lq, &dsh);
            p = put_str(t, "SYNC "); p = put_str(p, sync_name[sel]);
            for (int k = 0; k < NSYNC; k++)                                  /* does not fit even at 11.025 kHz: */
                if (lq != sync_lq[sel] && sync_lq[k] == lq) { p = put_str(p, " AS "); p = put_str(p, sync_name[k]); }   /* "1 BAR AS 1/2" */
            if (lq == sync_lq[sel] && dsh == 2) put_str(p, " LO-FI");       /* 11.025 kHz */
        }
        ic = C_BLUE;
    } else if (pg_green) {
        p = put_str(t, "ABSORB "); p = put_num(p, w6 * 100 >> 15, 1); put_str(p, "%");
        ic = C_GREEN;
    } else if (pg_white) {                                                   /* next capture's crossfade */
        int32_t w7 = wd[7] < 0 ? 0 : wd[7], ms = (w7 * (RING - 1) >> 15) * 10 / 2205;   /* 22.05 kHz samples */
        if (ms) { p = put_str(t, "XFADE "); p = put_num(p, ms, 1); put_str(p, " MS"); } else put_str(t, "XFADE OFF");
        ic = C_WHITE;
    } else if (mode == S_ERODE && emax > 0) {
        p = put_num(t, (s->room.f[kmax] + 128) >> 8, 1); put_str(p, " HZ");
    } else if (mode == S_IDLE) {
        put_str(t, !s->armed ? "PRESS ORANGE" : sel >= 4 ? "STARTS ON BAR 1" : "STARTS ON BEAT"); ic = C_GREY;
    } else t[0] = 0;
    sq_text(r, Q(SG_INFO_X), Q(sg_info_y[2]), SG_INFO_S, t, 0x500, ic);

    /* ---- floor plan: L across, W down, scaled so the largest room of this shape fills the box ---- */
    int32_t full = SG_PLAN_W;                                                /* px of L at max size */
    if ((full * rm.dim[1]) / rm.dim[0] > SG_PLAN_H) full = SG_PLAN_H * rm.dim[0] / rm.dim[1];
    int32_t lpx = full * (11469 + ((int32_t)k[0] * 21298 >> 15)) >> 5;      /* Q10, 35 .. 100 % with size */
    int32_t wpx = lpx * rm.dim[1] / rm.dim[0];                              /* (32 bits: lpx <= 102400 Q10, */
    int32_t hpx = lpx * rm.dim[2] / rm.dim[0];                              /*  dim <= 8000 mm) */
    int32_t x0 = Q(SG_PLAN_CX) - lpx / 2, x1 = x0 + lpx, y0 = Q(SG_PLAN_CY) - wpx / 2, y1 = y0 + wpx;
    int32_t sx = Q(SG_SECT_X), sy0 = Q(SG_PLAN_CY) - hpx / 2, sy1 = sy0 + hpx;
    for (int i = 0; i < NMODE; i++) {                                        /* nodal lines */
        int32_t lvl = (mode == S_ERODE && emax > 0) ? 40 + (s->env[i] * 216 / emax) : 40;
        dim(c, C_WHITE, lvl);
        for (int d = 0; d < 3; d++) if (rm.ax[i] & (1 << d)) {
            int32_t n = (int32_t)((((int64_t)rm.f[i] * rm.dim[d]) + 21952000) / 43904000);   /* order */
            if (n < 1) n = 1;
            if (n > 24) n = 24;                                              /* keep the GUI cheap */
            int32_t span = d == 0 ? lpx : (d == 1 ? wpx : hpx);
            for (int32_t j = 0; j < n; j++) {
                int32_t o = span * (2 * j + 1) / (2 * n);                    /* span <= lpx, j < 24 */
                if (d == 0) sq_line(r, x0 + o, y0 + Q(2), x0 + o, y1 - Q(2), 0x400, c);
                else if (d == 1) sq_line(r, x0 + Q(2), y0 + o, x1 - Q(2), y0 + o, 0x400, c);
                else sq_line(r, sx - Q(4), sy0 + o, sx + Q(4), sy0 + o, 0x400, c);
            }
        }
    }
    sq_line(r, x0, y0, x1, y0, 0x600, C_BLUE); sq_line(r, x1, y0, x1, y1, 0x600, C_BLUE);   /* walls */
    sq_line(r, x1, y1, x0, y1, 0x600, C_BLUE); sq_line(r, x0, y1, x0, y0, 0x600, C_BLUE);
    sq_line(r, sx - Q(6), sy0, sx + Q(6), sy0, 0x600, C_BLUE); sq_line(r, sx + Q(6), sy0, sx + Q(6), sy1, 0x600, C_BLUE);
    sq_line(r, sx + Q(6), sy1, sx - Q(6), sy1, 0x600, C_BLUE); sq_line(r, sx - Q(6), sy1, sx - Q(6), sy0, 0x600, C_BLUE);

    /* ---- loop waveform: the loop fills the lane ---- */
    int32_t lx0 = Q(SG_WAVE_X0), lw = Q(SG_WAVE_X1 - SG_WAVE_X0), ly = Q(SG_WAVE_Y);
    if (mode == S_IDLE) {                                                   /* blank tape */
        for (int32_t x = lx0; x < lx0 + lw; x += Q(6)) sq_line(r, x, ly, x + Q(3), ly, 0x400, C_GREY);
    } else {
        int32_t len = s->plen > 0 ? s->plen : 1, head = s->i > len ? len : s->i;
        int32_t hb = head * WB / len;                                       /* head <= NMAX */
        uint8_t cg[4]; dim(cg, C_WHITE, 120);
        for (int b = 0; b < WB; b++) {
            if (mode == S_REC && b > hb) break;
            int32_t x = lx0 + lw * (2 * b + 1) / (2 * WB);
            int32_t hi = wave_hi[pos][b] * 192, lo = wave_lo[pos][b] * 192;
            if (hi > Q(SG_WAVE_H)) hi = Q(SG_WAVE_H);
            if (lo < -Q(SG_WAVE_H)) lo = -Q(SG_WAVE_H);
            if (hi - lo < 0x300) { hi += 0x180; lo -= 0x180; }               /* silence: a dot */
            sq_line(r, x, ly - hi, x, ly - lo, 0x500, mode == S_REC ? C_RED : (b < hb ? C_WHITE : cg));
        }
        /* ticks under the lane: quarters (up to 1 beat) or beats (bar lines long) of a synced loop, free: seconds */
        int32_t ty = ly + Q(SG_WAVE_H + 2);
        if (s->lq) {
            int32_t nt = s->lq <= 4 ? s->lq : s->lq >> 2;
            for (int32_t j = 0; j <= nt; j++) {
                int32_t x = lx0 + lw * j / nt, bar = s->lq <= 4 ? j == 0 || j == nt : (j & 3) == 0;
                sq_line(r, x, ty, x, ty + Q(bar ? 6 : 3), 0x400, bar ? C_WHITE : C_GREY);
            }
        } else {
            int32_t rate = 44100 >> s->dsh;
            for (int32_t j = rate; j < len; j += rate) {
                int32_t x = lx0 + (int32_t)((int64_t)lw * j / len);
                sq_line(r, x, ty, x, ty + Q(3), 0x400, C_GREY);
            }
        }
        int32_t hx = lx0 + (int32_t)((int64_t)lw * head / len);              /* play / record head */
        sq_line(r, hx, ly - Q(SG_WAVE_H + 3), hx, ly + Q(SG_WAVE_H + 3), 0x500, C_RED);
    }

    /* ---- knobs: label + bar (page 2: its label, dashed); synced lengths ticked on the blue page-2 track ---- */
    static const uint8_t *const kc[4] = { C_BLUE, C_GREEN, C_WHITE, C_RED };
    static const char *const lab1[4] = { "SIZE", "SHAPE", "ERODE", "MIX" }, *const lab2[3] = { "LOOP", "ABSORB", "XFADE" };
    for (int i = 0; i < 4; i++) {
        int pg = (i == 0 && pg_blue) || (i == 1 && pg_green) || (i == 2 && pg_white);
        int32_t v = pg ? (i == 2 ? (wd[7] < 0 ? 0 : wd[7]) : i ? w6 : w5) : k[i];
        int32_t kx = Q(sg_knob_x[i]), ky = Q(SG_KNOB_Y), len = Q(SG_KNOB_W) * v >> 15;
        sq_text(r, kx, Q(SG_LABEL_Y), SG_LABEL_S, pg ? lab2[i] : lab1[i], 0x500, kc[i]);
        if (i == 3 && (s->armed || mode == S_REC) && ((blink >> 8) & 1))      /* capture pending / running */
            sq_line(r, kx + Q(SG_KNOB_W - 4), Q(SG_LABEL_Y + 3), kx + Q(SG_KNOB_W), Q(SG_LABEL_Y + 3), Q(4), C_RED);
        if (!pg) { if (len > 0x200) sq_line(r, kx, ky, kx + len, ky, SG_KNOB_T, kc[i]); }
        else for (int32_t x = 0; x < len; x += Q(4)) sq_line(r, kx + x, ky, kx + (x + Q(2) < len ? x + Q(2) : len), ky, SG_KNOB_T, kc[i]);
        if (pg && i == 0)                                                    /* FREE | 6 synced lengths */
            for (int j = 0; j <= NSYNC; j++) {
                int32_t tx = kx + (Q(SG_KNOB_W) * (FREE_V + j * (32768 - FREE_V) / NSYNC) >> 15);   /* <= 2^31 - 2^25 */
                sq_line(r, tx, ky + Q(4), tx, ky + Q(j ? 6 : 8), 0x300, C_GREY);
            }
        sq_line(r, kx + len, ky - Q(5), kx + len, ky + Q(5), 0x500, kc[i]);                 /* value tick */
    }
}

#pragma GCC pop_options
const op1_screen_t SCREEN_sitting = { sit_screen_init, sit_screen_update };
