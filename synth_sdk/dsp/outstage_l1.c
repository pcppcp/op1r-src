/* The output stage model as R246's master_output_stage itself (docs/81): compiled into the section .l1out, linked at
 * 0xFFA04280, and loaded there by a plain LDR block after the encrypted L1 (make_firmware.py): core_block_process's
 * CALL reaches it directly, from L1, at L1 speed. Must fit the original's 1,412 bytes and call nothing outside
 * (make_firmware.py checks both). Check builds (OP1R_OUTSTAGE=check / stress) keep R246's routine there. */
#include "../op1_synth.h"
#define OS_ATTR __attribute__((section(".l1out.0")))   /* the entry: first, at 0xFFA04280 (link.ld) */
#define OS_HELPER static __attribute__((section(".l1out.1"), noinline))   /* channel(): once, called twice (pc-relative) */
#define OS_ENTRY out_stage_l1
#include "outstage_model.h"
