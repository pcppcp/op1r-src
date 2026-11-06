/* adapter as tools/firmware/op1r_plugins.py makes it: hundekraft snowglobe (tools/fx/snowglobe) as an op1r plugin.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"
void snowglobe_fx_reset(int);
void snowglobe_fx_process(int, int16_t *, int, const int16_t *);
static const int16_t lo[4] = {64, 0, 0, 8192}, hi[4] = {14528, 24576, 22528, 32767};
/* (v - lo) x 32767 / (hi - lo) without a division (__divsi3 was ~190 packets per knob per block): the reciprocal
   floor(32767 x 65536 / (hi - lo)) gives the quotient or one less (the error is under (hi - lo) / 65536 < 0.4),
   the remainder decides; exact for every v (harness/snowglobe_check.py checks all of them) */
static const uint32_t rcp[4] = {148466, 87378, 95322, 87382};
static void init(int pos) { snowglobe_fx_reset(pos); }
static void render(int pos, int16_t *buf, const int16_t *params)
{
    int16_t k[8];
    for (int i = 0; i < 4; i++) {
        int32_t v = params[i];
        if (v < lo[i]) v = lo[i];
        if (v > hi[i]) v = hi[i];
        const uint32_t x = (uint32_t)(v - lo[i]), d = (uint32_t)(hi[i] - lo[i]);
        uint32_t q = (x * rcp[i]) >> 16;
        if (x * 32767u - q * d >= d) q++;
        k[i] = (int16_t)q;
    }
    for (int i = 4; i < 8; i++) k[i] = params[i];
    snowglobe_fx_process(pos, buf, OP1_FX_BLOCK, k);
}
const op1_fx_t FX_snowglobe = { init, render, {0, 0, 0, 0} };

#ifndef OP1_HOST
__attribute__((weak)) void snowglobe_fx_screen_load(void *s) { (void)s; }
__attribute__((weak)) void snowglobe_fx_screen_animate(void *s) { (void)s; }
__attribute__((weak)) void snowglobe_fx_screen_draw(void *s, void *c) { (void)s; (void)c; }
#define ITER_SCREEN_VT ((const uint32_t *)0x019103FCu)        /* +0x1C draw, +0x24 on_load */
static void screen_init(void *s)
{
    ((void (*)(void *))ITER_SCREEN_VT[0x24 / 4])(s);
    snowglobe_fx_screen_load(s);
}
static void screen_update(void *s, int dt)
{
    snowglobe_fx_screen_animate(s);
    ((int (*)(void *, int))ITER_SCREEN_VT[0x1C / 4])(s, dt);
    snowglobe_fx_screen_draw(s, ((void *(*)(void))0x01A3B490)());
}
const op1_screen_t SCREEN_snowglobe = { screen_init, screen_update };
#else   /* host previews (harness/golden.py): the screen code on the harness's mocks (hk/engine.h HK_SCREEN_HOST) */
__attribute__((weak)) void snowglobe_fx_screen_load(void *s) { (void)s; }
__attribute__((weak)) void snowglobe_fx_screen_animate(void *s) { (void)s; }
__attribute__((weak)) void snowglobe_fx_screen_draw(void *s, void *c) { (void)s; (void)c; }
static void screen_init(void *s) { snowglobe_fx_screen_load(s); }
static void screen_update(void *s, int dt) { snowglobe_fx_screen_animate(s); op1_screen_draw(s, dt); snowglobe_fx_screen_draw(s, op1_raster()); }
const op1_screen_t SCREEN_snowglobe = { screen_init, screen_update };
#endif
