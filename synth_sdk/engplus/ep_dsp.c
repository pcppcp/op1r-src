/* Engine layer DSP, see ep_dsp.h. Order: drive -> filter (lowpass + resonance) -> crush.
 * From op1-sdk features/engine_plus (c) 2026 BertBrandstifter, MIT License (LICENSE.op1-sdk); op1r: the filter
 * table for the voice rate (88.2 kHz, theirs was for 44.1 kHz: the cutoffs came out twice as high). */
#include "ep_dsp.h"
#if defined(__bfin__) && !defined(EP_C_ONLY)            /* the loops in assembly (ep_dsp_bf.S); EP_CHECK: both, compared */
void ep_drive_bf(int16_t *buf, int n, int G);
void ep_filter_bf(int32_t *y, int16_t *buf, int n, int32_t ak, int32_t g4);
void ep_crush_bf(ep_voice *v, int16_t *buf, int n, int32_t mask, int32_t hold);
static int ep_asm = 1;
#define EP_ASM ep_asm
#else
#define EP_ASM 0
#define ep_drive_bf(b, n, g) ((void)0)
#define ep_filter_bf(y, b, n, ak, g) ((void)0)
#define ep_crush_bf(v, b, n, m, h) ((void)0)
#endif

/* one-pole coefficient 1 - exp(-2 pi fc / 88200) in Q15 for fc = 18000 * 2^(-8 i / 32), i = 0..32
 * (18 kHz .. 70 Hz) */
static const int32_t a_tab[33] = {                  /* at OP1_VOICE_RATE, 88.2 kHz */
    23678, 21621, 19535, 17481, 15509, 13656, 11944, 10387, 8987, 7743, 6646, 5687, 4853, 4132, 3511, 2979, 2524,
    2136, 1805, 1525, 1287, 1086, 915, 772, 650, 547, 461, 388, 327, 275, 231, 195, 164
};

void ep_reset(ep_voice *v)
{
    v->y[0] = v->y[1] = v->y[2] = v->y[3] = 0;
    v->hold = 0;
    v->count = 0;
}

/* Blackfin fractional DSP ops (single-cycle MAC, saturating); portable equivalents elsewhere so
 * the host tests run the same arithmetic. fr16 = Q15, fr32 = Q31. */
#ifdef __bfin__
#define MULT(a, b)  __builtin_bfin_mult_fr1x32((a), (b))      /* fr16 x fr16 -> fr32, saturating */
#define ADD(a, b)   __builtin_bfin_add_fr1x32((a), (b))
#define SUB(a, b)   __builtin_bfin_sub_fr1x32((a), (b))
#define RND(a)      __builtin_bfin_round_fr1x32((a))          /* fr32 -> fr16, rounded, saturating */
#define SHL(a, n)   __builtin_bfin_shl_fr1x32((a), (n))
#else
static int32_t sat32(int64_t x) { return x > 0x7fffffff ? 0x7fffffff : x < -0x7fffffff - 1 ? -0x7fffffff - 1 : (int32_t)x; }
static int32_t MULT(int16_t a, int16_t b) { return a == -32768 && b == -32768 ? 0x7fffffff : (int32_t)a * b * 2; }
static int32_t ADD(int32_t a, int32_t b) { return sat32((int64_t)a + b); }
static int32_t SUB(int32_t a, int32_t b) { return sat32((int64_t)a - b); }
static int16_t RND(int32_t a) { int64_t r = ((int64_t)a + 0x8000) >> 16; return (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r); }
static int32_t SHL(int32_t a, int n) { return sat32((int64_t)a * ((int64_t)1 << n)); }
#endif

/* soft clip: u = x * g / 1.5 saturated to +-1, y = 1.5 u - 0.5 u^3 (smooth into +-1 at |u| = 1) */
static void drive(int16_t *buf, int n, int16_t p)
{
    int16_t G = (int16_t)((2048 + (int32_t)p * 15 / 16) * 2 / 3);   /* g / 1.5 in Q11, g = 1..16 */
    if (EP_ASM && !(((uintptr_t)buf | (uint32_t)n) & 3)) { ep_drive_bf(buf, n, G); return; }   /* pairs: 4-aligned */
    int16_t u, u2;
    int32_t u32;
    int i;

    for (i = 0; i < n; i++) {
        u = RND(SHL(MULT(buf[i], G), 4));       /* x * G, Q15, saturating at +-1 */
        u2 = RND(MULT(u, u));
        u32 = (int32_t)u * 65536;
        buf[i] = RND(ADD(u32, SUB(u32, MULT(u, u2)) >> 1));
    }
}

/* 4-pole ladder lowpass (24 dB/oct) with resonance feedback, in Q31 with 1 bit of headroom
 * (input enters at half scale). Saturation clips resonance peaks musically instead of wrapping. */
static void filter(ep_voice *v, int16_t *buf, int n, int16_t pf, int16_t pr)
{
    int32_t idx = pf >> 10, fr = pf & 1023;
    int32_t a = a_tab[idx] + (((a_tab[idx < 32 ? idx + 1 : 32] - a_tab[idx]) * fr) >> 10);
    int32_t k = (int32_t)pr * 3686 / 32767;    /* Q10: feedback 0 .. 3.6 */
    int32_t y1 = v->y[0], y2 = v->y[1], y3 = v->y[2], y4 = v->y[3], in;
    int16_t a16, k4, g4;
    int i;

    /* the one-sample feedback delay rings at very high cutoffs: scale resonance down there */
    if (a > 16384)
        k = k * (32768 - a) / 16384;
    a16 = (int16_t)(a > 32767 ? 32767 : a);
    k4 = (int16_t)(k << 3);                     /* k / 4 in Q15 */
    g4 = (int16_t)((1024 + k / 2) << 3);        /* make-up gain / 4 in Q15 (bass the feedback takes) */
    if (EP_ASM) { ep_filter_bf(v->y, buf, n, (uint16_t)a16 | (int32_t)k4 * 65536, g4); return; }
    for (i = 0; i < n; i++) {
        in = SUB((int32_t)buf[i] * 32768, SHL(MULT(k4, RND(y4)), 2));   /* x/2 - k * y4 */
        y1 = ADD(y1, MULT(a16, RND(SUB(in, y1))));
        y2 = ADD(y2, MULT(a16, RND(SUB(y1, y2))));
        y3 = ADD(y3, MULT(a16, RND(SUB(y2, y3))));
        y4 = ADD(y4, MULT(a16, RND(SUB(y3, y4))));
        buf[i] = RND(SHL(MULT(RND(y4), g4), 3));                       /* * gain * 2 (headroom) */
    }
    v->y[0] = y1; v->y[1] = y2; v->y[2] = y3; v->y[3] = y4;
}

static void crush(ep_voice *v, int16_t *buf, int n, int16_t p)
{
    int shift = (int32_t)p * 14 / 32768;        /* drop 0..13 low bits */
    int hold = 1 + (int32_t)p * 16 / 32768;     /* keep each sample 1..16 times */
    int i;
    if (EP_ASM) { ep_crush_bf(v, buf, n, ~((1 << shift) - 1), hold); return; }

    for (i = 0; i < n; i++) {
        if (v->count == 0) {
            v->hold = (int16_t)(buf[i] & ~((1 << shift) - 1));   /* (x >> s) << s without a negative shift */
            v->count = (uint8_t)hold;
        }
        v->count--;
        buf[i] = v->hold;
    }
}

#ifdef EP_PROF                                          /* profiling build: cycles per stage */
#include "../op1_synth.h"
volatile uint32_t ep_stage_cy[3];
#define ST_T0()    uint32_t t0 = op1_cycles()
#define ST_T1(i)   ep_stage_cy[i] += op1_cycles() - t0
#else
#define ST_T0()
#define ST_T1(i)
#endif
#ifdef EP_CHECK                                         /* check build: C and assembly on the same input */
static void ep_process_(ep_voice *v, int16_t *buf, int n, const int16_t *p);
volatile uint32_t ep_check[2];                          /* blocks compared, mismatches */
void ep_process(ep_voice *v, int16_t *buf, int n, const int16_t *p)
{
    int16_t c[128]; ep_voice cv = *v;
    for (int i = 0; i < n && i < 128; i++) c[i] = buf[i];
    ep_asm = 0; ep_process_(&cv, c, n, p);
    ep_asm = 1; ep_process_(v, buf, n, p);
    int bad = 0;
    for (int i = 0; i < n && i < 128; i++) bad |= c[i] != buf[i];
    for (int i = 0; i < 4; i++) bad |= cv.y[i] != v->y[i];
    bad |= cv.hold != v->hold || cv.count != v->count;
    ep_check[0]++; ep_check[1] += bad;
}
static void ep_process_(ep_voice *v, int16_t *buf, int n, const int16_t *p)
#else
void ep_process(ep_voice *v, int16_t *buf, int n, const int16_t *p)
#endif
{
    { ST_T0();
    if (p[EP_DRIVE] > 0)
        drive(buf, n, p[EP_DRIVE]);
    ST_T1(0); }
    { ST_T0();
    if (p[EP_FILTER] > 0 || p[EP_RESO] > 0)
        filter(v, buf, n, p[EP_FILTER] > 0 ? p[EP_FILTER] : 0, p[EP_RESO] > 0 ? p[EP_RESO] : 0);
    else
        v->y[0] = v->y[1] = v->y[2] = v->y[3] = 0;
    ST_T1(1); }
    { ST_T0();
    if (p[EP_CRUSH] > 0)
        crush(v, buf, n, p[EP_CRUSH]);
    else
        v->count = 0;
    ST_T1(2); }
}
