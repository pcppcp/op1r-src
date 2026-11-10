/* Master EQ (docs/81), stage 2 of the audio core takeover: R246's master_eq (L1 0xFFA030F0, called at 0xFFA01118 with
 * R0 = its state) goes through the L1 trampoline l1_tramp_eq to eq_stage(). Default: the original. OP1R_EQ=check:
 * original and the C model eq_model() per block from the same state, every written byte compared (dsp/stagecheck.h).
 * The model, from the disassembly, per channel (L: input 0xFF800D34, R: 0xFF800DB4, in place):
 *   lo = fir_fr16(x) (51 taps, state 0x01AABAD8 / B00), hi = fir_fr16(x) (state AEC / B14): 40-bit MAC, rounded to 16;
 *   mid = sat16(sat16(x delayed 25 samples - lo) - hi) (delay lines 0xFF8017F8 / 182C, index st[4] / st[5]);
 *   lo and mid through one-poles (W32 accumulators, st[0..3]): y = y + c x - c y.H with c = T[sat16(w0.H + 0x3000) >> 8]
 *   (lo) and T[sat16(w1.L + 0x5000) >> 8] (mid), T = int16 table 0x0106A5EC; w0 / w1 = [0xFF8017CC] / [0xFF8017D0];
 *   out = (sat(sat(lo a 2 + mid b 2) + hi c 2) 2).H with a = w0.L^2, b = w0.H^2, c = w1.L^2; peak meters [0xFF8017D4..DC]
 *   (decay x 0x7F00 per block, max of each band's product). */
#include "../op1_synth.h"
#include "stagecheck.h"

#define MASTER_EQ ((void (*)(void *))0xFFA030F0)

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
static inline int16_t hi16(int32_t v) { return (int16_t)(v >> 16); }
static inline int16_t rnd16(int64_t acc)                    /* accumulator -> 16 bits: round to nearest, ties to even */
{
    int64_t q = acc >> 16, r = acc & 0xFFFF;
    if (r > 0x8000 || (r == 0x8000 && (q & 1))) q++;
    return (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
}

static inline int16_t mulh16(int16_t a, int16_t b) { return rnd16(frac16(a, b)); }   /* R.L = a * b: the same rounding (ties to even: measured) */
typedef struct { const int16_t *h; int16_t *d; int16_t *p; int32_t k; } fir_st;
static void fir(const volatile int16_t *x, int16_t *y, int n, fir_st *s)
{
    const int k = s->k;
    int idx = (int)(s->p - s->d);
    for (int i = 0; i < n; i++) {
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
static int32_t onepole(int16_t *b, int16_t c, int32_t s)    /* in place; returns the new state */
{
    int32_t a = sat32((int64_t)s + frac16(c, b[0])), prev = s;
    for (int i = 0; i < 64; i++) {
        a = sat32((int64_t)a - frac16(c, hi16(prev)));
        prev = a;
        if (i < 63) { b[i] = hi16(a); a = sat32((int64_t)a + frac16(c, b[i + 1])); }
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
static void channel(volatile int16_t *x, volatile int16_t *dly, int32_t *st, int ch, int16_t cl, int16_t cm,
                    int32_t a, int32_t b, int32_t c)
{
    fir(x, LO, 64, (fir_st *)((uint8_t *)FIRS + 0x28 * ch));
    fir(x, HI, 64, (fir_st *)((uint8_t *)FIRS + 0x28 * ch + 0x14));
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
void eq_model(int32_t *st)
{
    const int32_t w0 = PRM[0], w1 = PRM[1];
    PRM[2] = mul32(PRM[2], 0x7F000000); PRM[3] = mul32(PRM[3], 0x7F000000); PRM[4] = mul32(PRM[4], 0x7F000000);
    const int16_t cl = TAB[sat16(hi16(w0) + 0x3000) >> 8], cm = TAB[sat16((int16_t)w1 + 0x5000) >> 8];
    const int32_t a = (int32_t)mulh16((int16_t)w0, (int16_t)w0) << 16, b = (int32_t)mulh16(hi16(w0), hi16(w0)) << 16;
    const int32_t c = (int32_t)mulh16((int16_t)w1, (int16_t)w1) << 16;
    channel((volatile int16_t *)0xFF800D34, (volatile int16_t *)0xFF8017F8, st, 0, cl, cm, a, b, c);
    channel((volatile int16_t *)0xFF800DB4, (volatile int16_t *)0xFF80182C, st, 1, cl, cm, a, b, c);
}

#ifdef EQ_CHECK
volatile stage_check_t eq_check;
static int32_t *eq_arg;
static void eq_orig(void) { MASTER_EQ(eq_arg); }
static void eq_mod(void) { eq_model(eq_arg); }
#endif
#ifdef EQ_STRESS
/* test only (OP1R_EQ=stress): synthetic input and EQ settings per block (all gains and both table indices, loud,
 * clipping, silence, noise). Changes the sound. */
static uint32_t es_n, es_rng = 777;
static uint32_t es_rand(void) { es_rng = es_rng * 1103515245u + 12345u; return es_rng >> 8; }
static void eq_stress(void)
{
    volatile int16_t *x = (volatile int16_t *)0xFF800D34;   /* L and R, 128 */
    const int mode = (int)((es_n >> 5) % 5);
    for (int i = 0; i < 128; i++) {
        int32_t v = mode == 0 ? (int32_t)(es_rand() & 0xFFFF) - 32768 : mode == 1 ? ((i & 2) ? 32767 : -32768)
                  : mode == 2 ? 0 : mode == 3 ? (int32_t)((es_rand() & 0x3F) - 32) : ((es_n & 16) ? 20000 : -20000);
        x[i] = (int16_t)v;
    }
    if (!(es_n & 31)) PRM[0] = (int32_t)es_rand() << 8 ^ (int32_t)es_rand(), PRM[1] = (int32_t)es_rand();
    es_n++;
}
#endif
void eq_stage(int32_t *st)
{
#ifdef EQ_STRESS
    eq_stress();
#endif
#ifdef EQ_CHECK
    static stage_region r[9];
    unsigned n = 0;
    r[n++] = (stage_region){ (void *)0xFF8017CC, 0x14 };
    r[n++] = (stage_region){ (void *)0xFF8017F8, 102 };   /* both delay lines: L 25 + 1 spare word, R 25 */
    r[n++] = (stage_region){ st, 0x18 };
    r[n++] = (stage_region){ (void *)0x01AABB28, 0x180 };
    r[n++] = (stage_region){ (void *)0x01AABAD8, 0x50 };
    for (int f = 0; f < 4; f++) {                          /* the four FIR delay lines */
        fir_st *s = (fir_st *)((uint8_t *)FIRS + 0x14 * f);
        r[n++] = (stage_region){ s->d, (uint32_t)s->k * 2 };
    }
    static stage_region rr[11];
    for (unsigned i = 0; i < n; i++) rr[i] = r[i];
    rr[n++] = (stage_region){ (void *)0xFF800D34, 128 };
    rr[n++] = (stage_region){ (void *)0xFF800DB4, 128 };
    eq_arg = st;
    stage_check(&eq_check, rr, n, eq_orig, eq_mod);
#else
    void (*volatile orig)(void *) = MASTER_EQ;
    orig(st);
#endif
}
