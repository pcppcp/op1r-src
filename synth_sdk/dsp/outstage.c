/* Master output stage hook (docs/81): core_block_process's CALL master_output_stage (L1 0xFFA04280, per block, both
 * channels: int16 mix 0xFF800D34 / DB4 -> gain, dither, limiter -> int32 0xFF800E34 / F34) goes through the L1
 * trampoline l1_tramp_out to here. Step 1: the original, unchanged (the hook's plumbing). */
#include "../op1_synth.h"

#define MASTER_OUTPUT_STAGE ((void (*)(void))0xFFA04280)
void out_stage(void)
{
    MASTER_OUTPUT_STAGE();
}
