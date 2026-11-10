/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* REACTOR Mac model demos (packet 1): renders reactor.c's engine on the host into WAVs for listening. Since 2026-11-03
 * it runs in R246's units (128 samples at 88.2 kHz a call; the first renders were made at 44.1 kHz, which is the sound
 * the engine's constants were re-derived to keep). SOURCE ONLY: a
 * plain linear ADSR stands in for the OP-1's envelope; no LFO, no FX, no native mix (voices summed, -6 dB). Knobs can move per block.
 *   cc -O2 -DOP1_HOST -I ../.. reactor_demo.c -lm -o /tmp/reactor_demo && /tmp/reactor_demo <out dir>
 * Prints per demo: peak, RMS, clipped samples, when a voice first went critical and the critical share.
 *   .../reactor_demo --level k0 k1 k2 k3 [note]   one held note's level (presets.py's level pass) */
#include "reactor.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_knobs[8][8]; int host_slot;
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }   /* the screen isn't drawn here */
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
void *host_raster(void) { return 0; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c) { (void)r; (void)a; (void)b; (void)w; (void)c; }

#define SR 88200                                  /* R246's voice rate: the model now runs as the unit does (WAVs at 88.2 kHz, before the OP-1's 2:1 decimation) */
typedef struct { int note; double on, off; int vel; } note_ev;
typedef struct {
    const char *name, *desc;
    int k[4];                                        /* cooling, radiation, rods, heat */
    double a, d, s, r;                               /* seconds, sustain 0..1 */
    double len;
    void (*automate)(double t, int16_t *k);          /* or 0 */
    note_ev ev[24]; int nev;
} demo_t;

static double ramp(double t, double t0, double t1) { return t <= t0 ? 0 : t >= t1 ? 1 : (t - t0) / (t1 - t0); }
static void heat_sweep(double t, int16_t *k)   { k[3] = (int16_t)(32767 * ramp(t, 1, 11)); }
static void cool_rescue(double t, int16_t *k)  { k[0] = (int16_t)(32767 * ramp(t, 4, 9)); }
static void rods_sweep(double t, int16_t *k)   { k[2] = (int16_t)(32767 * ramp(t, 0.5, 8.5)); }
static void rad_sweep(double t, int16_t *k)    { k[1] = (int16_t)(32767 * ramp(t, 0.5, 9)); }

#define CHORD3(n, a, b, c, t0, t1, v) {a,t0,t1,v},{b,t0,t1,v},{c,t0,t1,v}
static demo_t demos[] = {
    { "01-heat-sweep", "C major chord held; HEAT 0 to full over 10 s (cooling half, rods 60 %). Listen for the change into critical.",
      { 16384, 4000, 20000, 0 }, 0.01, 0.1, 1.0, 0.8, 13, heat_sweep, { CHORD3(0, 48, 52, 55, 0.0, 12.0, 110) }, 3 },
    { "02-cooling-rescue", "Hot chord with no cooling goes critical; COOLING rises from 4 s to 9 s and brings it back.",
      { 0, 6000, 22000, 22000 }, 0.01, 0.1, 1.0, 0.8, 11.5, cool_rescue, { CHORD3(0, 45, 52, 57, 0.0, 10.5, 110) }, 3 },
    { "03-rods-sweep", "One note; RODS from all in (a sine) to all out over 8 s, low heat.",
      { 20000, 0, 0, 8000 }, 0.01, 0.1, 1.0, 0.5, 9.5, rods_sweep, { { 48, 0.0, 9.0, 110 } }, 1 },
    { "04-hold-creep", "Heat just under critical: the held note slowly heats up and trips on its own after a few seconds.",
      { 16384, 3000, 16384, 21600 }, 0.01, 0.1, 1.0, 0.8, 11, 0, { { 48, 0.0, 10.0, 127 } }, 1 },
    { "05-radiation-sweep", "Short phrase looped; RADIATION 0 to full: loop noise and Geiger clicks.",
      { 18000, 0, 14000, 9000 }, 0.005, 0.25, 0.5, 0.25, 10, rad_sweep,
      { {48,0,0.4,120},{55,0.5,0.9,90},{60,1,1.4,120},{58,1.5,1.9,90},{48,2,2.4,120},{55,2.5,2.9,90},{63,3,3.8,120},
        {60,4,4.4,120},{55,4.5,4.9,90},{48,5,5.4,120},{51,5.5,5.9,90},{55,6,6.8,120},{48,7,7.4,120},{55,7.5,7.9,90},
        {60,8,9.0,120} }, 15 },
    { "06-china-syndrome", "Preset China Syndrome: a held bass note with no cooling. Critical at about 0.7 s, then the octave-down growl and the pitch sinking; it spins back up in the release.",
      { 2500, 4000, 32767, 16000 }, 0.002, 0.15, 0.8, 0.7, 8, 0, { {36,0,6,120} }, 1 },
    { "07-ignition", "Preset Ignition: keys that stay cool. Soft notes (40) answered by hard ones (127): velocity drives the attack.",
      { 20000, 1500, 18000, 9000 }, 0.002, 0.25, 0.43, 0.12, 7, 0,
      { {60,0,0.4,40},{60,0.5,0.9,127},{64,1,1.4,40},{64,1.5,1.9,127},{67,2,2.4,40},{67,2.5,2.9,127},{72,3,3.6,40},{72,3.75,4.6,127},
        {48,5,6.5,40},{55,5,6.5,127} }, 10 },
    { "08-coolant-leak", "Preset idea Coolant Leak: low cooling, medium heat. Short notes stay clean, long notes go critical.",
      { 6000, 3000, 14000, 13000 }, 0.01, 0.2, 0.8, 0.6, 12, 0,
      { {48,0,0.4,110},{51,0.5,0.9,110},{55,1,1.4,110},{53,1.5,6.0,120},{48,6.5,6.9,110},{46,7,7.4,110},{48,7.5,11.0,120} }, 7 },
    { "09-geiger-lead", "Preset idea Geiger Lead: high radiation, clicks and grit on a mono lead.",
      { 18000, 26000, 12000, 10000 }, 0.005, 0.3, 0.7, 0.2, 7.5, 0,
      { {62,0,0.5,120},{65,0.5,0.75,100},{67,0.75,1.5,120},{70,1.5,1.75,100},{69,1.75,2.5,120},{65,2.5,3.5,110},
        {62,3.75,4.25,120},{60,4.25,4.5,100},{62,4.5,6.5,127} }, 9 },
    { "10-half-life-pad", "Preset idea Half-Life Pad: slow pad with heat just under critical; it trips while held and calms in the release.",
      { 12000, 5000, 24000, 16000 }, 1.5, 0.5, 0.9, 2.5, 13, 0,
      { {45,0,6,80},{52,0,6,80},{57,0,6,80},{60,0,6,80},{41,6.2,10,80},{48,6.2,10,80},{57,6.2,10,80},{64,6.2,10,80} }, 8 },
    { "11-core-bass", "Preset idea Core Bass: all rods out, short envelope, drive.",
      { 14000, 2000, 32767, 12000 }, 0.002, 0.25, 0.5, 0.1, 8, 0,
      { {36,0,0.2,127},{36,0.25,0.4,90},{48,0.5,0.7,110},{36,0.75,0.9,90},{39,1,1.2,127},{39,1.25,1.4,90},{51,1.5,1.7,110},
        {41,1.75,1.9,90},{36,2,2.2,127},{36,2.25,2.4,90},{48,2.5,2.7,110},{36,2.75,2.9,90},{43,3,3.2,127},{43,3.25,3.4,90},
        {46,3.5,3.7,110},{34,3.75,3.9,90},{36,4,4.2,127},{36,4.25,4.4,90},{48,4.5,4.7,110},{36,4.75,4.9,90},{39,5,5.2,127},
        {43,5.5,6.8,120} }, 22 },
    { "12-meltdown", "Preset Meltdown: no cooling, everything else full. A hot overdriven drone that growls an octave down and sags.",
      { 0, 20000, 32767, 32767 }, 0.3, 0.5, 0.8, 1.5, 9, 0, { {36,0,7,90},{43,0,7,90},{48,0,7,90} }, 3 },
    { "13-breeder", "Preset Breeder: a pad chord on the edge. The rods breathe faster as it heats, and it trips after about 4 s.",
      { 9000, 3000, 20000, 13500 }, 0.9, 0.5, 0.9, 1.4, 12, 0,
      { {45,0,9,90},{52,0,9,90},{57,0,9,90},{60,0,9,90} }, 4 },
    { "14-turbine", "Preset Turbine: a lead that trips in about 0.6 s on long notes and dives in pitch; short notes stay in tune.",
      { 6000, 5000, 28000, 20000 }, 0.007, 0.16, 0.8, 0.14, 8, 0,
      { {67,0,0.3,110},{70,0.35,0.6,100},{72,0.65,2.4,124},{75,2.5,2.75,100},{74,2.75,3.0,100},{72,3.05,3.4,110},{70,3.45,3.7,90},
        {67,3.75,7.0,127} }, 8 },
};

static void put32(FILE *f, uint32_t x) { fputc(x & 255, f); fputc(x >> 8 & 255, f); fputc(x >> 16 & 255, f); fputc(x >> 24, f); }
static void put16(FILE *f, uint16_t x) { fputc(x & 255, f); fputc(x >> 8, f); }
static int32_t inc_of(int midi) { return (int32_t)lrint(4294967296.0 * 440.0 * pow(2, (midi - 69) / 12.0) / SR); }

static void run(const demo_t *d, const char *dir)
{
    const int nblk = (int)(d->len * SR / OP1_BLOCK), n = nblk * OP1_BLOCK;
    int16_t *pcm = calloc((size_t)n, 2);
    int vo[24], started[24] = { 0 }, released[24] = { 0 };
    double env[24] = { 0 };
    int owner[6]; for (int i = 0; i < 6; i++) owner[i] = -1;
    for (int e = 0; e < d->nev; e++) vo[e] = -1;
    double first_crit = -1; long crit_blocks = 0, voice_blocks = 0, clipped = 0; int peak = 0; double ss = 0;
    for (int b = 0; b < nblk; b++) {
        const double t = (double)b * OP1_BLOCK / SR, dt = 1.0 / SR;
        int16_t k[8] = { (int16_t)d->k[0], (int16_t)d->k[1], (int16_t)d->k[2], (int16_t)d->k[3], 0x5300 + 11, 0, 0, 0 };
        if (d->automate) d->automate(t, k);
        int32_t mix[OP1_BLOCK] = { 0 };
        for (int e = 0; e < d->nev; e++) {
            const note_ev *ev = &d->ev[e];
            if (!started[e] && t >= ev->on) {                                 /* voice: oldest free, else steal */
                int v = -1;
                for (int i = 0; i < 6 && v < 0; i++) if (owner[i] < 0) v = i;
                if (v < 0) { v = 0; vo[owner[0]] = -1; }
                owner[v] = e; vo[e] = v; started[e] = 1;
                host_voice_vel[v] = (int16_t)(ev->vel << 8);
                ENGINE_reactor.note_on(OP1_VOICE(v), k);
            }
            const int v = vo[e];
            if (v < 0) continue;
            if (!released[e] && t >= ev->off) { released[e] = 1; ENGINE_reactor.note_off(OP1_VOICE(v)); }
            for (int i = 0; i < OP1_BLOCK; i++) host_pitch_inc[i] = inc_of(ev->note);
            int16_t out[OP1_BLOCK];
            ENGINE_reactor.render(OP1_VOICE(v), out, k);
            voice_blocks++;
            if (voices[v].crit) { crit_blocks++; if (first_crit < 0) first_crit = t; }
            for (int i = 0; i < OP1_BLOCK; i++) {
                const double ts = t + i * dt - ev->on;
                double target;
                if (!released[e]) target = ts < d->a ? ts / d->a : (ts < d->a + d->d ? 1 - (1 - d->s) * (ts - d->a) / d->d : d->s);
                else target = 0;
                if (!released[e]) env[e] = target;
                else env[e] -= dt / d->r * (d->s > 0.05 ? d->s : 0.05);
                if (env[e] < 0) env[e] = 0;
                mix[i] += (int32_t)lrint(out[i] * env[e]);
            }
            if (released[e] && env[e] <= 0) { owner[v] = -1; vo[e] = -1; }
        }
        for (int i = 0; i < OP1_BLOCK; i++) {
            int32_t x = mix[i] >> 1;                                        /* -6 dB bus headroom */
            if (x > 32767) { x = 32767; clipped++; } else if (x < -32768) { x = -32768; clipped++; }
            pcm[b * OP1_BLOCK + i] = (int16_t)x;
            if (abs(x) > peak) peak = abs(x);
            ss += (double)x * x;
        }
    }
    char path[1024]; snprintf(path, sizeof path, "%s/%s.wav", dir, d->name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite("RIFF", 1, 4, f); put32(f, 36 + (uint32_t)n * 2); fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1);
    put16(f, 1); put32(f, SR); put32(f, SR * 2); put16(f, 2); put16(f, 16); fwrite("data", 1, 4, f); put32(f, (uint32_t)n * 2);
    fwrite(pcm, 2, (size_t)n, f); fclose(f); free(pcm);
    printf("%-20s peak %6.1f dBFS  rms %6.1f dBFS  clipped %5ld  first critical %s%5.2f s  critical %3.0f %%\n", d->name,
           20 * log10((peak + 1) / 32768.0), 10 * log10(ss / n / (32768.0 * 32768.0) + 1e-12), clipped,
           first_crit < 0 ? "never " : "", first_crit < 0 ? 0 : first_crit,
           voice_blocks ? 100.0 * crit_blocks / voice_blocks : 0);
}

/* --level k0 k1 k2 k3 [note]: one voice held 3 s at velocity 100, no envelope; prints the RMS of 0.5 .. 3 s in dBFS
 * (presets.py's level pass: the preset trim, word 7, evens these out) */
static int level(int argc, char **argv)
{
    int16_t k[8] = { 0, 0, 0, 0, 0x5300 + 11, 0, 0, 0 };
    for (int i = 0; i < 4 && i + 2 < argc; i++) k[i] = (int16_t)atoi(argv[i + 2]);
    const int note = argc > 6 ? atoi(argv[6]) : 48;
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = inc_of(note);
    host_voice_vel[0] = 100 << 8;
    ENGINE_reactor.note_on(OP1_VOICE(0), k);
    double ss = 0; long n = 0; int16_t out[OP1_BLOCK];
    for (int b = 0; b < 3 * SR / OP1_BLOCK; b++) {
        ENGINE_reactor.render(OP1_VOICE(0), out, k);
        if (b * OP1_BLOCK >= SR / 2) for (int i = 0; i < OP1_BLOCK; i++, n++) ss += (double)out[i] * out[i];
    }
    printf("%.2f\n", 10 * log10(ss / n / (32768.0 * 32768.0) + 1e-12));
    return 0;
}

int main(int argc, char **argv)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    if (argc > 1 && !strcmp(argv[1], "--level")) return level(argc, argv);
    const char *dir = argc > 1 ? argv[1] : ".";
    FILE *idx = 0;
    char p[1024]; snprintf(p, sizeof p, "%s/demos.tsv", dir);
    idx = fopen(p, "w");
    for (size_t i = 0; i < sizeof demos / sizeof demos[0]; i++) {
        memset(voices, 0, sizeof voices);
        run(&demos[i], dir);
        if (idx) fprintf(idx, "%s\t%s\n", demos[i].name, demos[i].desc);
    }
    if (idx) fclose(idx);
    return 0;
}
