#pragma GCC optimize ("no-tree-loop-distribute-patterns") /* no implicit libc calls */
/* perc: drum voice for the PO-32 patch model, integer only.
 * GPL-3.0-or-later with the OP1R firmware permission. Signal path and parameter maps follow libpo32's documented
 * reference renderer (MIT, Copyright (c) 2026 Eric Lewis): oscillator with pitch modulation (decay / sine / noise),
 * filtered noise with exp / linear / modulated envelope, mix, waveshaper, peaking EQ, level. Independent integer
 * implementation: the biquads are state-variable filters with the same bilinear responses, the envelopes use
 * log-domain attacks and Q30 decay multipliers, and coefficients are worked out once per hit. */
#include "op1_plugin.h"
#include "perc_dsp.h"
#include "perc_tables.h"

#define ONE30 (1 << 30)
#define ONE24 (1 << 24)
#define ONE16 65536

/* (a * b) >> 30, floor */
static inline int32_t mul30(int32_t a, int32_t b)
{
#if defined(__bfin__)
    int32_t r;
    __asm__("A1 = %1.L * %2.L (FU);\n\t"
            "A1 = A1 >> 16;\n\t"
            "A1 += %1.H * %2.L (M), A0 = %1.H * %2.H;\n\t"
            "A1 += %2.H * %1.L (M);\n\t"
            "A0 = A0 << 1;\n\t"
            "A1 = A1 >>> 14;\n\t"
            "%0 = (A0 += A1);"
            : "=d"(r) : "d"(a), "d"(b) : "A0", "A1", "ASTAT");
    return r;
#else
    return (int32_t)(((int64_t)a * b) >> 30);
#endif
}
/* Q24 x Q24 for values below 128 in magnitude (2^-18 resolution) */
#define SHL(x, n) ((int32_t)((uint32_t)(x) << (n)))   /* left shift of a signed value */
static inline int32_t clampi(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline int32_t iabs(int32_t x) { return x < 0 ? -x : x; }
static inline uint32_t uabs(int32_t x) { uint32_t m = (uint32_t)(x >> 31); return ((uint32_t)x ^ m) - m; }

/* ---- exp2 / log2 ---- */
static inline uint32_t ex2f(uint32_t f)
{
    uint32_t i = f >> 8, r = f & 255u, a = PERC_EXP2[i];
    return a + (((PERC_EXP2[i + 1] - a) * r) >> 8);
}
uint32_t perc_pow2(int32_t x, int fb)
{
    int32_t s = (x >> 16) + fb - 30;
    uint32_t m = ex2f((uint32_t)x & 0xffffu);
    if (s >= 2) return 0xffffffffu;
    if (s == 1) return m << 1;
    if (s <= -31) return 0;
    return m >> (uint32_t)(-s);
}
int32_t perc_log2(uint32_t v)
{
    int32_t e = 31;
    if (!v) return -(64 << 16);
    while (!(v & 0x80000000u)) { v <<= 1; e--; }
    uint32_t i = (v >> 23) & 255u, r = (v >> 15) & 255u;
    int32_t a = PERC_LOG2[i];
    return e * 65536 + a + (((PERC_LOG2[i + 1] - a) * (int32_t)r) >> 8);
}
static inline int32_t q16mul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }

/* sin of a quarter-turn fraction u (Q16, 0..65536 = 0..pi/2), Q30 */
static int32_t qsin(int32_t u)
{
    if (u <= 0) return 0;
    if (u >= 65536) return ONE30;
    uint32_t i = (uint32_t)u >> 8, r = (uint32_t)u & 255u;
    int32_t a = (int32_t)PERC_QSIN[i];
    return a + (int32_t)((((int64_t)PERC_QSIN[i + 1] - a) * r) >> 8);
}

/* Q32 fraction 2^(-e) for small e given as e * 2^32 (Taylor to the cube), returned Q30 */
static int32_t exp2neg_small(uint32_t e_q32)
{
    uint64_t y = ((uint64_t)e_q32 * PERC_LN2_Q32) >> 32;           /* e ln2, Q32 */
    uint64_t y2 = (y * y) >> 32, y3 = (y2 * y) >> 32;
    int64_t c = ((int64_t)1 << 32) - (int64_t)y + (int64_t)(y2 >> 1) - (int64_t)(y3 / 6);
    if (c < 0) c = 0;
    return (int32_t)(c >> 2);
}

/* ---- parameter maps (x: Q16, 0..65536) ---- */
static inline int32_t xq(uint16_t p) { return (int32_t)p + (p >> 15); }
static int32_t hz_l2(int32_t x) { return PERC_L2_20 + q16mul(x, PERC_L2_1000); }               /* 20 * 1000^x */
static int32_t dcy_l2(int32_t x) { return PERC_L2_0P01 + q16mul(x, PERC_L2_1000); }            /* 0.01 * 1000^x s */
static int32_t atk_l2(int32_t x)                                                                 /* x 0.0398 10^2.4x s */
{
    return perc_log2((uint32_t)x) - (16 << 16) + PERC_L2_0P0398 + q16mul(q16mul(x, 157286), PERC_L2_10);
}
/* decay multiplier for a fall of 1000x over 2^t_l2 seconds */
static int32_t decay_c(int32_t t_l2)
{
    if (t_l2 < -(11 << 16)) t_l2 = -(11 << 16);                                                  /* >= 0.5 ms */
    return exp2neg_small(perc_pow2(PERC_L2_L2_1000 - t_l2 - PERC_L2_FS, 32));
}
/* attack: samples and log2 step (Q24 per sample) for a rise of 1000x over 2^t_l2 seconds */
static void attack(int32_t t_l2, uint32_t *n, int32_t *step)
{
    *n = perc_pow2(t_l2 + PERC_L2_FS, 0);
    *step = *n ? (int32_t)perc_pow2(PERC_L2_L2_1000 - t_l2 - PERC_L2_FS, 24) : 0;
}
/* tan(pi f / fs) for f = 2^l2 Hz (capped at 0.45 fs), Q27 */
static int32_t tan_q27(int32_t l2)
{
    if (l2 > PERC_MAX_HZ_L2) l2 = PERC_MAX_HZ_L2;
    int32_t u = (int32_t)perc_pow2(l2 - PERC_L2_FS + ONE16, 16);                                  /* 2 f / fs, Q16 */
    int32_t s = qsin(u), c = qsin(65536 - u);
    if (c < 1) c = 1;
    return (int32_t)(((int64_t)s << 27) / c);
}
/* state-variable filter coefficients for g (Q27) and damping k (Q26) */
static void svf(int32_t g, int32_t k, int32_t *a1, int32_t *a2, int32_t *a3)
{
    int64_t gg = ((int64_t)g * (g + 2 * (int64_t)k)) >> 24;                                       /* g (g + k), Q30 */
    int64_t d = (int64_t)ONE30 + gg;
    *a1 = (int32_t)(((int64_t)1 << 60) / d);
    *a2 = (int32_t)(((int64_t)g * *a1) >> 27);
    *a3 = (int32_t)(((int64_t)g * *a2) >> 27);
}
/* velocity gain x 10^(37 (x - 1) / 20), x = clamp(1 - (127 - vel) / 63 sens), Q30 */
static int32_t vel_gain(int vel, int32_t sens)
{
    int32_t inv = ((127 - vel) << 16) / 63;
    int32_t x = clampi(ONE16 - q16mul(inv, sens), 0, ONE16);
    return (int32_t)(((int64_t)x * perc_pow2(q16mul(x - ONE16, PERC_VEL_K), 30)) >> 16);
}
static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, b = (uint64_t)1 << 62;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return (uint32_t)r;
}

static void to_control_rate(perc_prep_t *o);
void perc_prepare(perc_prep_t *o, const uint16_t *pw, const perc_macro_t *m, int vel)
{
    int32_t x[PERC_NPARAM];
    for (int i = 0; i < PERC_NPARAM; i++) x[i] = xq(pw[i]);
    if (vel < 1) vel = 1; else if (vel > 127) vel = 127;
    int32_t punch = clampi(m->punch, -ONE16, ONE16);

    for (unsigned i = 0; i < sizeof(*o); i++) ((uint8_t *)o)[i] = 0;
    o->wave = x[P_OSCWAVE] < 21845 ? 0 : x[P_OSCWAVE] < 43691 ? 1 : 2;
    o->mod_mode = x[P_MODMODE] < 21845 ? 0 : x[P_MODMODE] < 43691 ? 1 : 2;
    o->nf_mode = x[P_NFILMOD] < 21845 ? 0 : x[P_NFILMOD] < 43691 ? 1 : 2;
    o->nenv_mode = x[P_NENVMOD] > 43691 ? 2 : x[P_NENVMOD] > 21845 ? 1 : 0;

    /* tone */
    o->osc_l2 = hz_l2(x[P_OSCFREQ]) + m->pitch - PERC_L2_FS + (32 << 16);
    if (x[P_OSCATK] > 0) attack(atk_l2(x[P_OSCATK]) - 3 * punch, &o->osc_atk_n, &o->osc_atk_step);
    o->osc_dcy_c = decay_c(dcy_l2(x[P_OSCDCY]) + m->length);

    /* pitch modulation: semitones = b |b| limit, b = 2x - 1; velocity and punch scale it */
    {
        int32_t b = 2 * x[P_MODAMT] - ONE16;
        int32_t semis = q16mul(b, iabs(b)) * (o->mod_mode == 0 ? 96 : 48);
        semis = (int32_t)(((int64_t)semis * vel_gain(vel, x[P_MODVEL])) >> 30);
        semis = q16mul(semis, (int32_t)perc_pow2(2 * punch, 16));
        o->mod_scale = semis / 12;
        o->mod_on = o->mod_scale != 0;
        int32_t r = x[P_MODRATE];
        if (o->mod_mode == 0) {
            if (r <= 0) o->mod_dcy_c = ONE30;
            else {
                int32_t ratio = (int32_t)(((int64_t)3 * r << 16) / (67502 - r));            /* 3r / (1.03 - r), Q16 */
                o->mod_dcy_c = exp2neg_small((uint32_t)(((int64_t)ratio * PERC_MODK_Q32) >> 16));
            }
        } else if (r > 0) {
            int32_t l2hz = o->mod_mode == 1
                ? PERC_L2_2000 + perc_log2((uint32_t)r) - (16 << 16) + q16mul(q16mul(r - ONE16, 229376), PERC_L2_10)
                : PERC_L2_20000 + perc_log2((uint32_t)r) - (16 << 16) + q16mul(4 * (r - ONE16), PERC_L2_10);
            if (o->mod_mode == 1) o->mod_inc = perc_pow2(l2hz - PERC_L2_FS + (32 << 16), 0);
            else {
                int32_t w_l2 = PERC_L2_2PI + l2hz - PERC_L2_FS;                             /* 2 pi f / fs */
                int32_t e16 = (int32_t)perc_pow2(w_l2 + PERC_L2_LN2INV, 16);
                o->mod_alpha = e16 > (20 << 16) ? ONE30 : ONE30 - (int32_t)perc_pow2(-e16, 30);
            }
        }
    }

    /* noise filter: LP / BP / HP, Q 0.1 .. 10000 (kept below 2000 for headroom) */
    {
        int32_t g = tan_q27(hz_l2(x[P_NFILFRQ]) + m->tone);
        int32_t k = (int32_t)perc_pow2(PERC_L2_10 - q16mul(5 * x[P_NFILQ], PERC_L2_10), 26);
        if (k < 33554) k = 33554;                                                            /* Q <= 2000 */
        o->nf_k = k;
        svf(g, k, &o->nf_a1, &o->nf_a2, &o->nf_a3);
    }

    /* noise envelope */
    {
        int32_t al2 = x[P_NENVATK] > 0 ? atk_l2(x[P_NENVATK]) - 3 * punch : -(64 << 16);
        int32_t dl2 = dcy_l2(x[P_NENVDCY]) + m->length;
        if (o->nenv_mode == 1) {                                                            /* linear, times x 2/3 */
            int32_t two_thirds = PERC_L2_2_3;
            uint32_t na = x[P_NENVATK] > 0 ? perc_pow2(al2 + two_thirds + PERC_L2_FS, 0) : 0;
            uint32_t nd = perc_pow2(dl2 + two_thirds + PERC_L2_FS, 0);
            if (nd < 1) nd = 1;
            o->lin_atk_n = na; o->lin_end_n = na + nd;
            o->lin_up = na ? ONE30 / (int32_t)na : 0;
            o->lin_dn = ONE30 / (int32_t)(nd > 0x7fffffff ? 0x7fffffff : nd);
        } else {
            if (x[P_NENVATK] > 0) attack(al2, &o->n_atk_n, &o->n_atk_step);
            o->n_dcy_c = decay_c(dl2);
            if (o->nenv_mode == 2 && x[P_NENVDCY] > 6) {                                    /* damped cosine */
                int32_t xd = x[P_NENVDCY];
                int32_t per = perc_log2((uint32_t)xd) - (16 << 16) - PERC_L2_FS + q16mul(xd, PERC_L2_10000);
                int32_t corr_l2 = 0;
                if (per < PERC_L2_1P5MS) {                                                  /* < 1.5 ms: x 8, gain */
                    per += 3 << 16;
                    corr_l2 = (-ONE16 - per) / 2;                                           /* sqrt(0.5 / period) */
                } else per += ONE16;
                o->ring_inc = perc_pow2(-per - PERC_L2_FS + (32 << 16), 0);
                o->ring_corr = (int32_t)perc_pow2(corr_l2, 26);
                if (o->ring_corr < 0) o->ring_corr = 0x7fffffff;
            }
        }
    }

    /* mix (25 dB tilt) and velocity */
    {
        int32_t pan = 2 * x[P_MIX] - ONE16, og = ONE30, ng = ONE30;
        if (pan >= 0) og = (int32_t)(((int64_t)(ONE16 - pan) * perc_pow2(-q16mul(pan, PERC_MIX_K), 30)) >> 16);
        else ng = (int32_t)(((int64_t)(ONE16 + pan) * perc_pow2(q16mul(pan, PERC_MIX_K), 30)) >> 16);
        o->osc_g = mul30(og, vel_gain(vel, x[P_OSCVEL]));
        o->noise_g = mul30(ng, vel_gain(vel, x[P_NVEL]));
    }

    /* distortion: drive = 200 a^3 */
    {
        int32_t a = clampi(x[P_DIST] + m->dist, 0, ONE16);
        if (a > 655) {
            int64_t d32 = 200 * ((((int64_t)a * a) * a) >> 16);                            /* 200 a^3, Q32 */
            if (d32 <= 0) {}
            else if (d32 < ((int64_t)1 << 32)) {
                o->dist_mode = 1;
                o->d_drive = (int32_t)(d32 >> 2);                                           /* Q30 */
                o->d_gain = (int32_t)(ONE30 - (d32 >> 2) / 30);                             /* Q30 */
                uint64_t r16 = (uint64_t)(((d32 + ((int64_t)3 << 32)) << 16) / d32);        /* (drive + 3) / drive, Q16 */
                uint64_t cl = (((uint64_t)ONE16 + isqrt64(r16 << 16)) << 8) / 3;            /* Q24 */
                o->d_clip = cl > (uint64_t)127 * ONE24 ? 127 * ONE24 : (int32_t)cl;
                o->d_dc = (int32_t)((-2 * (d32 >> 8) - 9 * (int64_t)ONE24) / 27);          /* Q24 */
            } else {
                int64_t drive = d32 >> 16;                                                  /* Q16 */
                o->dist_mode = 2;
                o->d_drive = (int32_t)(drive * 64);                                         /* Q22 */
                o->d_clip = (int32_t)(((int64_t)4 << 40) / (3 * drive));                    /* |x| where x drive = 4/3, Q24 */
                o->d_gain = (int32_t)(((((int64_t)13107 << 16) / (drive - 31638)) + 38011) * 16384);       /* Q30 */
            }
        }
    }

    /* EQ: peaking, Q 1, -40 .. +40 dB; boosts lower the level by half the boost */
    int32_t level_l2 = 0;
    {
        int32_t db = (x[P_EQGAIN] - 32768) * 80;                                            /* Q16 */
        if (iabs(db) > 32768) {
            int32_t al2 = (int32_t)(((int64_t)db * PERC_L2_10 / 40) >> 16);                 /* log2 A */
            int32_t A = (int32_t)perc_pow2(al2, 26), iA = (int32_t)perc_pow2(-al2, 26);
            o->use_eq = 1;
            o->eq_m1 = A - iA;
            svf(tan_q27(hz_l2(x[P_EQFREQ]) + m->tone), iA, &o->eq_a1, &o->eq_a2, &o->eq_a3);
            if (db > 0) level_l2 -= al2;
        }
    }

    /* level: dB = 60x - 49 - 1/x */
    if (x[P_LEVEL] >= 7) {
        int32_t db = 60 * x[P_LEVEL] - 49 * ONE16 - (int32_t)(((int64_t)1 << 32) / x[P_LEVEL]) + m->level_db;
        if (db > -120 * ONE16) {
            uint32_t l = perc_pow2(level_l2 + q16mul(db, PERC_DB_K), 28);
            o->level = l > 0x7fffffffu ? 0x7fffffff : (int32_t)l;
        }
    }
    to_control_rate(o);
}

/* per-sample decay c -> per control step c^PERC_CR */
static int32_t pow_cr(int32_t c) { int32_t c2 = mul30(c, c); return mul30(c2, c2); }
/* a per-sample step x PERC_CR, at most 2^30 (a ramp that short is over within one control step) */
static int32_t sat_cr(int32_t x) { return x >= ONE30 / PERC_CR ? ONE30 : x * PERC_CR; }
static void to_control_rate(perc_prep_t *o)
{
    o->osc_dcy_c = pow_cr(o->osc_dcy_c); o->n_dcy_c = pow_cr(o->n_dcy_c);
    if (o->mod_mode == 0) o->mod_dcy_c = pow_cr(o->mod_dcy_c);
    o->osc_atk_step = sat_cr(o->osc_atk_step); o->n_atk_step = sat_cr(o->n_atk_step);
    o->lin_up = sat_cr(o->lin_up); o->lin_dn = sat_cr(o->lin_dn);
    o->ring_inc *= PERC_CR; o->mod_inc *= PERC_CR;          /* noise pitch modulation stays per sample */
}

void perc_trigger(perc_voice_t *v)
{
    const perc_prep_t *p = &v->p;
    v->n = 0; v->phase = 0; v->mod_phase = 0; v->ring_phase = 0; v->rnd = 123456789u;
    v->osc_env = ONE30; v->osc_lg = -PERC_L2_1000 * 256;
    v->nenv = ONE30; v->n_lg = -PERC_L2_1000 * 256; v->lin = 0;
    v->mod_env = ONE30; v->mod_state = 0;
    if (p->mod_mode == 2) { v->rnd = v->rnd * 1103515245u + 12345u; v->mod_state = (int32_t)(v->rnd & 0x7fffffffu) - ONE30; }
    v->f1 = v->f2 = v->e1 = v->e2 = 0;
    v->ao = v->an = 0; v->osync = v->nsync = 0; v->ramp = 0; v->tail = 0;
    v->active = 1;
}

static inline int32_t cos_q30(uint32_t ph) { return SHL(OP1_SINE_Q[((ph >> 19) + 2048) & 8191], 15); }

static int32_t distort(const perc_prep_t *p, int32_t s)
{
    if (p->dist_mode == 1) {
        int32_t lim = p->d_clip < 8 * ONE24 ? p->d_clip : 8 * ONE24;                         /* range of the fixed point */
        int32_t xc = clampi(s - ONE24 / 3, -lim, lim);
        int32_t sq = SHL(mul30(SHL(xc, 2), SHL(xc, 2)), 2);
        int32_t t = mul30(iabs(xc) - sq, p->d_drive) + ONE24;
        int32_t y = clampi(mul30(xc, t) - (p->d_dc >> 6), -(1 << 25) + 1, (1 << 25) - 1);      /* Q18 */
        return SHL(mul30(y, p->d_gain), 6);
    }
    /* strong: table of 0.5 (f(xd - 1/3) - f(-xd - 1/3)) over |xd| = |x drive| in 0 .. 4/3 (384 steps per unit) */
    uint32_t m = uabs(s);
    int32_t y;
    if (m >= (uint32_t)p->d_clip) y = PERC_DIST[512];
    else {
        uint32_t xd = (uint32_t)mul30((int32_t)m, p->d_drive) * 384u;                     /* Q16 x 384: index Q16 */
        uint32_t i = xd >> 16, f = (xd >> 8) & 255u;
        if (i >= 512) y = PERC_DIST[512];
        else y = PERC_DIST[i] + (((PERC_DIST[i + 1] - PERC_DIST[i]) * (int32_t)f) >> 8);
    }
    y = mul30(y, p->d_gain);
    return s < 0 ? -y : y;
}

/* hard voice-length cap: no perc voice outlives a real drum hit, whatever its envelopes say. A legit PO-32 patch can
 * have a multi-second or sustained tail; six of those never ending peg the one DSP core and starve the UI (the device
 * looks frozen). PERC_MAX_N samples (~6 s) is well past any drum hit and bounds the worst case. */
#define PERC_MAX_N (PERC_RATE * 3u)
int perc_render(perc_voice_t *v, int16_t *out, unsigned cnt, int32_t key_l2)
{
    const perc_prep_t *p = &v->p;
    unsigned i, j;
    if (!v->active) { for (i = 0; i < cnt; i++) out[i] = 0; return 0; }
    const int32_t base_l2 = p->osc_l2 + key_l2;
    const uint32_t inc0 = perc_pow2(base_l2, 0);
    for (i = 0; i < cnt; i += PERC_CR) {
        const uint32_t n = v->n; v->n += PERC_CR;
        /* ---- control step ---- */
        int32_t oe;                                                                       /* tone envelope, Q30 */
        if (n < p->osc_atk_n) { oe = (int32_t)perc_pow2(v->osc_lg >> 8, 30); v->osc_lg += p->osc_atk_step; }
        else { if (!v->osync) { v->osc_env = ONE30; v->osync = 1; } oe = v->osc_env; v->osc_env = mul30(v->osc_env, p->osc_dcy_c); }
        uint32_t inc = inc0;                                                              /* pitch */
        if (p->mod_on && p->mod_mode < 2) {                                               /* decay sweep, sine */
            int32_t md;
            if (p->mod_mode == 0) { md = v->mod_env; v->mod_env = mul30(v->mod_env, p->mod_dcy_c); }
            else { md = cos_q30(v->mod_phase); v->mod_phase += p->mod_inc; }
            inc = perc_pow2(base_l2 + mul30(md, p->mod_scale), 0);
        }
        const int fast_mod = p->mod_on && p->mod_mode == 2;                              /* noise: per sample */
        int32_t ne;                                                                       /* noise envelope, Q26 */
        if (p->nenv_mode == 1) {
            if (n < p->lin_atk_n) { ne = v->lin >> 4; v->lin = v->lin > ONE30 - p->lin_up ? ONE30 : v->lin + p->lin_up; }
            else if (n < p->lin_end_n) { if (!v->nsync) { v->lin = ONE30; v->nsync = 1; } ne = v->lin >> 4; v->lin -= p->lin_dn; if (v->lin < 0) v->lin = 0; }
            else ne = 0;
        } else if (n < p->n_atk_n) {
            if (p->nenv_mode == 2) ne = ONE30 >> 4;
            else { ne = (int32_t)perc_pow2(v->n_lg >> 8, 26); v->n_lg += p->n_atk_step; }
        } else {
            if (!v->nsync) { v->nenv = ONE30; v->nsync = 1; }
            int32_t d = v->nenv; v->nenv = mul30(v->nenv, p->n_dcy_c);
            if (p->nenv_mode == 2 && p->ring_inc) {
                uint32_t tri = 4096u - (uabs((int32_t)(v->ring_phase - 0x80000000u)) >> 19);
                v->ring_phase += p->ring_inc;
                ne = mul30(mul30(SHL(OP1_SINE_Q[(tri + 2048u) & 8191], 15), d), p->ring_corr);
            } else ne = d >> 4;
        }
        /* gains for this step (tone Q30, noise Q26), ramped from the last step */
        int32_t ao = mul30(oe, p->osc_g), an = mul30(ne, p->noise_g);
        if (!v->ramp) { v->ao = ao; v->an = an; v->ramp = 1; }
        int32_t go = v->ao, gn = v->an;
        const int32_t dgo = (int32_t)(((int64_t)ao - go) / PERC_CR), dgn = (int32_t)(((int64_t)an - gn) / PERC_CR);
        v->ao = ao; v->an = an;

        /* ---- samples ---- */
        uint32_t ph = v->phase, rnd = v->rnd;
        int32_t f1 = v->f1, f2 = v->f2;
        const int32_t a1 = p->nf_a1, a2 = p->nf_a2, a3 = p->nf_a3;
        for (j = 0; j < PERC_CR; j++) {
            go += dgo; gn += dgn;
            int32_t s = 0;
            if (fast_mod) {                                                               /* as libpo32: the modulation's
                                                                                             random number before the noise's */
                rnd = rnd * 1103515245u + 12345u;
                v->mod_state += mul30(p->mod_alpha, (int32_t)(rnd & 0x7fffffffu) - ONE30 - v->mod_state);
                inc = perc_pow2(base_l2 + mul30(v->mod_state, p->mod_scale), 0);
            }
            if (go) {
                ph += inc;
                int32_t w;                                                                /* Q15 */
                if (p->wave == 0) {
                    uint32_t k = ph >> 19, f = (ph >> 3) & 0xffffu;
                    int32_t x0 = OP1_SINE_Q[k & 8191], x1 = OP1_SINE_Q[(k + 1) & 8191];
                    w = x0 + (((x1 - x0) * (int32_t)f) >> 16);
                } else if (p->wave == 1) w = (int32_t)(uabs((int32_t)(ph - 0x80000000u)) >> 15) - 32768;
                else w = (int32_t)(ph - 0x80000000u) >> 16;
                s = (w * (go >> 15)) >> 6;                                                /* Q15 x Q15 -> Q24 */
            }
            if (gn) {                                                                     /* noise, Q18 in the filter */
                rnd = rnd * 1103515245u + 12345u;
                int32_t x0 = ((int32_t)(rnd & 0x7fffffffu) - ONE30) >> 12;
                int32_t v3 = x0 - f2;
                int32_t v1 = mul30(a1, f1) + mul30(a2, v3);
                int32_t v2 = f2 + mul30(a2, f1) + mul30(a3, v3);
                f1 = clampi(2 * v1 - f1, -(1 << 29), 1 << 29); f2 = clampi(2 * v2 - f2, -(1 << 29), 1 << 29);
                int32_t nz;
                if (p->nf_mode == 0) nz = v2;
                else if (p->nf_mode == 1) nz = SHL(mul30(v1, p->nf_k), 4);
                else nz = x0 - SHL(mul30(v1, p->nf_k), 4) - v2;
                s += SHL(clampi(mul30(nz, gn), -(1 << 20), 1 << 20), 10);                 /* Q14 -> Q24 */
            }
            if (p->dist_mode) s = distort(p, s);
            if (p->use_eq) {
                s = clampi(s, -(1 << 27), 1 << 27);
                int32_t e3 = s - v->e2;
                int32_t e1 = mul30(p->eq_a1, v->e1) + mul30(p->eq_a2, e3);
                int32_t e2 = v->e2 + mul30(p->eq_a2, v->e1) + mul30(p->eq_a3, e3);
                v->e1 = 2 * e1 - v->e1; v->e2 = 2 * e2 - v->e2;
                s = clampi(s + SHL(mul30(e1, p->eq_m1), 4), -(1 << 30), 1 << 30);
            }
            s = mul30(clampi(s, -(1 << 29), 1 << 29), p->level);                          /* Q22 */
            s = clampi(s, -(1 << 22), (1 << 22) - 1);
            out[i + j] = (int16_t)(s >> 7);
        }
        v->phase = ph; v->rnd = rnd; v->f1 = f1; v->f2 = f2;
    }
    /* ended: both envelopes below -100 dB after their attacks (the noise filter runs before the envelope) */
    if (v->n > p->osc_atk_n && v->n > p->n_atk_n && v->n > p->lin_end_n &&
        (v->osc_env < 10737 || !p->osc_g) && (v->nenv < 10737 || p->nenv_mode == 1 || !p->noise_g)) {
        if (!p->use_eq || v->tail >= PERC_RATE * 3 / 20) v->active = 0;               /* the EQ rings out for 150 ms */
        else v->tail += cnt;
    }
    if (v->n >= PERC_MAX_N && v->active) {                                            /* hard cap: fade out and end */
        for (j = 0; j < cnt; j++) out[j] = (int16_t)((int32_t)out[j] * (int32_t)(cnt - 1 - j) / (int32_t)cnt);
        v->active = 0;
    }
    return 1;
}
