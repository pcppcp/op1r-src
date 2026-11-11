/* Instrument bus DC blocker (docs/81 stage 5): R246's 0x019674F8 (SDRAM, called after the decimator at 0xFFA01EB8,
 * 0xFFA0245A, 0xFFA0288C) as a bit-exact model. From the disassembly: input x = 64 int16 at 0xFF801E50 (the
 * decimator's output), output 64 int16 at 0xFF800BB4; state s = 0x01AAAC14 { X', q', y' } (int32), c = [0x01BAB53C].
 * Per sample, X = x << 16: y = sat(sat(sat(y' - X') + X) - mul32(c, q')), q = y & 0xFFFF0000, out = y.H;
 * mul32 = ADI's mult_fr1x32x32 sequence (lo x lo unsigned >> 16, the two mixed cross products, >>> 15, + hi x hi
 * fractional, saturated). The entry is already op1r's input
 * engine hook (input/in_host.c in_bus); without an input engine it calls dc_run: the original (in_highpass_stock), or
 * with OP1R_DC=check|stress the original and the model from the same state, compared (dsp/stagecheck.h). */
#include "../op1_synth.h"
#include "stagecheck.h"

void in_highpass_stock(void);                                /* hook.S: R246's routine */

#define DCX  ((volatile int16_t *)0xFF801E50)
#define DCY  ((volatile int16_t *)0xFF800BB4)
#define DCS  ((volatile int32_t *)0x01AAAC14)
#define DCC  (*(volatile int32_t *)0x01BAB53C)

static inline int32_t sat32(int64_t v) { return v > 0x7FFFFFFFLL ? 0x7FFFFFFF : v < -0x80000000LL ? (int32_t)0x80000000 : (int32_t)v; }
static inline int32_t mul32(int32_t a, int32_t b)
{
    const int64_t al = (uint16_t)a, bl = (uint16_t)b, ah = (int16_t)(a >> 16), bh = (int16_t)(b >> 16);
    int64_t a1 = (int64_t)((uint64_t)(al * bl) >> 16);
    a1 += ah * bl + bh * al;
    a1 >>= 15;
    const int64_t a0 = (ah == -32768 && bh == -32768) ? 0x7FFFFFFF : 2 * ah * bh;
    return sat32(a0 + a1);
}

void dc_model(void)
{
    const int32_t c = DCC;
    int32_t Xp = DCS[0], q = DCS[1], y = DCS[2];
    for (int i = 0; i < 64; i++) {
        const int32_t X = (int32_t)((uint32_t)(uint16_t)DCX[i] << 16);
        y = sat32((int64_t)sat32((int64_t)sat32((int64_t)y - Xp) + X) - mul32(c, q));
        q = (int32_t)((uint32_t)y & 0xFFFF0000u);
        DCY[i] = (int16_t)(y >> 16);
        Xp = X;
    }
    DCS[0] = Xp; DCS[1] = q; DCS[2] = y;
}

#ifdef DC_CHECK
volatile stage_check_t dc_check;
#ifdef DC_STRESS
static uint32_t cs = 0x1234567;
static int32_t crnd(void) { cs ^= cs << 13; cs ^= cs >> 17; cs ^= cs << 5;
    switch (cs >> 29) { case 0: return 0x7FFFFFFF; case 1: return (int32_t)0x80000000; case 2: return 0; default: return (int32_t)(cs * 2654435761u); } }
#endif
void dc_stage(void)
{
#ifdef DC_STRESS                                            /* emulator only: random input, state, coefficient */
    for (int i = 0; i < 64; i++) DCX[i] = (int16_t)crnd();
    for (int i = 0; i < 3; i++) DCS[i] = crnd();
    DCS[1] &= (int32_t)0xFFFF0000;                          /* q' as the routine leaves it */
    DCC = crnd();
#endif
    static const stage_region r[] = { { (void *)0xFF800BB4, 128 }, { (void *)0x01AAAC14, 12 } };
    stage_check(&dc_check, r, 2, in_highpass_stock, dc_model);
}
#endif
void dc_run(void)
{
#ifdef DC_CHECK
    dc_stage();
#else
    in_highpass_stock();
#endif
}
