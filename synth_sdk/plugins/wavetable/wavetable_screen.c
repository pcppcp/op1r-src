/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* WAVETABLE's screen (GUI thread): an oscilloscope stack of the engine's wavetable, the owner's concept A
 * (wavetable.svg + wavetable_geom.h, gen_svg.py). The SVG holds the still parts (the stack of ten table frames, the
 * floor grid and rails, the axis, the ghost curve) and is drawn whole through the static-art cache; the parts that
 * move are drawn here:
 *   green  the frame WAVE plays (the engine's own morph via wavetable_shape, at its depth in the stack) and a fainter
 *          copy shifted by DETUNE plus the two oscillators' phase drift (read from the loudest sounding voice,
 *          relative to its note-on offset); both swell up to 15 % with the level; the fundamental's bar
 *   blue   the bracket at the frame's left edge, the tick on the front rail, the playhead dot (runs while notes
 *          play, rests at the frame's left end in silence)
 *   white  the low-pass curve, its knee following CUTOFF, its peak RESONANCE; four ticks hanging under it
 *   red    1 to 4 arcs over the peak (RESONANCE)
 *   lav.   drop lines from the frame's ends to the floor; the harmonics readout (harmonics 2..10 of the frame)
 * The level: wavetable.c keeps per voice the block peak times the stock envelope; the screen sums the voices that
 * rendered lately, maps it on a log scale (8 .. 23000 of 32767 -> 0 .. 1) and follows it fast up, slowly down. All
 * eases stop at zero, so in silence nothing moves. The curve is recomputed only when RESONANCE changes, the frame's
 * polyline only when WAVE does. */
#include "op1_plugin.h"
#include "wavetable_geom.h"

extern volatile uint32_t wavetable_blocks, wavetable_vblk[OP1_VOICES];
extern volatile int16_t wavetable_vlev[OP1_VOICES];
int32_t wavetable_shape(int32_t k0, uint32_t ph);
uint32_t wavetable_drift(int v);

static const uint8_t C_GREEN[4] = { 0x00, 0xED, 0x95, 0xFF }, C_GREEN_DIM[4] = { 0x00, 0x8E, 0x59, 0xFF };
static const uint8_t C_WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, C_RED[4] = { 0xFF, 0x3A, 0x5D, 0xFF };
static const uint8_t C_BLUE[4] = { 0x69, 0x8E, 0xFF, 0xFF }, C_LAV[4] = { 0xDE, 0xD7, 0xFF, 0xFF };

static uint32_t s_last, s_drift, s_ph;
static int32_t s_act, s_lvl;                    /* activity Q10, level Q15 */
static int32_t s_k0 = -1, s_k3 = -1, s_nwave, s_ncurve, s_peak;
static int32_t s_wx[WT_NW + 1], s_wy[WT_NW + 1];    /* one period of the frame, simplified: x Q10 px, value */
static int32_t s_cy[WT_CN];                     /* the curve on its 1 px grid, y Q10 */
static int16_t s_ci[WT_CN];                     /* the grid points the simplified curve keeps */

static inline int32_t sdeg(int32_t d10) { return OP1_SINE_TABLE[((d10 * 2330) >> 10) & 8191]; }   /* 0.1 degree */
static inline int32_t cdeg(int32_t d10) { return OP1_SINE_TABLE[(((d10 * 2330) >> 10) + 2048) & 8191]; }

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)   /* Q10 */
{
    const op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}

/* log2 of v (> 0) in Q8 */
static int32_t lg8(int32_t v)
{
    int n = 31 - __builtin_clz((uint32_t)v);
    const int32_t f = n >= 8 ? (v >> (n - 8)) & 255 : (v << (8 - n)) & 255;
    return n * 256 + f;
}

/* log2(x / 65536) in Q16, x > 0 */
static int32_t log2_q16(uint32_t x)
{
    const int n = 31 - __builtin_clz(x);
    const uint32_t m = n <= 30 ? x << (30 - n) : x >> (n - 30);
    const int64_t f = (m & 0x3FFFFFFFu) >> 14;
    int64_t acc = WT_LOG_C[0];
    for (int i = 1; i < 6; i++) acc = ((acc * f) >> 16) + WT_LOG_C[i];
    return (n - 16) * 65536 + (int32_t)acc;
}

/* 2^(e / 65536) in Q16 */
static uint32_t exp2_q16(int32_t e)
{
    const int n = e >> 16;
    const int64_t f = e & 0xFFFF;
    int64_t acc = WT_EXP_C[0];
    for (int i = 1; i < 5; i++) acc = ((acc * f) >> 16) + WT_EXP_C[i];
    return n >= 0 ? (uint32_t)acc << n : (uint32_t)acc >> -n;
}

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* keep from n values v[] only the points the line needs within tol: out[] gets their indices, returns the count - 1 */
static int simplify(const int32_t *v, int n, int32_t tol, int16_t *out)
{
    int a = 0, m = 0;
    out[0] = 0;
    while (a < n - 1) {
        int b = a + 1;
        while (b < n - 1) {                       /* extend while every point between stays on the line a..b+1 */
            int ok = 1;
            for (int j = a + 1; j <= b; j++) {
                const int32_t y = v[a] + (v[b + 1] - v[a]) * (j - a) / (b + 1 - a);
                if (y - v[j] > tol || v[j] - y > tol) { ok = 0; break; }
            }
            if (!ok) break;
            b++;
        }
        out[++m] = (int16_t)b;
        a = b;
    }
    return m;
}

/* one period of the frame at WAVE k0: 64 samples, then only the points the line needs (0.2 px at the frame's height) */
static void wave_period(int32_t k0)
{
    int32_t v[WT_NW + 1];
    int16_t idx[WT_NW + 1];
    for (int j = 0; j < WT_NW; j++) v[j] = wavetable_shape(k0, (uint32_t)j * (0xFFFFFFFFu / WT_NW + 1));
    v[WT_NW] = v[0];
    s_nwave = simplify(v, WT_NW + 1, WT_PEAK * 205 / WT_H, idx);
    for (int i = 0; i <= s_nwave; i++) { s_wx[i] = WT_W * idx[i] / WT_NW; s_wy[i] = v[idx[i]]; }
}

/* the concept's 2-pole response on a 1 px grid round the knee (offset i - WT_CI px; d = offset / 300): r^2 =
   10^(4.4 d), den = (1 - r^2)^2 + r^2 / Q^2, y = flat + 9 log10(den) px, clamped to the panel; Q = 0.7 + 7 res^1.5 */
static void curve_shape(int32_t k3)
{
    const int32_t r15 = (int32_t)(((int64_t)k3 * isqrt((uint32_t)k3 << 15)) >> 15);
    const uint32_t q16 = 45875u + ((uint32_t)(7 * r15) << 1);
    const int64_t invq = 0xFFFFFFFFu / q16, invq2 = (invq * invq) >> 16;
    int32_t best = 0x7FFFFFFF;
    s_peak = WT_CI;
    for (int i = 0; i < WT_CN; i++) {
        const int64_t r2 = exp2_q16((i - WT_CI) * 3193), a = 65536 - r2;
        int64_t den = ((a * a) >> 16) + ((r2 * invq2) >> 16);
        if (den < 1) den = 1;
        if (den > 0x7FFFFFFF) den = 0x7FFFFFFF;
        int32_t y = WT_FLAT_Y * 1024 + (int32_t)(((int64_t)2774 * log2_q16((uint32_t)den)) >> 16);
        if (y < WT_TOP_Y * 1024) y = WT_TOP_Y * 1024;
        if (y > WT_BOT_Y * 1024) y = WT_BOT_Y * 1024;
        s_cy[i] = y;
        if (k3 > 655 && y < best) { best = y; s_peak = i; }
    }
    s_ncurve = simplify(s_cy, WT_CN, 205, s_ci);
}

/* a frame's polyline from x0, repeated with period WT_W from x0 - off and clipped to x0 .. x0 + WT_W */
static void draw_frame(void *r, int32_t x0, int32_t y0, int32_t amp, int32_t off, int32_t w, const uint8_t *c)
{
    const int32_t sc = (int32_t)(((int64_t)amp << 15) / WT_PEAK);
    const int32_t x1 = x0 + WT_W;
    for (int32_t base = x0 - off; base < x1; base += WT_W) {
        for (int i = 0; i < s_nwave; i++) {
            int32_t xa = base + s_wx[i], xb = base + s_wx[i + 1];
            if (xb <= x0 || xa >= x1) continue;
            int32_t ya = y0 - (int32_t)(((int64_t)s_wy[i] * sc) >> 15), yb = y0 - (int32_t)(((int64_t)s_wy[i + 1] * sc) >> 15);
            if (xa < x0 && xb > xa) { ya += (int32_t)((int64_t)(yb - ya) * (x0 - xa) / (xb - xa)); xa = x0; }
            if (xb > x1 && xb > xa) { yb -= (int32_t)((int64_t)(yb - ya) * (xb - x1) / (xb - xa)); xb = x1; }
            ln(r, xa, ya, xb, yb, w, c);
        }
    }
}

/* the white curve, its knee at kx (Q10 px), clipped to the panel; the flat line left of the grid, the floor right */
static void draw_curve(void *r, int32_t kx)
{
    int32_t px = kx - WT_CI * 1024, py = s_cy[0];
    if (px > WT_PX0) ln(r, WT_PX0, py, px, py, WT_W_WHITE, C_WHITE);
    for (int i = 1; i <= s_ncurve + 1 && px < WT_PX1; i++) {
        int32_t x, y;
        if (i <= s_ncurve) { x = kx + (s_ci[i] - WT_CI) * 1024; y = s_cy[s_ci[i]]; }
        else { x = WT_PX1; y = WT_BOT_Y * 1024; }
        if (x <= px) continue;
        int32_t ax = px, ay = py, bx = x, by = y;
        px = x; py = y;
        if (bx <= WT_PX0) continue;
        if (bx > WT_PX1) { by = ay + (int32_t)((int64_t)(by - ay) * (WT_PX1 - ax) / (bx - ax)); bx = WT_PX1; }
        if (ax < WT_PX0) { ay += (int32_t)((int64_t)(by - ay) * (WT_PX0 - ax) / (bx - ax)); ax = WT_PX0; }
        if (bx > ax) ln(r, ax, ay, bx, by, WT_W_WHITE, C_WHITE);
    }
    for (int t = 0; t < 4; t++) {                 /* ticks hanging under the curve round the knee */
        const int i = WT_CI + WT_TICK_OFF[t];
        const int32_t x = kx + WT_TICK_OFF[t] * 1024, y = s_cy[i];
        int32_t len = WT_BOT_Y * 1024 - y;
        if (len > WT_TICK_LEN) len = WT_TICK_LEN;
        if (x < WT_PX0 || x > WT_PX1 || len < 512) continue;
        ln(r, x, y + 512, x, y + len, WT_TICK_W[t], C_WHITE);
    }
}

/* n arcs over (px, py): radius 5 + 4i px, ends at +-0.8 r, -0.3 r (the circle's centre 0.3 r under the peak) */
static void draw_arcs(void *r, int32_t px, int32_t py, int n)
{
    for (int a = 0; a < n; a++) {
        const int32_t rad = (5 + 4 * a) * 1024, cy = py + rad * 3 / 10;
        int32_t qx = 0, qy = 0;
        for (int s = 0; s <= 6; s++) {
            const int32_t e = 1431 - 1063 * s / 6;   /* 143.1 .. 36.9 degrees */
            const int32_t x = px + ((rad >> 5) * cdeg(e) >> 10), y = cy - ((rad >> 5) * sdeg(e) >> 10);
            if (s) ln(r, qx, qy, x, y, WT_W_RED, C_RED);
            qx = x; qy = y;
        }
    }
}

static void wavetable_screen_init(void *scr) { (void)scr; }

static void wavetable_screen_update(void *scr, int dt)
{
    int16_t k[4];
    op1_synth_knobs_live(OP1_SCREEN_SLOT & 7, k);
    for (int i = 0; i < 4; i++) if (k[i] < 0) k[i] = 0;
    const uint32_t blocks = wavetable_blocks;
    const int on = blocks != s_last;
    s_last = blocks;
    s_act += ((on ? 1024 : 0) - s_act) >> 3;
    if (!on && s_act < 16) s_act = 0;
    /* the level: the voices that rendered in about the last two frames (every voice call counts a block) */
    int32_t sum = 0, best = -1, bl = -1;
    for (int v = 0; v < OP1_VOICES; v++)
        if (wavetable_vblk[v] && blocks - wavetable_vblk[v] < 96u) {   /* 0: never rendered */
            sum += wavetable_vlev[v];
            if (wavetable_vlev[v] > bl) { bl = wavetable_vlev[v]; best = v; }
        }
    int32_t t = 0;
    if (on && sum > 8) {
        t = (lg8(sum > 32767 ? 32767 : sum) - 3 * 256) * 32767 / 2944;              /* log2 3 .. 14.5 */
        if (t > 32767) t = 32767;
    }
    s_lvl += (t - s_lvl) >> (t > s_lvl ? 1 : 3);
    if (!t && s_lvl < 16) s_lvl = 0;
    if (best >= 0) s_drift = wavetable_drift(best) - 0x55555555u;   /* relative to the note-on offset */
    if (s_act) s_ph += (uint32_t)s_act * 0x12000u;                   /* the playhead: about one run a second */
    else s_ph = 0;
    if (k[0] != s_k0) { s_k0 = k[0]; wave_period(k[0]); }
    if (k[3] != s_k3) { s_k3 = k[3]; curve_shape(k[3]); }

    op1_screen_draw_cached(scr, dt, 0);                           /* wavetable.svg is all still: cached whole */
    void *r = op1_raster();
    const int32_t f = k[0] * 9216 / 32767;                        /* depth in frames, Q10 */
    const int32_t ox = WT_X0 + (f * WT_DX >> 10), oy = WT_Y0 + (f * WT_DY >> 10);
    const int32_t amp = WT_H * (1024 + (WT_SWELL * (s_lvl >> 5) >> 10)) >> 10;
    /* harmonics (under the green frames, which may cross them near WAVE max): the table's entry at WAVE, interpolated */
    const int32_t hp = k[0] * (WT_NPOS - 1), hi = hp >> 15, hf = hp & 0x7FFF;
    const int32_t hs = 1024 + (WT_HB_SWELL * (s_lvl >> 5) >> 10);
    for (int i = 0; i < WT_NH; i++) {
        const int32_t a = WT_HARM[hi][i], b = WT_HARM[hi + (hi < WT_NPOS - 1)][i];
        const int32_t hq = (a * 32768 + (b - a) * hf) >> 15;                               /* 0 .. 255 */
        const int32_t h = (hq * WT_HB_H / 255) * hs >> 10;                                 /* Q10 px */
        if (h < 300) continue;
        const int32_t x = WT_HB_X0 + WT_HB_DX * i;
        ln(r, x, WT_HB_Y, x, WT_HB_Y - h, WT_W_BAR, i ? C_LAV : C_GREEN);
    }
    /* the copy: 0.04 + 0.12 DETUNE turns ahead, plus the drift */
    const uint32_t cph = ((uint32_t)(WT_COPY_PH0 + (WT_COPY_PHD * k[1] >> 15)) << 16) + s_drift;
    const int32_t coff = (int32_t)(((int64_t)(cph >> 16) * WT_W) >> 16);
    draw_frame(r, ox + WT_COPY_DX, oy, amp, coff, WT_W_COPY, C_GREEN_DIM);
    draw_frame(r, ox, oy, amp, 0, WT_W_GREEN, C_GREEN);
    /* drop lines from the frame's ends to the floor */
    ln(r, ox, oy + 512, ox, oy + WT_FLOOR, WT_W_LAV, C_LAV);
    ln(r, ox + WT_W, oy + 512, ox + WT_W, oy + WT_FLOOR, WT_W_LAV, C_LAV);
    /* blue: the bracket and the rail tick */
    const int32_t bx = ox - 5 * 1024, bt = oy - WT_H - 4 * 1024, bb = oy + WT_H + 4 * 1024;
    ln(r, bx + 3 * 1024, bt, bx, bt, WT_W_BLUE, C_BLUE);
    ln(r, bx, bt - 700, bx, bb + 700, WT_W_BLUE, C_BLUE);
    ln(r, bx, bb, bx + 3 * 1024, bb, WT_W_BLUE, C_BLUE);
    ln(r, ox, oy + WT_FLOOR, ox, oy + WT_FLOOR + 4 * 1024, WT_W_BLUE, C_BLUE);
    /* the playhead */
    const int32_t hx = ox + (int32_t)(((int64_t)(s_ph >> 16) * WT_W) >> 16);
    const int32_t hy = oy - (int32_t)(((int64_t)wavetable_shape(k[0], s_ph) * amp) / WT_PEAK);
    ln(r, hx - 1100, hy, hx + 1100, hy, WT_W_DOT, C_BLUE);
    /* the filter panel */
    const int32_t kx = WT_KNEE0 + (k[2] * 15 >> 1);
    draw_curve(r, kx);
    int32_t px = kx + (s_peak - WT_CI) * 1024;
    if (px < WT_PX0) px = WT_PX0;
    if (px > WT_PX1) px = WT_PX1;
    draw_arcs(r, px, s_cy[s_peak], 1 + k[3] * 3 / 32767);
}

const op1_screen_t SCREEN_wavetable = { wavetable_screen_init, wavetable_screen_update };
