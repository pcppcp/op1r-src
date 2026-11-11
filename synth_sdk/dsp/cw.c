/* Codec write (docs/81 stage 3b-1): the stereo loops at the end of R246's Core_mix_master (0xFFA015F0 normal,
 * 0xFFA0142A PO sync variant), as a bit-exact model. Master L / R int32 0xFF800E34 / F34 -> the codec buffer (the
 * function's argument, 128 int32 interleaved L R), volume ramp cur W[0x01DE4F70] += step W[0x01DE4F72] (saturating)
 * per sample, g = cur^2 (rounded). With mT(a, b) = (2ab) >> 16 (truncated) and f(a, b) = sat32(2ab):
 *   normal: out = sat(sat(f(g, x.H) + 2 mT(g, x.L)) + 0x400000) for L and R;
 *   PO sync: L the same with g = 0x7FFF (unity: the sync pulse channel), R with g.
 * x.L is the low half taken as signed, as R246 does. OP1R_CW=check: the volume call before the loops (0xFFA013FA)
 * goes to cw_pre (the model into a shadow), the VU getter after them (0xFFA015A8 / D8) to cw_post (compare). */
#include "../op1_synth.h"

static inline int32_t sat32(int64_t v) { return v > 0x7FFFFFFFLL ? 0x7FFFFFFF : v < -0x80000000LL ? (int32_t)0x80000000 : (int32_t)v; }
static inline int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static inline int32_t f32(int16_t a, int16_t b) { return (a == -32768 && b == -32768) ? 0x7FFFFFFF : 2 * (int32_t)a * b; }
static inline int16_t mT(int16_t a, int16_t b) { return (int16_t)(f32(a, b) >> 16); }
static inline int16_t mR(int16_t a, int16_t b)
{
    int32_t p = f32(a, b), q = p >> 16, r = p & 0xFFFF;
    if (r > 0x8000 || (r == 0x8000 && (q & 1))) q++;
    return sat16(q);
}
static inline int32_t gain(int16_t g, int32_t x)
{
    return sat32((int64_t)sat32((int64_t)f32(g, (int16_t)(x >> 16)) + 2 * (int32_t)mT(g, (int16_t)x)) + 0x400000);
}

#define CUR  (*(volatile int16_t *)0x01DE4F70)
#define STEP (*(volatile int16_t *)0x01DE4F72)
#define ML   ((const volatile int32_t *)0xFF800E34)
#define MR   ((const volatile int32_t *)0xFF800F34)

/* out: 128 int32; returns the new cur */
int16_t cw_model(int32_t *out, int16_t cur, int16_t step, int sync)
{
    for (int i = 0; i < 64; i++) {
        cur = sat16(cur + step);
        const int16_t g = mR(cur, cur);
        out[2 * i] = gain(sync ? 0x7FFF : g, ML[i]);
        out[2 * i + 1] = gain(g, MR[i]);
    }
    return cur;
}

/* speaker (B[0x018B621D] == 0, 0xFFA01698): x = sat(L/2 + R/2), v = mul32(c, sat(sat(x + v') - x')) (a high-pass,
 * c = 0x7BB8CB8E, state 0x01AAAC50 { v', x' }), out[2i] = mul32(cur^2 << 16, v) (one channel, the odd words untouched) */
static inline int32_t mul32(int32_t a, int32_t b)
{
    const int64_t al = (uint16_t)a, bl = (uint16_t)b, ah = (int16_t)(a >> 16), bh = (int16_t)(b >> 16);
    int64_t a1 = (int64_t)((uint64_t)(al * bl) >> 16);
    a1 += ah * bl + bh * al;
    a1 >>= 15;
    const int64_t a0 = (ah == -32768 && bh == -32768) ? 0x7FFFFFFF : 2 * ah * bh;
    return sat32(a0 + a1);
}
#define SPK  ((volatile int32_t *)0x01AAAC50)
int16_t spk_model(int32_t *out, int16_t cur, int16_t step, int32_t *st)
{
    int32_t v = st[0], xp = st[1];
    for (int i = 0; i < 64; i++) {
        cur = sat16(cur + step);
        const int32_t x = sat32((int64_t)(ML[i] >> 1) + (MR[i] >> 1));
        v = mul32(0x7BB8CB8E, sat32((int64_t)sat32((int64_t)x + v) - xp));
        xp = x;
        out[2 * i] = mul32((int32_t)((uint32_t)(uint16_t)mR(cur, cur) << 16), v);
    }
    st[0] = v; st[1] = xp;
    return cur;
}

#ifdef CW_CHECK
struct { uint32_t blocks, bad, first_i, first_orig, first_model, sync, nobuf, unmatched; } cw_check;   /* unmatched: the normal path calls both VU getters */
static int32_t sh[128];
static int16_t sh_cur;
static int sh_spk;
static int32_t sh_st[2];
static volatile int32_t *sh_out;
#ifdef CW_STRESS
static uint32_t ws = 0xC0FFEE;
static uint32_t wrnd(void) { ws ^= ws << 13; ws ^= ws >> 17; ws ^= ws << 5; return ws; }
static int32_t wval(void) { uint32_t r = wrnd(); switch (r >> 29) { case 0: return 0x7FFFFFFF; case 1: return (int32_t)0x80000000; case 2: return 0; default: return (int32_t)(r * 2654435761u); } }
#endif
int cw_pre(volatile int32_t *out, int sync)                 /* after R246's volume call, before its loops; returns the flag */
{
#ifdef CW_STRESS                                            /* emulator only: random master, ramp, step, variant */
    for (int i = 0; i < 64; i++) { ((volatile int32_t *)ML)[i] = wval(); ((volatile int32_t *)MR)[i] = wval(); }
    CUR = (int16_t)wval(); STEP = (int16_t)(wrnd() & 1 ? wval() : (int32_t)(wrnd() % 13) - 6);
    sync = (int)(wrnd() & 1);
#endif
#ifndef CW_SPK
    *(volatile uint8_t *)0x018B621D = 1;                    /* check builds (op1emu has no jack): headphones, R246 reads it next */
#endif
    sh_spk = !*(volatile uint8_t *)0x018B621D;    sh_out = out;
    if (!out) { cw_check.nobuf++; return sync; }
    if (sh_spk) {
        for (int i = 0; i < 64; i++) sh[2 * i + 1] = out[2 * i + 1];
        sh_st[0] = SPK[0]; sh_st[1] = SPK[1];
        sh_cur = spk_model(sh, CUR, STEP, sh_st);
    } else
        sh_cur = cw_model(sh, CUR, STEP, sync);
    cw_check.sync += sync != 0;
    cw_check.nobuf += (uint32_t)sh_spk << 16;   /* speaker blocks in the high half */
    return sync;
}
void cw_post(void)                                          /* the VU getter after the loops */
{
    volatile int32_t *o = sh_out;
    if (!o) { cw_check.unmatched++; return; }
    sh_out = 0;
    int bad = -1;
    for (int i = 0; i < 128 && bad < 0; i++) if (o[i] != sh[i]) bad = i;
    if (bad < 0 && CUR != sh_cur) bad = 128;
    if (bad < 0 && sh_spk && (SPK[0] != sh_st[0] || SPK[1] != sh_st[1])) bad = 129;
    cw_check.blocks++;
    if (bad >= 0) {
        if (!cw_check.bad) { cw_check.first_i = (uint32_t)bad;
            cw_check.first_orig = bad < 128 ? (uint32_t)o[bad] : (uint16_t)CUR; cw_check.first_model = bad < 128 ? (uint32_t)sh[bad] : (uint16_t)sh_cur; }
        cw_check.bad++;
    }
}
#else
int cw_pre(volatile int32_t *out, int sync) { (void)out; return sync; }
void cw_post(void) {}
#endif
