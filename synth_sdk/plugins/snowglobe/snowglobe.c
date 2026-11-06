/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* "snowglobe": shimmer reverb. Four allpass diffusers into a four-line feedback delay network (Hadamard
 * mixing, damped), with an octave-up pitch shifter fed back into the tank, so tails climb an octave at a time.
 * blue   (knob 0): snowfall: decay, feedback 0.6 .. 0.985
 * green  (knob 1): shimmer: share of the feedback (on two of the four lines) that is the octave-up tail
 * white  (knob 2): globe size: delay lengths 40 .. 100 % (about 35 .. 90 ms per line)
 * red    (knob 3): shake: mix (dry 1 -> 0.5 while the wet rises 0 -> 1); the last few percent freeze the tail
 *                  (feedback 1, no damping, no new input)
 * The reverb runs at half rate (22.05 kHz): each pair of input samples is averaged into one tank step and the wet
 * is interpolated back . Delay and allpass lengths, the shifter
 * grain and the filters are set for the half rate, so times and the loop's cutoff stay as they were at full rate;
 * the tank's own low-pass (about 6 kHz) sits well under the half rate's 11 kHz.
 * Per tank step: 4 allpasses, 4 delay reads/writes, 2 shifter reads, ~20 multiplies; per sample a mix.
 */
#include "hk/fx.h"

#define APLEN 256                         /* allpass buffers (the longest allpass is 189 steps) */
#define LINE 2048                         /* delay lines and the shifter (the longest line is 1808 steps) */
#define WINDOW 1024                       /* pitch shifter grain, 1024 steps = 46 ms */
#define CHUNK 32                          /* tank steps per chunk (one block of 64 samples) */
#define TAIL CHUNK                        /* the delay lines' copy of their first CHUNK slots, after the end */
enum { AP0 = 71, AP1 = 53, AP2 = 189, AP3 = 138 };     /* allpass lengths in steps (half the full-rate 142 107 379 277) */
static const int16_t line_len[4] = {3557, 3617, 3491, 3422};   /* at full size, in samples at 44.1 kHz */

/* No index masks in the loops: a chunk (32 steps) never crosses a multiple of 32, so within a chunk every read and
   write moves on by one slot per step (the shifter's heads by two) and only the reads can run past the end.
   - The allpasses (int32, so they need no saturation) and the shifter (row 4 of line) are written twice, at i and
     i + their length, and read at (write slot + length - delay): always inside the row.
   - The delay lines (rows 0..3) are written once; their reads start at (write slot - delay) mod LINE and may run
     into TAIL, a copy of the first CHUNK slots, made after the chunk that wrote them (the shortest line is 684
     steps, so no read sees that chunk's slots before the copy).
   32-byte aligned (one Blackfin cache line) so that every row starts a new line at the same w (w % 16 == 0).
   PAD (one line, unused) staggers the rows: unpadded they sit 2 KB / 4 KB / 8 KB apart and their lines at the same
   w compete for the same sets of the 2-way data cache. */
#define PAD 16
typedef struct {
    int32_t ap[4][2 * APLEN + PAD / 2];
    int16_t line[5][2 * LINE + PAD];     /* rows 0..3 use LINE + TAIL slots, row 4 (the shifter) 2 x LINE */
    int32_t lp[4], dc[4];
    int32_t wet;                          /* the last tank step's wet x mix, for the interpolation */
    uint32_t w;                           /* tank steps */
    uint32_t e, eblocks;                  /* the screen's energy sum, over 4 blocks */
} __attribute__((aligned(32))) state_t;

/* Line touch: the Blackfin data cache does not keep (a write-allocate policy is not assumed) a line that only gets
   written, so each write went to SDRAM on its own. Reading the slots about to be written once per 32-byte line
   loads the line, and the following writes hit it (written back later as one burst). The values read are not
   used: the sound is unchanged. */
static inline void touch_lines(state_t *s, uint32_t api, uint32_t wi)
{
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < CHUNK; i += 8) {
            (void)((const volatile int32_t *)s->ap[k])[api + i];
            (void)((const volatile int32_t *)s->ap[k])[api + APLEN + i];
        }
    for (int k = 0; k < 5; k++)
        for (int i = 0; i < CHUNK; i += 16) (void)((const volatile int16_t *)s->line[k])[wi + i];
    for (int i = 0; i < CHUNK; i += 16) (void)((const volatile int16_t *)s->line[4])[wi + LINE + i];
}

static FX_BUFFER state_t st[FX_INSTANCES];
volatile uint32_t snowglobe_blocks;       /* for the screen */
volatile int32_t snowglobe_level;         /* tail level, 0 .. ~32767 */
volatile int32_t snowglobe_freeze;

void fx_reset(int inst)
{
    state_t *s = &st[inst];
    /* whole state; the init runs in the audio callback (first render): 32-bit stores, the same zeros (bursts 2026-10-27) */
    _Static_assert(sizeof(state_t) % 4 == 0, "snowglobe state size");
    uint32_t *p = (uint32_t *)(void *)s;
    for (unsigned i = 0; i < sizeof(state_t) / 4; i++) p[i] = 0;
}

/* saturation to int16: on Blackfin a saturating shift up and back (two instructions, no constants held in
   registers; the nested ternary of fx_sat16 compiled to jumps), on the host the same in C */
static inline int32_t sg_sat(int32_t v)
{
#ifdef __bfin__
    return __builtin_bfin_shl_fr1x32(v, 16) >> 16;
#else
    v = v > 32767 ? 32767 : v;
    return v < -32768 ? -32768 : v;
#endif
}

/* allpass g = 0.5 on row a (already at the write slot), delay len; int32, so no saturation (it stays within
   twice its input) */
#define SG_AP(a, len, x)                                                                                       \
    do {                                                                                                     \
        const int32_t dl = (a)[APLEN - (len)];                                                               \
        const int32_t v = (x) - (dl >> 1);                                                                   \
        (a)[0] = (a)[APLEN] = v;                                                                             \
        (x) = dl + (v >> 1);                                                                                 \
    } while (0)

/* the params as each instance hears them (after the LFO), for the screen */
volatile int16_t snowglobe_knobs[FX_INSTANCES][4];
volatile uint32_t snowglobe_kblocks[FX_INSTANCES];

void fx_process(int inst, int16_t *io, int n, const int16_t *knobs)
{
    for (int i = 0; i < 4; i++) snowglobe_knobs[inst][i] = knobs[i];
    snowglobe_kblocks[inst]++;
    state_t *s = &st[inst];
    const int32_t k0 = knobs[0] < 0 ? 0 : knobs[0], k1 = knobs[1] < 0 ? 0 : knobs[1];
    const int32_t k2 = knobs[2] < 0 ? 0 : knobs[2], k3 = knobs[3] < 0 ? 0 : knobs[3];
    const int freeze = k3 > 31500;
    const int32_t fb = freeze ? 32767 : 19661 + ((k0 * 12616) >> 15);   /* Q15, 0.6 .. 0.985 */
    /* one-pole low-pass in the loop: 0.6 per sample at 44.1 kHz (pole 0.4) is pole 0.16 per step, 0.84 */
    const int32_t damp = freeze ? 32767 : 27525;
    const int32_t shimmer = freeze ? 0 : (k1 * 16384) >> 15;           /* share of the feedback, up to 0.5 */
    /* lines 0 and 2 feed back fb x ((1 - shimmer) x o + shimmer x up) */
    const int32_t fb_o = (fb * (32768 - shimmer)) >> 15, fb_up = (fb * shimmer) >> 15;
    const int32_t size = 13107 + ((k2 * 19660) >> 15);                 /* Q15, 0.4 .. 1.0 */
    const int32_t in_gain = freeze ? 0 : 9830;                         /* 0.3 into the tank */
    /* energy of the wet signal for the screen: the wet is clipped to int16 (as the shifter keeps it), so
       (wet / 8)^2 < 2^24 and the 128 steps of 4 blocks sum within 32 bits */
    uint32_t e = s->e;
    int32_t lp0 = s->lp[0], lp1 = s->lp[1], lp2 = s->lp[2], lp3 = s->lp[3];
    int32_t dc0 = s->dc[0], dc1 = s->dc[1], dc2 = s->dc[2], dc3 = s->dc[3];
    const int32_t len0 = (line_len[0] * size) >> 16, len1 = (line_len[1] * size) >> 16;   /* in steps */
    const int32_t len2 = (line_len[2] * size) >> 16, len3 = (line_len[3] * size) >> 16;

    /* One chunk at a time, in passes over the chunk (the whole step in one loop needed more registers than the
       Blackfin has, and spilled): the lines are read at least 684 steps back and the allpasses at least 53, so no
       step of a chunk reads what another step of it writes, except the shifter (pass 3 does it in order).
       1: the input pairs through the diffusers; 2: each line read and damped; 3: Hadamard, shifter, line writes;
       4: the wet back at 44.1 kHz and the mix. */
    int32_t sc[6][CHUNK];             /* per chunk: rows 0..3 the lines read, 4 the diffused input, 5 the wet */
    uint32_t w = s->w;
    int32_t wprev = s->wet;
    const int steps = n >> 1;
    for (int c = 0; c < steps;) {
        const int cl = (int)(CHUNK - (w & (CHUNK - 1))) < steps - c ? (int)(CHUNK - (w & (CHUNK - 1))) : steps - c;
        const uint32_t api = w & (APLEN - 1), wi = w & (LINE - 1);
        touch_lines(s, api, wi);
        int16_t *const io2 = io + 2 * c;

        int32_t *a0 = &s->ap[0][api], *a1 = &s->ap[1][api], *a2 = &s->ap[2][api], *a3 = &s->ap[3][api];
        for (int j = 0; j < cl; j++) {
            int32_t d = ((io2[2 * j] + io2[2 * j + 1]) * in_gain) >> 16;    /* the pair's mean x 0.3 */
            SG_AP(a0, AP0, d);
            SG_AP(a1, AP1, d);
            SG_AP(a2, AP2, d);
            SG_AP(a3, AP3, d);
            a0++; a1++; a2++; a3++;
            sc[4][j] = d;
        }

#define SG_LINE(k, ln, lp, dc)                                                                                 \
        do {                                                                                                 \
            const int16_t *r = &s->line[k][(wi - (uint32_t)(ln)) & (LINE - 1)];                             \
            int32_t *const y = sc[k];                                                                        \
            for (int j = 0; j < cl; j++) {                                                                   \
                (lp) += ((r[j] - (lp)) * damp) >> 15;                                                        \
                (dc) += ((lp) * 32 - (dc)) >> 8;      /* DC estimate, Q5, ~14 Hz: rounding in the loop */    \
                y[j] = (lp) - ((dc) >> 5);            /* builds DC when the feedback is near 1 */            \
            }                                                                                                \
        } while (0)
        SG_LINE(0, len0, lp0, dc0);
        SG_LINE(1, len1, lp1, dc1);
        SG_LINE(2, len2, lp2, dc2);
        SG_LINE(3, len3, lp3, dc3);
#undef SG_LINE

        /* shimmer: two read heads sweeping through the last WINDOW steps twice as fast as they are written (an
           octave up), half a grain apart, with triangular crossfades. ta = w % WINDOW turns at multiples of
           WINDOW / 2, never inside a chunk, so the crossfade moves by one per step */
        const uint32_t ta = w & (WINDOW - 1), tb = (w + WINDOW / 2) & (WINDOW - 1);
        int32_t ga = ta < WINDOW / 2 ? (int32_t)ta : (int32_t)(WINDOW - ta);
        const int32_t dga = ta < WINDOW / 2 ? 1 : -1;
        const int16_t *ra = &s->line[4][wi + LINE - WINDOW + ta], *rb = &s->line[4][wi + LINE - WINDOW + tb];
        int16_t *l0 = &s->line[0][wi];
        for (int j = 0; j < cl; j++) {
            const int32_t y0 = sc[0][j], y1 = sc[1][j], y2 = sc[2][j], y3 = sc[3][j], d = sc[4][j];
            const int32_t a = y0 + y1, b = y0 - y1, cc = y2 + y3, e2 = y2 - y3;
            const int32_t o0 = (a + cc) >> 1, o1 = (b + e2) >> 1, o2 = (a - cc) >> 1, o3 = (b - e2) >> 1;
            const int16_t t16 = (int16_t)sg_sat(o0);         /* the wet (o0 is the tank's sum / 2) */
            l0[4 * (2 * LINE + PAD)] = l0[4 * (2 * LINE + PAD) + LINE] = t16;
            const int32_t sb = rb[0], up = sb + (((ra[0] - sb) * ga) >> 9);   /* crossfade, / (WINDOW / 2) */
            ra += 2; rb += 2; ga += dga;
            /* the octave-up tail replaces part of the feedback on lines 0 and 2, so the loop gain stays fb */
            l0[0] = (int16_t)sg_sat(d + ((o0 * fb_o + up * fb_up) >> 15));
            l0[1 * (2 * LINE + PAD)] = (int16_t)sg_sat(d + ((o1 * fb) >> 15));
            l0[2 * (2 * LINE + PAD)] = (int16_t)sg_sat(d + ((o2 * fb_o + up * fb_up) >> 15));
            l0[3 * (2 * LINE + PAD)] = (int16_t)sg_sat(d + ((o3 * fb) >> 15));
            l0++;
            sc[5][j] = t16;
        }
        if (wi < TAIL)                     /* this chunk wrote slots the tail copies */
            for (int k = 0; k < 4; k++)
                for (uint32_t i = wi; i < wi + (uint32_t)cl && i < TAIL; i++) s->line[k][LINE + i] = s->line[k][i];

        /* the wet back at 44.1 kHz: halfway from the last step's for the first sample of each pair */
        for (int j = 0; j < cl; j++) {
            const int32_t x0 = io2[2 * j], x1 = io2[2 * j + 1], wk = (sc[5][j] * k3) >> 15;
            io2[2 * j] = (int16_t)sg_sat(x0 - ((x0 * k3) >> 16) + ((wprev + wk) >> 1));
            io2[2 * j + 1] = (int16_t)sg_sat(x1 - ((x1 * k3) >> 16) + wk);
            wprev = wk;
            const int32_t q = sc[5][j] >> 3;
            e += (uint32_t)(q * q);
        }
        w += (uint32_t)cl;
        c += cl;
    }
    s->lp[0] = lp0; s->lp[1] = lp1; s->lp[2] = lp2; s->lp[3] = lp3;
    s->dc[0] = dc0; s->dc[1] = dc1; s->dc[2] = dc2; s->dc[3] = dc3;
    s->w = w;
    s->wet = wprev;
    s->e = e;
    s->eblocks += (uint32_t)steps;
    if (inst != FX_MASTER) {
        snowglobe_blocks++;
        snowglobe_freeze = freeze;
    }
    if (s->eblocks >= 128) {               /* every 4 blocks of 64 samples (2 of 128) */
        /* mean square over those steps (x 64 for the / 8): a shift for 128, else a division (once per 4 blocks) */
        const uint32_t nst = s->eblocks;
        uint32_t m = (nst == 128 ? e >> 7 : e / nst) << 6;
        s->e = s->eblocks = 0;
        if (inst != FX_MASTER) {
            int32_t r = 0;                 /* integer square root of the mean square */
            for (uint32_t bit = 1u << 30; bit; bit >>= 2) {
                if (m >= (uint32_t)r + bit) { m -= (uint32_t)r + bit; r = (r >> 1) + (int32_t)bit; }
                else r >>= 1;
            }
            snowglobe_level = r;
        }
    }
}
