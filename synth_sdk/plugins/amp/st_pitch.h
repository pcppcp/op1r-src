/* st_pitch.h - integer-only incremental pitch detector (YIN) for the synth sampler's note readout.
 * 44100 Hz mono int16 in, frequency (Q16 Hz) + MIDI note + cents (-50..+50) out; "no note" for silence, noise
 * and aperiodic material. No float, no libc, no allocation: the caller owns one st_pitch_work (9080 bytes).
 * Meant for the main loop, in small chunks:
 *     st_pitch_begin(w, data, start, end);
 *     while (st_pitch_step(w, 4)) { ...other work... }
 *     st_pitch_result_get(w, &r);
 * `data` must stay unchanged until the analysis is done (the window is copied in the first steps, not in begin).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef ST_PITCH_H
#define ST_PITCH_H
#include <stdint.h>

#define ST_PITCH_RATE   44100
#define ST_PITCH_N      2048    /* analysis window (full rate); taken from the middle of the range */
#define ST_PITCH_MIN    1024    /* shorter ranges give no note */
#define ST_PITCH_DS     (ST_PITCH_N / 2)
#define ST_PITCH_MAXLAG 700     /* max lag at half rate = 1400 at full rate (31.5 Hz) */

typedef struct {
    /* private state */
    const int16_t *src;         /* caller's samples, window start already applied */
    int32_t len;                /* window length (even, 1024..2048) */
    int32_t pos;                /* progress inside the current phase */
    int32_t maxlag;             /* last half-rate lag computed */
    int32_t wds;                /* half-rate integration length */
    int32_t wf;                 /* full-rate integration length (refinement) */
    int32_t l0;                 /* refinement centre lag (full rate) */
    int64_t sum;                /* window sum / sum of squares for the level check */
    uint64_t sumsq;
    uint64_t cum;               /* running sum of d(1..tau) */
    uint8_t phase;
    uint8_t valid;
    uint8_t note;
    int8_t cents;
    uint32_t sample_rate;          /* set by begin_rate, default begin = 44100 */
    uint32_t freq_q16;
    uint32_t ops;               /* inner-loop operations done so far (statistics) */
    uint64_t df[7];             /* full-rate difference around l0 */
    int16_t x[ST_PITCH_N];      /* window copy */
    int16_t xd[ST_PITCH_DS];    /* window at half rate */
    uint32_t dn[ST_PITCH_MAXLAG + 1]; /* cumulative mean normalised difference, Q16 */
} st_pitch_work;

typedef struct {
    uint8_t valid;              /* 0: no note (silence, noise, too short) */
    uint8_t note;               /* MIDI note, 69 = A 440 Hz */
    int8_t cents;               /* -50..+50 against that note */
    uint32_t freq_q16;          /* Hz * 65536 */
} st_pitch_result;

/* Analyse data[start..end). Cheap: only records the range. */
void st_pitch_begin(st_pitch_work *w, const int16_t *data, int32_t start, int32_t end);
void st_pitch_begin_rate(st_pitch_work *w, const int16_t *data, int32_t start, int32_t end, uint32_t rate);
/* Do up to `budget` work units (one lag, or 128 samples of preparation). Returns 1 while more work remains. */
int st_pitch_step(st_pitch_work *w, int budget);
/* Valid after st_pitch_step returned 0. */
void st_pitch_result_get(const st_pitch_work *w, st_pitch_result *r);
/* TE naming: 60 -> "c3", 69 -> "a3", 61 -> "c#3", 21 -> "a-1" (octave = note / 12 - 2, so note 0 is "c-2").
 * `out` needs 6 bytes. Returns the length; "" and 0 for notes outside 0..127. */
int st_pitch_name(int note, char *out);

#endif
