/* "lab" screen for the stock iter synth: display/iterlab.svg (iter-lab.svg from op1repacker, MIT License,
 * (c) 2016 op1hacks) with subtle animation. Static drawing = SVG; moving parts = node rotations + raster lines.
 *
 *   knobs  : the four gauges (bar0..3: blue, green, white, red) turn 0..270 deg from their rest position;
 *            white knob: orbit speed of the dots in the white circle (top);
 *            red knob  : falling dashes in the top-right flask (speed, still at minimum)
 *   notes  : round flask - bubbles rise in the liquid (count with the level, speed with the blue knob)
 *            burner      - the flame glows brighter with the iter output level; fumes flicker above it
 *            red box     - corner dots = number of sounding voices (up to 4)
 *            display     - shows the last played midi note while a voice sounds
 *
 * Live note data (dispatcher.c, iter voices only): op1_iter_level[6] = per-voice output peak (Q15, decaying),
 * last note = VoiceCtl held-note list (0x01DFE734, stride 5 words, count 0x01DFF134). */
#include "../op1_synth.h"

extern volatile int16_t op1_iter_level[6];
#ifndef OP1_HOST
#define HELD_COUNT()  (*(volatile int *)0x01DFF134)
#define HELD_NOTE(i)  (((volatile int *)0x01DFE734)[(i) * 5])
#else
extern int host_held_count, host_held_note;
#define HELD_COUNT()  (host_held_count)
#define HELD_NOTE(i)  (host_held_note)
#endif

static const uint8_t C_BLUE[4]  = { 0x69, 0x8E, 0xFF, 0xFF };
static const uint8_t C_GREEN[4] = { 0x00, 0xED, 0x95, 0xFF };
static const uint8_t C_RED[4]   = { 0xFF, 0x3A, 0x5D, 0xFF };
static const uint8_t C_DOTS[4]  = { 0xAE, 0xB1, 0xDC, 0xFF };
static const uint8_t C_DASH[4]  = { 0x8A, 0x66, 0xA4, 0xFF };
#include "iterlab_flame.h"

/* gauge pivots (needle x1/y1 in the SVG); needles are drawn at the minimum position */
static const int16_t PIV[4][2] = { { 30, 108 }, { 158, 133 }, { 168, 53 }, { 265, 58 } };
static void *bars[4];
static uint32_t t_frames, orbit_ph, bubble_ph, dash_ph;
static int32_t lvl;                       /* displayed level, Q15 */
static int16_t shown_note = -1;

static inline int32_t tsin(int32_t a) { return op1_sin(a & 8191); }
static inline int32_t tcos(int32_t a) { return tsin(a + 2048); }

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
/* dot: the rasteriser drops zero-length lines and draws flat ends -> two crossing segments */
static void dot(void *r, int32_t x, int32_t y, int32_t w, const uint8_t *c)
{
    ln(r, x - w / 2, y, x + w / 2, y, w * 7 / 10, c);
    ln(r, x, y - w / 2, x, y + w / 2, w * 7 / 10, c);
}

static void lab_init(void *scr)
{
    static const char *ids[4] = { "bar0", "bar1", "bar2", "bar3" };
    for (int i = 0; i < 4; i++) bars[i] = op1_find_node(scr, ids[i]);
}

/* 7-segment digit in the display boxes: x0 = left edge, boxes y 95..100 and 105..110 */
static void digit(void *r, int x0, int d)
{
    static const uint8_t seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };   /* gfedcba */
    if (d < 0 || d > 9) return;
    uint8_t m = seg[d];
    int32_t L = OP1_Q10(x0), R = OP1_Q10(x0 + 10), w = 0x600;
    if (m & 0x01) ln(r, L, OP1_Q10(95), R, OP1_Q10(95), w, C_GREEN);                     /* a */
    if (m & 0x02) ln(r, R, OP1_Q10(95), R, OP1_Q10(100), w, C_GREEN);                    /* b */
    if (m & 0x04) ln(r, R, OP1_Q10(105), R, OP1_Q10(110), w, C_GREEN);                   /* c */
    if (m & 0x08) ln(r, L, OP1_Q10(110), R, OP1_Q10(110), w, C_GREEN);                   /* d */
    if (m & 0x10) ln(r, L, OP1_Q10(105), L, OP1_Q10(110), w, C_GREEN);                   /* e */
    if (m & 0x20) ln(r, L, OP1_Q10(95), L, OP1_Q10(100), w, C_GREEN);                    /* f */
    if (m & 0x40) { ln(r, L, OP1_Q10(100), R, OP1_Q10(100), w, C_GREEN);                 /* g */
                    ln(r, L, OP1_Q10(105), R, OP1_Q10(105), w, C_GREEN); }
}

static void lab_update(void *scr, int dt)
{
    int16_t k[4];
    op1_synth_knobs_live(OP1_SCREEN_SLOT & 7, k);
    /* live level: loudest iter voice; voices sounding */
    int32_t tgt = 0; int nv = 0;
    for (int i = 0; i < 6; i++) { int32_t l = op1_iter_level[i]; if (l > tgt) tgt = l; if (l > 600) nv++; }
    lvl = tgt > lvl ? tgt : lvl - (lvl >> 3);
    t_frames++;
    orbit_ph += 20 + ((uint32_t)k[2] >> 8) + ((uint32_t)lvl >> 9);          /* slow; white knob + notes */
    bubble_ph += 64 + ((uint32_t)k[0] >> 5);                               /* blue knob: bubble speed */
    dash_ph += (uint32_t)k[3] >> 6;                                        /* red knob: dash speed (0 = still) */
    if (HELD_COUNT() > 0) { int n = HELD_NOTE(HELD_COUNT() - 1); if (n >= 0 && n < 128) shown_note = (int16_t)n; }

    for (int i = 0; i < 4; i++) if (bars[i]) {                             /* gauges: 0..270 deg */
        int32_t a = (int32_t)k[i] * 6144 >> 15;
        op1_node_rotate(bars[i], OP1_Q10(PIV[i][0]), OP1_Q10(PIV[i][1]), tsin(a), tcos(a));
    }
    op1_screen_draw(scr, dt);
    void *r = op1_raster();

    /* orbit: centre dot + 5 dots on a ring around (197.5, 17) */
    dot(r, OP1_Q10(197.5), OP1_Q10(17), 0xA00, C_DOTS);
    for (int i = 0; i < 5; i++) {
        int32_t a = (int32_t)(orbit_ph >> 2) + i * 1638;
        dot(r, OP1_Q10(197.5) + 7 * tcos(a) / 32, OP1_Q10(17) + 7 * tsin(a) / 32, 0xA00, C_DOTS);
    }
    /* top-right flask: dashes falling in 5 columns (12 dashes), wrapping inside each column */
    static const int8_t col_x2[5] = { 3, 9, 15, 21, 27 };                 /* x = 290 + n/2 -> 291.5 .. 303.5 */
    for (int c = 0; c < 5; c++) {
        int top = (c & 1) ? 8 : 10, h = (c & 1) ? 16 : 12, len = (c & 1) ? 4 : 5, n = (c & 1) ? 3 : 2;
        int32_t x = OP1_Q10(290) + col_x2[c] * 512;
        for (int j = 0; j < n; j++) {
            int32_t off = (int32_t)(((dash_ph >> 4) + (uint32_t)(j * h * 1024 / n) + (uint32_t)c * 700) % (uint32_t)(h * 1024));
            int32_t y0 = OP1_Q10(top) + off, y1 = y0 + OP1_Q10(len);
            int32_t bot = OP1_Q10(top + h);
            if (y1 <= bot) ln(r, x, y0, x, y1, 0x400, C_DASH);
            else {                                                         /* wraps: split */
                if (y0 < bot) ln(r, x, y0, x, bot, 0x400, C_DASH);
                ln(r, x, OP1_Q10(top), x, OP1_Q10(top) + (y1 - bot), 0x400, C_DASH);
            }
        }
    }
    if (lvl < 300) return;                                                 /* idle: static lab */

    /* flame: outline over the dim one, dim blue -> blue -> near white with the level */
    {
        uint8_t fc[4]; int32_t t = lvl > 32767 ? 32767 : lvl;
        static const uint8_t lo[3] = { 0x4E, 0x4A, 0x9E }, mid[3] = { 0x69, 0x8E, 0xFF }, hi[3] = { 0xDF, 0xE6, 0xFF };
        for (int i = 0; i < 3; i++)
            fc[i] = (uint8_t)(t < 16384 ? lo[i] + ((mid[i] - lo[i]) * t >> 14) : mid[i] + ((hi[i] - mid[i]) * (t - 16384) >> 14));
        fc[3] = 0xFF;
        for (unsigned i = 1; i < sizeof FLAME / sizeof FLAME[0]; i++)
            ln(r, FLAME[i - 1][0], FLAME[i - 1][1], FLAME[i][0], FLAME[i][1], 0x600, fc);
    }

    /* flask bubbles: up to 4, rising from y 77 to the liquid surface (60) */
    int nb = 1 + (lvl * 4 >> 15); if (nb > 4) nb = 4;
    static const int8_t bx[4] = { -6, 4, -1, 8 };
    for (int i = 0; i < nb; i++) {
        uint32_t p = ((bubble_ph >> 6) + i * 23) % 64;                     /* 0..63 along the rise */
        int32_t y = OP1_Q10(77) - (int32_t)(p * OP1_Q10(17) / 64);
        int32_t x = OP1_Q10(30 + bx[i]) + (tsin((int32_t)(p * 400 + i * 2000)) >> 6);   /* slight wobble */
        dot(r, x, y, 0x900 + (int32_t)(p << 4), C_BLUE);
    }
    /* fumes above the burner: three dash columns at x 27/30/33, y 84..92, flicker with the level */
    int rows = 1 + (lvl * 3 >> 15); if (rows > 3) rows = 3;
    for (int c = 0; c < 3; c++) for (int j = 0; j < rows; j++) {
        if (((t_frames >> 1) + c * 3 + j) % 4 == 0) continue;               /* flicker */
        int32_t y = OP1_Q10(91 - j * 3);
        ln(r, OP1_Q10(27 + c * 3), y, OP1_Q10(27 + c * 3), y - OP1_Q10(2), 0x500, C_BLUE);
    }
    /* red box corners = sounding voices */
    static const int16_t corner[4][2] = { { 250, 50 }, { 280, 50 }, { 250, 65 }, { 280, 65 } };
    for (int i = 0; i < nv && i < 4; i++) dot(r, OP1_Q10(corner[i][0]), OP1_Q10(corner[i][1]), 0xA00, C_RED);
    /* display: last note, 3 digits (leading blanks) */
    if (shown_note >= 0) {
        int n = shown_note;
        if (n >= 100) digit(r, 120, n / 100);
        if (n >= 10) digit(r, 140, n / 10 % 10);
        digit(r, 160, n % 10);
    }
}

const op1_screen_t SCREEN_iterlab = { lab_init, lab_update };
