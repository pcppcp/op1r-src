/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* The folk-art moose crown (elg.svg + elg_geom.h, both from gen_svg.py and the owner's approved concept elg-art.png)
 * in motion. elg.svg holds the still parts (landscape, knotwork, moon ring and clouds, the fur under the breath) and
 * the two breath clouds; this file draws the rest with raster lines from elg_geom.h, so R246's drawing heap doesn't
 * hold them: the aurora, the moon's glow, the antlers, the fur and face shading, the stars. iter's draw turns the
 * hidden bar0..bar3. The knobs are scene parts (screen px):
 *   antlers  blue Interval: the antlers spread with the third voice's interval (0 .. 4: unison .. two octaves):
 *            93 .. 107 % wide and 96.5 .. 103.5 % tall about their base (160, 59), the concept's size at the octave
 *            (the default); they glide there (a one-pole per frame, ~0.3 s) and ignore the slot LFO
 *   breath   green Detune: each breath cloud is 87.5 .. 137.5 % about its nostril (100 % at the default), and puffs
 *            up to 6 % more with the level
 *   moon     white Timbre: above its default (the concept's pose, no glow) up to three dim glow arcs outside the moon
 *            ring (along the ring's own drawn arcs); a little glow with the level while notes play
 *   aurora   red Movement: the ribbons sink and the fringe stretches in a wave running along them, 0 .. 3 px and
 *            0 .. 45 % with the movement, the wave's depth swinging with Elg's LFO while notes play
 *   stars    twinkle while notes play
 * The other knob parts follow the slot LFO while notes play (elg_knobs, copied by engine_render).
 */
#ifndef ENGINE_HOST_TEST
#include "hk/engine.h"
#include "elg_geom.h"

extern volatile uint32_t elg_blocks;
extern volatile int32_t elg_level, elg_lfo;
extern volatile int16_t elg_knobs[4];
extern int elg_interval(int32_t value);

#define ONE 1024
#define Q2Q10(v) ((int32_t)(v) << 8)            /* quarter px -> Q10 */

static void *e_breath_l, *e_breath_r;
static uint32_t last_blocks, aph, tph;
static int32_t act, asx, asy;                    /* the antlers' spread, Q10, 0 = not set yet */
static int32_t aur_amp, aur_str, glow, playing;  /* this frame's aurora drop (Q10 px) and stretch (Q10), glow 0..3072 */
/* a knob as the voices hear it (after the slot LFO) while notes play, else as set */
#define K(i) screen_knob_live(elg_knobs, (i), act > 64)

static const uint8_t C_RED[4] = {0xFF, 0x3A, 0x5D, 0xFF}, C_BLUE[4] = {0x69, 0x8E, 0xFF, 0xFF},
                     C_BROWN[4] = {0xB0, 0x7A, 0x50, 0xFF}, C_LAV[4] = {0xDE, 0xD7, 0xFF, 0xFF},
                     C_WHITE[4] = {0xFF, 0xFF, 0xFF, 0xFF};

void engine_screen_load(void *screen)
{
    e_breath_l = SVG_FIND(screen, "breathL");
    e_breath_r = SVG_FIND(screen, "breathR");
}

void engine_screen_animate(void *screen)
{
    (void)screen;
    const uint32_t blocks = elg_blocks;
    const int on = blocks != last_blocks;
    last_blocks = blocks;
    act += ((on ? ONE : 0) - act) >> 4;
    playing = act > 64;
    const int32_t level = on ? elg_level : 0, lfo = on ? elg_lfo : 0;

    /* antlers: spread with the interval step (the knob as set, no LFO), gliding there: a one-pole per frame */
    const int32_t step = elg_interval(SCREEN_KNOB(0) < 0 ? 0 : SCREEN_KNOB(0));
    const int32_t tx = ONE + (step - 2) * 36, ty = ONE + (step - 2) * 18;
    asx = asx ? asx + ((tx - asx) >> 4) : tx;
    asy = asy ? asy + ((ty - asy) >> 4) : ty;
    /* breath: 87.5 .. 137.5 % with the detune, plus up to 6 % with the level */
    const int32_t b = 896 + ((K(1) * 512) >> 15) + ((level * act) >> 19);
    svg_scale(e_breath_l, b, b, Q2Q10(EG_NOSE_LX), Q2Q10(EG_NOSE_Y));
    svg_scale(e_breath_r, b, b, Q2Q10(EG_NOSE_RX), Q2Q10(EG_NOSE_Y));
    /* moon glow: none up to the default timbre (the concept's own pose), then up to 3 arcs at the top; plus up to half
       an arc with the level */
    const int32_t t = K(2) - 20000;
    glow = (t > 0 ? (t * 246) >> 10 : 0) + ((level * act) >> 16);
    if (glow > 3072) glow = 3072;
    /* aurora: the wave runs along the ribbons; its depth with the movement, swinging with the LFO while playing */
    const int32_t m = K(3);                                   /* 0 .. 32767 */
    aph += 24 + ((m * 40) >> 15);
    const int32_t swing = 24576 + (lfo >> 2);                 /* 0.75 +- 0.25 */
    aur_amp = (((m * 3072) >> 15) * swing) >> 15;             /* Q10 px, to 3 px */
    aur_str = (((m * 461) >> 15) * swing) >> 15;              /* Q10, to 0.45 */
    tph += playing ? 96 : 0;
}

static void seg(void *canvas, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    const svg_pt a = {x0, y0}, p = {x1, y1};
    SVG_LINE(canvas, &a, &p, w, c);
}

/* the strokes first .. last of elg_geom.h as they are, each at its own measured width */
static void strokes(void *canvas, int first, int last, const uint8_t *c)
{
    for (int s = first; s < last; s++) {
        const int16_t *p = EG_PTS + 2 * EG_STROKE[s][0];
        for (int i = 1; i < EG_STROKE[s][1]; i++, p += 2)
            seg(canvas, Q2Q10(p[0]), Q2Q10(p[1]), Q2Q10(p[2]), Q2Q10(p[3]), EG_STROKE[s][2], c);
    }
}

/* the aurora's drop at x (Q2 px): 0 .. aur_amp, a wave of 75 px running right */
static int32_t drop(int32_t xq2)
{
    const int32_t c = SVG_SIN(aph * 8 - xq2 * 27 + 2048);    /* 8192 a turn: 300 quarter px a period */
    return (aur_amp * (32767 - c)) >> 16;
}

static void aurora(void *canvas)
{
    const int32_t w = EG_W_RED;
    for (int s = EG_RIBBON_0; s < EG_RIBBON_1; s++) {
        const int16_t *p = EG_PTS + 2 * EG_STROKE[s][0];
        int32_t x0 = Q2Q10(p[0]), y0 = Q2Q10(p[1]) + drop(p[0]);
        for (int i = 1; i < EG_STROKE[s][1]; i++) {
            p += 2;
            const int32_t x1 = Q2Q10(p[0]), y1 = Q2Q10(p[1]) + drop(p[0]);
            seg(canvas, x0, y0, x1, y1, w, C_RED);
            x0 = x1, y0 = y1;
        }
    }
    /* the fringe hangs from its top end: dropped with the ribbon, stretched by up to aur_str in a second wave */
    for (int s = EG_FRINGE_0; s < EG_FRINGE_1; s++) {
        const int16_t *p = EG_PTS + 2 * EG_STROKE[s][0];
        const int32_t d = drop(p[0]);
        const int32_t st = ONE + ((aur_str * (32767 + SVG_SIN(aph * 5 + p[0] * 19))) >> 16);
        const int32_t top = Q2Q10(p[1]);
        int32_t x0 = Q2Q10(p[0]), y0 = top + d;
        for (int i = 1; i < EG_STROKE[s][1]; i++) {
            const int16_t *q = p + 2 * i;
            const int32_t x1 = Q2Q10(q[0]), y1 = top + d + (((Q2Q10(q[1]) - top) * st) >> 10);
            seg(canvas, x0, y0, x1, y1, w, C_RED);
            x0 = x1, y0 = y1;
        }
    }
}

static void antlers(void *canvas)
{
    const int32_t bx = Q2Q10(EG_ANT_X), by = Q2Q10(EG_ANT_Y);
    for (int s = EG_ANTLERS_0; s < EG_ANTLERS_1; s++) {
        const int16_t *p = EG_PTS + 2 * EG_STROKE[s][0];
        int32_t x0 = bx + ((Q2Q10(p[0]) - bx) * asx >> 10), y0 = by + ((Q2Q10(p[1]) - by) * asy >> 10);
        for (int i = 1; i < EG_STROKE[s][1]; i++) {
            p += 2;
            const int32_t x1 = bx + ((Q2Q10(p[0]) - bx) * asx >> 10), y1 = by + ((Q2Q10(p[1]) - by) * asy >> 10);
            seg(canvas, x0, y0, x1, y1, EG_STROKE[s][2], C_BLUE);
            x0 = x1, y0 = y1;
        }
    }
}

/* glow arcs 2.5, 5 and 7.5 px outside the ring, along its drawn arcs (each end pulled in 4 degrees an arc so they
   clear the head and antlers), white dimmed: the inner one brightest */
static void moon_glow(void *canvas)
{
    for (int k = 0; k < 3; k++) {
        int32_t f = glow - k * 1024;                          /* this arc's share, Q10 */
        if (f <= 0) break;
        if (f > ONE) f = ONE;
        const int32_t lvl = (f * (120 - 32 * k)) >> 10;       /* 0 .. 120 */
        const uint8_t c[4] = {(uint8_t)lvl, (uint8_t)lvl, (uint8_t)lvl, 0xFF};
        const int32_t r = Q2Q10(EG_MOON_R) + (5 * 512) * (k + 1);
        for (int a = 0; a < EG_NARCS; a++) {
            const int32_t a0 = EG_ARC[a][0] + 4 * (k + 1), a1 = EG_ARC[a][1] - 4 * (k + 1);
            if (a1 - a0 < 4) continue;
            int32_t px = 0, py = 0;
            for (int d = a0;; d += 6) {
                if (d > a1) d = a1;
                const uint32_t ang = (uint32_t)((d * 23302) >> 10) & 8191;   /* degrees -> 8192 a turn, no divide */
                const int32_t x = Q2Q10(EG_MOON_X) + ((r >> 5) * SVG_SIN(ang + 2048) >> 10);
                const int32_t y = Q2Q10(EG_MOON_Y) + ((r >> 5) * SVG_SIN(ang) >> 10);
                if (d != a0) seg(canvas, px, py, x, y, EG_W_WHITE, c);
                px = x, py = y;
                if (d == a1) break;
            }
        }
    }
}

/* four-point sparkles: two arms, a short brighter cross at the middle; the arms breathe while notes play */
static void stars(void *canvas)
{
    for (int i = 0; i < EG_NSTARS; i++) {
        const int32_t x = Q2Q10(EG_STAR[i][0]), y = Q2Q10(EG_STAR[i][1]);
        int32_t r = Q2Q10(EG_STAR[i][2]);
        if (playing) r = (r * (26 + (SVG_SIN(tph * 3 + i * 1500) >> 12))) >> 5;   /* 56 .. 106 % */
        seg(canvas, x - r, y, x + r, y, EG_W_LAV, C_LAV);
        seg(canvas, x, y - r, x, y + r, EG_W_LAV, C_LAV);
        const int32_t h = r >> 1, g = r >> 3;                /* the concave four-point body: a thicker inner cross, a dot */
        seg(canvas, x - h, y, x + h, y, EG_W_LAV * 2, C_LAV);
        seg(canvas, x, y - h, x, y + h, EG_W_LAV * 2, C_LAV);
        seg(canvas, x - g, y, x + g, y, EG_W_LAV * 4, C_LAV);
    }
}

void engine_screen_draw(void *screen, void *canvas)
{
    (void)screen;
    moon_glow(canvas);
    aurora(canvas);
    antlers(canvas);
    strokes(canvas, EG_FUR_0, EG_FUR_1, C_BROWN);
    strokes(canvas, EG_FACE_LAV_0, EG_FACE_LAV_1, C_LAV);
    strokes(canvas, EG_FACE_WHITE_0, EG_FACE_WHITE_1, C_WHITE);
    stars(canvas);
}
#endif
