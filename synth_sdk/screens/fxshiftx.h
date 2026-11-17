/* FX Shift words (op1_plugin.h op1_fx_shift_ext / op1_fx_shift_words): four more saved values per FX, edited with
 * Shift + turn on the FX page, separate from the eight knob words, so an FX can have both page-2 presses (words 5..7)
 * and a Shift layer; eight with a second Shift layer (Shift + an encoder press switches 1 <-> 2, not saved). The pure part (shared with harness/fxshiftx_test.c); the R246 side is screens/fxshiftx.c.
 *   Storage: synth / drum slots in the patch JSON as "op1r_fx": {"fx": name, "v": [8 ints]} (kept across a load by
 *   dp_keep_op1r, drumplus/dp_core.c); the master in op1rsettings "mfxs0".."mfxs7" + "mfxsn" (the FX number). The
 *   values belong to the FX that set them: another FX at that slot starts from its own defaults.
 * Copyright (C) 2026 pcppcp. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef FXSHIFTX_H
#define FXSHIFTX_H
#include <stdint.h>

#define FXX_MAX 16                                    /* FX that registered */
#define FXX_K 8                                       /* Shift values: layer 1 = 0..3, layer 2 = 4..7 (knob + 4) */
typedef struct { const void *fx; uint8_t n[FXX_K]; int16_t dflt[FXX_K]; } fxx_reg_t;
typedef struct { int16_t v[FXX_K]; int16_t owner; } fxx_words_t;   /* owner: the FX number the values belong to, -1 none */

static fxx_reg_t fxx_tab[FXX_MAX];
static int fxx_find(const void *fx, int make)
{
    int f = -1;
    for (int i = 0; i < FXX_MAX; i++) {
        if (fxx_tab[i].fx == fx) return i;
        if (f < 0 && !fxx_tab[i].fx) f = i;
    }
    if (f < 0 || !make) return -1;
    fxx_tab[f].fx = fx;
    return f;
}
static void fxx_register(const void *fx, int knob, int n, int dflt)
{
    if (!fx || knob < 0 || knob >= FXX_K || n < 1 || n > 127) return;
    const int f = fxx_find(fx, 1);
    if (f < 0) return;
    fxx_tab[f].n[knob] = (uint8_t)n;
    fxx_tab[f].dflt[knob] = (int16_t)(dflt < 0 ? 0 : dflt > 32767 ? 32767 : dflt);
}
static inline int fxx_n(const void *fx, int k)        /* steps of Shift value k (0: not registered) */
{
    const int f = fx && k >= 0 && k < FXX_K ? fxx_find(fx, 0) : -1;
    return f < 0 ? 0 : fxx_tab[f].n[k];
}
static inline int fxx_layers(const void *fx)          /* 2 when any value of layer 2 is registered */
{
    for (int k = 4; k < FXX_K; k++) if (fxx_n(fx, k)) return 2;
    return 1;
}
/* the words for FX number e (registration fx): kept if they are e's, else e's defaults (a new FX at the slot) */
static inline const int16_t *fxx_get(fxx_words_t *w, const void *fx, int e)
{
    static const int16_t none[FXX_K];
    const int f = fx ? fxx_find(fx, 0) : -1;
    if (f < 0) return none;                           /* not registered (yet): zeros, the slot's values untouched */
    if (w->owner != e) {
        for (int k = 0; k < FXX_K; k++) w->v[k] = fxx_tab[f].dflt[k];
        w->owner = (int16_t)e;
    }
    return w->v;
}
/* Shift + turn of knob k (n steps one a click of 1024, n = 1: fine, delta / 4). 1 = the value changed */
static inline int fxx_turn(int32_t *acc, int16_t *v, int delta, int n)
{
    if (n <= 1) {
        const int32_t x = *v + delta / 4, c = x < 0 ? 0 : x > 32767 ? 32767 : x;
        if (c == *v) return 0;
        *v = (int16_t)c; return 1;
    }
    if ((*acc > 0 && delta < 0) || (*acc < 0 && delta > 0)) *acc = 0;
    *acc += delta;
    const int32_t st = *acc / 1024;
    if (!st) return 0;
    *acc -= st * 1024;
    int32_t i = ((*v * n) >> 15) + st;
    i = i < 0 ? 0 : i > n - 1 ? n - 1 : i;
    const int16_t nv = (int16_t)(((2 * i + 1) * 32768) / (2 * n));
    if (nv == *v) return 0;
    *v = nv; return 1;
}
#endif
