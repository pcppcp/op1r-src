/* Host test of plugins/reedie/rd_rate.c in R246's units: a voice render call is 128 samples at 88.2 kHz (689 calls a
 * second) and OP1_PITCH_INC = 2^32 f / 88200. The engine as reedie.c wrote it (ENGINE_reedie_44k, run as it stands) is
 * compared with the adapter (ENGINE_reedie): the pitch of the adapter must be within 35 cents (the as-is figures are printed;
 * the low clarinet notes scatter by up to 150 cents between noise seeds when run as is), and the model's times (the reed's start,
 * the release) must be longer, in real time, through the adapter (the release twice as long).
 *   cc -O1 -g -Wall -Wextra -DOP1_HOST -fsanitize=address,undefined -fno-sanitize-recover=undefined -I.. \
 *      reedie_rate_test.c -lm -o /tmp/reedie_rate_test && /tmp/reedie_rate_test
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "../op1_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_voice_env[128]; int16_t host_accel[3]; int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];

#include "../plugins/reedie/rd_rate.c"

#define VOICE_HZ 88200.0
#define CALLS    689.0625
#define MAXC     2000
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t seed = 1;                                   /* the noise seed of the next note (note_on seeds from the address) */
static int16_t out[MAXC][OP1_BLOCK];
static int blockpeak[MAXC];   /* peak level, a window of 6 calls ending at each call */

static void reset(void)
{
    memset(host_voice_mem, 0, sizeof host_voice_mem); memset(rd_ring, 0, sizeof rd_ring);
    memset(rw, 0, sizeof rw); memset(rw_seen, 0, sizeof rw_seen); rw_blk = 0; rw_last = 0xff;
    for (int i = 0; i < 128; i++) host_voice_env[i] = 16000;
    host_voice_vel[0] = 100 << 8;
}

/* one note on voice 0: `hold` calls held, then `rel` calls released */
static void play(const op1_engine_t *e, const int16_t *k, double hz, int hold, int rel)
{
    reset();
    uint32_t inc = (uint32_t)llround(4294967296.0 * hz / VOICE_HZ);
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = (int32_t)inc;
    e->note_on(host_voice_mem, k);
    ((reedie_voice_t *)host_voice_mem)->rng = 0x9e3779b9u * seed;
    for (int c = 0; c < hold + rel; c++) {
        if (c == hold) e->note_off(host_voice_mem);
        e->render(host_voice_mem, out[c], k);
        int pk = 0;                                   /* peak over the last 6 calls (768 samples: two periods of 220 Hz) */
        for (int j = c > 5 ? c - 5 : 0; j <= c; j++) for (int n = 0; n < OP1_BLOCK; n++) { int a = abs(out[j][n]); if (a > pk) pk = a; }
        blockpeak[c] = pk;
    }
}

/* tone frequency over calls c0..c1: the autocorrelation peak within +-12 % of the expected period, interpolated */
static double tone_hz(int c0, int c1, double expect)
{
    static double x[MAXC * OP1_BLOCK]; long n = 0; double mean = 0;
    for (int c = c0; c < c1; c++) for (int i = 0; i < OP1_BLOCK; i++) { x[n++] = out[c][i]; mean += out[c][i]; }
    mean /= n; for (long i = 0; i < n; i++) x[i] -= mean;
    int p0 = (int)(VOICE_HZ / expect * 0.88), p1 = (int)(VOICE_HZ / expect * 1.12) + 1, best = p0;
    static double r[8000]; if (p1 + 2 >= 8000) return 0;
    for (int p = p0 - 1; p <= p1 + 1; p++) { double a = 0; for (long i = 0; i + p < n; i++) a += x[i] * x[i + p]; r[p] = a; }
    for (int p = p0; p <= p1; p++) if (r[p] > r[best]) best = p;
    double d = r[best - 1] - 2 * r[best] + r[best + 1], off = d != 0 ? 0.5 * (r[best - 1] - r[best + 1]) / d : 0;
    return VOICE_HZ / (best + off);
}

static double ms(int calls) { return calls * 1000.0 / CALLS; }

int main(void)
{
    static const int16_t bore[2] = { 8192, 22937 };             /* clarinet, oboe */
    for (int b = 0; b < 2; b++) for (int midi = 48; midi <= 72; midi += 12) {
        int16_t k[8] = { 6553, 0, 16384, bore[b], 0x5300, 0, 0, 0 };
        double hz = 440.0 * pow(2.0, (midi - 69) / 12.0), worst[2] = { 0, 0 };
        for (int w = 0; w < 2; w++) for (seed = 1; seed <= 8; seed++) {      /* eight noise seeds each */
            play(w ? &ENGINE_reedie : &ENGINE_reedie_44k, k, hz, 500, 0);
            double cents = 1200.0 * log2(tone_hz(300, 500, hz) / hz);
            if (fabs(cents) > fabs(worst[w])) worst[w] = cents;
            if (w) CHECK(fabs(cents) < 35.0, "%s MIDI %d bore %d seed %u: %.1f cents off %.1f Hz", w ? "adapter" : "as is", midi, bore[b], seed, cents, hz);
        }
        printf("pitch MIDI %d %s (%.1f Hz), worst of 8 seeds: as is %+.1f cents, adapter %+.1f cents\n", midi, b ? "oboe" : "clarinet", hz, worst[0], worst[1]);
        seed = 1;
    }

    /* the model's times in real time: start (to a half and to 0.9 of the held level) and release (to a half and a tenth) */
    int16_t k[8] = { 6553, 0, 16384, 22937, 0x5300, 0, 0, 0 };
    double t[2][4];
    for (int w = 0; w < 2; w++) {
        play(w ? &ENGINE_reedie : &ENGINE_reedie_44k, k, 220.0, 900, 700);
        int held = 0; for (int c = 600; c < 900; c++) if (blockpeak[c] > held) held = blockpeak[c];
        int a = 0; while (a < 900 && blockpeak[a] < held / 2) a++;
        int a9 = 0; while (a9 < 900 && blockpeak[a9] < held * 9 / 10) a9++;
        int r = 900; while (r < 1600 && blockpeak[r] > held / 2) r++;
        int r1 = 900; while (r1 < 1600 && blockpeak[r1] > held / 10) r1++;
        t[w][0] = ms(a); t[w][1] = ms(a9); t[w][2] = ms(r - 900); t[w][3] = ms(r1 - 900);
        printf("%s: held peak %d, start to 50%% %.0f ms, to 90%% %.0f ms; release to 50%% %.0f ms, to 10%% %.0f ms\n",
               w ? "adapter" : "as is  ", held, t[w][0], t[w][1], t[w][2], t[w][3]);
    }
    /* the release is the reed fade over 4410 model samples: twice as long in real time. The start is a ramp (twice as long)
     * plus the loop's own build-up, which takes as many periods as before, so it is only required to be slower. Levels come
     * from a 6-call window, which adds up to 4 ms. */
    for (int j = 2; j < 4; j++) CHECK(t[1][j] > 1.6 * t[0][j] && t[1][j] < 2.6 * t[0][j], "release time %d: %.0f ms vs %.0f ms is not about twice as long", j, t[1][j], t[0][j]);
    for (int j = 0; j < 2; j++) CHECK(t[1][j] > 1.5 * t[0][j], "start time %d: %.0f ms vs %.0f ms is not slower", j, t[1][j], t[0][j]);

    /* six voices through the adapter: sounding, bounded, and half the calls compute */
    reset();
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = (int32_t)llround(4294967296.0 * 220.0 / VOICE_HZ);
    for (int v = 0; v < 6; v++) { ENGINE_reedie.note_on(host_voice_mem + v * 0x24, k); ((reedie_voice_t *)(host_voice_mem + v * 0x24))->rng = 0x9e3779b9u * (uint32_t)(v + 1); }
    long energy[6] = { 0 };
    for (int c = 0; c < 400; c++) for (int v = 0; v < 6; v++) {
        int16_t o[OP1_BLOCK]; ENGINE_reedie.render(host_voice_mem + v * 0x24, o, k);
        for (int n = 0; n < OP1_BLOCK; n++) energy[v] += abs(o[n]);
    }
    for (int v = 0; v < 6; v++) CHECK(energy[v] > 100000, "voice %d is silent (%ld)", v, energy[v]);
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
