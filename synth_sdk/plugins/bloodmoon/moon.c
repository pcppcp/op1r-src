/* BLOODMOON screen: a blood moon behind a cathedral (bloodmoon.svg). One OP-1 knob colour per layer:
 *   red   (MOONRISE) : the red moon rises from behind the cathedral as the knob goes up
 *   green (ECLIPSE)  : the two green tower bells swing; the knob sets how far (still at 0, about +-20 degrees at full)
 *   blue  (BLOOD)    : the blue rose window turns (half a turn over the knob's travel)
 *   white (HAUNTING) : the white stars flicker; none at 0, more and longer the higher the knob
 * kennel (2026-10-29, the owner's pick of the redesign mockups): the knob names and values 0..100 beside the cathedral
 * are drawn with the OP-1's own font (native text objects) instead of the blackletter panels; the scene is placed for
 * the OLED by gen_svg.py, which writes the pivots to bloodmoon_geom.h.
 * GUI thread, integers only. Every node lookup is checked, so a missing id only disables that effect.
 * Copyright (C) 2026 Nola Drift. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 * Screen structure follows the SDK examples (fmbell, brassophone) by pcppcp and nvmsocool. */
#include "op1_synth.h"
#include "bloodmoon_geom.h"

static void *moon_node, *rose_node, *bell_l, *bell_r, *star_node[3];
static uint32_t swing_q, twinkle_q;        /* phases in 1/256 table steps (8192 * 256 = one period) */

/* kennel: names and values (native text, left-aligned): blue / green on the left, white / red on the right */
static const char *const knob_name[4] = { "blood", "eclipse", "haunting", "moonrise" };
static const uint8_t knob_rgba[4][4] = { { 105, 142, 255, 255 }, { 0, 237, 149, 255 }, { 255, 255, 255, 255 },
                                         { 255, 58, 93, 255 } };
static const int16_t text_x[4] = { 16, 16, 220, 220 }, text_y[4] = { 22, 84, 22, 84 };
#define NAME_SIZE  12
#define VALUE_SIZE 24
#define VALUE_DY   19                      /* the value's place under its name */
static uint8_t text_obj[8][OP1_TEXT_SIZE] __attribute__((aligned(4)));   /* 0..3 names, 4..7 values */
static uint8_t text_made[8];
static int shown[4];                       /* value on screen per knob, -1 = none yet */

static inline int32_t tsin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }
static inline int32_t tcos(int32_t a) { return tsin(a + 2048); }

static void moon_init(void *scr)
{
    moon_node = op1_find_node(scr, "moon");
    rose_node = op1_find_node(scr, "rose");
    bell_l = op1_find_node(scr, "bellL");
    bell_r = op1_find_node(scr, "bellR");
    star_node[0] = op1_find_node(scr, "stars0");
    star_node[1] = op1_find_node(scr, "stars1");
    star_node[2] = op1_find_node(scr, "stars2");
    swing_q = 0; twinkle_q = 0;
    for (int k = 0; k < 4; k++) shown[k] = -1;
}

/* text object i (made on first use, in the GUI thread after boot), its string s and its draw */
static void text(void *r, int i, int x, int y, int size, const uint8_t *rgba, const char *s, int changed)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    if (!text_made[i]) { op1_text_ctor(text_obj[i], OP1_Q10(x), y, size, rgba); text_made[i] = 1; changed = 1; }
    if (changed) op1_text_set(text_obj[i], s);
    op1_text_draw(text_obj[i], r, ident);
}

/* the knob's name and its value as 0..100 */
static void show_value(void *r, int k, int32_t knob)
{
    int v = (int)((knob * 100 + 16383) >> 15);
    int q = (v * 205) >> 11;                                     /* v / 10, exact for 0..1028: no libgcc division */
    char s[4], *p = s;
    if (v >= 100) *p++ = '1';
    if (v >= 10) *p++ = (char)('0' + (q >= 10 ? q - 10 : q));
    *p++ = (char)('0' + v - q * 10);
    *p = 0;
    text(r, k, text_x[k], text_y[k], NAME_SIZE, knob_rgba[k], knob_name[k], 0);
    text(r, 4 + k, text_x[k], text_y[k] + VALUE_DY, VALUE_SIZE, knob_rgba[k], s, v != shown[k]);
    shown[k] = v;
}

static void moon_update(void *scr, int dt)
{
    int16_t k[4];
    op1_synth_knobs_live(OP1_SCREEN_SLOT & 7, k);
    for (int i = 0; i < 4; i++) k[i] = k[i] < 0 ? 0 : k[i];          /* knobs are 0..32767 */
    if (dt < 1) dt = 1;
    if (dt > 100) dt = 100;                                          /* a stalled frame must not jump the animation */

    /* red / MOONRISE: moon height. The cathedral is filled black and drawn after the moon, so it hides the moon's lower part */
    if (moon_node) {
        int32_t dy_q10 = (int32_t)(((32767 - k[3]) * (MOON_DROP_Q10 >> 5)) >> 10);   /* Q10 pixels, 32-bit only */
        int32_t m[9] = { 1024, 0, 0, 0, 1024, 0, 0, dy_q10, 1024 };
        op1_node_set_transform(moon_node, m);
    }

    /* blue / BLOOD: rose window angle, 0..half a turn */
    if (rose_node) {
        int32_t a = (k[0] * 4096) >> 15;
        op1_node_rotate(rose_node, OP1_Q10(ROSE_X), OP1_Q10(ROSE_Y), tsin(a), tcos(a));
    }

    /* green / ECLIPSE: bells swing in opposite directions, about 1 swing/s (dt assumed to be ms, as in horn.c) */
    swing_q += (uint32_t)((16800 * dt) >> 3);
    {
        int32_t amp = (k[1] * 455) >> 15;                            /* 0..455 of 8192 = 0..20 degrees */
        int32_t a = (tsin((int32_t)(swing_q >> 8)) * amp) >> 15;
        if (bell_l) op1_node_rotate(bell_l, OP1_Q10(BELL_L_X), OP1_Q10(BELL_Y), tsin(a), tcos(a));
        if (bell_r) op1_node_rotate(bell_r, OP1_Q10(BELL_R_X), OP1_Q10(BELL_Y), tsin(-a), tcos(-a));
    }

    /* white / HAUNTING: three groups of stars flicker at different speeds; visible while sin > threshold,
     * threshold -32768 (always visible) at knob 0 up to about +7200 (visible ~40% of the time) at full */
    twinkle_q += (uint32_t)((17500 * dt) >> 3);
    {
        int32_t thr = -32768 + ((k[2] * 40000) >> 15);
        for (int i = 0; i < 3; i++) if (star_node[i]) {
            int32_t a = (int32_t)(twinkle_q >> 8) * (2 + i) + i * 2731;
            op1_node_set_visible(star_node[i], tsin(a) > thr);
        }
    }

    op1_screen_draw(scr, dt);                                        /* the SVG, then the text over it */
    void *r = op1_raster();
    for (int i = 0; i < 4; i++) show_value(r, i, k[i]);
}

const op1_screen_t SCREEN_bloodmoon = { moon_init, moon_update };
