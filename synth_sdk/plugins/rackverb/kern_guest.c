/* rackverb's Blackfin block engine (rv_mv.c with rv_kern.S) against the plain C reference (rv_mv_run_ref) on bcore
 * (kern_check.py): every 4th program of each unit plus all modulated ones, noise then silence, calls of 34/35 and
 * 45/46 samples, SIZE moving, LFOs moving; outputs and the whole ring compared after every call.
 * Prints "KERN ok calls=N" or the first difference.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include <stdint.h>
#include "rv_programs.h"
void t2_write(const char *, int);
uint32_t t2_r246_calls[2];
static rv_mv_t a __attribute__((aligned(32))), b __attribute__((aligned(32)));
static unsigned calls, bad;
static char line[128];
static int n;
static void put(const char *p) { while (*p) line[n++] = *p++; }
static void num(uint32_t v) { char s[11]; int i = 10; s[i] = 0; do s[--i] = (char)('0' + v % 10); while (v /= 10); put(s + i); }
static int same(const void *x, const void *y, unsigned len)
{
    const uint8_t *p = x, *q = y;
    for (unsigned i = 0; i < len; i++) if (p[i] != q[i]) return 0;
    return 1;
}
static uint32_t rng = 777;
static int16_t rnd(void) { rng = rng * 1664525u + 1013904223u; return (int16_t)((int32_t)rng >> 19); }
static void prog(const rv_row_t *row, int32_t fs2, int big)
{
    for (unsigned i = 0; i < sizeof a; i++) ((uint8_t *)&a)[i] = 0;
    if (rv_mv_build(&a.prog, row, fs2) < 0) { bad = 1; put("KERN build "); put(row->name); put("\n"); return; }
    rv_mv_size(&a.prog, 32767);
    b = a;
    for (unsigned c = 0; c < 400 && !bad; c++) {
        int cnt = (big ? 45 : 34) + (int)(c & 1);
        int16_t in[48], l1[48], r1[48], l2[48], r2[48];
        for (int i = 0; i < cnt; i++) in[i] = c < 250 ? rnd() : 0;
        if ((c & 63) == 32) { int32_t z = (int32_t)((c * 977u) & 32767); rv_mv_size(&a.prog, z); rv_mv_size(&b.prog, z); }
        rv_mv_run(&a, in, l1, r1, cnt); rv_mv_lfo(&a.prog, cnt);
        rv_mv_run_ref(&b, in, l2, r2, cnt); rv_mv_lfo(&b.prog, cnt);
        calls++;
        if (!same(l1, l2, 2 * cnt) || !same(r1, r2, 2 * cnt) || !same(a.ring, b.ring, sizeof a.ring)) {
            bad = 1; put("KERN differ "); put(row->name); put(" call="); num(c); put("\n");
        }
    }
}
int main(void)
{
    for (int i = 0; i < 63 && !bad; i += 4) prog(&rv_verb1[i], 46875, 0);
    for (int i = 1; i < 100 && !bad; i += (i >= 50 && i < 70) ? 1 : 4) prog(&rv_verb2[i], 62500, 1);
    for (int i = 0; i < 63 && !bad; i += 4) prog(&rv_fex[i], 46875, 0);
    if (!bad) { put("KERN ok calls="); num(calls); put("\n"); }
    t2_write(line, n);
    return 0;
}
