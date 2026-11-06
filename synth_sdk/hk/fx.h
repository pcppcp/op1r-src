/* fx.h: tools/fx/fx.h at 387a172, copied for the hundekraft plugins;
 * the only additions are the HK_SCREEN_HOST branches (host previews, see below).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* Effect interface for our FX next to op1r's (see docs/synth-engines.md; the firmware hook is not wired yet).
 *
 *   fx_reset(inst)                      clear the state of one instance (the FX was just picked)
 *   fx_process(inst, io, n, knobs)      process n mono int16 samples in place (n = 128 in the firmware)
 * inst    = which copy of the state: FX_SYNTH, FX_DRUM or FX_MASTER (the same FX can run on an instrument and
 *           on the master at the same time, as op1r's do).
 * knobs   = the FX's 8 int16 params; 0..3 are the encoders (blue, green, white, red), 0..32767.
 *
 * Rules as for engines: no floating point, no libc. Big delay lines go in FX_BUFFER (not stored in the
 * firmware image; zero them in fx_reset).
 */
#ifndef FX_H
#define FX_H

#include <stdint.h>

enum { FX_SYNTH, FX_DRUM, FX_MASTER, FX_INSTANCES };
#define FX_BLOCK 128

#ifdef FX_HOST_TEST
#define FX_BUFFER
#else
#define FX_BUFFER __attribute__((section(".fxbuf")))
#endif

static inline int16_t fx_sat16(int32_t x)
{
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

/* for an effect's screen code: its params as the knobs show them, scaled to 0..32767 like the audio gets
   them (the screen being edited: master, drum or synth slot) */
#ifndef FX_HOST_TEST
#ifdef HK_SCREEN_HOST
#include "op1_plugin.h"
#endif
static inline int32_t fx_screen_knob(int i)
{
#ifdef HK_SCREEN_HOST
    if (1) return op1_fx_knobs(0)[i] < 0 ? 0 : op1_fx_knobs(0)[i];   /* host: words are 0..32767 */
#endif
    static const int16_t lo[4] = {64, 0, 0, 8192}, hi[4] = {14528, 24576, 22528, 32767};
    const uint32_t mode = *(const volatile uint32_t *)0x01DF4E3C;
    const volatile int16_t *p = mode == 3 ? (const volatile int16_t *)0xFF8016C4u
        : mode == 1 ? (const volatile int16_t *)(0xFF8015C4u + (*(const volatile uint32_t *)0xFF8016D8 & 7) * 16)
        : (const volatile int16_t *)(0xFF801444u + (*(const volatile uint32_t *)0xFF8016D4 & 7) * 16);
    int32_t v = p[i];
    if (v < lo[i]) v = lo[i];
    if (v > hi[i]) v = hi[i];
    return ((v - lo[i]) * 32767) / (hi[i] - lo[i]);
}

/* the instance the screen being edited belongs to (same mode word as fx_screen_knob: 3 master, 1 drum, else synth).
   The synth/drum order of the mono instances is not confirmed, so fx_screen_snapshot only trusts an instance whose
   block counter moved since the last frame */
static inline int fx_screen_inst(void)
{
#ifdef HK_SCREEN_HOST
    if (1) return FX_SYNTH;
#endif
    const uint32_t mode = *(const volatile uint32_t *)0x01DF4E3C;
    return mode == 3 ? FX_MASTER : mode == 1 ? FX_DRUM : FX_SYNTH;
}

/* the four params for the screen, once a frame (in engine_screen_animate): as the effect heard them in its last
   block (after the LFO; the effect copies them into live[inst] and counts blocks[inst]) when the screen's instance
   ran since the last frame, else as set; clamped to 0..32767 */
static inline void fx_screen_snapshot(const volatile int16_t (*live)[4], const volatile uint32_t *blocks,
                                      uint32_t *seen, int32_t *k)
{
    const int inst = fx_screen_inst();
    const uint32_t b = blocks[inst];
    const int ran = b != seen[inst];
    seen[inst] = b;
    for (int i = 0; i < 4; i++) {
        const int32_t v = ran ? live[inst][i] : fx_screen_knob(i);
        k[i] = v < 0 ? 0 : v;
    }
}
#endif

void fx_reset(int inst);
void fx_process(int inst, int16_t *io, int n, const int16_t *knobs);

#endif
