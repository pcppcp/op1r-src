#pragma GCC optimize ("no-tree-loop-distribute-patterns") /* no implicit libc calls */
/* perc synth engine: the PO-32 patches decoded from the current slot's sample (6 s: about the first 6 instruments,
 * left sides), refreshed when the sample changes; decoded in the main loop (perc_xfer_service), bounded per call.
 * GPL-3.0-or-later with the OP1R firmware permission. */
#include "op1_plugin.h"
#include "perc.h"
#include "perc_xfer.h"

/* ---- per slot: the decoded patches, refreshed when the slot's sample changes ---- */
static perc_xfer_t dec;                                       /* the decode in progress */
static uint16_t bank[8][PERC_XFER_SLOTS][21];
static uint8_t bank_have[8][PERC_XFER_SLOTS];
static int dec_slot = -1;

const uint16_t *perc_xfer_patch(int instrument)
{
    if (instrument < 0 || instrument >= PERC_XFER_SLOTS) return 0;
    int slot = op1_current_slot() & 7;
    return bank_have[slot][instrument] ? bank[slot][instrument] : 0;
}
int perc_xfer_count(int slot)
{
    int n = 0;
    for (int i = 0; i < PERC_XFER_SLOTS; i++) n += bank_have[slot & 7][i] != 0;
    return n;
}
int perc_xfer_busy(void) { return dec_slot >= 0; }

/* take a finished decode: its left sides become the slot's po-32 sounds (no patches: the bank stays) */
static void take(int slot, const perc_xfer_t *x)
{
    int any = 0;
    for (int i = 0; i < PERC_XFER_SLOTS; i++) any |= x->have[i];
    if (!any) return;
    for (int i = 0; i < PERC_XFER_SLOTS; i++) {
        bank_have[slot][i] = (x->have[i] & 1) != 0;
        if (bank_have[slot][i]) for (int j = 0; j < 21; j++) bank[slot][i][j] = x->patch[i][0][j];
    }
}

#ifdef OP1_HOST
/* host tests: decode a whole recording into a slot */
void perc_xfer_decode_into(int slot, const int16_t *s, unsigned n)
{
    perc_xfer_begin(&dec); perc_xfer_push(&dec, s, n); take(slot & 7, &dec);
}
const perc_xfer_t *perc_xfer_last(void) { return &dec; }
void perc_xfer_service(void) {}
#else
static uint32_t sig_of[8];                                    /* the sample's signature when decoded */
static uint32_t dec_pos, dec_len;
static const int16_t *dec_data;
static uint32_t signature(const uint8_t *sb)
{
    const int16_t *pk = op1_sample_peaks(sb);
    uint32_t h = 2166136261u;
    for (int i = 0; i < OP1_PEAK_BUCKETS; i++) { h ^= (uint16_t)pk[i]; h *= 16777619u; }
    return h ^ (uint32_t)(uintptr_t)op1_sample_data(sb);
}
/* the recording's length from the sampler's peaks (one per 256 samples), as granular */
static uint32_t recorded_length(const uint8_t *sb)
{
    const int16_t *pk = op1_sample_peaks(sb);
    for (int i = OP1_PEAK_BUCKETS - 1; i >= 0; i--)
        if (pk[i] > 96) { uint32_t n = (uint32_t)(i + 1) * 256u; return n > OP1_SAMPLE_MAX ? OP1_SAMPLE_MAX : n; }
    return 0;
}
/* main loop: bounded work per call (8192 samples) */
void perc_xfer_service(void)
{
    int slot = op1_current_slot() & 7;
    const uint8_t *sb = op1_sample_buffer(slot);
    if (!sb) return;
    if (dec_slot < 0) {
        uint32_t sig = signature(sb);
        if (sig == sig_of[slot]) return;
        sig_of[slot] = sig;
        perc_xfer_begin(&dec);
        dec_slot = slot; dec_pos = 0;
        dec_data = op1_sample_data(sb);
        dec_len = recorded_length(sb);
        if (!dec_len) dec_slot = -1;                           /* no recording: nothing to read */
        return;
    }
    if (dec_slot != slot) { sig_of[dec_slot] = 0; dec_slot = -1; return; }   /* slot changed: decode again later */
    uint32_t n = dec_len - dec_pos; if (n > 8192) n = 8192;
    perc_xfer_push(&dec, dec_data + dec_pos, n);
    dec_pos += n;
    if (dec_pos >= dec_len) { take(slot, &dec); dec_slot = -1; }   /* whole sample: all transfers */
}
#endif
