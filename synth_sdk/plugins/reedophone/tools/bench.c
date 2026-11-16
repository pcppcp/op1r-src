/* Cycle estimate on the Blackfin simulator: bfin-elf-gcc -msim -O2 -DOP1_HOST bench.c ../reedophone.c; bfin-elf-run.
 * Renders 6 voices (3 low register, 3 upper) for 40 blocks each after a warm-up, reads CYCLES around render. */
#include "op1_plugin.h"
#include <stdio.h>
#ifndef ENG
#define ENG ENGINE_reedophone
#endif
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6]; int16_t host_voice_env[128]; int16_t host_accel[3];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
extern const op1_engine_t ENG;
static inline uint32_t cyc(void) { uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c)); return c; }
int main(void)
{
    __asm__ volatile ("R0 = SYSCFG; BITSET(R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");   /* enable CYCLES */
    /* sine table by a rotating phasor (no libm needed) */
    double c = 0.99999970586, s = 0.00076699031, x = 1, y = 0;
    for (int i = 0; i < 8192; i++) { host_sine[i] = (int16_t)(y * 32767); double nx = x * c - y * s; y = x * s + y * c; x = nx; }
    static const int32_t incs[6] = { 14239000, 19000000, 28500000, 45300000, 60000000, 90000000 };   /* ~146 .. 920 Hz */
    int16_t k[8] = { 26214, 16384, 16384, 8192, 0x5301, 0, 0, 0 }, out[128];
    for (int i = 0; i < 128; i++) host_voice_env[i] = 16000;
    for (int v = 0; v < 6; v++) { host_voice_vel[v] = 127 << 8; ENG.note_on(host_voice_mem + v * 0x24, k); }
    uint32_t tot = 0, setup_max = 0;
    for (int b = 0; b < 60; b++)
        for (int v = 0; v < 6; v++) {
            for (int i = 0; i < 128; i++) host_pitch_inc[i] = incs[v];
            uint32_t t0 = cyc();
            ENG.render(host_voice_mem + v * 0x24, out, k);
            uint32_t d = cyc() - t0;
            if (b >= 20) tot += d; else if (d > setup_max) setup_max = d;
        }
    printf("cycles per voice-block %lu (per sample %lu), first-blocks max %lu\n", (unsigned long)(tot / 240), (unsigned long)(tot / 240 / 128), (unsigned long)setup_max);
    printf("6 voices at 44.1 kHz: %lu Mcycles/s\n", (unsigned long)((uint64_t)tot / 240 * 6 * 44100 / 128 / 1000000));
    return 0;
}
