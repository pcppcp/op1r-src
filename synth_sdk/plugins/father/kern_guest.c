/* Father's Blackfin build (father_dsp.c with father_kern.S) against the plain C reference (father_reference.c) on
 * bcore (kern_check.py): mw_process and mw_ref_process on the same input, every output sample and the state (all but
 * the per-callback scratch rows) compared after every callback. Scenarios: every control endpoint with full-scale
 * alternating input then a tail (the saturating paths), changing controls on full-scale noise alternating mono / stereo, the gate closing and
 * opening on bursts, and long runs so every ring head passes every position (the rings wrap inside callbacks).
 * Prints "KERN ok blocks=N" or the first difference.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include <stdint.h>
#include "father_dsp.h"
void mw_ref_init(mw_state *);
void mw_ref_process(mw_state *, int16_t *, int16_t *, const int32_t *);
void t2_write(const char *, int);
uint32_t t2_r246_calls[2];
static mw_state s __attribute__((aligned(32))), t __attribute__((aligned(32)));
static unsigned blocks, bad;
static char line[96];
static int n;
static void put(const char *p) { while (*p) line[n++] = *p++; }
static void num(uint32_t v) { char b[11]; int i = 10; b[i] = 0; do b[--i] = '0' + v % 10; while (v /= 10); put(b + i); }
static int same(const void *a, const void *b, unsigned len)
{
    const uint8_t *x = a, *y = b;
    for (unsigned i = 0; i < len; i++) if (x[i] != y[i]) return 0;
    return 1;
}
static void block(int16_t *l, int16_t *r, const int32_t q[4], int stereo, unsigned what)
{
    int16_t a[64], b[64];
    for (int i = 0; i < 64; i++) { a[i] = l[i]; b[i] = r[i]; }
    mw_process(&s, l, stereo ? r : 0, q);
    mw_ref_process(&t, a, stereo ? b : 0, q);
    blocks++;
    if (bad) return;
    if (!same(l, a, sizeof a) || (stereo && !same(r, b, sizeof b)) || !same(&s, &t, __builtin_offsetof(mw_state, rows))
        || !same(&s.phase, &t.phase, sizeof s - __builtin_offsetof(mw_state, phase))) {
        bad = 1;
        put("KERN differ scenario="); num(what); put(" block="); num(blocks);
        put(same(l, a, sizeof a) ? " (state)\n" : " (output)\n");
    }
}
static uint32_t rng = 12345;
static int16_t rnd(void) { rng = rng * 1664525u + 1013904223u; return (int16_t)(rng >> 16); }
int main(void)
{
    int32_t q[4];
    int16_t l[64], r[64];
    for (unsigned e = 0; e < 16 && !bad; e++) {                  /* endpoints, full scale, then a tail */
        for (unsigned j = 0; j < 4; j++) q[j] = (e & (1u << j)) ? 32767 : 0;
        mw_init(&s); mw_ref_init(&t);
        for (unsigned k = 0; k < 160 && !bad; k++) {
            for (unsigned i = 0; i < 64; i++) { l[i] = k < 40 ? (i & 1 ? 32767 : -32768) : 0; r[i] = k < 40 ? (i & 2 ? 32767 : -32768) : 0; }
            block(l, r, q, e & 1, 1);
        }
    }
    mw_init(&s); mw_ref_init(&t);                              /* changing controls, noise, mono / stereo */
    for (unsigned k = 0; k < 1200 && !bad; k++) {
        for (unsigned j = 0; j < 4; j++) q[j] = (int32_t)((k * (j + 17) * 193u) & 32767);
        for (unsigned i = 0; i < 64; i++) { l[i] = rnd(); r[i] = rnd(); }
        block(l, r, q, k & 1, 2);
    }
    mw_init(&s); mw_ref_init(&t);                              /* gate: bursts and silence, gate on */
    for (unsigned k = 0; k < 900 && !bad; k++) {
        q[0] = 20000; q[1] = k < 450 ? 9000 : 30000; q[2] = 12000; q[3] = 26000;
        for (unsigned i = 0; i < 64; i++) { l[i] = (k % 60) < 8 ? rnd() >> 1 : 0; r[i] = l[i]; }
        block(l, r, q, k >= 600, 3);
    }
    mw_init(&s); mw_ref_init(&t);                              /* a long mono run at defaults: every head position */
    q[0] = 16384; q[1] = 0; q[2] = 18022; q[3] = 9830;
    for (unsigned k = 0; k < 1700 && !bad; k++) {
        for (unsigned i = 0; i < 64; i++) l[i] = (k & 63) < 20 ? rnd() >> 2 : 0;
        if ((k & 255) == 128) q[0] = (q[0] + 7919) & 32767;   /* the predelay slews through the ring */
        block(l, r, q, 0, 4);
    }
    if (!bad) { put("KERN ok blocks="); num(blocks); put("\n"); }
    t2_write(line, n);
    return 0;
}
