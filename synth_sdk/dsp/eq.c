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

#ifndef EQ_FIR_ST
#define EQ_FIR_ST
typedef struct { const int16_t *h; int16_t *d; int16_t *p; int32_t k; } fir_st;
#endif
#if defined(__bfin__)
void eq_fir(fir_st *s, const volatile int16_t *x, int16_t *y, const int32_t *hp);   /* dsp/fir.S */
#define EQ_FIR eq_fir
void eq_mid(volatile int16_t *x, volatile int16_t *dly, int32_t *di);
int32_t eq_onepole(int16_t *b, int32_t c, int32_t s);
void eq_mix(volatile int16_t *x, int32_t ab, int32_t c);
#define EQ_MID eq_mid
#define EQ_OP eq_onepole
#define EQ_MIX eq_mix
#endif
#include "eq_model.h"

/* the FIR's coefficient pairs (dsp/fir.S): hp[m] = (h[m] low, h[m + 1] high), the last high half 0; R246's two filters
 * have fixed coefficients in L1 (lo 0xFF800004, hi 0xFF80006C, 51 taps), so the tables are built once (boot) */
int32_t eq_hp[2][52] __attribute__((aligned(4)));
void eq_fir_pairs(void)
{
    for (int f = 0; f < 2; f++) {
        const volatile int16_t *h = (const volatile int16_t *)(f ? 0xFF80006C : 0xFF800004);
        for (int m = 0; m < 51; m++)
            eq_hp[f][m] = (int32_t)(uint16_t)h[m] | (int32_t)(m < 50 ? (uint32_t)(uint16_t)h[m + 1] << 16 : 0);
        eq_hp[f][51] = 0;
    }
}

static int32_t *eq_arg;
static void eq_mod(void) { eq_model(eq_arg); }
#ifdef EQ_CHECK
volatile stage_check_t eq_check;
static void eq_orig(void) { void (*volatile f)(void *) = MASTER_EQ; f(eq_arg); }
#endif

/* Boot self-test of the L1 copy (production builds): R246's routine is gone, so the compiled bytes at 0xFFA030F0
 * (dsp/eq_l1.c + fir_l1.S) are compared with the model in SDRAM (eq.c + fir.S) on 48 synthetic blocks (noise, extremes,
 * silence, random settings, states), every byte they write, from identical starting points. The FIR states at
 * 0x01AABAD8 are not set up yet at boot (db stage): temporary ones on our own delay lines, everything put back after.
 * eq_selftest: blocks, differing, first region, 1 = ran (page 9 shows "eqt .." only on a failure). */
volatile uint32_t eq_selftest[4];
static void eq_l1_call(void) { void (*volatile f)(void *) = MASTER_EQ; f(eq_arg); }
void eq_stage_selftest(void)
{
    eq_fir_pairs();                                           /* production: the L1 copy uses them from the first block */
#ifndef EQ_CHECK
    static int16_t dl[4][51];
    static int32_t fst[6];
    static stage_region r[11];
    static uint8_t keep[1300];
    unsigned n = 0;
    r[n++] = (stage_region){ (void *)0xFF8017CC, 0x14 };
    r[n++] = (stage_region){ (void *)0xFF8017F8, 102 };
    r[n++] = (stage_region){ fst, sizeof fst };
    r[n++] = (stage_region){ (void *)0x01AABB28, 0x180 };
    r[n++] = (stage_region){ (void *)0x01AABAD8, 0x50 };
    for (int f = 0; f < 4; f++) r[n++] = (stage_region){ dl[f], sizeof dl[f] };
    r[n++] = (stage_region){ (void *)0xFF800D34, 128 };
    r[n++] = (stage_region){ (void *)0xFF800DB4, 128 };
    uint32_t off = 0;
    for (unsigned i = 0; i < n; i++) { stage_copy(keep + off, (const volatile uint8_t *)r[i].addr, r[i].len); off += r[i].len; }
    stage_check_t st = { 0 };
    uint32_t rng = 0x2468ACEu;
    for (int b = 0; b < 48; b++) {
        for (int f = 0; f < 4; f++) {                       /* temporary FIR states: lo / hi coefficients, our lines */
            fir_st *s = (fir_st *)((uint8_t *)FIRS + 0x14 * f);
            s->h = (const int16_t *)(f & 1 ? 0xFF80006C : 0xFF800004); s->d = dl[f]; s->k = 51;
            rng = rng * 1103515245u + 12345u; s->p = dl[f] + (rng >> 8) % 51;
            for (int j = 0; j < 51; j++) { rng = rng * 1103515245u + 12345u; dl[f][j] = (int16_t)(rng >> 16); }
        }
        volatile int16_t *x = (volatile int16_t *)0xFF800D34;
        const int mode = b & 3;
        for (int i = 0; i < 128; i++) {
            rng = rng * 1103515245u + 12345u;
            int32_t v = (int32_t)(rng >> 16) - 32768;
            x[i] = (int16_t)(mode == 0 ? v : mode == 1 ? ((i & 2) ? 32767 : -32768) : mode == 2 ? 0 : v / 128);
        }
        volatile int16_t *dly = (volatile int16_t *)0xFF8017F8;
        for (int i = 0; i < 51; i++) { rng = rng * 1103515245u + 12345u; dly[i] = (int16_t)(rng >> 16); }
        for (int i = 0; i < 4; i++) { rng = rng * 1103515245u + 12345u; fst[i] = (int32_t)rng; }
        fst[4] = b % 25; fst[5] = (b * 7) % 25;
        rng = rng * 1103515245u + 12345u; PRM[0] = (int32_t)rng;
        rng = rng * 1103515245u + 12345u; PRM[1] = (int32_t)rng;
        for (int i = 2; i < 5; i++) { rng = rng * 1103515245u + 12345u; PRM[i] = (int32_t)(rng >> 1); }
        eq_arg = fst;
        stage_check(&st, r, n, eq_l1_call, eq_mod);
    }
    off = 0;
    for (unsigned i = 0; i < n; i++) { stage_put((volatile uint8_t *)r[i].addr, keep + off, r[i].len); off += r[i].len; }
    eq_selftest[0] = st.blocks; eq_selftest[1] = st.bad; eq_selftest[2] = st.first_region; eq_selftest[3] = 1;
#endif
}
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
    if (!eq_hp[0][0] && !eq_hp[0][1]) eq_fir_pairs();      /* check builds: before the first block */
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
