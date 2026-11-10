/* The master output stage model (docs/81), shared: dsp/outstage.c (the check, in SDRAM) and dsp/outstage_l1.c (the
 * production copy in L1 at 0xFFA04280, in place of R246's routine). Everything inline: one function, no calls. */
#ifndef OS_ATTR
#define OS_ATTR
#endif
#ifndef OS_ENTRY
#define OS_ENTRY out_stage_model
#endif
#ifndef OS_HELPER                                      /* the per-channel loop (twice per block) */
#define OS_HELPER static inline __attribute__((always_inline))
#endif
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
static inline int16_t mulh16(int16_t a, int16_t b)      /* rounded to 16 bits, ties to even (the unit's RND_MOD) */
{
    int64_t p = frac16(a, b), q = p >> 16, r = p & 0xFFFF;
    if (r > 0x8000 || (r == 0x8000 && (q & 1))) q++;
    return (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
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

static inline __attribute__((always_inline)) uint32_t dither_fill(uint32_t seed)                 /* 64 words: sat((s >>> 16) << 1) of the next seeds */
{
    for (int i = 0; i < 64; i++) {
        seed = seed * LCG_M + LCG_C;
        DITH[i] = shls((int32_t)seed >> 16, 1);
    }
    return seed;
}
/* one channel's 64 samples: env / state in, out; peak (max |out|.H) returned in *pk */
OS_HELPER void channel(const volatile int16_t *in, volatile int32_t *out, int32_t gain, int32_t outg, int32_t c1, int32_t c2,
                    int32_t *env, int32_t *state, int16_t *pk)
{
    int32_t e = *env, s = *state;
    const int32_t one_c1 = subs(ONE, c1), one_c2 = subs(ONE, c2);
    int16_t peak = *pk;
    for (int i = 0; i < 64; i++) {
        int32_t g = ONE;                                   /* fast path (docs/81): e = 0 -> g = 1 - 0 exactly */
        if (e) { int32_t e4 = shls(e, 4); g = subs(ONE, mul32(e4, e4)); }
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
        if (!e && !over) { s = mul32(s, c1); continue; }  /* at rest: att = rel = 0, e stays 0, only the state decays */
        int32_t att = adds(mul32(e, c2), mul32(over, one_c2));
        int32_t k = adds(c1, mul32(s, one_c1));
        int32_t s2 = mul32(s, c1);
        int32_t rel = adds(mul32(e, k), mul32(over, subs(ONE, k)));
        if (rel < over) { e = att; s = ONE; } else { e = rel; s = s2; }
    }
    *env = e; *state = s; *pk = peak;
}
OS_ATTR void OS_ENTRY(void)
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

