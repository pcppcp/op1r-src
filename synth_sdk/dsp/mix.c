/* Instrument bus into master L / R (docs/81 stage 3a): the two loops at the start of R246's Core_mix_master
 * (0xFFA00E58..0xFFA00FDC), as a bit-exact model. From the disassembly:
 *   idx = [0xFF802964], flag = B[0xFF80103C], vol / pan / lev = W[0xFF801928 / 0xFF801930 / 0xFF8018B4 + 2 idx],
 *   m = W[0x01B4EF6E]; a = sat(0x4000 - pan) << 1 (s), b = sat(pan + 0x4000) << 1 (s), vl = vol lev,
 *   gL = m (vl a), gR = m (vl b) (16-bit products, rounded ties to even); peak W[0xFF801084] = 0;
 *   ramp r1 = W[0xFF801082] steps by s1 per sample: flag ? -0x80 : (r1 < 0 ? 0x80 : 0);
 *   loop 1: L += (x gL) r1, R += (x gR) r1 (saturating), peak = max |x m|;
 *   ramp r2 = W[0xFF801080] steps by s2: flag ? (r2 < 0 ? 0x80 : 0) : -0x80;
 *   loop 2: v = (x m) r2, L += v, R += v; after: flag -> W[0xFF802990 + 2 idx] += peak (saturating).
 * x = the instrument bus 0xFF800BB4, L / R = 0xFF800D34 / DB4 (tape already in), all int16.
 * OP1R_MIX=check: mix_pre (the calls of Core_mix_master, trampoline l1_tramp_mix) computes the model into shadow
 * buffers; mix_post (our metronome hook ci_metro_render, the next routine after the loops) compares it with what
 * R246's loops wrote. What plays is R246's. */
#include "../op1_synth.h"

static inline int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static inline int16_t mulh16(int16_t a, int16_t b)          /* rounded, ties to even (as R.L = a * b) */
{
    int32_t p = (a == -32768 && b == -32768) ? 0x7FFFFFFF : 2 * (int32_t)a * b;
    int32_t q = p >> 16, r = p & 0xFFFF;
    if (r > 0x8000 || (r == 0x8000 && (q & 1))) q++;
    return sat16(q);
}
static inline int16_t abs16(int16_t v) { return v == -32768 ? 32767 : (int16_t)(v < 0 ? -v : v); }

#define IDX   (*(volatile int32_t *)0xFF802964)
#define FLAG  (*(volatile int8_t *)0xFF80103C)
#define BUS   ((const volatile int16_t *)0xFF800BB4)
#define RAMP  ((volatile int16_t *)0xFF801080)          /* [0] r2, [1] r1, [2] peak */
#define MET   ((volatile int16_t *)0xFF802990)

/* the model on given buffers (L, R: 64 int16 each; ramp: r2, r1, peak; met: the slot meter word or 0) */
void mix_model(int16_t *L, int16_t *R, int16_t *ramp, int16_t *met)
{
    const int idx = IDX, flag = FLAG != 0;
    const int16_t vol = ((volatile int16_t *)0xFF801928)[idx], pan = ((volatile int16_t *)0xFF801930)[idx];
    const int16_t lev = ((volatile int16_t *)0xFF8018B4)[idx], m = *(volatile int16_t *)0x01B4EF6E;
    const int16_t a = sat16((int32_t)sat16(0x4000 - pan) << 1), b = sat16((int32_t)sat16(pan + 0x4000) << 1);
    const int16_t vl = mulh16(vol, lev);
    const int16_t gL = mulh16(m, mulh16(vl, a)), gR = mulh16(m, mulh16(vl, b));
    ramp[2] = 0;
    const int16_t s1 = flag ? -0x80 : (ramp[1] < 0 ? 0x80 : 0);
    for (int i = 0; i < 64; i++) {
        const int16_t x = BUS[i], r1 = ramp[1];
        const int16_t pk = abs16(mulh16(x, m));
        if (pk > ramp[2]) ramp[2] = pk;
        L[i] = sat16(L[i] + mulh16(mulh16(x, gL), r1));
        R[i] = sat16(R[i] + mulh16(mulh16(x, gR), r1));
        ramp[1] = sat16(r1 + s1);
    }
    const int16_t s2 = flag ? (ramp[0] < 0 ? 0x80 : 0) : -0x80;
    for (int i = 0; i < 64; i++) {
        const int16_t v = mulh16(mulh16(BUS[i], m), ramp[0]);
        L[i] = sat16(v + L[i]);
        R[i] = sat16(v + R[i]);
        ramp[0] = sat16(ramp[0] + s2);
    }
    if (flag && met) *met = sat16(*met + ramp[2]);
}

#ifdef MIX_CHECK
struct { uint32_t blocks, bad, first_what, first_i, first_orig, first_model, pending, unmatched, sound, flagged, ramping, panned; } mix_check;
static int16_t shL[64], shR[64], shRamp[3], shMet;
static int sh_idx, sh_flag;
#ifdef MIX_STRESS
static uint32_t rs = 0x2545F491;
static int16_t rnd16(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    switch (rs >> 29) { case 0: return 32767; case 1: return -32768; case 2: return 0; default: return (int16_t)(rs >> 7); } }
#endif
void mix_pre(void)                                          /* the call of Core_mix_master: before its loops */
{
#ifdef MIX_STRESS                                           /* emulator only: random inputs, R246's loops run on them */
    { const int ix = IDX;
      for (int i = 0; i < 64; i++) { ((volatile int16_t *)0xFF800BB4)[i] = rnd16();
          ((volatile int16_t *)0xFF800D34)[i] = rnd16(); ((volatile int16_t *)0xFF800DB4)[i] = rnd16(); }
      ((volatile int16_t *)0xFF801928)[ix] = rnd16(); ((volatile int16_t *)0xFF801930)[ix] = rnd16();
      ((volatile int16_t *)0xFF8018B4)[ix] = rnd16(); *(volatile int16_t *)0x01B4EF6E = rnd16();
      RAMP[0] = rnd16(); RAMP[1] = rnd16(); MET[ix] = rnd16(); FLAG = (int8_t)(rnd16() & 1); }
#endif
    for (int i = 0; i < 64; i++) { shL[i] = ((volatile int16_t *)0xFF800D34)[i]; shR[i] = ((volatile int16_t *)0xFF800DB4)[i]; }
    for (int i = 0; i < 3; i++) shRamp[i] = RAMP[i];
    sh_idx = IDX; sh_flag = FLAG != 0; shMet = MET[sh_idx];
    { int snd = 0; for (int i = 0; i < 64; i++) snd |= BUS[i];
      mix_check.sound += snd != 0; mix_check.flagged += sh_flag;
      mix_check.ramping += shRamp[0] > -0x8000 && shRamp[0] < 0x7FFF && shRamp[1] > -0x8000 && shRamp[1] < 0x7FFF;
      mix_check.panned += snd && ((volatile int16_t *)0xFF801930)[sh_idx] != 0; }
    mix_model(shL, shR, shRamp, &shMet);
    mix_check.pending = 1;
}
void mix_post(void)                                         /* the metronome hook: right after the loops */
{
    if (!mix_check.pending) { mix_check.unmatched++; return; }
    mix_check.pending = 0;
    int bad = -1, at = 0; int16_t o = 0, md = 0;
    for (int i = 0; i < 64 && bad < 0; i++) {
        if (((volatile int16_t *)0xFF800D34)[i] != shL[i]) { bad = 0; at = i; o = ((volatile int16_t *)0xFF800D34)[i]; md = shL[i]; }
        else if (((volatile int16_t *)0xFF800DB4)[i] != shR[i]) { bad = 1; at = i; o = ((volatile int16_t *)0xFF800DB4)[i]; md = shR[i]; }
    }
    for (int i = 0; i < 3 && bad < 0; i++) if (RAMP[i] != shRamp[i]) { bad = 2; at = i; o = RAMP[i]; md = shRamp[i]; }
    if (bad < 0 && MET[sh_idx] != shMet) { bad = 3; at = sh_idx; o = MET[sh_idx]; md = shMet; }
    mix_check.blocks++;
    if (bad >= 0) {
        if (!mix_check.bad) { mix_check.first_what = (uint32_t)bad; mix_check.first_i = (uint32_t)at;
                              mix_check.first_orig = (uint16_t)o; mix_check.first_model = (uint16_t)md; }
        mix_check.bad++;
    }
}
#else
void mix_pre(void) {}
void mix_post(void) {}
#endif
