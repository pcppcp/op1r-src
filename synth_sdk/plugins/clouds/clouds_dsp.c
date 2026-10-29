/* Fixed-point port of Mutable Instruments Clouds, see clouds_dsp.h.
 * Structure and names follow the original files (granular_processor.cc, granular_sample_player.h, grain.h,
 * audio_buffer.h, looping_sample_player.h, fx/diffuser.h, fx/reverb.h, fx/pitch_shifter.h, stmlib Svf).
 * Original code (c) 2014 Emilie Gillet, MIT license. */
#include "clouds_dsp.h"
#include "clouds_tables.h"
#ifdef CLOUDS_DEBUG
#include <stdio.h>
#include <stdlib.h>
static FILE *dbgf[4];
static void dump(int k, const int32_t *o, int n){ if(!dbgf[k]){char nm[32]; sprintf(nm,"/tmp/dbg/p%d.raw",k); dbgf[k]=fopen(nm,"wb");} for(int i=0;i<n;i++){short v=(short)o[i]; fwrite(&v,2,1,dbgf[k]);} }
#define DUMP(k,o,n) dump(k,o,n)
#else
#define DUMP(k,o,n)
#endif

#define Q15 32768
#define CROSSFADE 256                 /* kCrossFadeSize */
#define INTERP_TAIL 8                 /* kInterpolationTail */
#define LOOP_XFADE 64                 /* kCrossfadeDuration */
#define SR_SCALE(x) ((int32_t)(((int64_t)(x) * CLOUDS_SR) / 32000))
enum { Q_LOW, Q_MEDIUM, Q_HIGH };

static inline int32_t clip16(int32_t x) { return x < -32768 ? -32768 : (x > 32767 ? 32767 : x); }
/* (a * b) >> 15 as the 64-bit product would give it, in 32-bit multiplies (the Blackfin has no 32 x 32 -> 64
 * instruction: a 64-bit product is a libgcc call). a = hi * 2^15 + lo (0 <= lo < 2^15), so the result is
 * hi * b + ((lo * b) >> 15) exactly; the operand within +-65536 goes in b so lo * b fits. Bit-exact with the
 * 64-bit version whenever the result fits 32 bits (as every call here does). */
static inline int32_t mulq15(int32_t a, int32_t b)
{
    if (((uint32_t)b + 65536u) > 131072u) { const int32_t t = a; a = b; b = t; }
    return (int32_t)((uint32_t)(a >> 15) * (uint32_t)b + (uint32_t)(((a & 32767) * b) >> 15));
}
/* (d * f) >> 16 exactly, f 0..65535, |d| < 2^31: the same split */
static inline int32_t mulf16(int32_t d, int32_t f) { return (d >> 16) * f + (int32_t)(((uint32_t)(d & 65535) * (uint32_t)f) >> 16); }
#ifdef CLOUDS_CHECK                   /* host: the split must equal the 64-bit result (clouds_compare CLOUDS_FLAGS) */
#include <assert.h>
static inline int32_t mulq15_c(int32_t a, int32_t b) { int32_t r = mulq15(a, b); assert(r == (int32_t)(((int64_t)a * b) >> 15)); return r; }
static inline int32_t mulf16_c(int32_t d, int32_t f) { int32_t r = mulf16(d, f); assert(r == (int32_t)(((int64_t)d * f) >> 16)); return r; }
#define mulq15 mulq15_c
#define mulf16 mulf16_c
#endif
/* same result as mulq15 when |a * b| < 2^31 (16-bit-ish operands): one 32-bit multiply on the Blackfin */
static inline int32_t mul16(int32_t a, int32_t b) { return (a * b) >> 15; }
static inline uint32_t rnd(clouds_t *c) { c->rng = c->rng * 1664525u + 1013904223u; return c->rng; }

/* SoftLimit(x), x and result Q15 (x clamped to +-8) */
static inline int32_t soft_limit(int32_t x)
{
    if (x <= -8 * Q15) return cl_soft_limit[0];
    if (x >= 8 * Q15 - 1) return cl_soft_limit[4096];
    int32_t i = (x + 8 * Q15) >> 7, f = (x + 8 * Q15) & 127;          /* 1/256 steps = 128 Q15 units */
    return cl_soft_limit[i] + (((cl_soft_limit[i + 1] - cl_soft_limit[i]) * f) >> 7);   /* Q15 step * 0..127: 32 bit */
}
static inline int16_t soft_convert(int32_t x) { return (int16_t)clip16(soft_limit(x >> 1)); }

/* Interpolate(table, x, size): x Q15 (0..1), table of size+1 Q15 points */
static inline int32_t interp_q15(const int16_t *t, int32_t x, int size_log2)
{
    if (x < 0) x = 0;
    int32_t pos = x << size_log2;                                       /* Q15 index */
    int32_t i = pos >> 15, f = pos & 32767;
    if (i >= (1 << size_log2)) return t[1 << size_log2];
    return t[i] + (((t[i + 1] - t[i]) * f) >> 15);
}

/* SemitonesToRatio, semitones*256 -> ratio Q30 (64 bit: up to +-48 semitones), 1/16 semitone steps
 * interpolated linearly in between */
static uint64_t semitones_to_ratio_q30(int32_t st256)
{
    int32_t s16 = st256 >> 4, fr = st256 & 15;                          /* 1/16 semitones + 1/256 rest */
    int32_t oct = s16 >= 0 ? s16 / 192 : -((-s16 + 191) / 192);
    int32_t k = s16 - oct * 192;
    uint64_t r = cl_pow2_semi16[k];
    if (fr) r += ((uint64_t)(cl_pow2_semi16[k + 1] - cl_pow2_semi16[k]) * fr) >> 4;
    return oct >= 0 ? r << oct : r >> -oct;
}
static inline uint32_t semitones_to_ratio(int32_t st256) { return (uint32_t)(semitones_to_ratio_q30(st256) >> 14); }  /* Q16 */

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, b = (uint64_t)1 << 62;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return (uint32_t)r;
}

/* ---------------- stmlib Svf (FREQUENCY_FAST), coefficients Q24 ---------------- */
static void svf_set(clouds_svf_t *s, int32_t f_q30, int32_t r_q24)    /* f = cutoff / sample rate */
{
    /* tan approximation: f * (pi + f^2 (a + b f^2)), a = 0.326 pi^3, b = 0.1823 pi^5 */
    int64_t f = f_q30, f2 = (f * f) >> 30;                               /* Q30, f <= 0.5 */
    int64_t poly = 52707179 + ((f2 * (169584874 + ((f2 * 935957068) >> 30))) >> 30);   /* Q24 */
    int64_t g = (f * poly) >> 30;                                        /* Q24 */
    s->g = (int32_t)g; s->r = r_q24;
    int64_t den = (1LL << 24) + (((int64_t)r_q24 * g) >> 24) + ((g * g) >> 24);
    s->h = (int32_t)((1LL << 48) / den);
}
/* state and internal signals Q23 (8 extra bits: integrators with tiny g would drift in Q15) */
static inline int32_t svf_hp(clouds_svf_t *s, int32_t in, int32_t *lp_out)
{
    int64_t hp = ((int64_t)(in * 256 - (int32_t)(((int64_t)(s->r + s->g) * s->s1) >> 24) - s->s2) * s->h) >> 24;
    int32_t gh = (int32_t)((s->g * hp) >> 24);
    int32_t bp = gh + s->s1;
    s->s1 = gh + bp;
    int32_t gb = (int32_t)(((int64_t)s->g * bp) >> 24);
    int32_t lp = gb + s->s2;
    s->s2 = gb + lp;
    if (lp_out) *lp_out = lp >> 8;
    return (int32_t)(hp >> 8);
}

/* ---------------- AudioBuffer (16 bit) ---------------- */
static void buf_write(clouds_t *c, int32_t x)                          /* AudioBuffer::Write, x Q15 */
{
    c->s16[c->head] = (int16_t)clip16(x);
    if (c->head < INTERP_TAIL) c->s16[c->head + c->size] = c->s16[c->head];
    if (++c->head >= c->size) c->head = 0;
}
static void buf_write_fade(clouds_t *c, const int32_t *in, int n, int write)
{
    if (!write) {
        while (n-- && c->crossfade < CROSSFADE)
            c->tail[c->crossfade++] = (int16_t)clip16(mul16(*in++, 32767));
    } else if (!c->crossfade && c->head >= INTERP_TAIL && c->head < c->size - n) {
        while (n--) c->s16[c->head++] = (int16_t)clip16(mul16(*in++, 32767));
    } else {
        while (n--) {
            int32_t x = *in++;
            if (c->crossfade) {
                --c->crossfade;
                int32_t t = c->tail[CROSSFADE - c->crossfade];
                x += ((t - x) * c->crossfade) >> 8;                     /* 17 bit * 0..256: 32 bit */
            }
            buf_write(c, x);
        }
    }
}
static inline int32_t buf_read(const clouds_t *c, int quality, int32_t i, uint32_t frac)   /* -> Q15 */
{
    if (i >= c->size) i -= c->size;
    const int16_t *s = c->s16 + i;
    if (quality == Q_LOW) return s[0];
    if (quality == Q_MEDIUM) return s[0] + (((s[1] - s[0]) * (int32_t)(frac >> 1)) >> 15);
    /* Hermite (Laurent de Soras), t Q16 */
    int32_t xm1 = s[0], x0 = s[1], x1 = s[2], x2 = s[3], t = (int32_t)frac;   /* frac 0..65535 */
    int32_t cc = (x1 - xm1) >> 1, v = x0 - x1, w = cc + v, a = w + v + ((x2 - x0) >> 1), b = w + a;   /* |.| < 2^21 */
    return mulf16(mulf16(mulf16(a, t) - b, t) + cc, t) + x0;               /* exact 32-bit (d * t) >> 16 */
}

/* ---------------- Grain ---------------- */
#define ENV_END INT32_MIN
static void grain_envelope(clouds_grain_t *g, int32_t *dst, int n)
{
    int use_lut = g->env_smoothness != 0;
    uint32_t ph = g->env_phase;
    while (n--) {
        uint32_t x = ph >= 0x40000000u ? 0x80000000u - ph : ph;        /* triangle, Q30 */
        int32_t gain = (int32_t)(x >> 15);                              /* Q15 */
        if (use_lut) {
            if (g->quality == Q_HIGH) {
                int32_t i = (int32_t)(x >> 18), f = (int32_t)((x >> 3) & 32767);   /* x * 4096 */
                int32_t w = i >= 4096 ? cl_lut_window[4096] : cl_lut_window[i] + (((cl_lut_window[i + 1] - cl_lut_window[i]) * f) >> 15);
                gain += mul16(g->env_smoothness, w - gain);
            }
        } else if (g->quality >= Q_MEDIUM) {
            gain = gain >= g->env_slope_limit ? 32767 : (gain * g->env_slope) >> 12;
        }
        ph += g->env_increment;
        if (ph >= 0x80000000u) { *dst = ENV_END; break; }
        *dst++ = gain;
    }
    g->env_phase = ph;
}
/* grain_envelope + the read, fused into one loop per grain and specialised per envelope branch (same arithmetic,
 * sample for sample as grain_envelope then the read: no env[] pass, buffer, size and grain state in registers).
 * GRAIN_LOOP(ENV): ENV turns gain (the triangle, Q15) and x (Q30) into the envelope value. */
#define GRAIN_LOOP(...)                                                                                      \
    for (i = 0; i < n; i++) {                                                                                \
        const uint32_t x = eph >= 0x40000000u ? 0x80000000u - eph : eph;                                     \
        int32_t gain = (int32_t)(x >> 15);                                                                   \
        __VA_ARGS__;                                                                                       \
        eph += einc;                                                                                         \
        if (eph >= 0x80000000u) { g->active = 0; break; }                                                    \
        int32_t k = first + (ph >> 16);                                                                      \
        if (k >= size) k -= size;                                                                            \
        const int16_t *r = s16 + k;                                                                          \
        const int32_t s = r[0] + (((r[1] - r[0]) * (int32_t)(((uint32_t)ph & 65535) >> 1)) >> 15);           \
        out[i] += mul16(mul16(s, gain), 23170);                                    /* mono: lut_sin(0.5) */  \
        ph += inc;                                                                                           \
    }
static void grain_overlap_add(clouds_t *c, clouds_grain_t *g, int32_t *out, int32_t *env, int n)
{
    if (!g->active) return;
    while (g->pre_delay && n) { out++; n--; g->pre_delay--; }
#ifdef CLOUDS_LINEAR_READ
    if (g->quality != Q_LOW) {                                     /* the reads are all Q_MEDIUM (linear) */
        const int16_t *const s16 = c->s16;
        const int32_t size = c->size, first = g->first_sample, inc = g->phase_increment;
        const int32_t sm = g->env_smoothness, slope = g->env_slope, lim = g->env_slope_limit;
        const uint32_t einc = g->env_increment;
        uint32_t eph = g->env_phase;
        int32_t ph = g->phase;
        int i;
        if (sm && g->quality == Q_HIGH) {
            GRAIN_LOOP({
                const int32_t j = (int32_t)(x >> 18), f = (int32_t)((x >> 3) & 32767);
                const int32_t w = j >= 4096 ? cl_lut_window[4096] : cl_lut_window[j] + (((cl_lut_window[j + 1] - cl_lut_window[j]) * f) >> 15);
                gain += mul16(sm, w - gain);
            })
        } else if (sm) {
            GRAIN_LOOP((void)0)
        } else {
            GRAIN_LOOP(gain = gain >= lim ? 32767 : (gain * slope) >> 12)
        }
        g->phase = ph;
        g->env_phase = eph;
        return;
    }
#endif
    grain_envelope(g, env, n);
    int32_t ph = g->phase;
    for (int i = 0; i < n; i++) {
        int32_t gain = env[i];
        if (gain == ENV_END) { g->active = 0; break; }
#ifdef CLOUDS_LINEAR_READ
        int32_t s = buf_read(c, g->quality == Q_LOW ? Q_LOW : Q_MEDIUM, g->first_sample + (ph >> 16), (uint32_t)ph & 65535);
#else
        int32_t s = buf_read(c, g->quality, g->first_sample + (ph >> 16), (uint32_t)ph & 65535);
#endif
        out[i] += mul16(mul16(s, gain), 23170);                        /* mono: lut_sin(pan 0.5) */
        ph += g->phase_increment;
    }
    g->phase = ph;
}

/* ---------------- GranularSamplePlayer ---------------- */
/* what schedule_grain works out from the parameters alone: the same for every grain of a block, so it is done once
 * per block (grain_tpl) and each grain start only copies it (schedule_grain). Same values as computing it per grain. */
static void grain_tpl(const clouds_t *c, const clouds_params_t *p, int32_t window_shape, clouds_tpl_t *t)
{
    /* grain size (samples, Q8): Interpolate(lut_grain_size, size, 256), rescaled to CLOUDS_SR */
    int32_t pos = p->size << 8, i = pos >> 15, f = pos & 32767;
    int64_t gs = cl_lut_grain_size[i] + (i < 256 ? (((int64_t)(cl_lut_grain_size[i + 1] - cl_lut_grain_size[i]) * f) >> 15) : 0);
    gs = SR_SCALE(gs);
    uint32_t ratio = semitones_to_ratio(p->pitch), inv = semitones_to_ratio(-p->pitch);
    int64_t bs = (int64_t)c->size << 8;
    if (ratio > 65536) { int64_t lim = ((bs >> 2) * inv) >> 16; if (gs > lim) gs = lim; }
    int64_t eaten_play = (gs * ratio) >> 16;
    int64_t available = bs - eaten_play - gs;
    t->gs = gs;
    t->width = (int32_t)(gs >> 8) & ~1;
    t->offset = (int32_t)((((available * p->position) >> 15) + eaten_play) >> 8);
    t->inc = (int32_t)ratio;
    /* Grain::Start's envelope set-up */
    t->env_increment = t->width > 0 ? (uint32_t)(0x80000000u / (uint32_t)t->width) : 0x80000000u;
    if (window_shape >= 16384) { t->env_smoothness = (window_shape - 16384) * 2; t->env_slope = 0; }
    else { t->env_smoothness = 0; t->env_slope = (int32_t)((2048LL * 32768) / (window_shape + 328)); }
    t->env_slope_limit = t->env_slope ? (int32_t)((32767u << 12) / (uint32_t)t->env_slope) : 32767;
}
static void schedule_grain(clouds_t *c, clouds_grain_t *g, const clouds_tpl_t *t, int32_t pre_delay,
                           int32_t head, int quality, int pan_rnd)
{
    if (pan_rnd) (void)rnd(c);                                          /* pan: Random::GetFloat() */
    int32_t start = head - t->offset, first = start + c->size;
    if (first < 0 || first >= 2 * c->size) first %= c->size;            /* = (start + size) % size */
    else if (first >= c->size) first -= c->size;
    g->pre_delay = pre_delay;
    g->width = t->width;
    g->first_sample = first;
    g->phase_increment = t->inc;
    g->phase = 0;
    g->env_phase = 0;
    g->env_increment = t->env_increment;
    g->env_smoothness = t->env_smoothness;
    g->env_slope = t->env_slope;
    g->env_slope_limit = t->env_slope_limit;
    g->active = 1;
    g->quality = (uint8_t)quality;
    c->hint_q8 += ((int32_t)t->gs - c->hint_q8) / 10;                   /* ONE_POLE 0.1 (gs fits 32 bits) */
}

static void __attribute__((noinline)) play_granular(clouds_t *c, const clouds_params_t *p, int32_t *out, int n)
{
    /* DENSITY / TEXTURE meta parameters (GranularProcessor::ProcessGranular) */
    int deterministic = p->density < 16384;
    int32_t overlap = 0;                                                 /* Q15 */
    if (p->density >= 17367) overlap = (int32_t)(((int64_t)(p->density - 17367) * 69468) >> 15);       /* *2.12 */
    else if (p->density <= 15401) overlap = (int32_t)(((int64_t)(15401 - p->density) * 69468) >> 15);
    int32_t window_shape = p->texture < 24576 ? (int32_t)(((int64_t)p->texture * 43680) >> 15) : 32768;  /* *1.333 */
    if (window_shape > 32768) window_shape = 32768;

    int32_t o3 = mulq15(mulq15(overlap, overlap), overlap);
    /* max * overlap^3, Q16 - with the original's 40 even if the grain pool is smaller (CLOUDS_MAX_GRAINS),
     * so DENSITY keeps its mapping; a smaller pool only limits the very dense end */
    int64_t target_q16 = ((int64_t)40 * o3) << 1;
    int64_t p_q32 = -1, space_q8 = INT64_MAX;
    if (target_q16 > 0) {
        p_q32 = (target_q16 << 24) / c->hint_q8;                         /* target / hint * 2^32 */
        space_q8 = ((int64_t)c->hint_q8 << 16) / target_q16;              /* hint / target, Q8 */
    }
    if (deterministic) p_q32 = -1; else c->phasor = -1000 * 256;

    /* the grain set-up depends on SIZE, PITCH, POSITION and the window only: reuse it while they don't move */
    if (!c->tpl_ok || c->tpl_key[0] != p->size || c->tpl_key[1] != p->pitch || c->tpl_key[2] != p->position
        || c->tpl_key[3] != window_shape) {
        grain_tpl(c, p, window_shape, &c->tpl);
        c->tpl_key[0] = p->size; c->tpl_key[1] = p->pitch; c->tpl_key[2] = p->position; c->tpl_key[3] = window_shape;
        c->tpl_ok = 1;
    }
    const clouds_tpl_t tpl = c->tpl;
    int avail[CLOUDS_MAX_GRAINS], navail = 0;
    for (int i = 0; i < CLOUDS_MAX_GRAINS; i++) if (!c->grains[i].active) avail[navail++] = i;
    int seedable = navail;

    int trigger = p->trigger;
    /* the 64-bit seeding tests, decided once per block (same results: w is 32 bit, phasor and the grain count do not
     * change their comparison partners within the block) */
    const int prob_on = p_q32 >= 0 && target_q16 > c->num_grains_q16, prob_all = p_q32 > 0xFFFFFFFFLL;
    const uint32_t prob_thr = prob_all || !prob_on ? 0 : (uint32_t)p_q32;
    const int32_t space32 = space_q8 > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)space_q8;
    const int space_never = space_q8 > 0x7FFFFFFF;
    for (int t = 0; t < n; t++) {
        c->phasor = (int32_t)((uint32_t)c->phasor + 256u);
        uint32_t w = rnd(c);
        int seed_prob = prob_on && (prob_all || w < prob_thr);
        int seed_det = !space_never && c->phasor >= space32;
        if (seedable && (seed_prob || seed_det || trigger)) {
            --navail;
            --seedable;
            clouds_grain_t *g = &c->grains[avail[navail]];
            int quality = navail < (3 * CLOUDS_MAX_GRAINS / 4) ? Q_MEDIUM : Q_HIGH;
            schedule_grain(c, g, &tpl, t, c->head - n + t, quality, 1);
            c->phasor = 0;
            trigger = 0;
        }
    }
    for (int t = 0; t < n; t++) out[t] = 0;
    int32_t env[CLOUDS_MAX_BLOCK];
    for (int i = 0; i < CLOUDS_MAX_GRAINS; i++) grain_overlap_add(c, &c->grains[i], out, env, n);

    /* normalization */
    int32_t active = CLOUDS_MAX_GRAINS - navail, err = (active << 16) - c->num_grains_q16;
    c->num_grains_q16 += err > 0 ? (int32_t)(((int64_t)err * 29491) >> 15) : (int32_t)(((int64_t)err * 6554) >> 15);
    int32_t gn = 32767;
    if (c->num_grains_q16 > (2 << 16)) {
        uint32_t sq = isqrt64((uint64_t)(c->num_grains_q16 - 65536) << 16);   /* sqrt(n-1) * 2^16 */
        gn = (int32_t)(((int64_t)32768 << 16) / sq);
    }
    int32_t wg = 32768 + 2 * window_shape; if (wg > 65536) wg = 65536;   /* window_gain, Q15 */
    gn = (int32_t)(((int64_t)gn * (32768 + mulq15(wg - 32768, overlap))) >> 15);
    for (int t = 0; t < n; t++) {
        c->gain_norm_q15 += mul16(gn - c->gain_norm_q15, 328);            /* ONE_POLE 0.01 */
        out[t] = mulq15(out[t], c->gain_norm_q15);
    }
}

/* ---------------- LoopingSamplePlayer (Q12 positions) ---------------- */
static void play_looping(clouds_t *c, const clouds_params_t *p, int32_t *out, int n)
{
    int32_t max_delay = c->size - LOOP_XFADE;
    if (!p->freeze) {
        int64_t target = ((int64_t)p->position * max_delay) << 13;            /* Q28 samples */
        for (int k = n - 1, t = 0; t < n; t++, k--) {
#ifdef CLOUDS_EMULATE_F32_DELAY
            /* host verification only: the original keeps current_delay_ in float32, where the glide stalls
             * ~20 samples short of the target (increments < 1/2 ulp are lost). The OP-1 build converges. */
            { float cur = (float)c->lp_current_delay / 268435456.0f, tgt = (float)target / 268435456.0f;
              cur = cur + 0.00005f * (tgt - cur); c->lp_current_delay = (int64_t)((double)cur * 268435456.0); }
            int32_t d = (c->head - 4 - k + c->size) * 4096 - (int32_t)((float)c->lp_current_delay / 268435456.0f * 4096.0f);
#else
            c->lp_current_delay += ((target - c->lp_current_delay) * 107374) >> 31;   /* 0.00005 */
            int32_t d = (c->head - 4 - k + c->size) * 4096 - (int32_t)(c->lp_current_delay >> 16);
#endif
            out[t] = buf_read(c, Q_HIGH, d >> 12, ((uint32_t)d << 4) & 65535);
        }
        c->lp_phase = 0;
        return;
    }
    int32_t loop_point = (int32_t)((((int64_t)p->position * max_delay) * 15 / 16) >> 3) + (LOOP_XFADE << 12);
    int64_t d3 = (int64_t)p->size * p->size * p->size;                       /* Q45 */
    int64_t frac = 351843720888LL + d3 / 100 * 99;                            /* 0.01 + 0.99 d^3, Q45 */
    int32_t loop_duration = (int32_t)((frac * max_delay) >> 33);              /* Q12 */
    if (loop_point + loop_duration >= (max_delay << 12)) loop_point = (max_delay << 12) - loop_duration;
    int32_t inc = (int32_t)(semitones_to_ratio(p->pitch) >> 4);         /* Q12 */
    int32_t base = (c->head - 4 + c->size) << 12;
    for (int t = 0; t < n; t++) {
        if (c->lp_phase >= c->lp_loop_duration || c->lp_phase == 0) {
            if (c->lp_phase >= c->lp_loop_duration) c->lp_loop_reset = c->lp_loop_duration;
            if (c->lp_loop_reset >= c->lp_loop_duration) c->lp_loop_reset = c->lp_loop_duration;
            c->lp_tail_start = c->lp_loop_duration - c->lp_loop_reset + c->lp_loop_point;
            c->lp_phase = 0;
            c->lp_tail_duration = inc < 4096 ? (LOOP_XFADE * inc) : (LOOP_XFADE << 12);
            c->lp_inv_tail = c->lp_tail_duration ? (int32_t)((1LL << 42) / c->lp_tail_duration) : 0;  /* Q30 */
            c->lp_loop_point = loop_point;
            c->lp_loop_duration = loop_duration;
#ifdef CLOUDS_DEBUG
            if (getenv("LDBG")) fprintf(stderr, "P reset head=%d lp=%.3f ld=%.3f ts=%.3f lr=%.3f td=%.3f\n", (int)c->head, c->lp_loop_point / 4096.0, c->lp_loop_duration / 4096.0, c->lp_tail_start / 4096.0, c->lp_loop_reset / 4096.0, c->lp_tail_duration / 4096.0);
#endif
        }
        c->lp_phase += inc;
        int32_t gain = 32768;
        if (c->lp_tail_duration) {
            gain = (int32_t)(((int64_t)c->lp_phase * c->lp_inv_tail) >> 27);    /* Q15 */
            if (gain > 32768) gain = 32768;
            if (gain < 0) gain = 0;
        }
        int32_t pos = base - (c->lp_loop_duration - c->lp_phase + c->lp_loop_point);
        int32_t x = mulq15(buf_read(c, Q_HIGH, pos >> 12, ((uint32_t)pos << 4) & 65535), gain);
        if (gain != 32768) {
            int32_t pos2 = base - (c->lp_tail_start - c->lp_phase);
            x += mulq15(buf_read(c, Q_HIGH, pos2 >> 12, ((uint32_t)pos2 << 4) & 65535), 32768 - gain);
        }
        out[t] = x;
    }
}

/* ---------------- Diffuser (FxEngine<2048, 32 bit>, left chain) ---------------- */
static void __attribute__((noinline)) diffuser(clouds_t *c, int32_t *io, int n, int32_t amount)
{
    /* lines 126, 180, 269, 444 at bases 0, 127, 308, 578 (diff_len + 1 each); write pointer in a register.
     * kap = 0.625 = 5/8 exactly: mulq15(a, +-20480) = (+-a * 5) >> 3 (same result, no multiply split) */
    int32_t *const db = c->diff_buf;
    int32_t wp = c->diff_wp;
#define K58(a) (((a) * 5) >> 3)
    for (int t = 0; t < n; t++) {
        if (--wp < 0) wp += 2048;
        int32_t acc = io[t], prev;
        prev = db[(wp + 125) & 2047]; acc += K58(prev); db[wp & 2047] = acc;         acc = K58(-acc) + prev;
        prev = db[(wp + 306) & 2047]; acc += K58(prev); db[(wp + 127) & 2047] = acc; acc = K58(-acc) + prev;
        prev = db[(wp + 576) & 2047]; acc += K58(prev); db[(wp + 308) & 2047] = acc; acc = K58(-acc) + prev;
        prev = db[(wp + 1021) & 2047]; acc += K58(prev); db[(wp + 578) & 2047] = acc; acc = K58(-acc) + prev;
        io[t] += mulq15(amount, acc - io[t]);
    }
#undef K58
    c->diff_wp = wp;
}

/* ---------------- PitchShifter (FxEngine<4096, 16 bit>, left line, len 2047) ---------------- */
static inline int32_t ps_interp(clouds_t *c, int64_t off_q16)
{
    int32_t i = (int32_t)(off_q16 >> 16), f = (int32_t)(off_q16 & 65535);
    int32_t a = c->ps_buf[(c->ps_wp + i) & 4095], b = c->ps_buf[(c->ps_wp + i + 1) & 4095];
    return a + mulf16(b - a, f);
}
static void pitch_shifter(clouds_t *c, int32_t *io, int n, uint64_t ratio_q30, int32_t size_q15)
{
    int64_t s3 = (int64_t)size_q15 * size_q15 * size_q15;                      /* Q45 */
    int32_t target = (128 << 16) + (int32_t)(((2047 - 128) * s3) >> 29);         /* Q16 samples */
    c->ps_size_q16 += (int32_t)(((int64_t)(target - c->ps_size_q16) * 1638) >> 15);  /* ONE_POLE 0.05 */
    int64_t delta = (((1LL << 30) - (int64_t)ratio_q30) * 262144) / c->ps_size_q16;  /* (1 - ratio)/size, 2^32 */
    for (int t = 0; t < n; t++) {
        if (--c->ps_wp < 0) c->ps_wp += 4096;
        c->ps_phase += (uint32_t)delta;
        uint32_t ph = c->ps_phase;
        int32_t tri = (int32_t)((ph >= 0x80000000u ? ~ph : ph) >> 16);          /* Q15 */
        int64_t phase = ((uint64_t)ph * (uint32_t)c->ps_size_q16) >> 32;        /* Q16 */
        int64_t half = phase + (c->ps_size_q16 >> 1);
        if (half >= c->ps_size_q16) half -= c->ps_size_q16;
        c->ps_buf[c->ps_wp & 4095] = (int16_t)clip16(io[t]);
        io[t] = mulq15(ps_interp(c, phase), tri) + mulq15(ps_interp(c, half), 32767 - tri);
    }
}

/* ---------------- Reverb (FxEngine<16384, 12 bit>) ---------------- */
static inline int32_t rv_dec(int16_t v) { return (int32_t)v * 8; }          /* Q12 -> Q15 */
static inline int16_t rv_enc(int32_t v) { return (int16_t)clip16(v / 8); }       /* truncate like the float cast */
/* The reverb with its state in registers and the line offsets as constants: lines of 113, 162, 241, 399, 1653, 2038,
 * 3411, 1913, 1663, 4782 samples (FxEngine), line k at RBk (each line + 1 after the previous), its tail at
 * RTk = RBk + length - 1. A buffer word v is v * 8 in Q15 (rv_dec), so mulq15(v * 8, k) = (v * k) >> 12 exactly,
 * one 16 x 16 multiply. */
enum { RL0 = 113, RL1 = 162, RL2 = 241, RL3 = 399, RL4 = 1653, RL5 = 2038, RL6 = 3411, RL7 = 1913, RL8 = 1663,
       RB0 = 0, RB1 = RB0 + RL0 + 1, RB2 = RB1 + RL1 + 1, RB3 = RB2 + RL2 + 1, RB4 = RB3 + RL3 + 1, RB5 = RB4 + RL4 + 1,
       RB6 = RB5 + RL5 + 1, RB7 = RB6 + RL6 + 1, RB8 = RB7 + RL7 + 1, RB9 = RB8 + RL8 + 1,
       RT0 = RB0 + RL0 - 1, RT1 = RB1 + RL1 - 1, RT2 = RB2 + RL2 - 1, RT3 = RB3 + RL3 - 1, RT4 = RB4 + RL4 - 1,
       RT5 = RB5 + RL5 - 1, RT6 = RB6 + RL6 - 1, RT7 = RB7 + RL7 - 1, RT8 = RB8 + RL8 - 1 };
typedef char rv_layout_ok[RB1 == 114 && RB5 == 2573 && RB9 == 11602 && RT0 == 112 && RT8 == 11600 ? 1 : -1];
#define RW(o) rb[(wp + (o)) & 16383]
#define VK(v, k) (((int32_t)(v) * (k)) >> 12)                       /* mulq15(rv_dec(v), k), |k| < 2^15 */
static inline int32_t rv_interp_at(const int16_t *rb, int32_t wp, int32_t base, int32_t off_q16)
{
    const int32_t k = base + (off_q16 >> 16), f = off_q16 & 65535;
    const int32_t a = rv_dec(RW(k)), d = rv_dec(RW(k + 1)) - a;
    return a + (((d * (f >> 8)) + ((d * (f & 255)) >> 8)) >> 8);
}
static void reverb(clouds_t *c, int32_t *io, int n, int32_t amount, int32_t time, int32_t lp)
{
    const int32_t kap = 22938, gain = 6554;                               /* diffusion 0.7, input 0.2 */
    int16_t *const rb = c->rev_buf;
    int32_t wp = c->rev_wp, lp1 = c->rev_lp1, lp2 = c->rev_lp2, l0 = c->rev_lfo[0], l1 = c->rev_lfo[1];
    for (int t = 0; t < n; t++) {
        if (--wp < 0) wp += 16384;
        if ((wp & 31) == 0) {                                               /* LFOs: CosineOscillator::Next */
            for (int k = 0; k < 2; k++) {
                int32_t y = c->lfo_y0[k];
                c->lfo_y0[k] = (int32_t)(((int64_t)c->lfo_iir[k] * y) >> 29) - c->lfo_y1[k];
                c->lfo_y1[k] = y;
                c->rev_lfo[k] = (y + (1 << 28)) >> 14;                     /* temp + 0.5, Q29 -> Q15 */
            }
            l0 = c->rev_lfo[0]; l1 = c->rev_lfo[1];
        }
        int32_t acc, prev, apout, wet;
        int16_t v;
        /* smear AP1 inside the loop */
        acc = rv_interp_at(rb, wp, RB0, (10 << 16) + 120 * l0);
        RW(RB0 + 100) = rv_enc(acc);
        acc = mulq15(2 * io[t], gain);                                     /* Read(l + r, gain) */
        v = RW(RT0); prev = rv_dec(v); acc += VK(v, kap); RW(RB0) = rv_enc(acc); acc = mulq15(acc, -kap) + prev;
        v = RW(RT1); prev = rv_dec(v); acc += VK(v, kap); RW(RB1) = rv_enc(acc); acc = mulq15(acc, -kap) + prev;
        v = RW(RT2); prev = rv_dec(v); acc += VK(v, kap); RW(RB2) = rv_enc(acc); acc = mulq15(acc, -kap) + prev;
        v = RW(RT3); prev = rv_dec(v); acc += VK(v, kap); RW(RB3) = rv_enc(acc); acc = mulq15(acc, -kap) + prev;
        apout = acc;
        /* left branch */
        acc = apout + mulq15(rv_interp_at(rb, wp, RB9, (4680 << 16) + 200 * l1), time);
        lp1 += mulq15(lp, acc - lp1); acc = lp1;
        v = RW(RT4); prev = rv_dec(v); acc += VK(v, -kap); RW(RB4) = rv_enc(acc); acc = mulq15(acc, kap) + prev;
        v = RW(RT5); prev = rv_dec(v); acc += VK(v, kap);  RW(RB5) = rv_enc(acc); acc = mulq15(acc, -kap) + prev;
        RW(RB6) = rv_enc(acc); acc *= 2;                                    /* Write(del1, 2.0) */
        wet = acc;
        io[t] += mulq15(wet - io[t], amount);
        /* right branch (feeds del2, output unused in mono) */
        acc = apout + VK(RW(RT6), time);
        lp2 += mulq15(lp, acc - lp2); acc = lp2;
        v = RW(RT7); prev = rv_dec(v); acc += VK(v, kap);  RW(RB7) = rv_enc(acc); acc = mulq15(acc, -kap) + prev;
        v = RW(RT8); prev = rv_dec(v); acc += VK(v, -kap); RW(RB8) = rv_enc(acc); acc = mulq15(acc, kap) + prev;
        RW(RB9) = rv_enc(acc);
    }
    c->rev_wp = wp; c->rev_lp1 = lp1; c->rev_lp2 = lp2;
}


/* ---------------- GranularProcessor ---------------- */
void clouds_init(clouds_t *c, void *mem, int mode)
{
    uint8_t *m = (uint8_t *)mem;
    /* init runs in the audio callback on the first render: clear with 32-bit stores (byte and halfword loops
     * took about 86k Blackfin packets). may_alias because the stores go through struct and buffer types alike. */
    typedef uint32_t __attribute__((may_alias)) word_t;
    _Static_assert(sizeof(*c) % 4 == 0 && CLOUDS_BUFFER_BYTES % 4 == 0, "clouds clear sizes");
    for (size_t i = 0; i < sizeof(*c) / 4; i++) ((word_t *)c)[i] = 0;
    c->mode = c->prev_mode = mode;
    c->s16 = (int16_t *)m;
    c->size = CLOUDS_BUFFER_BYTES / 2 - INTERP_TAIL;
    for (int i = 0; i < CLOUDS_BUFFER_BYTES / 4; i++) ((word_t *)m)[i] = 0;
    m += CLOUDS_BUFFER_BYTES;
    c->diff_buf = (int32_t *)m; m += 2048 * 4;
    c->rev_buf = (int16_t *)m;  m += 16384 * 2;
    c->ps_buf = (int16_t *)m;
    {   /* diff_buf (2048 words), rev_buf (16384 halfwords) and ps_buf (4096 halfwords) are contiguous */
        word_t *z = (word_t *)c->diff_buf;
        for (int i = 0; i < 2048 + 8192 + 2048; i++) z[i] = 0;
    }
    /* reverb LFOs: stmlib CosineOscillator::InitApproximate(f * 32), f = 0.5 Hz, 0.3 Hz (Q29) */
    for (int k = 0; k < 2; k++) {
        int64_t f = ((int64_t)(k ? 3 : 5) * 32 << 29) / (10LL * CLOUDS_SR);        /* frequency * 32 / SR */
        int64_t fr = (1LL << 27) - f;                                            /* 0.25 - f (f < 0.25) */
        c->lfo_iir[k] = (int32_t)((16 * fr * ((1LL << 29) - 2 * fr)) >> 29);
        c->lfo_y1[k] = c->lfo_iir[k] / 4;                                        /* initial amplitude */
        c->lfo_y0[k] = 1 << 28;                                                  /* 0.5 */
        c->rev_lfo[k] = (c->lfo_y1[k] + (1 << 28)) >> 14;                       /* value() before Next */
    }
    c->hint_q8 = 1024 << 8;
    c->gain_norm_q15 = 32767;
    c->ps_size_q16 = 2047 << 16;
    c->rng = 0x21;                                                         /* stmlib Random seed */
    svf_set(&c->fb_filter, 0, 1 << 24);
    c->lp_filter = c->hp_filter = c->fb_filter;
}

void clouds_process(clouds_t *c, const clouds_params_t *p, const int16_t *input, int16_t *output, int n)
{
    int32_t in[CLOUDS_MAX_BLOCK], out[CLOUDS_MAX_BLOCK];
    if (n > CLOUDS_MAX_BLOCK) n = CLOUDS_MAX_BLOCK;
    for (int i = 0; i < n; i++) in[i] = input[i];

    /* feedback with high-pass (freeze_lp: ONE_POLE 0.0005 per block) */
    c->freeze_lp_q30 += (int32_t)(((int64_t)((p->freeze ? (1 << 30) : 0) - c->freeze_lp_q30) * 16777) >> 25);
    int32_t fbk = p->feedback;
    int64_t cutoff_hz_q15 = 20LL * 32768 + 100LL * mulq15(fbk, fbk);              /* Hz, Q15 */
#ifdef CLOUDS_SKIP_IDLE
    if (fbk)                                                                       /* the filter only runs with feedback */
#endif
    svf_set(&c->fb_filter, (int32_t)((cutoff_hz_q15 << 15) / CLOUDS_SR), 1 << 24);
#ifdef CLOUDS_SKIP_IDLE
    if (fbk)
#endif
    for (int i = 0; i < n; i++) c->fb[i] = svf_hp(&c->fb_filter, c->fb[i], 0);
    int32_t fb_gain = mulq15(fbk, 32768 - (c->freeze_lp_q30 >> 15));
    if (fb_gain)
        for (int i = 0; i < n; i++) {
            int32_t x = mulq15(mulq15(fb_gain, 45875), c->fb[i]) + in[i];          /* 1.4 */
            in[i] += mulq15(fb_gain, soft_limit(x) - in[i]);
        }

    /* recording + playback */
    buf_write_fade(c, in, n, !p->freeze);
    if (c->mode == CLOUDS_GRANULAR) play_granular(c, p, out, n);
    else play_looping(c, p, out, n);

    DUMP(0, out, n);
    /* diffusion, pitch shifting, filters */
    int32_t diffusion = c->mode == CLOUDS_GRANULAR ? (p->texture > 24576 ? (p->texture - 24576) * 4 : 0) : p->density;
#ifdef CLOUDS_SKIP_IDLE
    if (diffusion)
#endif
    diffuser(c, out, n, diffusion);
    if (c->mode == CLOUDS_LOOPING) {
        if (!p->freeze) pitch_shifter(c, out, n, semitones_to_ratio_q30(p->pitch), p->size);
        DUMP(1, out, n);
        int32_t tex = p->texture;
        int32_t lp_st = tex < 16384 ? (int32_t)(((int64_t)(tex - 16384) * 216 * 256) >> 15) : 0;   /* semitones*256 */
        int32_t hp_st = (int32_t)(((int64_t)(tex < 16384 ? -16384 : tex - 32768) * 216 * 256) >> 15);
        /* lp_cutoff = 0.5 * ratio, hp_cutoff = 0.25 * ratio, Q30 (ratio Q16), clamped to 0.499 */
        int64_t lpc = (int64_t)semitones_to_ratio(lp_st) << 13, hpc = (int64_t)semitones_to_ratio(hp_st) << 12;
        if (lpc > 535797310) lpc = 535797310;
        if (hpc > 535797310) hpc = 535797310;
        /* lpq = 1 + 3 (1 - fb) (0.5 - lp_cutoff), Q24; r = 1 / lpq */
        int64_t lpq_q24 = (1LL << 24) + ((3LL * (32768 - fbk) * (((1LL << 29) - lpc) >> 15)) >> 6);
        svf_set(&c->lp_filter, (int32_t)lpc, (int32_t)((1LL << 48) / lpq_q24));
        svf_set(&c->hp_filter, (int32_t)hpc, 1 << 24);
        for (int i = 0; i < n; i++) {
            int32_t lp;
            svf_hp(&c->lp_filter, out[i], &lp);
            out[i] = svf_hp(&c->hp_filter, lp, 0);
        }
    }
    DUMP(2, out, n);
    for (int i = 0; i < n; i++) c->fb[i] = out[i];                                 /* fed back, no reverb */

    /* reverb */
    int32_t frz = c->freeze_lp_q30 >> 15;
    int32_t amt = mulq15(p->reverb, 31130) + mulq15(mulq15(fbk, 65536 - fbk), frz);  /* 0.95 r + fb (2 - fb) frz */
    if (amt > 32767) amt = 32767;
    if (amt < 0) amt = 0;
#ifdef CLOUDS_SKIP_IDLE
    if (amt)
#endif
    reverb(c, out, n, mulq15(amt, 17695), 11469 + mulq15(amt, 20644), 19661 + mulq15(fbk, 12124));

    /* dry / wet (ParameterInterpolator) */
    int32_t dw = c->dry_wet_q15, inc = (p->dry_wet - dw) / n;
    for (int i = 0; i < n; i++) {
        dw += inc;
        int32_t fin = interp_q15(cl_lut_xfade_in, dw, 4), fout = interp_q15(cl_lut_xfade_out, dw, 4);
        int32_t x = mul16(input[i], fout) + mulq15(mulq15(out[i], 39322), fin);        /* post gain 1.2 */
        output[i] = soft_convert(x);
    }
    c->dry_wet_q15 = p->dry_wet;
}
