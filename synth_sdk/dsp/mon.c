/* Master monitor (docs/81 stage 3b-2): the loop at 0xFFA013A0 in R246's Core_mix_master, when [0xFF8010B0] == 1
 * (sampling from master), as a bit-exact model: per sample v = sat16(mR(L.H, m) >>> 1 + mR(R.H, m) >>> 1) (master
 * int32 0xFF800E34 / F34, m = W[0x01B4EF6E], mR rounded), v -> the bus 0xFF800BB4, peak W[0xFF801084] = max(peak,
 * |v|); then, if [0xFF8010AC] (the sample buffer), CALL 0x01A3464A. OP1R_MON=check|stress: mon_pre after the album
 * call (0xFFA01372 -> l1v_mon_pre), mon_post before the volume call (cw_pre stub); check builds switch the monitor on
 * (no sample buffer), stress randomises master, m and peak. */
#include "../op1_synth.h"

static inline int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static inline int16_t mR(int16_t a, int16_t b)
{
    int32_t p = (a == -32768 && b == -32768) ? 0x7FFFFFFF : 2 * (int32_t)a * b, q = p >> 16, r = p & 0xFFFF;
    if (r > 0x8000 || (r == 0x8000 && (q & 1))) q++;
    return sat16(q);
}
static inline int16_t abs16(int16_t v) { return v == -32768 ? 32767 : (int16_t)(v < 0 ? -v : v); }

#define ON   (*(volatile int32_t *)0xFF8010B0)
#define BUF  (*(volatile int32_t *)0xFF8010AC)
#define M    (*(volatile int16_t *)0x01B4EF6E)
#define PEAK (*(volatile int16_t *)0xFF801084)
#define ML   ((volatile int32_t *)0xFF800E34)
#define MR   ((volatile int32_t *)0xFF800F34)
#define BUS  ((volatile int16_t *)0xFF800BB4)

int16_t mon_model(int16_t *y, int16_t peak)
{
    const int16_t m = M;
    for (int i = 0; i < 64; i++) {
        const int16_t v = sat16((mR((int16_t)(ML[i] >> 16), m) >> 1) + (mR((int16_t)(MR[i] >> 16), m) >> 1));
        y[i] = v;
        if (abs16(v) > peak) peak = abs16(v);
    }
    return peak;
}

#ifdef MON_CHECK
struct { uint32_t blocks, bad, first_i, first_orig, first_model, off, unmatched, pad; } mon_check;
static int16_t sh[64], sh_peak;
static int pending;
#ifdef MON_STRESS
static uint32_t ms = 0xBADC0DE;
static uint32_t mrnd(void) { ms ^= ms << 13; ms ^= ms >> 17; ms ^= ms << 5; return ms; }
static int32_t mval(void) { uint32_t r = mrnd(); switch (r >> 29) { case 0: return 0x7FFFFFFF; case 1: return (int32_t)0x80000000; case 2: return 0; default: return (int32_t)(r * 2654435761u); } }
#endif
void mon_pre(void)                                          /* after the album call, before the monitor */
{
    ON = 1; BUF = 0;                                        /* check builds (emulator): the monitor on, no sampling */
#ifdef MON_STRESS
    for (int i = 0; i < 64; i++) { ML[i] = mval(); MR[i] = mval(); }
    M = (int16_t)mval(); PEAK = (int16_t)(mrnd() & 0x7FFF);
#endif
    sh_peak = mon_model(sh, PEAK);
    pending = 1;
}
void mon_post(void)                                         /* before the volume call */
{
    if (!pending) { mon_check.unmatched++; return; }
    pending = 0;
    int bad = -1;
    for (int i = 0; i < 64 && bad < 0; i++) if (BUS[i] != sh[i]) bad = i;
    if (bad < 0 && PEAK != sh_peak) bad = 64;
    mon_check.blocks++;
    if (bad >= 0) {
        if (!mon_check.bad) { mon_check.first_i = (uint32_t)bad;
            mon_check.first_orig = (uint16_t)(bad < 64 ? BUS[bad] : PEAK); mon_check.first_model = (uint16_t)(bad < 64 ? sh[bad] : sh_peak); }
        mon_check.bad++;
    }
}
#else
void mon_pre(void) {}
void mon_post(void) {}
#endif
