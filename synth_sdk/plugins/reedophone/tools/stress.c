/* Stress test of a synth plugin: 6 voices, random note on / off (slurs, retriggers, notes never released), pitch
 * jumps and glides over the whole range (including absurd increments), knob sweeps and extremes, garbage voice
 * memory, envelope jumps. Build with the sanitizers (WSL / Linux):
 *   cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -DOP1_HOST -DENGINE=ENGINE_x \
 *      -I ../../.. stress.c <plugin sources> -o stress && ./stress
 * Fails on a sanitizer report, a host m16 range abort, or a voice that keeps sounding long after its release. */
#include "op1_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6]; int16_t host_voice_env[128]; int16_t host_accel[3] = { 0, 0, 16384 };
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];
extern const op1_engine_t ENGINE;

static uint32_t rs = 2024;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static int32_t rr(int32_t lo, int32_t hi) { return lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1)); }

int main(void)
{
    for (int i = 0; i < 8192; i++) {             /* sine table by a rotating phasor */
        static double c = 0.99999970586, s = 0.00076699031, x = 1, y = 0;
        host_sine[i] = (int16_t)(y * 32767); double nx = x * c - y * s; y = x * s + y * c; x = nx;
    }
    for (unsigned i = 0; i < sizeof host_voice_mem; i++) host_voice_mem[i] = (uint8_t)rnd();   /* garbage */
    for (int v = 0; v < 6; v++) memset(host_voice_mem + v * 0x24, 0, 4);                          /* vtable word */
    int16_t k[8] = { 26214, 16384, 16384, 8192, 0x5301, 0, 0, 0 }, out[128];
    int gate[6] = { 0 }, silent_for[6] = { 0 }, env[6] = { 0 }; int32_t inc[6];
    long loud_after_release = 0; int maxpk = 0;
    for (int v = 0; v < 6; v++) inc[v] = 20000000;
    for (int b = 0; b < 60000; b++) {
        if (b % 500 == 0) for (int i = 0; i < 4; i++) k[i] = (int16_t)(rnd() % 3 == 0 ? (rnd() & 1 ? 0 : 32767) : rr(0, 32767));
        for (int v = 0; v < 6; v++) {
            uint8_t *self = host_voice_mem + v * 0x24;
            uint32_t r = rnd() % 1000;
            if (r < 6) {                                         /* note on (maybe a slur / retrigger) */
                host_voice_vel[v] = (int16_t)(rr(1, 127) << 8);
                int m = rnd() % 20 == 0 ? rr(-40, 160) : rr(20, 110);
                double f = 440.0; for (int j = 69; j < m; j++) f *= 1.0594630943; for (int j = m; j < 69; j++) f /= 1.0594630943;
                inc[v] = (int32_t)(f * 4294967296.0 / 44100.0 > 2.1e9 ? 2100000000 : f * 4294967296.0 / 44100.0);
                if (rnd() % 50 == 0) inc[v] = rnd() & 1 ? 0 : 0x7fffffff;
                ENGINE.note_on(self, k); gate[v] = 1;
            } else if (r < 12 && gate[v]) { if (ENGINE.note_off) ENGINE.note_off(self); gate[v] = 0; silent_for[v] = 0; }
            else if (r < 14) { int64_t t = (int64_t)inc[v] + rr(-3000000, 3000000); inc[v] = t > 0x7fffffff ? 0x7fffffff : (int32_t)t; }   /* bend */
            if (inc[v] < 0) inc[v] = 0;
            /* envelope: attack to velocity peak, release to 0 over ~150 ms; sometimes jumps */
            int peak = (host_voice_vel[v] >> 8) * 128;
            env[v] = gate[v] ? (env[v] + 600 > peak ? peak : env[v] + 600) : (env[v] - 110 < 0 ? 0 : env[v] - 110);
            if (rnd() % 400 == 0) env[v] = rr(0, 32767);
            for (int i = 0; i < 128; i++) { host_voice_env[i] = (int16_t)env[v]; host_pitch_inc[i] = inc[v]; }
            ENGINE.render(self, out, k);
            int pk = 0; for (int i = 0; i < 128; i++) { int a = out[i] < 0 ? -out[i] : out[i]; if (a > pk) pk = a; }
            if (pk > maxpk) maxpk = pk;
            if (!gate[v]) { silent_for[v]++; if (silent_for[v] > 1500 && (long)pk * env[v] > 64L * 32768) loud_after_release++; }
        }
    }
    printf("60000 blocks x 6 voices: max |out| %d, loud blocks > 2 s after release (env-weighted): %ld\n", maxpk, loud_after_release);
    return loud_after_release ? 1 : 0;
}
