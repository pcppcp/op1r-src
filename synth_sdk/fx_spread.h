/* FX init spread over blocks (FX_SPREAD_INIT, off by default; ARCHITECTURE.md "Audio budget", bursts 2026-10-27).
 * The core runs an FX's init in the first audio callback after it is selected (dispatcher.c), and the inits that
 * clear a delay or grain buffer cost 25k..65k bcore packets (more cycles on the unit: SDRAM stores) in that one
 * block. With FX_SPREAD_INIT the buffer is cleared FX_SPREAD_WORDS words per block instead, and the FX passes its
 * input through unchanged (dry) until the clear is done, then starts from silence as before. Audible: the effect
 * starts a few blocks later (each block 64 samples, about 2.9 ms), so an effect selected while playing comes in
 * after ~6..17 ms dry. The owner decides; without the flag nothing changes.
 * Integers only, no libc: plain 32-bit stores.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef FX_SPREAD_H
#define FX_SPREAD_H
#include <stdint.h>

#ifndef FX_SPREAD_WORDS
#define FX_SPREAD_WORDS 8192                       /* per block: about 8k packets, as a plain FX render */
#endif

typedef struct { uint32_t *p; uint32_t left; } fx_spread_t;

static inline void fx_spread_start(fx_spread_t *c, void *p, uint32_t words) { c->p = (uint32_t *)p; c->left = p ? words : 0; }

/* one block's slice: 1 while this block was spent clearing (the FX leaves its buffer as it came: dry), 0 after */
static inline int fx_spread_step(fx_spread_t *c)
{
    if (!c->left) return 0;
    uint32_t n = c->left < FX_SPREAD_WORDS ? c->left : FX_SPREAD_WORDS, *p = c->p;
    for (uint32_t i = 0; i < n; i++) p[i] = 0;
    c->p = p + n; c->left -= n;
    return 1;
}
#endif
