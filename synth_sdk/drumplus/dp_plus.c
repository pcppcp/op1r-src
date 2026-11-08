/* drum+ voice (docs/31): the stock drum voice plus per-pad envelope, pitch envelope, fine tune, retrig, filter and
 * drive. All extras off: the stock functions run unchanged (bit-exact). Otherwise an own per-sample loop on the
 * same primitives (dp_bound, dp_tap_sample, dp_gain): the envelope multiplies the velocity before the stock gain law
 * (level = vol^2 * (velocity * env)^2), the pitch envelope / fine tune scale the increment, retrig restarts the
 * region sample-accurately, then filter and drive on the voice output. Constants come from dp_plus_prepare (when a
 * pad's extras change); the render is 32-bit without divisions. */
#include "dp_plus.h"
#include "dp_tables.h"

#define Q30   (1 << 30)
#define PSTEP 96                                           /* pitch unit: 1/96 octave (eighth semitone) */

int dp_extra_active(const dp_extra *x)
{
    return x && (x->env_on || x->pitch_env || x->fine || x->retrig >= 2 || x->filter || x->drive ||
                 x->vel_pitch || x->vel_start || x->human);
}

#define OFF(f) ((uint8_t)__builtin_offsetof(dp_extra, f))
static const struct { uint8_t off; int8_t sgn; int16_t lo, hi; } dp_fields[DP_NFIELDS] = {
    { OFF(env_on), 0, 0, 1 }, { OFF(attack), 0, 0, 127 }, { OFF(hold), 0, 0, 127 }, { OFF(decay), 0, 0, 127 },
    { OFF(release), 0, 0, 127 }, { OFF(curve_exp), 0, 0, 1 }, { OFF(pitch_env), 1, -48, 48 },
    { OFF(pitch_time), 0, 0, 127 }, { OFF(fine), 1, -50, 50 }, { OFF(retrig), 0, 0, 8 }, { OFF(retrig_div), 0, 0, 5 },
    { OFF(filter), 0, 0, 3 }, { OFF(cutoff), 0, 0, 127 }, { OFF(resonance), 0, 0, 127 }, { OFF(drive), 0, 0, 127 },
    { OFF(choke), 0, 0, 4 }, { OFF(mono), 0, 0, 1 }, { OFF(vel_pitch), 1, -24, 24 }, { OFF(vel_cutoff), 1, -127, 127 },
    { OFF(vel_start), 0, 0, 127 }, { OFF(human), 0, 0, 127 },
};

int dp_extra_set(dp_extra *x, int i, int32_t v)
{
    if ((unsigned)i >= DP_NFIELDS) return 0;
    v = v < dp_fields[i].lo ? dp_fields[i].lo : v > dp_fields[i].hi ? dp_fields[i].hi : v;
    if (i == 9 && v == 1) v = 0;                            /* retrig: 0 off, 2..8 hits */
    ((uint8_t *)x)[dp_fields[i].off] = (uint8_t)v;
    return v;
}
int dp_extra_get(const dp_extra *x, int i)
{
    if ((unsigned)i >= DP_NFIELDS) return 0;
    uint8_t b = ((const uint8_t *)x)[dp_fields[i].off];
    return dp_fields[i].sgn ? (int8_t)b : b;
}

/* 0..127 -> 44.1 kHz samples: 0, then 3 ms * 2^(t / 12) (127: ~4.6 s); the voice counts DP_VOICE_X per sample */
uint32_t dp_time_samples(uint8_t t)
{
    static const uint16_t pow12[12] = { 256, 271, 287, 304, 323, 342, 362, 384, 406, 431, 456, 483 };
    if (!t) return 0;
    if (t > 127) t = 127;
    return (132u * pow12[t % 12] << (t / 12)) >> 8;
}

/* 2^(u / 96) for u = pitch offset in 1/96 octave << 8, -4..+4 octaves, in Q12 */
static uint16_t dp_p2[PSTEP + 1];                          /* 2^(i / 96) in Q14 (16384 .. 32768) */
static void dp_p2_init(void)
{
    if (dp_p2[0]) return;
    int32_t v = 1 << 30;
    for (int i = 0; i <= PSTEP; i++) {
        dp_p2[i] = (uint16_t)((v + (1 << 15)) >> 16);
        v = (int32_t)(((int64_t)v * 1081522600 + (1 << 29)) >> 30);    /* prepare only: 2^(1/96) in Q30 */
    }
    dp_p2[PSTEP] = 32768;
}
static int32_t dp_pitch_q12(int32_t u)
{
    const int32_t lim = 4 * PSTEP * 256;
    if (u > lim) u = lim; else if (u < -lim) u = -lim;
    int32_t oct = 0, w = u + lim;                          /* >= 0 */
    while (w >= PSTEP << 8) { w -= PSTEP << 8; oct++; }    /* at most 8 rounds */
    int32_t i = w >> 8, f = w & 255;
    int32_t m = dp_p2[i] + (((dp_p2[i + 1] - dp_p2[i]) * f) >> 8);   /* Q14, 1.0 .. 2.0 */
    oct -= 4;
    return oct >= 2 ? m << (oct - 2) : m >> (2 - oct);
}

/* s * c >> 15 (c <= 2^15, |s| < 2^28) without a 64-bit multiply */
static inline int32_t mul15(int32_t s, int32_t c) { return (s >> 15) * c + (((s & 0x7FFF) * c) >> 15); }
static inline int32_t mul14(int32_t s, int32_t c) { return (s >> 14) * c + (((s & 0x3FFF) * c) >> 14); }

void dp_plus_prepare(const dp_extra *x, dp_pre *c)
{
    /* may run while the audio thread renders: off first, filled, switched on last (no memset in the firmware) */
    for (volatile uint8_t *b = (volatile uint8_t *)c; b < (volatile uint8_t *)(c + 1); b++) *b = 0;
    if (!dp_extra_active(x)) return;
    dp_p2_init();
    c->t_att = DP_VOICE_X * dp_time_samples(x->attack); c->t_hold = DP_VOICE_X * dp_time_samples(x->hold);
    c->t_dec = DP_VOICE_X * dp_time_samples(x->decay); c->t_rel = DP_VOICE_X * dp_time_samples(x->release);
    c->a_step = c->t_att ? Q30 / (int32_t)c->t_att : Q30;
    c->d_step = c->t_dec ? Q30 / (int32_t)c->t_dec : Q30;
    c->r_step = c->t_rel ? Q30 / (int32_t)c->t_rel : Q30;
    /* exponential: per-sample decrement 6.9 / T (Q22; ~-60 dB after T voice samples, within 1 % up to ~4.6 s) */
    c->kd = c->t_dec ? 28940698u / (c->t_dec + 1) : 0;
    c->kr = c->t_rel ? 28940698u / (c->t_rel + 1) : 0;
    uint32_t t_pitch = DP_VOICE_X * dp_time_samples(x->pitch_time);
    c->p_start = (int32_t)x->pitch_env * 8388608;          /* semitones -> 1/96 octave << 20 */
    c->p_step = t_pitch ? c->p_start / (int32_t)t_pitch : c->p_start;
    c->fine_u = (int32_t)x->fine * 512 / 25;               /* cents -> 1/96 octave << 8 */
    if (x->filter) {                                        /* k = 1 / Q: 2 .. 0.06 */
        int32_t k = 131072 - (int32_t)(x->resonance > 127 ? 127 : x->resonance) * 127100 / 127;   /* Q16 */
        c->f_k = k >> 2; c->f_k16 = (int16_t)(k >> 2);     /* Q14 both (fits int16: <= 32768 -> 32767) */
        if (k >> 2 > 32767) c->f_k16 = 32767;
    }
    c->drive_q12 = x->drive ? 4096 + (int32_t)x->drive * 12288 / 127 : 0;
    *(volatile uint8_t *)&c->active = 1;
}

/* ZDF SVF (Simper) coefficients for a cutoff step: g = tan(pi fc / fs); at note-on (one 64-bit division) */
static void dp_filter_coefs(dp_xvoice *s, int cutoff, int32_t k14)
{
    int64_t g = dp_filter_g[cutoff < 0 ? 0 : cutoff > 127 ? 127 : cutoff], k = (int64_t)k14 << 2;   /* Q16 */
    int64_t den = 65536 + ((g * (g + k)) >> 16);
    int64_t a1 = ((int64_t)1 << 32) / den, a2 = (g * a1) >> 16, a3 = (g * a2) >> 16;
    s->f_a1 = (int32_t)(a1 >> 1); s->f_a2 = (int32_t)(a2 >> 1); s->f_a3 = (int32_t)(a3 >> 1);   /* Q15 */
}

static uint32_t dp_rand(void)
{
    static uint32_t r = 0x12345678;
    r = r * 1664525u + 1013904223u;
    return r >> 8;                                          /* 24 bits */
}

static const uint8_t dp_per_beat[6] = { 2, 3, 4, 6, 8, 16 }; /* 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/64 */
static uint32_t dp_retrig_interval(const dp_extra *x, uint32_t samples_per_beat)
{
    uint32_t t = DP_VOICE_X * samples_per_beat / dp_per_beat[x->retrig_div < 6 ? x->retrig_div : 5];   /* per hit, not per sample; voice samples */
    return t ? t : 1;
}

static void dp_hit(dp_voice *v, dp_xvoice *s, const dp_pad *pad, const dp_extra *x, const dp_pre *c)
{
    v->pos = pad->reverse >> 14 ? dp_bound(pad->end_hi, pad->end_lo) : dp_bound(pad->start_hi, pad->start_lo);
    s->stage_n = 0;
    if (x->attack) { s->stage = 0; s->env = 0; }
    else { s->stage = x->hold ? 1 : 2; s->env = Q30; }
    s->pitch_u = c->p_start;
}

void dp_plus_note_on(dp_voice *v, dp_xvoice *s, const dp_pad *pad, const dp_extra *x, const dp_pre *c,
                     int velocity, uint32_t samples_per_beat)
{
    dp_stock_note_on(v, pad);
    s->fading = 0; s->hits_left = 0; s->pitch_u = 0; s->vel_u = 0;                  /* stolen voice: no leftovers in the fade path */
    if (!c->active) return;
    dp_hit(v, s, pad, x, c);
    s->hit_gain = 32767; s->ic1 = s->ic2 = 0;
    s->hits_left = x->retrig >= 2 ? (uint8_t)(x->retrig - 1) : 0;
    s->until_retrig = dp_retrig_interval(x, samples_per_beat);
    int vel = velocity < 0 ? 0 : velocity > 127 ? 127 : velocity;
    s->vel_u = (int32_t)x->vel_pitch * 2048 * (vel - 64) / 63;                  /* semitones -> 1/96 octave << 8 */
    if (x->filter) dp_filter_coefs(s, x->cutoff + x->vel_cutoff * (vel - 64) / 63, c->f_k);
    /* start later: soft hits (vel_start) and random delay (human); positions are samples << 11 */
    int32_t off = x->vel_start * 32 * (127 - vel) / 127;
    if (x->human) {
        off += (int32_t)(dp_rand() % (uint32_t)(x->human * 4 + 1));
        s->hit_gain = (int16_t)(32767 - (int32_t)(dp_rand() % (uint32_t)(x->human * 20 + 1)));  /* 0 .. -1.4 dB */
    }
    if (off) {
        int32_t a = dp_bound(pad->start_hi, pad->start_lo), b = dp_bound(pad->end_hi, pad->end_lo);
        int32_t room = (b - a) >> 11;
        if (off > room) off = room;
        v->pos += pad->reverse >> 14 ? -(off << 11) : off << 11;
    }
}

void dp_plus_note_off(dp_voice *v, dp_xvoice *s, const dp_pad *pad, const dp_extra *x, const dp_pre *c)
{
    if (!c->active || !x->env_on) { dp_stock_note_off(v, pad); return; }
    v->state = DP_RELEASED;
    if ((pad->playmode >> 13) != 1) { s->stage = 3; s->stage_n = 0; }   /* gate / loop: release; one-shot runs on */
}

#define DP_FADE_STEP 186                                   /* 32767 / 176: ~2 ms to silence at 88.2 kHz */

int dp_plus_choke(dp_voice *const *v, dp_xvoice *s, const uint8_t *voice_pad, int nvoices, int new_pad, const dp_extra *x)
{
    int n = 0, g = x[new_pad].choke;
    for (int i = 0; i < nvoices; i++) {
        int p = voice_pad[i];
        if (v[i]->state == DP_OFF || s[i].fading) continue;
        if ((p == new_pad && x[p].mono) || (g && p != new_pad && x[p].choke == g)) { s[i].fading = 1; s[i].fade = 32767; n++; }
    }
    return n;
}

/* ---- per-sample pieces shared by the C path (dp_plus_render) and the fast path (stock render, dp_plus_fast_*) ---- */
static inline int32_t dp_env_tick(dp_xvoice *s, const dp_extra *x, const dp_pre *c, int *done)
{
    if (!x->env_on) return 32767;
    switch (s->stage) {
    case 0:                                                             /* attack */
        s->env += c->a_step;
        if (s->env >= Q30 || s->stage_n >= c->t_att) { s->env = Q30; s->stage = c->t_hold ? 1 : 2; s->stage_n = 0; }
        break;
    case 1:                                                             /* hold */
        if (s->stage_n >= c->t_hold) { s->stage = 2; s->stage_n = 0; }
        break;
    case 2:                                                             /* decay (0: sustain at full) */
        if (!c->t_dec) break;
        if (x->curve_exp) s->env -= (int32_t)(((uint32_t)s->env >> 15) * c->kd >> 7); else s->env -= c->d_step;
        if (s->env <= (x->curve_exp ? (Q30 >> 10) : 0)) { s->env = 0; s->stage = 4; }
        break;
    case 3:                                                             /* release */
        if (x->curve_exp) s->env -= (int32_t)(((uint32_t)s->env >> 15) * c->kr >> 7); else s->env -= c->r_step;
        if (s->env <= (x->curve_exp ? (Q30 >> 10) : 0) || !c->t_rel) { s->env = 0; s->stage = 4; }
        break;
    default:
        s->env = 0;
        break;
    }
    s->stage_n++;
    if (s->stage == 4 && !s->hits_left) *done = 1;
    return s->env >> 15;
}
/* increment step (1/2048 sample) of sample n; factor: pitch factor Q12, refreshed every 16 samples */
static inline int32_t dp_step_tick(dp_xvoice *s, const dp_pre *c, int n, int32_t inc, int32_t *factor)
{
    if (!(n & 15)) {
        int32_t u = (s->pitch_u >> 12) + c->fine_u + s->vel_u;
        *factor = u ? dp_pitch_q12(u) : 4096;
    }
    if (s->pitch_u) {                                                   /* glide back to 0 */
        int32_t nc = s->pitch_u - c->p_step;
        s->pitch_u = (c->p_start > 0 ? nc > 0 : nc < 0) ? nc : 0;
    }
    int32_t st = ((inc >> 17) * *factor) >> 12;
    return st > 16383 ? 16383 : st;                                     /* <= 8x: fits the stock buffer as st << 17 */
}
/* the velocity-buffer value of sample n: velocity x envelope x hit gain x choke fade */
static inline int32_t dp_e_tick(dp_xvoice *s, const dp_pre *c, int32_t vel, int32_t env15, int *done)
{
    int32_t e = ((vel * env15) >> 15) * (c->active ? s->hit_gain : 32767) >> 15;
    if (s->fading) {                                                    /* choke / mono: fast fade-out */
        s->fade = (int16_t)(s->fade > DP_FADE_STEP ? s->fade - DP_FADE_STEP : 0);
        e = (e * s->fade) >> 15;
        if (!s->fade) { s->hits_left = 0; *done = 1; }
    }
    return e;
}
static inline int32_t dp_post_tick(dp_xvoice *s, const dp_extra *x, const dp_pre *c, int32_t y)
{
    if (x->filter) {                                                    /* ZDF state-variable filter */
        int32_t v0 = y * 256, v3 = v0 - s->ic2;
        int32_t v1 = mul15(s->ic1, s->f_a1) + mul15(v3, s->f_a2);
        int32_t v2 = s->ic2 + mul15(s->ic1, s->f_a2) + mul15(v3, s->f_a3);
        s->ic1 = 2 * v1 - s->ic1; s->ic2 = 2 * v2 - s->ic2;
        int32_t f = x->filter == 1 ? v2 : x->filter == 3 ? v1 : v0 - mul14(v1, c->f_k) - v2;
        f >>= 8;
        y = f > 32767 ? 32767 : f < -32768 ? -32768 : f;
    }
    if (c->drive_q12) {                                                 /* gain 1..4 into x - x^3/3 */
        int32_t d = (y * c->drive_q12) >> 12;
        d = d > 32767 ? 32767 : d < -32767 ? -32767 : d;
        y = d - (((((d * d) >> 15) * d) >> 15) * 10923 >> 15);
    }
    return y;
}

void dp_plus_render(dp_voice *v, dp_xvoice *s, int16_t *out, const dp_pad *pad, const dp_extra *x, const dp_pre *c,
                    const int32_t *inc, const int16_t *vel, const int16_t *data, const int16_t *fir,
                    uint32_t samples_per_beat)
{
    if (!c->active && !s->fading) { dp_stock_render(v, out, pad, inc, vel, data, fir); return; }
    int32_t start = dp_bound(pad->start_hi, pad->start_lo), end = dp_bound(pad->end_hi, pad->end_lo);
    int rev = pad->reverse >> 14, mode = pad->playmode >> 13, done = 0;
    int32_t factor = 4096;
    for (int n = 0; n < DP_BLOCK; n++) {
        if (s->hits_left && !--s->until_retrig) {                       /* retrig: restart, 2 dB softer */
            s->hits_left--;
            dp_hit(v, s, pad, x, c);
            s->hit_gain = (int16_t)((s->hit_gain * 29204) >> 15);      /* x0.891: level -2 dB (squared) */
            if (v->state == DP_OFF) v->state = DP_RELEASED;
            s->until_retrig = dp_retrig_interval(x, samples_per_beat);
            done = 0;
        }
        int32_t env15 = dp_env_tick(s, x, c, &done);
        int32_t step = dp_step_tick(s, c, n, inc[n], &factor);
        int32_t g = dp_gain(pad->volume, dp_e_tick(s, c, vel[n], env15, &done));
        int loop = mode == 2 && v->state == DP_HELD;
        int32_t y;
        if (!rev) {
            if (!loop && v->pos >= end) { y = 0; if (!s->hits_left) done = 1; }
            else {
                y = dp_tap_sample(data, fir, v->pos, g);
                int32_t p = v->pos + step;
                v->pos = loop && p >= end ? start : p;
            }
        } else {
            if (!loop && v->pos < start) { y = 0; if (!s->hits_left) done = 1; }
            else {
                y = dp_tap_sample(data, fir, v->pos, g);
                int32_t p = v->pos - step;
                v->pos = loop && p < start ? end : p;
            }
        }
        out[n] = (int16_t)dp_post_tick(s, x, c, y);
    }
    if (done) v->state = DP_OFF;
    v->blocks++;
}

/* fast path: our envelope / pitch into the stock per-sample buffers, the stock render, then filter / drive.
 * pre returns 0 (nothing changed) when a retrig falls inside this block: then run dp_plus_render instead. */
int dp_plus_fast_pre(dp_voice *v, dp_xvoice *s, const dp_extra *x, const dp_pre *c, int32_t *inc, int16_t *vel)
{
    if (!c->active && !s->fading) return 0;
    if (s->hits_left && s->until_retrig <= DP_BLOCK) return 0;
    int done = 0;
    int32_t factor = 4096;
    for (int n = 0; n < DP_BLOCK; n++) {
        int32_t env15 = dp_env_tick(s, x, c, &done);
        int32_t step = dp_step_tick(s, c, n, inc[n], &factor);
        vel[n] = (int16_t)dp_e_tick(s, c, vel[n], env15, &done);
        inc[n] = step << 17;
    }
    if (s->hits_left) s->until_retrig -= DP_BLOCK;
    s->pend_done = (uint8_t)done;
    s->pend_state = (uint8_t)v->state;
    return 1;
}
void dp_plus_fast_post(dp_voice *v, dp_xvoice *s, const dp_extra *x, const dp_pre *c, int16_t *out)
{
    if (x->filter || c->drive_q12)
        for (int n = 0; n < DP_BLOCK; n++) out[n] = (int16_t)dp_post_tick(s, x, c, out[n]);
    if (s->pend_done) v->state = DP_OFF;
    else if (s->hits_left && v->state == DP_OFF) v->state = s->pend_state;   /* region ended, a retrig is pending */
}
