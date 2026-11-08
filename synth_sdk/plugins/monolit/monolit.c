/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* "monolit": a three-oscillator ladder synth for bass and lead, with macro knobs: every position is meant to be
 * usable, so each knob moves several settings along a tuned path instead of one raw parameter.
 * Page 1
 *   blue  (knob 0) shape:   one saw -> saw + sub square -> two detuned saws -> three saws -> wide stack
 *   green (knob 1) cutoff:  ladder cutoff 80 Hz .. 12.8 kHz at C4 over the first three quarters, then one more octave
 *                           over the last quarter (the ladder opens fully), with 60 % key tracking built in
 *   white (knob 2) contour: the filter envelope; left half a pluck (the full 6.5-octave sweep by a fifth of the knob,
 *                           then a longer decay), right half a swell (sustain and then attack come in)
 *   red   (knob 3) heat:    three zones: fuller and driven (the Minimoog's output fed back into its mixer) over the
 *                           first half, resonant towards 90 %, self-oscillating at the top; level-compensated
 * Page 2 (encoder press on blue, green, white; preset words 5, 6, 7; 0 is the default)
 *   blue  body:     osc 3's interval: -1 octave, unison, fifth, +1 octave, -2 octaves
 *   green velocity: how much velocity opens the filter and moves the level (0: level only, a little)
 *   white age:      oscillator drift and a per-note cutoff spread
 * The amp envelope, glide and play modes are the OP-1's own. Integers only; per sample: four polyBLEP saws and Moai's
 * zero-delay four-pole ladder (2026-11-04: the resonance on the cutoff, up to self-oscillation; it was a unit-delay
 * ladder whose hot input clip crushed the resonance to +5 .. +10 dB).
 * Rate (2026-11-03): a voice call is 128 samples at 88.2 kHz (689 calls/s) on R246, and the cutoff table, the key
 * tracking, the contour and the drift are derived for that (they were for 44.1 kHz / 344 calls/s: the cutoff sat
 * 0.4 octave high, the contour and the drift ran twice as fast). */
#include "op1_plugin.h"
#include "monolit_tables.h"

typedef struct {
    uint32_t ph[4];          /* osc 1, osc 2, osc 3, sub (2^32 = one cycle) */
    int32_t s[4];            /* ladder stages, Q16 (the kernels read and write s[0..3] as Moai's lad_t) */
    int32_t env;             /* contour, Q15 */
    int32_t stage;           /* 0 attack, 1 decay / sustain, 2 release */
    int32_t drift[3], dv[3]; /* slow pitch walk per oscillator, +-32767 */
    uint32_t rnd;
    int32_t spread;          /* per-note cutoff spread, +-2048, scaled by age */
    int32_t pos, dc;         /* last block's cutoff position (Q10 octaves over 20 Hz); the output's AC coupling, Q16 */
    int32_t ready;
    uint32_t inc0, rdt;      /* br_pitch's cache: the block's first increment, and from it */
    int32_t key, floor_x;
    int32_t yp;              /* the ladder's last output (the kernels keep it; MONOLIT feeds nothing back with it) */
    int32_t cgg, ck;         /* the stage coefficient and feedback the cached chunk coefficients are for */
    int32_t clc[4];          /* those coefficients (mo_coef_t): a held cutoff and heat solve nothing */
} __attribute__((aligned(32))) br_voice;

/* The voices are the persistent state and live in the L1 pool's synth partition when the core gives it
 * (op1_l1_claim_state, OP1_L1_API 2): the core copies them in from br_home.v and back when another engine takes the
 * partition or the pool goes off. render and note_on claim first and reach everything through br_hot() for that
 * call only; note_off (br_rel) and the screen (monolit_live, monolit_env) never touch br_home. The per-block
 * scratch (the mix, osc 3's increments) follows the voices and is written before it is read. Same samples either way. */
typedef struct {
    br_voice v[OP1_VOICES];
    int32_t mix[1 + OP1_BLOCK + 7];   /* [0]: the pad word the kernels' stores run behind into */
    uint32_t pre[OP1_BLOCK + 8];      /* osc 3's increments; the passes read one word ahead */
    int32_t blk[24];                  /* mo_run's parameter block */
} br_hot_t;
#ifdef __bfin__
_Static_assert(sizeof(br_hot_t) <= 8192 && sizeof(br_voice) % 32 == 0 && sizeof(((br_hot_t *)0)->v) % 32 == 0,
               "the voices (state) first, the scratch after them, all in OP1_L1_SYNTH");
#endif
static br_hot_t br_home __attribute__((aligned(32)));
static br_hot_t *br_hp;
static void br_claim(void)
{
    br_hot_t *h = &br_home;
#if defined(__bfin__)
    int fresh;                                 /* nothing to copy ourselves: the scratch is written before it is read */
    br_hot_t *l = op1_l1_claim_state(OP1_L1_SYNTH, h, sizeof h->v, sizeof *h, &fresh);
    if (l) h = l;
#endif
    br_hp = h;
}
#define voices (br_hp->v)
/* note_off's release request (VoiceCtl's thread), kept out of the voices so it never writes the voice state: note_on
 * clears it, render turns it into stage 2 */
static volatile int32_t br_rel[OP1_VOICES];
#if defined(__bfin__) && !defined(MONOLIT_C_LOOP)
/* monolit_dsp.S: the oscillators (see its header); monolit_fc.S: the ladder (gen_fc.py) */
void mo_run(void *voice, int16_t *out, const int32_t *b);
#endif
volatile int16_t monolit_live[4];   /* the knobs as the last block heard them (after the LFO), for the screen */
volatile int32_t monolit_env;       /* the last rendered voice's contour, for the screen */

/* shape breakpoints at 0, 1/4, 1/2, 3/4, 1: levels of osc 1, osc 2, osc 3, sub (Q15, RMS-normalised for
 * uncorrelated oscillators) and osc 2's detune (Q16 fraction: 227 = 6 cents) */
static const int16_t shape[5][5] = {
    {26000,     0,     0,     0,   0},
    {21300,     0,     0, 14900,   0},
    {17760, 17760,     0,  6660, 227},
    {14810, 14810, 14810,  4230, 341},
    {14330, 14330, 14330,  7720, 607},
};

#define C4_INC 12740061u   /* 2^32 * 261.63 / 88200 (OP1_PITCH_INC counts 88.2 kHz steps) */
#define F40_INC 1947817u   /* 2^32 * 40 / 88200: the cutoff table's x = 0 */

/* u * (1 + m / 2^16) for |m| < 2^15, dropping the low 16 bits of u: under 0.002 % of the increment, a fixed
 * fraction of a cent */
static inline uint32_t detune(uint32_t u, int32_t m)
{
    return u + (uint32_t)((int32_t)(u >> 16) * m);
}

/* ---- the ladder: Moai's zero-delay ladder (moai.c lad_coef / lad_step / filter_chunk_t; PROVENANCE.md) ----
 * Four trapezoidal one-poles with the feedback solved inside the sample, u = (x - k S) / (1 + k G^4) by a reciprocal
 * table, a cubic soft clip on u (the input clip also limits the resonance, as on the real ladder), 14 Hz AC coupling,
 * the resonance's make-up and level (mu) and a soft output clip. The coefficients once per 64-sample chunk (mo_coef);
 * the samples in monolit_fc.S on the Blackfin (gen_fc.py), in mo_chunk_c here (host builds, and kern_check's
 * reference), bit for bit. The 16 x 16 products are Moai's intrinsics' host twins.
 * Copyright (C) 2026 moai contributors (the ladder's arithmetic); GPL-3.0-or-later with the OP-1 firmware permission. */
#define MO_CHUNK 64                    /* = gen_fc.py's CHUNK */
#define MO_CLIP_K 2731                 /* the soft clip's constant: x^2 / (3 L^2), L = 1.5 F, F = 5461 */
#define MO_OG 24576                    /* the output: F -> full scale (Q12: 6) */
typedef struct { int32_t ggk, c12, c34, invk; } mo_coef_t;     /* Q15 pairs: lo | hi << 16 */
typedef struct { int32_t g, b, lp, mu; } mo_sat_t;              /* the kernels' st: only mu (Q13) is read */
static inline int32_t m16(int32_t a, int32_t b) { return (int32_t)(int16_t)a * (int32_t)(int16_t)b; }

/* the chunk's coefficients for stage coefficient gg (Q15) and feedback k (Q12) */
static void mo_coef(mo_coef_t *c, int32_t gg, int32_t k)
{
    if (!k) {                                   /* no resonance: the plain kernel reads only gg and the clip's constant */
        c->ggk = gg; c->c12 = c->c34 = 0; c->invk = 32767 | (MO_CLIP_K << 16);
        return;
    }
    const int32_t om = 32767 - gg;
    if (gg > MO_GG_K0) {                                          /* no resonance up in the open range */
        const int32_t d = MO_GG_K1 - gg;
        k = d <= 0 ? 0 : m16(k, (d * (MO_GG_KA + ((d * MO_GG_KB) >> 16))) >> 8) >> 15;
    }
    const int32_t g2 = m16(gg, gg) >> 15, g3 = m16(g2, gg) >> 15, g4 = m16(g3, gg) >> 15;
    const int32_t c3 = m16(gg, om) >> 15, c2 = m16(g2, om) >> 15, c1 = m16(g3, om) >> 15;
    c->ggk = gg | (k << 16);
    c->c12 = c1 | (c2 << 16);
    c->c34 = c3 | (om << 16);
    const int32_t t = m16(k, g4) >> 12, i = t >> 11, f = t & 2047;   /* k G4, Q15: 1/16 per table step */
    const int32_t inv = MO_INV[i] - (m16((int32_t)MO_INV[i] - (int32_t)MO_INV[i + 1], f) >> 11);
    c->invk = inv | (MO_CLIP_K << 16);
}

#if !defined(__bfin__) || defined(MONOLIT_C_LOOP)
static inline int32_t mo_sat32(int64_t v) { return v > 2147483647 ? 2147483647 : v < -2147483647 - 1 ? -2147483647 - 1 : (int32_t)v; }
static inline int32_t mo_fll(int32_t a, int32_t b) { return mo_sat32(2 * (int64_t)((int32_t)(int16_t)a * (int32_t)(int16_t)b)); }
static inline int32_t mo_fhl(int32_t a, int32_t b) { return mo_sat32(2 * (int64_t)((a >> 16) * (int32_t)(int16_t)b)); }
static inline int32_t mo_fhh(int32_t a, int32_t b) { return mo_sat32(2 * (int64_t)((a >> 16) * (b >> 16))); }
static inline int32_t mo_subs(int32_t a, int32_t b) { return mo_sat32((int64_t)a - b); }
static inline int32_t mo_shl2(int32_t a) { return mo_sat32((int64_t)a * 4); }
static inline int32_t mo_shl3(int32_t a) { return mo_sat32((int64_t)a * 8); }
static inline int32_t mo_clip(int32_t x, int32_t kk)                 /* x - x^3 / (3 L^2) up to +-L, Q16 */
{
    x = mo_shl2(x) >> 2;
    const int32_t xi = x >> 16;
    const int32_t q = ((int32_t)(int16_t)(m16(xi, xi) >> 12) * (kk >> 16)) >> 12;
    return x - mo_fhl(x, q);
}
/* one chunk in place: b (the mix at half scale) -> the ladder -> b; s the stages, Q16 */
static void mo_chunk_c(int16_t *b, int32_t *s, const mo_coef_t *c, int32_t gxog, int32_t mu, int32_t *pyp, int32_t *pdc,
                       int res)
{
    int32_t s1 = s[0], s2 = s[1], s3 = s[2], s4 = s[3], yp = *pyp, dc = *pdc;
    for (int n = 0; n < MO_CHUNK; n++) {
        const int32_t x = mo_shl2(mo_fll(b[n], gxog));
        int32_t u;
        if (res) {
            const int64_t a = 2 * (int64_t)((s1 >> 16) * (int16_t)c->c12) + 2 * (int64_t)((s3 >> 16) * (int16_t)c->c34)
                            + 2 * (int64_t)((s2 >> 16) * (c->c12 >> 16)) + 2 * (int64_t)((s4 >> 16) * (c->c34 >> 16));
            const int32_t S = mo_sat32(a);
            u = mo_clip(mo_fhl(mo_subs(x, mo_shl3(mo_fhh(S, c->ggk))), c->invk), c->invk);
        } else u = mo_clip(x, c->invk);
        int32_t v, y;
        v = mo_fhl(mo_subs(u, s1), c->ggk); y = s1 + v; s1 = y + v;
        v = mo_fhl(mo_subs(y, s2), c->ggk); y = s2 + v; s2 = y + v;
        v = mo_fhl(mo_subs(y, s3), c->ggk); y = s3 + v; s3 = y + v;
        v = mo_fhl(mo_subs(y, s4), c->ggk); y = s4 + v; s4 = y + v;
        yp = y;
        y = yp - dc; dc += y >> 10;                                     /* AC coupling: 14 Hz at 88.2 kHz */
        y = mo_shl2(mo_fhl(y, mu));                                     /* make-up and level, Q13 */
        b[n] = (int16_t)(mo_shl3(mo_fhh(mo_clip(y, c->invk), gxog)) >> 16);   /* soft output stage, x og */
    }
    s[0] = s1; s[1] = s2; s[2] = s3; s[3] = s4; *pyp = yp; *pdc = dc;
}
#else
/* monolit_fc.S (gen_fc.py): mo_chunk_c, but the input is the int32 mix at mix (its low half), the output b */
void mo_fc_plain(int16_t *b, int32_t *lad, const mo_coef_t *lc, int32_t gxog, int32_t mix, const mo_sat_t *st,
                 int32_t *pyp, int32_t *pdc);
void mo_fc_res(int16_t *b, int32_t *lad, const mo_coef_t *lc, int32_t gxog, int32_t mix, const mo_sat_t *st,
               int32_t *pyp, int32_t *pdc);
void mo_fc_plain128(int16_t *b, int32_t *lad, const mo_coef_t *lc, int32_t gxog, int32_t mix, const mo_sat_t *st,
                    int32_t *pyp, int32_t *pdc);                 /* the same over 128 samples */
void mo_fc_res128(int16_t *b, int32_t *lad, const mo_coef_t *lc, int32_t gxog, int32_t mix, const mo_sat_t *st,
                  int32_t *pyp, int32_t *pdc);
#endif

/* log2(v) in Q12 (mantissa read linearly, within 0.09 octave): once per block */
static int32_t log2q12(uint32_t v)
{
    int32_t e = 31;
    if (!v) return 0;
    while (!(v & 0x80000000u)) { v <<= 1; e--; }
    return e * 4096 + (int32_t)((v >> 19) & 4095);
}

/* polyBLEP saw, Q15. rdt ~ 2^32 / inc, a little low so t * rdt never passes 2^32 (t < inc) */
static inline int32_t saw(uint32_t t, uint32_t inc, uint32_t rdt)
{
    int32_t s = (int32_t)(t >> 16) - 32768;
    if (t < inc) {
        const int32_t a = 32768 - (int32_t)((t * rdt) >> 17);
        s += (a * a) >> 15;
    } else {
        const uint32_t nt = 0u - t;
        if (nt < inc) {
            const int32_t a = 32768 - (int32_t)((nt * rdt) >> 17);
            s -= (a * a) >> 15;
        }
    }
    return s;
}

static void note_on(void *self, const int16_t *q)
{
    br_claim();
    br_voice *v = &voices[op1_voice_index(self)];
    (void)q;
    v->rnd = v->rnd * 1664525u + 1013904223u + (uint32_t)op1_voice_index(self) * 0x9E3779B9u;
    v->spread = (int32_t)(v->rnd >> 20) - 2048;
    v->stage = 0;
    v->ready = 0;
    v->cgg = -1;                                   /* solve the coefficients anew (also a fresh voice's zeros) */
    br_rel[op1_voice_index(self)] = 0;
}

static void note_off(void *self)
{
    br_rel[op1_voice_index(self)] = 1;
}

static inline int32_t clampk(int32_t x) { return x < 0 ? 0 : x; }

/* everything the knob words (0..7) alone decide, recomputed out of line only when one of them changed */
typedef struct {
    int16_t q[8];
    int32_t valid;
    int32_t o1, o2, o3, osub, det;   /* shape: the levels and osc 2's detune */
    int32_t damt, bsel, age;
    int32_t amt, sus, ra, rd;        /* contour (rd clamped to 32767) */
    int32_t vs, vsn, vlv;            /* velocity: vs, 32767 - vs, 8192 + (vs * 24575 >> 15) */
    int32_t x0;                      /* 4096 + the cutoff knob's part */
    int32_t k, mu, gx, make;         /* heat: the ladder's feedback (k - fb, Q12), the make-up (Q12), the input gain
                                      * (Q15), the level (Q13) */
} br_knobs;
static br_knobs br_kc __attribute__((aligned(32)));

static void __attribute__((noinline)) br_knob_update(const int16_t *q)
{
    br_knobs *c = &br_kc;
    for (int i = 0; i < 8; i++) c->q[i] = q[i];
    c->valid = 1;
    const int32_t k0 = clampk(q[0]), k1 = clampk(q[1]), k2 = clampk(q[2]), k3 = clampk(q[3]);
    const int32_t body = clampk(q[5]), vs = clampk(q[6]), age = clampk(q[7]);

    /* shape: interpolate the breakpoints */
    int32_t pos = k0 * 4, bi = pos >> 15, bf = pos & 32767;
    if (bi > 3) { bi = 3; bf = 32767; }
    int32_t lv[5];
    for (int j = 0; j < 5; j++) lv[j] = shape[bi][j] + (((shape[bi + 1][j] - shape[bi][j]) * bf) >> 15);
    c->o1 = lv[0] >> 1; c->o2 = lv[1] >> 1; c->o3 = lv[2] >> 1; c->osub = lv[3] >> 1; c->det = lv[4];   /* the levels at
                                                       * half scale: the widest stack's sum stays in 16 bits, the ladder's input */

    /* age: three slow random walks, 1 .. 8 cents (Q16: 38 .. 303) */
    c->damt = 38 + ((age * 265) >> 15);
    c->age = age;

    /* body: osc 3's interval */
    int32_t bsel = (body * 5) >> 15;
    if (bsel > 4) bsel = 4;
    c->bsel = bsel;

    /* contour */
    int32_t amt = 0, dec_ms, sus = 0, att_ms = 0;
    /* the left half: the amount up to 6.5 octaves by a fifth of the knob, the decay 15 .. 400 ms on a square law (most
     * of the travel in short plucks: a deep, fast sweep is the Minimoog bass; 2026-11-04, was 5 octaves by half the knob
     * and 60 .. 400 ms linear, which put a deep sweep only with a 400 ms decay) */
    if (k2 >= 1311) amt = ((k2 * 5 > 32767 ? 32767 : k2 * 5) * 26624) >> 15;
    if (k2 < 16384) dec_ms = 15 + ((((k2 * k2) >> 14) * 385) >> 14);
    else {
        dec_ms = 400 + (((k2 - 16384) * 2100) >> 14);
        sus = ((k2 - 16384) * 2 * 11469) >> 15;
    }
    if (k2 > 19661) att_ms = ((k2 - 19661) * 500) / 13107;
    const int32_t rd = dec_ms < 3 ? 32767 : 47554 / dec_ms;   /* per block: 1.451 ms / tau, Q15 */
    c->ra = att_ms < 3 ? 32767 : 47554 / att_ms;
    c->rd = rd > 32767 ? 32767 : rd;
    c->amt = amt;
    c->sus = sus;

    /* velocity */
    c->vs = vs;
    c->vsn = 32767 - vs;
    c->vlv = 8192 + ((vs * 24575) >> 15);

    /* cutoff */
    c->x0 = 4096 + ((k1 * 30000) >> 15);
    if (k1 > 24576) c->x0 += ((k1 - 24576) * 4096) >> 13;     /* the last quarter: one more octave (the ladder opens) */

    /* heat (2026-11-04): the resonance k (Q12) rises to 4.12 at the top, where the ladder self-oscillates (Moai's
     * emphasis at full). The Minimoog's feedback trick (its output patched back into the mixer: fuller, driven, less
     * whistle) swells over the first half, holds to 70 % and is gone by 92 %, out of the self-oscillation's way. In the
     * zero-delay loop the fed-back output is the resonance's own feedback with the opposite sign, so it is k - fb, held
     * at 0 (within 1 dB of Moai's explicit feedback path, source-only renders), and free per sample. The input drive
     * goes 1 .. 2x; the output loses 1 / (1 + 0.6 (drive - 1)) and up to 20 % for the feedback (level-compensated). */
    const int32_t h2 = (k3 * k3) >> 15;
    const int32_t k = (((k3 * 5734 + h2 * 8602) >> 15) * 38621) >> 15;      /* x 16896 / 14336: 4.12 at the top */
    const int32_t fb = k3 < 16384 ? k3 >> 2 : k3 < 22938 ? 4096 : k3 < 30147 ? ((30147 - k3) * 18618) >> 15 : 0;
    c->k = k > fb ? k - fb : 0;
    c->mu = 4096 + ((k * 1400) >> 12);                         /* Moai's make-up: the passband the resonance takes */
    const int32_t drive = 4096 + (h2 >> 3);
    c->gx = (drive * 1075) >> 12;                              /* one saw at about 0.3 of the ladder's full scale */
    const int32_t makez = 134213632 / (4096 + (((drive - 4096) * 19661) >> 15));          /* Q15 */
    const int32_t fbc = 32768 - ((fb * 52429) >> 15);                                    /* Q15: 1 - 0.2 fb */
    c->make = ((((makez * 10650) >> 12) * (fbc >> 1)) >> 14) >> 2;                       /* Q13 */
    if (k3 > 29491) c->make = (c->make * (32768 - ((k3 - 29491) << 2))) >> 15;   /* the whistle: -4.4 dB at the top */
}

/* the voice's pitch facts, recomputed out of line only when the block's first increment changed */
static void __attribute__((noinline)) br_pitch(br_voice *v, uint32_t inc0)
{
    uint32_t rdt = 0xFFFFFFFFu / inc0;
    rdt -= rdt >> 5;                           /* margin for detune, drift and in-block pitch moves */
    v->inc0 = inc0;
    v->rdt = rdt;
    const int32_t l = log2q12(inc0);
    v->key = ((l - log2q12(C4_INC)) * 2458) >> 12;   /* 60 % key tracking */
    v->floor_x = l - log2q12(F40_INC) + 2048;        /* never below half an octave over the note */
}

static void render(void *self, int16_t *out, const int16_t *q)
{
    br_claim();
    br_voice *v = &voices[op1_voice_index(self)];
    if (br_rel[op1_voice_index(self)]) v->stage = 2;
    for (int i = 0; i < 4; i++) monolit_live[i] = q[i];
    {
        const int16_t *cq = br_kc.q;
        if (!br_kc.valid || q[0] != cq[0] || q[1] != cq[1] || q[2] != cq[2] || q[3] != cq[3] || q[5] != cq[5]
            || q[6] != cq[6] || q[7] != cq[7])
            br_knob_update(q);
    }
    const br_knobs *c = &br_kc;
    const int32_t vg = op1_voice_vel_gain(self);
    const int32_t det = c->det;

    /* age: three slow random walks, per block at 689 blocks/s (the design's walk at 344/s with its rates halved and
     * its noise / sqrt 2: the same spread and speed within 3 %) */
    const int32_t damt = c->damt;
    int32_t m[3];
    for (int j = 0; j < 3; j++) {
        v->rnd = v->rnd * 1664525u + 1013904223u;
        v->dv[j] += ((((int32_t)(v->rnd >> 20) - 2048) * 181) >> 8) - (v->drift[j] >> 7);
        v->dv[j] -= v->dv[j] >> 5;
        v->drift[j] += v->dv[j] >> 5;
        if (v->drift[j] > 32767) v->drift[j] = 32767;
        if (v->drift[j] < -32767) v->drift[j] = -32767;
        m[j] = (v->drift[j] * damt) >> 15;
    }
    m[1] += det;

    /* body: osc 3's interval */
    const int32_t bsel = c->bsel;
    if (bsel == 1) m[2] -= (det * 23) >> 5;   /* unison: detuned the other way */

    /* block-rate pitch facts */
    uint32_t inc0 = (uint32_t)OP1_PITCH_INC[0];
    if (inc0 < 4096) inc0 = 4096;
    if (inc0 != v->inc0) br_pitch(v, inc0);
    const uint32_t rdt = v->rdt;
    const uint32_t rdt3 = bsel == 0 ? rdt << 1 : bsel == 1 ? rdt : bsel == 2 ? (rdt * 21845u) >> 15
                        : bsel == 3 ? rdt >> 1 : rdt << 2;

    /* contour */
    if (v->stage == 0) {
        v->env += ((34000 - v->env) * c->ra) >> 15;
        if (v->env >= 32767) { v->env = 32767; v->stage = 1; }
    } else {
        v->env += (((v->stage == 1 ? c->sus : 0) - v->env) * c->rd) >> 15;
    }

    /* velocity: filter amount, cutoff and level depth */
    const int32_t vs = c->vs;
    const int32_t amt_v = (c->amt * (c->vsn + ((vs * vg) >> 15))) >> 15;
    const int32_t xv = (vs * ((vg - 16384) >> 2)) >> 15;
    const int32_t lvel = 32767 - (((32767 - vg) * c->vlv) >> 15);

    /* cutoff: 80 Hz .. 12.8 kHz at C4 on the knob's first three quarters, up to 20 kHz with the last and the contour */
    int32_t x = c->x0 + v->key + ((v->env * amt_v) >> 15) + xv + ((v->spread * c->age) >> 16);
    if (x < v->floor_x) x = v->floor_x;
    if (x < 0) x = 0;
    if (x > MO_X_MAX) x = MO_X_MAX;
    const int32_t post = (x >> 2) + 1024;                     /* Q12 octaves over 40 Hz -> Q10 octaves over 20 Hz */
    const int32_t pos0 = v->ready ? v->pos : post;
    v->pos = post;
    v->ready = 1;

    /* the level (Q15, the velocity and heat's compensation; 29612 is the 2026-10-29 level pass's +8.6 dB) times the
     * resonance's make-up, before the output's soft clip (Q13, at most 4x: it also trims self-oscillation's level) */
    const int32_t gz = (((29612 * lvel) >> 15) * c->make) >> 13;
    int32_t mu = (c->mu * gz) >> 14;
    if (mu > 32767) mu = 32767;
    const int32_t gxog = c->gx | (MO_OG << 16);

    const int32_t o1 = c->o1, o2 = c->o2, o3 = c->o3, osub = c->osub;
#if defined(__bfin__) && !defined(MONOLIT_C_LOOP)
    /* monolit_dsp.S's mo_run: the oscillators in passes into br_hp->mix (the parameter block b: see its header); the
     * ladder's kernels read the mix from there and write out[] */
    {
        int32_t *b = br_hp->blk;
        const uint32_t rsub = rdt << 1;
        b[0] = (int32_t)((uint32_t)(uint16_t)m[0] | (uint32_t)o1 << 16);
        b[1] = (int32_t)rdt;
        b[2] = (int32_t)((uint32_t)(uint16_t)m[1] | (uint32_t)o2 << 16);
        b[3] = (int32_t)rdt;
        b[4] = (int32_t)((uint32_t)(uint16_t)m[2] | (uint32_t)o3 << 16);
        b[5] = (int32_t)rdt3;
        b[6] = (int32_t)((uint32_t)(uint16_t)m[0] | (uint32_t)osub << 16);
        b[7] = (int32_t)rsub;
        b[8] = (osub * -16384) >> 15;
        b[9] = (osub * 16384) >> 15;
        b[19] = bsel;
        b[20] = (int32_t)(uintptr_t)br_hp->pre;
        b[21] = (int32_t)(uintptr_t)br_hp->mix;
        mo_run(v, out, b);
    }
#else
    {
        uint32_t p0 = v->ph[0], p1 = v->ph[1], p2 = v->ph[2], p3 = v->ph[3];
        const uint32_t rsub = rdt << 1;
        static const uint8_t ratio4[5] = {2, 4, 6, 8, 1};   /* osc 3 / osc 1, in quarters */
        const uint32_t r3 = ratio4[bsel];
        for (int i = 0; i < OP1_BLOCK; i++) {
            const uint32_t inc = (uint32_t)OP1_PITCH_INC[i];
            const uint32_t i1 = detune(inc, m[0]);
            const uint32_t i2 = detune(inc, m[1]);
            const uint32_t i3 = detune((inc >> 2) * r3, m[2]);
            const uint32_t isub = i1 >> 1;
            p0 += i1; p1 += i2; p2 += i3; p3 += isub;
            const int32_t sq = (saw(p3, isub, rsub) - saw(p3 + 0x80000000u, isub, rsub)) >> 1;
            const int32_t mix = ((o1 * saw(p0, i1, rdt)) >> 15) + ((o2 * saw(p1, i2, rdt)) >> 15)
                              + ((o3 * saw(p2, i3, rdt3)) >> 15) + ((osub * sq) >> 15);
            out[i] = (int16_t)mix;                            /* half scale already (the levels) */
        }
        v->ph[0] = p0; v->ph[1] = p1; v->ph[2] = p2; v->ph[3] = p3;
    }
#endif
    /* the ladder, two chunks: the cutoff position moves from last block's to this block's (one call while it holds:
     * the same samples, the same coefficients for both halves) */
    for (int ch = 0; ch < OP1_BLOCK / MO_CHUNK; ch++) {
        const int32_t pos = pos0 + (((post - pos0) * (ch + 1)) >> 1);
        const int32_t gi = pos >> 6;
        const int32_t gg = MO_G[gi] + ((((int32_t)MO_G[gi + 1] - (int32_t)MO_G[gi]) * (pos & 63)) >> 6);
        mo_coef_t *lcp = (mo_coef_t *)v->clc;
        if (gg != v->cgg || c->k != v->ck) { mo_coef(lcp, gg, c->k); v->cgg = gg; v->ck = c->k; }
        const mo_coef_t lc = *lcp;
        int16_t *bp = out + ch * MO_CHUNK;
#if defined(__bfin__) && !defined(MONOLIT_C_LOOP)
        const mo_sat_t st = { 0, 0, 0, mu };
        const int32_t mx = (int32_t)(uintptr_t)(br_hp->mix + 1 + ch * MO_CHUNK);   /* the kernels read the int32 mix */
        if (pos0 == post) {
            (lc.ggk >> 16 ? mo_fc_res128 : mo_fc_plain128)(bp, v->s, &lc, gxog, mx, &st, &v->yp, &v->dc);
            break;
        }
        (lc.ggk >> 16 ? mo_fc_res : mo_fc_plain)(bp, v->s, &lc, gxog, mx, &st, &v->yp, &v->dc);
#else
        mo_chunk_c(bp, v->s, &lc, gxog, mu, &v->yp, &v->dc, lc.ggk >> 16 != 0);
#endif
    }
    monolit_env = v->env;
}

const op1_engine_t ENGINE_monolit = { note_on, render, note_off,
                                    { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_NONE } };

/* ---- screen: monolit.svg is the night landscape and the monolith (gen_svg.py); drawn here with the knobs as the
 * voices hear them:
 *   green the filter ridge: a resonance curve standing over the monolith, wider as the cutoff opens
 *   red   its tip: longer, and the summit higher, with heat
 *   blue  rings on the ground round the monolith's foot: two to seven with shape; they ripple out while the contour is up
 *   white the contour's orbit: a short low arc (pluck) to a wide high one (swell), gone with the contour off; its dot
 *         climbs with the contour
 *   page 2: the paged knobs' names and values, top right (BODY, VEL, AGE)
 * Live lines are hidden where they pass behind the monolith; the rings also behind the horizon. */
#include "screens/stroke_font.h"
#pragma GCC push_options
#pragma GCC optimize("Os")                             /* GUI code: size, not speed */
#define QP(v) ((int32_t)((v) * 1024))
#define MO_HORIZON 104                                 /* gen_svg.py's HORIZON and MONO */
#define MO_X 160
#define MO_FOOT 116
static const uint8_t B_BLUE[4] = { 0x69, 0x8E, 0xFF, 0xFF }, B_GREEN[4] = { 0x00, 0xED, 0x95, 0xFF },
                     B_WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, B_RED[4] = { 0xFF, 0x3A, 0x5D, 0xFF };
static int32_t mo_phase;

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    const op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}

/* Q10 point behind the monolith (apex y 32, shoulders +-5 at y 42, foot +-10 at y 116), with a 2 px margin */
static int behind(int32_t x, int32_t y)
{
    x >>= 10; y >>= 10;
    if (y < 30 || y > MO_FOOT + 2) return 0;
    const int32_t d = y < 42 ? (y > 32 ? (y - 32) >> 1 : 0) : 5 + (y - 42) * 5 / 74;
    return x >= MO_X - d - 2 && x <= MO_X + d + 2;
}

static void seg(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    if (!behind(x0, y0) && !behind(x1, y1)) ln(r, x0, y0, x1, y1, w, c);
}

static void num(char *b, int32_t v)                    /* 0 .. 100 */
{
    if (v >= 100) { b[0] = '1'; b[1] = '0'; b[2] = '0'; b[3] = 0; return; }
    int i = 0;
    if (v >= 10) b[i++] = (char)('0' + v / 10);
    b[i++] = (char)('0' + v % 10);
    b[i] = 0;
}

static void mo_screen_init(void *scr) { (void)scr; }

static void mo_screen_update(void *scr, int dt)
{
    int16_t kn[4];
    op1_synth_knobs_live(OP1_SCREEN_SLOT & 7, kn);
    const int16_t *words = op1_synth_knobs(OP1_SCREEN_SLOT & 7);
    int32_t k[4];
    for (int i = 0; i < 4; i++) k[i] = kn[i] < 0 ? 0 : kn[i];
    const int32_t env = monolit_env < 0 ? 0 : monolit_env;
    op1_screen_draw(scr, dt);
    void *r = op1_raster();

    /* blue rings round the foot (160, 116), flattened 7:1; radius up to 140 px; drifting out while the contour is up */
    int n = 2 + ((k[0] * 6) >> 15);
    if (n > 7) n = 7;
    if (env > 2000) mo_phase = (mo_phase + (env >> 7)) & 32767;
    for (int i = 0; i < n; i++) {
        const int32_t rx = ((i * 32768 + mo_phase) >> 5) * 140 / n;   /* Q10 */
        if (rx < QP(14)) continue;
        const int32_t ry = rx / 7;
        int32_t px = 0, py = 0;
        for (int j = 0; j <= 32; j++) {
            const int32_t t = j * 256;
            const int32_t x = QP(MO_X) + (((rx >> 5) * OP1_SINE_TABLE[(t + 2048) & 8191]) >> 10);
            const int32_t y = QP(MO_FOOT) + (((ry >> 5) * OP1_SINE_TABLE[t & 8191]) >> 10);
            if (j && py >= QP(MO_HORIZON + 2) && y >= QP(MO_HORIZON + 2)) seg(r, px, py, x, y, QP(1.5), B_BLUE);
            px = x; py = y;
        }
    }

    /* green ridge: y = 100 - h * w^2 / (w^2 + d^2) across x 14 .. 306; width w 10 .. 66 px with the cutoff,
     * summit y 28 .. 18 with heat; the top (2 .. 16 px) red */
    const int32_t w = 10 + ((k[1] * 56) >> 15), w2 = w * w * 64;      /* d in 1/8 px */
    const int32_t h = 72 + ((k[3] * 10) >> 15), tip = QP(2) + ((k[3] * 14) >> 5), top = QP(100 - h);
    for (int s = -1; s <= 1; s += 2) {
        int32_t px = 0, py = 0;
        for (int i = 0; i <= 36; i++) {
            const int32_t d8 = i * i * 146 * 8 / (36 * 36);
            const int32_t f = (w2 << 12) / (w2 + d8 * d8);                    /* Q12, 1 at the summit */
            const int32_t x = QP(MO_X) + s * (d8 << 7), y = QP(100) - (((h << 10) * f) >> 12);
            if (i) seg(r, px, py, x, y, QP(2.5), (py - top < tip && y - top < tip) ? B_RED : B_GREEN);
            px = x; py = y;
        }
    }

    /* white orbit: the contour, a dashed arc from the horizon over the monolith; its dot climbs with the contour */
    if (k[2] >= 1000) {
        const int32_t span = 24 + ((k[2] * 110) >> 15), rise = 12 + ((k[2] * 70) >> 15);
        const int32_t x0 = QP(MO_X - span), cy = QP(100 - 2 * rise), x2 = QP(MO_X + span), y0 = QP(100);
        int32_t px = x0, py = y0;
        for (int i = 1; i <= 24; i++) {
            const int32_t t = (i << 12) / 24, u = 4096 - t;
            const int32_t x = ((u * u >> 12) * (x0 >> 4) + (2 * u * t >> 12) * (QP(MO_X) >> 4) + (t * t >> 12) * (x2 >> 4)) >> 8;
            const int32_t y = ((u * u >> 12) * (y0 >> 4) + (2 * u * t >> 12) * (cy >> 4) + (t * t >> 12) * (y0 >> 4)) >> 8;
            if (i & 1) seg(r, px, py, x, y, QP(1.5), B_WHITE);
            px = x; py = y;
        }
        if (env > 300) {
            const int32_t t = env >> 4, u = 4096 - t;             /* 0 .. 1/2 of the way: up to the top of the arc */
            const int32_t x = ((u * u >> 12) * (x0 >> 4) + (2 * u * t >> 12) * (QP(MO_X) >> 4) + (t * t >> 12) * (x2 >> 4)) >> 8;
            const int32_t y = ((u * u >> 12) * (y0 >> 4) + (2 * u * t >> 12) * (cy >> 4) + (t * t >> 12) * (y0 >> 4)) >> 8;
            seg(r, x - QP(1), y, x + QP(1), y, QP(5), B_WHITE);
        }
    }

    /* page 2 labels */
    static const char *const body[5] = { "SUB", "UNI", "5TH", "OCT", "SUB2" };
    static const char *const name[3] = { "BODY ", "VEL ", "AGE " };
    static const uint8_t *const col[3] = { B_BLUE, B_GREEN, B_WHITE };
    for (int i = 0; i < 3; i++) {
        if (!op1_eng_page[i]) continue;
        const int32_t v = words[5 + i] < 0 ? 0 : words[5 + i];
        char b[5];
        const int32_t x = sf_text(r, QP(214), QP(14) + i * QP(14), QP(1.5), name[i], QP(1.4), col[i]);
        if (i == 0) {
            int32_t s = (v * 5) >> 15;
            sf_text(r, x, QP(14), QP(1.5), body[s > 4 ? 4 : s], QP(1.4), col[i]);
        } else {
            num(b, (v * 100 + 16383) >> 15);
            sf_text(r, x, QP(14) + i * QP(14), QP(1.5), b, QP(1.4), col[i]);
        }
    }
}
#pragma GCC pop_options

const op1_screen_t SCREEN_monolit = { mo_screen_init, mo_screen_update };
