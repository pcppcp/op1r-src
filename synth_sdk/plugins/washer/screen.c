/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* The washing machine screen (washer.svg, docs/design/engine-screens/round5/build_round5.py) in motion; drum
 * centre (100, 76), screen px:
 *   arrows   two blue arrows round the rim (radius 69), drawn live: 25..100 degrees long and spinning faster
 *            with the rate
 *   water    the drawn line is hidden; the green water inside the glass is drawn live, swinging higher with
 *            the depth and running with the LFO
 *   socks    the blue, green and white sock (one sock and its chorus copies) swing with the LFO about the drum
 *            centre, 8..30 degrees with the depth
 *   bub0..8  the white foam: 2 .. 9 bubbles with the feedback, bobbing while sound goes through
 *   sock     the red sock that got away: 45 px left of its drawing with no effect, 15 px right with all of it;
 *            it hops while sound goes through
 */
#ifndef FX_HOST_TEST
#include "hk/fx.h"
#include "hk/engine.h"

extern volatile uint32_t washer_phase, washer_blocks;
extern volatile int32_t washer_level;
extern volatile int16_t washer_knobs[FX_INSTANCES][4];
extern volatile uint32_t washer_kblocks[FX_INSTANCES];

#define PX(v) ((int32_t)(v) << 10)
#define ONE 1024
#define CX PX(100)
#define CY PX(76)

static void *e_water, *e_socks, *e_sock, *e_bub[9];
static uint32_t last_blocks, arrows, t;
static int32_t act;

static const uint8_t blue[4] = {0x69, 0x8E, 0xFF, 0xFF}, green[4] = {0x00, 0xED, 0x95, 0xFF};

static uint32_t kseen[FX_INSTANCES];
static int32_t kn[4];   /* the params this frame, as the effect hears them (after the LFO) */

void engine_screen_load(void *screen)
{
    static const char *const bubs[9] = {"bub0", "bub1", "bub2", "bub3", "bub4", "bub5", "bub6", "bub7", "bub8"};
    for (int i = 0; i < 9; i++) e_bub[i] = SVG_FIND(screen, bubs[i]);
    e_water = SVG_FIND(screen, "water");
    e_socks = SVG_FIND(screen, "socks");
    e_sock = SVG_FIND(screen, "sock");
}

void engine_screen_animate(void *screen)
{
    (void)screen;
    fx_screen_snapshot(washer_knobs, washer_kblocks, kseen, kn);
    const uint32_t blocks = washer_blocks;
    const int on = blocks != last_blocks && washer_level > 64;
    last_blocks = blocks;
    act += ((on ? ONE : 0) - act) >> 3;
    t++;

    const uint32_t lfo = washer_phase >> 19;                       /* 8192 per LFO cycle */
    /* arrows (drawn in engine_screen_draw): up to ~1/30 turn per frame with the rate */
    arrows += 16 + (uint32_t)(kn[0] >> 7);
    svg_move(e_water, 0, PX(400));                                 /* drawn live */
    /* socks: swing 8 .. 30 degrees (182 .. 683 of 8192) with the depth */
    const int32_t swing = 182 + ((kn[1] * 501) >> 15);
    svg_turn(e_socks, (uint32_t)((SVG_SIN(lfo) * swing) >> 15), CX, CY);
    /* foam: 2 .. 9 bubbles with the feedback; the rest out of view; they bob up to 2 px while sound goes through */
    const int n = 2 + ((kn[2] * 7 + 16384) >> 15);
    for (int i = 0; i < 9; i++) {
        const int32_t bob = (((SVG_SIN(t * 220 + (uint32_t)i * 1500) + 32767) >> 5) * act) >> 10;   /* 0 .. 2 px */
        svg_move(e_bub[i], 0, i < n ? -bob : PX(400));
    }
    /* escaped sock: -45 .. +15 px with the mix (further would leave the screen); hops while sound goes through */
    const int32_t s = SVG_SIN(t * 600);
    const int32_t hop = act > 64 ? (((s < 0 ? -s : s) * act) >> 15) : 0;
    svg_move(e_sock, -PX(45) + (int32_t)(((int64_t)kn[3] * PX(60)) >> 15), -hop * 2);
}

static void line(void *canvas, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    const svg_pt a = {x0, y0}, b = {x1, y1};
    SVG_LINE(canvas, &a, &b, w, c);
}

/* point on a circle about the drum, radius r px, angle a (8192 per turn) */
static void at(uint32_t a, int32_t r, int32_t *x, int32_t *y)
{
    *x = CX + ((SVG_SIN(a + 2048) * r) >> 5);
    *y = CY + ((SVG_SIN(a) * r) >> 5);
}

/* inside the glass (radius 49), compared in 1/16 px so the squares stay in 32 bits */
static int inside(int32_t x, int32_t y)
{
    const int32_t dx = (x - CX) >> 6, dy = (y - CY) >> 6;
    return dx * dx + dy * dy < (49 * 16) * (49 * 16);
}

void engine_screen_draw(void *screen, void *canvas)
{
    (void)screen;
    if (!canvas) return;
    /* arrows: two arcs on radius 69, each 25..100 degrees with the rate, opposite each other and turning; an
       arrowhead (radius 65..73, 14 degrees back) at the leading end */
    const uint32_t sweep = 569 + (uint32_t)((kn[0] * 1707) >> 15);
    enum { N = 12 };
    for (int e = 0; e < 2; e++) {
        const uint32_t a0 = arrows + (uint32_t)e * 4096 - sweep / 2;
        int32_t px, py, x, y;
        at(a0, 69, &px, &py);
        for (int i = 1; i <= N; i++) {
            at(a0 + sweep * (uint32_t)i / N, 69, &x, &y);
            line(canvas, px, py, x, y, 0xC00, blue);           /* 3 px */
            px = x;
            py = y;
        }
        int32_t hx, hy;
        at(a0 + sweep - 319, 65, &hx, &hy);
        line(canvas, hx, hy, px, py, 0xC00, blue);
        at(a0 + sweep - 319, 73, &hx, &hy);
        line(canvas, hx, hy, px, py, 0xC00, blue);
    }
    /* water: 2.3 waves across the glass, 30 px below the centre, 2 .. 9 px high with the depth, running with the
       LFO; only the parts inside the glass */
    const uint32_t lfo = washer_phase >> 19;
    const int32_t amp = 2 + ((kn[1] * 7) >> 15);
    enum { W = 48 };
    int32_t px = 0, py = 0, pin = 0;
    for (int i = 0; i <= W; i++) {
        const int32_t x = CX - PX(51) + i * PX(102) / W;
        const int32_t y = CY + PX(30) - SVG_SIN((uint32_t)(i * 8192 * 23 / (W * 10)) + lfo) * amp / 32;
        const int in = inside(x, y);
        if (i && in && pin) line(canvas, px, py, x, y, 0x99A, green);   /* 2.4 px */
        px = x;
        py = y;
        pin = in;
    }
}
#endif
