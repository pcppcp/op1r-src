/* adapter as tools/firmware/op1r_plugins.py makes it: hundekraft taffy (tools/engines/taffy) as an op1r plugin.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"
void taffy_note_on(void *, const int16_t *);
void taffy_render(void *, int16_t *, const int16_t *);
void taffy_note_off(void *, const int16_t *);
#ifdef OP1_HOST
uint32_t engine_host_pitch[128];
#define VOICE(self) ((void *)(uintptr_t)(0xFFB00000u + op1_voice_index(self) * OP1_VOICE_SIZE))
#else
#define VOICE(self) (self)
#endif
static const int16_t no_knobs[8];
static void note_on(void *self, const int16_t *p) { taffy_note_on(VOICE(self), p); }
static void render(void *self, int16_t *out, const int16_t *p)
{
#ifdef OP1_HOST
    for (int i = 0; i < OP1_BLOCK; i++) engine_host_pitch[i] = (uint32_t)OP1_PITCH_INC[i];
#endif
    taffy_render(VOICE(self), out, p);
}
static void note_off(void *self) { taffy_note_off(VOICE(self), no_knobs); }
const op1_engine_t ENGINE_taffy = { note_on, render, note_off, {0, 0, 0, 0} };

#ifndef OP1_HOST
__attribute__((weak)) void taffy_screen_load(void *s) { (void)s; }
__attribute__((weak)) void taffy_screen_animate(void *s) { (void)s; }
__attribute__((weak)) void taffy_screen_draw(void *s, void *c) { (void)s; (void)c; }
/* the screen (screen.c): taffy.svg is all static (no iter bars, so no iter on_load / draw), drawn through the core's
   static-prefix cache (docs/screen-art.md), then the live drawing on top */
static void screen_init(void *s) { taffy_screen_load(s); }
static void screen_update(void *s, int dt)
{
    taffy_screen_animate(s);
    op1_screen_draw_cached(s, dt, 0);
    taffy_screen_draw(s, op1_raster());
}
const op1_screen_t SCREEN_taffy = { screen_init, screen_update };
#else   /* host previews (harness/golden.py): the screen code on the harness's mocks (hk/engine.h HK_SCREEN_HOST) */
__attribute__((weak)) void taffy_screen_load(void *s) { (void)s; }
__attribute__((weak)) void taffy_screen_animate(void *s) { (void)s; }
__attribute__((weak)) void taffy_screen_draw(void *s, void *c) { (void)s; (void)c; }
static void screen_init(void *s) { taffy_screen_load(s); }
static void screen_update(void *s, int dt) { taffy_screen_animate(s); op1_screen_draw_cached(s, dt, 0); taffy_screen_draw(s, op1_raster()); }
const op1_screen_t SCREEN_taffy = { screen_init, screen_update };
#endif
