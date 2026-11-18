/* reedie at R246's voice rate: an adapter that includes reedie.c unchanged and plays its 44.1 kHz model in real time.
 *
 * reedie is a 44.1 kHz design, 128 samples a block: the breath filters (200 Hz high-pass, ~4 kHz low-pass), the chatter
 * smoother (~45 Hz), the output tilt (~1.5 kHz), the reed's 20..66 ms start, its 34 ms attack blow, the 100 ms release and
 * the DMIN / INCMAX limits (5.5 kHz) are all counted in 44.1 kHz samples. R246 calls a voice's render 689 times a second
 * for 128 samples at 88.2 kHz, and OP1_PITCH_INC counts 88.2 kHz steps (2^32 f / 88200; op1r's op1_plugin.h calls it
 * OP1_VOICE_RATE). Run as it stands, the pitch is right (the bore length comes from the increment) but every time is
 * twice as fast, every filter corner an octave up and the release 50 ms. So, as windwood's ww_rate.c in the hundekraft
 * kennel port: the engine runs as designed once per two calls, a fresh 128-sample block at 44.1 kHz with the increment
 * doubled, and each call plays 64 of its samples, upsampled 2x by the midpoint filter (-1, 9, 9, -1) / 16. The output
 * is delayed by two 44.1 kHz samples. Half the calls compute, so the CPU per voice halves on average; even and odd
 * voices take turns (voice index parity), so a chord's blocks split between the two calls. A voice that missed blocks
 * lines up again. There is no slur: every note_on is a fresh start, as in reedie.c.
 * Written by Hundekraft from nvmsocool's reedie.c (Copyright (C) 2026 nvmsocool).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"

static inline uint32_t rd_real_inc(void) { return (uint32_t)OP1_PITCH_INC[0]; }   /* 88.2 kHz steps, before the redefinition */
static int32_t rd_inc[1];                                                        /* what the engine reads: 44.1 kHz steps */
#undef OP1_PITCH_INC
#define OP1_PITCH_INC ((const int32_t *)rd_inc)
#define ENGINE_reedie ENGINE_reedie_44k
#include "reedie.c"
#undef ENGINE_reedie
#ifndef RD_RENDER
#define RD_RENDER rd_render           /* reedie.c names the render it uses RD_RENDER once it has a fast one (rd_fast.h) */
#endif

enum { RW_COMPUTE, RW_REST, RW_WAIT, RW_ALIGN };
typedef struct {
    int16_t x[4 + OP1_BLOCK];        /* the last four 44.1 kHz samples of the previous block, then this block */
    uint8_t half;                    /* RW_COMPUTE: compute a block, play its first 64 samples; RW_REST: play the rest;
                                      * RW_WAIT: silence this call, compute the next; RW_ALIGN: fresh start, choose at the first call */
} rw_t;
static rw_t rw[OP1_VOICES];
static uint32_t rw_blk, rw_seen[OP1_VOICES];   /* blocks counted; the block each voice was last rendered in */
static uint8_t rw_last = 0xff;                  /* the voice rendered last */

static void rw_clear(rw_t *w) { for (int i = 0; i < 4 + OP1_BLOCK; i++) w->x[i] = 0; w->half = RW_ALIGN; }

static void rw_note_on(void *self, const int16_t *params)
{
    int vi = op1_voice_index(self); if (vi < 0 || vi >= OP1_VOICES) vi = 0;
    rd_note_on(self, params);
    rw_clear(&rw[vi]);
}

static void rw_render(void *self, int16_t *out, const int16_t *k)
{
    int vi = op1_voice_index(self); if (vi < 0 || vi >= OP1_VOICES) vi = 0;
    rw_t *w = &rw[vi];
    if ((uint8_t)vi <= rw_last) rw_blk++;                       /* a new block: voices render in index order */
    rw_last = (uint8_t)vi;
    if (rw_seen[vi] + 1 != rw_blk && w->half != RW_ALIGN) {     /* blocks missed: line up again */
        if (w->half == RW_REST) for (int i = 0; i < 4; i++) w->x[i] = w->x[OP1_BLOCK + i];
        w->half = RW_ALIGN;
    }
    rw_seen[vi] = rw_blk;
    if (w->half == RW_ALIGN) w->half = ((rw_blk ^ (uint8_t)vi) & 1) ? RW_WAIT : RW_COMPUTE;
    if (w->half == RW_WAIT) {                                    /* the other phase's call: silence, compute next */
        for (int n = 0; n < OP1_BLOCK; n++) out[n] = 0;
        w->half = RW_COMPUTE;
        return;
    }
    if (w->half == RW_COMPUTE) {
        rd_inc[0] = (int32_t)(rd_real_inc() * 2u);
        RD_RENDER(self, &w->x[4], k);
    }
    const int16_t *x = &w->x[w->half == RW_REST ? 4 + OP1_BLOCK / 2 : 4];
    int32_t x3 = x[-3], x2 = x[-2], x1 = x[-1];            /* a sliding window: one load a sample */
    for (int n = 0; n < OP1_BLOCK / 2; n++) {
        int32_t x0 = x[n], m = x2 + x1;
        out[2 * n] = (int16_t)x2;
        out[2 * n + 1] = (int16_t)rd_clamp((9 * m - x3 - x0) >> 4, -32768, 32767);
        x3 = x2; x2 = x1; x1 = x0;
    }
    if (w->half == RW_REST) { for (int i = 0; i < 4; i++) w->x[i] = w->x[OP1_BLOCK + i]; w->half = RW_COMPUTE; }
    else w->half = RW_REST;
}

const op1_engine_t ENGINE_reedie = { rw_note_on, rw_render, rd_note_off,
                                     { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE } };
