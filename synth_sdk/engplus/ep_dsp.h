/* Engine layer DSP: four extra per-voice stages after a TE synth voice has rendered.
 * Pure C (no TE dependencies): runs on the OP-1 and in the host unit tests (test_ep_dsp.c).
 * Parameters are the patch's knob slots 5-8 (int16, 0..32767); 0 = stage off, so presets that
 * never touched these slots sound exactly as on stock (bit-exact). */
#ifndef EP_DSP_H
#define EP_DSP_H

#include <stdint.h>

enum { EP_DRIVE = 0, EP_FILTER, EP_RESO, EP_CRUSH, EP_PARAMS };

typedef struct ep_voice {
    int32_t y[4];               /* ladder filter poles, Q31 (input at half scale) */
    int16_t hold;               /* crush: held sample */
    uint8_t count;              /* crush: samples left to hold */
} ep_voice;

void ep_reset(ep_voice *v);

/* process n samples of one voice in place; p = the four layer parameters */
void ep_process(ep_voice *v, int16_t *buf, int n, const int16_t *p);

#endif
