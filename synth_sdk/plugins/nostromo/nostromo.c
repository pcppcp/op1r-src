/* "nostromo" - a reverb on Dattorro's plate tank (room, plate, hall), the Chase Bliss CXM 1978's controls, fixed point.
 * Topology, coefficients and output taps: J. Dattorro, "Effect Design, Part 1" (1997), as the Synthstrom
 * Deluge's "Digital" model (src/deluge/dsp/reverb/digital.hpp, (c) 2024 Katherine Whitlock, GPL-3.0), the delay
 * lengths rescaled from 29.761 kHz to 44.1 kHz; fixed-point helpers from plugins/reverb (Mutable Instruments
 * Elements, (c) 2014 Emilie Gillet, MIT). Mono: the 7 + 7 output taps are summed.
 *   knob 0 (blue)  : mid decay     RT60 above the 700 Hz crossover, 0.34 / 0.55 / 0.8 .. 10 s (room / plate / hall)
 *                                  press: bass, RT60 below it, its own time over the same range (224, CXM 1978)
 *   knob 1 (green) : treble decay  loop damping, 500 Hz .. 16 kHz          press: clock HIFI / STD / LOFI, a step
 *                                  every 2 detents (2048), round and round
 *   knob 2 (white) : pre-delay     0 .. 500 ms (LOFI: the clock)           press: program 1 .. 27 (a step every
 *                                  1.2 detents: the main page's raw word has too few for 27)
 *   knob 3 (red)   : mix           dry .. wet
 * program = 9 type + 3 diffusion + tank mod + 1. Clock HIFI / STD / LOFI runs the tank at 44.1 kHz,
 * 22.05 kHz or a LOFI clock; type room / plate / hall scales every delay x0.6 / 1 / 1.5 (a change of either fades the
 * wet out, then clears the tank); diffusion (after the CXM 1978) low: no input allpasses (a hall: a sparse cloud of 8
 * soft taps around the pre-delay), med: Dattorro's, high: stronger, the pre-delay regenerated through them (fixed 0.5, a
 * block late); tank mod low / med / high the LFO rate and depth (low: a slow random wander); a room adds 7 early
 * reflections. Each half of the tank carries its
 * decay in one two-band stage, g_mid + (g_bass - g_mid) * two-pole low-pass at the crossover, the gains from the
 * RT60s once per block. Every setting ramps (~45 ms); the pre-delay crossfades to a new time over 1024 samples.
 * STD runs the tank between half-band filters, LOFI samples and holds at its clock; both add the Lexicon 224's
 * gain-stepped 12-bit converters to the wet path.
 * Memory: a 64 Ki-sample int16 ring (tank) and a 500 ms int16 line (pre-delay) in this FX position's scratch
 * buffer, cleared 2048 words per block with the wet muted.
 * Copyright (C) 2026 Lewis Clayton. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef OP1_HOST
#pragma GCC optimize ("no-tree-loop-distribute-patterns")          /* no memset calls */
#endif
#include "op1_plugin.h"
#include "nostromo_tables.h"

#define NT_MASK  (NT_RING - 1)
#define NT_CLEAR 2048                                             /* words cleared per block */
#define NT_FADE  1024                                             /* pre-delay crossfade, samples */
#define NT_OUT   256                                              /* wet fade-out before a type change, samples */
#define NT_WET_K 22209                                            /* wet = taps * K / 8192: plugins/reverb's level */
#define NT_GLIDE 64                                               /* LFO steps of a glide to or from tank mod LOW */
#define NT_KNEE  28672                                            /* the output bends above -1.2 dBFS, slopes 1/4, 1/16 */
typedef char nt_fits[(NT_RING + NT_PD) * 2 <= OP1_FX_SCRATCH_BYTES ? 1 : -1];
typedef char nt_wet_fits[65535LL * NT_WET_K <= 0x7FFFFFFF ? 1 : -1];   /* clamped taps * K in int32 */
typedef char nt_lofi_fits[NT_LOFI_PD + 1024 < NT_PD ? 1 : -1];   /* LOFI's pre-delay, reflections, in the line */

enum { NT_HIFI, NT_STD, NT_LOFI };
typedef struct {                                                  /* per-block parameters */
    int32_t clock, type, diff, mod;                               /* program: 0..2 each */
    int32_t lay, rate;                                            /* tank layout: type + 3 clock; tables: 0 44.1, 1 22.05 kHz */
    int32_t rt, rtb;                                              /* RT60 mid, bass, ms */
    int32_t bt, bx;                                               /* one-poles: treble decay, crossover (Q15) */
    int32_t mix, pd, inc, wk;                                     /* mix (Q15), pre-delay (tank samples), LOFI clock (Q16),
                                                                     the wet's level (NT_WET_K less for a long decay) */
    int32_t gm[2], gb[2], dd2;                                    /* per half: g_mid, g_bass - g_mid; decay diffusion 2 */
} nt_par;

static inline int32_t nt_c17(int32_t x) { return x < -65535 ? -65535 : (x > 65535 ? 65535 : x); }
static inline int16_t nt_s16(int32_t x) { return (int16_t)(x < -32768 ? -32768 : (x > 32767 ? 32767 : x)); }   /* MIN, MAX */
static inline int32_t nt_knee(int32_t x)                          /* above -1.2 dBFS a bend, not a clip: slopes 1/4, then */
{                                                                 /* 1/16, full scale at 2.1x */
    int32_t a = x < 0 ? -x : x;
    if (a <= NT_KNEE) return x;
    int32_t d = a - NT_KNEE;
    a = d < 8192 ? NT_KNEE + (d >> 2) : 30720 + ((d - 8192) >> 4);
    return nt_s16(x < 0 ? -a : a);
}
static inline int32_t nt_tz(int32_t x, int n)                    /* x / 2^n towards zero, n 1..31, without a branch */
{
    return (x + (int32_t)((uint32_t)(x >> 31) >> (32 - n))) >> n;
}
/* |b| <= 32768: fits 32 bit. Truncates towards zero, so tails die out (as plugins/reverb) */
static inline int32_t nt_m15(int32_t a, int32_t b) { return nt_tz(nt_c17(a) * b, 15); }
static inline int32_t nt_m15w(int32_t a, int32_t b)               /* nt_m15 for |a| < 2^30, b >= 0 */
{
    uint32_t u = a < 0 ? -(uint32_t)a : (uint32_t)a;
    int32_t r = (int32_t)((u >> 15) * (uint32_t)b + ((u & 32767) * (uint32_t)b >> 15));
    return a < 0 ? -r : r;
}
static inline int32_t nt_shr(int32_t x, int n) { return nt_tz(x, n); }   /* towards zero */
static inline int32_t nt_gstep(int32_t pk)                        /* 12-bit converter: the 6 dB gain steps that fit */
{
    int32_t s = 0;
    while (s < 4 && pk << (s + 1) < 32768) s++;
    return s;
}
static inline int32_t nt_q12(int32_t v, int32_t s)               /* 12 bits at gain step s: a grid of 16 >> s, to 0 */
{
    int32_t m = ~((16 >> s) - 1);
    return v >= 0 ? v & m : -(-v & m);
}
static inline int32_t nt_lut(const int32_t *t, int32_t q)         /* knob 0..32767 -> table, interpolated */
{
    int32_t i = q >> 8;
    return t[i] + ((t[i + 1] - t[i]) * (q & 255) >> 8);
}
static inline int32_t nt_clock(int32_t w) { return (w >> 11) % 3; }   /* the clock word: HIFI, STD, LOFI, HIFI .. */
static inline int32_t nt_pd(int32_t v) { return (v * v >> 15) * NT_PD >> 15; }   /* knob -> samples, 500 ms v^2 */

static int32_t nt_gain(int32_t k16, int32_t rt)                   /* loop gain for an RT60 of rt ms: 2^-(k / rt) */
{
    int32_t x = k16 / rt, n = x >> 16;
    if (n >= 15) return 0;
    int32_t g = nt_exp2[(x >> 8) & 255] >> n;
    return g > NT_GMAX ? NT_GMAX : g;
}

static void nt_params(const int16_t *p, nt_par *o)
{
    int32_t w5 = p[5] < 0 ? 0 : p[5], w6 = p[6] < 0 ? 0 : p[6], w7 = p[7] < 0 ? 0 : p[7];
    int32_t prog = w7 * 27 >> 15, v2 = op1_fx_param_q15(p, 2);
    o->clock = nt_clock(w6); o->type = prog / 9; o->diff = prog / 3 % 3; o->mod = prog % 3;
    o->lay = o->type + 3 * o->clock;
    const int r = o->rate = o->clock != NT_HIFI;
    o->rt = nt_lut(nt_rt[o->type], op1_fx_param_q15(p, 0));
    o->wk = NT_WET_K * nt_lut(nt_wg[o->type], op1_fx_param_q15(p, 0)) >> 15;
    o->rtb = nt_lut(nt_rt[o->type], w5);                         /* bass: its own RT60, the mid's range (224, CXM 1978) */
    o->bt = nt_lut(nt_treb_b[r], op1_fx_param_q15(p, 1));
    o->bx = nt_xo_b[r];
    o->pd = o->clock == NT_LOFI ? NT_LOFI_PD : nt_pd(v2) >> r;    /* STD: half the samples at half the clock */
    o->inc = nt_lut(nt_lofi_inc, v2);
    o->mix = op1_fx_param_q15(p, 3);
    for (int h = 0; h < 2; h++) {
        o->gm[h] = nt_gain(nt_k16[r][o->type][h], o->rt);
        o->gb[h] = nt_gain(nt_k16[r][o->type][h], o->rtb) - o->gm[h];
    }
    int32_t d = nt_gain(nt_k16[r][o->type][0] >> 1, o->rt) + 4915;   /* decay diffusion 2: sqrt(g) + 0.15 .. */
    o->dd2 = d < 8192 ? 8192 : (d > 16384 ? 16384 : d);           /* .. within 0.25 .. 0.5 */
}

typedef struct {
    int16_t *ring, *pd;                                           /* scratch: tank ring, pre-delay (after the ring) */
    int32_t wp, pw;                                               /* ring write pointer (counts down), pre-delay index */
    int32_t clear, cend;                                          /* words left to clear, the end of that area */
    int32_t out, ein;                                             /* wet fade-out before a type change; reflections fading
                                                                     in after its clear (HIFI keeps the pre-delay): samples left */
    int32_t lay, fresh;                                           /* tank layout in use (-1: none), first block */
    int32_t lp[2], xo[2], xo2[2], ef[2][4], dc;                   /* damping, crossover (2 poles), their and the gains'
                                                                     carried fractions, low-cut (Q8) states */
    uint32_t ph[2]; int32_t lfo[2];                               /* LFO phases, values (Q15 0..1) */
    uint32_t ws[2]; int32_t wt[2], wn[2], wv[2], wmod, glide;     /* wander: LCG, target, countdown, value; mod, glide */
    int32_t id[2], inc[2], amp;                                   /* ramped: input diffusion, LFO steps, depth (Q8) */
    int32_t gm[2], gb[2], dd2, bt, bx, mix, linc, wk;             /* ramped: as nt_par (linc: LOFI's clock) */
    int32_t pd_cur, pd_next, pd_fade;                             /* pre-delay read delays, crossfade (0: none) */
    int16_t hbx[32]; int32_t hbw[16];                             /* STD: half-band input, wet histories (the wet up to
                                                                     8x full scale, for the mix) */
    int32_t hbi, hbj, hbo, half;                                  /* their counts, the next odd sample, the phase */
    uint32_t acc; int32_t hold;                                   /* LOFI: clock phase (Q16), the held wet */
    int32_t cvn, apk, ask, dpk, dsk;                              /* 12-bit converters: count, ADC / DAC peak, step */
    int32_t tx[OP1_FX_BLOCK], ta[OP1_FX_BLOCK], te[OP1_FX_BLOCK], tl[2][OP1_FX_BLOCK];   /* a block of ticks: samples, half 1's
                                                                     input then the taps, reflections, LFOs */
    uint8_t tk[OP1_FX_BLOCK];                                     /* LOFI: the samples that tick */
    int32_t rg[OP1_FX_BLOCK], rgn, rgg, spg;                      /* diffusion HIGH: the last block's diffused input, its
                                                                     count, the regeneration; LOW: the hall's sparse taps */
} nt_t;
static nt_t nt[3];                                                /* synth, drum, master */

/* ---- the tank, a block of ticks at a time: each stage runs over the block on pointers that step back a sample a tick
 * (the ring counts down) and wrap at its start; per tick, the arithmetic of one tick at a time. Half 1 reads half 2's
 * tail 1406 or more ticks back, so no stage waits on a later one within a block. ---- */
#define NT_AT(k) (ring + ((w + (k)) & NT_MASK))                    /* offset k at the block's first tick */
static inline int nt_span(const int16_t *ring, const int16_t *p, int m)   /* ticks before p wraps, m at most */
{
    int a = (int)(p - ring) + 1;
    return a < m ? a : m;
}
static inline int16_t *nt_wrap(int16_t *ring, int16_t *p) { return p < ring ? p + NT_RING : p; }
static inline void nt_ap_b(int16_t *ring, int32_t w, int32_t kh, int32_t kt, int32_t *x, int n, int32_t g, int soft)
{                                                                 /* allpass, tail at len - 1 (FxEngine); soft: bends */
    int16_t *ph = NT_AT(kh), *pt = NT_AT(kt);
    for (int j = 0; j < n; ph = nt_wrap(ring, ph), pt = nt_wrap(ring, pt))
        for (int e = j + nt_span(ring, pt, nt_span(ring, ph, n - j)); j < e; j++) {
            int32_t tail = *pt--, acc = x[j] + nt_m15(tail, g);
            *ph-- = soft ? (int16_t)nt_knee(acc) : nt_s16(acc);   /* the loop's allpasses: a hot tank bends */
            x[j] = nt_m15(acc, -g) + tail;
        }
}
static inline void nt_map_b(int16_t *ring, int32_t w, int32_t kh, int32_t len, int32_t amp, const int32_t *lfo, int32_t *x, int n)
{                                                                 /* modulated allpass, decay diffusion 1 (-0.70) */
    int16_t *ph = NT_AT(kh);
    for (int j = 0; j < n; ph = nt_wrap(ring, ph))
        for (int e = j + nt_span(ring, ph, n - j); j < e; j++) {
            int32_t pos = (len << 15) + (amp * lfo[j] >> 8), i = w - j + kh + (pos >> 15);   /* Q15 samples */
            int32_t a = ring[i & NT_MASK], r = a + nt_m15(ring[(i + 1) & NT_MASK] - a, pos & 32767);
            int32_t acc = x[j] - nt_m15(r, NT_DD1);
            *ph-- = nt_s16(acc);
            x[j] = nt_m15(acc, NT_DD1) + r;
        }
}
static inline void nt_dl_b(int16_t *ring, int32_t w, int32_t kh, int32_t kr, int32_t *x, int n)
{                                                                 /* delay: in at the head, out len back */
    int16_t *ph = NT_AT(kh), *pr = NT_AT(kr);
    for (int j = 0; j < n; ph = nt_wrap(ring, ph), pr = nt_wrap(ring, pr))
        for (int e = j + nt_span(ring, pr, nt_span(ring, ph, n - j)); j < e; j++) { *ph-- = nt_s16(x[j]); x[j] = *pr--; }
}
static inline void nt_add_b(int16_t *ring, int32_t w, int32_t k, int32_t *x, int n)   /* x += the line at offset k */
{
    int16_t *p = NT_AT(k);
    for (int j = 0; j < n; p = nt_wrap(ring, p))
        for (int e = j + nt_span(ring, p, n - j); j < e; j++) x[j] += *p--;
}
static inline void nt_wr_b(int16_t *ring, int32_t w, int32_t k, const int32_t *x, int n)   /* the line at offset k = x */
{
    int16_t *p = NT_AT(k);
    for (int j = 0; j < n; p = nt_wrap(ring, p))
        for (int e = j + nt_span(ring, p, n - j); j < e; j++) *p-- = nt_s16(x[j]);
}
static inline void nt_tap_b(int16_t *ring, int32_t w, int32_t k, int32_t *sum, int n, int neg)   /* sum += +-tap */
{
    int16_t *p = NT_AT(k);
    for (int j = 0; j < n; p = nt_wrap(ring, p))
        for (int e = j + nt_span(ring, p, n - j); j < e; j++) { int32_t v = *p--; sum[j] += neg ? -v : v; }
}
static inline void nt_filt_b(int32_t *x, int n, int32_t *lp, int32_t *xo, int32_t *xo2, int32_t *ef, int32_t bt, int32_t bx, int32_t gm, int32_t gb)
{                                                                 /* damping, then mid / bass gains split by the crossover; */
    int32_t l = *lp, x1 = *xo, x2 = *xo2, e = ef[0], e1 = ef[1], e2 = ef[2], e3 = ef[3];   /* what each product drops carries */
    for (int j = 0; j < n; j++) {                                 /* on to the next tick: a quiet tail decays and filters as */
        int32_t p = nt_c17(l - x[j]) * bt + e1;                   /* a loud one */
        int32_t acc = l = x[j] + (p >> 15); e1 = p & 32767;
        p = nt_c17(x1 - acc) * bx + e2; x1 = acc + (p >> 15); e2 = p & 32767;
        p = nt_c17(x2 - x1) * bx + e3; x2 = x1 + (p >> 15); e3 = p & 32767;
        int32_t y = nt_c17(acc) * gm + e, z = nt_c17(x2) * gb + (y & 32767);
        x[j] = (y >> 15) + (z >> 15); e = z & 32767;
    }
    *lp = l; *xo = x1; *xo2 = x2; ef[0] = e; ef[1] = e1; ef[2] = e2; ef[3] = e3;
}
static inline int32_t nt_ramp(int32_t v, int32_t t)
{
    int32_t d = (t - v) >> 5;
    return v + (d ? d : (t > v) - (t < v));
}
static inline int32_t nt_pdi(int32_t i) { return i < 0 ? i + NT_PD : i; }
#define NT_HW 262143                                              /* STD's wet history: up to 8x full scale */
static inline int64_t nt_hbi(const int32_t *h, int32_t m)        /* the half-band's interpolation sum, exact for |h| <= NT_HW */
{                                                                 /* (tap 1 x 2 NT_HW > 2^31: in two parts; |taps 3..15| < 2^12) */
    int32_t p = h[m & 15] + h[(m + 1) & 15];
    int64_t b = (int64_t)((nt_hb[0] >> 2) * p) * 4 + (nt_hb[0] & 3) * p;
    for (int i = 1; i < 8; i++) b += nt_hb[i] * (h[(m - i) & 15] + h[(m + 1 + i) & 15]);
    return b;
}

static void nt_lfo(nt_t *s, int r)                                /* the LFOs, every 32 tank samples (FxEngine) */
{
    for (int k = 0; k < 2; k++) {
        int32_t v;
        if (!s->wmod) {                                           /* tank mod LOW: a slow random drift */
            if (--s->wn[k] <= 0) {
                s->ws[k] = s->ws[k] * 1103515245u + 12345u;
                s->wt[k] = (int32_t)(s->ws[k] >> 17);
                s->wn[k] = nt_wander[r][k];
            }
            v = s->wv[k] += (s->wt[k] - s->wv[k]) >> nt_wander[r][2];
        } else {
            s->ph[k] += (uint32_t)s->inc[k];
            v = (OP1_SINE_TABLE[s->ph[k] >> 19] + 32768) >> 1;
        }
        s->lfo[k] = s->glide ? s->lfo[k] + ((v - s->lfo[k]) >> 3) : v;
    }
    if (s->glide) s->glide--;
}
static inline int32_t nt_pdc(int32_t d) { return d < 0 ? 0 : (d < NT_PD ? d : NT_PD - 1); }
static inline __attribute__((always_inline)) int32_t nt_er(nt_t *s, int sh, const int32_t *t, const int32_t *g, int n)
{                                                                 /* taps around the pre-delay: a room's 7 reflections behind
                                                                     it, a hall's sparse cloud (diffusion LOW) either side */
    int32_t e = 0;
    for (int i = 0; i < n; i++) {
        int32_t v = s->pd[nt_pdi(s->pw - nt_pdc(s->pd_cur + t[i]))] * (1 << sh);
        if (s->pd_fade)                                           /* crossfading, as the tank's input */
            v += nt_m15(s->pd[nt_pdi(s->pw - nt_pdc(s->pd_next + t[i]))] * (1 << sh) - v, s->pd_fade << 5);
        e += nt_m15(v, g[i]);
    }
    return e;
}

static void nt_snap(nt_t *s, const nt_par *q)
{
    for (int k = 0; k < 2; k++) {
        s->id[k] = nt_diff[q->diff][k]; s->inc[k] = nt_mod[q->rate][q->mod][k];
        s->gm[k] = q->gm[k]; s->gb[k] = q->gb[k];
    }
    s->amp = nt_mod[q->rate][q->mod][2]; s->dd2 = q->dd2; s->bt = q->bt; s->bx = q->bx; s->mix = q->mix;
    s->linc = q->inc; s->wk = q->wk;
    s->rgg = q->diff == 2 ? NT_REGEN : 0; s->spg = q->diff == 0 ? 32767 : 0;
}
static void nt_ramps(nt_t *s, const nt_par *q)
{
    for (int k = 0; k < 2; k++) {
        s->id[k] = nt_ramp(s->id[k], nt_diff[q->diff][k]); s->inc[k] = nt_ramp(s->inc[k], nt_mod[q->rate][q->mod][k]);
        s->gm[k] = nt_ramp(s->gm[k], q->gm[k]); s->gb[k] = nt_ramp(s->gb[k], q->gb[k]);
    }
    s->amp = nt_ramp(s->amp, nt_mod[q->rate][q->mod][2]); s->dd2 = nt_ramp(s->dd2, q->dd2);
    s->bt = nt_ramp(s->bt, q->bt); s->bx = nt_ramp(s->bx, q->bx); s->mix = nt_ramp(s->mix, q->mix);
    s->linc = nt_ramp(s->linc, q->inc); s->wk = nt_ramp(s->wk, q->wk);
    s->rgg = nt_ramp(s->rgg, q->diff == 2 ? NT_REGEN : 0); s->spg = nt_ramp(s->spg, q->diff == 0 ? 32767 : 0);
}

static int32_t nt_muted(nt_t *s, int16_t *buf, const nt_par *q)  /* clearing: the dry part only */
{
    int32_t n = s->clear < NT_CLEAR ? s->clear : NT_CLEAR, peak = 0;
    int16_t *z = s->ring + (s->cend - s->clear);
    for (int32_t i = 0; i < n; i++) z[i] = 0;
    int keep = s->cend == NT_RING;                                /* the tank only: the pre-delay runs on */
    for (int t = 0; t < OP1_FX_BLOCK; t++) {
        if (keep) { s->pd[s->pw] = buf[t]; if (++s->pw == NT_PD) s->pw = 0; }
        int32_t o = buf[t] - nt_m15(buf[t], s->mix);
        if (s->mix) o = nt_knee(o);                               /* as nt_run's output */
        buf[t] = (int16_t)o;
        if (o < 0) o = -o;
        if (o > peak) peak = o;
    }
    if (!(s->clear -= n)) {                                       /* cleared: start from silence */
        s->ein = keep ? NT_OUT : 0;
        s->wp = s->dc = 0;
        s->lp[0] = s->lp[1] = s->xo[0] = s->xo[1] = s->xo2[0] = s->xo2[1] = 0;
        for (int i = 0; i < 8; i++) s->ef[i / 4][i % 4] = 0;
        for (int i = 0; i < 32; i++) s->hbx[i] = 0;
        for (int i = 0; i < 16; i++) s->hbw[i] = 0;
        s->hbi = s->hbj = s->hbo = s->half = s->hold = s->cvn = s->apk = s->dpk = s->rgn = 0;
        s->acc = 0; s->ask = s->dsk = 4;
        if (!keep) { s->pw = s->pd_fade = 0; s->pd_cur = q->pd; }
        nt_snap(s, q);
    }
    return peak;
}

static __attribute__((noinline)) void nt_tank(nt_t *s, int r, int ty, int sh, int32_t *x, int n)   /* n tank samples: in -> wet;
                                                                     sh: the pre-delay holds x / 2^sh (STD: 1, its overshoot) */
{
    const int32_t *base = nt_base[r][ty], *len = nt_len[r][ty], *tap = nt_tap[r][ty];
    int16_t *ring = s->ring;
    int32_t w = s->wp - 1, cv = s->cvn, wk = s->wk, *ta = s->ta, *te = s->te;
    for (int j = 0; j < n; j++) {                                 /* per tick: the LFOs, pre-delay, reflections, ADC */
        if (((w - j) & 31) == 0) nt_lfo(s, r);
        s->tl[0][j] = s->lfo[1]; s->tl[1][j] = s->lfo[0];
        int32_t in = sh ? nt_tz(x[j], 1) : x[j];                  /* pre-delay, crossfading to a new time; diffusion HIGH: */
        if (s->rgg && j < s->rgn) in += nt_m15(s->rg[j] * 4 >> sh, s->rgg);   /* regenerated, the last block's diffused out */
        s->pd[s->pw] = nt_s16(in);
        int32_t v = s->pd[nt_pdi(s->pw - s->pd_cur)] * (1 << sh);
        te[j] = ty ? 0 : nt_er(s, sh, nt_er_t[r], nt_er_g, NT_ERS);
        if (ty == 2 && s->spg) te[j] += nt_m15(nt_er(s, sh, nt_sp_t[r], nt_sp_g, NT_SPS), s->spg);   /* hall, LOW: sparse */
        if (s->ein) { te[j] = te[j] * (NT_OUT - s->ein) >> 8; s->ein--; }   /* a room's reflections: no click */
        if (s->pd_fade) {
            v += nt_m15(s->pd[nt_pdi(s->pw - s->pd_next)] * (1 << sh) - v, s->pd_fade << 5);
            if (++s->pd_fade > NT_FADE) { s->pd_fade = 0; s->pd_cur = s->pd_next; }
        }
        if (++s->pw == NT_PD) s->pw = 0;
        if (r) {                                                  /* the ADC: 12 bits at the last 32 samples' gain */
            int32_t a = v < 0 ? -v : v;
            if (a > s->apk) s->apk = a;
            v = nt_q12(v, s->ask);
            if (++cv == 32) { cv = 0; s->ask = nt_gstep(s->apk); s->apk = 0; }
        }
        x[j] = nt_m15(v, 8192);                                   /* 0.25 in (6 dB of room in the lines), 4 allpasses */
    }
    nt_ap_b(ring, w, base[0], base[0] + len[0] - 1, x, n, s->id[0], 0);
    nt_ap_b(ring, w, base[1], base[1] + len[1] - 1, x, n, s->id[0], 0);
    nt_ap_b(ring, w, base[2], base[2] + len[2] - 1, x, n, s->id[1], 0);
    nt_ap_b(ring, w, base[3], base[3] + len[3] - 1, x, n, s->id[1], 0);
    for (int j = 0; j < n; j++) { ta[j] = x[j]; s->rg[j] = x[j]; }   /* rg: fed back next block (diffusion HIGH) */
    s->rgn = n;
    nt_add_b(ring, w, base[11] + len[11], x, n);                  /* half 1, fed by half 2's tail */
    nt_map_b(ring, w, base[4], len[4], s->amp, s->tl[0], x, n);
    nt_dl_b(ring, w, base[5], base[5] + len[5], x, n);
    nt_filt_b(x, n, &s->lp[0], &s->xo[0], &s->xo2[0], s->ef[0], s->bt, s->bx, s->gm[0], s->gb[0]);
    nt_ap_b(ring, w, base[6], base[6] + len[6] - 1, x, n, s->dd2, 1);
    nt_dl_b(ring, w, base[7], base[7] + len[7], x, n);
    for (int j = 0; j < n; j++) { x[j] += ta[j]; ta[j] = 0; }     /* half 2, fed by half 1's tail; ta: the taps' sum */
    nt_map_b(ring, w, base[8], len[8], s->amp, s->tl[1], x, n);
    nt_dl_b(ring, w, base[9], base[9] + len[9], x, n);
    nt_filt_b(x, n, &s->lp[1], &s->xo[1], &s->xo2[1], s->ef[1], s->bt, s->bx, s->gm[1], s->gb[1]);
    nt_ap_b(ring, w, base[10], base[10] + len[10] - 1, x, n, s->dd2, 1);
    nt_wr_b(ring, w, base[11], x, n);
#pragma GCC unroll 14
    for (int i = 0; i < NT_TAPS; i++)                             /* 7 left + 7 right taps; unrolled: the tables fold */
        nt_tap_b(ring, w, base[nt_tap_line[i]] + tap[i], ta, n, nt_tap_sign[i] < 0);
    for (int j = 0; j < n; j++) {                                 /* per tick: the wet's level, low cut, the DAC */
        int32_t wet = nt_c17(ta[j]) * wk;                         /* clamped where the output clips anyway */
        wet = nt_tz(wet, 13) + te[j];                             /* x2 for the 0.25 in */
        int32_t wq = wet * 256;                                   /* low cut, Q8: holds 20 Hz at low levels */
        s->dc = wq + nt_m15w(s->dc - wq, nt_dc_b[r]);
        wet = nt_tz(wq - s->dc, 8);
        if (r) {                                                  /* the DAC; a new gain step every 32 samples */
            int32_t a = wet < 0 ? -wet : wet;
            if (a > s->dpk) s->dpk = a;
            wet = nt_q12(wet, s->dsk);
            if (++s->cvn == 32) { s->cvn = 0; s->dsk = nt_gstep(s->dpk); s->dpk = 0; }
        }
        x[j] = wet;
    }
    s->wp = (s->wp - n) & NT_MASK;
}

static __attribute__((noinline)) int32_t nt_run(nt_t *s, int16_t *buf, const nt_par *q)
{
    int fading = s->out > 0, clock = s->lay >= 6 ? NT_LOFI : s->lay >= 3, n = 0;   /* lay = type + 3 clock */
    const int r = s->lay >= 3, ty = s->lay - 3 * clock;           /* STD and LOFI: 22.05 kHz tables, converters */
    if (!s->pd_fade && q->pd != s->pd_cur) { s->pd_next = q->pd; s->pd_fade = 1; }
    int32_t peak = 0, *x = s->tx;
    if (clock == NT_HIFI) for (; n < OP1_FX_BLOCK; n++) x[n] = buf[n];
    else if (clock == NT_STD)                                     /* 22.05 kHz between half-band filters: decimate */
        for (int t = 0, h = s->half; t < OP1_FX_BLOCK; t++) {
            s->hbx[s->hbi++ & 31] = (int16_t)buf[t];
            if (!(h ^= 1)) {
                int32_t c = s->hbi - 16, a = 16384 * s->hbx[c & 31];   /* centred 15 samples back */
                for (int j = 0; j < 8; j++) a += nt_hb[j] * (s->hbx[(c - 2 * j - 1) & 31] + s->hbx[(c + 2 * j + 1) & 31]);
                x[n++] = nt_shr(a, 15);
            }
        }
    else                                                          /* LOFI: sample and hold at its clock */
        for (int t = 0; t < OP1_FX_BLOCK; t++)
            if ((s->tk[t] = (s->acc += (uint32_t)s->linc) >= 65536u)) { s->acc -= 65536u; x[n++] = buf[t]; }
    nt_tank(s, r, ty, clock == NT_STD, x, n);
    for (int t = 0, j = 0; t < OP1_FX_BLOCK; t++) {
        int32_t in = buf[t], wet;
        if (clock == NT_HIFI) wet = x[t];
        else if (clock == NT_STD) {
            if ((s->half ^= 1)) wet = s->hbo;                     /* the interpolator's odd sample */
            else {
                int32_t v = x[j++];
                s->hbw[s->hbj++ & 15] = v < -NT_HW ? -NT_HW : (v > NT_HW ? NT_HW : v);   /* to 8x full scale */
                int32_t m = s->hbj - 9;                           /* interpolate: centred 8 tank samples back */
                int64_t b = nt_hbi(s->hbw, m);
                wet = s->hbw[m & 15]; s->hbo = (int32_t)(b >= 0 ? b >> 14 : -(-b >> 14));
            }
        } else {
            if (s->tk[t]) s->hold = x[j++];
            wet = s->hold;
        }
        if (fading) { wet = wet * s->out >> 8; if (s->out) s->out--; }   /* before a type or clock change */
        int32_t o = in + nt_m15w(wet - in, s->mix);              /* the wet in full, however far off */
        if (s->mix) o = nt_knee(o);                               /* mix 0: the dry untouched */
        buf[t] = (int16_t)o;
        if (o < 0) o = -o;
        if (o > peak) peak = o;
    }
    if (fading && !s->out) {                                      /* faded out: clear the tank (STD, LOFI: and the */
        int all = s->lay >= 3 || q->lay >= 3;                     /* pre-delay, which runs at their clock) */
        s->lay = q->lay; s->clear = s->cend = all ? NT_RING + NT_PD : NT_RING;
    }
    return peak;
}

static void nt_init(int pos)
{
    nt_t *s = &nt[pos % 3];
    int16_t *m = (int16_t *)op1_fx_scratch(pos);
    for (uint32_t i = 0; i < sizeof *s; i++) ((uint8_t *)s)[i] = 0;
    if (!m) return;
    s->ring = m; s->pd = m + NT_RING;
    s->lay = -1; s->fresh = 1; s->ask = s->dsk = 4; s->ws[0] = 1; s->ws[1] = 2;
}

static void nt_render(int pos, int16_t *buf, const int16_t *p)
{
    nt_t *s = &nt[pos % 3];
    if (!s->ring) return;
    nt_par q;
    nt_params(p, &q);
    if (s->fresh) { s->fresh = 0; s->wmod = q.mod; nt_snap(s, &q); }
    else {
        nt_ramps(s, &q);
        if (!q.mod != !s->wmod) s->glide = NT_GLIDE;              /* to or from LOW: glide between the LFOs */
        s->wmod = q.mod;
    }
    if (q.lay != s->lay && !s->clear && !s->out) {
        if (s->lay < 0) { s->lay = q.lay; s->clear = s->cend = NT_RING + NT_PD; }   /* first: everything */
        else s->out = NT_OUT;                                     /* fade the wet out, then clear the tank */
    }
    int32_t peak = s->clear ? nt_muted(s, buf, &q) : nt_run(s, buf, &q);
    volatile int16_t *m = op1_fx_meter(pos);
    int32_t mv = (*m * 0xfe00) >> 16;
    *m = (int16_t)(peak > mv ? peak : mv);
}

const op1_fx_t FX_nostromo = { nt_init, nt_render, { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_NONE } };

/* ======================= screen (GUI thread) =======================
 * nostromo.svg (gen_svg.py, geometry in nostromo_geom.h), one page:
 *   the LED readout   the control turned or paged last, named in its knob's colour
 *   the scope         the reverb's impulse response on a log time axis (0 .. 10 s), drawn as raster lines and kept
 *                     until a setting changes: the dry hit (red, the mix), the pre-delay (white bracket, the gate), the
 *                     reflections (their number the diffusion, scattered by the tank mod, a room's early ones; STD in
 *                     12-bit steps, LOFI a sample-and-hold staircase at its clock; diffusion HIGH: the attack's echoes
 *                     through the regenerated pre-delay; a hall at LOW: the sparse taps' cloud around the gate), the
 *                     mid decay (blue), the bass (blue, dashed) and the treble decay (green), each falling 60 dB
 *                     at its RT60
 *   the ship          the Nostromo where the tail falls silent, its exhaust back to the gate: the exhaust's volume the
 *                     bass against the mid, its heat the treble, its brightness the mix; the engines flare with the FX meter; it moves with
 *                     the tank mod, least to most: drifts 1 px (LOW), bobs 2 px (MED), 4 px at 3 Hz, its flame
 *                     shimmering (HIGH), and the stars with it: steady (LOW), a slow twinkle (MED), a fast flicker
 *                     (HIGH); smoothly (HIFI), at half rate on a 2 px
 *                     grid (STD) or in steps at the clock (LOFI); a type or clock change (the reverb fades and clears)
 *                     jumps it to hyperspace and back
 *   the labels        each knob's two pages, the current one bright: one copy each, recoloured; in LOFI white's page 1
 *                     reads RATE (it sets the clock there, as the CXM 1978's pre-delay slider), on the readout too
 *   the caption       the program in words (its number: the readout), typed out when it changes (in place while it keeps changing; still for 10 s, the
 *                     next change wipes it again), a cursor blinking after it; MU/TH/UR 6000 when the screen
 *                     opens; SPECIAL ORDER 937 when a hall's mid decay reaches 10 s, CREW EXPENDABLE when the mix
 *                     reaches 0 (turned by hand)
 * Turns count on the stored words, so an LFO moves the picture but marks nothing. */
#ifndef OP1_HOST
#pragma GCC optimize ("Os", "no-tree-loop-distribute-patterns")   /* GUI code: size */
#endif
#include "nostromo_geom.h"

enum { NC_MID, NC_TREB, NC_PRE, NC_MIX, NC_BASS, NC_CLK, NC_PROG, NC_N };  /* readout controls */
enum { NU_NONE = -1, NU_SEC, NU_MS, NU_HZ, NU_KHZ };
enum { NM_PROG, NM_BOOT, NM_SO937, NM_CREW };                     /* the caption's modes */
#define NS_BLANK   10
#define NS_GAP_MS  250                                            /* a frame gap that long: the screen was hidden */
#define NS_MOVE    128                                            /* knob change that marks a control */
#define NS_TYPE_MS 30                                             /* the caption types a character in 30 ms */
#define NS_BOOT_MS 1600                                           /* MU/TH/UR 6000 on the screen */
#define NS_EGG_MS  2600                                           /* an easter egg on the screen */
#define NS_WIPE_MS 10000                                          /* the caption still that long: the next change wipes it */
#define NS_JUMP_MS 600                                            /* hyperspace: out 250 ms, gone 100 ms, back 250 ms */
#define NS_LINES   192                                            /* the scope's lines, kept */
#define C_BLUE  0x4D9EFFu
#define C_GREEN 0x00ED95u
#define C_WHITE 0xFFFFFFu
#define C_RED   0xFF3A5Du
#define C_GREY  0x6F7173u
static const uint8_t ns_7seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };   /* a..g */

typedef struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } ns_ln;
static struct {
    int32_t prev[NC_N], last, pos, armed, pg[3];
    uint32_t t, ts, fr;                                           /* the animation's time (ms), the time drawn, frames */
    int32_t key[NC_N + 1], nl, xs, sx;                            /* the scope: its inputs, lines; the gate, the ship's place (px) */
    int32_t lay, hj;                                              /* type + 3 clock seen; hyperspace: ms left */
    int32_t cm, ct, ck, cq, egg[2];                               /* the caption: mode, ms in it, program + clock, ms since it
                                                                     changed; eggs' conditions */
    int32_t fl, ms;                                               /* the flare (Q15), the meter's slow average */
} ns;
static ns_ln ns_l[NS_LINES];
static void *ns_seg[3][7], *ns_dp[2], *ns_unit[4], *ns_nm[NC_N], *ns_w[4][3], *ns_rate[3];   /* RATE: label, PRE-DLAY, readout */
static void *ns_cp, *ns_msg[3], *ns_cv, *ns_cu, *ns_mxh, *ns_mxt[10], *ns_mxo[10], *ns_sh, *ns_sr[6];
static uint8_t *ns_sf[6];                                         /* the stars' fills: they twinkle with the tank mod */
static uint8_t *ns_lab[NS_LABELS][NS_LAB_MAX];                    /* the labels' stroke colours */
static const int8_t ns_page_ctl[3][2] = { { NC_MID, NC_BASS }, { NC_TREB, NC_CLK }, { NC_PRE, NC_PROG } };   /* per page */
static const uint16_t ns_star[6][5] = { { 40, 41, 8, 255, 0 }, { 95, 55, 12, 150, 2700 }, { 150, 39, 16, 215, 5400 },
                                        { 200, 50, 8, 130, 1300 }, { 250, 43, 12, 240, 4100 }, { 290, 56, 16, 175, 6800 } };   /* x, y, px/s,
                                                                     brightness (Q8), twinkle phase (8192 a turn) */

/* the readout for one control (ty: the type; hz: LOFI's clock, 0 otherwise - the decay, cutoffs and white knob
 * follow it): 3 digits (NS_BLANK: off), the decimal point after digit dp (-1: none), a unit */
static void ns_fmt(int c, int32_t v, int ty, int32_t hz, uint8_t d[3], int *dp, int *unit)
{
    int32_t n;
    *dp = -1; *unit = NU_NONE;
    if ((c == NC_PRE && hz) || c == NC_CLK) {                     /* the tank's clock, kHz (LOFI: the white knob) */
        int32_t f = hz ? hz : nt_clock(v) ? 22050 : 44100;
        *unit = NU_KHZ;
        n = (f + 5) / 10; *dp = 0;
        if (n >= 1000) { n = (f + 50) / 100; *dp = 1; }           /* 9.995 kHz and up: 10.0 */
    } else switch (c) {
    case NC_MID: case NC_BASS:                                                                 /* s: 0.55, 2.4 */
        n = nt_lut(nt_rt[ty], v); *unit = NU_SEC;
        if (hz) n = (n * 22050 + hz / 2) / hz;                    /* LOFI stretches it */
        if (n < 1000) { n = (n + 5) / 10; *dp = 0; } else { n = (n + 50) / 100; *dp = 1; }
        break;
    case NC_TREB:
        n = nt_lut(nt_treb_hz, v);
        if (hz) n = (n * hz + 11025) / 22050;                     /* LOFI scales it */
        if (n < 1000) *unit = NU_HZ;
        else { n = (n + 50) / 100; *dp = 1; *unit = NU_KHZ; }
        break;
    case NC_PRE: n = (nt_pd(v) * 1000 + 22050) / 44100; *unit = NU_MS; break;
    case NC_MIX: n = (v * 100 + 16383) >> 15; break;
    default: n = (v * 27 >> 15) + 1; break;                                                   /* program */
    }
    d[0] = n >= 100 || *dp == 0 ? (uint8_t)(n / 100) : NS_BLANK;
    d[1] = n >= 10 || *dp >= 0 ? (uint8_t)(n / 10 % 10) : NS_BLANK;
    d[2] = (uint8_t)(n % 10);
}

/* the control turned or paged last (else blue's page) (a: stored words, pg: pages) */
static void ns_track(const int32_t a[NC_N], const int pg[3], int pos)
{
    if (!ns.armed || pos != ns.pos) {                             /* first frame, another FX position */
        for (int i = 0; i < NC_N; i++) ns.prev[i] = a[i];
        for (int i = 0; i < 3; i++) ns.pg[i] = pg[i];
        ns.armed = 1; ns.pos = pos; ns.last = ns_page_ctl[0][pg[0]];
    }
    for (int i = 0; i < NC_N; i++) {
        int32_t d = a[i] - ns.prev[i];
        if (d >= NS_MOVE || d <= -NS_MOVE) { ns.last = i; ns.prev[i] = a[i]; }
    }
    for (int i = 0; i < 3; i++) if (pg[i] != ns.pg[i]) { ns.pg[i] = pg[i]; ns.last = ns_page_ctl[i][pg[i]]; }
}

static void *ns_find(void *scr, const char *pre, int a, int b)    /* id: pre, number a (-1: none), char b (0: none) */
{
    char id[8];
    int k = 0;
    while (*pre && k < 4) id[k++] = *pre++;
    if (a >= 10) id[k++] = (char)('0' + a / 10);
    if (a >= 0) id[k++] = (char)('0' + a % 10);
    if (b) id[k++] = (char)b;
    id[k] = 0;
    return op1_find_node(scr, id);
}
static void ns_vis(void *n, int on) { if (n) op1_node_set_visible(n, on); }
static void ns_mat(void *n, int32_t a, int32_t tx, int32_t ty)   /* scale x by a (Q10), then translate (Q10 px) */
{
    int32_t m[9] = { a, 0, 0, 0, 1024, 0, tx, ty, 1024 };
    if (n) op1_node_set_transform(n, m);
}
static inline int32_t ns_sin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }   /* Q15, 8192 a turn */
static uint32_t ns_rgb(uint32_t a, uint32_t b, int32_t f)         /* a .. b, f Q15 */
{
    uint32_t o = 0;
    for (int s = 0; s < 24; s += 8) {
        int32_t x = (int32_t)(a >> s & 255), y = (int32_t)(b >> s & 255);
        o |= (uint32_t)(x + ((y - x) * f >> 15)) << s;
    }
    return o;
}
static void ns_col(uint8_t c[4], uint32_t rgb, int32_t l)        /* l: brightness, Q8 */
{
    c[0] = (uint8_t)((rgb >> 16 & 255) * l >> 8); c[1] = (uint8_t)((rgb >> 8 & 255) * l >> 8);
    c[2] = (uint8_t)((rgb & 255) * l >> 8); c[3] = 0xFF;
}
static void ns_draw(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, uint32_t rgb, int32_t l)
{                                                                 /* Q10 px; the OP-1 draws no zero-length line */
    if (x0 == x1 && y0 == y1) return;
    uint8_t c[4];
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    ns_col(c, rgb, l);
    op1_raster_line(r, &a, &b, w, c);
}
static void ns_keep(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, uint32_t rgb, int32_t l)
{                                                                 /* a scope line, kept until a setting changes */
    if ((x0 == x1 && y0 == y1) || ns.nl >= NS_LINES) return;
    ns_ln *p = &ns_l[ns.nl++];
    p->x0 = x0; p->y0 = y0; p->x1 = x1; p->y1 = y1; p->w = w;
    ns_col(p->c, rgb, l);
}

static void ns_dash(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, uint32_t rgb, int32_t l, int32_t *ph)
{                                                                 /* a kept line dashed, 4 px on, 3 off; ph: along the pattern */
    int32_t dx = x1 - x0, dy = y1 - y0, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    int32_t len = ax > ay ? ax + (ay * 3 >> 3) : ay + (ax * 3 >> 3), t = 0;   /* about the length, Q10 */
    while (t < len) {
        int32_t p = *ph % 7168, on = p < 4096, seg = (on ? 4096 : 7168) - p;
        if (seg > len - t) seg = len - t;
        if (on) {
            int32_t f0 = (t << 10) / len, f1 = ((t + seg) << 10) / len;
            ns_keep(x0 + (dx * f0 >> 10), y0 + (dy * f0 >> 10), x0 + (dx * f1 >> 10), y0 + (dy * f1 >> 10), w, rgb, l);
        }
        *ph += seg; t += seg;
    }
}

/* the scope's log time axis: u, Q15 of its width, to ms x 8, and back (10 s and longer: its end) */
static int32_t ns_t_of_u(int32_t u)
{
    int32_t i = u >> 9;
    if (i >= 64) return ns_tinv[64];
    return ns_tinv[i] + ((ns_tinv[i + 1] - ns_tinv[i]) * (u & 511) >> 9);
}
static int32_t ns_u_of_t(int32_t t8)
{
    if (t8 >= ns_tinv[64]) return 32768;
    int i = 0;
    while (ns_tinv[i + 1] <= t8) i++;
    return (i << 9) + ((t8 - ns_tinv[i]) << 9) / (ns_tinv[i + 1] - ns_tinv[i]);
}
static int32_t ns_env(int32_t t8, int32_t rt)                    /* level at t, Q15: 60 dB down, linearly, at rt ms */
{
    int32_t e = 32768 - t8 * 4096 / rt;
    return e < 0 ? 0 : e;
}

/* the scope, kept in ns_l (rt: the mid decay, ms, LOFI's stretch in; hz: LOFI's clock, else 0) */
static void ns_scope(const int32_t v[NC_N], const int sel[4], const int pg[3], int32_t hz, int32_t rt)
{
    const int32_t H = NS_BASE - NS_TOP, B = NS_BASE << 10;
    ns.nl = 0;
    int32_t pre;                                                  /* pre-delay: the knob's travel; LOFI: 186 ms .. 1.5 s */
    if (hz) { int32_t x = 384 * (32767 - v[NC_PRE]) >> 15; pre = NS_PRE_PX * (nt_exp2[x & 255] >> (x >> 8)) >> 15; }
    else pre = NS_PRE_PX * v[NC_PRE] >> 15;
    int32_t xs = ns.xs = NS_XD + 4 + pre, W = NS_X1 - xs;
#define NS_X(u) ((xs << 10) + (W * (u) >> 5))                     /* u Q15 -> x, Q10 */
    static const int32_t tick[3] = { 800, 8000, 80000 };          /* 100 ms, 1 s, 10 s */
    for (int i = 0; i < 3; i++) {
        int32_t x = (NS_X(ns_u_of_t(tick[i])) & ~1023) + 512;
        ns_keep(x, B + 1024, x, B + 4096, 1024, C_GREY, 204);
    }
    int32_t dry = 32768 - v[NC_MIX];                              /* the dry hit */
    if (dry > 655) ns_keep((NS_XD << 10) + 512, B, (NS_XD << 10) + 512, B - ((H << 10) * dry >> 15), 3072, C_RED, 256);
    int32_t lw = pg[2] ? 128 : 256;                               /* pre-delay (white, page 1): the bracket, the gate */
    ns_keep((NS_XD + 2) << 10, B + 3584, xs << 10, B + 3584, 1024, C_WHITE, lw);
    ns_keep((xs << 10) + 512, B + 6144, (xs << 10) + 512, (NS_SHIP_Y + 2) << 10, 1024, C_WHITE, lw);
    int32_t hw = (H << 10) * (6554 + (v[NC_MIX] * 26214 >> 15)) >> 15;   /* the wet's height: 20 .. 100 % */
    if (sel[3] != NT_LOFI) {                                      /* reflections */
        static const int32_t jit[3] = { 6554, 16384, 29491 };     /* scattered by the tank mod */
        int nb = sel[1] == 0 ? 18 : sel[1] == 1 ? 28 : 40;        /* their number: the diffusion */
        uint32_t seed = 7;
        for (int i = 0; i < nb; i++) {
            seed = (seed * 1103515245u + 12345u) & 0x7fffffff;
            int32_t r = (int32_t)(seed >> 8) & 1023, p = i * 1024 + 512 + ((r - 512) * jit[sel[2]] >> 15);
            int32_t x = ((xs + 2) << 10) + (W - 2) * p / nb, u = ((x - (xs << 10)) << 5) / W;
            int32_t h = (hw * ns_env(ns_t_of_u(u), rt) >> 15) * (614 + (r * 410 >> 10)) >> 10;
            if (sel[3] == NT_STD) { int32_t lv = (H << 10) / 10; h = (h + lv / 2) / lv * lv; }   /* 12-bit steps */
            x = (x & ~1023) + 512;
            if (h >= 1024) ns_keep(x, B, x, B - h, 1024, C_WHITE, 153);
        }
        if (sel[0] == 0) {                                        /* a room: early reflections */
            static const uint8_t dx[5] = { 3, 6, 8, 11, 14 }, f[5] = { 230, 166, 205, 141, 179 };
            for (int i = 0; i < 5; i++) {
                int32_t x = ((xs + dx[i]) << 10) + 512;
                ns_keep(x, B, x, B - (hw * f[i] >> 8), 1024, C_WHITE, 153);
            }
        }
    } else {                                                      /* LOFI: a sample-and-hold staircase at its clock */
        int32_t step = 8 * 22050 / hz, x = xs + 1, y0 = B;
        while (x < NS_X1) {
            int32_t x1 = x + step < NS_X1 ? x + step : NS_X1, u = ((x - xs) << 15) / W;
            int32_t y = B - ((hw * ns_env(ns_t_of_u(u), rt) >> 15) * 218 >> 8);
            ns_keep(x << 10, y0, x << 10, y, 1024, C_WHITE, 153);
            ns_keep(x << 10, y, x1 << 10, y, 1024, C_WHITE, 153);
            x = x1; y0 = y;
        }
        ns_keep(NS_X1 << 10, y0, NS_X1 << 10, B, 1024, C_WHITE, 153);
    }
    int32_t pms = hz ? (186 * 22050 + hz / 2) / hz : (nt_pd(v[NC_PRE]) * 1000 + 22050) / 44100;   /* pre-delay, ms */
    if (sel[1] == 2) {                                            /* diffusion HIGH: the attack regenerated through the */
        int32_t h = hw;                                           /* pre-delay, every pre-delay + a block (1.45 ms) */
        for (int k = 1; k <= 4; k++) {
            h >>= 1;
            int32_t u = ns_u_of_t(k * (pms * 8 + 12));
            if (u >= 32768) break;
            int32_t x = (NS_X(u) & ~1023) + 512;
            ns_keep(x, B, x, B - h, 2048, C_WHITE, 230);
        }
    }
    if (sel[1] == 0 && sel[0] == 2) {                             /* a hall at diffusion LOW: the sparse cloud */
        for (int i = 0; i < NT_SPS; i++) {
            int32_t ms = nt_sp_t[0][i] * 1000 / 44100, x;
            if (hz) ms = ms * 22050 / hz;
            if (ms >= 0) x = NS_X(ns_u_of_t(ms * 8)) + 2048;
            else { int32_t d = pms > 0 ? -ms * pre / pms : 0; x = ((xs - (d < pre ? d : pre) - 2) << 10); }
            x = (x & ~1023) + 512;
            int32_t g = nt_sp_g[i] < 0 ? -nt_sp_g[i] : nt_sp_g[i];
            ns_keep(x, B, x, B - (hw * g >> 13), 1024, C_WHITE, 204);   /* x4: soft taps, shown */
        }
    }
    int32_t hf = 1907 + (29789 * v[NC_TREB] >> 15) - (hz ? 17873 * v[NC_PRE] >> 15 : 0);   /* treble: log2(f / 400) / 5.5 */
    hf = hf < 3932 ? 3932 : hf > 32768 ? 32768 : hf;
    int32_t rtb = nt_lut(nt_rt[sel[0]], v[NC_BASS]);
    if (hz) rtb = (rtb * 22050 + hz / 2) / hz;                    /* LOFI stretches it */
    const int32_t ert[3] = { rtb, rt * (hf >> 3) >> 12, rt };     /* rt: 80 s at most */
    const uint32_t ec[3] = { C_BLUE, C_GREEN, C_BLUE };
    const int32_t el[3] = { pg[0] ? 256 : 140, pg[1] ? 128 : 256, pg[0] ? 128 : 256 }, ew[3] = { 1536, 1024, 1536 };   /* bass dashed: two blues */
    for (int k = 0; k < 3; k++) {                                 /* bass, treble, mid on top: 16 segments each */
        int32_t ph = 0;
        int32_t x0 = xs << 10, y0 = B - hw, end = ert[k] * 8;
        for (int i = 1; i <= 16; i++) {
            int32_t u = i << 11, t = ns_t_of_u(u), last = t >= end;
            if (last) { u = ns_u_of_t(end); t = end; }
            int32_t x = NS_X(u), y = B - (hw * ns_env(t, ert[k]) >> 15);
            if (k == 0) ns_dash(x0, y0, x, y, ew[k], ec[k], el[k], &ph); else ns_keep(x0, y0, x, y, ew[k], ec[k], el[k]);
            x0 = x; y0 = y;
            if (last) break;
        }
    }
    int32_t sx = xs + (W * ns_u_of_t(rt * 8) >> 15);              /* the ship: where the mid decay falls silent */
    ns.sx = sx < xs + 4 ? xs + 4 : sx > NS_X1 - NS_SHIP_W - 1 ? NS_X1 - NS_SHIP_W - 1 : sx;
#undef NS_X
}

static void ns_init(void *scr)
{
    for (int d = 0; d < 3; d++) for (int g = 0; g < 7; g++) ns_seg[d][g] = ns_find(scr, "s", d, 'a' + g);
    for (int i = 0; i < 2; i++) ns_dp[i] = ns_find(scr, "dp", i, 0);
    for (int i = 0; i < 4; i++) ns_unit[i] = ns_find(scr, "u", i, 0);
    for (int i = 0; i < NC_N; i++) ns_nm[i] = ns_find(scr, "nm", i, 0);
    ns_rate[0] = ns_find(scr, "lr", -1, 0); ns_rate[1] = ns_find(scr, "lp", -1, 0); ns_rate[2] = ns_find(scr, "nm", 7, 0);
    for (int i = 0; i < NS_LABELS; i++)
        for (int j = 0; j < ns_lab_n[i]; j++) { char id[4] = { 'l', (char)('0' + i), (char)('a' + j), 0 }; ns_lab[i][j] = op1_node_line_rgba(scr, id); }
    static const char *const w[4] = { "wa", "wb", "wc", "wd" };
    for (int c = 0; c < 4; c++) for (int r = 0; r < 3; r++) ns_w[c][r] = ns_find(scr, w[c], r, 0);
    ns_cp = ns_find(scr, "cp", -1, 0); ns_cv = ns_find(scr, "cv", -1, 0); ns_cu = ns_find(scr, "cu", -1, 0);
    ns_msg[0] = ns_find(scr, "bt", -1, 0); ns_msg[1] = ns_find(scr, "e", 0, 0); ns_msg[2] = ns_find(scr, "e", 1, 0);
    ns_mxh = ns_find(scr, "xh", 1, 0);
    for (int i = 0; i < 10; i++) { ns_mxt[i] = ns_find(scr, "xt", i, 0); ns_mxo[i] = ns_find(scr, "xo", i, 0); }
    ns_sh = ns_find(scr, "sh", -1, 0);
    for (int i = 0; i < 6; i++) { char id[4] = { 's', 'f', (char)('0' + i), 0 }; ns_sr[i] = ns_find(scr, "sr", i, 0); ns_sf[i] = op1_node_fill_rgba(scr, id); }
    ns.armed = 0;
}

static void ns_update(void *scr, int dt)
{
    int pos = op1_fx_screen_pos();
    const int16_t *w = op1_fx_knobs(pos);
    int16_t k[4];
    op1_fx_knobs_live(k);
    int32_t v[NC_N] = { k[0], k[1], k[2], k[3], w[5] < 0 ? 0 : w[5], w[6] < 0 ? 0 : w[6], w[7] < 0 ? 0 : w[7] };   /* live */
    int32_t a[NC_N] = { op1_fx_param_q15(w, 0), op1_fx_param_q15(w, 1), op1_fx_param_q15(w, 2), op1_fx_param_q15(w, 3),
                        v[4], v[5], v[6] };                                     /* stored: moved by hand only */
    int pg[3];
    for (int i = 0; i < 3; i++) pg[i] = op1_fx_page[pos][i] != 0;
    int fresh = !ns.armed || pos != ns.pos || dt > NS_GAP_MS;                   /* the screen opens */
    ns_track(a, pg, pos);
    if (dt < 0 || dt > NS_GAP_MS) dt = 0;                                       /* no time passes */
    int prog = v[NC_PROG] * 27 >> 15, sel[4] = { prog / 9, prog / 3 % 3, prog % 3, nt_clock(v[NC_CLK]) };
    int32_t hz = sel[3] == NT_LOFI ? nt_lut(nt_lofi_inc, v[NC_PRE]) * 44100 >> 16 : 0;   /* LOFI's clock, Hz */
    int32_t rt = nt_lut(nt_rt[sel[0]], v[NC_MID]);
    if (hz) rt = (rt * 22050 + hz / 2) / hz;                                    /* LOFI stretches it */
    int32_t lay = sel[0] + 3 * sel[3];
    int egg[2] = { sel[0] == 2 && a[NC_MID] >= 32640, a[NC_MIX] < 164 };      /* a hall at 10 s; the mix at 0 */
    if (fresh) { ns.cm = NM_BOOT; ns.ct = 0; ns.hj = 0; ns.lay = lay; ns.key[0] = -1; ns.fl = 0; ns.cq = NS_WIPE_MS; ns.ms = *op1_fx_meter(pos); }
    else {
        if (lay != ns.lay) { ns.lay = lay; ns.hj = NS_JUMP_MS; }                /* the reverb fades and clears: hyperspace */
        if (egg[0] && !ns.egg[0]) { ns.cm = NM_SO937; ns.ct = 0; }
        if (egg[1] && !ns.egg[1]) { ns.cm = NM_CREW; ns.ct = 0; }
    }
    ns.egg[0] = egg[0]; ns.egg[1] = egg[1];
    int ck = prog * 3 + sel[3];
    if (ns.cq < NS_WIPE_MS) ns.cq += dt;
    if (ck != ns.ck && !fresh) {                                                /* a new program: wiped and typed out once, */
        if (ns.cm == NM_PROG && ns.cq >= NS_WIPE_MS) ns.ct = 0;                 /* then in place while it keeps changing */
        ns.cq = 0;
    }
    ns.ck = ck;
    ns.ct += dt;
    if (ns.cm != NM_PROG && ns.ct >= (ns.cm == NM_BOOT ? NS_BOOT_MS : NS_EGG_MS)) { ns.cm = NM_PROG; ns.ct = 0; }
    ns.t += (uint32_t)dt;                                                       /* the clock: smooth, half rate, LOFI's steps */
    if (sel[3] == NT_HIFI || (sel[3] == NT_STD && (++ns.fr & 1)) || (sel[3] == NT_LOFI && ns.t - ns.ts >= (uint32_t)(1837500 / hz)))
        ns.ts = ns.t;

    uint8_t d[3];                                                               /* the readout */
    int dp, unit;
    ns_fmt(ns.last, v[ns.last], sel[0], hz, d, &dp, &unit);
    for (int i = 0; i < 3; i++)
        for (int g = 0; g < 7; g++) ns_vis(ns_seg[i][g], d[i] != NS_BLANK && (ns_7seg[d[i]] >> g & 1));
    for (int i = 0; i < 2; i++) ns_vis(ns_dp[i], dp == i);
    for (int i = 0; i < 4; i++) ns_vis(ns_unit[i], unit == i);
    for (int i = 0; i < NC_N; i++) ns_vis(ns_nm[i], ns.last == i && !(i == NC_PRE && hz));
    ns_vis(ns_rate[0], hz != 0); ns_vis(ns_rate[1], !hz);                        /* LOFI: white's page 1 is the clock, */
    ns_vis(ns_rate[2], ns.last == NC_PRE && hz);                                 /* RATE (as the CXM 1978's pre-delay) */
    const int bright[NS_LABELS] = { !pg[0], pg[0], !pg[1], pg[1], !pg[2], pg[2], !pg[2] };   /* each knob's current page */
    for (int i = 0; i < NS_LABELS; i++)
        for (int j = 0; j < ns_lab_n[i]; j++) {
            uint8_t *c = ns_lab[i][j];
            if (c) for (int b = 0; b < 3; b++) c[b] = (uint8_t)(ns_lab_rgb[i][b] * (bright[i] ? 256 : 102) >> 8);
        }
    int pc = (v[NC_MIX] * 100 + 16383) >> 15;                                   /* the mix percent */
    ns_vis(ns_mxh, pc == 100);
    for (int i = 0; i < 10; i++) { ns_vis(ns_mxt[i], pc >= 10 && pc / 10 % 10 == i); ns_vis(ns_mxo[i], pc % 10 == i); }
    ns_vis(ns_cp, ns.cm == NM_PROG);                                            /* the caption */
    for (int i = 0; i < 3; i++) ns_vis(ns_msg[i], ns.cm == i + 1);
    for (int c = 0; c < 4; c++) for (int r = 0; r < 3; r++) ns_vis(ns_w[c][r], sel[c] == r);
    int32_t end = ns.cm == NM_PROG ? ns_cap_end[sel[3]] : ns_cap_end[2 + ns.cm], x = NS_CAP_X + 6 * (ns.ct / NS_TYPE_MS);
    int typing = x < end;
    if (!typing) x = end;
    ns_vis(ns_cv, typing);                                                      /* the cover: shrinks to the right */
    if (typing) { int32_t s = ((NS_COVER_R - x) << 10) / (NS_COVER_R - NS_CAP_X); ns_mat(ns_cv, s, (NS_COVER_R << 10) - NS_COVER_R * s, 0); }
    ns_vis(ns_cu, typing || !(ns.t / 500 & 1));                                 /* the cursor: solid, then blinks */
    ns_mat(ns_cu, 1024, x << 10, 0);

    if (ns.key[0] < 0 || ns.key[NC_N] != pg[0] + 2 * pg[1] + 4 * pg[2] || ns.key[0] != v[0] || ns.key[1] != v[1] || ns.key[2] != v[2] ||
        ns.key[3] != v[3] || ns.key[4] != v[4] || ns.key[5] != v[5] || ns.key[6] != v[6]) {   /* a setting changed: the scope */
        for (int i = 0; i < NC_N; i++) ns.key[i] = v[i];
        ns.key[NC_N] = pg[0] + 2 * pg[1] + 4 * pg[2];
        ns_scope(v, sel, pg, hz, rt);
    }
    /* the ship: its place, its drift or bob, hyperspace; the flare from the meter */
    int32_t dy = 0;
    if (sel[2] == 0) dy = (ns_sin((int32_t)(ns.ts % 1500 * 8192 / 1500)) + 16384) >> 15;          /* LOW: 1 px, 1.5 s */
    else if (sel[2] == 1) dy = (5 * ns_sin((int32_t)(ns.ts % 1111 * 8192 / 1111)) + 32768) >> 16;  /* MED: 2 px, 0.9 Hz */
    else dy = (9 * ns_sin((int32_t)(ns.ts % 333 * 8192 / 333)) + 32768) >> 16;                     /* HIGH: 4 px, 3 Hz */
    if (sel[3] != NT_HIFI) dy &= ~1;                                            /* STD, LOFI: a 2 px grid */
    int32_t sy = NS_SHIP_Y + dy, sx = ns.sx, gone = 0, hot = 0;
    if (ns.hj) {
        ns.hj -= dt;
        if (ns.hj < 0) ns.hj = 0;
        int32_t el = NS_JUMP_MS - ns.hj, q;
        if (el < 250) { q = el * 131; sx += (NS_X1 - NS_SHIP_W - 1 - sx) * (q * q >> 15) >> 15; hot = 1; }   /* out */
        else if (el < 350) gone = 1;                                                                     /* gone */
        else if (ns.hj) { q = 32768 - (el - 350) * 131; sx = ns.xs + 4 + ((ns.sx - ns.xs - 4) * (32768 - (q * q >> 15)) >> 15); hot = 1; }   /* back */
    }
    int32_t m = *op1_fx_meter(pos), f;                                          /* the engines flare on a rise */
    ns.ms += (m - ns.ms) >> 3;
    f = (m - ns.ms) * 4;
    ns.fl -= dt * 131;
    if (f > ns.fl) ns.fl = f > 32767 ? 32767 : f;
    if (ns.fl < 0) ns.fl = 0;
    if (hot) ns.fl = 32767;
    ns_vis(ns_sh, !gone);
    ns_mat(ns_sh, 1024, sx << 10, sy << 10);
    for (int i = 0; i < 6; i++) {                                               /* stars drift past */
        int32_t off = (int32_t)(ns.ts % 292000u * ns_star[i][2] / 1000 % 292), x = 14 + (ns_star[i][0] - off + 292) % 292;
        ns_vis(ns_sr[i], gone || x <= sx - 3 || x >= sx + NS_SHIP_W + 2);
        int32_t l = ns_star[i][3];                                              /* LOW: steady; MED: a slow twinkle; */
        if (sel[2] == 1) l = l * (45 + ((55 * (ns_sin((int32_t)(ns.ts % 1400 * 8192 / 1400) + ns_star[i][4]) + 32768)) >> 16)) / 100;
        else if (sel[2] == 2) {                                                 /* HIGH: a fast flicker, ~5..7 Hz each */
            int32_t p = ns_sin((int32_t)(ns.ts % 1000 * (20480 + 1638 * (int32_t)i) / 1000) + ns_star[i][4]);   /* 2.5 .. 3.3 turns: 5 .. 7 Hz */
            l = l * (25 + 75 * (p < 0 ? -p : p) / 32768) / 100;
        }
        if (ns_sf[i]) ns_sf[i][0] = ns_sf[i][1] = ns_sf[i][2] = (uint8_t)(l > 255 ? 255 : l);
        ns_mat(ns_sr[i], 1024, x << 10, ns_star[i][1] << 10);
    }
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    for (int i = 0; i < ns.nl; i++) {                                           /* the scope */
        op1_pt p0 = { ns_l[i].x0, ns_l[i].y0 }, p1 = { ns_l[i].x1, ns_l[i].y1 };
        op1_raster_line(r, &p0, &p1, ns_l[i].w, ns_l[i].c);
    }
    if (gone) { ns_draw(r, ns.xs << 10, ((sy + 6) << 10) + 512, (NS_X1 + 4) << 10, ((sy + 6) << 10) + 512, 2048, C_WHITE, 256); return; }
    /* the exhaust: the bass over the mid its volume, treble its heat, the mix its brightness; ship back to the gate */
    int32_t b = nt_lut(nt_rt[sel[0]], v[NC_BASS]), heat = v[NC_TREB];
    if (hz) b = (b * 22050 + hz / 2) / hz;
    b = b * 4096 / rt;                                            /* the bass decay over the mid's, Q12 */
    int ne = b < 3686 ? 1 : b < 6144 ? 2 : 3;                                   /* x0.9, x1.5 */
    int32_t wc = b >= 8192 ? 3072 : ne == 1 ? 1024 : 2048, len = (sx - ns.xs - 2) << 10;   /* x2.0: 3 px */
    if (len < 4096) len = 4096;
    if (sel[2] == 2) len -= (len * 3277 >> 15) * (32768 + ns_sin((int32_t)(ns.ts % 1000 * 49152 / 1000))) >> 16;   /* HIGH: 6 Hz, 10 % */
    uint32_t hc = ns_rgb(C_GREEN, C_WHITE, heat > 13107 ? (heat - 13107) * 32768 / 19661 : 0);
    int32_t lift = (ns.fl + 8192) >> 14, mixl = 154 + (v[NC_MIX] * 102 >> 15);  /* a flare runs the heat down the plume */
    static const uint16_t seg[4] = { 256, 179, 115, 56 }, frac[3] = { 32768, 22938, 26214 };
    for (int e = 0; e < ne; e++) {
        int32_t ln = (len >> 5) * frac[e] >> 10, y = ((sy + ns_nozzle[e]) << 10) + 512;
        uint32_t c = ns_rgb(0x005334u, e ? C_GREEN : hc, heat + 8192 > 32767 ? 32767 : heat + 8192);
        for (int s = 0; s < 4; s++) {
            int32_t x0 = (sx << 10) - ln * s / 4, x1 = (sx << 10) - ln * (s + 1) / 4, k2 = s - lift < 0 ? 0 : s - lift;
            if (x0 - x1 >= 1024) ns_draw(r, x1, y, x0, y, e ? (wc > 1024 ? wc - 1024 : 1024) : wc, c, seg[k2] * mixl >> 8);
        }
    }
    if (ns.fl > 9830) ns_draw(r, ((sx - 1) << 10) + 512, (sy << 10) - 512, ((sx - 1) << 10) + 512, ((sy + 11) << 10) + 512, 2048, hc, 256);
}

const op1_screen_t SCREEN_nostromo = { ns_init, ns_update };
