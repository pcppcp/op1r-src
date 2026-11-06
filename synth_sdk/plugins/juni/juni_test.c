/* JUNI host test: six voices with a simulated T2 envelope, WAV demos of each macro and checks (levels, resonance
 * bounds, clipping). Source-only audio: no stock voice allocator, decimator or FX.
 *   cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -DOP1_HOST -I../.. \
 *      juni_test.c juni.c -lm -o /tmp/juni_test && /tmp/juni_test <out dir>
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include "op1_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_voice_env[128];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];
void *host_raster(void) { return 0; }                      /* the screen's calls (juni.c holds the screen too) */
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c) { (void)r; (void)a; (void)b; (void)w; (void)c; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
extern const op1_engine_t ENGINE_juni;

/* a T2-like ADSR per voice, Q15 per block (2.9 ms) */
typedef struct { int on, note, stage; double lvl; int vel; } tv_t;
static tv_t tv[6];
static double A = 0.008, D = 0.4, S = 0.65, R = 0.45;
static int16_t k[8];

static int32_t inc_of(int midi) { return (int32_t)lrint(4294967296.0 * 440.0 * pow(2, (midi - 69) / 12.0) / 44100); }

static void on(int v, int note, int vel)
{
    tv[v].on = 1; tv[v].note = note; tv[v].stage = 1; tv[v].vel = vel;
    host_voice_vel[v] = (int16_t)(vel << 8);
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = inc_of(note);
    ENGINE_juni.note_on(OP1_VOICE(v), k);
}
static void off(int v) { tv[v].stage = 4; }

/* one block of all sounding voices, summed (each voice's output times its envelope, ramped as the stock one) */
static double peak; static int clips;
static void block(float *mix)
{
    int16_t out[OP1_BLOCK];
    for (int i = 0; i < OP1_BLOCK; i++) mix[i] = 0;
    for (int v = 0; v < 6; v++) {
        tv_t *t = &tv[v];
        if (!t->stage) continue;
        double l0 = t->lvl, dt = OP1_BLOCK / 44100.0;
        if (t->stage == 1) { t->lvl += dt / A; if (t->lvl >= 1) { t->lvl = 1; t->stage = 2; } }
        else if (t->stage == 2) { t->lvl -= (t->lvl - S) * dt / (D / 4); if (t->lvl - S < 1e-3) t->stage = 3; }
        else if (t->stage == 4) { t->lvl -= t->lvl * dt / (R / 4); if (t->lvl < 1e-4) { t->lvl = 0; t->stage = 0; } }
        for (int i = 0; i < 128; i++) { host_voice_env[i] = (int16_t)(t->lvl * 32767); host_pitch_inc[i] = inc_of(t->note); }
        ENGINE_juni.render(OP1_VOICE(v), out, k);
        for (int i = 0; i < OP1_BLOCK; i++) {
            if (abs(out[i]) >= 32767) clips++;
            double y = out[i] * (l0 + (t->lvl - l0) * i / OP1_BLOCK);   /* the peak after the envelope: what is heard */
            if (fabs(y) > peak) peak = fabs(y);
            mix[i] += (float)y;
        }
    }
}

typedef struct { FILE *f; long n; } wav_t;
static wav_t wopen(const char *dir, const char *name)
{
    char p[512]; wav_t w; snprintf(p, sizeof p, "%s/%s", dir, name);
    w.f = fopen(p, "wb"); w.n = 0; if (!w.f) { perror(p); exit(2); }
    fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\x44\xac\0\0\x88\x58\x01\0\x02\0\x10\0data\0\0\0\0", 1, 44, w.f);
    return w;
}
static void wput(wav_t *w, const float *x, double gain)
{
    for (int i = 0; i < OP1_BLOCK; i++) {
        double y = x[i] * gain; int16_t s = (int16_t)(y > 32767 ? 32767 : y < -32767 ? -32767 : y);
        fwrite(&s, 2, 1, w->f); w->n++;
    }
}
static void wclose(wav_t *w)
{
    uint32_t d = (uint32_t)(w->n * 2), r = d + 36;
    fseek(w->f, 4, SEEK_SET); fwrite(&r, 4, 1, w->f); fseek(w->f, 40, SEEK_SET); fwrite(&d, 4, 1, w->f); fclose(w->f);
}
static void set_k(int a, int b, int c, int d, int p5, int p6, int p7)
{ k[0] = (int16_t)a; k[1] = (int16_t)b; k[2] = (int16_t)c; k[3] = (int16_t)d; k[4] = 0x5301; k[5] = (int16_t)p5; k[6] = (int16_t)p6; k[7] = (int16_t)p7; }
static void all_off(void) { for (int v = 0; v < 6; v++) off(v); }
static void run(wav_t *w, double sec, float *mix, double gain)
{ for (long b = 0; b < (long)(sec * 44100 / OP1_BLOCK); b++) { block(mix); if (w) wput(w, mix, gain); } }

static const int CH[4][4] = { {48, 55, 60, 64}, {45, 52, 57, 60}, {41, 48, 53, 57}, {43, 50, 55, 59} };   /* C Am F G */
#define MIXG 0.35                                /* the demo mix: six voices into one WAV */

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    float mix[OP1_BLOCK];
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));

    /* 1. the default patch: a chord progression */
    set_k(18000, 16384, 11000, 9800, 0, 0, 0);
    wav_t w = wopen(dir, "juni_default_chords.wav");
    for (int c = 0; c < 4; c++) { for (int v = 0; v < 4; v++) on(v, CH[c][v], 100); run(&w, 1.6, mix, MIXG); all_off(); run(&w, 0.25, mix, MIXG); }
    run(&w, 1.0, mix, MIXG); wclose(&w);

    /* 2. each macro swept over a held chord (8 s, 0 -> max) */
    static const char *names[4] = { "tone", "shape", "sweep", "chorus" };
    for (int m = 0; m < 4; m++) {
        char n[64]; snprintf(n, sizeof n, "juni_sweep_%s.wav", names[m]);
        set_k(18000, 16384, 11000, 9800, 0, 0, 0);
        w = wopen(dir, n);
        long nb = (long)(8.0 * 44100 / OP1_BLOCK);
        for (int v = 0; v < 4; v++) on(v, CH[0][v], 100);
        for (long b = 0; b < nb; b++) { k[m] = (int16_t)(32767 * b / nb); if (m == 2 && b % 120 == 0) { all_off(); for (int v = 0; v < 4; v++) on(v, CH[0][v], 100); } block(mix); wput(&w, mix, MIXG); }
        all_off(); run(&w, 0.8, mix, MIXG); wclose(&w);
    }

    /* 3. plucks: SWEEP high, a short envelope, a bass line */
    set_k(9000, 4000, 26000, 0, 6000, 0, 0); A = 0.002; D = 0.25; S = 0.0; R = 0.15;
    w = wopen(dir, "juni_bass_pluck.wav");
    static const int BL[8] = { 36, 36, 48, 36, 39, 41, 43, 46 };
    for (int r = 0; r < 2; r++) for (int i = 0; i < 8; i++) { on(i & 1, BL[i], i % 4 ? 90 : 127); run(&w, 0.2, mix, 0.8); off(i & 1); run(&w, 0.05, mix, 0.8); }
    run(&w, 0.6, mix, 0.8); wclose(&w);

    /* 4. PWM strings with chorus II, slow attack */
    set_k(20000, 24576, 4000, 20000, 0, 0, 0); A = 0.5; D = 1.0; S = 0.85; R = 1.2;
    w = wopen(dir, "juni_strings.wav");
    for (int c = 0; c < 4; c++) { for (int v = 0; v < 4; v++) on(v, CH[c][v] + 12, 100); run(&w, 2.0, mix, MIXG); all_off(); }
    run(&w, 1.5, mix, MIXG); wclose(&w);

    /* probes: one held A3, filter open, chorus off / on (noise floor and aliasing between the harmonics) */
    A = 0.008; D = 0.4; S = 1.0; R = 0.05;
    for (int c = 0; c < 2; c++) {
        set_k(32767, 8192, 0, c ? 20000 : 0, 0, 0, 0);
        w = wopen(dir, c ? "juni_probe_chorus.wav" : "juni_probe_dry.wav");
        on(0, 57, 127); run(&w, 2.0, mix, 1.0); off(0); run(&w, 0.3, mix, 1.0); wclose(&w);
    }
    static const int CHV[4] = { 0, 10000, 20000, 32767 };       /* off, I, II, I+II: one saw note each */
    for (int c = 0; c < 4; c++) {
        char n[64]; snprintf(n, sizeof n, "juni_probe_ch%d.wav", c);
        set_k(32767, 8192, 0, CHV[c], 0, 0, 0);
        w = wopen(dir, n); on(0, 57, 127); run(&w, 4.0, mix, 1.0); off(0); run(&w, 0.3, mix, 1.0); wclose(&w);
    }

    set_k(3000, 8192, 0, 0, 0, 0, 0);
    w = wopen(dir, "juni_probe_lowcut.wav"); on(0, 57, 127); run(&w, 2.0, mix, 1.0); off(0); run(&w, 0.3, mix, 1.0); wclose(&w);
    set_k(0, 8192, 0, 0, 32767, 0, 0);
    w = wopen(dir, "juni_probe_lowres.wav"); on(0, 36, 127); run(&w, 1.5, mix, 1.0); off(0); run(&w, 0.5, mix, 1.0); wclose(&w);

    /* checks: level of each SHAPE point (one voice, sustain, filter open) */
    A = 0.008; D = 0.4; S = 1.0; R = 0.45;
    printf("SHAPE points, one voice C3, TONE max, RMS dBFS:");
    for (int p = 0; p <= 4; p++) {
        double s2 = 0; long n = 0;
        set_k(32767, p * 8192 > 32767 ? 32767 : p * 8192, 0, 0, 0, 0, 0);
        on(0, 48, 127);
        for (int b = 0; b < 400; b++) { block(mix); if (b > 100) for (int i = 0; i < OP1_BLOCK; i++) { s2 += mix[i] * mix[i]; n++; } }
        off(0); run(0, 1.0, mix, 1);
        printf(" %.1f", 10 * log10(s2 / n / (32768.0 * 32768.0)));
    }
    printf("\n");

    /* checks: the chorus keeps the level (a held 'juni pad' chord, C3 chord, at CHORUS 0 / I / II / I+II) */
    printf("CHORUS level, held pad chord, RMS dBFS:");
    for (int c = 0; c < 4; c++) {
        static const int CV[4] = { 0, 8000, 20000, 32767 };
        double s2 = 0; long n = 0;
        set_k(17000, 24576, 6000, CV[c], 0, 0, 0);
        for (int v = 0; v < 4; v++) on(v, CH[0][v], 100);
        for (int b = 0; b < 700; b++) { block(mix); if (b > 200) for (int i = 0; i < OP1_BLOCK; i++) { s2 += mix[i] * mix[i]; n++; } }
        all_off(); run(0, 1.5, mix, 1);
        printf(" %.1f", 10 * log10(s2 / n / (32768.0 * 32768.0)));
    }
    printf("\n");

    /* checks: resonance at the top, every 6 semitones of TONE, notes low and high: bounded, and how loud */
    printf("RESONANCE max, SWEEP 0: peak per TONE step (notes 36 / 84):\n");
    for (int t = 0; t <= 32767; t += 2730) {
        double pk[2] = {0, 0};
        for (int h = 0; h < 2; h++) {
            set_k(t, 8192, 0, 0, 32767, 0, 0);
            peak = 0; on(0, h ? 84 : 36, 127); run(0, 0.6, mix, 1); pk[h] = peak; off(0); run(0, 1.0, mix, 1);
        }
        printf("  tone %5d: %6.0f %6.0f\n", t, pk[0], pk[1]);
    }
    /* checks: everything at max, and everything at 0, still sound and stay bounded */
    set_k(32767, 32767, 32767, 32767, 32767, 32767, 32767); peak = 0; clips = 0;
    for (int v = 0; v < 6; v++) on(v, 36 + 12 * v, 127);
    run(0, 2.0, mix, 1); printf("all max, 6 voices: peak %.0f, clipped samples %d\n", peak, clips); all_off(); run(0, 1.5, mix, 1);
    set_k(0, 0, 0, 0, 0, 0, 0); peak = 0; clips = 0;
    for (int v = 0; v < 6; v++) on(v, 36 + 12 * v, 127);
    run(0, 2.0, mix, 1); printf("all 0, 6 voices: peak %.0f, clipped samples %d\n", peak, clips);
    return 0;
}
