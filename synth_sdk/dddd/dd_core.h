/* dddd: harsh noise (docs/53). One shared paraphonic wall: sources -> feedback loop (one tap per held key) ->
 * filter -> destroy (fold, clip, crush) -> limiter; random "hands" move it. Integer only, no division per sample. */
#ifndef DD_CORE_H
#define DD_CORE_H
#include <stdint.h>

#define DD_SR    88200                   /* the voice rate (OP1_VOICE_RATE): R246 renders voices 2x oversampled */
#define DD_TAPS  4
#define DD_BUF   8192                    /* loop buffer, samples (power of two): 93 ms */

typedef struct {
    int32_t source, feedback, destroy, hands;   /* knobs 0..3, Q15 0..32767 */
    int32_t filter, spread, crush;              /* page 2: blue, green, white (Q15) */
    int32_t vel, env;                           /* note velocity, the stock envelope now (Q15) */
} dd_par;

typedef struct {
    int16_t  buf[DD_BUF];
    uint32_t wp, rng;
    int32_t  ntaps, tap[DD_TAPS];       /* loop taps (samples) for the held keys */
    uint32_t inc;                       /* oscillator phase increment of the newest key (2^32 = a cycle) */
    uint32_t ph[3];                     /* oscillators: square A, its modulator B, metal partner C */
    int32_t  chaos_x;                   /* logistic map state (Q15) */
    uint32_t chaos_ph;                  /* its step phase: a step per wrap */
    int32_t  crackle;                   /* crackle impulse, decaying */
    int32_t  lp, bp;                    /* SVF state */
    int32_t  fbk;                       /* last loop output */
    int32_t  hold, hold_n, crush_n;     /* rate crusher: held sample, samples left */
    int32_t  dcx, dcy, dc2x, dc2y;      /* DC blockers (before / after the limiter) */
    int32_t  gain, peak;                /* limiter: gain (Q15), this block's peak */
    int32_t  hands_n, h_cut, h_spread, h_src, gate, gate_n, gate_g;   /* the hands' targets, stutter */
    uint32_t jumps;                     /* hands jumps so far (the screen tears on a new one) */
} dd_t;

void dd_init(dd_t *d, uint32_t seed);
void dd_reset_loop(dd_t *d);                                  /* note-on with nothing held: no stale howl */
/* the held keys (midi notes, newest last): loop taps and the oscillators' pitch (block rate) */
void dd_set_notes(dd_t *d, const uint8_t *notes, int n);
/* the same from the firmware's phase increments (2^32 = a cycle per sample), newest last */
void dd_set_incs(dd_t *d, const uint32_t *inc, int n);
void dd_render(dd_t *d, const dd_par *p, int16_t *out, int n);
uint32_t dd_period(int midi);                                 /* samples per cycle (Q8) */
#endif
