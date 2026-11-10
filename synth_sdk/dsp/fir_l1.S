/* dsp/fir.S assembled into the L1 copy of the master EQ (dsp/eq_l1.c, docs/81) */
#define FIR_SECTION .l1eq.1
#define FIR_NAME _eq_fir_l1
#define MID_NAME _eq_mid_l1
#define OP_NAME _eq_onepole_l1
#define MIX_NAME _eq_mix_l1
#include "fir.S"
