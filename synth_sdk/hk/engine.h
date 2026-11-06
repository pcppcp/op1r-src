/* engine.h: tools/engines/engine.h at 387a172, copied for the hundekraft plugins;
 * the only additions are the HK_SCREEN_HOST branches (host previews, see below).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* Synth engine interface for OP-1 firmware R246 (see docs/synth-engines.md).
 *
 * An engine replaces the hidden "iter" engine. The firmware calls, per voice and per 128-sample block:
 *   engine_note_on(self, knobs)        when a note starts on that voice
 *   engine_render(self, out, knobs)    every block while the voice is active (also during the release)
 *   engine_note_off(self, knobs)       when the key is released
 * self    = iter's 0x24-byte voice object in L1 scratchpad; only its address is useful (ENGINE_VOICE).
 * out     = 128 mono samples, int16 Q15. The firmware applies the ADSR envelope, voice gain and mixing.
 * knobs   = 8 int16 (0..32767 for iter's ranges), after LFO modulation. Knobs 0-3 are the four encoders
 *           (blue, green, white, orange).
 * Pitch is not an argument: ENGINE_PITCH[i] is the phase increment for sample i, 2^32 * f / 44100.
 *
 * Rules: no floating point, no libc. Keep per-voice state in your own arrays (the image is loaded at
 * 0x01F00000; zero-initialised globals are fine). Calls run in the audio path at 44.1 kHz / 128 = 344 Hz per
 * voice, 6 voices: keep render well under ~20 cycles per sample.
 */
#ifndef ENGINE_H
#define ENGINE_H

#include <stdint.h>

#define ENGINE_VOICES 6
#define ENGINE_BLOCK 128
#ifdef ENGINE_HOST_TEST   /* tools/engines/hosttest.c: native build with a fake pitch buffer */
extern uint32_t engine_host_pitch[128];
#define ENGINE_PITCH engine_host_pitch
#else
#define ENGINE_PITCH ((const volatile uint32_t *)0xFF801950)
#endif

/* Voice index 0..5 from the object address: objects are 0x24 bytes apart from 0xFFB00000. */
static inline int ENGINE_VOICE(const void *self)
{
    return (int)((((uint32_t)(uintptr_t)self - 0xFFB00000u) * 1821u) >> 16);   /* exact /36 for 0..5 */
}

/* Note velocity of a voice, 1..127 (op1r's engines read it here, W[0xFF801064 + 2*voice] >> 8; 0 = not set). */
#ifdef ENGINE_HOST_TEST
static inline int ENGINE_VELOCITY(int voice) { (void)voice; return 127; }
#else
static inline int ENGINE_VELOCITY(int voice) { return ((const volatile int16_t *)0xFF801064)[voice] >> 8; }
#endif

static inline int16_t engine_sat16(int32_t x)
{
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

void engine_note_on(void *self, const int16_t *knobs);
void engine_render(void *self, int16_t *out, const int16_t *knobs);
void engine_note_off(void *self, const int16_t *knobs);

/* ---- own screen (op1r route only, see op1r_host.c) ----
 * Optional; the table build.sh generates has empty weak versions. They run in the UI, not the audio path.
 *   engine_screen_load(screen)          after the SVG is loaded: look up elements with SVG_FIND
 *   engine_screen_animate(screen)       every frame before the SVG is drawn: set element matrices
 *   engine_screen_draw(screen, canvas)  every frame after it: draw lines on top with SVG_LINE
 * Coordinates are SVG units in Q10 (pixel * 1024). R246 calls, found in op1r's fmbell screen. */
#ifndef ENGINE_HOST_TEST
typedef struct { int32_t x, y; } svg_pt;
/* element by id attribute, 0 if missing (the document's recursive search with required = 0; the screen's own
   lookup 0x01A3B580 passes 1, which stops the firmware with an error on a missing id) */
#ifdef HK_SCREEN_HOST   /* kennel host previews: the harness's mocks (op1_host.h) */
#include "op1_plugin.h"
#define SVG_FIND(screen, id) op1_find_node((screen), (id))
#define SVG_MATRIX(elem, m) op1_node_set_transform((elem), (m))
#define SVG_LINE(canvas, a, b, width, rgba) op1_raster_line((canvas), (a), (b), (width), (rgba))
#define SVG_SIN(i) (OP1_SINE_TABLE[(i) & 8191])
#define SCREEN_SLOT() (OP1_SCREEN_SLOT & 7)
#define SCREEN_KNOB(i) (op1_synth_knobs(SCREEN_SLOT())[i])
#else
#define SVG_FIND(screen, id) \
    (((void *(*)(void *, const char *, int))0x019EB8AC)(*(void **)(screen), (id), 0))
/* element matrix, Q10, m = {a, b, 0, c, d, 0, tx, ty, 1024}: x' = a x + c y + tx, y' = b x + d y + ty */
#define SVG_MATRIX(elem, m) (((void (*)(void *, const int32_t *))0x019EAD58)((elem), (m)))
/* stroke from a to b, width Q10, colour = 4 bytes R, G, B, A */
#define SVG_LINE(canvas, a, b, width, rgba) \
    (((void (*)(void *, const svg_pt *, const svg_pt *, int32_t, const uint8_t *))0x01A24A4C)( \
        (canvas), (a), (b), (width), (rgba)))
/* sine, 8192 steps per turn, +-32767 (cosine: index + 2048) */
#define SVG_SIN(i) (((const int16_t *)0x0101E5E8)[(i) & 8191])
/* the synth slot on screen (0..7) and its knobs, 0..32767 */
#define SCREEN_SLOT() (*(const volatile uint32_t *)0x01DF4E38 & 7)
#define SCREEN_KNOB(i) (((const volatile int16_t *)(0xFF8013C4u + SCREEN_SLOT() * 16))[i])
#endif
/* matrix helpers, Q10 pixels; angle 8192 per turn */
static inline void svg_move(void *e, int32_t tx, int32_t ty)
{
    if (!e) return;
    const int32_t m[9] = {1024, 0, 0, 0, 1024, 0, tx, ty, 1024};
    SVG_MATRIX(e, m);
}
static inline void svg_turn(void *e, uint32_t angle, int32_t px, int32_t py)
{
    if (!e) return;
    const int32_t s = SVG_SIN(angle) >> 5, c = SVG_SIN(angle + 2048) >> 5;
    const int32_t m[9] = {c, s, 0, -s, c, 0, px - ((c * px - s * py) >> 10), py - ((s * px + c * py) >> 10), 1024};
    SVG_MATRIX(e, m);
}
/* scale by sx, sy (Q10, may be negative) around (px, py) */
static inline void svg_scale(void *e, int32_t sx, int32_t sy, int32_t px, int32_t py)
{
    if (!e) return;
    const int32_t m[9] = {sx, 0, 0, 0, sy, 0, px - ((sx * px) >> 10), py - ((sy * py) >> 10), 1024};
    SVG_MATRIX(e, m);
}
/* a knob for the screen: as the voices hear it (after the slot LFO; the engine copies it per block into live)
   while notes play, else as set; clamped to 0..32767 */
static inline int32_t screen_knob_live(const volatile int16_t *live, int i, int playing)
{
    const int32_t v = playing ? live[i] : SCREEN_KNOB(i);
    return v < 0 ? 0 : v;
}
void engine_screen_load(void *screen);
void engine_screen_animate(void *screen);
void engine_screen_draw(void *screen, void *canvas);
#endif

#endif
