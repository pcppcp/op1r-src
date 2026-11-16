/* bq3.h: Blackfin assembly vs C reference on random blocks (bfin-elf-run). */
#include "op1_plugin.h"
#include "../../licorice/bq3.h"
#include <stdio.h>
#include <string.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6]; int16_t host_voice_env[128]; int16_t host_accel[3];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
static uint32_t rs = 777;
static int32_t rr(int32_t lo, int32_t hi) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return lo + (int32_t)(rs % (uint32_t)(hi - lo + 1)); }
static inline uint32_t cyc(void) { uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c)); return c; }
int main(void)
{
    __asm__ volatile ("R0 = SYSCFG; BITSET(R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");
    int bad = 0; uint32_t tc = 0, ta = 0;
    for (int it = 0; it < 300; it++) {
        int16_t c[20], a[128], b[128]; int32_t za[6], zb[6];
        int amp = it % 3 == 0 ? 3000 : 32767;
        for (int i = 0; i < 20; i++) c[i] = (int16_t)rr(-32768, 32767);
        if (it % 3 == 0) { c[0] = 8000; c[3] = 13000; c[4] = -7000; }
        for (int i = 0; i < 128; i++) a[i] = b[i] = (int16_t)rr(-amp, amp);
        for (int i = 0; i < 6; i++) za[i] = zb[i] = (int32_t)((uint32_t)(uint16_t)rr(-amp, amp) | ((uint32_t)rr(-amp, amp) << 16));
        uint32_t t0 = cyc(); bq3_c(a, c, za); uint32_t t1 = cyc(); bq3_asm(b, c, zb); uint32_t t2 = cyc();
        tc += t1 - t0; ta += t2 - t1;
        if (memcmp(a, b, sizeof a) || memcmp(za, zb, sizeof za)) { if (bad++ < 3) for (int i = 0; i < 128; i++) if (a[i] != b[i]) { printf("it %d n %d: c %d asm %d\n", it, i, a[i], b[i]); break; } }
    }
    printf("%s; cycles/sample C %lu asm %lu\n", bad ? "FAILED" : "300 blocks bit-exact", (unsigned long)(tc / 300 / 128), (unsigned long)(ta / 300 / 128));
    return bad != 0;
}
