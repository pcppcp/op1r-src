/* adapter as tools/firmware/op1r_plugins.py makes it: hundekraft elg (tools/engines/elg) as an op1r plugin.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"
void elg_note_on(void *, const int16_t *);
void elg_render(void *, int16_t *, const int16_t *);
void elg_note_off(void *, const int16_t *);
#ifdef OP1_HOST
uint32_t engine_host_pitch[128];
#define VOICE(self) ((void *)(uintptr_t)(0xFFB00000u + op1_voice_index(self) * OP1_VOICE_SIZE))
#else
#define VOICE(self) (self)
#endif
static const int16_t no_knobs[8];
static void note_on(void *self, const int16_t *p) { elg_note_on(VOICE(self), p); }
static void render(void *self, int16_t *out, const int16_t *p)
{
#ifdef OP1_HOST
    for (int i = 0; i < OP1_BLOCK; i++) engine_host_pitch[i] = (uint32_t)OP1_PITCH_INC[i];
#endif
    elg_render(VOICE(self), out, p);
}
static void note_off(void *self) { elg_note_off(VOICE(self), no_knobs); }
const op1_engine_t ENGINE_elg = { note_on, render, note_off, {0, 0, 0, 0} };

#ifndef OP1_HOST
__attribute__((weak)) void elg_screen_load(void *s) { (void)s; }
__attribute__((weak)) void elg_screen_animate(void *s) { (void)s; }
__attribute__((weak)) void elg_screen_draw(void *s, void *c) { (void)s; (void)c; }
#define ITER_SCREEN_VT ((const uint32_t *)0x019103FCu)        /* +0x1C draw, +0x24 on_load */
static void screen_init(void *s)
{
    ((void (*)(void *))ITER_SCREEN_VT[0x24 / 4])(s);
    elg_screen_load(s);
}
static void screen_update(void *s, int dt)
{
    elg_screen_animate(s);
    ((int (*)(void *, int))ITER_SCREEN_VT[0x1C / 4])(s, dt);
    elg_screen_draw(s, ((void *(*)(void))0x01A3B490)());
}
const op1_screen_t SCREEN_elg = { screen_init, screen_update };
#else   /* host previews (harness/golden.py): the screen code on the harness's mocks (hk/engine.h HK_SCREEN_HOST) */
__attribute__((weak)) void elg_screen_load(void *s) { (void)s; }
__attribute__((weak)) void elg_screen_animate(void *s) { (void)s; }
__attribute__((weak)) void elg_screen_draw(void *s, void *c) { (void)s; (void)c; }
static void screen_init(void *s) { elg_screen_load(s); }
static void screen_update(void *s, int dt) { elg_screen_animate(s); op1_screen_draw(s, dt); elg_screen_draw(s, op1_raster()); }
const op1_screen_t SCREEN_elg = { screen_init, screen_update };
#endif
