/* Pew's Blackfin kernels against its C loop on bcore (kern_check.py): pew_src_pew.c built twice, with pew_dsp.S and
 * with PEW_C_LOOP (the C loop, symbols renamed ref_*), driven with the same calls and compared sample by sample.
 * Scenarios: random knobs (negative ones too), velocity words (0 and out-of-range ones too), notes struck on random
 * voices (the key click, the percussion's single trigger), notes released, page-2 pushes, voices rendered out of
 * order or skipped, and pitch words over the whole 32-bit range (the Doppler product wraps, the table index reaches
 * 511). Then pew_out alone on random words, amps, clicks, knees and gains up to 2 * 32767, against a copy of its C
 * formula, which reaches the output saturation more often than pew's own gain does. Prints "KERN ok blocks=N" or the first difference.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include <stdint.h>
void pew_note_on(void *, const int16_t *);
void pew_render(void *, int16_t *, const int16_t *);
void ref_note_on(void *, const int16_t *);
void ref_render(void *, int16_t *, const int16_t *);
void pew_note_off(void *, const int16_t *);
void ref_note_off(void *, const int16_t *);
void pew_out(int16_t *out, const uint32_t *mh, const int32_t *c);
void t2_write(const char *, int);
uint32_t t2_r246_calls[2];
static unsigned blocks, bad, sats;
static char line[96];
static int n;
static void put(const char *p) { while (*p) line[n++] = *p++; }
static void num(uint32_t v) { char b[11]; int i = 10; b[i] = 0; do b[--i] = '0' + v % 10; while (v /= 10); put(b + i); }
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng; }
static void *voice(int v) { return (void *)(uintptr_t)(0xFFB00000u + 0x24u * (uint32_t)v); }
#define PITCH ((volatile uint32_t *)0xFF801950)
#define VEL ((volatile int16_t *)0xFF801064)

static void fail(const char *what, unsigned i)
{
    bad = 1;
    put("KERN differ "); put(what); put(" block="); num(blocks); put(" i="); num(i); put("\n");
}

static void render_both(int v, const int16_t *k)
{
    int16_t a[128], b[128];
    pew_render(voice(v), a, k);
    ref_render(voice(v), b, k);
    blocks++;
    for (int i = 0; i < 128 && !bad; i++)
        if (a[i] != b[i]) fail("render", (unsigned)i);
}

static int16_t knob(void)
{
    uint32_t r = rnd() >> 16;
    switch (r & 7) {
    case 0: return 0;
    case 1: return 32767;
    case 2: return (int16_t)-(int32_t)(r >> 4);
    default: return (int16_t)(r >> 1);
    }
}

static uint32_t pitch_word(unsigned mode, uint32_t base, int i)
{
    switch (mode) {
    case 0: return base;                                              /* steady */
    case 1: return base + (base >> 13) * (uint32_t)(i & 63);           /* moving */
    case 2: return rnd();                                             /* anything */
    default: return 0xFFFFFFFFu - (rnd() >> 8);                       /* the top */
    }
}

static void scenarios(void)
{
    int16_t k[8] = { 16384, 8192, 0, 8192, 0, 0, 0, 0 };
    for (unsigned s = 0; s < 64 && !bad; s++) {
        const unsigned mode = s & 3;
        uint32_t base = rnd() >> (1 + (rnd() & 7));
        for (int v = 0; v < 6; v++) VEL[v] = (int16_t)(s % 5 == 4 ? rnd() : (rnd() % 128) << 8);
        for (int j = 0; j < 4; j++) k[j] = knob();
        for (int j = 5; j < 8; j++) k[j] = (s & 8) ? knob() : 0;
        for (int v = 0; v < 6; v++) if (rnd() & 1) { pew_note_on(voice(v), k); ref_note_on(voice(v), k); }
        for (unsigned r = 0; r < 24 && !bad; r++) {
            for (int i = 0; i < 128; i++) PITCH[i] = pitch_word(mode, base, i);
            if ((rnd() & 3) == 0) k[rnd() & 3] = knob();
            if ((rnd() & 7) == 0) {
                const int v = (int)(rnd() % 6);
                pew_note_on(voice(v), k);
                ref_note_on(voice(v), k);
            }
            if ((rnd() & 7) == 0) {
                const int v = (int)(rnd() % 6);
                pew_note_off(voice(v), k);
                ref_note_off(voice(v), k);
            }
            if (s & 4) {                                              /* out of order, some voices skipped */
                for (int t = 0; t < 6 && !bad; t++) if (rnd() & 1) render_both((int)(rnd() % 6), k);
            } else
                for (int v = 0; v < 6 && !bad; v++) render_both(v, k);
        }
    }
}

static int16_t sat(int64_t x) { return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }
static uint32_t mh[129];
static int32_t cb[128];
static void out_alone(void)
{
    for (unsigned t = 0; t < 2000 && !bad; t++) {
        const int32_t da = (int32_t)(rnd() >> 17), ha = (int32_t)(rnd() >> 17), gain = (int32_t)(rnd() >> 17);
        const int click = t & 1;
        for (int i = 0; i < 128; i++) { mh[i] = rnd(); cb[i] = click ? (int32_t)(int16_t)(rnd() >> 16) : 0; }
        const int32_t kp = (int32_t)(rnd() >> 17), kn = -(int32_t)(rnd() >> 17);
        const int32_t c[5] = { (int32_t)((uint32_t)ha << 16 | (uint32_t)da), gain * 2, click ? (int32_t)(uintptr_t)cb : 0, kp,
                               kn };
        int16_t out[128];
        pew_out(out, mh, c);
        blocks++;
        for (int i = 0; i < 128 && !bad; i++) {
            const int32_t lo = (int16_t)mh[i], hi = (int16_t)(mh[i] >> 16);
            int32_t y = (lo * da + hi * ha) >> 14;
            y += cb[i];
            if (y > kp) y = kp + ((y - kp) >> 2);
            if (y < kn) y = kn + ((y - kn) >> 2);
            const int64_t p = ((int64_t)y * gain) >> 15;
            if (p > 32767 || p < -32768) sats++;
            if (out[i] != sat(p)) fail("pew_out", (unsigned)i);
        }
    }
}

int main(void)
{
    scenarios();
    out_alone();
    if (!bad) { put("KERN ok blocks="); num(blocks); put(" saturated="); num(sats); put("\n"); }
    t2_write(line, n);
    return 0;
}
