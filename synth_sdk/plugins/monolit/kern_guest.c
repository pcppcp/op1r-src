/* MONOLIT's Blackfin kernels against its C loop on bcore (kern_check.py): monolit.c built twice, with monolit_dsp.S
 * and with MONOLIT_C_LOOP (the C loop, symbols renamed ref_*), driven with the same calls and compared sample by
 * sample. Scenarios: random knob words (negative ones and the extremes too, page 2 words included, so every shape
 * zone, body interval, contour and heat), velocity words (0 and out of range too), notes struck and released on
 * random voices, voices rendered out of order or skipped, and pitch words steady, gliding, random over the whole
 * 32-bit range (the polyBLEP windows, t * rdt wrapping, increments of 2^31 and more) and near the top.
 * Prints "KERN ok blocks=N" or the first difference.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include <stdint.h>
typedef struct {
    void (*note_on)(void *, const int16_t *);
    void (*render)(void *, int16_t *, const int16_t *);
    void (*note_off)(void *);
    uint8_t press[4];
} eng;
extern const eng ENGINE_monolit, ref_ENGINE_monolit;
void t2_write(const char *, int);
uint32_t t2_r246_calls[2];
volatile uint8_t op1_eng_page[4];
void *op1_l1_claim_state(int part, void *home, uint32_t state_bytes, uint32_t bytes, int *fresh)
{ (void)part; (void)home; (void)state_bytes; (void)bytes; if (fresh) *fresh = 0; return 0; }
void *op1_l1_claim(int part, const void *owner, uint32_t bytes, int *fresh)
{ (void)part; (void)owner; (void)bytes; if (fresh) *fresh = 0; return 0; }
const int16_t *op1_l1_sine_quarter(void) { return 0; }
static unsigned blocks, bad;
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
    ENGINE_monolit.render(voice(v), a, k);
    ref_ENGINE_monolit.render(voice(v), b, k);
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
    case 1: return base + (base >> 10) * (uint32_t)(i & 127);         /* gliding up fast */
    case 2: return base - (base >> 9) * (uint32_t)i;                  /* gliding down */
    case 3: return rnd();                                             /* anything */
    case 4: return 0xFFFFFFFFu - (rnd() >> 8);                        /* the top */
    default: return rnd() >> (rnd() & 31);                            /* any octave, 0 too */
    }
}

static void scenarios(void)
{
    int16_t k[8] = { 16384, 14745, 11469, 9830, 0, 0, 0, 0 };
    for (unsigned s = 0; s < 240 && !bad; s++) {
        const unsigned mode = s % 6;
        uint32_t base = rnd() >> (2 + (rnd() % 10));
        for (int v = 0; v < 6; v++) VEL[v] = (int16_t)(s % 5 == 4 ? rnd() : (rnd() % 128) << 8);
        for (int j = 0; j < 8; j++) if (j != 4) k[j] = knob();
        for (int v = 0; v < 6; v++) if (rnd() & 1) { ENGINE_monolit.note_on(voice(v), k); ref_ENGINE_monolit.note_on(voice(v), k); }
        for (unsigned r = 0; r < 24 && !bad; r++) {
            for (int i = 0; i < 128; i++) PITCH[i] = pitch_word(mode, base, i);
            if ((rnd() & 3) == 0) { const int j = (int)(rnd() & 7); if (j != 4) k[j] = knob(); }
            if ((rnd() & 7) == 0) {
                const int v = (int)(rnd() % 6);
                ENGINE_monolit.note_on(voice(v), k);
                ref_ENGINE_monolit.note_on(voice(v), k);
            }
            if ((rnd() & 7) == 0) {
                const int v = (int)(rnd() % 6);
                ENGINE_monolit.note_off(voice(v));
                ref_ENGINE_monolit.note_off(voice(v));
            }
            if (s & 8) {                                              /* out of order, some voices skipped */
                for (int t = 0; t < 6 && !bad; t++) if (rnd() & 1) render_both((int)(rnd() % 6), k);
            } else
                for (int v = 0; v < 6 && !bad; v++) render_both(v, k);
        }
    }
}

int main(void)
{
    scenarios();
    if (!bad) { put("KERN ok blocks="); num(blocks); put("\n"); }
    t2_write(line, n);
    return 0;
}
