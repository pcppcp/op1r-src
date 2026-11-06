/* adapter as tools/firmware/op1r_plugins.py makes it: hundekraft pew (tools/engines/pew) as an op1r plugin.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"
void pew_note_on(void *, const int16_t *);
void pew_render(void *, int16_t *, const int16_t *);
void pew_note_off(void *, const int16_t *);
#ifdef OP1_HOST
uint32_t engine_host_pitch[128];
#define VOICE(self) ((void *)(uintptr_t)(0xFFB00000u + op1_voice_index(self) * OP1_VOICE_SIZE))
#else
#define VOICE(self) (self)
#endif
static const int16_t no_knobs[8];
static void note_on(void *self, const int16_t *p) { pew_note_on(VOICE(self), p); }
static void render(void *self, int16_t *out, const int16_t *p)
{
#ifdef OP1_HOST
    for (int i = 0; i < OP1_BLOCK; i++) engine_host_pitch[i] = (uint32_t)OP1_PITCH_INC[i];
#endif
    pew_render(VOICE(self), out, p);
}
static void note_off(void *self) { pew_note_off(VOICE(self), no_knobs); }
/* blue, green and white pressed: page 2, their bars pushed out (preset words 5, 6, 7); red has none (word 7 is white's) */
const op1_engine_t ENGINE_pew = { note_on, render, note_off, {OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_NONE} };

#ifndef OP1_HOST
__attribute__((weak)) void pew_screen_load(void *s) { (void)s; }
__attribute__((weak)) void pew_screen_animate(void *s) { (void)s; }
__attribute__((weak)) void pew_screen_draw(void *s, void *c) { (void)s; (void)c; }
/* the screen (screen.c): pew.svg's still scene, everything before note0, is drawn through the core's static-prefix
   cache (docs/screen-art.md); the notes after it and the live drawing on top every frame. No iter bars, so no iter
   on_load / draw */
extern void *pew_first_live;
static void screen_init(void *s) { pew_screen_load(s); }
static void screen_update(void *s, int dt)
{
    pew_screen_animate(s);
    op1_screen_draw_cached(s, dt, pew_first_live);
    pew_screen_draw(s, op1_raster());
}
const op1_screen_t SCREEN_pew = { screen_init, screen_update };
#else   /* host previews (harness/golden.py): the screen code on the harness's mocks (hk/engine.h HK_SCREEN_HOST) */
__attribute__((weak)) void pew_screen_load(void *s) { (void)s; }
__attribute__((weak)) void pew_screen_animate(void *s) { (void)s; }
__attribute__((weak)) void pew_screen_draw(void *s, void *c) { (void)s; (void)c; }
static void screen_init(void *s) { pew_screen_load(s); }
static void screen_update(void *s, int dt) { pew_screen_animate(s); op1_screen_draw_cached(s, dt, 0); pew_screen_draw(s, op1_raster()); }
const op1_screen_t SCREEN_pew = { screen_init, screen_update };
#endif
