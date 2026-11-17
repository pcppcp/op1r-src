/* FX switch declick: the arithmetic (after hundekraft 3.3 kennel/kn_declick.h, Hundekraft, GPL-3.0-or-later with the
 * OP-1 firmware permission), shared by dsp/declick.c and harness/declick_test.c.
 * R246 swaps an effect with no fade: scrolling the FX list builds the new object in place and the next block renders
 * it from an empty state, so the output jumps from the old effect's last sample to the new one's first (up to ~90 % of
 * the peak). On a change of identity the jump is added back as an offset that decays by 1/16 a sample (then 1 a
 * sample at the end): about 3 ms from full scale. Measured against the last output sample if the previous render was
 * this block or the one before, else (the effect was off, or the swap left the FX pointer empty for a block, which
 * passes the dry bus) against this block's dry input sample. Integers only. */
#ifndef DECLICK_H
#define DECLICK_H
#include <stdint.h>

typedef struct {
    uint32_t id;          /* identity of the last effect rendered (0: none yet) */
    uint32_t blk;         /* block of that render */
    int32_t off;          /* offset still to add, decays to 0 */
    int16_t last;         /* last output sample of that render */
    uint16_t switches;    /* identity changes seen */
} dc_state_t;

static inline void dc_run(dc_state_t *s, uint32_t id, int16_t *x, int n, int16_t dry0, uint32_t blk)
{
    if (n <= 0) return;
    if (s->id && id != s->id) {
        int32_t ref = (uint32_t)(blk - s->blk) <= 1u ? s->last : dry0;
        s->off = ref - x[0];
        s->switches++;
    }
    s->id = id;
    int32_t off = s->off;
    for (int i = 0; off && i < n; i++) {
        int32_t v = x[i] + off;
        x[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        int32_t d = off >> 4;
        if (!d) d = off > 0 ? 1 : -1;
        off -= d;
    }
    s->off = off;
    s->last = x[n - 1];
    s->blk = blk;
}

/* a block in which the effect does not render although it did in the block before (R246's swap: the FX pointer
 * empty, the dry bus goes out): fade from the last output into the dry signal, mark the state dry. 1 = acted. */
#define DC_DRY 0xFFFFFFFEu
static inline int dc_gap(dc_state_t *s, int16_t *x, int n, uint32_t blk)
{
    if (n <= 0 || !s->id || blk - s->blk != 1u) return 0;
    dc_run(s, DC_DRY, x, n, x[0], blk);
    return 1;
}
#endif
