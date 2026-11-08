/* Host test of plugins/aether (docs/36): pitch from forward tilt (range, dead zone, sign), volume from the right side,
 * shake -> vibrato envelope (not for still / noisy / slow tilting), exp2 accuracy, levels.
 *   cc -O1 -Wall -DOP1_HOST -fsanitize=address,undefined -I.. aether_test.c -lm -o build/aether_test */
#include "../op1_plugin.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6]; int16_t host_voice_env[128];
int16_t host_accel[3];
volatile uint8_t op1_eng_page[4];
int16_t host_knobs[8][8]; int host_slot;                                 /* screen stubs (screen not drawn here) */
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
void *host_raster(void) { return 0; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c) { (void)r; (void)a; (void)b; (void)w; (void)c; }
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t z, const uint8_t *c) { (void)x; (void)y; (void)z; (void)c; return o; }
void host_text_set(void *t, const char *s) { (void)t; (void)s; }
void host_text_draw(void *t, void *r, const int32_t *m) { (void)t; (void)r; (void)m; }
#include "../plugins/aether/aether.c"

static int16_t knobs[8] = { 15671, 11469, 16384, 32767, 0x530A, 0, 0, 0 };   /* defaults: 12 semitones */
static uint32_t seed = 1;
static int noise(void) { seed = seed * 1664525u + 1013904223u; return (int)(seed >> 30) - 2; }   /* -2..1 LSB */
static int hold_n;
static void tilt(double fwd_deg, double side_deg, int with_noise)       /* sensor as measured (docs/36) */
{
    if (with_noise && hold_n++ % 10) return;                             /* firmware reads every 640 output samples (10 blocks) */
    double f = fwd_deg * M_PI / 180, s = side_deg * M_PI / 180;
    double v[3] = { sin(s), -sin(f) * cos(s), cos(f) * cos(s) * 0.95 + 0.094 };
    for (int i = 0; i < 3; i++) host_accel[i] = (int16_t)(lrint(v[i] * 16384 / 64 + (with_noise ? noise() : 0)) * 64);
}
static double block(int16_t *out)                                        /* -> phase advance per block (cycles) */
{
    uint32_t p0 = st[0].ph;
    ae_render(host_voice_mem, out, knobs);
    return (double)(uint32_t)(st[0].ph - p0) / 4294967296.0;
}
static double settle(int blocks, int16_t *out) { double c = 0; for (int i = 0; i < blocks; i++) c = block(out); return c; }
static int peak(const int16_t *o) { int p = 0; for (int i = 0; i < 128; i++) if (abs(o[i]) > p) p = abs(o[i]); return p; }

int main(void)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    /* exp2 */
    for (int x = -2 * 65536; x <= 2 * 65536; x += 997) {
        double e = 1200 * log2(exp2_q14(x) / 16384.0) - 1200.0 * x / 65536;
        assert(fabs(e) < 1.0);                                           /* cents */
    }
    double base = 110.0 / OP1_VOICE_RATE * 128;                                   /* cycles per block at A2 (< 1: phase does not wrap) */
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = (int32_t)lrint(110.0 / OP1_VOICE_RATE * 4294967296.0);
    host_voice_vel[0] = 127 << 8;
    int16_t out[128];

    tilt(10, 0, 0); ae_note_on(host_voice_mem, knobs);                   /* home: 10 deg forward */
    double c = settle(400, out);
    assert(fabs(1200 * log2(c / base)) < 1);                             /* home: the key's pitch */
    int pk = 0; for (int i = 0; i < 100; i++) { block(out); if (peak(out) > pk) pk = peak(out); }
    assert(pk > 12000 && pk < 32767);
    tilt(11.5, 0, 0); c = settle(400, out);                              /* inside the 2.5 deg dead zone */
    assert(fabs(1200 * log2(c / base)) < 1);
    /* full range: sin(30 deg) beyond home (in g, as the engine measures) -> +12 semitones */
    double full = asin(sin(10 * M_PI / 180) + 0.5) * 180 / M_PI;
    tilt(full + 1, 0, 0); c = settle(400, out);
    assert(fabs(1200 * log2(c / base) - 1200) < 3);
    tilt(-40, 0, 0); c = settle(400, out);                               /* back: down, clamped at -12 */
    assert(fabs(1200 * log2(c / base) + 1200) < 3);
    tilt(10, 0, 0); settle(400, out);
    /* volume: right side up 30 deg (X -0.5 g) -> silent; left side up: unchanged */
    tilt(10, -31, 0); settle(200, out); assert(peak(out) < 300);
    tilt(10, 25, 0); settle(200, out); assert(peak(out) > 12000);
    tilt(10, 0, 0); settle(1400, out);                                    /* (the steps above were "shakes": decay) */
    /* shake envelope: noise / slow tilt stay low, a 6 Hz hand shake opens it */
    int32_t emax = 0;
    for (int b = 0; b < 2000; b++) { tilt(10, 0, 1); block(out); if (b > 1000 && st[0].env > emax) emax = st[0].env; }
    fprintf(stderr, "noise env %d\n", emax); assert(emax < 3000);
    emax = 0;
    for (int b = 0; b < 2400; b++) {                                    /* 0 -> 35 deg and back in 3.5 s */
        double t = b * 128.0 / OP1_VOICE_RATE;
        tilt(10 + 35 * pow(sin(M_PI * t / 3.5), 2), 0, 1); block(out); if (st[0].env > emax) emax = st[0].env;
    }
    assert(emax < 6000);
    for (int b = 0; b < 1400; b++) {                                     /* sideways 6 Hz shake, 0.15 g */
        double t = b * 128.0 / OP1_VOICE_RATE;
        double s = asin(0.15 * sin(2 * M_PI * 6 * t)) * 180 / M_PI;
        tilt(10, s, 1); block(out);
    }
    assert(st[0].env > 16000);
    printf("aether_test: ok (peak %d, shake env %d)\n", pk, st[0].env);
    return 0;
}
