/* "palme" - Ondes Martenot diffuseurs (Maurice Martenot): the "Palme" (1950), a palm-leaf soundboard with 12
 * chromatic strings that ring in sympathy with the loudspeaker, morphing into the "Metallique" gong speaker.
 * 12 Karplus-Strong strings excited by the input, tuned to the 12 semitones above the root. Each loop is one
 * 3-tap kernel = linear-interpolated fractional delay (1 - f, f) convolved with the Karplus-Strong averaging
 * filter (1 - b, b) with the decay gain folded into the taps; the filter's delay b is taken off the line
 * length so the fundamental stays in tune (FFT-verified within 1 cent for all 36 roots x 12 strings);
 * + excitation, - DC servo, saturating (= the loop clamp). Metallique glides the same 12 loops to free-plate
 * mode ratios (8 modes + 4 split twins that beat) and drives the sum (up to 2x) into a soft clip whose
 * residue is fed back into the strings while they are struck (energy blooms into the other modes).
 *   knob 0 (blue)  : tuning    root note C2 .. B4 in semitones (strings = root .. root + 11)
 *   knob 1 (green) : sustain   string decay T60 0.25 s .. 30 s
 *   knob 2 (white) : excite    input drive into the strings + brightness of the excitation
 *   knob 3 (red)   : mix       dry .. dry + strings .. strings
 * press (page 2, params[5..7], saved with the patch):
 *   blue  -> metallique  morph palme -> gong
 *   green -> damping     loop filter colour: dark (felt) .. bright (wire)
 *   white -> spread      detune spread 0 .. +-35 cents (fixed pattern per string)
 *   red   -> LATCH "etouffoir off" (not saved): the dampers lift. While input plays the loops sit at unity gain
 *            (they collect what they hear); ~0.3 s after it stops, each string's fundamental is held forever by
 *            an AGC (unity gain computed from the loop losses + PI on the fundamental's energy). Per-string
 *            loop limiter and a DC servo on each loop, so it never runs away or drifts.
 * Memory: 12 ring buffers of 1024 int16 (+ a 2-sample mirror of each ring's end in front of it) in this
 * position's scratch buffer, 25.7 KB. No periodic bulk work: a string's block is at most 2 runs (split where
 * its reads cross the ring's start; no masks per sample). Init only resets state (it runs in the audio
 * interrupt); the scratch is cleared over the first 8 blocks (strings silent meanwhile, the dry part passes).
 * CPU: string-major inner loop (one string over the 64-sample block: 3 fract16 MACs, 17 instructions per
 * sample); analysis / pitch / gain for 2 strings per block. ~371 instructions per sample in all: ~4.1 % of a
 * 400 MHz Blackfin at 1 instruction per cycle (bfin-elf-gcc -O2 objdump count); worst block 1.06 x median.
 * Copyright (C) 2026 ultralazr. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"
#include "palme_geom.h"

#define NS        PG_NS
#define RL        1024                 /* ring per string (power of 2): longest look-back 682 (P <= 674.2 + 8) */
#define RM        (RL - 1)
#define RG        48                   /* in front of each ring: [-2, -1] mirror ring[RL - 2 .. RL - 1]; the rest
                                        * pads the stride to an odd number of 32-byte cache lines (no aliasing) */
#define SL        (RG + RL)            /* line stride: 12 x 1072 x 2 = 25728 bytes of the 176460 */
#define DMAX      (RL - OP1_FX_BLOCK - 16)   /* clamp of read delay / DC window (never reached: P <= 674.2) */
#define NCLR      8                    /* blocks to clear the scratch after init */
#define CLRW      (NS * SL / 2 / NCLR) /* 32-bit words cleared per block (804)   */
#define SLIM      14000                /* per-string soft limit: above it the loop gets extra damping          */
#define ECAP      (1750 * 1750)        /* latch: highest held fundamental, (A / 4)^2: A = 7000                 */
#define EFLOOR    (12 * 12)            /* latch: fundamentals below A = 48 are not held (no noise drones)      */
#define D0_Q7     86303                /* period of C2 (65.406 Hz) at 44.1 kHz, Q7 samples                     */
#define A0_Q27    84095                /* 6.9078 / (44100 * 0.25 s), Q27: decay rate for T60 = 0.25 s          */
#define RELEASE   256                  /* below this peak (-42 dB), with no input and no latch hold: fade out  */
#define FBG       1000                 /* metallique bloom: soft-clip residue fed back into the strings, Q15   */
#define GATE      12                   /* a string whose peak fell to this (-68 dB) with no input is stopped    */
#define SETTLE    200                  /* latch: blocks (290 ms) after the input stops until the levels freeze */

typedef struct {
    int16_t *L;                         /* NS rings of RL samples in scratch, stride SL                         */
    int32_t  wp;                        /* write index (0 .. RL - 64, steps of 64), shared by all strings       */
    int32_t  age;                       /* samples written since the start (saturates at RL)                    */
    int32_t  P[NS];                     /* loop period, Q16 samples (glides to the target)                     */
    int32_t  t24[NS][3];                /* loop taps, Q24 (set by the slow update)                              */
    int16_t  di[NS], fr[NS];            /* read delay: integer part, fraction Q11                               */
    int32_t  q[NS][3];                  /* sigma-delta remainders of the 3 loop taps (Q24 -> Q15)               */
    int32_t  S[NS];                     /* DC servo: sum of the last pw[] samples of each line                  */
    int32_t  dgl[NS];                   /* latch: loop gain - 1 from the AGC, Q24                               */
    int32_t  env[NS];                   /* string level (peak over the last period, smoothed)                  */
    int32_t  fe[NS];                    /* energy of the fundamental, (A / 4)^2 (smoothed)                      */
    int32_t  tgt[NS], integ[NS];        /* latch: held fundamental energy, PI integrator (Q24)                  */
    int32_t  ein, lp, hq, ow;           /* input level, excitation low-pass, input DC (Q8), output DC (Q10)     */
    int32_t  fb[OP1_FX_BLOCK], fbsum;   /* metallique: soft-clip residue of the last block, its sum              */
    int16_t  pw[NS], kq[NS];            /* DC servo window (samples), strength kappa / P (Q20)                  */
    uint16_t czm;                       /* bit i: string i cleared (gate) and no excitation since: still zero  */
    uint8_t  latched, quiet, rr, clr;   /* latch flag of the last block; blocks since the input stopped; slow
                                         * update round robin 0..5 (strings rr, rr + 6); scratch blocks cleared */
} palme_t;

static palme_t st[3];                                   /* synth, drum, master */
static volatile int16_t s_env[3][NS];                    /* per-string level for the screen */
static volatile int16_t s_in[3];                         /* input level for the screen (speaker) */

/* 2^(-k/256), Q15, k = 0..256, expanded from palme_geom.h's packed form by the first init (256 short steps, once) */
static uint16_t pm_pow2n[257];
static void pow2n_expand(void)
{
    if (pm_pow2n[256]) return;                          /* (written last: set once complete) */
    int32_t v = 32768, d = PM_POW2_D0;
    for (int k = 0; k < 256; k++) {
        pm_pow2n[k] = (uint16_t)v;
        v -= d;
        d -= ((pm_pow2z[k >> 2] >> ((k & 3) * 2)) & 3) - 1;   /* second difference + 1, 2 bits */
    }
    pm_pow2n[256] = (uint16_t)v;
}

/* v * 2^(-n) * 2^(15 - sh): n = octaves Q16 (>= 0), v <= 2^17 (the Q15 mantissa product fits 32 bits) */
static uint32_t pow2n(uint32_t v, int32_t n, int sh)
{
    if (n < 0) n = 0;
    uint32_t i = (uint32_t)n >> 16, f = (uint32_t)n & 0xffff, k = f >> 8, t = f & 0xff;
    uint32_t m = pm_pow2n[k] - (((uint32_t)(pm_pow2n[k] - pm_pow2n[k + 1]) * t) >> 8);   /* Q15 */
    uint32_t r = (v * m) >> sh;
    return i < 31 ? r >> i : 0;
}

/* 16-bit ops: gcc emits the slow 32 x 32 "*=" for clamped int32 values, so spell them out on Blackfin
 * (fract builtins, one instruction each). MULQ15 = (a * b) >> 15 (truncating; saturates only -1 * -1),
 * MUL16 = a * b, ADDQ / SUBQ = 16-bit saturating add / subtract, MULF = a * b * 2 (fract32), ADDF =
 * saturating 32-bit add. The host build gets the same in C. */
#ifdef __bfin__
#define MULQ15(a, b) ((int32_t)__builtin_bfin_mult_fr1x16((int16_t)(a), (int16_t)(b)))
#define MUL16(a, b)  (__builtin_bfin_mult_fr1x32((int16_t)(a), (int16_t)(b)) >> 1)
#define ADDQ(a, b)   ((int32_t)__builtin_bfin_add_fr1x16((int16_t)(a), (int16_t)(b)))
#define SUBQ(a, b)   ((int32_t)__builtin_bfin_sub_fr1x16((int16_t)(a), (int16_t)(b)))
#define MULF(a, b)   (__builtin_bfin_mult_fr1x32((int16_t)(a), (int16_t)(b)))
#define ADDF(a, b)   (__builtin_bfin_add_fr1x32((a), (b)))
#else
static inline int32_t MULQ15(int16_t a, int16_t b) { int32_t r = (a * b) >> 15; return r > 32767 ? 32767 : r; }
static inline int32_t MUL16(int16_t a, int16_t b) { return a * b; }
static inline int32_t ADDQ(int16_t a, int16_t b) { return op1_sat16(a + b); }
static inline int32_t SUBQ(int16_t a, int16_t b) { return op1_sat16(a - b); }
static inline int32_t MULF(int16_t a, int16_t b) { return a == -32768 && b == -32768 ? 0x7fffffff : a * b * 2; }
static inline int32_t ADDF(int32_t a, int32_t b)
{
    int32_t r = (int32_t)((uint32_t)a + (uint32_t)b);
    return ((a ^ r) & (b ^ r)) < 0 ? (a < 0 ? (int32_t)0x80000000 : 0x7fffffff) : r;
}
#endif

static inline int32_t softclip16(int16_t x)              /* Q15, x - x^3/3 (peaks at 2/3) */
{
    int32_t x2 = MULQ15(x, x), x3 = MULQ15(x2, x);
    return x - MULQ15(x3, 10923);
}

/* floor(log2 v) (0 for v <= 1) in constant time */
static inline int msb(uint32_t v)
{
    int b = 0;
    if (v >> 16) { v >>= 16; b += 16; }
    if (v >> 8) { v >>= 8; b += 8; }
    if (v >> 4) { v >>= 4; b += 4; }
    if (v >> 2) { v >>= 2; b += 2; }
    return b + (int)(v >> 1);
}

/* n int16 zeros with 32-bit stores (may_alias: the int16 accesses around it stay ordered) */
typedef int32_t __attribute__((may_alias)) a32_t;
static void zero16(int16_t *p, int32_t n)
{
    if (n <= 0) return;
    if ((uintptr_t)p & 2) { *p++ = 0; n--; }
    a32_t *w = (a32_t *)(void *)p;
    for (int32_t k = 0; k < n >> 1; k++) w[k] = 0;
    if (n & 1) p[n - 1] = 0;
}

/* sum of the samples k0 .. k1 - 1 back from the write index (k1 <= RL), at most 2 contiguous runs */
static int32_t rsum(const int16_t *ls, int32_t wp, int32_t k0, int32_t k1)
{
    int32_t lo = wp - k1, hi = wp - k0, s = 0;          /* indices lo .. hi - 1 */
    if (lo >= hi) return 0;
    if (lo < 0) {
        int32_t e = hi < 0 ? hi : 0;
        for (const int16_t *p = ls + RL + lo; p < ls + RL + e; p++) s += *p;
        lo = 0;
    }
    for (const int16_t *p = ls + lo; p < ls + hi; p++) s += *p;
    return s;
}

/* init can run in the audio interrupt (dispatcher: on the first render after the FX is selected), so it only
 * resets state; render clears the scratch over the first NCLR blocks */
static void palme_init(int pos)
{
    palme_t *s = &st[pos % 3];
    pow2n_expand();
    s->L = (int16_t *)op1_fx_scratch(pos);
    if (!s->L) return;
    s->L += RG;
    s->clr = 0; s->age = 0; s->czm = (1 << NS) - 1;   /* (the ring is all zero once cleared) */
    s->wp = 0; s->rr = 0; s->ein = 0; s->lp = 0; s->hq = 0; s->latched = 0; s->quiet = SETTLE;
    for (int i = 0; i < NS; i++) {
        s->P[i] = 0;                                     /* set from the knobs on the first render */
        s->q[i][0] = s->q[i][1] = s->q[i][2] = 0;
        s->S[i] = 0; s->pw[i] = 0; s->kq[i] = 0; s->dgl[i] = 0; s->di[i] = 2; s->fr[i] = 0;
        s->t24[i][0] = s->t24[i][1] = s->t24[i][2] = 0;
        s->env[i] = s->fe[i] = s->tgt[i] = s->integ[i] = 0;
        s_env[pos % 3][i] = 0;
    }
    for (int n = 0; n < OP1_FX_BLOCK; n++) s->fb[n] = 0;
    s->fbsum = 0; s->ow = 0;
}

static const int16_t pm_cos16[16] = { 2048, 1892, 1448, 784, 0, -784, -1448, -1892, -2048, -1892, -1448, -784, 0, 784, 1448, 1892 };   /* Q11 */
/* detune pattern (x spread), roughly balanced so the chord's centre stays in tune */
static const int8_t pm_spread[NS] = { 0, 90, -58, 127, -108, 38, -77, 102, -127, 64, -26, 115 };   /* /128 */

/* scratch starts with another FX's data: clear 1/NCLR of it per block (no expensive block); meanwhile the
 * strings are silent and the dry part passes. The DSP then starts exactly as from a cleared scratch. */
__attribute__((noinline)) static void palme_clearing(palme_t *s, int pos, int16_t *buf, int32_t k3)
{
    a32_t *w = (a32_t *)(void *)(s->L - RG) + s->clr * CLRW;
    for (int k = 0; k < CLRW; k++) w[k] = 0;
    s->clr++;
    int16_t dryg = (int16_t)(k3 < 16384 ? 32767 : (32767 - k3) * 2);
    int32_t peak = 0;
    for (int n = 0; n < OP1_FX_BLOCK; n++) {
        int32_t o = MULQ15(buf[n], dryg);
        buf[n] = (int16_t)o;
        if (o < 0) o = -o;
        if (o > peak) peak = o;
    }
    volatile int16_t *m = op1_fx_meter(pos);
    int32_t mv = (*m * 0xfe00) >> 16;
    *m = (int16_t)(peak > mv ? peak : mv);
}

static void palme_render(int pos, int16_t *buf, const int16_t *p)
{
    palme_t *s = &st[pos % 3];
    int16_t *L = s->L;
    if (!L) return;
    int32_t k0 = op1_fx_param_q15(p, 0), k1 = op1_fx_param_q15(p, 1);
    int32_t k2 = op1_fx_param_q15(p, 2), k3 = op1_fx_param_q15(p, 3);
    int32_t mg = p[5] < 0 ? 0 : p[5], damp = p[6] < 0 ? 0 : p[6], spread = p[7] < 0 ? 0 : p[7];
    int latch = op1_fx_page[pos % 3][3] != 0;

    if (s->clr < NCLR) { palme_clearing(s, pos, buf, k3); return; }

    /* ---- per block: knob mapping ---- */
    int32_t root = 36 + ((k0 * 36) >> 15);                                  /* C2 .. B4 */
    uint32_t a27 = pow2n(A0_Q27, (k1 * 14143) >> 10, 15);                 /* decay rate: T60 0.25 * 2^(6.9 k) s */
    int32_t bq = 15729 - ((damp * 15401) >> 15);                           /* averaging filter tap b 0.48 .. 0.01 */
    int32_t sp = (spread * 90) >> 15;                                      /* Q8 semitones per unit of pattern */
    int32_t ex2 = (k2 * k2) >> 15;
    int32_t lpc = 1500 + ((ex2 * 29000) >> 15);                            /* excitation low-pass ~240 Hz .. open */
    int32_t drive = 600 + ((ex2 * 15000) >> 15) + (k2 >> 3);               /* -35 .. -3.5 dB into the strings */

    /* input: level (screen, latch), DC removed (the loops would sum it up), low-pass, drive; + gong bloom */
    int16_t ex[OP1_FX_BLOCK] __attribute__((aligned(4)));
    int32_t ipk = 0, lp = s->lp, hq = s->hq;
    /* gong bloom: residue gain, only while struck (input present, + 290 ms): with long sustains or the latch's
     * unity gain it would otherwise pump the loops into a self-sustaining roar */
    int32_t fbg = latch || s->quiet >= SETTLE ? 0 : (mg * FBG) >> 15, fbm = s->fbsum >> 6;
    for (int n = 0; n < OP1_FX_BLOCK; n++) {
        int32_t in = buf[n];
        int32_t a = in < 0 ? -in : in;
        if (a > ipk) ipk = a;
        hq += in - (hq >> 8);                                              /* DC estimate * 256 (27 Hz) */
        lp += MULQ15(SUBQ(SUBQ(in, hq >> 8), lp), lpc);
        ex[n] = (int16_t)ADDQ(MULQ15(lp, drive), MULQ15(SUBQ(s->fb[n], fbm), fbg));
    }
    int32_t exo = 0;                                     /* any excitation in this block (czm) */
    for (int n = 0; n < OP1_FX_BLOCK / 2; n++) exo |= ((const a32_t *)(const void *)ex)[n];
    if (!ipk && lp > -64 && lp < 64) lp = 0;          /* the truncating low-pass has a dead zone: no hum in silence */
    s->lp = lp; s->hq = hq;
    s->ein = ipk > s->ein ? ipk : s->ein - (s->ein >> 4);
    /* latch: while input plays (and SETTLE blocks after) the loops sit at unity gain and the held level follows
     * each string (its line also carries the forced, non-resonant part of the input); then the levels freeze */
    if (s->ein > 300) s->quiet = 0; else if (s->quiet < SETTLE) s->quiet++;
    int follow = s->quiet < SETTLE;
    if (latch && !s->latched)
        for (int i = 0; i < NS; i++) { s->integ[i] = 0; s->dgl[i] = 0; }
    s->latched = (uint8_t)latch;

    int32_t wp = s->wp, age = s->age;
    /* slow per-string work (analysis, pitch, delay, gain, taps) for 2 strings per block, round robin: every
     * string every 6 blocks (8.7 ms). Every block every string only re-quantises its taps and runs its loop. */
    int rr = s->rr; s->rr = (uint8_t)(rr == 5 ? 0 : rr + 1);
    for (int i = 0; i < NS; i++) {
        int16_t *ls = L + i * SL;
        if (i == rr || i == rr + 6) {
            int32_t P = s->P[i], zl = 0;       /* zl: the newest zl samples are known zero (gate below) */
            if (P) {
                /* ---- analysis: 16 points over the last period: peak (limiter, screen), energy of the
                 * fundamental (16-point DFT bin 1: what the latch holds - harmonics may fade with the loop
                 * filter, the note stays) ---- */
                int32_t vmax = -32768, vmin = 32767, sum = 0, ca = 0, sa = 0, env = s->env[i];
                for (int j = 0; j < 16; j++) {
                    int32_t v = ls[(wp - 1 - ((j * (P >> 8)) >> 12)) & RM];
                    ca += v * pm_cos16[j]; sa += v * pm_cos16[(j + 4) & 15]; sum += v;
                    if (v > vmax) vmax = v;
                    if (v < vmin) vmin = v;
                }
                int32_t mean = sum >> 4, pk = vmax - mean > mean - vmin ? vmax - mean : mean - vmin;
                ca >>= 16; sa >>= 16;                                        /* = 8 A cos / 32: A / 4 */
                int32_t fe = s->fe[i] + ((ca * ca + sa * sa - s->fe[i]) >> 1); /* (A / 4)^2 */
                env += (pk - env) >> 1;
                if (env <= GATE && s->ein < 64 && !(latch && s->tgt[i])) {
                    /* rounding keeps a dying loop alive as a tiny limit cycle (-70 dB): clear its history
                     * (what the loop reads in the next period; at most 2 strings per block, ~690 samples each;
                     * nothing to do if it is still zero from the last time, as when a gated string stays silent:
                     * with no excitation, zero history and S = 0 the loop writes zeros (z = 0x8000 >> 16 = 0), and
                     * the zero span grows 64 per block, far faster than the delay can glide) */
                    zl = s->di[i] + 8;
                    if (!(s->czm & (1 << i))) {
                        int32_t k0 = wp - zl;
                        if (k0 >= 0) zero16(ls + k0, zl);
                        else { zero16(ls + RL + k0, -k0); zero16(ls, wp); }
                        ls[-1] = ls[RL - 1]; ls[-2] = ls[RL - 2];            /* keep the mirror in step */
                        s->czm |= (uint16_t)(1 << i);
                    }
                    env = fe = 0; s->S[i] = 0; s->pw[i] = 0;                 /* window refilled below */
                }
                s->fe[i] = fe; s->env[i] = env;
                s_env[pos % 3][i] = (int16_t)env;
                /* loop loss per period at f0 of filter + interpolation: (1 - cos w0) (b(1-b) + f(1-f)),
                 * 1 - cos w0 ~ 2 pi^2 / P^2, Q24 */
                int32_t Pi = P >> 16, fr = s->fr[i], ir = 2048 - fr;
                uint32_t inv = (1u << 30) / (uint32_t)(P >> 6);             /* 1/P, Q20 (division per block) */
                uint32_t i16 = inv >> 4, u = ((((i16 * i16) >> 8) * 20213u) >> 10);   /* 2 pi^2 / P^2, Q24 */
                uint32_t cl = (uint32_t)(((bq * (32768 - bq)) >> 15) + ((fr * ir) >> 7));
                int32_t lf = (int32_t)(((u >> 4) * cl) >> 11);
                /* DC servo strength kappa = 2^-kk (fraction of the loop's DC removed per period): ~ 4 L, so it
                 * beats the latch's excess gain at DC (= L), 1/64 .. 1/8 */
                int32_t kk = 24 - msb((uint32_t)lf) - 2;
                kk = kk < 3 ? 3 : (kk > 6 ? 6 : kk);
                s->kq[i] = (int16_t)(inv >> kk);                             /* kappa / P, Q20 */
                if (!latch) s->integ[i] = 0;
                else {
                    /* latch: unity gain at the fundamental = 1 / (filter and interpolation loss),
                     * + PI on the fundamental's energy */
                    int32_t ff = lf + (((lf >> 9) * (lf >> 9)) >> 6);        /* 1 / (1 - l) ~ 1 + l + l^2 */
                    int32_t rel = 0;
                    if (follow) s->tgt[i] = fe < EFLOOR ? 0 : (fe < ECAP ? fe : ECAP);
                    else if (s->tgt[i]) {
                        int32_t t = s->tgt[i], sh = msb((uint32_t)t), d = t - fe;
                        rel = sh > 14 ? d >> (sh - 14) : d * (1 << (14 - sh));   /* ~ (t - fe) / t, Q14..15 */
                        if (rel < -32768) rel = -32768; else if (rel > 32767) rel = 32767;
                        s->integ[i] += (rel * Pi) >> 7;
                        if (s->integ[i] > 335544) s->integ[i] = 335544; else if (s->integ[i] < -335544) s->integ[i] = -335544;
                    }
                    s->dgl[i] = ff + s->integ[i] + ((rel * Pi * 13) >> 6);
                }
            }
            /* ---- pitch: chromatic string <-> plate mode, + detune, Q8 semitones above C2 ---- */
            int32_t pc = (root - 36 + i) << 8, pg = ((root - 36) << 8) + pm_gong[i];
            int32_t pitch = pc + (((pg - pc) * mg) >> 15) + ((sp * pm_spread[i]) >> 7);
            if (pitch < 0) pitch = 0;                      /* never below C2 (string 0 is not detuned) */
            /* period Q16 = 674.25 * 2^(-pitch/12/256): octaves Q16 = pitch * 65536 / 3072 */
            int32_t Pt = (int32_t)pow2n(D0_Q7, (pitch * 21845) >> 10, 6);
            if (!P) P = Pt;
            else {                       /* glide: half the way, at most P/128 (2.8 degrees of phase) per update,
                                          * so retuning / morphing never jumps the read point (no clicks) */
                int32_t d = (Pt - P) >> 1, lim = P >> 7;
                P += d > lim ? lim : (d < -lim ? -lim : d);
            }
            s->P[i] = P;
            /* read delay = period - b (the averaging filter's delay) */
            int32_t Dl = P - (bq << 1);
            if (Dl < (2 << 16)) Dl = 2 << 16;
            if (Dl > (DMAX << 16)) Dl = DMAX << 16;
            int32_t fr = (Dl & 0xffff) >> 5, ir = 2048 - fr;                 /* interpolation, Q11 */
            s->di[i] = (int16_t)(Dl >> 16); s->fr[i] = (int16_t)fr;
            /* DC servo window = the last floor(P) samples (a period: its sum is the loop's DC mode, the
             * fundamental cancels); follow the period incrementally (a few samples per update). Samples
             * known to be zero are skipped: older than the start (age) or just cleared by the gate (zl), so
             * filling the window at the start or after the gate costs no burst of reads */
            int32_t pw = P >> 16, ow = s->pw[i];
            if (pw > DMAX) pw = DMAX;
            if (ow < zl) ow = zl < pw ? zl : pw;                          /* (gate: ow was 0, S 0) */
            s->S[i] += rsum(ls, wp, ow, pw < age ? pw : age);
            s->S[i] -= rsum(ls, wp, pw, ow < age ? ow : age);
            s->pw[i] = (int16_t)pw;

            /* ---- loop gain - 1, Q24 ---- */
            int32_t env = s->env[i], dg;
            if (latch && (follow || s->tgt[i])) dg = s->dgl[i];             /* AGC (analysis above) */
            else {
                /* e^-x - 1, x = a P (decay per period), Q24 */
                int32_t x = (int32_t)(((a27 >> 3) * ((uint32_t)P >> 10)) >> 6);
                int32_t x15 = x >> 9, x2 = (x15 * x15) >> 15, x3 = (x2 * x15) >> 15, x4 = (x3 * x15) >> 15;
                dg = -x + ((x2 >> 1) - ((x3 * 5461) >> 15) + ((x4 * 1365) >> 15)) * 512;
            }
            if (env > SLIM) dg -= (env - SLIM) << 6;                          /* loop soft limit */
            else if (env < RELEASE && s->ein < 64 && !(latch && s->tgt[i]) && dg > -(1 << 21))
                dg = -(1 << 21);    /* quiet tail: -12.5 % per period, so rounding cannot hold it in a limit
                                     * cycle (dead band ~ 0.5 / (1 - G) = 4) above the gate */
            if (dg < -(1 << 23)) dg = -(1 << 23); else if (dg > (1 << 20)) dg = 1 << 20;
            /* loop = interpolation (1 - f, f) * filter g (1 - b, b): 3 taps, Q24 */
            int32_t G = (1 << 24) + dg;
            int32_t gb = (int32_t)((((uint32_t)G >> 8) * (uint32_t)bq) >> 7), ga = G - gb;
            gb >>= 5; ga >>= 5;
            s->t24[i][0] = (ga * ir) >> 6; s->t24[i][1] = (ga * fr + gb * ir) >> 6; s->t24[i][2] = (gb * fr) >> 6;
        }
        /* taps Q24 -> Q15 with sigma-delta across blocks, so the loop gain keeps ~Q24 precision */
        int16_t c[3];
        for (int k = 0; k < 3; k++) {
            int32_t v = s->t24[i][k] + s->q[i][k], cq = (v + 256) >> 9;
            if (cq > 32767) cq = 32767;
            s->q[i][k] = v - cq * 512;
            c[k] = (int16_t)cq;
        }
        /* DC servo: subtract kappa * (mean of the last period) from every sample of this block. Proportional on
         * the DC mode itself: first order, no low-frequency resonance (an integrating DC blocker inside a
         * near-lossless comb makes a high-Q ~30 Hz mode that input and clipping can pump) */
        int32_t S = s->S[i], pw = s->pw[i];
        /* window completed to the exact (fractional) period, so the fundamental cancels and cannot detune */
        int32_t Sx = S + ((ls[(wp - 1 - pw) & RM] * (int32_t)((s->P[i] >> 1) & 0x7fff)) >> 15);
        int32_t dcs = ((Sx >> 6) * s->kq[i]) * 4;                           /* kappa * mean, Q16 */
        dcs = dcs > (1 << 30) ? (1 << 30) : (dcs < -(1 << 30) ? -(1 << 30) : dcs);
        const int32_t k0 = 0x8000 - dcs;                     /* rounding - servo, into the fract32 sum (Q16) */

        /* ---- the string over the whole block; S keeps the sum of the last pw samples. The writes never wrap
         * (wp is a multiple of 64); the reads start di back, so if that is before index 0 the first -rb samples
         * read the ring's end, the rest its start (whose taps [-1], [-2] and od = rp - 1 hit the mirror) ---- */
        int16_t *op = ls + wp;
        if (pw) {
            int32_t rb = wp - s->di[i], dd = pw - s->di[i];      /* dd = 0 or 1 (P - b with b < 1/2) */
            const int16_t *rp = ls + rb, *ep = ex, *const ee = ex + OP1_FX_BLOCK;
            const int16_t c0 = c[0], c1 = c[1], c2 = c[2];
            int n = OP1_FX_BLOCK;
            if (rb < 0) { rp += RL; if (-rb < n) n = -rb; }
            for (;;) {                                           /* 1 or 2 segments */
                const int16_t *od = rp - dd;
                for (int j = 0; j < n; j++) {
                    int32_t z = ADDF(ADDF(ADDF(MULF(rp[0], c0), k0), MULF(rp[-1], c1)), MULF(rp[-2], c2)) >> 16;
                    rp++;
                    int32_t y = ADDQ(z, *ep++);
                    S += y - *od++;
                    *op++ = (int16_t)y;
                }
                if (ep == ee) break;
                rp = ls; n = (int)(ee - ep);
            }
        } else                       /* not started (P = 0): taps and servo are 0, the window is empty */
            for (int n = 0; n < OP1_FX_BLOCK; n++) { op[n] = ex[n]; S += ex[n]; }
        s->S[i] = S;
    }
    if (wp == RL - OP1_FX_BLOCK)                         /* the ring's end was just written: its mirror */
        for (int i = 0; i < NS; i++) { int16_t *ls = L + i * SL; ls[-1] = ls[RL - 1]; ls[-2] = ls[RL - 2]; }
    if (exo) s->czm = 0;
    s->wp = (wp + OP1_FX_BLOCK) & RM;
    s->age = age + OP1_FX_BLOCK < RL ? age + OP1_FX_BLOCK : RL;

    /* ---- output: strings (+ metallique soft clip) mixed with the dry signal ---- */
    int32_t dry = k3 < 16384 ? 32767 : (32767 - k3) * 2;
    int32_t wet = k3 > 16384 ? 32767 : k3 * 2;
    /* sum of the strings / 32 -> clip input: * 0.2136 * 32 * (1 .. 2 metallique drive), Q10 */
    int16_t kd = (int16_t)(6999 + ((mg * 6999) >> 15));
    int16_t mk = (int16_t)(32767 - ((mg * 10923) >> 15));                  /* make-up ~ 1 / drive */
    int32_t ow = s->ow;
    int16_t dryg = (int16_t)dry, wetg = (int16_t)wet;
    int32_t peak = 0, fbsum = 0;
    const int16_t *ob = L + wp;
    for (int n = 0; n < OP1_FX_BLOCK; n++, ob++) {
        /* resonance only: the 12 strings minus the excitation that each of them just got */
        int32_t sum = ob[0] + ob[SL] + ob[2 * SL] + ob[3 * SL] + ob[4 * SL] + ob[5 * SL]
                    + ob[6 * SL] + ob[7 * SL] + ob[8 * SL] + ob[9 * SL] + ob[10 * SL] + ob[11 * SL];
        int32_t v = (sum - ex[n] * NS) >> 5;                                /* |v| <= 24 x 32767 / 32 < 2^15 */
        int32_t d = MUL16(v, kd) >> 10;
        d = d > 32767 ? 32767 : (d < -32767 ? -32767 : d);
        int32_t c = softclip16((int16_t)d), r = c - d;                      /* gentle limiter / metallique clang */
        s->fb[n] = r;                                                       /* shaved-off part -> strings (bloom) */
        fbsum += r;
        v = MULQ15(c + (c >> 1), mk);                                       /* x - x^3/3 peaks at 2/3 */
        ow += v - (ow >> 10); v = SUBQ(v, ow >> 10);                         /* 7 Hz DC blocker (the clip is not
                                                                                symmetric for every waveform) */
        int32_t o = ADDQ(MULQ15(buf[n], dryg), MULQ15(v, wetg));
        buf[n] = (int16_t)o;
        if (o < 0) o = -o;
        if (o > peak) peak = o;
    }
    s->fbsum = fbsum; s->ow = ow;
    volatile int16_t *m = op1_fx_meter(pos);
    int32_t mv = (*m * 0xfe00) >> 16;
    *m = (int16_t)(peak > mv ? peak : mv);
    s_in[pos % 3] = (int16_t)(s->ein > 32767 ? 32767 : s->ein);
}

const op1_fx_t FX_palme = { palme_init, palme_render,
                            { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_LATCH } };

/* ======================= screen (GUI thread) =======================
 * palmediff.svg (gen_svg.py, geometry in palme_geom.h): the Palme - a palm-leaf soundboard whose 12 strings fan
 * out from the loudspeaker at its foot. Drawn here every frame:
 *   - the strings, each from its own delay line: one period of the string's waveform laid along the string under
 *     a standing-wave window, so it visibly vibrates with what it rings (amplitude ~ log level, brightness too);
 *   - the metallique gong behind the leaf (rings fade in with the morph);
 *   - the felt damper (etouffoir) across the strings: LATCH swings it open like two wings (red);
 *   - the loudspeaker cone breathing with the input;
 *   - root note (stroke font), PALME / METAL with the morph, and 4 dials: page-2 dials get a ring and their
 *     page-2 label, the red dial a ring while latched. */
#include "seq/seq_text.h"
#pragma GCC push_options
#pragma GCC optimize("Os")                             /* GUI code: size, not speed (code space is short) */

static void *pbars[4];
static int32_t damp_open[3];                                  /* damper wing angle per position, 0..1024 */
static const uint8_t C_WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, C_GREY[4] = { 0x6F, 0x71, 0x73, 0xFF },
                     C_DGREY[4] = { 0x3A, 0x3B, 0x3D, 0xFF }, C_RED[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     C_FELT[4] = { 0x8A, 0x8C, 0x8E, 0xFF }, C_BLACK[4] = { 0, 0, 0, 0xFF };

static inline int32_t psin(int32_t a) { return op1_sin(a & 8191); }      /* table[k] = sin(2 pi k/8192) */
#define pline sq_line                                 /* seq/seq_text.h: one raster line */
static void pcircle(void *r, int32_t cx, int32_t cy, int32_t rad, int32_t w, const uint8_t *c)   /* Q10 */
{
    op1_pt p = { cx + rad, cy };
    for (int a = 341; a <= 8192 + 170; a += 341) {                       /* 24 segments */
        op1_pt q = { cx + (rad * psin(a + 2048) >> 15), cy + (rad * psin(a) >> 15) };
        op1_raster_line(r, &p, &q, w, c);
        p = q;
    }
}
static void pmix(uint8_t *o, const uint8_t *a, const uint8_t *b, int32_t t)     /* a -> b, t 0..256 */
{
    for (int i = 0; i < 3; i++) o[i] = (uint8_t)(a[i] + (((b[i] - a[i]) * t) >> 8));
    o[3] = 0xFF;
}
static int32_t text_w(const char *t, int32_t s)
{
    int32_t w = 0;
    for (; *t; t++) w += (*t == '1' || *t == '.' ? 4 : 6) * s;
    return w - 2 * s;
}
static int32_t log2q4(int32_t v) { int32_t l = 0; while (v > 1 && l < 240) { v >>= 1; l += 16; } return l; }

static void palme_screen_init(void *scr)
{
    static const char *ids[4] = { "bar0", "bar1", "bar2", "bar3" };
    for (int i = 0; i < 4; i++) pbars[i] = op1_find_node(scr, ids[i]);
}

static void palme_screen_update(void *scr, int dt)
{
    int pos = op1_fx_screen_pos() % 3;
    int16_t wsh[8]; const int16_t *w = op1_knobs_shown(1, pos, op1_fx_knobs(pos), wsh);   /* page 2 as modulated */
    int16_t k[4];
    sq_fx_knobs_live(k);
    int32_t val[4];
    for (int i = 0; i < 4; i++) val[i] = (i < 3 && op1_fx_page[pos][i]) ? (w[5 + i] < 0 ? 0 : w[5 + i]) : k[i];
    for (int i = 0; i < 4; i++) if (pbars[i]) {                      /* dials: -135 .. +135 degrees */
        int32_t a = (val[i] * 6144 >> 15) - 3072;
        sq_node_rotate(pbars[i], OP1_Q10(pg_dial[i][0]), OP1_Q10(pg_dial[i][1]), psin(a), psin(a + 2048));
    }
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    palme_t *s = &st[pos];
    int latch = op1_fx_page[pos][3] != 0;
    int32_t mg = w[5] < 0 ? 0 : w[5];
    uint8_t c[4];

    /* ---- metallique: the gong behind the leaf ---- */
    if (mg > 600) {
        int32_t t = mg >> 7;                                             /* 0..255 */
        pmix(c, C_BLACK, C_GREY, t);
        for (int g = 0; g < 3; g++)
            pcircle(r, OP1_Q10(PG_GONG_X), OP1_Q10(PG_GONG_Y), OP1_Q10(22 + g * 15), 0x400 + g * 0x100, c);
        pmix(c, C_BLACK, C_WHITE, t);
        pcircle(r, OP1_Q10(PG_GONG_X), OP1_Q10(PG_GONG_Y), OP1_Q10(7), 0x600, c);   /* the boss */
    }

    /* ---- strings: their own delay lines, one period along the string, standing-wave window ---- */
    const int16_t *L = s->L;
    int32_t wp = s->wp;
    for (int i = 0; i < NS; i++) {
        int32_t env = s_env[pos][i], P = s->P[i] >> 16, lv = log2q4(env);  /* log2 level, Q4 */
        int32_t amp = lv > 96 ? (lv - 96) * 5 * 1024 / 144 : 0;          /* px Q10: 0 (-54 dB) .. 5 px */
        int32_t bri = lv > 64 ? (lv - 64) * 256 / 176 : 0;
        pmix(c, C_GREY, C_WHITE, bri > 256 ? 256 : bri);
        int32_t bx = pg_bot[i][0], by = pg_bot[i][1], tx = pg_top[i][0], ty = pg_top[i][1];
        int32_t nx = pg_nrm[i][0], ny = pg_nrm[i][1];
        op1_pt prev = { bx, by };
        for (int j = 1; j <= 12; j++) {
            int32_t px = bx + ((tx - bx) * j) / 12, py = by + ((ty - by) * j) / 12;
            if (j < 12 && L && P > 1 && env > 0 && amp) {
                int32_t v = L[i * SL + ((wp - 1 - (P * j) / 12) & RM)];    /* GUI thread: racy read, harmless */
                int32_t win = psin(j * 4096 / 12);                       /* sin(pi j / 12) */
                int32_t d = (int32_t)(((v * 1024) / env) * win >> 15);   /* -1024..1024 (shape) */
                d = d > 1024 ? 1024 : (d < -1024 ? -1024 : d);
                d = (d * amp) >> 10;                                     /* px, Q10 */
                px += (nx * d) >> 10; py += (ny * d) >> 10;
            }
            op1_pt q = { px, py };
            op1_raster_line(r, &prev, &q, 0x500, c);
            prev = q;
        }
    }

    /* ---- the etouffoir (felt damper): across the strings; LATCH swings it open like two wings ---- */
    int32_t *opn = &damp_open[pos];
    *opn += latch ? (*opn < 1024 ? 128 : 0) : (*opn > 0 ? -128 : 0);
    int32_t ang = (*opn * 700) >> 10;                                    /* up to ~31 degrees */
    pmix(c, C_FELT, C_RED, *opn >> 2);
    for (int side = 0; side < 2; side++) {
        int i0 = side ? NS - 1 : 0;                                      /* hinge at the outer string */
        int32_t hx = pg_dmp[i0][0], hy = pg_dmp[i0][1], sn = psin(side ? ang : -ang), cs = psin(ang + 2048);
        op1_pt prev = { hx, hy };
        for (int j = 1; j < NS / 2; j++) {
            int idx = side ? NS - 1 - j : j;
            int32_t dx = pg_dmp[idx][0] - hx, dy = pg_dmp[idx][1] - hy;
            op1_pt q = { hx + ((dx * cs - dy * sn) >> 15), hy + ((dx * sn + dy * cs) >> 15) };
            op1_raster_line(r, &prev, &q, *opn ? 0x800 : 0xC00, c);
            prev = q;
        }
    }

    /* ---- the loudspeaker cone breathes with the input ---- */
    int32_t il = log2q4(s_in[pos]) >> 4;                                 /* 0..15 */
    int32_t cr = OP1_Q10(2) + (il > 6 ? (il - 6) * 512 : 0);             /* 2 .. 6.5 px */
    pmix(c, C_GREY, C_WHITE, il > 6 ? (il - 6) * 28 : 0);
    pcircle(r, OP1_Q10(PG_CX), OP1_Q10(PG_CY), cr, 0x500, c);

    /* ---- left: root note, PALME / METAL ---- */
    static const char *const nn[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    int root = 36 + ((k[0] * 36) >> 15), pc = root % 12, oc = root / 12 - 1;
    char note[4] = { 0, 0, 0, 0 };
    int n = 0;
    for (const char *p = nn[pc]; *p; p++) note[n++] = *p;
    note[n] = (char)('0' + oc);
    sq_text(r, OP1_Q10(10), OP1_Q10(12), OP1_Q10(4), note, 0x800, C_WHITE);
    int32_t mt = mg >> 7;                                                /* 0..255 */
    pmix(c, C_WHITE, C_GREY, mt);
    sq_text(r, OP1_Q10(10), OP1_Q10(52), OP1_Q10(2), "PALME", 0x600, c);
    pmix(c, C_GREY, C_WHITE, mt);
    sq_text(r, OP1_Q10(10), OP1_Q10(74), OP1_Q10(2), "METAL", 0x600, c);
    int32_t mx = OP1_Q10(10) + (OP1_Q10(56) >> 5) * (mg >> 10);          /* morph slider between the words */
    pline(r, OP1_Q10(10), OP1_Q10(68), OP1_Q10(66), OP1_Q10(68), 0x200, C_DGREY);
    pline(r, mx - OP1_Q10(2), OP1_Q10(68), mx + OP1_Q10(2), OP1_Q10(68), 0x600, pg_dial_col[0]);
    if (latch) sq_text(r, OP1_Q10(10), OP1_Q10(134), OP1_Q10(2), "LATCH", 0x600, C_RED);

    /* ---- dials: labels (page aware), rings for page 2 / latch ---- */
    static const char *const lab1[4] = { "TUNE", "SUST", "EXCT", "MIX" }, *const lab2[3] = { "METAL", "DAMP", "SPRD" };
    for (int i = 0; i < 4; i++) {
        int p2 = i < 3 && op1_fx_page[pos][i];
        const char *t = p2 ? lab2[i] : lab1[i];
        int32_t x = OP1_Q10(pg_dial[i][0]) - text_w(t, OP1_Q10(1)) / 2, y = OP1_Q10(pg_dial[i][1] + PG_DIAL_R + 8);
        sq_text(r, x, y, OP1_Q10(1), t, 0x500, p2 ? pg_dial_col[i] : C_GREY);
        if (p2 || (i == 3 && latch))
            pcircle(r, OP1_Q10(pg_dial[i][0]), OP1_Q10(pg_dial[i][1]), OP1_Q10(PG_DIAL_R + 5), 0x500, pg_dial_col[i]);
    }
}

#pragma GCC pop_options
const op1_screen_t SCREEN_palme = { palme_screen_init, palme_screen_update };
