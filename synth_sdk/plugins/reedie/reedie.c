/* reedie: a single-reed woodwind engine. A nonlinear reed drives a two-tap bore reflection loop:
 * the pipe's reflection sign decides whether it behaves closed-open (clarinet: odd harmonics) or
 * open-open (oboe: all harmonics), and the bore knob blends continuously between the two while
 * pushing the bell past both ends into instruments that do not exist.
 *   page 1  blue  = breath   player air: audible before the reed speaks, then a reduced bed
 *           green = buzz     reed chatter/slap: broadband rasp + aperture rattle, pitch stays locked
 *           white = bite     lip pressure: loose/dull .. natural .. biting/bright -> thin and choked
 *           red   = bore     clarinet at 25 %, oboe at 70 %, unreal beyond both
 *
 * DSP (integers only, no per-sample division or 64-bit math):
 *   reed: x = rest - r, u = A (x - x^3) with a fixed rest position. The fold is the negative
 *       resistance that makes the loop self-oscillate; x is clamped to [-1, 1], so u >= 0 (air
 *       only flows into the pipe) and the reed closes at both extremes. The rest position sets the
 *       asymmetry, which is where the oboe's even harmonics come from.
 *   bore: u is written at wp; two linear-interpolated reads at D and 2D,
 *         r = ((32768 - c) * y[wp-D] - c * y[wp-2D]) / 32768
 *       Both taps resonate at the same f0 for D = fs / (2 f0). The reed fold is inverting around its
 *       rest position, so the D tap is the inverting reflection (odd harmonics, cylindrical) and the
 *       2D tap the non-inverting one (all harmonics, conical). The blend is continuous, so the bore
 *       knob never jumps pitch. Each tap is interpolated with its own fraction (D and 2D).
 *       c reaches 1 at bore 22937 (70 %); below 8192 and above 22937 only colour changes.
 *   loop loss: one-pole low-pass, corner fixed at 3 f0, so the harmonic roll-off is the same
 *       fraction of the spectrum at every pitch. It damps H3 hard enough that the loop cannot
 *       mode-jump, which a flat loss let it do at intermediate bore blends. Under the clarinet
 *       the pipe is stuffed: the corner drops to 2 f0.
 *   loop gain: 32760 per sample and constant, so low notes sustain as long as high ones.
 *   DC blocker: one pole whose corner tracks f0 (f0/4), so the loop's phase is pitch independent;
 *       the residual phase of the loss filter and the blocker is folded back into D.
 *   Bell colour: a tilt on the output only (one pole at ~1.5 kHz mixed back into the dry signal),
 *       so the outer ends of the bore knob change tone without touching the pitch. Past the oboe,
 *       band-limited turbulence is mixed into the output: the far end is a leaky, rough pipe, not
 *       just a brighter oboe.
 *   Bores start empty: while age < D the reflection is ramped in, then a decaying noise chiff
 *       (a few bore periods) seeds the mode, so stale ring contents never chirp.
 *   Breath: xorshift32 -> high-pass 200 Hz -> fixed ~4 kHz low-pass. An onset burst decays to a
 *       bed; part is injected into the bore, part goes straight to the output so air is heard first.
 *   Buzz: rectified noise low-passed to ~47 Hz gates broadband rasp injected into the bore and
 *       rattles the reed's rest position.
 *   Bite: shifts the rest position (0.268 .. 0.331) and the reed drive (14000 .. 19000); the top
 *       of the knob is a thin, choked tone rather than silence, so it stays playable.
 *   Velocity: breath level and brightness, plus up to ~6 cents sharp.
 *   Level: trimmed per bore (the clarinet is quieter) and rounded above 28000 instead of clipped.
 * Voice state: 36-byte slot (firmware vtable + 32 bytes). Rings: 6 x 4096 int16 = 48 KB static.
 * Copyright (C) 2026 nvmsocool. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"

#define RING      4096
#define RMASK     4095
#define DMAX_Q16  (2000 << 16)     /* 2D <= 4000: both taps stay inside the ring */
#define DMIN_Q16  (4 << 16)        /* f0 <= 5.5 kHz */
#define INCMAX    536870912        /* 2^32 * 5512 / 44100 */

typedef struct {
    uint32_t vtable;               /* +0x00 firmware-owned: never write */
    uint32_t rng;                  /* +0x04 xorshift32, per voice */
    int32_t  D_q16;                /* +0x08 bore delay, Q16 samples (smoothed) */
    int32_t  lp;                   /* +0x0c loop low-pass state */
    int32_t  dc;                   /* +0x10 loop DC blocker state */
    int16_t  b1, b2;               /* +0x14 breath high-pass / low-pass */
    int16_t  chat;                 /* +0x18 chatter modulator */
    int16_t  op;                   /* +0x1a output colour low-pass state */
    uint16_t rel;                  /* +0x1c samples since note_off */
    uint16_t wp;                   /* +0x1e ring write index */
    uint16_t age;                  /* +0x20 samples since note_on (fill, breath, reed ramps) */
    uint8_t  gate;                 /* +0x22 note held */
    uint8_t  pad;                  /* +0x23 reserved */
} reedie_voice_t;
typedef char reedie_voice_fits[sizeof(reedie_voice_t) == OP1_VOICE_SIZE ? 1 : -1];

static int16_t rd_ring[OP1_VOICES][RING];

static int32_t rd_clamp(int32_t x, int32_t lo, int32_t hi)
{ return x < lo ? lo : (x > hi ? hi : x); }

static void rd_note_on(void *self, const int16_t *params)
{
    reedie_voice_t *v = (reedie_voice_t *)self;
    (void)params;
    v->rng = 0x9e3779b9u ^ ((uint32_t)(uintptr_t)self * 2654435761u);
    v->D_q16 = 0;
    v->lp = 0;
    v->dc = 0;
    v->b1 = 0;
    v->b2 = 0;
    v->chat = 0;
    v->op = 0;
    v->rel = 0;
    v->wp = 0;
    v->age = 0;
    v->gate = 1;
}

static void rd_note_off(void *self)
{
    reedie_voice_t *v = (reedie_voice_t *)self;
    v->gate = 0;
    v->rel = 0;
}

static void rd_render(void *self, int16_t *out, const int16_t *params)
{
    reedie_voice_t *v = (reedie_voice_t *)self;
    int32_t vi = op1_voice_index(self);
    int16_t *ring;
    if (vi < 0 || vi >= OP1_VOICES) vi = 0;
    ring = rd_ring[vi];

    /* ---- knobs, shaped once per block ---- */
    int32_t kb = rd_clamp(params[0], 0, 32767);
    int32_t kz = rd_clamp(params[1], 0, 32767);
    int32_t kt = rd_clamp(params[2], 0, 32767) - 16384;   /* signed: neutral at 50 % */
    int32_t kp = rd_clamp(params[3], 0, 32767);
    /* a little square keeps the low end of the knob usable without making the top wild */
    int32_t breath = (kb >> 1) + ((kb * kb) >> 16);
    int32_t buzz   = (kz >> 1) + ((kz * kz) >> 16);
    int32_t vgain  = op1_voice_vel_gain(self);

    /* bore: c = 0 cylinder (clarinet), 32767 cone (oboe); the outer thirds move bell colour */
    int32_t c = 0;
    if (kp > 8192) c = kp >= 22937 ? 32767 : (kp - 8192) * 32767 / 14745;
    int32_t ex_lo = kp < 8192 ? 8192 - kp : 0;
    int32_t ex_hi = kp > 22937 ? kp - 22937 : 0;
    int32_t kcol = 16384 - ((ex_lo * 8000) >> 13) + ((ex_hi * 9000) >> 13);
    kcol = rd_clamp(kcol, 4096, 28000);
    /* loop loss corner (in 1/256 f0): 3 f0 on the real instruments; under the clarinet the
     * pipe is stuffed and dark */
    int32_t rq8 = 768 - ((ex_lo * 256) >> 13);
    rq8 = rd_clamp(rq8, 320, 1400);

    /* ---- pitch -> bore length: D = fs / (2 f0) in Q16, one division per block ---- */
    int32_t inc = rd_clamp(OP1_PITCH_INC[0], 1, INCMAX);
    int32_t Dt = (int32_t)(140737488355328LL / inc);      /* 2^47 / inc */
    Dt -= (int32_t)(((int64_t)Dt * vgain) >> 23);         /* harder blow: up to ~6 cents sharp */

    /* The loop filters add phase delay that is not proportional to D. The two taps see it once per
     * D and once per 2D, so most of it folds into one constant; the slope with c and the darker
     * (lower-corner) loop under the clarinet are corrected separately. Fitted on the host model:
     * measured within about +-20 cents over bore 0..100 % and MIDI 36..88. */
    int32_t comp = 2200 + ((300 * c) >> 15);
    comp -= 3355;                                          /* loss low-pass phase at its 3 f0 corner, ~D/10 */
    if (rq8 < 768) comp -= ((768 - rq8) * 7) >> 1;         /* lower corner: the same filter adds phase */
    Dt += (int32_t)(((int64_t)Dt * comp) >> 15);
    Dt += 32768;                                           /* residual absolute loop delay, +0.5 sample */
    Dt = rd_clamp(Dt, DMIN_Q16, DMAX_Q16);
    int32_t D = v->D_q16 ? v->D_q16 + ((Dt - v->D_q16) >> 1) : Dt;
    D = rd_clamp(D, DMIN_Q16, DMAX_Q16);

    /* loop gain: constant per sample, so low notes sustain as long as high ones */
    int32_t g = 32760;

    /* key up: fade the reed drive instead of breaking the loop, so the pipe stops on a slope
     * rather than on whatever sample happens to be in flight (that step is a click) */
    int32_t relstep = (32767 << 15) / 4410;                /* ~100 ms at any pitch */

    /* DC blocker pole: corner = f0 / 4, in Q15 */
    int32_t alpha = (int32_t)(((int64_t)inc * 51472) >> 32);
    alpha = rd_clamp(alpha, 48, 12000);

    /* loss low-pass: one pole at rq8/256 * f0, Q15 coefficient */
    int32_t aloss = (int32_t)(((int64_t)inc * rq8) / 5340160);
    aloss = rd_clamp(aloss, 120, 20000);


    /* reed: rest position (bite) and drive; the fold is monotone around it at the factory setting */
    int32_t rest = 9830 + (kt >> 4);                      /* 0.268 .. 0.331 */
    int32_t amp  = 16384 - (kt >> 4);                     /* lip pressure: harder bite, less drive */
    amp = rd_clamp(amp, 14000, 19000);
    /* the steady flow through the reed is not sound: write only the fluctuation into the bore */
    int32_t rest2 = (rest * rest) >> 15;
    int32_t rest3 = (rest2 * rest) >> 15;
    int32_t u0 = ((rest - rest3) * amp) >> 13;

    /* reed speaks late (breath first); the firmware's envelope shapes the level afterwards */
    int32_t ramplen = 882 + ((breath >> 5) << 1);         /* 20 .. 66 ms */
    int32_t rstep = (32767 << 15) / ramplen;
    int32_t dint = D >> 16;
    int32_t fstep = dint > 0 ? 32767 / dint : 32767;
    int32_t sd0 = dint * 8;
    int32_t seeddur = sd0 < 2048 ? sd0 : 2048;            /* a few bore periods of chiff */
    if (seeddur < 8) seeddur = 8;
    int32_t sstep = (32767 << 15) / seeddur;
    int32_t astep = (8192 << 15) / 1500;                  /* attack blow: extra reed drive */
    int32_t alp = 14238;                                   /* breath low-pass, fixed ~4 kHz */
    int32_t gain = 44000 - ((11232 * c) >> 15);           /* level the bore sweep: clarinet needs more */
    int32_t tilt = kcol - 16384;                           /* output colour: negative dark, positive bright */
    int32_t b1 = v->b1, b2 = v->b2, chat = v->chat;
    int32_t op = v->op;

    uint32_t rng = v->rng;
    uint32_t wp = v->wp;
    int32_t age = v->age;
    int32_t lp = v->lp, dc = v->dc;
    int32_t rel = v->rel;

    for (int n = 0; n < OP1_BLOCK; n++) {
        /* ---- bore: two interpolated reads at D and 2D ---- */
        int32_t di = D >> 16;
        int32_t fr = (D >> 1) & 32767;
        int32_t i1 = ((int32_t)wp - di) & RMASK;
        int32_t a = ring[i1];
        a += ((ring[(i1 - 1) & RMASK] - a) * fr) >> 15;
        int32_t D2 = D << 1;
        int32_t di2 = D2 >> 16;
        int32_t fr2 = (D2 >> 1) & 32767;
        int32_t i2 = ((int32_t)wp - di2) & RMASK;
        int32_t b = ring[i2];
        b += ((ring[(i2 - 1) & RMASK] - b) * fr2) >> 15;
        int32_t r = (((32768 - c) * a) - (c * b)) >> 15;

        /* a cold line must not sing stale audio: ramp the reflection in as the bore fills */
        if (age < di) {
            int32_t f = age * fstep;
            if (f < 32767) r = (r * f) >> 15;
        }

        /* ---- loop losses: one-pole low-pass (harmonic roll-off), DC block, gain ---- */
        lp += ((r - lp) * aloss) >> 15;
        dc += ((lp - dc) * alpha) >> 15;
        r = (((lp - dc) * g) >> 15);

        /* noise: one stream feeds breath, chatter and the starting chiff */
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        int32_t nz = (int16_t)(rng >> 16);

        /* chatter ~45 Hz around its mean: aperture rattle, pitch untouched */
        int32_t an = nz < 0 ? -nz : nz;
        chat += ((an - chat) * 220) >> 15;
        int32_t wob = ((chat - 16384) * buzz) >> 15;      /* +-1/8 full scale at maximum buzz */
        wob = rd_clamp(wob, -8192, 8192);

        /* ---- reed: rest position, fold, and a slow opening as the player's air arrives ---- */
        int32_t ak = 1500 - age;                          /* attack blow, ~34 ms */
        int32_t ampe = amp;
        if (ak > 0) ampe = amp + ((amp * ((ak * astep) >> 15)) >> 15);
        int32_t x = rest - r + (wob >> 3);
        x = rd_clamp(x, -32767, 32767);
        int32_t x2 = (x * x) >> 15;
        int32_t x3 = (x2 * x) >> 15;
        int32_t u = ((x - x3) * ampe) >> 13;
        u -= u0;
        int32_t reed = age >= ramplen ? 32767 : ((age * rstep) >> 15);
        if (!v->gate) {
            reed = rel >= 4410 ? 0 : 32767 - ((rel * relstep) >> 15);
            rel++;
        }
        u = (u * reed) >> 15;

        /* ---- breath: high-pass 200 Hz, then a fixed ~4 kHz low-pass ---- */
        b1 += ((nz - b1) * 934) >> 15;
        int32_t hp = nz - b1;
        b2 += ((hp - b2) * alp) >> 15;
        int32_t bamp = 32767 - age * 3;                   /* onset puff decays to a bed */
        if (bamp < 11468) bamp = 11468;
        int32_t airq = ((((b2 * bamp) >> 15) * breath) >> 14);
        airq = (airq * vgain) >> 15;
        airq = rd_clamp(airq, -12000, 12000);

        /* buzz: noise gated by the chatter envelope -> broadband rasp in the bore */
        int32_t rasp = (((nz * chat) >> 15) * buzz) >> 15;

        /* chiff: a few periods of decaying noise as the reed opens, so it fills the pipe at the
         * note's own wavelength and the attack speaks at any pitch */
        int32_t sk = age - ramplen;
        int32_t seed = 0;
        if (sk >= 0 && sk < seeddur)
            seed = (nz * (((seeddur - sk) * sstep) >> 15)) >> 16;

        /* ---- close the loop ---- */
        int32_t in = u + (airq >> 1) + (wob >> 2) + rasp + seed + (nz >> 10);
        ring[wp] = op1_sat16(in);
        wp = (wp + 1) & RMASK;

        if (age < 64000) age++;
        /* bell colour: tilt the output with a one-pole high-pass mix (dark .. bright) */
        op += ((r - op) * 6400) >> 15;
        int32_t col = r + ((tilt * (r - op)) >> 15);
        int32_t y = (col * gain) >> 15;
        y += airq + (airq >> 2);
        /* past the oboe the bore is leaky and rough: turbulence from the same band-limited
         * noise stream, so the far end of the knob is an airy, unruly pipe rather than a
         * slightly brighter oboe */
        y += (((ex_hi * 6000) >> 13) * b2) >> 15;
        /* soft ceiling: breath and the unreal bores can push past full scale; round the top
         * instead of clipping it, so extreme settings stay loud but never square off */
        if (y > 28000) y = 28000 + ((y - 28000) >> 3);
        else if (y < -28000) y = -28000 + ((y + 28000) >> 3);
        if (y > 32767) y = 32767;                          /* the knee already rounds; this only stops
                                                            * an int16 wrap in the wildest corner */
        else if (y < -32767) y = -32767;
        out[n] = (int16_t)y;
    }

    v->wp = (uint16_t)wp;
    v->age = (uint16_t)age;
    v->D_q16 = D;
    v->lp = lp;
    v->dc = dc;
    v->b1 = (int16_t)b1;
    v->b2 = (int16_t)b2;
    v->chat = (int16_t)chat;
    v->op = (int16_t)op;
    v->rel = (uint16_t)(rel > 65535 ? 65535 : rel);
    v->rng = rng;
}

const op1_engine_t ENGINE_reedie = {
    rd_note_on, rd_render, rd_note_off,
    { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE }
};
