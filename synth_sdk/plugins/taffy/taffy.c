/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* "taffy": phase distortion in the style of Casio's CZ synths. A cosine is read through a bent phase: the
 * further the phase is bent, the brighter the wave, with no filter involved.
 * blue   (knob 0): wave, morphing saw -> square -> pulse -> saw-pulse (the CZ shapes, each bent its own way)
 * green  (knob 1): distortion (DCW): 0 = pure cosine, full = the bright end of the shape
 * white  (knob 2): envelope depth: how much extra distortion each note starts with, falling over ~0.2 s
 * red    (knob 3): resonance: mixes in the CZ resonant wave (a sine at 1..16x, windowed by a falling ramp),
 *                  its frequency following the distortion
 * Follows note velocity. Int-only; per sample: 2 bent phases, 3 interpolated cosine reads, a few multiplies.
 */
#include "hk/engine.h"
#include "op1_l1.h"
#include "taffy_tables.h"

typedef struct {
    uint32_t ph;
    int32_t env;   /* Q15, 32767 at note on */
    int32_t wave, dcw, resonance;
    int control_ready;
    int32_t dc_q11; /* per-voice mean estimate, before native voice mixing (Q11) */
    uint32_t tick;  /* calls: the bend envelope steps every other call */
} voice_t;

static voice_t voices[ENGINE_VOICES];
/* for the screen (screen.c), written at the end of each voice's block; nothing in the audio reads them */
volatile uint32_t taffy_blocks;                 /* rendered blocks, all voices */
volatile uint32_t taffy_vblk[ENGINE_VOICES];    /* taffy_blocks after the voice's last block: a voice sounds while
                                                   this stays close to taffy_blocks */
volatile int16_t taffy_venv[ENGINE_VOICES];     /* its note-start bend envelope (Q15), as the next block hears it */
volatile int16_t taffy_vlev[ENGINE_VOICES];     /* its stock ADSR level times velocity (Q15) at the block's end */

/* cosine of a Q15 phase (32768 = one turn) */
static inline int32_t cosq(int32_t p)
{
    p &= 0x7FFF;
    const int32_t i = p >> 5, f = (p & 31) << 10;
    const int32_t a = tf_cos[i];
    return a + (((tf_cos[i + 1] - a) * f) >> 15);
}

/* the CZ bend: phase x (Q15) reaches half a turn at d, the rest of the turn over the remainder;
   inv1 = 16384 * 32768 / d, inv2 = 16384 * 32768 / (32768 - d) */
static inline int32_t bend(int32_t x, int32_t d, int32_t inv1, int32_t inv2)
{
    return x < d ? (x * inv1) >> 15 : 16384 + (((x - d) * inv2) >> 15);
}

/* shape s at phase x, bend point d (16384 = no bend) */
static inline int32_t shape(int s, int32_t x, int32_t d, int32_t inv1, int32_t inv2)
{
    switch (s) {
    case 0:   /* saw: one bend per turn */
        return cosq(bend(x, d, inv1, inv2));
    case 1: { /* square: each half falls (rises) quickly, then holds */
        const int32_t h = x >> 14, xl = (x << 1) & 0x7FFF;
        const int32_t w = xl < 2 * d ? (xl * inv1) >> 16 : 16384;   /* xl / (2d) of half a turn */
        return cosq((h << 14) + w);
    }
    case 2: { /* pulse: a full cosine squeezed into the first 2d of the turn, then held */
        return x < 2 * d ? cosq((x * inv1) >> 15) : 32767;
    }
    default:  /* saw-pulse: a bent turn in the first half, held for the second */
        return x < 16384 ? cosq(bend(x << 1, d, inv1, inv2)) : 32767;
    }
}

#ifdef __bfin__
uint32_t tf_recip29(uint32_t d, uint32_t r0, uint32_t b);
#endif
/* everything that depends on the knobs, once per block */
typedef struct {
    int sa, sb;
    int32_t frac, d, inv1, inv2, rr, k3;
} setup_t;

static inline void setup(setup_t *p, int32_t k0, int32_t dcw, int32_t k3)
{
    p->d = 16384 - ((dcw * 15872) >> 15);                    /* 0.5 .. 1/64 of a turn */
#ifdef __bfin__
    p->inv1 = (int32_t)tf_recip29((uint32_t)p->d, (1u << 29) >> 21, 21);       /* the same quotients (taffy_dsp.S) */
    p->inv2 = (int32_t)tf_recip29((uint32_t)(32768 - p->d), (1u << 29) >> 16, 16);
#else
    p->inv1 = (16384 << 15) / p->d;
    p->inv2 = (16384 << 15) / (32768 - p->d);
#endif
    const int32_t pos = k0 * 3;                              /* shape index in the high bits, Q15 fraction */
    p->sa = pos >> 15;
    p->sb = p->sa + (p->sa < 3);
    p->frac = pos & 0x7FFF;
    p->rr = 256 + ((dcw * 3840) >> 15);                      /* resonance ratio, Q8, 1..16 */
    p->k3 = k3;
}

static inline int32_t sample(const setup_t *p, int32_t x)
{
    const int32_t a = shape(p->sa, x, p->d, p->inv1, p->inv2);
    int32_t y = a + (((shape(p->sb, x, p->d, p->inv1, p->inv2) - a) * p->frac) >> 15);
    /* resonance: sine at rr times the pitch, windowed by a ramp falling over the turn */
    const int32_t reso = (cosq(((x * p->rr) >> 8) - 8192) * (32767 - x)) >> 15;
    return y + (((reso - y) * p->k3) >> 15);
}

#ifdef __bfin__
/* taffy_dsp.S: sample() over a block for one setup (the variant by its shapes and resonance), the crossfade, DC
 * rejection and gain, bit for bit as the C loop in engine_render (which the host builds keep) */
#include "taffy_pairs.h"
typedef uint32_t (*tf_eval_t)(int16_t *y, const uint32_t *pitch, uint32_t ph, const int32_t *k, const uint32_t *pairs);
uint32_t tf_eval_saw(int16_t *, const uint32_t *, uint32_t, const int32_t *, const uint32_t *);
uint32_t tf_eval_saw_r(int16_t *, const uint32_t *, uint32_t, const int32_t *, const uint32_t *);
uint32_t tf_eval_saw_square(int16_t *, const uint32_t *, uint32_t, const int32_t *, const uint32_t *);
uint32_t tf_eval_saw_square_r(int16_t *, const uint32_t *, uint32_t, const int32_t *, const uint32_t *);
uint32_t tf_eval_square_pulse(int16_t *, const uint32_t *, uint32_t, const int32_t *, const uint32_t *);
uint32_t tf_eval_square_pulse_r(int16_t *, const uint32_t *, uint32_t, const int32_t *, const uint32_t *);
uint32_t tf_eval_pulse_sawpulse(int16_t *, const uint32_t *, uint32_t, const int32_t *, const uint32_t *);
uint32_t tf_eval_pulse_sawpulse_r(int16_t *, const uint32_t *, uint32_t, const int32_t *, const uint32_t *);
void tf_out(int16_t *out, const int16_t *y, int32_t *dc_q11, int32_t gain);
void tf_out_fade(int16_t *out, const int16_t *y, const int16_t *yp, int32_t *dc_q11, int32_t gain, const uint32_t *w);
static const tf_eval_t tf_evals[4][2] = {
    { tf_eval_saw, tf_eval_saw_r }, { tf_eval_saw_square, tf_eval_saw_square_r },
    { tf_eval_square_pulse, tf_eval_square_pulse_r }, { tf_eval_pulse_sawpulse, tf_eval_pulse_sawpulse_r },
};
/* the two setups' samples: y[-1] takes tf_eval's first (one behind) store, y[128] is read past the end */
static int16_t tf_y[2][130];
static setup_t tf_last[ENGINE_VOICES];                     /* each voice's last setup (engine_render) */

/* L1 pool (op1_plugin.h, kennel/kn_l1.h): while this engine holds the synth partition the kernels read the cosine
   pairs (4,096 B at offset 0, three reads a sample) and the knob crossfade's weights (512 B at 4,096) from on-chip L1,
   copied in when the partition is new to it; otherwise from the file's SDRAM copies. Same bytes either way. */
static const uint32_t *tf_pairs_at = tf_pairs, *tf_fade_at = tf_fade;
static void tf_place(void)
{
    int fresh;
    uint32_t *t = op1_l1_claim(OP1_L1_SYNTH, voices, sizeof tf_pairs + sizeof tf_fade, &fresh);
    if (t && fresh) {
        for (uint32_t i = 0; i < sizeof tf_pairs / 4; i++) t[i] = tf_pairs[i];
        for (uint32_t i = 0; i < sizeof tf_fade / 4; i++) t[sizeof tf_pairs / 4 + i] = tf_fade[i];
    }
    tf_pairs_at = t ? t : tf_pairs;
    tf_fade_at = t ? t + sizeof tf_pairs / 4 : tf_fade;
}

/* one shape's words of taffy_dsp.S's k (its header lists them) */
static void tf_slot(int32_t *w, int s, const setup_t *p)
{
    const int32_t c2 = (int32_t)((1u << 29) - (uint32_t)p->d * (uint32_t)p->inv2);
    switch (s) {
    case 0: w[0] = p->d; w[1] = p->inv1; w[2] = p->inv2; w[3] = c2; break;
    case 1: w[0] = p->d; w[1] = p->inv1; w[2] = 1 << 29; w[3] = 0; break;
    case 2: w[0] = 2 * p->d; w[1] = p->inv1; w[2] = 0; w[3] = 0; break;
    default: w[0] = (p->d + 1) >> 1; w[1] = 2 * p->inv1; w[2] = 2 * p->inv2; w[3] = c2; break;
    }
}

/* fills k and renders the block's sample() values for setup p into y; returns the phase after the block */
static uint32_t tf_eval(int16_t *y, uint32_t ph, const setup_t *p)
{
    int32_t k[16];
    const int v = p->frac ? p->sa + 1 : 0;                  /* frac = 0 only at k0 = 0: shape 0 alone */
    tf_slot(k, p->sa, p);
    tf_slot(k + 4, p->sb, p);
    k[8] = 32 << 16 | 0x140A;
    k[9] = 0x0F05;
    k[10] = p->frac << 16 | (32768 - p->frac);
    k[11] = p->rr;
    k[12] = -(1 << 21);
    k[13] = 32 << 16 | 0x0D0A;
    k[14] = 32767 << 16 | 0x0805;
    k[15] = p->k3 << 16 | (32768 - p->k3);
    return tf_evals[v][p->k3 > 0](y, (const uint32_t *)ENGINE_PITCH, ph, k, tf_pairs_at);
}
#endif

/* one cycle of the wave without the resonance, n points (n a power of two up to 32768), for the screen: one setup,
   then sample() at each phase */
void taffy_wave_cycle(int16_t *y, int n, int32_t k0, int32_t dcw)
{
    setup_t p;
    setup(&p, k0, dcw, 0);
    const int32_t step = 32768 / n;
    for (int i = 0; i < n; i++) {
        const int32_t v = sample(&p, i * step);
        y[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

void engine_note_on(void *self, const int16_t *knobs)
{
    voice_t *v = &voices[ENGINE_VOICE(self)];
    (void)knobs;
    v->control_ready = 0;
    v->ph = 0;
    v->dc_q11 = 0;
    v->env = 32767;
}

void engine_note_off(void *self, const int16_t *knobs)
{
    (void)self;
    (void)knobs;
}

/* the knobs as this block hears them (after the slot LFO), for the screen */
volatile int16_t taffy_knobs[4];

void engine_render(void *self, int16_t *out, const int16_t *knobs)
{
    for (int i = 0; i < 4; i++) taffy_knobs[i] = knobs[i];
#ifdef __bfin__
    tf_place();
#endif
    const int vi = ENGINE_VOICE(self);
    voice_t *v = &voices[vi];
    int vel = ENGINE_VELOCITY(vi);
    if (vel < 1 || vel > 127) vel = 100;
    const int32_t k0 = knobs[0] < 0 ? 0 : knobs[0];
    const int32_t k1 = knobs[1] < 0 ? 0 : knobs[1];
    const int32_t k2 = knobs[2] < 0 ? 0 : knobs[2];
    const int32_t k3 = knobs[3] < 0 ? 0 : knobs[3];

    int32_t dcw = k1 + ((k2 * v->env) >> 15);
    if (dcw > 32767) dcw = 32767;
    if (v->tick++ & 1) v->env -= v->env >> 6;               /* ~0.19 s time constant: 64 steps of 2 calls (689 calls/s) */
    setup_t p, previous = {0};
    int changing = v->control_ready && (v->wave != k0 || v->dcw != dcw || v->resonance != k3);
#ifdef __bfin__
    /* tf_last[vi] = the setup of (v->wave, v->dcw, v->resonance) once control_ready: held controls reuse it and a
       change takes it as the previous setup, so only a change pays setup()'s two divisions */
    if (v->control_ready && !changing) {
        p = tf_last[vi];
    } else {
        setup(&p, k0, dcw, k3);
        if (changing) previous = tf_last[vi];
    }
    tf_last[vi] = p;
#else
    setup(&p, k0, dcw, k3);
    if (changing) setup(&previous, v->wave, v->dcw, v->resonance);
#endif
    v->wave = k0;
    v->dcw = dcw;
    v->resonance = k3;
    v->control_ready = 1;
    int32_t gain = (((17134 * vel) >> 7) * (knobs[7] > 0 ? knobs[7] : 8192)) >> 13;   /* Q15 output gain with velocity (17134:
                                                                level pass 2026-10-29, +2.4 dB, was 13000) and the preset
                                                                trim (word 7); TE's voice level */
    if (gain > 32767) gain = 32767;                            /* tf_out takes it as a 16-bit operand */

#ifdef __bfin__
    int32_t dc_q11 = v->dc_q11;
    const uint32_t end = tf_eval(&tf_y[0][1], v->ph, &p);
    if (changing) {
        tf_eval(&tf_y[1][1], v->ph, &previous);
        tf_out_fade(out, &tf_y[0][1], &tf_y[1][1], &dc_q11, gain, tf_fade_at);
    } else {
        tf_out(out, &tf_y[0][1], &dc_q11, gain);
    }
    v->ph = end;
    v->dc_q11 = dc_q11;
#else
    uint32_t ph = v->ph;
    int32_t dc_q11 = v->dc_q11;
    for (int i = 0; i < ENGINE_BLOCK; i++) {
        ph += ENGINE_PITCH[i];
        const int32_t x = (int32_t)(ph >> 17);
        int32_t y = sample(&p, x);
        if (changing) {
            /* Crossfade complete phase shapes: interpolating bend reciprocals independently
             * creates a discontinuity where the two phase segments meet. Only changing blocks
             * take the second evaluation; phase and envelope state continue uninterrupted. */
            const int32_t before = sample(&previous, x);
            y = before + (((y - before) * i) >> 7);
        }
        /* ~6.86Hz DC rejection at 88.2 kHz (a 2048-sample pole), before gain/voice summing. Q11
         * estimate avoids a coarse integer dead band. No division or 64-bit
         * sample operations; |y-mean| <=65535 keeps the gain product safe. */
        dc_q11 += (y * 2048 - dc_q11) >> 11;
        y -= dc_q11 >> 11;
        out[i] = engine_sat16((y * gain) >> 15);
    }
    v->ph = ph;
    v->dc_q11 = dc_q11;
#endif
    const uint32_t blocks = taffy_blocks + 1;
    taffy_blocks = blocks;
    taffy_vblk[vi] = blocks;
    taffy_venv[vi] = (int16_t)v->env;
#ifdef ENGINE_HOST_TEST
    taffy_vlev[vi] = (int16_t)((32767 * vel) >> 7);
#else
    taffy_vlev[vi] = (int16_t)((((const volatile int16_t *)0xFF801D50)[ENGINE_BLOCK - 1] * vel) >> 7);   /* OP1_VOICE_ENV */
#endif
}
