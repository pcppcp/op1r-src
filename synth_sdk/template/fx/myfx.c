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
        int32_t x = (buf[n] * drive) >> 15;
        x = x > 32767 ? 32767 : (x < -32767 ? -32767 : x);
        x -= (((((x * x) >> 15) * x) >> 15) * 10923) >> 15;   /* soft clip x - x^3/3 (no division) */
        x = (x * g) >> 15;
        buf[n] = op1_sat16(buf[n] + (((x - buf[n]) * mix) >> 15));
    }
}

/* press: per knob OP1_PRESS_NONE, OP1_PRESS_PAGE (second parameter) or OP1_PRESS_LATCH (on/off flag) */
const op1_fx_t FX_myfx = { my_init, my_render, { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE } };
