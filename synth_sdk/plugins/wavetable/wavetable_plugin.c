/* adapter as tools/firmware/op1r_plugins.py makes it: hundekraft wavetable (tools/engines/wavetable) as an op1r plugin.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"
void wavetable_note_on(void *, const int16_t *);
void wavetable_render(void *, int16_t *, const int16_t *);
void wavetable_note_off(void *, const int16_t *);
#ifdef OP1_HOST
uint32_t engine_host_pitch[128];
#define VOICE(self) ((void *)(uintptr_t)(0xFFB00000u + op1_voice_index(self) * OP1_VOICE_SIZE))
#else
#define VOICE(self) (self)
#endif
static const int16_t no_knobs[8];
static void note_on(void *self, const int16_t *p) { wavetable_note_on(VOICE(self), p); }
static void render(void *self, int16_t *out, const int16_t *p)
{
#ifdef OP1_HOST
    for (int i = 0; i < OP1_BLOCK; i++) engine_host_pitch[i] = (uint32_t)OP1_PITCH_INC[i];
#endif
    wavetable_render(VOICE(self), out, p);
}
static void note_off(void *self) { wavetable_note_off(VOICE(self), no_knobs); }
const op1_engine_t ENGINE_wavetable = { note_on, render, note_off, {0, 0, 0, 0} };

/* the screen: wavetable_screen.c (SCREEN_wavetable, the oscilloscope stack; kennel's own, no longer iter's screen) */
