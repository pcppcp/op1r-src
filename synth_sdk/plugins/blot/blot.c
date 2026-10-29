/* "blot" (BLOT) - a TB-303 style bass voice: saw / square into a resonant 4-pole ladder low-pass with a decaying filter
 * envelope and accent.
 *   knob 0 (blue)  : cutoff, 30 Hz .. 15 kHz        Shift + turn: waveform, saw .. square
 *   knob 1 (green) : resonance (squelch)             Shift + turn: accent amount
 *   knob 2 (white) : envelope mod, 0 .. 5 octaves    Shift + turn: drive
 *   knob 3 (red)   : decay, 40 .. 640 ms (time constant)
 * Accent: notes with velocity >= 100 sweep the filter further (+ accent x 2 octaves), decay fast and play louder.
 * Slide: the OP-1's own portamento (mono / legato play mode); the pitch increments already include the glide.
 * Cost: per sample a polyBLEP saw pair, four one-pole stages and a soft-clipped feedback, all 16 x 16 multiplies;
 * the envelope and the cutoff coefficient every 8 samples; one division per block (polyBLEP reciprocal, decay).
 * Copyright (C) 2026 pcppcp. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include "op1_plugin.h"
#include "blot_tables.h"

typedef struct {
    uint32_t vtable;            /* +0x00 firmware-owned, never written */
    uint32_t ph;                /* oscillator phase (2^32 = one cycle) */
    int32_t  y[4];              /* ladder stages */
    int32_t  env;               /* filter envelope, Q24 */
    uint8_t  gate, acc;         /* key held (screen), accented note */
} ac_voice_t;
typedef char ac_size_check[sizeof(ac_voice_t) <= OP1_VOICE_SIZE ? 1 : -1];

#define AC_ACCENT_VEL (100 * 258)   /* op1_voice_vel_gain of velocity 100 */
#define AC_ACCENT_TAU 50            /* ms: an accented note's decay */

volatile int16_t ac_level[OP1_VOICES];   /* filter envelope per voice, Q15 (screen) */

static void ac_note_on(void *self, const int16_t *params)
{
    ac_voice_t *v = (ac_voice_t *)self;
    (void)params;
    v->env = 1 << 24;           /* phase and filter run on (no click, as the 303's free-running oscillator) */
    v->gate = 1;
}
static void ac_note_off(void *self) { ((ac_voice_t *)self)->gate = 0; }

static inline int32_t clip(int32_t x, int32_t m) { return x > m ? m : (x < -m ? -m : x); }
static inline int32_t soft(int32_t x)    /* x - x^3 / 3 on +-32767: +-21845 at the ends, slope 0 there */
{
    x = clip(x, 32767);
    int32_t x2 = (x * x) >> 15;
    return x - ((((x2 * x) >> 15) * 10923) >> 15);
}

/* ---- the sample loop: everything per sample works on 16-bit values, so every product is one 16 x 16 MAC on the
 * Blackfin (blot_dsp.S, bit for bit this C, which host builds and -DBLOT_C_REF use). The cutoff coefficient per
 * 8 samples (g[]) and the gains are worked out per block. */
typedef struct {
    uint32_t p;                 /*  0 oscillator phase (in / out) */
    int32_t  y[4];              /*  4 ladder stages, +-32767 (in / out) */
    const int32_t *inc;         /* 20 OP1_PITCH_INC */
    int16_t *out;               /* 24 */
    int32_t  k;                 /* 28 feedback, Q12, 0 .. 15974 */
    int32_t  wave;              /* 32 saw .. square, Q15 (0: the 2nd saw is skipped) */
    int32_t  cd;                /* 36 comp x drive, Q12, <= 24267 */
    int32_t  gain;              /* 40 output gain, Q15 */
    int32_t  rinv;              /* 44 polyBLEP reciprocal, 16384 .. 32768 */
    int32_t  sh;                /* 48 its shift */
    int16_t  g[16];             /* 52 ladder coefficient per 8 samples, Q14 */
    uint32_t w;                 /* 84 fast path (blot_dsp.S): both polyBLEP terms are 0 and the clip idle while the phase
                                 *    is >= w from a wrap; 0: no fast path (high notes) */
} bl_blk;
#ifdef __bfin__
_Static_assert(sizeof(bl_blk) == 88 && __builtin_offsetof(bl_blk, g) == 52 && __builtin_offsetof(bl_blk, w) == 84, "bl_blk layout (blot_dsp.S)");
#endif

/* q / di in Q15 (rinv = 2^29 / (di >> sh) of the block's first increment), capped at 32767; the operand is capped at
 * 16 bits, which changes nothing: from 65535 on the fraction is past the cap anyway (rinv >= 16384) */
static inline int32_t bl_frac(uint32_t q, int sh, uint32_t rinv)
{
    uint32_t a = q >> sh; if (a > 65535u) a = 65535u;
    uint32_t t = (a * rinv) >> 14;
    return t > 32767u ? 32767 : (int32_t)t;
}
/* a saw at phase q with both polyBLEP halves, branch-free: after a wrap + (1 - q / di)^2, before one
 * - (1 - (2^32 - q) / di)^2; away from a wrap both fractions are capped and both terms are 0 ((32768 - 32767)^2 >> 15) */
static inline int32_t bl_saw(uint32_t q, int sh, uint32_t rinv)
{
    int32_t x = (int32_t)(q ^ 0x80000000u) >> 16;                       /* (q >> 16) - 32768 */
    int32_t e = 32768 - bl_frac(q, sh, rinv), t = 32768 - bl_frac(0u - q, sh, rinv);
    x += ((e * e) >> 15) - ((t * t) >> 15);
    return clip(x, 32767);
}

#if defined(__bfin__) && !defined(BLOT_C_REF)
void bl_loop(bl_blk *b);                                                /* blot_dsp.S */
#else
/* two passes, as blot_dsp.S: the oscillator (osc >> 1) into out[], then the ladder and the output stage in place */
static void bl_loop(bl_blk *b)
{
    uint32_t p = b->p;
    const int32_t sh = b->sh, wave = b->wave;
    const uint32_t rinv = (uint32_t)b->rinv;
    for (int n = 0; n < OP1_BLOCK; n++) {
        p += (uint32_t)b->inc[n];
        int32_t osc = bl_saw(p, sh, rinv);
        if (wave) osc -= (bl_saw(p + 0x80000000u, sh, rinv) * wave) >> 15;   /* saw .. square */
        b->out[n] = (int16_t)(osc >> 1);                                /* +-32767 */
    }
    b->p = p;
    int32_t y0 = b->y[0], y1 = b->y[1], y2 = b->y[2], y3 = b->y[3];
    const int32_t k = b->k, cd = b->cd, gain = b->gain;
    for (int n = 0; n < OP1_BLOCK; n++) {
        const int32_t g = b->g[n >> 3];
        /* ladder: soft-clipped input minus feedback, four one-pole stages; (a - b) g as a g - b g: 16-bit factors; the
         * step saturated to 16 bits (it only gets larger from a garbage state, blot_dsp.S takes it as a 16-bit half) */
        int32_t u = soft(b->out[n] - ((k * y3) >> 12));
        y0 += op1_sat16((u * g - y0 * g) >> 14);
        y1 += op1_sat16((y0 * g - y1 * g) >> 14);
        y2 += op1_sat16((y1 * g - y2 * g) >> 14);
        y3 += op1_sat16((y2 * g - y3 * g) >> 14);
        int32_t o = soft((y3 * cd) >> 12);                              /* drive 1x: a plain saw stays below the knee */
        o = clip(o + (o >> 1), 32767);                                  /* soft() peaks at +-21846: +-32769 here */
        b->out[n] = (int16_t)((o * gain) >> 15);                       /* 16 x 16 bits, no saturation needed */
    }
    b->y[0] = y0; b->y[1] = y1; b->y[2] = y2; b->y[3] = y3;
}
#endif

static bl_blk bl_b;                                                     /* the audio thread only, one voice at a time */

static void ac_render(void *self, int16_t *out, const int16_t *params)
{
    ac_voice_t *v = (ac_voice_t *)self;
    const int32_t *inc = OP1_PITCH_INC;
    int32_t vg = op1_voice_vel_gain(self);
    v->acc = vg >= AC_ACCENT_VEL;
    int32_t acc = v->acc ? (params[6] > 0 ? params[6] : 20000) : 0;   /* accent amount, Q15 (word 0: a new sound) */

    /* per block: cutoff / env mod in Q12 octaves, feedback, decay step, waveform, gains */
    bl_blk *b = &bl_b;
    int32_t base = (params[0] * (9 * 4096)) >> 15;                      /* 30 Hz .. 15 kHz */
    int32_t em = ((params[2] * (5 * 4096)) >> 15) + ((acc * (2 * 4096)) >> 15);
    b->k = (params[1] * 15974) >> 15;                                   /* feedback 0 .. 3.9, Q12 */
    int32_t kd = params[3] < 0 ? 0 : params[3] >> 11, kf = params[3] < 0 ? 0 : params[3] & 2047;
    int32_t tau = v->acc ? AC_ACCENT_TAU : ac_tau_ms[kd] + (((ac_tau_ms[kd + (kd < 16)] - ac_tau_ms[kd]) * kf) >> 11);
    int32_t dstep = (int32_t)(1342177280 / (441 * tau));                /* 8 / (44.1 tau) per 8 samples, Q24 */
    b->wave = params[5] > 0 ? params[5] : 0;                            /* saw .. square, Q15 */
    int32_t drive = 4096 + ((params[7] > 0 ? params[7] : 0) >> 2);      /* 1x .. 3x, Q12 */
    b->gain = 19661 + ((acc * 13106) >> 15);                            /* 0.6, accent up to 1.0 */
    b->cd = ((4096 + (b->k >> 2)) * drive) >> 12;                       /* (some bass back with resonance) x drive */

    /* polyBLEP fraction t = p / inc in Q15 without a per-sample division: rinv from this block's first inc */
    uint32_t inc0 = (uint32_t)inc[0] < 16384u ? 16384u : (uint32_t)inc[0];
    int sh = 0;
    while ((inc0 >> sh) >= 32768u) sh++;
    b->sh = sh;
    b->rinv = (int32_t)((1u << 29) / (inc0 >> sh));                     /* 16384 .. 32768 */
    /* from w = 2 inc0 away from a wrap frac() is capped (q >> sh >= 2 m, m = inc0 >> sh: (q >> sh) rinv >= 2m (2^29 / m
     * - 1) >= 2^30 - 2^16, >> 14 > 32767; or the 65535 cap: >= 65535); w >= 65536: x = -32768 only for q < 65536 */
    b->w = inc0 < (1u << 28) ? (2 * inc0 > 65536u ? 2 * inc0 : 65536u) : 0;

    /* the voice object keeps the previous engine's bytes (note_on keeps the filter for legato): clamp the states to
     * their range, else the ladder's products overflow and it locks into a clipped oscillation */
    b->p = v->ph;
    for (int i = 0; i < 4; i++) b->y[i] = clip(v->y[i], 32767);
    int32_t env = v->env < 0 ? 0 : (v->env > (1 << 24) ? 1 << 24 : v->env);
    for (int j = 0; j < OP1_BLOCK / 8; j++) {                           /* envelope and cutoff every 8 samples */
        env -= (env >> 12) * (dstep >> 4) >> 8;
        int32_t o = base + ((env >> 9) * em >> 15);                     /* env Q15 x em (<= 7 octaves) */
        if (o > (AC_G_N - 1) * 512 - 1) o = (AC_G_N - 1) * 512 - 1;
        int32_t i = o >> 9, f = o & 511;
        b->g[j] = (int16_t)(ac_g[i] + (((ac_g[i + 1] - ac_g[i]) * f) >> 9));
    }
    b->inc = inc; b->out = out;
    bl_loop(b);
    v->ph = b->p; for (int i = 0; i < 4; i++) v->y[i] = b->y[i];
    v->env = env;
    unsigned vi = (unsigned)op1_voice_index(self);
    if (vi < OP1_VOICES) ac_level[vi] = (int16_t)(env >> 9);           /* (a voice object outside the 6: no screen) */
}

const op1_engine_t ENGINE_blot = { ac_note_on, ac_render, ac_note_off,
                                   { OP1_PRESS_SHIFT, OP1_PRESS_SHIFT, OP1_PRESS_SHIFT, OP1_PRESS_NONE } };

/* ======================= screen (GUI thread) =======================
 * An acid house smiley in lines, standing still (blot.svg is empty), and dddd's bottom row. Each knob changes one part,
 * drawn in its colour:
 *   blue   cutoff     the eyes: sleepy slits .. wide open
 *   green  resonance  the smile: flat .. a big grin, dimples at the top
 *   white  env mod    the mouth opens with each note (env mod x the filter envelope) and closes with it
 *   red    decay      drips below the face (the melting smiley): longer with the decay
 *   Shift + white (drive): the face outline wiggles (8 bumps, 0 .. +-3 px), standing still
 *   accent            a wink; idle: a blink now and then
 * Bottom row: cutoff (Hz), resonance, env mod, decay (ms); while Shift is held: wave, accent, drive. ~70 lines. */
#include "op1_synth.h"
#define Q(v) ((v) << 10)
#define TS 12
#define FX  160                 /* face centre and radius, px */
#define FY  50
#define FR  34
static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     c_face[4] = { 0xDF, 0xD9, 0xFF, 0xFF };
static const uint8_t *const col4[4] = { c_blue, c_green, c_white, c_red };
enum { TX_LAB, TX_VAL = 4, TX_N = 8 };
static uint32_t tx[TX_N][OP1_TEXT_SIZE / 4];
static char shown[TX_N][14];
static uint8_t tx_made;
static int32_t sm_open, sm_wink, sm_t, sm_lv;   /* mouth (Q15, eased), wink frames left, frame count, level seen */

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)   /* Q10 */
{
    if (x0 == x1 && y0 == y1) return;
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
static void txt(void *r, int i, const char *s)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    int same = 1;
    for (int j = 0; j < 14; j++) { if (shown[i][j] != s[j]) { same = 0; break; } if (!s[j]) break; }
    if (!same) { op1_text_set(tx[i], s); int j = 0; for (; j < 13 && s[j]; j++) shown[i][j] = s[j]; shown[i][j] = 0; }
    op1_text_draw(tx[i], r, ident);
}
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *num(char *p, int32_t x) { char t[8]; int n = 0; do t[n++] = (char)('0' + x % 10); while ((x /= 10)); while (n) *p++ = t[--n]; *p = 0; return p; }
static void pct(char *p, int32_t v) { put(num(p, (v * 100 + 16383) >> 15), "%"); }
/* an ellipse (Q10 centre, px radii) of n segments from angle a0 to a1 (8192 a turn) */
static void arc(void *r, int32_t cx, int32_t cy, int32_t rx, int32_t ry, int n, int32_t a0, int32_t a1, int32_t w, const uint8_t *c)
{
    int32_t px = 0, py = 0;
    for (int i = 0; i <= n; i++) {
        int32_t a = a0 + (a1 - a0) * i / n;
        int32_t x = cx + Q(rx) * OP1_SINE_TABLE[(a + 2048) & 8191] / 32768, y = cy + Q(ry) * OP1_SINE_TABLE[a & 8191] / 32768;
        if (i) ln(r, px, py, x, y, w, c);
        px = x; py = y;
    }
}

#define FACE_N 64               /* outline segments: 8 per bump, the wiggle stays smooth */
/* the outline's point at angle a (8192 a turn), Q10; wg = wiggle depth, Q10 px (8 bumps) */
static op1_pt face_pt(int32_t a, int32_t wg)
{
    int32_t rr = Q(FR) + wg * OP1_SINE_TABLE[(a * 8) & 8191] / 32768;
    op1_pt p = { Q(FX) + rr * OP1_SINE_TABLE[(a + 2048) & 8191] / 32768, Q(FY) + rr * OP1_SINE_TABLE[a & 8191] / 32768 };
    return p;
}

static void ac_screen_init(void *scr) { (void)scr; }

static void ac_screen_update(void *scr, int dt)
{
    int slot = OP1_SCREEN_SLOT & 7;
    int16_t k[4];
    op1_synth_knobs_live(slot, k);
    const int16_t *w = op1_synth_knobs(slot);
    for (int i = 0; i < 4; i++) if (k[i] < 0) k[i] = 0;
    if (!tx_made) {
        for (int i = 0; i < 4; i++) {
            op1_text_ctor(tx[TX_LAB + i], Q(8 + 78 * i), 114, TS, col4[i]);
            op1_text_ctor(tx[TX_VAL + i], Q(8 + 78 * i), 127, TS, c_white);
        }
        tx_made = 1;
    }
    int32_t lv = 0, acc = 0;                                     /* the loudest held voice's filter envelope */
    for (int i = 0; i < OP1_VOICES; i++) {
        const ac_voice_t *v = (const ac_voice_t *)OP1_VOICE(i);
        if (v->gate && ac_level[i] > lv) { lv = ac_level[i]; acc = v->acc; }
    }
    if (acc && lv > sm_lv + 8192) sm_wink = 12;                  /* an accented note: wink */
    sm_lv = lv; sm_t++;
    int32_t open = lv * k[2] >> 15;                              /* the mouth: env mod x the envelope, eased */
    sm_open += (open - sm_open) >> 1;
    op1_screen_draw(scr, dt);
    void *r = op1_raster();

    /* the face: a circle, wiggling with the drive (Shift + white) */
    int32_t wg = Q(3) * (w[7] > 0 ? w[7] : 0) >> 15;
    for (int j = 0; j < FACE_N; j++) {
        op1_pt a = face_pt(j * 8192 / FACE_N, wg), b = face_pt((j + 1) * 8192 / FACE_N, wg);
        ln(r, a.x, a.y, b.x, b.y, 0x700, c_face);
    }
    /* eyes (blue): height with the cutoff; a blink every ~4 s, the left eye winks on an accent */
    int32_t eh = 1 + (k[0] * 7 >> 15), blink = (sm_t % 240) >= 235;
    for (int e = 0; e < 2; e++) {
        int32_t ex = Q(FX - 12 + 24 * e), ey = Q(FY - 10);
        if (blink || (e == 0 && sm_wink)) ln(r, ex - Q(4), ey, ex + Q(4), ey, 0x700, c_blue);
        else arc(r, ex, ey, 4, eh, 10, 0, 8192, 0x700, c_blue);
    }
    if (sm_wink) sm_wink--;
    /* smile (green): a parabola, deeper with the resonance; dimples at the ends from 60 % */
    int32_t depth = Q(2) + (Q(12) * k[1] >> 15), half = Q(19), my = Q(FY + 8);
    /* the lip: depth + opening <= 23 px below the smile's corners, so it stays inside the face (r 34, corners at
     * +-19 px, 8 px below the centre: at the middle 8 + 23 = 31 < 34 - 3, at +-half way 8 + 17 < 32.6 - 3) */
    int32_t px = 0, py = 0, qx = 0, qy = 0, mo = (Q(23) - wg - depth) * sm_open >> 15;   /* (the wiggle reaches in) */
    for (int i = 0; i <= 10; i++) {
        int32_t u = (i - 5) * 6554;                              /* -1 .. 1, Q15 */
        int32_t x = Q(FX) + (half * u >> 15), y = my + depth - (int32_t)((int64_t)depth * u * u >> 30);
        int32_t yl = y + (int32_t)((int64_t)mo * (32768 - (int32_t)((int64_t)u * u >> 15)) >> 15);   /* the lower lip */
        if (i) {
            ln(r, px, py, x, y, 0x700, c_green);
            if (mo > Q(1)) ln(r, qx, qy, x, yl, 0x700, c_white);
        }
        px = x; py = y; qx = x; qy = yl;
    }
    if (k[1] > 19661)
        for (int e = -1; e <= 1; e += 2) ln(r, Q(FX) + e * half, my, Q(FX) + e * (half + Q(3)), my - Q(3), 0x600, c_green);
    /* drips (red): from the chin, longer with the decay */
    static const int8_t dx[3] = { -10, 2, 11 }, dl[3] = { 6, 10, 8 };
    for (int i = 0; i < 3; i++) {
        /* start on the drawn outline (wiggle included): where its lower half crosses x, + half the line width */
        int32_t x = Q(FX + dx[i]), y0 = Q(FY + FR);
        for (int j = 0; j < FACE_N; j++) {
            op1_pt a = face_pt(j * 8192 / FACE_N, wg), b = face_pt((j + 1) * 8192 / FACE_N, wg);
            if (a.y > Q(FY) && b.y > Q(FY) && ((a.x <= x && x <= b.x) || (b.x <= x && x <= a.x)) && a.x != b.x) {
                y0 = a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x); break;   /* <= 38k x 5k (Q10): 32 bit */
            }
        }
        y0 += 0x700 / 2;
        int32_t len = Q(2) + (Q(dl[i]) * k[3] >> 15) * 2;
        ln(r, x, y0, x, y0 + len, 0x600, c_red);
        ln(r, x - OP1_Q10(1.5), y0 + len, x + OP1_Q10(1.5), y0 + len, 0x900, c_red);    /* the drop: two crossing lines */
        ln(r, x, y0 + len - OP1_Q10(1.5), x, y0 + len + OP1_Q10(1.5), 0x900, c_red);
    }

    /* the bottom row */
    for (int i = 0; i < 4; i++) ln(r, Q(8 + 78 * i), Q(110), Q(78 + 78 * i), Q(110), 0x800, col4[i]);
    char v[16];
    if (op1_shift_held()) {
        int32_t w5 = w[5] > 0 ? w[5] : 0, w6 = w[6] > 0 ? w[6] : 20000, w7 = w[7] > 0 ? w[7] : 0;
        txt(r, TX_LAB, "wave"); if (w5 < 328) put(v, "saw"); else if (w5 > 32440) put(v, "square"); else pct(v, w5); txt(r, TX_VAL, v);
        txt(r, TX_LAB + 1, "accent"); pct(v, w6); txt(r, TX_VAL + 1, v);
        int32_t d = 10 + (w7 * 20 >> 15); char *p = put(v, "x"); p = num(p, d / 10); p = put(p, "."); num(p, d % 10);   /* drive x1.0 .. x3.0 */
        txt(r, TX_LAB + 2, "drive"); txt(r, TX_VAL + 2, v);
        txt(r, TX_LAB + 3, "decay");
    } else {
        /* cutoff: 30 Hz x 2^(9 k / 32768), 2^frac ~ 1 + f (0.657 + 0.343 f) */
        int32_t o = k[0] * 9, oi = o >> 15, of = o & 32767;
        int32_t hz = (30 << oi) * (32768 + (int32_t)(((int64_t)of * (21529 + (11239 * of >> 15))) >> 15)) >> 15;
        if (hz < 1000) put(num(v, hz), "hz"); else { char *p = num(v, hz / 1000); p = put(p, "."); put(num(p, hz / 100 % 10), "khz"); }   /* lower case: the OP-1 font has another H */
        txt(r, TX_LAB, "cutoff"); txt(r, TX_VAL, v);
        txt(r, TX_LAB + 1, "reso"); pct(v, k[1]); txt(r, TX_VAL + 1, v);
        txt(r, TX_LAB + 2, "env mod"); pct(v, k[2]); txt(r, TX_VAL + 2, v);
        txt(r, TX_LAB + 3, "decay");
    }
    int32_t kd = k[3] >> 11, kf = k[3] & 2047;
    put(num(v, ac_tau_ms[kd] + (((ac_tau_ms[kd + (kd < 16)] - ac_tau_ms[kd]) * kf) >> 11)), "ms");
    txt(r, TX_VAL + 3, v);
}

const op1_screen_t SCREEN_blot = { ac_screen_init, ac_screen_update };
