/* INPUT GAIN for input engines (VOX, SNAP, Amp): a digital trim of -12..+24 dB in 1 dB steps on the selected input,
 * before the engine's process(). Q11 gains (2048 = 0 dB, round(2048 x 10^(dB / 20))); each sample is multiplied and
 * shifted, saturated to int16 (never wraps). 0 dB is never applied: the caller passes the input buffer unchanged.
 * (from Hundekraft kennel 3.2.2 kennel/kn_input_gain.h) Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef IN_GAIN_H
#define IN_GAIN_H
#include <stdint.h>
#define IN_GAIN_MIN (-12)
#define IN_GAIN_MAX 24
static const uint16_t in_gain_q11[IN_GAIN_MAX - IN_GAIN_MIN + 1] = {
    514, 577, 648, 727, 815, 915, 1026, 1152, 1292, 1450, 1627, 1825, 2048, 2298, 2578, 2893, 3246, 3642, 4086,
    4585, 5144, 5772, 6476, 7267, 8153, 9148, 10264, 11517, 12922, 14499, 16268, 18253, 20480, 22979, 25783, 28929,
    32459 };
static inline int in_gain_clamp(int db) { return db < IN_GAIN_MIN ? IN_GAIN_MIN : db > IN_GAIN_MAX ? IN_GAIN_MAX : db; }
/* out[i] = in[i] x gain(db), saturated; n samples (64 a block) */
static inline void in_gain_apply(const int16_t *in, int16_t *out, int n, int db)
{
    const int32_t g = in_gain_q11[in_gain_clamp(db) - IN_GAIN_MIN];
    for (int i = 0; i < n; i++) {
        int32_t y = (in[i] * g) >> 11;                   /* |in| <= 32768, g < 32768: fits 32 bits */
        out[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
}
#endif
