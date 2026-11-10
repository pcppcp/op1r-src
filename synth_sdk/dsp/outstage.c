/* Master output stage (docs/81), stage 1 of the audio core takeover (docs/56 hook): core_block_process's CALL
 * master_output_stage (L1 0xFFA04280) comes here through the L1 trampoline l1_tramp_out.
 *
 * out_stage_model() is a bit-exact C model of R246's routine, transcribed from its disassembly (not the decompiler):
 * per block, both channels:
 *   prologue: gains G = [0xFF80189C] (L in .L, R in .H), M = [0xFF8018A0] (.L output gain, .H >> 10 + 4 = table
 *             index), squared to 16 bits (rounded); time constants c1 = [0x0106A8E4 + 4 i], c2 = [0x0106A8F4 + 4 i];
 *             the envelopes coupled (env = 0.99375 own + 0.00625 other); hold counter [0x...890] - 1, at < 0 the held
 *             peaks [0x...898] cleared; current peaks [0x...894] cleared; 64 dither words (LCG at 0xFF802BC8).
 *   per sample: g = 1 - (env << 4)^2; y = x * gain + d * gain; y = y * g; y = y * outgain; out = sat(y << 4);
 *             peak = max |out|.H; over = max(|out| - 0.5, 0) << 1; attack env' = env c2 + over (1 - c2);
 *             release k = c1 + state (1 - c1), env'' = env k + over (1 - k), state' = state c1;
 *             env'' < over: env = attack, state = 1; else env = release, state = state'.
 *   epilogue: block peaks [0x...894 / 896], the held ones [0x...898 / 89A] with a 689-block hold.
 * Arithmetic as the Blackfin does it: mul32 = libdsp's 32x32 fractional multiply (16x16 partial products in the
 * accumulators), mulh16 = a 16-bit fractional product rounded to 16 bits, saturating add / sub / shift.
 *
 * OP1R_OUTSTAGE=check (build flag OUTSTAGE_CHECK): every block runs the original and the model from the same state and
 * compares everything they write (dsp/stagecheck.h); the original's result is kept (nothing audible). Without it the
 * original routine's address is called: in production builds that is our copy in L1 (dsp/outstage_l1.c). */
#include "../op1_synth.h"
#include "stagecheck.h"

#define MASTER_OUTPUT_STAGE ((void (*)(void))0xFFA04280)

#include "outstage_model.h"

#ifdef OUTSTAGE_CHECK
static const stage_region os_regions[] = {                /* everything the stage writes */
    { (void *)0xFF801880, 0x24 }, { (void *)0xFF802BC8, 4 }, { (void *)0xFF801B50, 256 },
    { (void *)0xFF800E34, 256 }, { (void *)0xFF800F34, 256 },
};
volatile stage_check_t os_check;                           /* blocks, differing blocks, first difference (docs/81) */
#endif
#ifdef OUTSTAGE_STRESS
/* test only (OP1R_OUTSTAGE=stress): synthetic input and parameters per block, so the check covers every path (loud,
 * clipping, the limiter attacking and releasing, extreme gains, every time-constant index). Changes the sound. */
static uint32_t st_n, st_rng = 12345;
static uint32_t st_rand(void) { st_rng = st_rng * 1103515245u + 12345u; return st_rng >> 8; }
static void stress_inputs(void)
{
    volatile int16_t *inl = (volatile int16_t *)0xFF800D34, *inr = (volatile int16_t *)0xFF800DB4;
    const uint32_t phase = st_n >> 6;                  /* a new setting every 64 blocks */
    const int mode = (int)(phase % 6);
    int32_t amp = (int32_t)((st_n & 63) * 520);        /* rises over 64 blocks to about full scale */
    for (int i = 0; i < 64; i++) {
        int32_t v;
        switch (mode) {
        case 0: v = (i & 8) ? amp : -amp; break;                                  /* square, rising */
        case 1: v = (int32_t)(st_rand() & 0xFFFF) - 32768; break;                 /* full-scale noise */
        case 2: v = (i & 1) ? 32767 : -32768; break;                              /* extremes */
        case 3: v = 0; break;                                                     /* silence: release */
        case 4: v = (int32_t)((st_rand() & 0x3FF) - 512) * (amp >> 9); break;     /* quiet noise */
        default: v = (st_n & 128) ? 30000 : 100;                                   /* steps: attack / release */
        }
        inl[i] = (int16_t)v; inr[i] = (int16_t)(mode == 1 ? -v : v / 2);
    }
    if (!(st_n & 63)) {                                 /* gains and the time-constant index */
        static const int16_t gains[] = { 0x4000, 0x7FFF, (int16_t)0x8000, 0x2000, 0x6000, 0x7000 };
        volatile int16_t *w = (volatile int16_t *)0xFF80189C;
        w[0] = gains[phase % 6]; w[1] = gains[(phase + 2) % 6];
        w[2] = gains[(phase + 1) % 6];
        w[3] = (int16_t)((int32_t)((phase * 7) % 12) * 1024 - 4096);              /* index (>> 10) + 4 = 0..11 */
    }
    st_n++;
}
#endif
/* Boot self-test of the L1 copy (production builds, docs/81): R246's routine is gone there, so the compiled bytes at
 * 0xFFA04280 (dsp/outstage_l1.c) are compared with the C model in SDRAM on 64 synthetic blocks from identical starting
 * points (loud, quiet, clipping, every gain and time-constant index), every byte they write; then the real state is
 * put back. Run once in the db stage, before the audio interrupt starts. os_selftest: blocks, differing, 1 = ran. */
volatile uint32_t os_selftest[4];
#ifndef OUTSTAGE_CHECK
#include "stagecheck.h"
static const stage_region os_st_regions[] = {
    { (void *)0xFF801880, 0x24 }, { (void *)0xFF802BC8, 4 }, { (void *)0xFF801B50, 256 },
    { (void *)0xFF800E34, 256 }, { (void *)0xFF800F34, 256 },
};
#endif
void out_stage_selftest(void)
{
#ifndef OUTSTAGE_CHECK
    static uint8_t keep[804], keep_in[256];
    volatile uint8_t *in = (volatile uint8_t *)0xFF800D34;                 /* L and R inputs: 128 int16 */
    uint32_t off = 0;
    for (unsigned i = 0; i < 5; i++) { stage_copy(keep + off, (const volatile uint8_t *)os_st_regions[i].addr, os_st_regions[i].len); off += os_st_regions[i].len; }
    stage_copy(keep_in, in, 256);
    stage_check_t st = { 0 };
    uint32_t rng = 0x1234567u;
    for (int b = 0; b < 64; b++) {
        volatile int16_t *x = (volatile int16_t *)0xFF800D34;
        const int mode = b & 3;
        for (int i = 0; i < 128; i++) {
            rng = rng * 1103515245u + 12345u;
            int32_t v = (int32_t)(rng >> 16) - 32768;
            x[i] = (int16_t)(mode == 0 ? v : mode == 1 ? v / 64 : mode == 2 ? ((i & 4) ? 32767 : -32768) : 0);
        }
        volatile int32_t *stw = (volatile int32_t *)0xFF801880;                 /* states, gains, index */
        rng = rng * 1103515245u + 12345u; stw[0] = (int32_t)(rng & 0x0FFFFFFF);
        rng = rng * 1103515245u + 12345u; stw[2] = (int32_t)(rng & 0x03FFFFFF);
        stw[1] = (b & 8) ? 0x7FFFFFFF : (int32_t)(rng >> 1); stw[3] = (int32_t)(rng >> 2);
        static const int16_t gains[] = { 0x4000, 0x7FFF, (int16_t)0x8000, 0x2000, 0x6000, 0x1000 };
        volatile int16_t *w = (volatile int16_t *)0xFF80189C;
        w[0] = gains[b % 6]; w[1] = gains[(b + 2) % 6]; w[2] = gains[(b + 1) % 6];
        w[3] = (int16_t)((b % 12) * 1024 - 4096);
        stage_check(&st, os_st_regions, 5, (void (*)(void))0xFFA04280, out_stage_model);   /* "original" = the L1 copy */
    }
    off = 0;
    for (unsigned i = 0; i < 5; i++) { stage_put((volatile uint8_t *)os_st_regions[i].addr, keep + off, os_st_regions[i].len); off += os_st_regions[i].len; }
    stage_put(in, keep_in, 256);
    os_selftest[0] = st.blocks; os_selftest[1] = st.bad; os_selftest[2] = st.first_off; os_selftest[3] = 1;
#endif
}
void out_stage(void)
{
#ifdef OUTSTAGE_STRESS
    stress_inputs();
#endif
#ifdef OUTSTAGE_CHECK
    stage_check(&os_check, os_regions, sizeof os_regions / sizeof os_regions[0], MASTER_OUTPUT_STAGE, out_stage_model);
#else
    MASTER_OUTPUT_STAGE();
#endif
}
