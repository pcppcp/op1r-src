/* perc: PO-32 transfer decoder state (host-testable core). GPL-3.0-or-later with the OP1R firmware permission;
 * the demodulator and framing follow libpo32 (MIT, Eric Lewis). */
#ifndef PERC_XFER_H
#define PERC_XFER_H
#include <stdint.h>
#define PERC_XFER_LIST 16
typedef struct {
    uint32_t car;                     /* carrier phase, Q32 */
    int64_t sym;                      /* symbol phase, Q32 (with timing corrections) */
    int32_t ai, aq, pi, pq;           /* this and the last symbol's I / Q */
    uint32_t win_lo, win_hi;          /* the last 64 bits, for the sync word */
    uint8_t started, synced, done, bits, cur;
    uint16_t crc;
    unsigned work_len, packets;
    uint8_t work[260];
    uint16_t patch[16][2][21];        /* [instrument][left, right] */
    uint8_t have[16];                 /* bit 0 left, bit 1 right */
    /* the same patches in the order they arrive (a drum kit fills its keys forward from them): a left patch starts
     * an entry, the right patch of that instrument in the same transfer completes it, in either order (list_right: 0 left
     * only, right = left; 1 both; 2 right only) */
    uint8_t session, list_n, list_inst[PERC_XFER_LIST], list_ses[PERC_XFER_LIST], list_right[PERC_XFER_LIST];
    uint16_t list[PERC_XFER_LIST][2][21];
} perc_xfer_t;
void perc_xfer_begin(perc_xfer_t *x);
void perc_xfer_push(perc_xfer_t *x, const int16_t *samples, unsigned n);   /* 44.1 kHz mono */
int perc_xfer_count(int slot);
int perc_xfer_busy(void);
#endif
