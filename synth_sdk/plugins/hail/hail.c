/* HAIL: granular texture FX, a fixed-point port of Mutable Instruments Beads (granular mode, digital media).
 * Original: https://github.com/pichenettes/eurorack/tree/master/beads (commit f4cfa769), dsp/processor.cc,
 * granular_processor.h, delay_granular_player.h, granular_player.h, delay_grain.h, grain.h, grain_envelope.h,
 * voice_stealing_envelope.h, trigger_generator.cc, limiter.h, fx/reverb.h. Copyright 2017 Emilie Gillet,
 * MIT licence (LICENSE.beads in this folder). Port, screen and OP-1 glue: Copyright (C) 2026 Hundekraft,
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 *
 *   knob 0 (blue)  TIME     where in the recording (1.49 s) new grains start: 0 = just behind the write head
 *   knob 1 (green) SIZE     grain length 7.8 ms .. 4 s; below 1/3 the grains play backwards (as Beads)
 *   knob 2 (white) SHAPE    grain envelope: square, decay, bell, reverse ramp (as Beads)
 *   knob 3 (red)   DENSITY  left half: regular grains, fastest at the far left (130 Hz); right half: random
 *                           grains, fastest at the far right; the middle (48..52 %) starts no grains (Beads with
 *                           SEED locked and no gate patched)
 * encoder press (page 2, params[5..7], saved with the patch):
 *   blue press  -> PITCH  -24 .. +24 semitones per grain (centre = 0, small dead band): one detent = one
 *                         semitone (snapped), Shift + turn = one cent (OP1_PRESS_PAGE_SEMI, the word law there)
 *   green press -> MIX    dry / wet (Beads' laws: 0.75 dry at 0, 1.5 wet at full)
 *   white press -> SPACE  0..50 %: reverb 0 .. 0.75; 50..100 %: reverb stays, feedback 0 .. 1
 *   red press   -> FREEZE (latch, not saved): nothing new is recorded, grains keep playing the buffer
 *
 * What the port leaves out or changes (OP-1: mono, 44.1 kHz, no FPU, 176 KB scratch, ~1/3 of the FX budget):
 *   - mono delay line, 16-bit, 65536 samples (Beads: 4 s stereo); no pan spread; grains read with linear
 *     instead of Hermite interpolation; up to BD_GRAINS grains (Beads: 32)
 *   - no delay mode (SIZE fully right stays granular), no wavetable synth mode, no CV / attenuverter randomness,
 *     no seed / gate input, no monophonic mode, no grain start at a fractional sample, no freeze-tail crossfade
 *   - feedback is taken from the previous 64-sample block (one block more loop delay), the 40 Hz SVF DC
 *     blocker is a one-pole high-pass (~27 Hz), the 0.999 one-pole after it is left out (it is ~identity)
 *   - Block-rate processing: the limiters run at block rate
 *     (peak per block, gain ramped; samples untouched while nothing is limited), the grain amplitude (envelope x
 *     voice stealing x gain x head fade) is computed once per block and ramped, each grain renders the block once
 *     (a trigger starts its grain with a pre-delay to its sample; a stolen grain's fade starts up to one block early),
 *     the reverb runs at quarter rate (11.025 kHz, Beads' delay lengths x0.1875 = x0.82 of Beads' times, loop
 *     low-pass set for the same cutoff), and the scheduler's pool is 12 grains, 8 while the reverb runs or rings out
 *     (Beads: 32; its 4 "safety" fades scale with the pool; a full pool gets a sqrt(16 / pool) level make-up)
 * Memory in this position's op1_fx_scratch: 128 KB recording + 32 KB reverb = 160 KB of 172 KB.

 * State per FX position (0 synth, 1 drum, 2 master; on the master the dispatcher feeds the mono sum). 


*/
#include "op1_plugin.h"
#include "hail_tables.h"

#define NBUF       65536
#define BMASK      (NBUF - 1)
#define MAXD       (NBUF - 256)          /* MAX_DELAY: the block is written ahead of the reads, plus room */
#define SAFETY     257                   /* head_crossing_safety_zone + 1 */
#define FZ         (256 << 12)           /* head fade zone, Q12 */
#ifndef BD_GRAINS
#define BD_GRAINS  16
#endif
/* Grain scheduling: the scheduler uses a pool of BD_POOL grains, BD_POOL_REVERB while the reverb runs or rings
 * out  */
#ifndef BD_POOL
#define BD_POOL        12
#define BD_POOL_REVERB 8
#endif
#define REV_N      8192                  /* reverb line, written twice (16 K words) */
#define INSTANT    220                   /* kInstantCrossfadeTime 5 ms */
#define PREEMPT    4410                  /* kPreemptiveCrossfadeTime 100 ms */
#define SIMPLE_TH  1102                  /* kSimpleSchedulerThreshold 25 ms */
#define MIN_ATTACK 22                    /* 0.5 ms */
#define MIN_REL    441                   /* 10 ms */
#define ONE30      (1u << 30)
#define D_LO       15729                 /* density 0.48 */
#define D_HI       17039                 /* density 0.52 */

typedef struct {
    uint32_t r;                          /* read position: absolute sample index, Q12 (wraps with the head) */
    int32_t speed;                       /* Q12, negative = backwards */
    int32_t remaining, gain;             /* samples left, Q15 */
    uint32_t ph, inc[3];                 /* GrainEnvelope: phase Q30, per stage */
    int32_t smooth;                      /* Q15 */
    int32_t vs, vsdec;                   /* VoiceStealingEnvelope, Q24 */
    uint8_t stage, rev;                  /* 0 attack, 1 sustain, 2 release, 3 done */
    uint8_t pre;                         /* samples of this block before the grain starts */
    int32_t amp;                         /* amplitude now, Q15 (ramped per chunk) */
    int32_t dur;                         /* screen: grain length in samples */
} grain_t;

typedef struct { int32_t pk, lp, g, g0; } lim_t;          /* Limiter peak states (Q23), gain now and before (Q15) */

typedef struct {
    int16_t *buf, *rev;
    uint32_t w;                          /* samples written (head) */
    int32_t dur;                         /* samples recorded, <= MAXD */
    uint8_t frozen, ok;
    grain_t g[BD_GRAINS];
    grain_t *prio[BD_GRAINS];
    int npool;                           /* grains the scheduler uses now (prio[0..npool)) */
    int32_t makeup;                      /* sqrt(BD_GRAINS / npool), Q12 */
    int32_t interval;
    /* trigger generator */
    uint32_t tph, tinc;                  /* phase Q31 */
    int32_t tbase, ratio;                /* Q31 base increment, Q12 random ratio */
    uint32_t rng;
    /* feedback, mix */
    int32_t prev[OP1_FX_BLOCK];
    int32_t dc;                          /* DC blocker state, Q23 */
    int32_t fbg, dry, wet;               /* smoothed: feedback gain, dry, wet (Q15) */
    lim_t lin, lpre, lout;
    /* reverb */
    int32_t rwp, rlp1, rlp2, rlfo[2], ly0[2], ly1[2], liir[2], rwo;
    int32_t rdry, rwet, rsend, rtime;
    int32_t ridle;
    /* screen */
    int32_t level;
    uint32_t trigs;                      /* grains started */
    struct { uint32_t at; int32_t dur; uint8_t rev; } trail[16];   /* where the last grains started (sample index) */
} bd_t;

static bd_t bd[3];

/* (a * b) >> 15 in 32-bit multiplies, exact for a >= 0 and |b| <= 65536 */
#define MULQ15(a, b) (((a) >> 15) * (b) + ((((a) & 32767) * (b)) >> 15))
static inline int32_t clip16(int32_t x) { return x < -32768 ? -32768 : (x > 32767 ? 32767 : x); }
static inline uint32_t rnd(bd_t *s) { return s->rng = s->rng * 1664525u + 1013904223u; }

/* 2^(e / 65536), Q16 (e Q16; result 0 below 2^-30) */
static uint32_t pow2_q16(int32_t e)
{
    const int32_t i = e >> 16;
    const uint32_t f = (uint32_t)e & 65535u, k = f >> 8, fr = f & 255u;
    const uint32_t m = 65536u + bd_exp2[k] + (((bd_exp2[k + 1] - bd_exp2[k]) * fr) >> 8);
    return i >= 0 ? m << i : (i > -31 ? m >> -i : 0);
}

/* Limiter over a block in place, block rate: the peak follower (Beads' SLOPE: up ~0.1 per sample, so within a block
 * it reaches the block's peak; down 2^-sh per sample, applied 64 times at once) is updated from the block's peak, and
 * the gain max / peak_lp ramps from the last block's gain to this one's. While nothing is limited (gain 1 then and
 * now) the samples pass untouched, as with the per-sample limiter; while limiting, attack is one block (1.45 ms). */
/* the same limiter in two halves, so the block peak can be taken in a loop that runs anyway: lim_update (block peak
 * pk of the samples clamped to +-2^18) -> 1 when a gain must be applied, lim_apply (clamp, ramp g0 -> g) */
static int lim_update(lim_t *l, int32_t pk, int32_t maxq15, int sh)
{
    const int32_t p8 = pk << 8, m8 = maxq15 << 8;
    l->pk = p8 > l->pk ? p8 : l->pk - (((l->pk - p8) >> sh) << 6);
    l->lp = l->pk > l->lp ? l->pk : l->lp - (((l->lp - l->pk) >> sh) << 6);
    int32_t g = 32768;
    if (l->lp > m8) {
        const int32_t pq = l->lp >> 8, k = pq >> 12, f = pq & 4095;
        const uint32_t r = bd_recip[k] - (((bd_recip[k] - bd_recip[k + 1]) * (uint32_t)f) >> 12);   /* k >= 7 */
        g = (int32_t)(((uint32_t)maxq15 * r) >> 15);
    }
    l->g0 = l->g ? l->g : 32768;
    l->g = g;
    return !(l->g0 == 32768 && g == 32768);
}
static void __attribute__((noinline)) lim_apply(const lim_t *l, int32_t *x)
{
    const int32_t dg = (l->g - l->g0) >> 6;
    int32_t c = l->g0;
    for (int i = 0; i < OP1_FX_BLOCK; i++) {
        int32_t v = x[i];
        if (v > (1 << 18)) v = 1 << 18;
        if (v < -(1 << 18)) v = -(1 << 18);
        c += dg;
        x[i] = ((v >> 2) * c) >> 13;
    }
}
static void __attribute__((noinline)) limit_block(lim_t *l, int32_t *x, int32_t maxq15, int sh)
{
    int32_t pk = 0;
    for (int i = 0; i < OP1_FX_BLOCK; i++) {
        int32_t v = x[i];
        if (v > (1 << 18)) v = 1 << 18;
        if (v < -(1 << 18)) v = -(1 << 18);
        x[i] = v;
        if (v < 0) v = -v;
        if (v > pk) pk = v;
    }
    if (lim_update(l, pk, maxq15, sh)) lim_apply(l, x);
}

/* ---------------- grains ---------------- */
static inline int grain_done(const grain_t *g) { return g->stage >= 3 || g->vs <= 0; }

static void fade_out(grain_t *g, int32_t d)                        /* Grain::FadeOut */
{
    if (d < 1) d = 1;
    if (g->remaining > d) {
        g->vsdec = (1 << 24) / d;
        if (d > (1 << 19)) d = 1 << 19;                                /* (vs * d) >> 24 in 32 bit, exact below 2^19 */
        g->remaining = ((g->vs >> 12) * d + (((g->vs & 4095) * d) >> 12)) >> 12;
    }
}

static void env_start(grain_t *g, int32_t dur, int32_t shape)      /* GrainEnvelope::Start */
{
    int32_t sus = 32768 - 3 * shape;
    if (sus < 0) sus = 0;
    int32_t sh = ((shape - 10923) * 3) >> 1;
    if (sh < 0) sh = 0;
    else if (sh < 16384) sh = (((sh * sh) >> 15) * sh) >> 13;
    const int32_t atk = sh, rel = ((32768 - atk) * (32768 - sus)) >> 15;
    int32_t ad = MULQ15(dur, atk), sd = MULQ15(dur, sus), rd = MULQ15(dur, rel);
    const int32_t amin = (MIN_ATTACK * (32768 + 3 * sus)) >> 15;
    if (ad < amin) ad = amin;
    if (sd < 32) sd = 32;
    if (rd < MIN_REL) rd = MIN_REL;
    g->inc[0] = ONE30 / (uint32_t)ad;
    g->inc[1] = ONE30 / (uint32_t)sd;
    g->inc[2] = ONE30 / (uint32_t)rd;
    g->smooth = atk < 16384 ? 2 * atk : 65536 - 2 * atk;
    g->stage = 0;
    g->ph = 0;
}

/* DelayGrain::Start; delay in samples behind the head hq (Q12) */
static void grain_start(bd_t *s, grain_t *g, int32_t delay, int32_t speed, int32_t dur, int32_t shape, int32_t gain,
                        uint32_t hq, int pre)
{
    const int32_t cons = speed - (s->frozen ? 0 : 4096), ac = cons < 0 ? -cons : cons;
    if (ac) {
        const int32_t lim = (MAXD << 12) / ac;                          /* < 2^31: 32-bit division */
        if (dur > lim) dur = lim;
    }
    const int32_t need = (dur >> 12) * ac + (((dur & 4095) * ac) >> 12);   /* (dur * ac) >> 12 exactly */
    if (cons > 0 && delay < need) delay = need;
    else if (cons < 0 && delay > MAXD - need) delay = MAXD - need;
    if (delay < 0) delay = 0;
    g->r = hq - ((uint32_t)delay << 12);
    g->speed = speed;
    g->gain = gain;
    g->rev = speed < 0;
    g->dur = dur;
    env_start(g, dur, shape);
    g->remaining = dur;
    g->vs = 1 << 24;
    g->vsdec = 0;
    g->amp = 0;
    g->pre = (uint8_t)pre;
}

/* envelope value after n more samples (GrainEnvelope::Render, stepped a chunk at a time), Q15 */
static inline int32_t env_step(grain_t *g, int n)
{
    if (g->stage >= 3) return 0;
    uint32_t ph = g->ph + g->inc[g->stage] * (uint32_t)n;            /* inc <= 2^26 (stages >= 16 samples): x CH 32 fits */
    if (ph >= ONE30) {
        ph = 0;
        if (++g->stage == 3) { g->ph = 0; return 0; }
    }
    g->ph = (uint32_t)ph;
    if (g->stage == 1) return 32768;
    int32_t t = (int32_t)(g->ph >> 15);
    if (g->stage == 2) t = 32768 - t;
    const int32_t t2 = (t * t) >> 15, base = g->stage == 0 ? t : (t2 * t2) >> 15;
    const int32_t ss = (t2 * (49152 - t)) >> 14;
    return base + (((ss - base) * g->smooth) >> 15);
}

/* render one grain over the block into out, from its start (g->pre) to the end; hq = head (Q12) after writing
 * sample 0, hstep 4096 or 0 (frozen). The amplitude (envelope x voice stealing x gain x head fade) is computed every
 * CH samples and ramped linearly in between; the head crossing is checked per chunk (a crossing grain ramps to 0 and
 * stops). Whole-sample steps (no pitch) read through a pointer while the chunk doesn't wrap the buffer. */
#define CH 64
#ifdef __bfin__
#define MUL16(a, b) __builtin_bfin_mult_fr1x16((a), (b))     /* (a * b) >> 15, one cycle */
#else
static inline int16_t MUL16(int32_t a, int32_t b) { const int32_t v = ((int16_t)a * (int16_t)b) >> 15; return (int16_t)(v > 32767 ? 32767 : v); }
#endif
static const int16_t ch_rcp[CH + 1] = { 0, 32767, 16384, 10923, 8192, 6554, 5461, 4681, 4096, 3641, 3277, 2979, 2731, 2521, 2341, 2185, 2048, 1928, 1820, 1725, 1638, 1560, 1489, 1425, 1365, 1311, 1260, 1214, 1170, 1130, 1092, 1057, 1024, 993, 964, 936, 910, 886, 862, 840, 819, 799, 780, 762, 745, 728, 712, 697, 683, 669, 655, 643, 630, 618, 607, 596, 585, 575, 565, 555, 546, 537, 529, 520, 512 };
static void __attribute__((noinline)) grain_run(const int16_t *buf, grain_t *g, int32_t *out, uint32_t hq, uint32_t hstep)
{
    const int32_t tail = MAXD << 12;
    uint32_t r = g->r;
    const int32_t speed = g->speed;
    int32_t amp = g->amp;
    int i = g->pre;
    g->pre = 0;
    hq += (uint32_t)i * hstep;
    while (i < OP1_FX_BLOCK) {
        int n = OP1_FX_BLOCK - i;
        if (n > CH) n = CH;
        hq += (uint32_t)n * hstep;
        const int32_t d = (int32_t)(hq - (r + (uint32_t)(n * speed)));
        int32_t target = 0, stop = 0;
        if (d < 0 || d >= tail) stop = 1;                            /* HEAD_CROSSING_POLICY_STOP */
        else {
            const int32_t e = env_step(g, n);
            g->vs -= g->vsdec * n;
            if (g->vs <= 0) { g->vs = 0; stop = 1; }
            else {
                target = (((e * (g->vs >> 9)) >> 15) * g->gain) >> 15;
                const int32_t dd = d >= (tail >> 1) ? tail - d : d;     /* fade near the head (both ends) */
                if (dd < FZ) {
                    const int32_t t = dd >> 5, t2 = (t * t) >> 15;
                    target = (target * ((t2 * (49152 - t)) >> 14)) >> 15;
                }
            }
            if (g->stage >= 3) stop = 1;
        }
        const int32_t damp = ((target - amp) * ch_rcp[n]) >> 15;
        int32_t *o = out + i;
        i += n;
        if (((uint32_t)speed & 4095u) == 0 && (r & 4095u) == 0) {      /* whole-sample steps: no interpolation */
            const int32_t st = speed >> 12, k0 = (int32_t)(((r + (uint32_t)speed) >> 12) & BMASK), k1 = k0 + (n - 1) * st;
            if (k1 >= 0 && k1 < NBUF) {
                const int16_t *p = buf + k0;
                for (int j = 0; j < n; j++, p += st) {
                    amp += damp;
                    *o++ += MUL16(*p, amp);
                }
                r += (uint32_t)(n * speed);
            } else {
                for (int j = 0; j < n; j++) {
                    r += (uint32_t)speed;
                    amp += damp;
                    *o++ += MUL16(buf[(r >> 12) & BMASK], amp);
                }
            }
        } else {
            for (int j = 0; j < n; j++) {
                r += (uint32_t)speed;
                const int16_t *q = buf + ((r >> 12) & BMASK);           /* buf[NBUF] mirrors buf[0] */
                const int32_t x0 = q[0], x = x0 + (((q[1] - x0) * (int32_t)(r & 4095u)) >> 12);
                amp += damp;
                *o++ += (x * amp) >> 15;
            }
        }
        amp = target;
        g->remaining -= n;
        if (stop) { g->stage = 3; break; }
    }
    g->r = r; g->amp = amp;
    if (grain_done(g)) { g->remaining = 0; g->amp = 0; }
}

/* GranularPlayer::SortGrainList (polyphonic) */
static void sort_grains(bd_t *s)
{
    grain_t **p = s->prio;
    int32_t ft[4];
    /* Beads fades 4 "safety" grains (of 32) at every start; with a smaller pool the same share: npool / 4 (16: 4,
     * 11: 2, 7: 1), the first entries of the fade list (the grain stolen next first) */
    const int ns = s->npool >> 2 < 1 ? 1 : s->npool >> 2;
    const int32_t iv = s->interval;
    if (iv < SIMPLE_TH) {
        grain_t *f = p[0];
        for (int i = 0; i < s->npool - 1; i++) p[i] = p[i + 1];
        p[s->npool - 1] = f;
        ft[0] = iv + 1; ft[1] = INSTANT; ft[2] = INSTANT; ft[3] = PREEMPT;
        if (iv > INSTANT) { const int32_t t = ft[0]; ft[0] = ft[2]; ft[2] = t; }
    } else {
        for (int i = 0; i < ns; i++) {                                 /* partial sort: the ns shortest first */
            int m = i;
            for (int j = i + 1; j < s->npool; j++) if (p[j]->remaining < p[m]->remaining) m = j;
            grain_t *t = p[i]; p[i] = p[m]; p[m] = t;
        }
        ft[0] = INSTANT; ft[1] = INSTANT; ft[2] = iv; ft[3] = PREEMPT;
        if (iv > PREEMPT) { const int32_t t = ft[2]; ft[2] = ft[3]; ft[3] = t; }
    }
    for (int i = 0; i < ns; i++) fade_out(p[i], ft[i]);
    s->interval = 0;
}

/* ---------------- reverb (Beads fx/reverb.h, Griesinger / Dattorro) ----------------
 * Runs at quarter rate (11.025 kHz: four input samples averaged in, the output interpolated back; the CPU budget
 * on the unit) in an 8192-word line written twice (rb[i] and rb[i + REV_N]), so reads need no index mask. Delay
 * lengths in samples: Beads' x0.1875 at 11.025 kHz (= x0.82 of Beads' times): 28 40 60 99 409 505 844 474 412 1184;
 * the loop low-pass coefficient is set for the same cutoff as Beads' 0.6 at twice the rate. B_i = base of line i
 * (each line takes length + 1), T_i = its tail (base + length - 1). */
enum { B0 = 0, B1 = 29, B2 = 70, B3 = 131, B4 = 231, B5 = 641, B6 = 1147, B7 = 1992, B8 = 2467, B9 = 2880,
       T0 = 27, T1 = 68, T2 = 129, T3 = 229, T4 = 639, T5 = 1145, T6 = 1990, T7 = 2465, T8 = 2878 };
#define RMUL(a, c) ((((a) >> 2) * (c)) >> 13)                       /* |a| < 2^18, c Q15 (up to 2^16) */
#define M16(a, c) (((int32_t)(int16_t)(a) * (int16_t)(c)) >> 15)    /* a: a buffer word */
#define RD(o) q[o]                                                  /* q = rb + wp */
#define WR(o, v) do { const int32_t i_ = (wp + (o)) & (REV_N - 1); rb[i_] = (v); rb[i_ + REV_N] = (v); } while (0)
static inline int32_t sat(int32_t x) { return x < -(1 << 17) ? -(1 << 17) : (x > (1 << 17) ? (1 << 17) : x); }
static inline int32_t rv_interp(const int16_t *q, int32_t base, int32_t off_q16)
{
    const int16_t *r = q + base + (off_q16 >> 16);
    const int32_t f = (off_q16 & 65535) >> 1, a = r[0], b = r[1];
    return a + (((b - a) * f) >> 15);
}

static void __attribute__((noinline)) reverb(bd_t *s, int32_t *io, int32_t amount)
{
    /* Processor: mix = r^2 0.9, time = 0.3 + 0.6 r^3, lp 0.6 (digital media) */
    const int32_t r2 = (amount * amount) >> 15, mix = (r2 * 29491) >> 15;
    const int32_t time = 9830 + ((((r2 * amount) >> 15) * 19661) >> 15);
    const int32_t ig = (6554 * (32768 - ((time * 10912) >> 15))) >> 15;      /* 0.2 (1 - 0.333 time) */
    const int32_t og = (13107 << 15) / ig;                                   /* 0.4 / ig, Q15 */
    const int32_t ssq = (mix * (65536 - mix)) >> 15, send = (((ssq * (65536 - ssq)) >> 15) * ig) >> 15;
    const int32_t dry = ((32768 - mix) * (32768 + mix)) >> 15;
    if (amount == 0 && s->rdry == 32768 && s->ridle > 32) return;            /* off and rung out: skip (dry = 1) */
    const int32_t kap = 20480, klp = 27525;                  /* 0.625; 0.84 = 1 - 0.4^2 (Beads' 0.6 at half the rate) */
    const int32_t dd = (dry - s->rdry) >> 4, dw = (og - s->rwet) >> 4, dsn = (send - s->rsend) >> 4;
    int32_t cd = s->rdry, cw = s->rwet, cs = s->rsend;
    int16_t *const rb = s->rev;
    int32_t wp = s->rwp, lp1 = s->rlp1, lp2 = s->rlp2, l0 = s->rlfo[0], l1 = s->rlfo[1], wo = s->rwo, wpk = 0;
    const int32_t rt = s->rtime;
    for (int t = 0; t < OP1_FX_BLOCK; t += 4) {
        cd += dd; cw += dw; cs += dsn;
        if (--wp < 0) wp += REV_N;
        if ((wp & 31) == 0) {                                               /* CosineOscillator::Next */
            for (int k = 0; k < 2; k++) {
                const int32_t y = s->ly0[k];
                s->ly0[k] = (int32_t)(((int64_t)s->liir[k] * y) >> 29) - s->ly1[k];
                s->ly1[k] = y;
                s->rlfo[k] = (y + (1 << 28)) >> 14;                             /* Q15, 0..1 */
            }
            l0 = s->rlfo[0]; l1 = s->rlfo[1];
        }
        const int16_t *const q = rb + wp;
        int32_t acc, prev, apout, wl;
        int16_t v;
        acc = rv_interp(q, B0, (15 << 13) + 30 * l0);                         /* ap1 smear: 1.875 + 15 lfo1 */
        v = (int16_t)clip16(acc); WR(B0 + 18, v);
        acc = RMUL(sat((io[t] + io[t + 1] + io[t + 2] + io[t + 3]) >> 1), cs);    /* l + r = 2 x: 4 samples' mean x 2 */
        prev = RD(T0); acc += M16(prev, kap); v = (int16_t)clip16(acc); WR(B0, v); acc = MUL16(v, -kap) + prev;
        prev = RD(T1); acc += M16(prev, kap); v = (int16_t)clip16(acc); WR(B1, v); acc = MUL16(v, -kap) + prev;
        prev = RD(T2); acc += M16(prev, kap); v = (int16_t)clip16(acc); WR(B2, v); acc = MUL16(v, -kap) + prev;
        prev = RD(T3); acc += M16(prev, kap); v = (int16_t)clip16(acc); WR(B3, v); acc = MUL16(v, -kap) + prev;
        apout = acc;
        acc = apout + MUL16(rv_interp(q, B9, (1173 << 16) + (75 * l1 >> 2)), rt);   /* del2: 1173 + 9.375 lfo2 */
        lp1 += RMUL(acc - lp1, klp); acc = lp1;
        prev = RD(T4); acc -= M16(prev, kap); v = (int16_t)clip16(acc); WR(B4, v); acc = MUL16(v, kap) + prev;
        prev = RD(T5); acc += M16(prev, kap); v = (int16_t)clip16(acc); WR(B5, v); acc = MUL16(v, -kap) + prev;
        v = (int16_t)clip16(acc); WR(B6, v);
        wl = acc;
        acc = apout + MUL16(rv_interp(q, B6, (835 << 16) + 15 * l0), rt);      /* del1: 835 + 7.5 lfo1 */
        lp2 += RMUL(acc - lp2, klp); acc = lp2;
        prev = RD(T7); acc += M16(prev, kap); v = (int16_t)clip16(acc); WR(B7, v); acc = MUL16(v, -kap) + prev;
        prev = RD(T8); acc -= M16(prev, kap); v = (int16_t)clip16(acc); WR(B8, v); acc = MUL16(v, kap) + prev;
        v = (int16_t)clip16(acc); WR(B9, v);
        const int32_t w = RMUL(sat((wl + acc) >> 1), cw >> 1) * 2;            /* mono: mean of both sides */
        const int32_t dwo = (w - wo) >> 2;                                     /* back to 44.1 kHz: linear */
        io[t] = RMUL(sat(io[t]), cd) + wo + dwo;
        io[t + 1] = RMUL(sat(io[t + 1]), cd) + wo + 2 * dwo;
        io[t + 2] = RMUL(sat(io[t + 2]), cd) + w - dwo;
        io[t + 3] = RMUL(sat(io[t + 3]), cd) + w;
        wo = w;
        const int32_t aw = w < 0 ? -w : w;
        if (aw > wpk) wpk = aw;
    }
    /* ring-out: with SPACE at 0 the reverb stops once its output has stayed under 8 (-72 dBFS) for 32 blocks */
    s->ridle = amount == 0 && wpk < 8 ? s->ridle + 1 : 0;
    s->rwp = wp; s->rlp1 = lp1; s->rlp2 = lp2; s->rwo = wo;
    s->rdry = dry; s->rwet = og; s->rsend = send; s->rtime = time;
}

/* ---------------- processor ---------------- */
#ifdef FX_SPREAD_INIT
#include "fx_spread.h"
static fx_spread_t bd_spread[3];
#endif
static void bd_init(int pos)
{
    pos %= 3;
    bd_t *s = &bd[pos];
    uint8_t *m = (uint8_t *)op1_fx_scratch(pos);
    for (unsigned i = 0; i < sizeof(*s); i++) ((uint8_t *)s)[i] = 0;
    if (!m) return;
    s->buf = (int16_t *)m;
    s->rev = (int16_t *)(m + NBUF * 2 + 4);
#ifdef FX_SPREAD_INIT                    /* grain and reverb buffers (contiguous) cleared per block, dry meanwhile */
    fx_spread_start(&bd_spread[pos], m, (NBUF * 2 + 4) / 4 + REV_N);
#else
    for (int i = 0; i <= NBUF; i++) s->buf[i] = 0;
    for (int i = 0; i < 2 * REV_N; i++) s->rev[i] = 0;
#endif
    for (int i = 0; i < BD_GRAINS; i++) { s->g[i].stage = 3; s->prio[i] = &s->g[i]; }
    s->npool = BD_GRAINS;
    s->makeup = 4096;
    for (int k = 0; k < 2; k++) {                                         /* LFOs 0.5 / 0.3 Hz at the reverb's 11.025 kHz (clouds port) */
        const int64_t f = ((int64_t)(k ? 3 : 5) * 32 << 29) / (10LL * 11025);
        const int64_t fr = (1LL << 27) - f;
        s->liir[k] = (int32_t)((16 * fr * ((1LL << 29) - 2 * fr)) >> 29);
        s->ly1[k] = s->liir[k] / 4;
        s->ly0[k] = 1 << 28;
        s->rlfo[k] = (s->ly1[k] + (1 << 28)) >> 14;
    }
    s->rdry = 32768; s->rtime = 9830;
    s->ridle = 1000;                                                    /* an empty reverb starts idle */
    s->lin.pk = s->lin.lp = s->lpre.pk = s->lpre.lp = s->lout.pk = s->lout.lp = 16384 << 8;
    s->ratio = 4096;
    s->rng = 0x2545F491u + (uint32_t)pos * 0x9E3779B9u;
    s->interval = 1;
    s->ok = 1;
}

static inline int32_t pword(const int16_t *p, int i) { return p[i] < 0 ? 0 : p[i]; }

static void bd_render(int pos, int16_t *io, const int16_t *p)
{
    pos %= 3;
    bd_t *s = &bd[pos];
    if (!s->ok) return;
#ifdef FX_SPREAD_INIT
    if (fx_spread_step(&bd_spread[pos])) return;
#endif
    const int32_t time = op1_fx_param_q15(p, 0), size = op1_fx_param_q15(p, 1), shape = op1_fx_param_q15(p, 2),
                  dens = op1_fx_param_q15(p, 3);
    const int32_t w5 = pword(p, 5), w6 = pword(p, 6), w7 = pword(p, 7);
    s->frozen = op1_fx_page[pos][3] != 0;

    /* per-block parameters */
    int32_t pv = w5 - 16384;                                          /* PITCH: +-24 st, dead band +-600 */
    pv = pv > 600 ? pv - 600 : (pv < -600 ? pv + 600 : 0);
    if (pv == 16384 - 600 - 1) pv = 16384 - 600;                     /* word 32767: +24 st exactly */
    const int32_t st8 = pv * (24 * 256) / (16384 - 600);              /* semitones * 256 */
    int32_t speed = (int32_t)(pow2_q16(st8 * 256 / 12) >> 4);        /* Q12 */
    const int reverse = size < 10923;
    if (reverse) speed = -speed;
    int32_t cs = reverse ? ((3 * size) * (65536 - 3 * size)) >> 15 : 32768 - ((3 * (size - 10923)) >> 1);
    cs = (cs * 33587) >> 15;                                          /* 1.025 */
    if (cs > 32768) cs = 32768;
    const int32_t px2 = (int32_t)pow2_q16(-(cs * 9) * 2);
    const int32_t gdur = (2756 * px2 + (px2 >> 2)) >> 10;             /* (176400 * x) >> 16 exactly: 4 s * 2^(-9 shortness) */
    const int32_t rv = w7 < 16384 ? (w7 * 3) >> 1 : 24576;           /* SPACE */
    const int32_t fb = w7 > 16384 ? (w7 - 16384) * 2 : 0;
    const int32_t fbt = ((fb * fb) >> 15) * 3;                        /* feedback^2 * 3 (16-bit resolution) */

    /* feedback + input limiter + write (the whole block, ahead of the reads) */
    const uint32_t w0 = s->w;
    int32_t fbg = s->fbg;
    const int32_t fstep = (fbt - fbg) >> 6;
    int32_t ipk = 0;
    if (fbg == 0 && fbt == 0)                                          /* no feedback (SPACE below 50 %) */
        for (int i = 0; i < OP1_FX_BLOCK; i++) {
            const int32_t v = io[i];
            s->prev[i] = v;
            const int32_t a = v < 0 ? -v : v;
            if (a > ipk) ipk = a;
        }
    else
        for (int i = 0; i < OP1_FX_BLOCK; i++) {
            fbg += fstep;
            const int32_t v = io[i] + ((clip16(s->prev[i]) * (fbg >> 1)) >> 14);   /* prev now holds the write input */
            s->prev[i] = v;
            const int32_t a = v < 0 ? -v : v;
            if (a > ipk) ipk = a;
        }
    if (lim_update(&s->lin, ipk, 31130, 15)) lim_apply(&s->lin, s->prev);
    if (!s->frozen) {                                                   /* two runs without index masks */
        const int k0 = (int)(w0 & BMASK), n1 = NBUF - k0 < OP1_FX_BLOCK ? NBUF - k0 : OP1_FX_BLOCK;
        int16_t *d = s->buf + k0;
        for (int i = 0; i < n1; i++) d[i] = (int16_t)clip16(s->prev[i]);
        for (int i = n1; i < OP1_FX_BLOCK; i++) s->buf[i - n1] = (int16_t)clip16(s->prev[i]);
        if (k0 == 0) s->buf[NBUF] = s->buf[0];                              /* mirror for the interpolation */
        else if (n1 < OP1_FX_BLOCK) s->buf[NBUF] = s->buf[0];
    }
    s->fbg = fbt;
    if (!s->frozen) {
        s->w = w0 + OP1_FX_BLOCK;
        s->dur = s->dur + OP1_FX_BLOCK > MAXD ? MAXD : s->dur + OP1_FX_BLOCK;
    }

    /* triggers (TriggerGenerator::RenderPulseTrain, gate always high) */
    int ntr = 0;
    uint8_t tr[OP1_FX_BLOCK];
    if (dens < D_LO || dens > D_HI) {
        const int rnd_mode = dens > D_HI;
        const uint32_t px = pow2_q16(rnd_mode ? -(32767 - dens) * 32 : -dens * 32);  /* 130.81 Hz / SR, Q31: (k * x) >> 16 */
        s->tbase = rnd_mode ? (int32_t)(48 * px + ((39154u * px) >> 16)) : (int32_t)(97 * px + ((12772u * px) >> 16));
        if (!rnd_mode) s->ratio = 4096;
        int32_t f = (s->tbase >> 12) * s->ratio + (((s->tbase & 4095) * s->ratio) >> 12);
        s->tinc = (uint32_t)(f < 2147 ? 2147 : (f > (1 << 29) ? (1 << 29) : f));
        for (int i = 0; i < OP1_FX_BLOCK; i++) {
            s->tph += s->tinc;
            if (s->tph >= 0x80000000u) {
                s->tph -= 0x80000000u;
                tr[ntr++] = (uint8_t)i;
                s->trigs++;
                if (rnd_mode) {
                    s->ratio = bd_rand_ratio[rnd(s) >> 24];
                    f = (s->tbase >> 12) * s->ratio + (((s->tbase & 4095) * s->ratio) >> 12);
                    s->tinc = (uint32_t)(f < 2147 ? 2147 : (f > (1 << 29) ? (1 << 29) : f));
                }
            }
        }
    }

    /* grains: this block's triggers start their grains first (each with a pre-delay to its sample), then every
     * playing grain renders the block once */
    int32_t wet[OP1_FX_BLOCK];
    for (int i = 0; i < OP1_FX_BLOCK; i++) wet[i] = 0;
    const uint32_t hstep = s->frozen ? 0 : 4096;
    const uint32_t hq0 = s->frozen ? w0 << 12 : (w0 + 1) << 12;           /* head after writing sample 0 */
    {   /* the pool for this block; grains left outside it play on until they end */
        const int np = (rv || s->ridle <= 32) ? BD_POOL_REVERB : BD_POOL;       /* the reverb runs (or rings out) */
        if (np != s->npool) {
            for (int i = 0; i < np; i++) s->prio[i] = &s->g[i];
            s->npool = np;
            s->makeup = np >= 16 ? 4096 : np >= 12 ? 4730 : np >= 9 ? 5461 : np >= 8 ? 5793 : 6214;   /* sqrt(16 / np): 12, 9, 8, 7 */
        }
    }
    /* make-up for the smaller pool, only while it is full (the cap is cutting grains): sqrt(BD_GRAINS / npool), Q12 */
    const int pool_small = s->npool < BD_GRAINS;
    int a = 0;
    for (int n = 0; n < ntr; n++) {                                      /* DelayGranularPlayer::Render, triggers */
        const int b = tr[n];
        s->interval += b - a;
        a = b;
        int active = 0;
        for (int k = 0; k < BD_GRAINS; k++) active += !grain_done(&s->g[k]);
        const int32_t mk = pool_small && active >= s->npool - (s->npool >> 2) - 1 ? s->makeup : 4096;   /* full: all but the fading ones */
        int32_t md = s->dur - 2 * SAFETY;
        if (md < 0) md = 0;
        const int32_t delay = MULQ15(md, time) + SAFETY;
        grain_start(s, s->prio[0], delay, speed, gdur, shape, (bd_inv_sqrt[active + 1] * mk) >> 12, hq0 + (uint32_t)b * hstep, b);
        {
            const grain_t *g = s->prio[0];
            const int t = (int)(s->trigs & 15u);
            s->trail[t].at = g->r >> 12; s->trail[t].dur = g->dur; s->trail[t].rev = g->rev;
        }
        sort_grains(s);
    }
    s->interval += OP1_FX_BLOCK - a;
    for (int k = 0; k < BD_GRAINS; k++)
        if (!grain_done(&s->g[k])) grain_run(s->buf, &s->g[k], wet, hq0, hstep);

    /* feedback source (DC-blocked wet signal, used by the next block) and dry / wet in one pass:
     * dry (1 - m)(1 + m) 0.75, wet m (2 - m) 1.5 */
    const int32_t dry = ((((32768 - w6) * (32768 + w6)) >> 15) * 24576) >> 15;
    const int32_t wt = ((((w6 * (65536 - w6)) >> 15) * 49152) >> 15);
    const int32_t ddr = (dry - s->dry) >> 6, dwt = (wt - s->wet) >> 6;
    int32_t cd = s->dry, cw = s->wet, out[OP1_FX_BLOCK], dc = s->dc, mpk = 0;
    for (int i = 0; i < OP1_FX_BLOCK; i++) {
        const int32_t w = wet[i];
        const int32_t x = w > 131071 ? 131071 : (w < -131071 ? -131071 : w);
        dc += (x * 256 - dc) >> 8;
        s->prev[i] = x - (dc >> 8);
        cd += ddr; cw += dwt;
        const int32_t xm = w > 65535 ? 65535 : (w < -65535 ? -65535 : w);
        const int32_t o = ((io[i] * cd) >> 15) + ((xm * (cw >> 1)) >> 14);
        out[i] = o;
        const int32_t a = o < 0 ? -o : o;
        if (a > mpk) mpk = a;
    }
    s->dc = dc;
    if (lim_update(&s->lpre, mpk > (1 << 18) ? 1 << 18 : mpk, 29491, 17)) lim_apply(&s->lpre, out);
    s->dry = dry; s->wet = wt;
    reverb(s, out, rv);
    limit_block(&s->lout, out, 29491, 18);
    int32_t pk = 0;
    for (int i = 0; i < OP1_FX_BLOCK; i++) {
        const int32_t y = clip16(out[i]);
        io[i] = (int16_t)y;
        const int32_t ab = y < 0 ? -y : y;
        if (ab > pk) pk = ab;
    }
    volatile int16_t *mt = op1_fx_meter(pos);
    const int32_t mv = (*mt * 0xfe00) >> 16;
    *mt = (int16_t)(pk > mv ? pk : mv);
    s->level = pk;
}

const op1_fx_t FX_hail = { bd_init, bd_render, { OP1_PRESS_PAGE_SEMI, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_LATCH } };

/* ======================= screen (GUI thread) =======================
 * hail.svg (round-5 art, from docs/design/engine-screens/night/beads in the full project; geometry in hail_geom.h): a bead
 * loom with the thread between two pulleys, and on the right the four readouts (TIME / SIZE / SHAPE / DENSITY)
 * with dim ring tracks and meter ticks. Drawn here:
 *   on the thread (write head left, oldest audio right) the trail: the last 16 grains as dim green pellets where
 *   they started, drifting right as that audio ages (they stop while frozen); every playing grain as a green bead at its read
 *   position, 4..14 px long with its size, thicker with its envelope, hollow when it plays backwards; above each
 *   bead a small white envelope in the SHAPE knob's form; a blue ring at TIME (where new grains start)
 *   in the tray loose red pellets, more as DENSITY moves away from the middle (scattered when random); the one
 *   that matches the newest grain lights up
 *   the readouts: a ring arc (270 degrees), face and pointer, and the lit meter ticks, each in its knob colour;
 *   while a knob is on page 2 its row shows the page-2 value (PITCH, MIX, SPACE) with a hollow face and its ticks
 *   FREEZE (red press): the thread drawn thick in ice blue */
#include "hail_geom.h"
#pragma GCC push_options
#pragma GCC optimize("Os")                             /* GUI code: size, not speed */
static const uint8_t B_BLUE[4] = { 0x69, 0x8E, 0xFF, 0xFF }, B_GREEN[4] = { 0x00, 0xED, 0x95, 0xFF },
                     B_WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, B_RED[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     B_ICE[4] = { 0xBF, 0xD8, 0xFF, 0xFF }, B_DIM[4] = { 0x00, 0x8E, 0x59, 0xFF };
static const uint8_t *const B_COL[4] = { B_BLUE, B_GREEN, B_WHITE, B_RED };
static const int32_t B_RING_Y[4] = { BD_RING0_Y, BD_RING1_Y, BD_RING2_Y, BD_RING3_Y };
static const int32_t B_TICK_Y[4][2] = { { BD_TICK0_Y0, BD_TICK0_Y1 }, { BD_TICK1_Y0, BD_TICK1_Y1 },
                                        { BD_TICK2_Y0, BD_TICK2_Y1 }, { BD_TICK3_Y0, BD_TICK3_Y1 } };

static inline int32_t bsin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }       /* a: 8192 = full turn */
static inline int32_t bcos(int32_t a) { return OP1_SINE_TABLE[(a + 2048) & 8191]; }
static void bl(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    const op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
/* point on the thread at u (Q15, 0 = write head .. 32768 = oldest), Q10; *dx, *dy = the segment's direction */
static void thread_at(int32_t u, int32_t *x, int32_t *y, int32_t *dx, int32_t *dy)
{
    if (u < 0) u = 0;
    if (u > 32767) u = 32767;
    const int32_t p = u * (BD_THREAD_N - 1), k = p >> 15, f = p & 32767;
    const int32_t *a = BD_THREAD[k], *b = BD_THREAD[k + 1];
    *x = a[0] + (((b[0] - a[0]) * f) >> 15);
    *y = a[1] + (((b[1] - a[1]) * f) >> 15);
    *dx = b[0] - a[0]; *dy = b[1] - a[1];
}
/* a capsule (stadium) outline centred at (x, y) along (ux, uy) (Q15 unit): length len, height h (Q10, ends
 * round), line width lw; solid: a centre stroke fills it */
static void capsule(void *r, int32_t x, int32_t y, int32_t ux, int32_t uy, int32_t len, int32_t h, int32_t lw,
                    const uint8_t *c, int solid, int st)
{
    const int32_t rr = h >> 1, hl = (len >> 1) - rr > 0 ? (len >> 1) - rr : 0;
    const int32_t ax = (ux * hl) >> 15, ay = (uy * hl) >> 15;
    int32_t px = 0, py = 0;
    for (int k = 0; k <= 12; k += st) {                                   /* two half circles, joined */
        const int side = k <= 6, j = side ? k : k - 6;
        /* angle around the end: from +normal over the end to -normal, 6 steps of 30 degrees */
        const int32_t ang = (side ? 6144 : 2048) + j * 683;                 /* 8192 = turn */
        const int32_t cs = OP1_SINE_TABLE[(ang + 2048) & 8191], sn = OP1_SINE_TABLE[ang & 8191];
        /* rotate (cs, sn) into the capsule's frame: x along u, y along the normal */
        const int32_t ox = (int32_t)(((int64_t)rr * ((cs * ux - sn * uy) >> 15)) >> 15);
        const int32_t oy = (int32_t)(((int64_t)rr * ((cs * uy + sn * ux) >> 15)) >> 15);
        const int32_t qx = x + (side ? ax : -ax) + ox, qy = y + (side ? ay : -ay) + oy;
        if (k) bl(r, px, py, qx, qy, lw, c);
        px = qx; py = qy;
    }
    {   /* close: back to the first point */
        const int32_t ang = 6144, cs = OP1_SINE_TABLE[(ang + 2048) & 8191], sn = OP1_SINE_TABLE[ang & 8191];
        const int32_t ox = (int32_t)(((int64_t)rr * ((cs * ux - sn * uy) >> 15)) >> 15);
        const int32_t oy = (int32_t)(((int64_t)rr * ((cs * uy + sn * ux) >> 15)) >> 15);
        bl(r, px, py, x + ax + ox, y + ay + oy, lw, c);
    }
    if (solid) bl(r, x - ax, y - ay, x + ax, y + ay, h - lw > 0x200 ? h - lw : 0x200, c);
}
/* GrainEnvelope's shape for SHAPE = sh (Q15) at t (Q15 of the grain), Q15; the same law as env_start */
static int32_t env_shape(int32_t sh, int32_t t)
{
    int32_t sus = 32768 - 3 * sh;
    if (sus < 0) sus = 0;
    int32_t a = ((sh - 10923) * 3) >> 1;
    if (a < 0) a = 0;
    else if (a < 16384) a = (((a * a) >> 15) * a) >> 13;
    const int32_t rel = ((32768 - a) * (32768 - sus)) >> 15, ae = a > 655 ? a : 655, re = rel > 655 ? rel : 655;
    const int32_t sm = a < 16384 ? 2 * a : 65536 - 2 * a;
    int32_t v, u;
    if (t < ae) { u = (int32_t)(((int64_t)t << 15) / ae); v = u; }
    else if (t < ae + sus) return 32767;
    else { u = 32768 - (int32_t)(((int64_t)(t - ae - sus) << 15) / re); if (u < 0) u = 0; v = (((u * u) >> 15) * ((u * u) >> 15)) >> 15; }
    if (t < ae || t >= ae + sus) {
        const int32_t u2 = (u * u) >> 15, ss = (u2 * (49152 - u)) >> 14;
        v += ((ss - v) * sm) >> 15;
    }
    return v;
}

/* The FROZEN label under DENSITY. The static loom art keeps the primary labels; the page-2 rows show
 * meter ticks, like page 1. 5x7 strokes avoid extra firmware text allocations. */
static const uint8_t b_glyphs[][7] = {
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14},
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
    {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
    {0,4,4,31,4,4,0},{0,0,0,31,0,0,0},{0,0,0,0,0,12,12},
    {17,2,4,8,16,17,0}
};
static void btext(void *r, int x, int y, const char *text, const uint8_t *c)
{
    for (; *text; text++, x += 6) {
        int g = *text >= '0' && *text <= '9' ? *text-'0' :
                *text >= 'A' && *text <= 'Z' ? *text-'A'+10 :
                *text == '+' ? 36 : *text == '-' ? 37 : *text == '.' ? 38 : *text == '%' ? 39 : -1;
        if (g < 0) continue;
        for (int row=0; row<7; row++) for (int col=0; col<5;) {
            if (!(b_glyphs[g][row] & (16 >> col))) { col++; continue; }
            int first=col++;
            while (col<5 && (b_glyphs[g][row] & (16 >> col))) col++;
            bl(r,(x+first)*1024-256,(y+row)*1024,(x+col-1)*1024+256,(y+row)*1024,1280,c);
        }
    }
}
static void bfrozen(void *r)
{
    btext(r,274,B_RING_Y[3]/1024+6,"FROZEN",B_RED);
}

static void *b_label_scr, *b_primary[4], *b_secondary[3], *b_ticks[4];
static uint8_t b_labels_ready;
static void bd_screen_init(void *scr) { (void)scr; b_labels_ready=0; }
static void b_labels(void *scr, int pos)
{
    if (!b_labels_ready || b_label_scr!=scr) {
        char name[12];
        for (int i=0;i<4;i++) {
            const char *p="primary";int n=0;while (*p)name[n++]=*p++;name[n++]=(char)('0'+i);name[n]=0;
            b_primary[i]=op1_find_node(scr,name);
            p="ticks";n=0;while (*p)name[n++]=*p++;name[n++]=(char)('0'+i);name[n]=0;
            b_ticks[i]=op1_find_node(scr,name);
            if (i<3) { p="secondary";n=0;while (*p)name[n++]=*p++;name[n++]=(char)('0'+i);name[n]=0;b_secondary[i]=op1_find_node(scr,name); }
        }
        b_label_scr=scr;b_labels_ready=1;
    }
    for (int i=0;i<4;i++) {
        int pg=i<3 && op1_fx_page[pos][i];
        op1_node_set_visible(b_primary[i],!pg);
        if(i<3)op1_node_set_visible(b_secondary[i],pg);
        op1_node_set_visible(b_ticks[i],!(i==3 && op1_fx_page[pos][3]));
    }
}

static void bd_screen_update(void *scr, int dt)
{
    const int pos = op1_fx_screen_pos() % 3;
    b_labels(scr,pos);
    op1_screen_draw_cached(scr, dt, b_primary[0]);
    void *r = op1_raster();
    const bd_t *s = &bd[pos];
    int16_t kn[4];
    op1_fx_knobs_live(kn);
    int32_t k[4];
    for (int i = 0; i < 4; i++) k[i] = kn[i] < 0 ? 0 : kn[i];
    const int16_t *wv = op1_fx_knobs(pos);
    const int frozen = op1_fx_page[pos][3] != 0;
    int32_t x, y, dx, dy;

    /* FREEZE: the thread lit */
    if (frozen)
        for (int i = 0; i + 1 < BD_THREAD_N; i++)
            bl(r, BD_THREAD[i][0], BD_THREAD[i][1], BD_THREAD[i + 1][0], BD_THREAD[i + 1][1], 0xC00, B_ICE);

    /* the tray: loose pellets, more away from the middle of DENSITY; scattered when random */
    {
        int32_t dd = k[3] - 16384;
        const int rnd_side = dd > 655;
        if (dd < 0) dd = -dd;
        dd = dd > 655 ? dd - 655 : 0;
        int n = (dd * (BD_PILE_N + 1)) / (16384 - 655);
        if (n > BD_PILE_N) n = BD_PILE_N;
        const int lit = n ? (int)(s->trigs % (uint32_t)n) : -1;
        for (int i = 0; i < n; i++) {
            int32_t ang = BD_PILE[i][2];
            if (rnd_side) ang += (int32_t)(((s->trigs + (uint32_t)i * 7u) * 2654435761u) >> 27) - 16;   /* jiggle */
            const int32_t a = ang * 8192 / 360;
            capsule(r, BD_PILE[i][0], BD_PILE[i][1], bcos(a), bsin(a), 0x2800, 0x1400, 0x500, i == lit ? B_WHITE : B_RED, 0, 1);
        }
    }

    /* TIME: where new grains start, over the recorded part */
    {
        int32_t md = s->dur - 2 * SAFETY;
        if (md < 0) md = 0;
        const int32_t d = (int32_t)(((int64_t)k[0] * md) >> 15) + SAFETY;
        thread_at((int32_t)(((int64_t)d << 15) / MAXD), &x, &y, &dx, &dy);
        int32_t px = x + 0x1000, py = y;
        for (int a = 512; a <= 8192; a += 512) {
            const int32_t qx = x + ((bcos(a) * 0x1000) >> 15), qy = y + ((bsin(a) * 0x1000) >> 15);
            bl(r, px, py, qx, qy, 0x600, B_BLUE);
            px = qx; py = qy;
        }
    }

    /* the trail: each grain leaves a bead where it started; it drifts right as that audio ages */
    if (s->ok) {
        const int n = s->trigs < 16 ? (int)s->trigs : 16;
        for (int i = 0; i < n; i++) {
            const int32_t d = (int32_t)(s->w - s->trail[i].at);
            if (d < 0 || d >= MAXD) continue;
            thread_at((int32_t)(((int64_t)d << 15) / MAXD), &x, &y, &dx, &dy);
            const int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            int32_t m = ax > ay ? ax + (ay * 3 >> 3) : ay + (ax * 3 >> 3);
            if (m < 1) m = 1;
            const int32_t len = 0x1000 + (int32_t)(((int64_t)(s->trail[i].dur > 44100 ? 44100 : s->trail[i].dur) * 0x2800) / 44100);
            capsule(r, x, y, (int32_t)(((int64_t)dx << 15) / m), (int32_t)(((int64_t)dy << 15) / m), len, 0x1400, 0x400,
                    B_DIM, 0, 2);
        }
    }

    /* grains: pellets on the thread, an envelope above each */
    if (s->ok) {
        const uint32_t hq = s->w << 12;
        for (int i = 0; i < BD_GRAINS; i++) {
            const grain_t *g = &s->g[i];
            if (g->stage >= 3 || g->vs <= 0) continue;
            int32_t d = (int32_t)(hq - g->r) >> 12;
            if (d < 0) d = 0;
            thread_at((int32_t)(((int64_t)d << 15) / MAXD), &x, &y, &dx, &dy);
            const int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            int32_t m = ax > ay ? ax + (ay * 3 >> 3) : ay + (ax * 3 >> 3);           /* |dir|, alpha max beta min */
            if (m < 1) m = 1;
            const int32_t ux = (int32_t)(((int64_t)dx << 15) / m), uy = (int32_t)(((int64_t)dy << 15) / m);
            const int32_t len = 0x1000 + (int32_t)(((int64_t)(g->dur > 44100 ? 44100 : g->dur) * 0x2800) / 44100);   /* 4 .. 14 px */
            const int32_t amp = g->amp > 24576 ? 24576 : (g->amp < 0 ? 0 : g->amp);
            const int32_t w = 0xC00 + ((amp * 0x800) / 24576);                         /* 3 .. 5 px */
            capsule(r, x, y, ux, uy, len + 0x400, w + 0x400, 0x500, B_GREEN, !g->rev, 1);
            /* the envelope, 10 px wide, 6 px high, 10 px above */
            const int32_t ex = x - 0x1400, ey = y - 0x2800;
            int32_t px = ex, py = ey + 0x1800 - (int32_t)((env_shape(k[2], 0) * 0x1800) >> 15);
            for (int t = 1; t <= 8; t++) {
                const int32_t qx = ex + t * 0x500;
                const int32_t qy = ey + 0x1800 - (int32_t)((env_shape(k[2], t * 4096 > 32767 ? 32767 : t * 4096) * 0x1800) >> 15);
                bl(r, px, py, qx, qy, 0x400, B_WHITE);
                px = qx; py = qy;
            }
        }
    }

    /* the readouts: ring arc + face + pointer, meter ticks */
    for (int i = 0; i < 4; i++) {
        const int pg = i < 3 && op1_fx_page[pos][i];
        const int32_t v = pg ? (wv[5 + i] < 0 ? 0 : wv[5 + i]) : k[i];
        const uint8_t *c = B_COL[i];
        const int32_t cx = BD_RING_X, cy = B_RING_Y[i], R = BD_RING_R;
        const int32_t a0 = 3072, span = (v * 6144) >> 15;                          /* 135 degrees, 270 for 100 % */
        int32_t px = cx + ((bcos(a0) * R) >> 15), py = cy + ((bsin(a0) * R) >> 15);
        for (int a = 256; a <= span; a += 256) {
            const int32_t qx = cx + ((bcos(a0 + a) * R) >> 15), qy = cy + ((bsin(a0 + a) * R) >> 15);
            bl(r, px, py, qx, qy, 0x800, c);
            px = qx; py = qy;
        }
        const int32_t fr = (R * 737) >> 10;                                          /* face 0.72 R */
        px = cx + fr; py = cy;
        for (int a = 512; a <= 8192; a += 512) {
            const int32_t qx = cx + ((bcos(a) * fr) >> 15), qy = cy + ((bsin(a) * fr) >> 15);
            bl(r, px, py, qx, qy, pg ? 0x400 : 0x600, c);
            px = qx; py = qy;
        }
        const int32_t pr = (R * 614) >> 10;                                          /* pointer 0.6 R */
        bl(r, cx, cy, cx + ((bcos(a0 + span) * pr) >> 15), cy + ((bsin(a0 + span) * pr) >> 15), 0x600, c);
        const int lit = (v * 12 + 16383) >> 15;
        for (int t = 0; !(i==3 && frozen) && t < lit && t < 12; t++)
            bl(r, BD_TICK_X0 + t * BD_TICK_DX, B_TICK_Y[i][0], BD_TICK_X0 + t * BD_TICK_DX, B_TICK_Y[i][1], 0x600, c);
        if (i==3 && frozen) bfrozen(r);
    }
}
#pragma GCC pop_options

const op1_screen_t SCREEN_hail = { bd_screen_init, bd_screen_update };
