/* Shift + turn and a push flag on a custom FX's page (op1_plugin.h op1_fx_shift / op1_fx_push_bit; after hundekraft
 * 3.3 kennel/kn_fxshift.h, Hundekraft, GPL-3.0-or-later with the OP-1 firmware permission: the same API and word
 * layout, so kennel FX that use it, e.g. khorus, run unchanged). An FX registers from its screen's init; only an FX that
 * did is touched (fxs_n / fxs_push return 0 for every other FX, and dispatcher.c runs its paths as before). Words:
 *   blue word 5, green word 6: n = 2..127 one step a click, step i's word (2i+1) * 32768 / (2n), an unset word (<= 0)
 *     from step 0; n = 1 fine (delta / 4 a tick, 1 .. 32767)
 *   white bits 0..6, red bits 8..14 of word 7: a 7-bit field, one unit a click through 0 .. n-1 (n = 2..127) or
 *     0..127 (n = 1); bits 7 and 15 are the FX's own (a push flag may own one)
 *   a push of knob k registered with op1_fx_push_bit toggles bit 7 or 15 of word 7 (press only; the release is taken)
 * Words 0..4 are never written here. One step = one encoder click = delta 1024 on the device (kennel counts 2048, its
 * emulator's detent). Included once by dispatcher.c (the exported functions) and by harness/fxshift_test.c. */
#ifndef FXSHIFT_H
#define FXSHIFT_H
#include <stdint.h>

#define FXS_MAX 8
#define FXS_CLICK 1024
static struct { const void *fx; uint8_t sh[4]; int8_t bit[4]; } fxs_tab[FXS_MAX];

static int fxs_slot(const void *fx, int make)
{
    int f = -1;
    for (int i = 0; i < FXS_MAX; i++) {
        if (fxs_tab[i].fx == fx) return i;
        if (f < 0 && !fxs_tab[i].fx) f = i;
    }
    if (f < 0 || !make) return -1;                    /* table full: the FX keeps the stock turn */
    fxs_tab[f].fx = fx;
    for (int k = 0; k < 4; k++) fxs_tab[f].bit[k] = -1;
    return f;
}
void op1_fx_shift(const op1_fx_t *fx, int knob, int n)
{
    if (!fx || knob < 0 || knob > 3 || n < 0 || n > 127) return;
    const int f = fxs_slot(fx, 1);
    if (f >= 0) fxs_tab[f].sh[knob] = (uint8_t)n;
}
void op1_fx_push_bit(const op1_fx_t *fx, int knob, int bit)
{
    if (!fx || knob < 0 || knob > 3 || (bit != 7 && bit != 15)) return;
    const int f = fxs_slot(fx, 1);
    if (f >= 0) fxs_tab[f].bit[knob] = (int8_t)bit;
}
static inline int fxs_n(const void *fx, int knob)    /* the FX's shift steps for knob k (0: not registered) */
{
    const int f = fx && knob >= 0 && knob < 4 ? fxs_slot(fx, 0) : -1;
    return f < 0 ? 0 : fxs_tab[f].sh[knob];
}
static inline int fxs_step(int32_t *acc, int16_t *word, int delta, int n)   /* n steps, one a click; 1 = moved */
{
    if ((*acc > 0 && delta < 0) || (*acc < 0 && delta > 0)) *acc = 0;
    *acc += delta;
    int32_t st = *acc / FXS_CLICK;
    if (!st) return 0;
    *acc -= st * FXS_CLICK;
    int32_t i = (((*word > 0 ? *word : 0) * n) >> 15) + st;
    i = i < 0 ? 0 : (i > n - 1 ? n - 1 : i);
    *word = (int16_t)(((2 * i + 1) * 32768) / (2 * n));
    return 1;
}
static inline int fxs_turn(int32_t *acc, int16_t *w, int knob, int delta, int n)   /* Shift + turn: 1 = a word changed */
{
    if (knob < 2) {                                   /* blue word 5, green word 6 */
        int16_t *wd = &w[5 + knob];
        if (n > 1) return fxs_step(acc, wd, delta, n);
        const int32_t v = (*wd > 0 ? *wd : 0) + delta / 4;
        *wd = (int16_t)(v < 1 ? 1 : (v > 32767 ? 32767 : v));
        return 1;
    }
    const int sh = knob == 2 ? 0 : 8;                 /* white bits 0..6, red bits 8..14 of word 7 */
    const int m = n > 1 ? n : 128;
    int f = (w[7] >> sh) & 127;
    f = f < m ? f : m - 1;
    int16_t t = (int16_t)(((2 * f + 1) * 32768) / (2 * m));
    if (!fxs_step(acc, &t, delta, m)) return 0;
    const int g = (t * m) >> 15;
    w[7] = (int16_t)((w[7] & ~(127 << sh)) | g << sh);
    return 1;
}
static inline int fxs_push(const void *fx, int16_t *w, int knob, int pressed)   /* 1 = the FX owns the push */
{
    const int f = fx && knob >= 0 && knob < 4 ? fxs_slot(fx, 0) : -1;
    if (f < 0 || fxs_tab[f].bit[knob] < 0) return 0;
    if (pressed) w[7] = (int16_t)(w[7] ^ (1 << fxs_tab[f].bit[knob]));
    return 1;
}
/* word 7 written by anything but the FX's own push / shift (a MIDI CC, midi/mx.c): the FX's push bits keep their value */
static inline int16_t fxs_keep_push_bits(const void *fx, int16_t old, int16_t v)
{
    const int f = fx ? fxs_slot(fx, 0) : -1;
    if (f < 0) return v;
    uint16_t m = 0;
    for (int k = 0; k < 4; k++) if (fxs_tab[f].bit[k] >= 0) m |= (uint16_t)(1u << fxs_tab[f].bit[k]);
    return (int16_t)(((uint16_t)v & ~m) | ((uint16_t)old & m));
}
#endif
