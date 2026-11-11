/* Our master section (docs/81 stage 3b-3): in place of R246's Core_mix_master (L1 0xFFA00E50, 2 460 bytes; called
 * from core_block_process at 0xFFA02116 / 0xFFA02DB2 with R0 = the codec buffer or 0), the same calls in the same
 * order with the same side effects (map in docs/81); the loops are ours (dsp/mix_l1.S, dsp/master_loops.S), each
 * bit-exact with its model. Entered through l1_master (hook.S: keeps the caller's M1-M3 and LC1 / LT1 / LB1).
 * Compiled -Os -mlong-calls into .l1master. OP1R_MASTER=check|stress: every loop against its model per block. */
#include "../op1_synth.h"

#define L1M __attribute__((section(".l1master.1")))
#define B8(a)  (*(volatile uint8_t *)(a))
#define W16(a) (*(volatile int16_t *)(a))
#define D32(a) (*(volatile int32_t *)(a))
#define FN(t, a) ((t)(a))

void mm_mix(void);
void mm_cw(int32_t *out, int sync);
void mm_spk(int32_t *out);
void mm_mon(void);
void ci_metro_render(void);

static inline __attribute__((always_inline)) void cyc(volatile uint32_t *p)
{
    uint32_t lo, hi;
    __asm__ volatile ("cli r3; %0 = CYCLES; %1 = CYCLES2; sti r3;" : "=d"(lo), "=d"(hi) : : "R3");
    p[0] = lo; p[1] = hi;
}
/* R246's stage timer record (0x2C bytes): start, last (end - start - 8), max, min, sum (64-bit), count */
static inline __attribute__((always_inline)) void rec_end(uint32_t a)
{
    volatile uint32_t *r = (volatile uint32_t *)a;
    cyc(r + 2);
    const uint64_t d = ((uint64_t)r[3] << 32 | r[2]) - ((uint64_t)r[1] << 32 | r[0]) - 8;
    r[2] = (uint32_t)d; r[3] = (uint32_t)(d >> 32);
    if (((uint64_t)r[5] << 32 | r[4]) < d) { r[4] = (uint32_t)d; r[5] = (uint32_t)(d >> 32); }
    if (d < ((uint64_t)r[7] << 32 | r[6])) { r[6] = (uint32_t)d; r[7] = (uint32_t)(d >> 32); }
    const uint64_t s = ((uint64_t)r[9] << 32 | r[8]) + d;
    r[8] = (uint32_t)s; r[9] = (uint32_t)(s >> 32);
    r[10]++;
}
#define REC(i) (0x01BA2618u + 0x2Cu * (i))
static inline __attribute__((always_inline)) int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static inline __attribute__((always_inline)) int16_t mR(int16_t a, int16_t b)
{
    int32_t r;
    __asm__ ("%0.L = %1.L * %2.L;" : "=d"(r) : "d"(a), "d"(b));
    return (int16_t)r;
}

#ifdef MASTER_CHECK
#include "master_check.h"
#else
#define CK_MIX_PRE()
#define CK_MIX_POST()
#define CK_MON_PRE()
#define CK_MON_POST()
#define CK_CW_PRE(o, f)
#define CK_CW_POST(o)
#endif

L1M void master_section(int32_t *out)
{
    CK_MIX_PRE();
    mm_mix();                                                /* instrument bus into master L / R */
    CK_MIX_POST();
    if (B8(0xFF80103C)) {                                    /* slot meter */
        volatile int16_t *m = (volatile int16_t *)0xFF802990 + D32(0xFF802964);
        *m = sat16(*m + W16(0xFF801084));
    }
    cyc((volatile uint32_t *)REC(1));
    ci_metro_render();
    rec_end(REC(1));
    cyc((volatile uint32_t *)REC(9));
    FN(void (*)(void *), 0xFFA030F0)(FN(void *(*)(void), 0x010004CC)());   /* master EQ (state from the getter) */
    rec_end(REC(9));
    cyc((volatile uint32_t *)REC(10));
    {
        const int32_t obj = D32(0x01AAAD68);
        if (obj && B8(0x01AAAC40)) {                         /* master FX (on): vtable + 0x14 */
            const int32_t vt = *(volatile int32_t *)obj;
            FN(void (*)(int32_t, void *, void *, void *), *(volatile int32_t *)(vt + 0x14))
                (obj + *(volatile int16_t *)(vt + 0x10), (void *)0xFF800D34, (void *)0xFF800DB4, (void *)0xFF8016C4);
        }
    }
    rec_end(REC(10));
    cyc((volatile uint32_t *)REC(11));
    int sync = 0;
    if (FN(uint8_t (*)(void), 0x019DE53C)() && B8(0x018B621D)) {   /* PO sync on and a cable */
        const int32_t mode = D32(0x018C17D4);
        int32_t lim = (mode == 4 || mode == 5 || mode == 8 || D32(0xFF802960) == 2) ? 0x7FFFFFFF : 0;
        void *st = FN(void *(*)(void), 0x019D90E8)();
        FN(void (*)(void *, int32_t *), 0x019D90F2)(st, &lim);
        sync = 1;
    } else
        FN(void (*)(void *), 0xFFA04280)(FN(void *(*)(void), 0x019D90E8)());   /* output stage (ours) */
    FN(void (*)(void), 0x01915608)();                        /* album */
    if (D32(0xFF8010B0) == 1) {                              /* master monitor (sampling from master) */
        CK_MON_PRE();
        mm_mon();
        CK_MON_POST();
        const int32_t buf = D32(0xFF8010AC);
        if (buf) FN(void (*)(int32_t, int32_t, void *), 0x01A3464A)(buf, 0, (void *)0xFF800BB4);
    }
    if (out) {
        FN(void (*)(void), 0x01A10DE4)();                    /* volume */
#ifdef MASTER_STRESS
        sync = (int)(ck_rnd() & 1);                          /* stress: either loop variant */
#endif
        CK_CW_PRE(out, sync);
        if (!B8(0x018B621D)) mm_spk(out); else mm_cw(out, sync);
        CK_CW_POST(out);
    }
    rec_end(REC(11));
    int16_t v;
    volatile int16_t *vu = (volatile int16_t *)0xFF80108A;
    if (!sync) {
        FN(void (*)(int16_t *), 0x019D941E)(&v);
        if (v > *vu) *vu = v;
    }
    FN(void (*)(int16_t *), 0x019D942E)(&v);
    *vu = mR(v > *vu ? v : *vu, 0x7F00);
}
