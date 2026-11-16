/* Bit-exactness of the Blackfin assembly loops (rv_asm.h) against the C reference (bore_c, radiate_c), on random
 * states including saturating extremes. Blackfin simulator only:
 *   bfin-elf-gcc -msim -O2 -std=gnu99 -mcpu=bf524 -DOP1_HOST -I ../../.. asm_check.c -o asm_check.elf
 *   bfin-elf-run asm_check.elf
 * Also times each loop (cycles per sample). */
#include "../reedophone.c"
#include <stdio.h>
#include <string.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6]; int16_t host_voice_env[128]; int16_t host_accel[3];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];

static uint32_t rs = 12345;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static int32_t rr(int32_t lo, int32_t hi) { return lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1)); }
static inline uint32_t cyc(void) { uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c)); return c; }

static int16_t qa[LN][2] __attribute__((aligned(8))), qb[LN][2] __attribute__((aligned(8)));
static int16_t ga[OP1_BLOCK + 2], gb2[OP1_BLOCK + 2], oa[OP1_BLOCK], ob[OP1_BLOCK];

int main(void)
{
    __asm__ volatile ("R0 = SYSCFG; BITSET(R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");
    int bad = 0; uint32_t tc[2] = { 0, 0 }, ta[2] = { 0, 0 };
    for (int it = 0; it < 400 && bad < 5; it++) {
        int mode = it % 4;                         /* 0: realistic, 1-3: increasingly wild */
        int32_t amp = mode == 0 ? 4000 : (mode == 1 ? 12000 : 32767);
        for (int i = 0; i < LN; i++) for (int j = 0; j < 2; j++) qa[i][j] = (int16_t)rr(-amp, amp);
        memcpy(qb, qa, sizeof qa);
        rv_io_t io; memset(&io, 0, sizeof io);
        io.k[0] = REED_B1; io.k[1] = REED_A1; io.k[2] = REED_A2; io.k[3] = (int16_t)rr(0, 32767);
        int32_t fr = rr(0, 32767);                 /* Lagrange coefficients as in set_delays, or random */
        if (mode < 2) { io.k[4] = (int16_t)rr(-3000, 3000); io.k[5] = (int16_t)rr(0, 16384); io.k[6] = (int16_t)(16384 - io.k[5]); io.k[7] = (int16_t)rr(-3000, 3000); }
        else for (int j = 4; j < 8; j++) io.k[j] = (int16_t)rr(-32768, 32767);
        (void)fr;
        if (mode == 3) for (int j = 8; j < 12; j++) io.k[j] = (int16_t)rr(-32768, 32767);   /* unused slots */
        io.k[12] = (int16_t)rr(0, 32767); io.k[13] = (int16_t)rr(16384, 32767);
        io.k[14] = EQ_C[0]; io.k[15] = EQ_C[1];
        io.f[0] = EQ_C[2]; io.f[1] = -EQ_C[3]; io.f[2] = -EQ_C[4]; io.f[4] = OLP_C[0]; io.f[5] = OLP_C[1]; io.f[6] = OLP_C[2]; io.f[7] = -OLP_C[3];
        io.f2[0] = -OLP_C[4]; io.f2[1] = RADHP_Q15; io.f2[2] = OUT_GAIN;
        if (mode == 3) { for (int j = 14; j < 16; j++) io.k[j] = (int16_t)rr(-32768, 32767); for (int j = 0; j < 8; j++) io.f[j] = (int16_t)rr(-32768, 32767); io.f2[0] = (int16_t)rr(-32768, 32767); io.f2[1] = (int16_t)rr(0, 32767); io.f2[2] = (int16_t)rr(0, 32767); }
        for (int n = 0; n < OP1_BLOCK + 2; n++) ga[n] = (int16_t)rr(0, mode == 0 ? 12000 : 18431);
        memcpy(gb2, ga, sizeof ga);
        io.sqd = SQD - 64;
        uint32_t w = (uint32_t)rr(0, LM), lbb = (uint32_t)rr(1, 1000), lbf = (uint32_t)rr(1, 1000);
        io.x12 = pk(rr(-amp, amp), rr(-amp, amp)); io.pp = pk(rr(-amp, amp), rr(-amp, amp)); io.lps = rr(-amp, amp) << 15;
        io.hy = rr(-amp, amp) << 9; io.ex = pk(rr(-amp, amp), rr(-amp, amp)); io.ey = pk(rr(-amp, amp), rr(-amp, amp));
        io.lx = pk(rr(-amp, amp), rr(-amp, amp)); io.ly = pk(rr(-amp, amp), rr(-amp, amp));
        rv_io_t ia = io, ib = io;
        ia.base = &qa[0][0]; ib.base = &qb[0][0]; ia.g = ga + 2; ib.g = gb2 + 2; ia.out = oa; ib.out = ob;
        ia.ip[0] = &qa[0][0]; ia.ip[1] = &qa[(w - lbb) & LM][1]; ia.ip[2] = &qa[(w + 1 - lbf - 2) & LM][0]; ia.ip[3] = &qa[(w + 1) & LM][0];
        ib.ip[0] = &qb[0][0]; ib.ip[1] = &qb[(w - lbb) & LM][1]; ib.ip[2] = &qb[(w + 1 - lbf - 2) & LM][0]; ib.ip[3] = &qb[(w + 1) & LM][0];
        uint32_t t0 = cyc(); bore_c(&ia); uint32_t t1 = cyc();
        bore_asm(&ib);
        uint32_t t2 = cyc();
        tc[0] += t1 - t0; ta[0] += t2 - t1;
        t0 = cyc(); radiate_c(&ia); t1 = cyc(); radiate_asm(&ib); t2 = cyc();
        tc[1] += t1 - t0; ta[1] += t2 - t1;
        const char *what = 0;
        if (memcmp(qa, qb, sizeof qa)) what = "lines";
        else if (memcmp(ga, gb2, sizeof ga)) what = "g (e + r) / 2";
        else if (ia.x12 != ib.x12 || ia.pp != ib.pp || ia.lps != ib.lps) what = "bore state";
        else if (memcmp(oa, ob, sizeof oa)) what = "out";
        else if (ia.hy != ib.hy || ia.ex != ib.ex || ia.ey != ib.ey || ia.lx != ib.lx || ia.ly != ib.ly) what = "radiate state";
        if (what) {
            bad++;
            printf("MISMATCH it %d (mode %d): %s\n", it, mode, what);
            for (int n = 0; n < OP1_BLOCK; n++) if (ga[n + 2] != gb2[n + 2]) { printf("  first g diff at n %d: c %d asm %d\n", n, ga[n + 2], gb2[n + 2]); break; }
            for (int i = 0; i < LN; i++) for (int j = 0; j < 2; j++) if (qa[i][j] != qb[i][j]) { printf("  line diff q[%d][%d] c %d asm %d (w0 %lu)\n", i, j, qa[i][j], qb[i][j], (unsigned long)w); i = LN; break; }
            printf("  x12 %08lx %08lx pp %08lx %08lx lps %ld %ld\n", (unsigned long)ia.x12, (unsigned long)ib.x12, (unsigned long)ia.pp, (unsigned long)ib.pp, (long)ia.lps, (long)ib.lps);
            for (int n = 0; n < OP1_BLOCK; n++) if (oa[n] != ob[n]) { printf("  first out diff at n %d: c %d asm %d\n", n, oa[n], ob[n]); break; }
            printf("  hy %ld %ld\n", (long)ia.hy, (long)ib.hy);
        }
    }
    printf("%s\n", bad ? "FAILED" : "all 400 blocks bit-exact");
    printf("cycles/sample  C: bore %lu radiate %lu   asm: bore %lu radiate %lu\n",
           (unsigned long)(tc[0] / 400 / 128), (unsigned long)(tc[1] / 400 / 128),
           (unsigned long)(ta[0] / 400 / 128), (unsigned long)(ta[1] / 400 / 128));
    return bad != 0;
}
