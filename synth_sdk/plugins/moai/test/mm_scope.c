/* Scope steadiness: plays one note per scenario, runs the GUI's scope every 4..8 blocks (the frame rate against the
 * audio, with jitter) and measures how much the drawn trace moves from one frame to the next, in pixels (mean over
 * the 65 points). The same ring is also drawn the v3.1 way (first rising zero crossing, peak of this frame) as the
 * reference. A steady wave should barely move; detune and drift make it morph slowly, they should not make it jump.
 *   mm_scope    (fails if the new trace is not steadier than v3.1's, or moves more than 2 px on a plain note) */
#include "../../../op1_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24] __attribute__((aligned(4)));
int16_t host_voice_vel[6]; int16_t host_accel[3]; int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];
int host_shift; int host_set_knobs_calls;
void host_set_knobs(int slot, const int16_t *w) { (void)slot; (void)w; }
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return (void *)1; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
void *host_raster(void) { static int r; return &r; }
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c) { (void)r; (void)p0; (void)p1; (void)w; (void)c; }
void *read_fx_param_safe(int slot, int pos) { (void)slot; (void)pos; return 0; }
#define MM_HOST_UI
#include "../moai.c"
#include "../moai_ui.c"

#define H 43                                    /* the scope's half height in pixels (view_osc) */
static int16_t ring[1024]; static int ring_w;   /* full rate, as v3.1 kept it */
static void play(const int16_t *w, int blocks)
{
    static int16_t out[OP1_BLOCK];
    while (blocks--) {
        ENGINE_moai.render(host_voice_mem, out, w);
        for (int i = 0; i < OP1_BLOCK; i++) ring[(ring_w + i) & 1023] = out[i];
        ring_w = (ring_w + OP1_BLOCK) & 1023;
    }
}
static void old_trace(double *y)                /* v3.1: first rising zero crossing, integer samples, this frame's peak */
{
    const mm_voice_t *v = &V[mm_last];
    int32_t per = (int32_t)(0xFFFFFFFFu / v->key_inc);
    if (per < 4) per = 4;
    if (per > 360) per = 360;
    int32_t n = 2 * per, start = ring_w - n - per;
    for (int32_t i = 0; i < per; i++) {
        int16_t a = ring[(start + i - 1) & 1023], b = ring[(start + i) & 1023];
        if (a < 0 && b >= 0) { start += i; break; }
    }
    int32_t pk = 2048;
    for (int32_t i = 0; i < n; i++) { int32_t a = abs(ring[(start + i) & 1023]); if (a > pk) pk = a; }
    for (int j = 0; j <= SC_PTS; j++) y[j] = (double)ring[(start + j * n / SC_PTS) & 1023] * H / pk;
}

int main(void)
{
    static const struct { const char *name; int16_t k[8]; int32_t inc; double limit; } S[] = {
        /* k: setup, cutoff, emphasis, contour knob, selector, detune, drive, drift */
        { "CLASSIC, plain",          { 512, 15000, 9000, 12000, 0x5301, 0, 0, 0 },          25480370, 2 },
        { "CLASSIC, detune + drift", { 512, 15000, 9000, 12000, 0x5301, 8000, 0, 16000 },   25480370, 0 },
        { "SHARK",                   { 22016, 15000, 9000, 12000, 0x5301, 0, 0, 0 },        25480370, 0 },
        { "OBOE (pulses)",           { 16896, 15000, 9000, 12000, 0x5301, 0, 0, 0 },        25480370, 0 },
        { "BREATH (noise)",          { 30208, 15000, 9000, 12000, 0x5301, 0, 0, 0 },        25480370, 0 },
        { "resonant, open",          { 512, 26000, 29000, 0, 0x5301, 0, 0, 0 },             25480370, 0 },
        { "MOOG BS (32'), low note", { 1536, 15000, 9000, 12000, 0x5301, 0, 0, 0 },         25480370 / 4, 0 },
        { "high note (fast beating)", { 512, 30000, 9000, 0, 0x5301, 0, 0, 0 },             203842960, 0 },
    };
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    int fails = 0;
    srand(1);
    mg_screen_init(0);
    for (unsigned s = 0; s < sizeof S / sizeof S[0]; s++) {
        for (int i = 0; i < 128; i++) host_pitch_inc[i] = S[s].inc;
        host_voice_vel[0] = 100 << 8;
        memcpy(host_knobs[0], S[s].k, sizeof S[s].k);
        const int16_t *w = host_knobs[0];
        ENGINE_moai.note_on(host_voice_mem, w);
        play(w, 40);                                                                     /* past the attack */
        sc_per = 0;
        for (int f = 0; f < 30; f++) { play(w, 6); mg_screen_update(0, 16); mg_scope(host_raster(), 182, 4, 314, 96, 0); }  /* settle the decimation */
        double po[SC_PTS + 1], pn[SC_PTS + 1], mo = 0, mn = 0;
        int frames = 120;
        for (int f = 0; f <= frames; f++) {
            play(w, 4 + rand() % 5);
            double yo[SC_PTS + 1], yn[SC_PTS + 1];
            old_trace(yo);
            mg_scope(host_raster(), 182, 4, 314, 96, 0);
            for (int j = 0; j <= SC_PTS; j++) yn[j] = (double)sc_tr[j] * H / sc_pk;
            if (f) for (int j = 0; j <= SC_PTS; j++) { mo += fabs(yo[j] - po[j]); mn += fabs(yn[j] - pn[j]); }
            memcpy(po, yo, sizeof po); memcpy(pn, yn, sizeof pn);
        }
        mo /= frames * (SC_PTS + 1); mn /= frames * (SC_PTS + 1);
        int bad = mn >= mo || (S[s].limit > 0 && mn > S[s].limit);
        printf("%s %-26s moves %5.2f px per frame (v3.1: %5.2f)\n", bad ? "FAIL" : "ok  ", S[s].name, mn, mo);
        fails += bad;
    }
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("scope ok\n");
    return 0;
}
