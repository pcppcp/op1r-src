/* sampler+ voice (docs/46): see sp_plus.h. Pieces shared in spirit with drum+ (dp_plus.c): envelope, ZDF filter,
 * pitch factor, soft clip; per-block constants, no per-sample division. */
#include "sp_plus.h"
#include "../drumplus/dp_tables.h"

#define Q30   (1 << 30)
#define PSTEP 96                                           /* pitch unit: 1/96 octave */

#define OFF(f) ((uint8_t)__builtin_offsetof(sp_extra, f))
static const struct { uint8_t off; int8_t sgn; int16_t lo, hi; } sp_fields[SP_NFIELDS] = {
    { OFF(attack), 0, 0, 127 }, { OFF(hold), 0, 0, 127 }, { OFF(decay), 0, 0, 127 }, { OFF(sustain), 0, 0, 127 },
    { OFF(release), 0, 0, 127 }, { OFF(curve_exp), 0, 0, 1 }, { OFF(vel_amt), 0, 0, 127 }, { OFF(filter), 0, 0, 3 },
    { OFF(cutoff), 0, 0, 127 }, { OFF(resonance), 0, 0, 127 }, { OFF(fenv), 1, -127, 127 },
    { OFF(vel_cutoff), 1, -127, 127 }, { OFF(pitch_env), 1, -48, 48 }, { OFF(pitch_time), 0, 0, 127 },
    { OFF(loop_mode), 0, 0, 1 }, { OFF(loop_rel), 0, 0, 1 }, { OFF(xfade), 0, 0, 127 }, { OFF(vel_start), 0, 0, 127 },
    { OFF(rand_start), 0, 0, 127 }, { OFF(drive), 0, 0, 127 }, { OFF(sync), 0, 0, 6 },
};
int sp_extra_set(sp_extra *x, int i, int32_t v)
{
    if ((unsigned)i >= SP_NFIELDS) return 0;
    v = v < sp_fields[i].lo ? sp_fields[i].lo : v > sp_fields[i].hi ? sp_fields[i].hi : v;
    ((uint8_t *)x)[sp_fields[i].off] = (uint8_t)v;
    return v;
}
int sp_extra_get(const sp_extra *x, int i)
{
    if ((unsigned)i >= SP_NFIELDS) return 0;
    uint8_t b = ((const uint8_t *)x)[sp_fields[i].off];
    return sp_fields[i].sgn ? (int8_t)b : b;
}
void sp_extra_default(sp_extra *x)
{
    for (unsigned i = 0; i < sizeof *x; i++) ((uint8_t *)x)[i] = 0;
    x->sustain = 127; x->release = 30; x->vel_amt = 64; x->cutoff = 127;
}

/* 0..127 -> 44.1 kHz samples: 0, then 6 ms * 2^(t / 12) (127: ~11 s); the voice counts SP_VOICE_X per sample */
uint32_t sp_time_samples(uint8_t t)
{
    static const uint16_t pow12[12] = { 256, 271, 287, 304, 323, 342, 362, 384, 406, 431, 456, 483 };
    if (!t) return 0;
    if (t > 127) t = 127;
    return (132u * pow12[t % 12] << (t / 12)) >> 7;
}

static uint16_t sp_p2[PSTEP + 1];                          /* 2^(i / 96), Q14 */
static void sp_p2_init(void)
{
    if (sp_p2[0]) return;
    int32_t v = 1 << 30;
    for (int i = 0; i <= PSTEP; i++) {
        sp_p2[i] = (uint16_t)((v + (1 << 15)) >> 16);
        v = (int32_t)(((int64_t)v * 1081522600 + (1 << 29)) >> 30);   /* prepare only */
    }
    sp_p2[PSTEP] = 32768;
}
static int32_t sp_pitch_q12(int32_t u)                     /* u: 1/96 octave << 8, +-4 octaves -> Q12 */
{
    const int32_t lim = 4 * PSTEP * 256;
    if (u > lim) u = lim; else if (u < -lim) u = -lim;
    int32_t oct = 0, w = u + lim;
    while (w >= PSTEP << 8) { w -= PSTEP << 8; oct++; }
    int32_t i = w >> 8, f = w & 255;
    int32_t m = sp_p2[i] + (((sp_p2[i + 1] - sp_p2[i]) * f) >> 8);
    oct -= 4;
    return oct >= 2 ? m << (oct - 2) : m >> (2 - oct);
}

#ifdef __bfin__                     /* fractional 32 x 32 (3 MAC instructions; the low x low product is dropped) */
static inline int32_t mul15(int32_t s, int32_t c) { return __builtin_bfin_mult_fr1x32x32(s, c << 16); }   /* c < 2^15 */
static inline int32_t mul14(int32_t s, int32_t c) { return __builtin_bfin_mult_fr1x32x32(s, c << 16) << 1; }
#else
static inline int32_t mul15(int32_t s, int32_t c) { return (s >> 15) * c + (((s & 0x7FFF) * c) >> 15); }
static inline int32_t mul14(int32_t s, int32_t c) { return (s >> 14) * c + (((s & 0x3FFF) * c) >> 14); }
#endif
static inline int32_t mul12(int32_t s, int32_t c) { return (s >> 12) * c + (((s & 0xFFF) * c) >> 12); }

/* the exponential curve's decrement over SP_ESUB (16) voice samples: per sample 1 - 6.9 / (T + 1) (~-60 dB after T),
 * to the 16th power in Q30, as 1 - that in Q20 (within 1 % up to ~11 s); prepare only */
static uint32_t sp_exp16(uint32_t t)
{
    if (!t) return 0;
    int64_t k = ((int64_t)1 << 30) - 7408817357LL / (int64_t)(t + 1);   /* 6.9 * 2^30 */
    if (k < 0) k = 0;
    for (int i = 0; i < 4; i++) k = (k * k) >> 30;
    return (uint32_t)((((int64_t)1 << 30) - k) >> 10);
}

void sp_plus_prepare(const sp_extra *x, sp_pre *c)
{
    *(volatile uint8_t *)&c->render_active = 0;            /* the audio thread may read c meanwhile */
    sp_p2_init();
    c->t_att = SP_VOICE_X * sp_time_samples(x->attack); c->t_hold = SP_VOICE_X * sp_time_samples(x->hold);
    c->t_dec = SP_VOICE_X * sp_time_samples(x->decay); c->t_rel = SP_VOICE_X * sp_time_samples(x->release);
    c->sus = (int32_t)(((int64_t)x->sustain * Q30) / 127);
    c->a_step = c->t_att ? Q30 / (int32_t)c->t_att : Q30;
    c->d_step = c->t_dec ? (Q30 - c->sus) / (int32_t)c->t_dec : Q30;
    c->r_step = c->t_rel ? Q30 / (int32_t)c->t_rel : Q30;
    c->kd16 = sp_exp16(c->t_dec); c->kr16 = sp_exp16(c->t_rel);
    uint32_t tp = SP_VOICE_X * sp_time_samples(x->pitch_time);
    c->p_start = (int32_t)x->pitch_env * 8388608;                       /* semitones -> 1/96 octave << 20 */
    c->p_step = tp ? c->p_start / (int32_t)tp : c->p_start / SP_BLOCK;   /* 0: gone after one block (p_step * SP_BLOCK fits) */
    int32_t k = 131072 - (int32_t)x->resonance * 127100 / 127;          /* 1 / Q, Q16: 2 .. 0.06 */
    c->f_k = k >> 2 > 32767 ? 32767 : k >> 2;
    for (int i = 0; i < 128; i++) {                                    /* ZDF SVF coefficients per cutoff step */
        int64_t g = dp_filter_g[i], kk = k;
        int64_t den = 65536 + ((g * (g + kk)) >> 16);
        int64_t a1 = ((int64_t)1 << 32) / den, a2 = (g * a1) >> 16, a3 = (g * a2) >> 16;
        c->fa[i][0] = (int32_t)(a1 >> 1); c->fa[i][1] = (int32_t)(a2 >> 1); c->fa[i][2] = (int32_t)(a3 >> 1);
        for (int j = 0; j < 3; j++) if (c->fa[i][j] > 32767) c->fa[i][j] = 32767;   /* Q15 below 1.0 (fractional mult) */
    }
    c->drive_q12 = x->drive ? 4096 + (int32_t)x->drive * 12288 / 127 : 0;
    if (!x->sync) c->sync_q12 = 4096;
    *(volatile uint8_t *)&c->render_active = (uint8_t)(x->filter || x->drive || x->pitch_env || x->loop_mode ||
                                                       x->loop_rel || x->xfade || x->sync);
}

void sp_plus_sync(const sp_extra *x, sp_pre *c, const int16_t *k, int32_t rate, uint32_t samples_per_bar, int32_t ref_inc)
{
    static const uint8_t q4[7] = { 0, 1, 2, 4, 8, 16, 32 };              /* quarter bars: 1/4 .. 8 bars */
    int32_t f = 4096;
    if (x->sync && x->sync <= 6 && samples_per_bar) {
        int64_t len = (int64_t)sp_bound(k[3], 0) - sp_bound(k[0], 0);   /* region, pos units (Q12 samples) */
        int64_t want = (int64_t)samples_per_bar * q4[x->sync] / 4;     /* samples */
        int64_t step = sp_step(ref_inc, rate);                          /* pos units per sample at the reference key */
        if (len > 0 && want > 0 && step > 0) {
            int64_t q = (len << 12) / (want * step);                    /* speed factor, Q12 */
            f = q < 16 ? 16 : q > 65536 ? 65536 : (int32_t)q;           /* 1/256 .. 16 x (short loops need very slow) */
        }
    }
    c->sync_q12 = f;
}

static uint32_t sp_rand(void)
{
    static uint32_t r = 0x2468ACE1;
    r = r * 1664525u + 1013904223u;
    return r >> 8;
}

void sp_plus_note_on(sp_voice *v, sp_xvoice *s, const int16_t *k, const sp_extra *x, const sp_pre *c)
{
    (void)x;
    sp_stock_note_on(v, k);
    s->dir = k[4] >> 14 ? -1 : 1;
    s->pitch_u = c->p_start;
    s->ic1 = s->ic2 = 0;
    s->cut_vel = 0;
    s->wrapped = 0;
    s->pp_left = 0;
}
static inline int32_t sat16(int32_t x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : x; }
int32_t sp_plus_level(const sp_extra *x, int32_t vel_w, int32_t g)
{
    int32_t full = sat16((vel_w * g * 2 + 0x8000) >> 16), none = sat16((32512 * g * 2 + 0x8000) >> 16);
    return none + (((full - none) * x->vel_amt * 258) >> 15);
}
void sp_plus_velocity(sp_voice *v, sp_xvoice *s, const int16_t *k, const sp_extra *x, int velocity)
{
    int vel = velocity < 0 ? 0 : velocity > 127 ? 127 : velocity;
    s->cut_vel = (int16_t)(x->vel_cutoff * (vel - 64) / 63);
    int32_t off = x->vel_start * 64 * (127 - vel) / 127;
    if (x->rand_start) off += (int32_t)(sp_rand() % (uint32_t)(x->rand_start * 64 + 1));
    if (off) {
        int32_t room = (sp_bound(k[3], 0) - sp_bound(k[0], 0)) >> 12;
        if (off > room) off = room;
        v->pos += s->dir > 0 ? off << 12 : -(off << 12);
    }
}

/* the envelope advances in steps of 16 voice samples (stage logic 8 times per block), the output is interpolated
 * linearly between them: stage times are rounded to 16 samples (0.18 ms), the exponential curves become 16-sample
 * chords */
#define SP_ESUB 16
static void sp_env_step(sp_xvoice *s, const sp_extra *x, const sp_pre *c)
{
    switch (s->stage) {
    case 0: s->env += c->a_step * SP_ESUB; if (s->env >= Q30 || s->stage_n >= c->t_att) { s->env = Q30; s->stage = c->t_hold ? 1 : 2; s->stage_n = 0; } break;
    case 1: if (s->stage_n >= c->t_hold) { s->stage = 2; s->stage_n = 0; } break;
    case 2:
        if (!c->t_dec) s->env = c->sus;
        else if (x->curve_exp) s->env -= (int32_t)(((uint32_t)(s->env - c->sus) >> 16) * c->kd16 >> 4);
        else s->env -= c->d_step * SP_ESUB;
        if (s->env <= c->sus + (x->curve_exp ? (Q30 >> 10) : 0)) { s->env = c->sus; s->stage = 3; }
        break;
    case 3: if (!c->sus) s->stage = 5; break;
    case 4:
        if (!c->t_rel) s->env = 0;
        else if (x->curve_exp) s->env -= (int32_t)(((uint32_t)s->env >> 16) * c->kr16 >> 4); else s->env -= c->r_step * SP_ESUB;
        if (s->env <= (x->curve_exp ? (Q30 >> 10) : 0)) { s->env = 0; s->stage = 5; }
        break;
    case 6: s->env -= s->fade_step * SP_ESUB; if (s->env <= 0) { s->env = 0; s->stage = 5; } break;
    default: s->env = 0; break;
    }
    s->stage_n += SP_ESUB;
}
int sp_plus_env(sp_xvoice *s, const sp_extra *x, const sp_pre *c, int32_t *state, int16_t *env, int32_t level, int fresh)
{
    if (*state == 7 || (fresh && *state == 1)) { s->stage = 0; s->stage_n = 0; s->env = 0; *state = 1; }
    else if ((*state == 4 || *state == 5) && s->stage < 4) { s->stage = 4; s->stage_n = 0; }   /* note-off (seen: 5) */
    else if ((*state == 6 || *state == 8) && s->stage != 6) { s->stage = 6; s->fade_step = (s->env >> 8) + 1; }   /* 256 voice samples, 2.9 ms */
    else if (*state == 0) { for (int n = 0; n < SP_BLOCK; n++) env[n] = 0; return 1; }
    if (s->stage == 0 && !c->t_att) { s->env = Q30; s->stage = c->t_hold ? 1 : 2; }
    int32_t e0 = ((s->env >> 15) * level) >> 15;
    for (int k = 0; k < SP_BLOCK; k += SP_ESUB) {
        sp_env_step(s, x, c);
        int32_t e1 = ((s->env >> 15) * level) >> 15, d = e1 - e0, v = e0 << 4;
        for (int n = 0; n < SP_ESUB; n++) { v += d; env[k + n] = (int16_t)(v >> 4); }   /* ends exactly on e1 */
        e0 = e1;
    }
    if (s->stage == 5) {
        if (*state == 6) { *state = 7; return 0; }                       /* retrigger: the next block starts again */
        *state = 0; return 1;
    }
    return 0;
}

static inline int32_t sp_post(sp_xvoice *s, const sp_extra *x, const sp_pre *c, const int32_t *fa, int32_t y)
{
    if (x->filter) {                                                     /* ZDF state-variable filter */
        int32_t v0 = y * 256, v3 = v0 - s->ic2;
        int32_t v1 = mul15(s->ic1, fa[0]) + mul15(v3, fa[1]);
        int32_t v2 = s->ic2 + mul15(s->ic1, fa[1]) + mul15(v3, fa[2]);
        s->ic1 = 2 * v1 - s->ic1; s->ic2 = 2 * v2 - s->ic2;
        int32_t f = x->filter == 1 ? v2 : x->filter == 3 ? v1 : v0 - mul14(v1, c->f_k) - v2;
        f >>= 8;
        y = f > 32767 ? 32767 : f < -32768 ? -32768 : f;
    }
    if (c->drive_q12) {
        int32_t d = (y * c->drive_q12) >> 12;
        d = d > 32767 ? 32767 : d < -32767 ? -32767 : d;
        y = d - (((((d * d) >> 15) * d) >> 15) * 10923 >> 15);
    }
    return y;
}

/* per block: pitch multiplier (envelope + sync, Q12), then the envelope glides on */
static int32_t sp_block_mult(sp_xvoice *s, const sp_pre *c)
{
    int32_t pu = s->pitch_u >> 12;
    int32_t mult = mul12(pu ? sp_pitch_q12(pu) : 4096, c->sync_q12);
    if (mult > 32768) mult = 32768;                                    /* 8x: inc * mult stays in 32 bits */
    if (s->pitch_u) {
        int32_t nc = s->pitch_u - c->p_step * SP_BLOCK;
        s->pitch_u = (c->p_start > 0 ? nc > 0 : nc < 0) ? nc : 0;
    }
    return mult;
}
static const int32_t *sp_block_fa(const sp_xvoice *s, const sp_extra *x, const sp_pre *c)
{
    int ci = x->cutoff + s->cut_vel + ((x->fenv * (s->env >> 22)) >> 8);   /* env Q30 -> Q8 */
    return c->fa[ci < 0 ? 0 : ci > 127 ? 127 : ci];
}
void sp_plus_fast_pre(sp_xvoice *s, const sp_pre *c, int32_t *inc)
{
    int32_t mult = sp_block_mult(s, c);
    if (mult != 4096)
        for (int n = 0; n < SP_BLOCK; n++) inc[n] = mul12(inc[n], mult);    /* inc < 2^27, mult <= 2^16 */
}
void sp_plus_fast_post(sp_xvoice *s, const sp_extra *x, const sp_pre *c, int16_t *out)
{
    if (!x->filter && !c->drive_q12) return;
    const int32_t *fa = sp_block_fa(s, x, c);
    for (int n = 0; n < SP_BLOCK; n++) out[n] = (int16_t)sp_post(s, x, c, fa, out[n]);
}

/* equal-power crossfade gains: sin / cos quarter wave, 33 points, Q15 */
static const int16_t sp_qsin[33] = {
    0, 1608, 3212, 4808, 6393, 7962, 9512, 11039, 12539, 14010, 15446, 16846, 18204, 19519, 20787, 22005,
    23170, 24279, 25329, 26319, 27245, 28105, 28898, 29621, 30273, 30852, 31356, 31785, 32137, 32412, 32609, 32728, 32767 };

void sp_xfade_gains(int32_t w, int32_t *gin, int32_t *gout)     /* w 0..1024 */
{
    int32_t i = w >> 5, f = w & 31;
    if (i >= 32) { i = 31; f = 32; } else if (i < 0) { i = 0; f = 0; }
    *gin = sp_qsin[i] + (((sp_qsin[i + 1] - sp_qsin[i]) * f) >> 5);
    *gout = sp_qsin[32 - i] + (((sp_qsin[31 - i] - sp_qsin[32 - i]) * f) >> 5);
}

void sp_plus_render(sp_voice *v, sp_xvoice *s, int16_t *out, const int16_t *k, const sp_extra *x, const sp_pre *c,
                    const int32_t *inc, int32_t rate, const int16_t *data, const int16_t *fir)
{
    if (!c->render_active) { sp_stock_render(v, out, k, inc, rate, data, fir); return; }
    int32_t start = sp_bound(k[0], 0), end = sp_bound(k[3], 0);
    int32_t ls = sp_bound(k[1], k[5]), le = sp_bound(k[2], k[6]);
    if (v->first) { if (s->dir > 0 && v->pos < start) v->pos = start; v->first = 0; }
    int32_t g = ((int32_t)k[7] * k[7] * 2 + 0x8000) >> 16; if (g > 32767) g = 32767;
    int loop = k[0] < k[2] && le > ls && (!v->released || x->loop_rel);
    int rev = k[4] >> 14;
    /* per block: pitch (envelope + sync), cutoff (base + envelope + velocity), crossfade length */
    int32_t mult = sp_block_mult(s, c);
    const int32_t *fa = sp_block_fa(s, x, c);
    int32_t xf = (int32_t)x->xfade * 32 << 12, span = le - ls, inv = 0;   /* inv: 1024 / (xf >> 8), Q13 */
    if (xf > span) xf = span;
    if (xf > ls) xf = ls;
    if (!loop || x->loop_mode || rev) xf = 0;
    if (xf >= 256) inv = (1 << 23) / (xf >> 8); else xf = 0;           /* one division per block */
    for (int n = 0; n < SP_BLOCK; n++) {
        int32_t step = mul12(sp_step32((uint32_t)inc[n], (uint32_t)rate), mult), y;
        if (loop) {
            if (s->dir > 0 && v->pos >= le) {
                if (x->loop_mode) { v->pos = 2 * le - v->pos; s->dir = -1; } else v->pos -= span;
            } else if (s->dir < 0 && v->pos < ls) {
                if (x->loop_mode) { v->pos = 2 * ls - v->pos; s->dir = 1; } else v->pos += span;
            }
        }
        if (s->dir > 0 ? v->pos >= end : v->pos < start) y = 0;
        else {
            y = sp_tap(data, fir, v->pos, g);
            if (xf && v->pos >= le - xf) {                               /* fade into the audio before ls */
                int32_t w = (((v->pos - (le - xf)) >> 8) * (inv >> 4)) >> 9;   /* 0..1024 = 0..32 << 5 */
                int32_t i = w >> 5, f = w & 31;
                if (i >= 32) { i = 31; f = 32; }
                int32_t gi = sp_qsin[i] + (((sp_qsin[i + 1] - sp_qsin[i]) * f) >> 5);
                int32_t go = sp_qsin[32 - i] + (((sp_qsin[31 - i] - sp_qsin[32 - i]) * f) >> 5);
                int32_t z = sp_tap(data, fir, v->pos - span, g);
                y = ((y * go) >> 15) + ((z * gi) >> 15);
            }
            v->pos += s->dir > 0 ? step : -step;
        }
        out[n] = (int16_t)sp_post(s, x, c, fa, y);
    }
}
