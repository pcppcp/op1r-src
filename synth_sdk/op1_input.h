/* Input engines (input/in_host.c, after Hundekraft kennel 3.2.2 op1_input.h): a synth plugin with plugin.json
 * "input_engine": true exports this struct (32 bytes) as its symbol. It plays the selected input (mic / line / radio /
 * USB) once per 64-sample block instead of voices; the keyboard does not gate it.
 *   voice    silent ordinary voice callbacks (the engine still is a synth engine: knobs, screen, page 2)
 *   reset    audio thread: slot -1 deactivates; drop DSP / capture state
 *   process  audio thread: input -> output, 64 samples at `rate` (44100 on the unit), the sound's 8 knob words
 *   service  optional, GUI thread (while the tuner page shows): bounded, non-blocking work
 *   tuner    optional page that replaces the synth T2 page while the engine is current (blue there: INPUT GAIN)
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef OP1_INPUT_H
#define OP1_INPUT_H
#include "op1_plugin.h"
typedef struct {
    op1_engine_t voice;
    void (*reset)(int slot, uint32_t rate);
    void (*process)(const int16_t *input, int16_t *output, const int16_t *params, uint32_t rate);
    void (*service)(void);
    const op1_screen_t *tuner;
} op1_input_engine_t;
int op1_input_tuner_active(void);                    /* the engine's tuner page is shown (any thread) */
/* INPUT GAIN of the selected source, -12..+24 dB: applied to the input before process(); for the tuner page to show */
#ifdef OP1_HOST
__attribute__((weak)) int op1_host_input_gain_db;
static inline int op1_input_gain_db(void) { return op1_host_input_gain_db; }
#else
int op1_input_gain_db(void);
typedef char op1_input_size[(sizeof(op1_input_engine_t) == 32) ? 1 : -1];
#endif
#endif
