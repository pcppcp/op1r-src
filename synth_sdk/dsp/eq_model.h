/* The master EQ model (docs/81), shared: dsp/eq.c (check builds, SDRAM) and dsp/eq_l1.c (the L1 copy at 0xFFA030F0
 * in place of R246's master_eq). The FIR is dsp/fir.S on the Blackfin (EQ_FIR), C on the host. */
#ifndef EQ_ATTR
#define EQ_ATTR
#endif
#ifndef EQ_ENTRY
#define EQ_ENTRY eq_model
#endif
#ifndef EQ_HELPER
#define EQ_HELPER static
#endif
#if defined(__bfin__)                                  /* the DSP instructions (GCC builtins): the same results */
static inline int32_t sat32(int64_t v) { return v > 0x7FFFFFFFLL ? 0x7FFFFFFF : v < -0x80000000LL ? (int32_t)0x80000000 : (int32_t)v; }
static inline int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static inline int64_t frac16(int16_t a, int16_t b) { return __builtin_bfin_mult_fr1x32(a, b); }
static inline int32_t adds(int32_t a, int32_t b) { return __builtin_bfin_add_fr1x32(a, b); }
static inline int32_t shls1(int32_t a) { return __builtin_bfin_shl_fr1x32(a, 1); }
static inline int32_t mul32(int32_t a, int32_t b) { return __builtin_bfin_mult_fr1x32x32(a, b); }
#define ACC_ADD(a, p) __builtin_bfin_add_fr1x32((a), (int32_t)(p))
#define ACC_SUB(a, p) __builtin_bfin_sub_fr1x32((a), (int32_t)(p))
#else
static inline int32_t sat32(int64_t v) { return v > 0x7FFFFFFFLL ? 0x7FFFFFFF : v < -0x80000000LL ? (int32_t)0x80000000 : (int32_t)v; }
static inline int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static inline int64_t frac16(int16_t a, int16_t b) { return (a == -32768 && b == -32768) ? 0x7FFFFFFF : 2 * (int64_t)a * b; }
static inline int32_t adds(int32_t a, int32_t b) { return sat32((int64_t)a + b); }
static inline int32_t shls1(int32_t a) { return sat32((int64_t)a << 1); }
static inline int32_t mul32(int32_t a, int32_t b)          /* libdsp mult_fr1x32x32 (as dsp/outstage_model.h) */
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
#define ACC_ADD(a, p) sat32((int64_t)(a) + (p))
#define ACC_SUB(a, p) sat32((int64_t)(a) - (p))
#endif
static inline int16_t hi16(int32_t v) { return (int16_t)(v >> 16); }
static inline int16_t rnd16(int64_t acc)                    /* accumulator -> 16 bits: round to nearest, ties to even */
{
    int64_t q = acc >> 16, r = acc & 0xFFFF;
    if (r > 0x8000 || (r == 0x8000 && (q & 1))) q++;
    return (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
}

static inline int16_t mulh16(int16_t a, int16_t b) { return rnd16(frac16(a, b)); }   /* ties to even, as the original (the GCC builtin differs at ties) */   /* R.L = a * b: the same rounding (ties to even: measured) */
#ifndef __bfin__
static void fir_c(fir_st *s, const volatile int16_t *x, int16_t *y)

{
    const int k = s->k;
    int idx = (int)(s->p - s->d);
    for (int i = 0; i < 64; i++) {
        const int16_t xi = x[i];
        int64_t acc = frac16(s->h[0], xi);
        int e = idx + 1 == k ? 0 : idx + 1;               /* the newest sample sits at p + 1 (measured), older ones after it */
        for (int j = 1; j < k; j++) { int t = e + j; if (t >= k) t -= k; acc += frac16(s->h[j], s->d[t]); }
        y[i] = rnd16(acc);
        s->d[e] = xi;
        idx = idx ? idx - 1 : k - 1;
    }
    s->p = s->d + idx;
}
#define EQ_FIR fir_c
#endif
EQ_HELPER int32_t onepole(int16_t *b, int16_t c, int32_t s)    /* in place; returns the new state */
{
    int32_t a = ACC_ADD(s, frac16(c, b[0])), prev = s;
    for (int i = 0; i < 64; i++) {
        a = ACC_SUB(a, frac16(c, hi16(prev)));
        prev = a;
        if (i < 63) { b[i] = hi16(a); a = ACC_ADD(a, frac16(c, b[i + 1])); }
        else b[i] = hi16(a);
    }
    return a;
}
#define PRM   ((volatile int32_t *)0xFF8017CC)              /* [0] w0, [1] w1, [2..4] meters */
#define LO    ((int16_t *)0x01AABB28)
#define HI    ((int16_t *)0x01AABBA8)
#define MID   ((int16_t *)0x01AABC28)
#define FIRS  ((fir_st *)0x01AABAD8)                       /* lo L, hi L, lo R, hi R (stride 0x14) */
#define TAB   ((const volatile int16_t *)0x0106A5EC)
EQ_HELPER void channel(volatile int16_t *x, volatile int16_t *dly, int32_t *st, int ch, int16_t cl, int16_t cm,
                    int32_t a, int32_t b, int32_t c)
{
    EQ_FIR((fir_st *)((uint8_t *)FIRS + 0x28 * ch), x, LO);
    EQ_FIR((fir_st *)((uint8_t *)FIRS + 0x28 * ch + 0x14), x, HI);
    int32_t di = st[4 + ch];
    for (int i = 0; i < 64; i++) {
        MID[i] = sat16(sat16(dly[di] - LO[i]) - HI[i]);
        dly[di] = x[i];
        if (++di >= 25) di -= 25;
    }
    st[4 + ch] = di;
    st[2 * ch] = onepole(LO, cl, st[2 * ch]);
    st[2 * ch + 1] = onepole(MID, cm, st[2 * ch + 1]);
    int32_t m0 = PRM[2], m1 = PRM[3], m2 = PRM[4];
    for (int i = 0; i < 64; i++) {
        int32_t s1 = shls1(mul32((int32_t)LO[i] << 16, a));
        if (s1 > m0) m0 = s1;
        int32_t s3 = shls1(mul32((int32_t)MID[i] << 16, b));
        if (s3 > m1) m1 = s3;
        int32_t sum = adds(s1, s3);
        int32_t s2 = shls1(mul32((int32_t)HI[i] << 16, c));
        if (s2 > m2) m2 = s2;
        sum = shls1(adds(s2, sum));
        x[i] = hi16(sum);
    }
    PRM[2] = m0; PRM[3] = m1; PRM[4] = m2;
}
EQ_ATTR void EQ_ENTRY(int32_t *st)
{
    const int32_t w0 = PRM[0], w1 = PRM[1];
    PRM[2] = mul32(PRM[2], 0x7F000000); PRM[3] = mul32(PRM[3], 0x7F000000); PRM[4] = mul32(PRM[4], 0x7F000000);
    const int16_t cl = TAB[sat16(hi16(w0) + 0x3000) >> 8], cm = TAB[sat16((int16_t)w1 + 0x5000) >> 8];
    const int32_t a = (int32_t)mulh16((int16_t)w0, (int16_t)w0) << 16, b = (int32_t)mulh16(hi16(w0), hi16(w0)) << 16;
    const int32_t c = (int32_t)mulh16((int16_t)w1, (int16_t)w1) << 16;
    channel((volatile int16_t *)0xFF800D34, (volatile int16_t *)0xFF8017F8, st, 0, cl, cm, a, b, c);
    channel((volatile int16_t *)0xFF800DB4, (volatile int16_t *)0xFF80182C, st, 1, cl, cm, a, b, c);
}

