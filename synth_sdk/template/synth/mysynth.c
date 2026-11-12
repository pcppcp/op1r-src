/* Template synth engine: two detuned saws through a one-pole low-pass.
 *   blue  : detune of the second saw
 *   green : low-pass cutoff
 *   white : drive (soft clip)
 *   red   : level
 * Copy this folder to plugins/<name>/, rename the file, the symbol and "name" in plugin.json.
 * Copyright (C) <year> <you>. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"

typedef struct {              /* one per voice, max 0x24 bytes */
    uint32_t vtable;          /* firmware-owned: never write */
    uint32_t ph1, ph2;        /* saw phases (2^32 = one cycle) */
    int32_t  lp;              /* low-pass state */
} my_voice_t;

static void my_note_on(void *self, const int16_t *params)
{
    my_voice_t *v = (my_voice_t *)self; (void)params;
    v->ph1 = 0; v->ph2 = 0x80000000u; v->lp = 0;
}

static void my_render(void *self, int16_t *out, const int16_t *params)
{
    my_voice_t *v = (my_voice_t *)self;
    const int32_t *inc = OP1_PITCH_INC;                    /* per-sample phase increment of this note */
    uint32_t detune = 32768 + (params[0] >> 4);            /* 1.0 .. 1.06, Q15 */
    int32_t  cut    = 512 + (params[1] >> 1);              /* one-pole coefficient, Q15 */
    int32_t  drive  = 32768 + params[2] * 3;               /* 1 .. 4x, Q15 */
    int32_t  gain   = (params[3] * op1_voice_vel_gain(self)) >> 15;
    uint32_t ph1 = v->ph1, ph2 = v->ph2; int32_t lp = v->lp;

    for (int n = 0; n < OP1_BLOCK; n++) {
        uint32_t i = (uint32_t)inc[n];
        ph1 += i;
        ph2 += i + (i >> 15) * (detune - 32768);             /* i * detune, 32-bit only (no 64-bit math) */
        int32_t s = (((int32_t)ph1 >> 17) + ((int32_t)ph2 >> 17)) >> 1;   /* two saws, Q15 */
        lp += ((s - lp) * cut) >> 15;
        int32_t x = (lp * drive) >> 15;
        x = x > 32767 ? 32767 : (x < -32767 ? -32767 : x);
        x -= (((((x * x) >> 15) * x) >> 15) * 10923) >> 15;   /* soft clip x - x^3/3 (no division) */
        out[n] = op1_sat16((x * gain) >> 15);
    }
    v->ph1 = ph1; v->ph2 = ph2; v->lp = lp;
}

/* note_off may be 0; press: per knob OP1_PRESS_NONE or OP1_PRESS_PAGE (second parameter page, see op1_plugin.h) */
const op1_engine_t ENGINE_mysynth = { my_note_on, my_render, 0, { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE } };

/* ---- own screen (optional: without "screen" in plugin.json the engine uses the stock iter graphics) ----
 * GUI thread, every frame. mysynth.svg holds the static parts and the 4 dials (ids bar0..bar3); the waves are drawn
 * here with raster lines. Coordinates: 320 x 160 screen units, Q10 (OP1_Q10). */
static void *my_bars[4];

static void my_screen_init(void *scr)
{
    static const char *ids[4] = { "bar0", "bar1", "bar2", "bar3" };
    for (int i = 0; i < 4; i++) my_bars[i] = op1_find_node(scr, ids[i]);
}

static void my_screen_update(void *scr, int dt)
{
    static const uint8_t blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    static const int16_t dial_x[4] = { 70, 130, 190, 250 };
    int16_t k[4];
    op1_synth_knobs_live(OP1_SCREEN_SLOT & 7, k);
    for (int i = 0; i < 4; i++) if (my_bars[i]) {                      /* dials: -135 .. +135 degrees */
        int32_t a = ((int32_t)k[i] * 6144 >> 15) - 3072;
        op1_node_rotate(my_bars[i], OP1_Q10(dial_x[i]), OP1_Q10(136), op1_sin(a), op1_sin(a + 2048));
    }
    op1_screen_draw(scr, dt);                                          /* the SVG, then our lines on top */
    void *r = op1_raster();
    int32_t h = OP1_Q10(8 + k[3] * 30 / 32767);                        /* wave height: level (divisions per frame: fine) */
    for (int w = 0; w < 2; w++) {                                      /* the two saws, the second one detuned */
        int32_t period = OP1_Q10(48 - (w ? k[0] * 12 / 32767 : 0)), x = OP1_Q10(40);
        while (x < OP1_Q10(280)) {
            int32_t x1 = x + period > OP1_Q10(280) ? OP1_Q10(280) : x + period;
            int32_t y1 = OP1_Q10(70) - h * (x1 - x) / period;
            op1_pt a = { x, OP1_Q10(70) }, b = { x1, y1 }, c = { x1, OP1_Q10(70) };
            op1_raster_line(r, &a, &b, 0x600, w ? blue : white);
            if (x1 < OP1_Q10(280)) op1_raster_line(r, &b, &c, 0x600, w ? blue : white);
            x = x1;
        }
    }
}

const op1_screen_t SCREEN_mysynth = { my_screen_init, my_screen_update };
