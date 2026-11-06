/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* The jazz club screen (pew.svg + pew_geom.h from gen_svg.py, traced from the owner's mockup pew-art.png) in motion.
 * The scene is still and drawn once through the core's static-prefix cache (pew_plugin.c); the SVG's only live nodes
 * are the three green notes after it (note0..note2). On top, drawn here every frame:
 *   drawbars   blue (REGISTER) and the page-2 pushes: the 2 x 9 bars on the organ's top stand out as far as the
 *              registration pulls them (pew_macro.h, the same levels the sound uses), sliding there
 *   notes      green (PERCUSSION): each new note sends a note up from the organ to its place; the sharper the
 *              percussion, the bigger it pops and the faster it flies (none with the percussion off, they drift)
 *   smoke      white (DRIVE / CHORUS): the smoke from the cigarette sways and thickens; the lamp glows brighter
 *   rotor      red (LESLIE): the red motion lines round the horn and the drum turn with the audio's rotors, long at
 *              fast, short at slow, gone at the brake's standstill
 *   keys       one key lights per held voice (its key down), like Oxbow's stars
 * Coordinates are screen px in Q10 (the SVG is drawn at 1:1).
 */
#ifndef ENGINE_HOST_TEST
#include "hk/engine.h"
#include "pew_geom.h"
#include "pew_macro.h"

extern volatile uint32_t pew_blocks, pew_horn, pew_drum, pew_strikes;
extern volatile uint8_t pew_held[ENGINE_VOICES];
extern volatile int32_t pew_speed;
extern volatile int16_t pew_knobs[4];
void *pew_first_live;                                 /* note0: the static-prefix cache's boundary (pew_plugin.c) */
void pew_prepare(const int16_t *knobs);               /* pew.c: the first wave tables, outside the audio callback */

#define ONE 1024
#define Q(v) ((int32_t)((v) * 1024))   /* screen px -> Q10 */

static void *e_note[3];
static uint32_t last_blocks, last_strikes, t;
static int32_t act;
static int32_t bar_shown[9];                          /* Q12, eased towards the registration */
static int32_t note_p[3] = {ONE, ONE, ONE};           /* 0 = just struck (at the organ) .. ONE = home */
static int32_t note_pop[3];                           /* Q10 extra scale at the strike */
static int next_note;
/* a knob as the voices hear it (after the slot LFO) while notes play, else as set */
#define K(i) screen_knob_live(pew_knobs, (i), act > 64)

static const uint8_t c_blue[4] = {0x69, 0x8E, 0xFF, 0xFF}, c_green[4] = {0x00, 0xED, 0x95, 0xFF};
static const uint8_t c_white[4] = {0xFF, 0xFF, 0xFF, 0xFF}, c_red[4] = {0xFF, 0x3A, 0x5D, 0xFF};
static const uint8_t c_smoke2[4] = {0x9A, 0x9A, 0xA4, 0xFF};

void engine_screen_load(void *screen)
{
    static const char *const ids[3] = {"note0", "note1", "note2"};
    for (int i = 0; i < 3; i++) e_note[i] = SVG_FIND(screen, ids[i]);
    pew_first_live = e_note[0];
    /* the sound's first tables, here at boot rather than in the first audio block (about 75k packets once) */
    int16_t k[8];
    for (int i = 0; i < 8; i++) k[i] = (int16_t)SCREEN_KNOB(i);
    pew_prepare(k);
}

static int32_t word(int i)
{
    const int32_t v = SCREEN_KNOB(i);
    return v < 0 ? 0 : v;
}

void engine_screen_animate(void *screen)
{
    (void)screen;
    const uint32_t blocks = pew_blocks;
    const int on = blocks != last_blocks;
    last_blocks = blocks;
    act += ((on ? ONE : 0) - act) >> 3;
    t++;

    /* drawbars ease towards the registration */
    int16_t push[3];
    for (int g = 0; g < 3; g++) push[g] = (int16_t)word(5 + g);
    int32_t lv[9];
    pew_registration(K(0), push, lv);
    for (int i = 0; i < 9; i++) bar_shown[i] += (lv[i] - bar_shown[i]) >> 2;

    /* notes: each strike (a note-on) sends the next note up from the organ */
    const int32_t perc = K(1);
    const uint32_t strikes = pew_strikes;
    int n = (int)(strikes - last_strikes);
    last_strikes = strikes;
    if (n > 3) n = 3;
    while (n-- > 0) {
        note_p[next_note] = 0;
        note_pop[next_note] = (perc * 600) >> 15;     /* up to x 1.6 at the strike */
        next_note = next_note == 2 ? 0 : next_note + 1;
    }
    for (int i = 0; i < 3; i++) {
        /* rise: 64 frames with the percussion off, 16 at the sharpest */
        const int32_t step = 16 + ((perc * 48) >> 15);
        note_p[i] += step;
        if (note_p[i] > ONE) note_p[i] = ONE;
        note_pop[i] -= note_pop[i] >> 3;
        const int32_t u = ONE - note_p[i];                         /* 1 .. 0 */
        const int32_t ease = (u * u) >> 10;                        /* ease out */
        const int32_t dy = (ease * Q(20)) >> 10;                   /* from 20 px below its place */
        const int32_t bob = (SVG_SIN(t * 64 + (uint32_t)i * 2730) * 410) >> 15;   /* +-0.4 px */
        const int32_t s = ONE + note_pop[i] - (((ONE - note_p[i]) * 300) >> 10);   /* small far down, pop on top */
        const int32_t cx = pew_note_home[i][0], cy = pew_note_home[i][1];
        if (!e_note[i]) continue;
        const int32_t m[9] = {s, 0, 0, 0, s, 0, cx - ((s * (cx >> 4)) >> 6), cy + dy + bob - ((s * (cy >> 4)) >> 6),
                              1024};
        SVG_MATRIX(e_note[i], m);
    }
}

static void line(void *canvas, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    const svg_pt a = {x0, y0}, b = {x1, y1};
    SVG_LINE(canvas, &a, &b, w, c);
}

static void bars(void *canvas)
{
    for (int m = 0; m < 2; m++)
        for (int i = 0; i < PEW_BAR_N; i++) {
            const uint8_t *c = i < 3 ? c_blue : i < 6 ? c_green : c_white;
            const int32_t x = pew_bar_foot[m][i][0], y = pew_bar_foot[m][i][1];
            const int32_t top = y - Q(1.6) - ((bar_shown[i] * PEW_BAR_LEN) >> 15);
            if (top < y - Q(1.7)) line(canvas, x, y, x, top, PEW_BAR_W, c);       /* the bar */
            line(canvas, x, top, x, top - Q(1.0), PEW_BAR_W + Q(0.7), c);        /* its handle */
        }
}

static void smoke(void *canvas, int32_t white)
{
    /* sway grows up the plume and with white; the plume thickens, a second strand beside it above half */
    const int32_t amp = Q(0.5) + ((white * Q(2.2)) >> 15);
    const int32_t w = Q(0.9) + ((white * Q(1.0)) >> 15);
    for (int s = 0; s < (white > 16384 ? 2 : 1); s++) {
        int32_t px = 0, py = 0;
        for (int i = 0; i < PEW_SMOKE_N; i++) {
            const int32_t h = (i * (ONE * 65536 / PEW_SMOKE_N)) >> 16;                        /* 0 at the cigarette .. 1 on top */
            const int32_t dx = (((SVG_SIN((uint32_t)i * 300 - t * 70 + (uint32_t)s * 1500) * amp) >> 15) * h) >> 10;
            const int32_t x = pew_smoke[i][0] + dx + (s ? Q(1.3) + ((h * Q(1.5)) >> 10) : 0);
            const int32_t y = pew_smoke[i][1];
            if (i && !(s && i < PEW_SMOKE_N / 4)) line(canvas, px, py, x, y, s ? w - Q(0.3) : w, s ? c_smoke2 : c_white);
            px = x;
            py = y;
        }
    }
}

static void lamp(void *canvas, int32_t white)
{
    /* five rays under the bulb, 1 .. 6 px long, dim ochre to warm white */
    uint8_t c[4] = {0, 0, 0, 0xFF};
    static const uint8_t lo[3] = {0x76, 0x59, 0x32}, hi[3] = {0xFF, 0xE6, 0xB8};
    for (int k = 0; k < 3; k++) c[k] = (uint8_t)(lo[k] + (((hi[k] - lo[k]) * white) >> 15));
    const int32_t len = Q(1.0) + ((white * Q(5.0)) >> 15) + ((act * Q(0.4) >> 10) * SVG_SIN(t * 400) >> 15);
    const int32_t r0 = pew_bulb[2] + Q(1.2);
    for (int i = 0; i < 5; i++) {
        const uint32_t a = 2048 - 1300 + (uint32_t)i * 650;                    /* down, +-57 degrees */
        const int32_t cs = SVG_SIN(a + 2048), sn = SVG_SIN(a);
        line(canvas, pew_bulb[0] + ((cs * (r0 >> 5)) >> 10), pew_bulb[1] + ((sn * (r0 >> 5)) >> 10),
             pew_bulb[0] + ((cs * ((r0 + len) >> 5)) >> 10), pew_bulb[1] + ((sn * ((r0 + len) >> 5)) >> 10), Q(0.8), c);
    }
}

/* point on ring r at angle a (8192 per turn), radius scale rs (Q10) */
static void ring_at(int r, uint32_t a, int32_t rs, int32_t *x, int32_t *y)
{
    const int32_t cs = SVG_SIN(a + 2048), sn = SVG_SIN(a);                    /* Q15 */
    const int32_t u = (pew_ring[r][2] * cs) >> 15, v = (pew_ring[r][3] * sn) >> 15;   /* radii < 40 px: 32 bits */
    const int32_t st = pew_ring[r][4], ct = pew_ring[r][5];
    *x = pew_ring[r][0] + ((((u * ct) >> 15) - ((v * st) >> 15)) * rs >> 10);
    *y = pew_ring[r][1] + ((((u * st) >> 15) + ((v * ct) >> 15)) * rs >> 10);
}

static void rotor(void *canvas)
{
    /* the horn's speed (phase per block): the audio's while notes play, else the speed the red knob asks for */
    const int32_t sp = act > 64 ? (pew_speed < 0 ? 0 : pew_speed) : pew_leslie_target(K(3));
    /* sweep: none at a standstill, 35 % of the mockup's at slow, 100 % at fast */
    int32_t f;
    if (sp <= PEW_SLOW) f = ((sp >> 12) * (int32_t)((358 << 16) / (PEW_SLOW >> 12))) >> 16;
    else f = 358 + ((((sp - PEW_SLOW) >> 12) * (int32_t)((666 << 16) / ((PEW_FAST - PEW_SLOW) >> 12))) >> 16);
    if (f > ONE) f = ONE;
    if (f < 16) return;
    for (int k = 0; k < PEW_ARC_N; k++) {
        const int r = pew_arc[k][0];
        const uint32_t turn = (r == 2 ? pew_drum : pew_horn) >> 19;
        const int32_t sw = (pew_arc[k][2] * f) >> 10;
        const uint32_t a0 = (uint32_t)pew_arc[k][1] + turn + (uint32_t)((pew_arc[k][2] - sw) >> 1);
        enum { N = 8 };                                        /* segments per line (sw * i >> 3) */
        int32_t px, py, x, y;
        ring_at(r, a0, pew_arc[k][3], &px, &py);
        for (int i = 1; i <= N; i++) {
            ring_at(r, a0 + (uint32_t)((sw * i) >> 3), pew_arc[k][3], &x, &y);
            line(canvas, px, py, x, y, Q(1.0), c_red);
            px = x;
            py = y;
        }
    }
}

static void keys(void *canvas)
{
    for (int v = 0; v < ENGINE_VOICES && v < 6; v++)
        if (pew_held[v])
            line(canvas, pew_pulse[v][0], pew_pulse[v][1], pew_pulse[v][2], pew_pulse[v][3], PEW_KEY_W, c_green);
}

void engine_screen_draw(void *screen, void *canvas)
{
    (void)screen;
    if (!canvas) return;
    const int32_t white = K(2);
    bars(canvas);
    keys(canvas);
    lamp(canvas, white);
    smoke(canvas, white);
    rotor(canvas);
}
#endif
