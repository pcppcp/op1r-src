/* Host test of dddd/dd_core.c (docs/53) + WAV renders to listen to.
 *   cc -O1 -Wall -Wextra -fsanitize=address,undefined -I.. dddd_test.c ../dddd/dd_core.c -lm -o build/dddd_test
 *   build/dddd_test [wav]      wav: out/dddd_*.wav (presets and a 60 s performance) */
#include "../dddd/dd_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define B 128
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static dd_t D;

static void run(const dd_par *p, int16_t *o, int n) { for (int t = 0; t < n; t += B) dd_render(&D, p, o + t, n - t < B ? n - t : B); }
static void notes(const uint8_t *k, int n) { dd_set_notes(&D, k, n); }

static void t_bounds(void)                                     /* every corner: bounded, no DC, never silent by bug */
{
    static int16_t o[DD_SR];
    static const int32_t v[3] = { 0, 16384, 32767 };
    int cases = 0;
    for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) for (int c = 0; c < 3; c++) for (int e = 0; e < 3; e++)
    for (int pg = 0; pg < 3; pg++) {
        dd_init(&D, 7 + cases); uint8_t k[3] = { 36, 60, 67 }; notes(k, 1 + cases % 3);
        dd_par p = { v[a], v[b], v[c], v[e], v[pg], v[2 - pg], v[pg], 24000, 30000 };
        run(&p, o, DD_SR);
        int32_t pk = 0; double sum = 0, sq = 0;
        for (int i = 0; i < DD_SR; i++) { int32_t x = abs(o[i]); if (x > pk) pk = x; sum += o[i]; sq += (double)o[i] * o[i]; }
        CHECK(pk <= 16384, "peak %d (case %d)", pk, cases);
        CHECK(fabs(sum / DD_SR) < 1024, "DC %.0f (case %d)", sum / DD_SR, cases);   /* < 6 % of half scale: the hardest settings clip lopsided */
        cases++;
    }
    printf("ok bounds (%d settings)\n", cases);
}

static void t_pitch(void)                                      /* the loop howls at the key: autocorrelation peak */
{
    static int16_t o[DD_SR];
    dd_init(&D, 3);
    uint8_t k[1] = { 60 }; notes(k, 1);
    dd_par p = { 0, 26000, 0, 0, 0, 0, 0, 32767, 32767 };      /* noise, feedback 0.99, clean, still */
    run(&p, o, DD_SR);
    double best = 0; int lag = 0;
    for (int l = 80; l < 1200; l++) {
        double c = 0, e0 = 0, e1 = 0;
        for (int i = DD_SR / 2; i < DD_SR - 1200; i++) { c += (double)o[i] * o[i + l]; e0 += (double)o[i] * o[i]; e1 += (double)o[i + l] * o[i + l]; }
        c /= sqrt(e0 * e1 + 1);
        if (c > best) { best = c; lag = l; }
    }
    CHECK(abs(lag - 337) <= 3, "loop period %d (want 337 for C4 at DD_SR), corr %.2f", lag, best);
    printf("ok pitch (period %d, corr %.2f)\n", lag, best);
}

static void t_para(void)                                       /* chords: one tap per key, newest four */
{
    dd_init(&D, 5);
    uint8_t k[6] = { 48, 52, 55, 60, 64, 67 };
    notes(k, 6);
    CHECK(D.ntaps == 4 && D.tap[3] == (int32_t)(dd_period(67) >> 8), "taps %d, %d", D.ntaps, D.tap[3]);
    uint8_t lo[1] = { 0 }; notes(lo, 1);
    CHECK(D.tap[0] < DD_BUF / 2, "low note tap %d fits the buffer", D.tap[0]);
    printf("ok paraphonic taps\n");
}

static void t_onset(void)                                      /* a new note after a loud one starts at full level */
{
    static int16_t o[DD_SR];
    dd_init(&D, 9);
    uint8_t k[1] = { 48 }; notes(k, 1);
    dd_par loud = { 32767, 32767, 32767, 0, 0, 0, 0, 32767, 32767 };
    run(&loud, o, DD_SR / 2);                                  /* the limiter pulls the gain down */
    dd_reset_loop(&D);                                         /* (silence: not rendered) a new note */
    dd_par p = { 0, 12000, 6000, 0, 0, 0, 0, 24000, 2000 };    /* attack: the envelope still low */
    run(&p, o, 4 * B);
    double e0 = 0, e1 = 0;
    for (int i = 0; i < B; i++) e0 += (double)o[i] * o[i];
    for (int i = 3 * B; i < 4 * B; i++) e1 += (double)o[i] * o[i];
    CHECK(e0 > 0.25 * e1 && e1 > 0, "first block %.0f vs fourth %.0f: a slow start", sqrt(e0 / B), sqrt(e1 / B));
    printf("ok onset (first block rms %.0f, fourth %.0f)\n", sqrt(e0 / B), sqrt(e1 / B));
}

static void wav(const char *fn, const int16_t *x, int n)
{
    FILE *f = fopen(fn, "wb");
    if (!f) return;
    uint32_t d = (uint32_t)n * 2, sz = 36 + d, sr = DD_SR, br = DD_SR * 2, sixteen = 16;
    uint16_t one = 1, ba = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&sz, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&sixteen, 4, 1, f);
    fwrite(&one, 2, 1, f); fwrite(&one, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&d, 4, 1, f); fwrite(x, 2, (size_t)n, f); fclose(f);
}

static void renders(void)
{
    static int16_t o[60 * DD_SR];
    static const struct { const char *name; dd_par p; uint8_t k[3]; int nk; } pre[] = {
        { "wall",     { 2000, 30000, 26000, 9000, 6000, 8000, 0, 30000, 30000 }, { 40 }, 1 },
        { "howl",     { 0, 31000, 12000, 4000, 12000, 0, 0, 28000, 26000 }, { 57, 60, 64 }, 3 },
        { "junk",     { 14000, 22000, 30000, 20000, 18000, 20000, 6000, 30000, 30000 }, { 45 }, 1 },
        { "metal",    { 32767, 26000, 20000, 14000, 26000, 4000, 0, 30000, 30000 }, { 62, 69 }, 2 },
        { "tear",     { 22000, 32767, 32767, 32767, 10000, 32767, 20000, 32767, 32767 }, { 36 }, 1 },
    };
    char fn[64];
    for (unsigned i = 0; i < sizeof pre / sizeof *pre; i++) {
        dd_init(&D, 11 + i); notes(pre[i].k, pre[i].nk);
        run(&pre[i].p, o, 8 * DD_SR);
        snprintf(fn, sizeof fn, "out/dddd_%s.wav", pre[i].name); wav(fn, o, 8 * DD_SR);
    }
    /* 60 s performance: knobs drift and jump, chords change, the envelope swells and cuts */
    dd_init(&D, 99);
    dd_par p = { 0, 20000, 15000, 8000, 8000, 6000, 0, 30000, 30000 };
    uint32_t r = 1;
    for (int t = 0; t < 60 * DD_SR; t += B) {
        int s = t / DD_SR;
        if (t % (DD_SR * 2) == 0) {                                     /* every 2 s a new key or chord */
            r = r * 1103515245u + 12345u;
            uint8_t k[3] = { (uint8_t)(30 + (r >> 16) % 40), (uint8_t)(35 + (r >> 20) % 40), (uint8_t)(40 + (r >> 24) % 40) };
            notes(k, 1 + (int)((r >> 8) % 3));
        }
        p.source = (int32_t)(16384 + 16000 * sin(t * 2 * M_PI / (17.0 * DD_SR)));
        p.feedback = (int32_t)(20000 + 12000 * sin(t * 2 * M_PI / (23.0 * DD_SR)));
        p.destroy = s < 10 ? s * 3000 : (int32_t)(26000 + 6000 * sin(t * 2 * M_PI / (7.0 * DD_SR)));
        p.hands = s > 40 ? 30000 : s * 600;
        p.filter = (int32_t)(16384 + 16000 * sin(t * 2 * M_PI / (11.0 * DD_SR)));
        p.env = (t / (DD_SR / 2)) % 5 == 4 ? 4000 : 32767;               /* cuts */
        dd_render(&D, &p, o + t, 60 * DD_SR - t < B ? 60 * DD_SR - t : B);
    }
    wav("out/dddd_performance.wav", o, 60 * DD_SR);
    printf("-> out/dddd_{wall,howl,junk,metal,tear,performance}.wav\n");
}

int main(int argc, char **argv)
{
    (void)argv;
    t_bounds(); t_pitch(); t_para(); t_onset();
    if (argc > 1) renders();
    printf(fails ? "%d FAILED\n" : "dddd ok\n", fails);
    return fails != 0;
}
