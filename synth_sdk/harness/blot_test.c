/* Host test for plugins/blot: the filter envelope sweeps down, accent sweeps further and louder, the decay knob sets
 * the sweep's speed, the resonance peak sits near the cutoff knob's frequency, and nothing runs away at the extremes.
 * Writes harness/out/blot_line.wav: a 16-step acid line (125 BPM, A1, accents, one slide), 4 bars with the cutoff
 * opening.
 *   mkdir -p build out && cc -O1 -fsanitize=address,undefined -DOP1_HOST -I.. blot_test.c -lm -o build/blot_test \
 *     && build/blot_test */
#include "../plugins/blot/blot.c"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int host_shift; int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6]; int16_t host_voice_env[128]; int16_t host_accel[3];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
uint8_t *host_node_style_rgba(void *s, const char *id) { static uint8_t c[4]; (void)s; (void)id; return c; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
void *host_raster(void) { return 0; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c) { (void)r; (void)a; (void)b; (void)w; (void)c; }
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t s, const uint8_t *c) { (void)x; (void)y; (void)s; (void)c; return o; }
void host_text_set(void *t, const char *s) { (void)t; (void)s; }
void host_text_draw(void *t, void *r, const int32_t *m) { (void)t; (void)r; (void)m; }

#define SR 44100
static int16_t buf[SR * 10];
static int32_t inc_of(double hz) { return (int32_t)lrint(4294967296.0 * hz / SR); }
static void *V = host_voice_mem;

/* n samples of one held note (vel 1..127) into buf from 0; the pitch glides from hz0 to hz1 over the first block */
static void play(const int16_t *k, double hz, int vel, int n, int retrig)
{
    host_voice_vel[0] = (int16_t)(vel << 8);
    if (retrig) ENGINE_blot.note_on(V, k);
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = inc_of(hz);
    for (int b = 0; b < n / 128; b++) ENGINE_blot.render(V, buf + b * 128, k);
}
/* brightness: RMS frequency in Hz, fs / 2 pi * rms(x[n] - x[n-1]) / rms(x) over [a, b) (follows the spectral centroid) */
static double bright(int a, int b)
{
    double d = 0, s = 0;
    for (int i = a + 1; i < b; i++) { double e = (double)buf[i] - buf[i - 1]; d += e * e; s += (double)buf[i] * buf[i]; }
    return s > 0 ? SR / (2 * M_PI) * sqrt(d / s) : 0;
}
static int peak(int a, int b) { int p = 0; for (int i = a; i < b; i++) if (abs(buf[i]) > p) p = abs(buf[i]); return p; }
/* harmonic magnitudes of a 55 Hz note in buf[a, a + n) */
#define NH 290
static void spectrum(int a, int n, double *m)
{
    for (int h = 1; h < NH; h++) {
        double re = 0, im = 0, w = 2 * M_PI * h * 55.0 / SR;
        for (int i = 0; i < n; i++) { re += buf[a + i] * cos(w * i); im += buf[a + i] * sin(w * i); }
        m[h] = sqrt(re * re + im * im) + 1e-9;
    }
}
static void wav(const char *fn, const int16_t *s, int n)
{
    FILE *f = fopen(fn, "wb"); if (!f) return;
    uint32_t b = n * 2, h[] = { 0x46464952, 36 + b, 0x45564157, 0x20746d66, 16, 0x00010001, SR, SR * 2, 0x00100002, 0x61746164, b };
    fwrite(h, 4, 11, f); fwrite(s, 2, n, f); fclose(f);
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    int16_t k[8] = { 9000, 22000, 18000, 9000, 0x5301, 0, 0, 0 };

    /* the envelope sweeps the filter down: bright at the start, darker 300 ms later */
    play(k, 55, 64, SR / 2, 1);
    double b0 = bright(0, 1323), b1 = bright(13230, 14553);
    printf("sweep: brightness %.0f Hz -> %.0f Hz\n", b0, b1);
    assert(b0 > 1.5 * b1);

    /* accent: brighter at the start and louder */
    play(k, 55, 64, SR / 4, 1); double bn = bright(0, 1323); int pn = peak(0, 4410);
    play(k, 55, 127, SR / 4, 1); double ba = bright(0, 1323); int pa = peak(0, 4410);
    printf("accent: brightness %.0f Hz -> %.0f Hz, peak %d -> %d\n", bn, ba, pn, pa);
    assert(ba > 1.2 * bn && pa > pn);

    /* decay knob: short decay is darker 150 ms in than long decay */
    k[3] = 0; play(k, 55, 64, SR / 4, 1); double ds = bright(6615, 7938);
    k[3] = 32767; play(k, 55, 64, SR / 4, 1); double dl = bright(6615, 7938);
    printf("decay: brightness at 150 ms %.0f Hz (short) vs %.0f Hz (long)\n", ds, dl);
    assert(dl > 1.3 * ds);

    /* resonance: the harmonic it lifts most (high vs no resonance, env mod 0) sits near the cutoff knob's frequency */
    k[2] = 0; k[3] = 9000;
    for (int c = 12000; c <= 27000; c += 5000) {
        static double m0[NH], m1[NH];
        k[0] = (int16_t)c;
        k[1] = 0; play(k, 55, 64, SR / 2, 1); spectrum(SR / 4, 8820, m0);
        k[1] = 30000; play(k, 55, 64, SR / 2, 1); spectrum(SR / 4, 8820, m1);
        double fc = 30 * pow(2, c * 9.0 / 32768);
        int hb = 0;                                     /* within two octaves of the cutoff (the stop band is ~0 in both) */
        for (int h = 1; h < NH; h++) if (h * 55.0 > fc / 4 && h * 55.0 < fc * 4 && (!hb || m1[h] / m0[h] > m1[hb] / m0[hb])) hb = h;
        double f = hb * 55.0;
        printf("resonance: cutoff knob %d (%.0f Hz) -> lifts %.0f Hz by %.1f dB\n", c, fc, f, 20 * log10(m1[hb] / m0[hb]));
        assert(f > fc / 1.6 && f < fc * 1.6 && m1[hb] > 2 * m0[hb]);
    }

    /* a voice that another engine used before (its state is garbage, as on the OP-1): no run-away, a normal level */
    {
        int16_t kd[8] = { 9000, 22000, 18000, 9000, 0x5301, 0, 0, 0 };
        for (int t = 0; t < 8; t++) {
            uint32_t r = 12345u + t;
            for (int i = 4; i < 0x24; i++) { r = r * 1664525u + 1013904223u; host_voice_mem[i] = (uint8_t)(r >> 24); }
            play(kd, 261.63, 127, SR / 4, 1);
            int pg = peak(SR / 8, SR / 4);
            assert(pg < 24000);
        }
        printf("garbage voice state: ok\n");
    }

    /* extremes: all knobs and page-2 words at max, notes C0 .. C8 with jumps, accents: bounded, no DC run-away */
    int16_t kx[8] = { 32767, 32767, 32767, 32767, 0x5301, 32767, 32767, 32767 };
    for (int n = 0; n < 40; n++) {
        play(kx, 16.35 * pow(2, (n * 7 % 97) / 12.0), n & 1 ? 127 : 1, SR / 20, n % 3 == 0);
        double m = 0; for (int i = 0; i < SR / 20; i++) m += buf[i];
        assert(fabs(m / (SR / 20)) < 8000);
    }
    printf("extremes: ok\n");

    /* the line: 16 sixteenths at 125 BPM, 4 bars, cutoff opening from 6000 to 20000 */
    static const int pat[16] = { 0, 0, 12, 0, 3, 0, 15, 0, 0, 7, 0, 10, 0, 12, 3, 0 };       /* semitones over A1 */
    static const int acc[16] = { 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 0 };
    static const int sld[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0 };          /* step 14 slides from 13 */
    int16_t kl[8] = { 6000, 26000, 20000, 6000, 0x5301, 0, 20000, 4000 };
    int step = SR * 60 / 125 / 4, pos = 0;
    static int16_t line[SR * 8];
    for (int s = 0; s < 64; s++) {
        kl[0] = (int16_t)(6000 + 14000 * s / 63);
        int i = s & 15;
        double hz = 55 * pow(2, pat[i] / 12.0), from = 55 * pow(2, pat[(i + 15) & 15] / 12.0);
        host_voice_vel[0] = (int16_t)((acc[i] ? 127 : 80) << 8);
        if (!sld[i]) ENGINE_blot.note_on(V, kl);
        for (int b = 0; b < step / 128; b++) {
            double f = sld[i] && b < 8 ? from * pow(hz / from, b / 8.0) : hz;               /* 23 ms glide */
            for (int j = 0; j < 128; j++) host_pitch_inc[j] = inc_of(f);
            ENGINE_blot.render(V, line + pos, kl);
            /* the 303's VCA gate: open for 3/4 of the step (the slide's step stays open), a 3 ms release */
            int open = b < step / 128 * 3 / 4 || sld[(i + 1) & 15];
            for (int j = 0; j < 128 && !open; j++) line[pos + j] = (int16_t)(line[pos + j] * (b == step / 128 * 3 / 4 ? (128 - j) / 128.0 : 0));
            pos += 128;
        }
    }
    wav("out/blot_line.wav", line, pos);
    printf("blot_test: ok -> out/blot_line.wav (%.1f s)\n", pos / (double)SR);
    return 0;
}
