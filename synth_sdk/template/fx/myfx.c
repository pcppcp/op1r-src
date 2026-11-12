/* Template effect: tremolo with a soft clipper.
 *   blue  : rate
 *   green : depth
 *   white : drive
 *   red   : mix
 * Copy this folder to plugins/<name>/, rename the file, the symbol and "name" in plugin.json.
 * Copyright (C) <year> <you>. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"

typedef struct { uint32_t lfo; } my_state_t;
static my_state_t st[3];                                  /* one per FX position: 0 synth, 1 drum, 2 master */

static void my_init(int pos) { st[pos % 3].lfo = 0; }

static void my_render(int pos, int16_t *buf, const int16_t *p)
{
    my_state_t *s = &st[pos % 3];
    int32_t rate  = 2000 + op1_fx_param_q15(p, 0) * 40;   /* raw knob words -> 0..32767 first */
    int32_t depth = op1_fx_param_q15(p, 1);
    int32_t drive = 32768 + op1_fx_param_q15(p, 2) * 3;
    int32_t mix   = op1_fx_param_q15(p, 3);
    for (int n = 0; n < OP1_FX_BLOCK; n++) {
        s->lfo += (uint32_t)rate;
        int32_t l = (op1_sin(s->lfo >> 19) + 32768) >> 1;           /* 0..32767 */
        int32_t g = 32767 - ((depth * (32767 - l)) >> 15);
        int32_t x = (buf[n] * (drive >> 1)) >> 14;              /* drive / 2: a full-scale input times 4x fits 32 bits */
        x = x > 32767 ? 32767 : (x < -32767 ? -32767 : x);
        x -= (((((x * x) >> 15) * x) >> 15) * 10923) >> 15;   /* soft clip x - x^3/3 (no division) */
        x = (x * g) >> 15;
        buf[n] = op1_sat16(buf[n] + (((x - buf[n]) * mix) >> 15));
    }
}

/* press: per knob OP1_PRESS_NONE, OP1_PRESS_PAGE (second parameter) or OP1_PRESS_LATCH (on/off flag) */
const op1_fx_t FX_myfx = { my_init, my_render, { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE } };

/* ---- own screen (optional: without "screen" in plugin.json the FX gets a generic page with its 4 knobs) ----
 * GUI thread, every frame. myfx.svg holds the static parts and the 4 dials (ids bar0..bar3); the tremolo curve and
 * the moving LFO dot are drawn here with raster lines. Coordinates: 320 x 160 screen units, Q10 (OP1_Q10). */
static void *my_bars[4];

static void my_screen_init(void *scr)
{
    static const char *ids[4] = { "bar0", "bar1", "bar2", "bar3" };
    for (int i = 0; i < 4; i++) my_bars[i] = op1_find_node(scr, ids[i]);
}

static void my_screen_update(void *scr, int dt)
{
    static const uint8_t green[4] = { 0x00, 0xED, 0x95, 0xFF }, white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    static const int16_t dial_x[4] = { 70, 130, 190, 250 };
    int16_t k[4];
    op1_fx_knobs_live(k);                                              /* the knobs of the FX on screen, 0..32767 */
    for (int i = 0; i < 4; i++) if (my_bars[i]) {                      /* dials: -135 .. +135 degrees */
        int32_t a = ((int32_t)k[i] * 6144 >> 15) - 3072;
        op1_node_rotate(my_bars[i], OP1_Q10(dial_x[i]), OP1_Q10(136), op1_sin(a), op1_sin(a + 2048));
    }
    op1_screen_draw(scr, dt);                                          /* the SVG, then our lines on top */
    void *r = op1_raster();
    int32_t depth = k[1] * 30 / 32767;                                 /* curve height in px: depth */
    op1_pt p = { OP1_Q10(40), OP1_Q10(55) };
    for (int x = 44; x <= 280; x += 4) {                               /* two LFO cycles over the frame */
        op1_pt q = { OP1_Q10(x), OP1_Q10(55) - depth * op1_sin((x - 40) * 8192 / 120) / 32 };
        op1_raster_line(r, &p, &q, 0x600, green);
        p = q;
    }
    uint32_t ph = st[op1_fx_screen_pos() % 3].lfo >> 19;              /* where the LFO is now (0..8191) */
    int32_t x = OP1_Q10(40) + (int32_t)ph * 120 / 8192 * 1024, y = OP1_Q10(55) - depth * op1_sin((int32_t)ph) / 32;
    op1_pt a = { x - OP1_Q10(2), y }, b = { x + OP1_Q10(2), y }, c = { x, y - OP1_Q10(2) }, d = { x, y + OP1_Q10(2) };
    op1_raster_line(r, &a, &b, 0x600, white);                          /* a dot: two short crossing lines */
    op1_raster_line(r, &c, &d, 0x600, white);
}

const op1_screen_t SCREEN_myfx = { my_screen_init, my_screen_update };
