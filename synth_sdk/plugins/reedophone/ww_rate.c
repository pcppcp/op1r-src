/* windwood on R246's voice rate (kennel addition; upstream's reedophone.c is included unchanged).
 *
 * The model is a 44.1 kHz design, 128 samples a call at 344 calls/s: its reed, filters, noise, growl and every time
 * in it (blocks, ms, samples) are derived from that. R246 calls a voice 689 times a second for 128 samples at
 * 88.2 kHz, and OP1_PITCH_INC counts 88.2 kHz steps. Run as it stands, the model reads every note an octave low
 * (tuning table, loss, cutoffs, hum pitch), plays in tune only because the loop length comes from the increment, and
 * runs its times at twice the speed. So the engine runs as designed, once per two calls: a fresh 128-sample block at
 * 44.1 kHz (increment doubled) every other call, 64 of its samples per call, upsampled 2x by the midpoint filter
 * (-1, 9, 9, -1) / 16 (as juni and strum). Output is delayed by two 44.1 kHz samples. The engine's own block, in
 * which all its constants were fitted, is exactly two R246 calls; half the calls compute, so the CPU per voice halves
 * on average; the stagger below spreads a chord's blocks over both calls.
 * A fresh (tongued) start restarts the phase and the history; a slur keeps both, so the click-free legato is intact.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"

static inline uint32_t ww_real_inc(void) { return (uint32_t)OP1_PITCH_INC[0]; }   /* 88.2 kHz steps, before the redefinition */
static int32_t ww_inc[1];                                                        /* what the engine reads: 44.1 kHz steps */
#undef OP1_PITCH_INC
#define OP1_PITCH_INC ((const int32_t *)ww_inc)
#define ENGINE_reedophone ENGINE_reedophone_44k
#define setup rv_setup_up                        /* upstream's, kept as the reference of the kennel copies (ww_fast.h) */
#define set_delays rv_set_delays_up
#define warp rv_warp_up
#define rv_render rv_render_up
#include "reedophone.c"
#undef ENGINE_reedophone
#undef setup
#undef set_delays
#undef warp
#undef rv_render
#include "ww_fast.h"                             /* setup, set_delays, warp, rv_render without 64-bit divides */

/* Stagger: a chord struck at once would compute every voice's block in the same call. Each voice computes on the
 * calls whose block parity (ww_blk) equals its index's parity, so even and odd voices take turns and a call carries
 * about half the blocks. The parity counts R246's blocks: voices are rendered in index order within a block (as dddd
 * relies on), so a call for a voice at or below the last one rendered starts a new block. A fresh start lines its
 * voice up with its phase at its first call, waiting one call (1.45 ms of silence) when that call is the other
 * phase; from then on the voice alternates on its own, through a slur and a steal (R246 renders a sounding voice
 * every block). A voice that missed blocks (its release ended, so R246 stopped rendering it, and a note_on slurred
 * on its still-blowing engine) lines up again the same way: its unplayed half block is dropped (the voice was
 * silent: its envelope had ended). */
enum { WW_COMPUTE, WW_REST, WW_WAIT, WW_ALIGN };
typedef struct {
    int16_t x[4 + OP1_BLOCK];        /* the last four 44.1 kHz samples of the previous block, then this block */
    uint8_t half;                    /* WW_COMPUTE: compute a block and play its first 64 samples; WW_REST: play the
                                      * rest; WW_WAIT: silence this call, compute the next; WW_ALIGN: a fresh start,
                                      * pick COMPUTE or WAIT at the first call */
} ww_t;
static ww_t ww[OP1_VOICES];
static uint32_t ww_blk, ww_seen[OP1_VOICES];   /* blocks counted; the block each voice was last rendered in */
static uint8_t ww_last = 0xff;                  /* the voice rendered last */

static void ww_clear(ww_t *w) { for (int i = 0; i < 4 + OP1_BLOCK; i++) w->x[i] = 0; w->half = WW_ALIGN; }

static void ww_note_on(void *self, const int16_t *params)
{
    int vi = op1_voice_index(self); if (vi < 0 || vi >= OP1_VOICES) vi = 0;
    rv_note_on(self, params);
    if (((rv_voice_t *)self)->tongued) ww_clear(&ww[vi]);      /* the engine started a fresh air column; a slur keeps both */
}

static void ww_render(void *self, int16_t *out, const int16_t *k)
{
    int vi = op1_voice_index(self); if (vi < 0 || vi >= OP1_VOICES) vi = 0;
    ww_t *w = &ww[vi];
    if ((uint8_t)vi <= ww_last) ww_blk++;                       /* a new block */
    ww_last = (uint8_t)vi;
    if (ww_seen[vi] + 1 != ww_blk && w->half != WW_ALIGN) {     /* blocks missed: line up again */
        if (w->half == WW_REST) for (int i = 0; i < 4; i++) w->x[i] = w->x[OP1_BLOCK + i];
        w->half = WW_ALIGN;
    }
    ww_seen[vi] = ww_blk;
    if (((rv_voice_t *)self)->sig != SIG) ww_clear(w);         /* the engine resets itself in rv_render */
    if (w->half == WW_ALIGN) w->half = ((ww_blk ^ (uint8_t)vi) & 1) ? WW_WAIT : WW_COMPUTE;
    if (w->half == WW_WAIT) {                                    /* the other phase's call: silence, compute next */
        for (int n = 0; n < OP1_BLOCK; n++) out[n] = 0;
        w->half = WW_COMPUTE;
        return;
    }
    if (w->half == WW_COMPUTE) {
        ww_inc[0] = (int32_t)(ww_real_inc() * 2u);
        ww_rv_render(self, &w->x[4], k);
    }
    const int16_t *x = &w->x[w->half == WW_REST ? 4 + OP1_BLOCK / 2 : 4];
    for (int n = 0; n < OP1_BLOCK / 2; n++) {
        out[2 * n] = x[n - 2];
        out[2 * n + 1] = (int16_t)clampi((-x[n - 3] + 9 * x[n - 2] + 9 * x[n - 1] - x[n]) >> 4, -32768, 32767);
    }
    if (w->half == WW_REST) { for (int i = 0; i < 4; i++) w->x[i] = w->x[OP1_BLOCK + i]; w->half = WW_COMPUTE; }
    else w->half = WW_REST;
}

const op1_engine_t ENGINE_reedophone = { ww_note_on, ww_render, rv_note_off,
                                         { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE } };
