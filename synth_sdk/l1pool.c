/* Sine table in L1 (docs/67). The idle Lander L1 range (0xFF900820.., unused in op1r: the Lander is never built)
 * holds R246's whole sine table (OP1_SINE_TABLE, 8192 x int16, 16 KB): one-cycle reads instead of SDRAM (device,
 * fmbell 6 voices: 12.8 % -> 5.6 % of the time; a quarter table with a fold only reached 9 %). Plugins and core read
 * it with op1_sin() (op1_plugin.h). op1_sine_q points to the firmware table until the first audio block has copied it
 * (the same values, so there is no "not ready" state). Its first 2,049 entries are a quarter-wave table: plugins
 * built with the quarter fold (and granular_dsp.S) read correct values from it too.
 * The rest of the range (from 0xFF904820, 8.4 KB) is free for later (a lease pool, docs/66 / 67). */
#include "op1_synth.h"

#define L1_SINE    0xFF900820u
const int16_t *op1_sine_q = (const int16_t *)0x0101E5E8;   /* OP1_SINE_TABLE until the copy */

void l1_block(void)                                         /* audio thread, once per block (fr_snap) */
{
    if (op1_sine_q == (const int16_t *)L1_SINE) return;
    volatile uint32_t *d = (volatile uint32_t *)L1_SINE;
    const uint32_t *s = (const uint32_t *)0x0101E5E8;
    for (int i = 0; i < 4096; i++) d[i] = s[i];
    op1_sine_q = (const int16_t *)L1_SINE;
}
