/* Brass synth engine: pitch-bent sine with timbre envelope.
 *   blue : starting interpolation position (sine → spread waveform), anywhere from 0% up to
 *          (100% - sweep); knob up = 0%, knob down = the highest start that still leaves room for the sweep
 *   green: sweep: how far the envelope pushes the interpolation up from the start (0%..100% of the whole range)
 *   white: vibrato depth
 *   red  : vibrato frequency (the orange knob)
 * Copyright (C) 2026 nick (nvmsocool). GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"
#include "warp_table.h"

/* Pitch bend range: ~50 cents. */
#define BEND_AMOUNT       892


/* Vibrato frequency range: red (orange) knob maps to VIBRATO_MIN_INC..VIBRATO_MAX_INC (0..32 Hz) on a squared
 * curve: slow speeds get most of the knob travel (knob 1/4 = 2 Hz, 1/2 = 8 Hz, 3/4 = 18 Hz, full = 32 Hz).
 * The LFO is advanced once per SAMPLE, so these are per-sample phase increments:
 * inc = f * 2^32 / 44100 (1 Hz = 97392). */
#define VIBRATO_MIN_INC   0          /*  0   Hz */
#define VIBRATO_MAX_INC   3116530    /* 32   Hz */

/* Vibrato depth range: white knob maps to 0..~90 cents (peak). */
#define VIBRATO_MAX_AMT   (BEND_AMOUNT * 2)

/* Vibrato gate: it fades in only once the stock envelope has reached sustain, i.e. it has stayed within
 * ENV_STEADY_TOL (Q15) of one value for ENV_STEADY_BLOCKS blocks in a row, and the key is held. The test spans
 * several blocks (not one) so the slow tail of an attack or decay curve is not mistaken for sustain.
 * Vibrato starts late: raise ENV_STEADY_BLOCKS or lower ENV_STEADY_TOL. Vibrato starts early: the opposite. */
#define ENV_STEADY_TOL    8
#define ENV_STEADY_BLOCKS 8          /* ~23 ms */
#define VIB_FADE_IN       1600       /* depth gained per block while steady (~20 blocks = ~60 ms to full) */
#define VIB_FADE_OUT      4096       /* depth lost per block otherwise */

/* Voice state (the firmware gives us 0x24 bytes: vtable + 32 bytes). */
typedef struct {
    uint32_t vtable;          /* firmware-owned */
    uint32_t ph;              /* oscillator phase (2^32 = one cycle) */
    int32_t  lp;              /* low-pass state */
    int32_t  interp_start;    /* blue knob: starting interp amount Q15, smoothed */
    uint32_t vibrato_phase;   /* vibrato LFO phase (2^32 = one cycle) */
    int32_t  vibrato_depth;   /* 0..32767: vibrato fade, rises while the envelope is steady */
    int32_t  prev_block_env;  /* stock envelope value of the previous block (Q15) */
    int16_t  held;            /* 1 from note_on until note_off */
    int16_t  env_anchor;      /* envelope value the steady test compares against (Q15) */
    int16_t  steady_blocks;   /* blocks in a row the envelope stayed within ENV_STEADY_TOL of env_anchor */
} brass_voice_t;
typedef char brass_voice_fits[sizeof(brass_voice_t) <= OP1_VOICE_SIZE ? 1 : -1];

/* Starting interpolation position (Q15) from the blue and green knobs. Green is the sweep (the envelope adds up to
 * green on top of the start), so the start may use whatever is left: blue (inverted: up = 0%) picks anywhere in
 * 0 .. 100% - green. Green full: start is always 0%. Green zero: blue picks any position and nothing sweeps. */
static inline int32_t interp_start_for(const int16_t *params)
{
    return op1_mul16(32767 - params[0], 32767 - params[1]);
}


static void brass_note_on(void *self, const int16_t *params)
{
    brass_voice_t *v = (brass_voice_t *)self;
    v->interp_start = interp_start_for(params);
    v->ph = 0;
    v->lp = 0;
    v->vibrato_phase = 0;
    v->vibrato_depth = 0;
    v->prev_block_env = 0;
    v->held = 1;
    v->env_anchor = 0;
    v->steady_blocks = 0;
}

static void brass_note_off(void *self)
{
    ((brass_voice_t *)self)->held = 0;
}

/* Warped phase (Q15) at oscillator row s (0..63) + fraction frac_q15, in table column col.
 * The row after row 63 is the end of the cycle (32768): the warped phase keeps rising through the last
 * 1/64 of the cycle instead of freezing on row 63 and then jumping back to row 0. */
static inline int32_t warp_row(int32_t s, int32_t frac_q15, int32_t col)
{
    int32_t a = warp_table[s][col];
    int32_t b = s < 63 ? warp_table[s + 1][col] : 32768;
    return a + op1_mul16(b - a, frac_q15);
}

#if defined(__bfin__) && !defined(BRASS_C_REF)
/* brass_dsp.S: the sample loop below, hand-scheduled for the Blackfin, bit for bit. The block's values in the order
 * the kernel reads them; its circular constant list k[] (10 words) is cycled once per sample. */
typedef struct {
    uint32_t ph;              /*  0: oscillator phase (updated) */
    int32_t  lp;              /*  4: low-pass state (updated) */
    uint32_t vph;             /*  8: vibrato phase (updated) */
    int32_t  envq;            /* 12: envelope ramp, Q15 << 7 (updated) */
    int16_t *out;             /* 16 */
    const int16_t *warp;      /* 20: warp_table (65 rows) */
    const int32_t *inc;       /* 24: OP1_PITCH_INC */
    int32_t  k[10];           /* 28: vinc, {vamt : 1784}, {width : 257}, interp_start, 0, {32767 : 891},
                               *     EXTRACT(7, 8), EXTRACT(11, 15), {vel gain : 8191}, env_step */
} br_blk;
_Static_assert(sizeof(br_blk) == 68, "br_blk layout (brass_dsp.S)");
void br_loop(br_blk *b);
static br_blk br_b;           /* the audio thread only, one voice at a time */
#define BR_HALVES(h, l) ((int32_t)(((uint32_t)(uint16_t)(h) << 16) | (uint16_t)(l)))
#endif

static void brass_render(void *self, int16_t *out, const int16_t *params)
{
    brass_voice_t *v = (brass_voice_t *)self;
    const int32_t *inc = OP1_PITCH_INC;

    uint32_t ph = v->ph; int32_t lp = v->lp;
    uint32_t vibrato_phase = v->vibrato_phase;
    int32_t vg = op1_voice_vel_gain(self);             /* note velocity */

    /* ---- per-block control values ---- */

    /* Stock envelope: flat inside a block, steps between blocks. Ramp it linearly from the previous block's
     * value to this block's value, so nothing driven by it (timbre column, pitch bend) steps at a block edge.
     * env_q is the ramp in Q15 << 7; after 128 additions it equals env_end << 7 exactly. */
    int32_t env_end = OP1_VOICE_ENV[OP1_BLOCK - 1];
    int32_t env_q = v->prev_block_env << 7;
    int32_t env_step = env_end - v->prev_block_env;

    /* Vibrato gate: sustain reached (envelope held still for ENV_STEADY_BLOCKS blocks) while the key is held
     * fades the vibrato in, anything else fades it out. */
    int32_t env_delta = env_end - v->env_anchor;
    if (env_delta < 0) env_delta = -env_delta;
    if (env_delta <= ENV_STEADY_TOL) {
        if (v->steady_blocks < 32767) v->steady_blocks++;
    } else {
        v->env_anchor = (int16_t)env_end;
        v->steady_blocks = 0;
    }
    if (v->held && env_end > 0 && v->steady_blocks >= ENV_STEADY_BLOCKS) {
        v->vibrato_depth += VIB_FADE_IN;
        if (v->vibrato_depth > 32767) v->vibrato_depth = 32767;
    } else {
        v->vibrato_depth -= VIB_FADE_OUT;
        if (v->vibrato_depth < 0) v->vibrato_depth = 0;
    }

    /* Red/orange knob (0..32767) maps to vibrato frequency (per-sample LFO increment), squared: x^2 in Q15 gives
     * fine control at the slow end and squeezes the fast end into the last part of the knob. */
    int32_t knob_sq = (params[3] * params[3]) >> 15;
    /* = (int64_t)(VIBRATO_MAX_INC - VIBRATO_MIN_INC) * knob_sq >> 15 in 32 bits (no __muldi3): 3116530 = 95 * 2^15 + 3570
     * and knob_sq >= 0, so the product's high part shifts out exactly */
    _Static_assert(VIBRATO_MAX_INC - VIBRATO_MIN_INC == 95 * 32768 + 3570, "vibrato increment split");
    int32_t vibrato_phase_inc = VIBRATO_MIN_INC + 95 * knob_sq + ((3570 * knob_sq) >> 15);

    /* White knob (0..32767) maps to vibrato depth, scaled by the fade-in. */
    int32_t vibrato_amt_q15 = (params[2] * VIBRATO_MAX_AMT) >> 15;
    vibrato_amt_q15 = op1_mul16(vibrato_amt_q15, v->vibrato_depth);

    /* Smooth blue/green tracking of the start position. Rounded up when rising so it reaches the target exactly
     * (a plain >> 6 would stop up to 63 short on the way up, and the top of the range must stay reachable). */
    int32_t interp_start = v->interp_start;
    int32_t start_diff = interp_start_for(params) - interp_start;
    interp_start += (start_diff + (start_diff > 0 ? 63 : 0)) >> 6;

    /* Green knob (0..32767) is the sweep: 0%..100% of the whole interpolation range, scaled by the envelope. */
    int32_t width = params[1];

#if defined(__bfin__) && !defined(BRASS_C_REF)
    {
        br_blk *b = &br_b;
        b->ph = ph; b->lp = lp; b->vph = vibrato_phase; b->envq = env_q;
        b->out = out; b->warp = &warp_table[0][0]; b->inc = inc;
        b->k[0] = vibrato_phase_inc;
        b->k[1] = BR_HALVES(vibrato_amt_q15, 1784);         /* BEND_AMOUNT * 2: see brass_dsp.S */
        b->k[2] = BR_HALVES(width, 257);
        b->k[3] = interp_start;
        b->k[4] = 0;
        b->k[5] = BR_HALVES(32767, 891);
        b->k[6] = 7 << 8 | 8;
        b->k[7] = 11 << 8 | 15;
        b->k[8] = BR_HALVES(vg, 8191);
        b->k[9] = env_step;
        br_loop(b);
        ph = b->ph; lp = b->lp; vibrato_phase = b->vph;
    }
#else
    for (int n = 0; n < OP1_BLOCK; n++) {
        /* Smoothed envelope (Q15, 0..32767). */
        env_q += env_step;
        int32_t env_val = env_q >> 7;

        /* Interpolation: start (blue, limited by green) + sweep (green) scaled by the envelope. */
        int32_t interp_amount = interp_start + op1_mul16(width, env_val);
        interp_amount = interp_amount > 32767 ? 32767 : (interp_amount < 0 ? 0 : interp_amount);
        int32_t col_pos = interp_amount * 256;         /* column position, 15 fractional bits */
        int32_t col = col_pos >> 15;                   /* 0..255 */
        int32_t col_frac = col_pos & 0x7FFF;

        /* Pitch bend: driven by the envelope. */
        int32_t pitch_bend_phase = env_val + env_val;
        pitch_bend_phase = pitch_bend_phase > 32767 ? 32767 : pitch_bend_phase;
        int32_t bend_q15 = op1_mul16(BEND_AMOUNT, pitch_bend_phase);

        /* Vibrato: per-sample LFO, scaled by the gate/fade above. */
        int32_t vibrato_q15 = (OP1_SINE_TABLE[vibrato_phase >> 19] * vibrato_amt_q15) >> 15;
        vibrato_phase += (uint32_t)vibrato_phase_inc;

        /* inc * (1 + mod): 32-bit only. |mod| < 2^12 and inc < 2^31, so (inc >> 12) * mod stays below 2^31. */
        int32_t mod_q15 = bend_q15 + vibrato_q15;
        int32_t effective_inc = inc[n] + (((inc[n] >> 12) * mod_q15) >> 3);

        /* Advance oscillator phase. */
        ph += (uint32_t)effective_inc;

        /* Phase warp: bilinear in (oscillator phase, table column). */
        int32_t s = ph >> 26;
        int32_t frac_q15 = (ph & 0x3FFFFFF) >> 11;
        int32_t w0 = warp_row(s, frac_q15, col);
        int32_t w1 = warp_row(s, frac_q15, col + 1);
        int32_t ph_f_warped = w0 + op1_mul16(w1 - w0, col_frac);

        /* Convert warped phase to sine table index. */
        int32_t sine_idx = (ph_f_warped * 8191) >> 15;
        sine_idx = sine_idx > 8191 ? 8191 : (sine_idx < 0 ? 0 : sine_idx);

        /* Lookup sine value. */
        int32_t sample = OP1_SINE_TABLE[sine_idx];

        /* One-pole low-pass filter. */
        lp += ((sample - lp) * 16384) >> 15;

        /* Velocity, half scale as the other engines (six voices on the synth bus); the firmware applies the
         * gain envelope. */
        out[n] = op1_sat16((lp * vg) >> 16);
    }
#endif
    v->ph = ph; v->lp = lp;
    v->interp_start = interp_start;
    v->vibrato_phase = vibrato_phase;
    v->prev_block_env = env_end;
}

const op1_engine_t ENGINE_brass = { brass_note_on, brass_render, brass_note_off, { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE } };
