/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission. */
/* One fixed run through mw_process with CLASSIC on, hashed (64-bit FNV-1a over every output sample): father_test.c
   asserts CLASSIC_HASH, and classic_check.py builds the same run against kennel bd899531's father_dsp.c (hundekraft
   3.1.4, CLASSIC_ON defined empty: its only mode) and prints the hash both builds give. The run: 6,000 calls of
   full-scale noise with every control changing, mono and stereo in turn; 1,500 calls of gated bursts and silence
   (the gate opening and closing, the predelay slewing); 1,200 calls of a decaying buzz in stereo at the defaults (mix 20). */
#ifndef CLASSIC_RUN_H
#define CLASSIC_RUN_H
#ifndef CLASSIC_ON
#define CLASSIC_ON(st) ((st)->classic = 1)
#endif
#define CLASSIC_HASH 0xf9efc022308538aaull   /* bd899531 (classic_check.py) */
static mw_state cr_s __attribute__((aligned(32)));
static uint64_t cr_hash;
static void cr_put(const int16_t *x, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        cr_hash = (cr_hash ^ (uint16_t)x[i]) * 0x100000001B3ull;
    }
}
static uint64_t classic_run(void)
{
    int32_t q[4];
    int16_t l[64], r[64];
    unsigned rng = 777;
    cr_hash = 0xCBF29CE484222325ull;
    mw_init(&cr_s); CLASSIC_ON(&cr_s);
    for (unsigned k = 0; k < 6000; k++) {
        for (unsigned j = 0; j < 4; j++) q[j] = (int32_t)((k * (j + 17) * 193u) & 32767);
        for (unsigned i = 0; i < 64; i++) {
            rng = rng * 1664525u + 1013904223u; l[i] = (int16_t)(rng >> 16);
            rng = rng * 1664525u + 1013904223u; r[i] = (int16_t)(rng >> 16);
        }
        mw_process(&cr_s, l, k & 1 ? r : 0, q);
        cr_put(l, 64); if (k & 1) cr_put(r, 64);
    }
    mw_init(&cr_s); CLASSIC_ON(&cr_s);
    for (unsigned k = 0; k < 1500; k++) {
        q[0] = k < 700 ? 20000 : 6000; q[1] = k < 450 ? 9000 : (k < 1000 ? 30000 : 0); q[2] = 12000; q[3] = 26000;
        for (unsigned i = 0; i < 64; i++) {
            rng = rng * 1664525u + 1013904223u;
            l[i] = (k % 60) < 8 ? (int16_t)((int32_t)(rng >> 16) - 32768) / 2 : 0;
            r[i] = (k % 60) < 8 ? (int16_t)(l[i] / 2) : 0;
        }
        mw_process(&cr_s, l, k >= 750 ? r : 0, q);
        cr_put(l, 64); if (k >= 750) cr_put(r, 64);
    }
    mw_init(&cr_s); CLASSIC_ON(&cr_s);
    q[0] = 16384; q[1] = 0; q[2] = 18022; q[3] = 6620;
    for (unsigned k = 0; k < 1200; k++) {
        for (unsigned i = 0; i < 64; i++) {
            const unsigned n = k * 64 + i, t = n % 44100;
            const int32_t env = t < 22050 ? 12000 - (int32_t)(t * 12000u / 22050u) : 0;
            const int32_t a = (int32_t)((n * 389u) & 1023) - 512, b = (int32_t)((n * 491u) & 1023) - 512;
            l[i] = (int16_t)(a * env >> 9); r[i] = (int16_t)(b * env >> 9);
        }
        mw_process(&cr_s, l, r, q);
        cr_put(l, 64); cr_put(r, 64);
    }
    return cr_hash;
}
#endif
