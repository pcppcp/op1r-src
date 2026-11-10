/* The master EQ model as R246's master_eq itself (docs/81): section .l1eq at 0xFFA030F0 (entry in .l1eq.0, helpers and
 * the FIR (dsp/fir_l1.S) in .l1eq.1), loaded there by a plain LDR block after the encrypted L1 (make_firmware.py: fits
 * the original's 1 204 bytes, no CALL / JUMP leaves the section); the call at 0xFFA01118 reaches it directly.
 * Check builds (OP1R_EQ=check / stress / stock) keep R246's routine. */
#include "../op1_synth.h"
typedef struct { const int16_t *h; int16_t *d; int16_t *p; int32_t k; } fir_st;
void eq_fir_l1(fir_st *s, const volatile int16_t *x, int16_t *y, const int32_t *hp);
#define EQ_FIR eq_fir_l1
void eq_mid_l1(volatile int16_t *x, volatile int16_t *dly, int32_t *di);
int32_t eq_onepole_l1(int16_t *b, int32_t c, int32_t s);
void eq_mix_l1(volatile int16_t *x, int32_t ab, int32_t c);
#define EQ_MID eq_mid_l1
#define EQ_OP eq_onepole_l1
#define EQ_MIX eq_mix_l1
#define EQ_ATTR __attribute__((section(".l1eq.0")))
#define EQ_ENTRY eq_l1
#define EQ_HELPER static __attribute__((section(".l1eq.1"), noinline))
#include "eq_model.h"
