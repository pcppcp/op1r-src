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
 * original runs (the model is not used until it is proven). */
#include "../op1_synth.h"
#include "stagecheck.h"

#define MASTER_OUTPUT_STAGE ((void (*)(void))0xFFA04280)

#if defined(__bfin__)                                  /* the DSP instructions themselves (GCC builtins) */
static inline int32_t sat32(int64_t v) { return v > 0x7FFFFFFFLL ? 0x7FFFFFFF : v < -0x80000000LL ? (int32_t)0x80000000 : (int32_t)v; }
static inline int32_t adds(int32_t a, int32_t b) { return __builtin_bfin_add_fr1x32(a, b); }
static inline int32_t subs(int32_t a, int32_t b) { return __builtin_bfin_sub_fr1x32(a, b); }
static inline int32_t shls(int32_t a, int n) { return __builtin_bfin_shl_fr1x32(a, n); }
static inline int32_t mul32(int32_t a, int32_t b) { return __builtin_bfin_mult_fr1x32x32(a, b); }
static inline int64_t frac16(int16_t a, int16_t b) { return (a == -32768 && b == -32768) ? 0x7FFFFFFF : 2 * (int64_t)a * b; }
static inline int16_t mulh16(int16_t a, int16_t b) { return __builtin_bfin_mult_fr1x16(a, b); }
#else                                                  /* host: the same arithmetic in C */
static inline int32_t sat32(int64_t v) { return v > 0x7FFFFFFFLL ? 0x7FFFFFFF : v < -0x80000000LL ? (int32_t)0x80000000 : (int32_t)v; }
static inline int32_t adds(int32_t a, int32_t b) { return sat32((int64_t)a + b); }
static inline int32_t subs(int32_t a, int32_t b) { return sat32((int64_t)a - b); }
static inline int32_t shls(int32_t a, int n) { return sat32((int64_t)a << n); }
/* signed fractional 16x16 product into an accumulator: 2ab, except -1 * -1 (0x8000 x 0x8000) = 0x7FFFFFFF */
static inline int64_t frac16(int16_t a, int16_t b) { return (a == -32768 && b == -32768) ? 0x7FFFFFFF : 2 * (int64_t)a * b; }
/* libdsp mult_fr1x32x32: A1 = aL*bL (FU) >> 16; A1 += aH*bL (M); A0 = aH*bH; A1 += bH*aL (M); A1 >>>= 15; sat(A0 + A1) */
static inline int32_t mul32(int32_t a, int32_t b)
{
    uint16_t al = (uint16_t)a, bl = (uint16_t)b;
    int16_t ah = (int16_t)(a >> 16), bh = (int16_t)(b >> 16);
    int64_t a1 = ((uint32_t)al * bl) >> 16;
    a1 += (int64_t)ah * bl;
    int64_t a0 = frac16(ah, bh);
    a1 += (int64_t)bh * al;
    a1 >>= 15;
    return sat32(a0 + a1);
}
/* Dreg_lo = Dreg_x * Dreg_y (16-bit fractional result): 2ab rounded to the top 16 bits, saturated */
static inline int16_t mulh16(int16_t a, int16_t b)
{
    int64_t p = frac16(a, b) + 0x8000;
    p >>= 16;
    return (int16_t)(p > 32767 ? 32767 : p < -32768 ? -32768 : p);
}
#endif
static inline int16_t hi16(int32_t v) { return (int16_t)(v >> 16); }
static inline int16_t vmax16(int16_t a, int16_t b) { return a > b ? a : b; }

#define ST     ((volatile int32_t *)0xFF801880)        /* +0 envL +4 stateL +8 envR +12 stateR +16 hold count */
#define STW    ((volatile int16_t *)0xFF801880)        /* halves: 0x14 / 0x16 block peaks, 0x18 / 0x1A held peaks */
#define SEED   (*(volatile int32_t *)0xFF802BC8)
#define DITH   ((volatile int32_t *)0xFF801B50)
#define IN_L   ((const volatile int16_t *)0xFF800D34)
#define IN_R   ((const volatile int16_t *)0xFF800DB4)
#define OUT_L  ((volatile int32_t *)0xFF800E34)
#define OUT_R  ((volatile int32_t *)0xFF800F34)
#define TAB1   ((const volatile int32_t *)0x0106A8E4)
#define TAB2   ((const volatile int32_t *)0x0106A8F4)
#define ONE    0x7FFFFFFF
#define LCG_M  0x0019660Du
#define LCG_C  0x3C6EF35Fu

static uint32_t dither_fill(uint32_t seed)                 /* 64 words: sat((s >>> 16) << 1) of the next seeds */
{
    for (int i = 0; i < 64; i++) {
        seed = seed * LCG_M + LCG_C;
        DITH[i] = shls((int32_t)seed >> 16, 1);
    }
    return seed;
}
/* one channel's 64 samples: env / state in, out; peak (max |out|.H) returned in *pk */
static void channel(const volatile int16_t *in, volatile int32_t *out, int32_t gain, int32_t outg, int32_t c1, int32_t c2,
                    int32_t *env, int32_t *state, int16_t *pk)
{
    int32_t e = *env, s = *state;
    const int32_t one_c1 = subs(ONE, c1), one_c2 = subs(ONE, c2);
    int16_t peak = *pk;
    for (int i = 0; i < 64; i++) {
        int32_t e4 = shls(e, 4);
        int32_t g = subs(ONE, mul32(e4, e4));
        int32_t y = adds(mul32((int32_t)in[i] << 16, gain), mul32(DITH[i], gain));
        y = mul32(mul32(y, g), outg);
        int32_t o = shls(y, 4);
        out[i] = o;
        int32_t ab = (int32_t)(0u - (uint32_t)o);         /* R5 = -R2 (wraps), MAX(R5, R2): 0x80000000 stays */
        if (o > ab) ab = o;
        peak = vmax16(peak, hi16(ab));
        int32_t over = adds(ab, (int32_t)0xC0000000);    /* |out| - 0.5 */
        if (over < 0) over = 0;
        over = shls(over, 1);
        int32_t att = adds(mul32(e, c2), mul32(over, one_c2));
        int32_t k = adds(c1, mul32(s, one_c1));
        int32_t s2 = mul32(s, c1);
        int32_t rel = adds(mul32(e, k), mul32(over, subs(ONE, k)));
        if (rel < over) { e = att; s = ONE; } else { e = rel; s = s2; }
    }
    *env = e; *state = s; *pk = peak;
}
void out_stage_model(void)
{
    const int32_t G = ST[7], M = ST[8];                    /* 0x...89C, 0x...8A0 */
    const int idx = ((int16_t)(M >> 16) >> 10) + 4;
    const int32_t c1 = TAB1[idx], c2 = TAB2[idx];
    const int32_t gainL = (int32_t)mulh16((int16_t)G, (int16_t)G) << 16;
    const int32_t gainR = (int32_t)mulh16(hi16(G), hi16(G)) << 16;
    const int32_t outg = (int32_t)mulh16((int16_t)M, (int16_t)M) << 16;
    int32_t hold = ST[4] - 1;
    ST[4] = hold;
    if (hold < 0) ST[6] = 0;                               /* held peaks (0x...898 / 89A) */
    const int32_t eL = ST[0], eR = ST[2];
    int32_t envL = adds(mul32(eL, 0x7F333333), mul32(eR, 0x00CCCCCC));
    int32_t envR = adds(mul32(eR, 0x7F333333), mul32(eL, 0x00CCCCCC));
    ST[5] = 0;                                             /* block peaks (0x...894 / 896) */
    uint32_t seed = dither_fill((uint32_t)SEED);
    int32_t sL = ST[1], sR = ST[3];
    int16_t pL = 0, pR = 0;
    channel(IN_L, OUT_L, gainL, outg, c1, c2, &envL, &sL, &pL);
    ST[0] = envL; STW[10] = pL; ST[1] = sL;
    seed = dither_fill(seed);
    SEED = (int32_t)seed;
    channel(IN_R, OUT_R, gainR, outg, c1, c2, &envR, &sR, &pR);
    ST[2] = envR; ST[3] = sR; STW[11] = pR;
    if (STW[12] < STW[10]) { ST[4] = 0x2B1; STW[12] = pL; }
    if (STW[13] < STW[11]) { ST[4] = 0x2B1; STW[13] = pR; }
}

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
