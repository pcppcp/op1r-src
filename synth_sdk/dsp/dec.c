/* Instrument bus decimator (docs/81 stage 4): R246's synth_bus_decimate (L1 0xFFA005D4, called at 0xFFA01EB4,
 * 0xFFA02456, 0xFFA02888, no arguments) as a bit-exact model. From the disassembly: input x = 128 int16 at
 * 0xFF800BB4 (88.2 kHz), output y = 64 int16 at 0xFF801E50; two 32-word delay lines (L = 64 bytes), even samples
 * A 0xFF80123C, odd samples B 0xFF80127C, written from index 0 every block (circular, 64 per block: the index is fixed);
 * g / s = W[0xFF80107C] / W[0xFF80107E] (gain, step). Per output i:
 *   B[i & 31] = x[2i + 1]; A[i & 31] = x[2i]; acc = B[(i + 16) & 31] << 15 (M0 = 30 bytes);
 *   acc = sat32(acc + 2 A[(i + 1 + n) & 31] h[n]) for n = 0..31 (h: int16 0x0106A6F0);
 *   y[i] = (acc.H g) rounded; g = sat16(g + s); after the block W[0xFF80107C] = g.
 * OP1R_DEC=check|stress: the trampoline l1_tramp_dec calls dec_stage, which runs the original and the model from the
 * same state (dsp/stagecheck.h); what plays is the original. */
#include "../op1_synth.h"
#include "stagecheck.h"

#define DEC_ORIG ((void (*)(void))0xFFA005D4)
#define DX   ((volatile int16_t *)0xFF800BB4)
#define DY   ((volatile int16_t *)0xFF801E50)
#define DA   ((volatile int16_t *)0xFF80123C)
#define DB   ((volatile int16_t *)0xFF80127C)
#define DG   ((volatile int16_t *)0xFF80107C)
#define DH   ((const volatile int16_t *)0x0106A6F0)

static inline int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static inline int32_t sat32(int64_t v) { return v > 0x7FFFFFFFLL ? 0x7FFFFFFF : v < -0x80000000LL ? (int32_t)0x80000000 : (int32_t)v; }
static inline int16_t mulh16(int16_t a, int16_t b)
{
    int32_t p = (a == -32768 && b == -32768) ? 0x7FFFFFFF : 2 * (int32_t)a * b;
    int32_t q = p >> 16, r = p & 0xFFFF;
    if (r > 0x8000 || (r == 0x8000 && (q & 1))) q++;
    return sat16(q);
}

void dec_model(void)
{
    int16_t g = DG[0];
    const int16_t s = DG[1];
    for (int i = 0; i < 64; i++) {
        DB[i & 31] = DX[2 * i + 1];
        DA[i & 31] = DX[2 * i];
        int32_t acc = (int32_t)DB[(i + 16) & 31] << 15;
        for (int n = 0; n < 32; n++) {
            const int16_t a = DA[(i + 1 + n) & 31], h = DH[n];
            acc = sat32((int64_t)acc + ((a == -32768 && h == -32768) ? 0x7FFFFFFF : 2 * (int32_t)a * h));
        }
        DY[i] = mulh16((int16_t)(acc >> 16), g);
        g = sat16(g + s);
    }
    DG[0] = g;
}

#ifdef DEC_CHECK
volatile stage_check_t dec_check;
#ifdef DEC_STRESS
static uint32_t ds = 0x9E3779B9;
static int16_t drnd(void) { ds ^= ds << 13; ds ^= ds >> 17; ds ^= ds << 5;
    switch (ds >> 29) { case 0: return 32767; case 1: return -32768; case 2: return 0; default: return (int16_t)(ds >> 7); } }
#endif
void dec_stage(void)
{
#ifdef DEC_STRESS                                           /* emulator only: random input, delay lines, gain / step */
    for (int i = 0; i < 128; i++) DX[i] = drnd();
    if (!(ds & 7)) for (int i = 0; i < 32; i++) { DA[i] = drnd(); DB[i] = drnd(); }
    DG[0] = drnd(); DG[1] = drnd();
#endif
    static const stage_region r[] = { { (void *)0xFF80123C, 128 }, { (void *)0xFF801E50, 128 }, { (void *)0xFF80107C, 4 } };
    stage_check(&dec_check, r, 3, DEC_ORIG, dec_model);
}
#else
void dec_stage(void) { void (*volatile f)(void) = DEC_ORIG; f(); }
#endif
