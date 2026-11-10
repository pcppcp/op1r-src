/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* "reactor": four sine "rods" in a feedback loop. HEAT pushes the loop and COOLING calms it. Each voice keeps a core
 * temperature; past critical the loop gain and the overdrive jump, still in tune with the note (every rod runs at a
 * whole-number multiple of it). Design: OP1-Emulator docs/reactor-design-2026-11-03.md.
 * blue   (knob 0) COOLING: lowpass in the feedback path (12 kHz .. 300 Hz, log) and how fast the core cools
 * green  (knob 1) RADIATION: noise in the loop, and Geiger clicks on the output whose rate rises with temperature
 * white  (knob 2) RODS: rods 2, 3 and 4 (ratios 2, 3, 5) come out one after another over the knob's travel
 * red    (knob 3) HEAT: feedback gain, the overdrive (drive, asymmetry, a darker tone) and the core's heat source
 * Chain: rod 4 -> rod 3 -> rod 2 -> rod 1 (phase modulation, each scaled by its rod level); rod 1's output goes back
 * through the coolant lowpass into every pulled rod's phase. Integers only, no per-sample division.
 * The sample loop is reactor_dsp.h's rx_block_c, and on the Blackfin reactor_kernel.S's rx_block (the same samples).
 * Rate (2026-11-03): a voice call is 128 samples at 88.2 kHz = 1.451 ms (689.06 calls/s). The design is the Mac
 * model's (reactor_demo.c, which ran this code at 44.1 kHz and 2.9 ms calls), so every time and Hz below is that
 * model's, re-derived for 88.2 kHz: per-sample poles are the model's taken in two steps, per-call steps are halved,
 * call counts doubled. */
#include "op1_plugin.h"
#include "reactor_dsp.h"
#include "reactor_sin.h"
#include "reactor_geom.h"
#include "../../seq/seq_text.h"

#define T_ONE     65536                   /* temperature Q16: 1.0 = critical */
#define T_LEAVE   52429                   /* leaves critical below 0.8 (hysteresis) */
#define T_MAX     (4 * T_ONE)
#define BOOST_ONE 4096                    /* Q12 */
#ifndef BOOST_CRIT
#define BOOST_CRIT 5734                   /* 1.4x loop gain while critical (2.5x ran the loop into noise) */
#endif
#ifndef LOOP_M
#define LOOP_M    1565                    /* loop phase gain at full HEAT: 1.2 rad */
#endif
#define HELD_FULL 2752                    /* calls to full hold creep, 4.0 s */

typedef struct {
    rx_run   run;          /* the sample loop's state and this block's constants */
    int32_t  temp;         /* core temperature, Q16 */
    int32_t  boost;        /* critical boost, Q12, glides to its target */
    int32_t  rl[3];        /* smoothed levels of rods 2..4, Q15 */
    int32_t  inc0, ks0;    /* cached: the index scale for this pitch */
    int32_t  mean;         /* mean click interval (samples at 88.2 kHz), refreshed every 16 calls */
    uint32_t last;         /* rx_blocks at this voice's last render (the screen: is it sounding) */
    uint16_t held;         /* calls since note on, saturating */
    uint8_t  crit, gate;
    /* the living core and the runaway (2026-11-04) */
    uint8_t  crit0;        /* crit at the last call: the flare starts on the change */
    uint32_t frng;         /* per-call LCG for the flux */
    int16_t  w[4], wt[4];  /* flux: smoothed random walks and their targets, Q15 */
    uint32_t bph;          /* breathing phase */
    int32_t  flare;        /* Q15, 1 on going critical, tau 0.5 s */
    int32_t  strike;       /* Q15, velocity squared at note on, tau 120 ms */
    int32_t  sub;          /* the runaway rod's index (before the pitch scale), glides */
    int32_t  sag;          /* the pitch sag, Q15 of the pitch step (negative), glides */
} rx_voice;
static rx_voice voices[OP1_VOICES];
static volatile uint32_t rx_blocks;    /* voice renders so far */

/* the tone lowpass at 88.2 kHz for the model's 44.1 kHz coefficient a = 20000 + 512 i: 32768 - sqrt((32768 - a) 32768), the
 * same cutoff in Hz (two steps of this pole are one of the model's) */
static const int16_t TONE88[21] = { 12314, 12728, 13151, 13583, 14026, 14479, 14943, 15420, 15911, 16416, 16937, 17476, 18035, 18616, 19222, 19856, 20523, 21228, 21979, 22787, 23666 };
/* one-pole coefficient (Q15) for 12 kHz * (300/12000)^(i/16), i = 0..16: 32768 (1 - exp(-2 pi f / 88200)) */
static const int16_t LP[17] = { 18830, 16148, 13654, 11411, 9443, 7752, 6322, 5129, 4143, 3335, 2677, 2144, 1715,
                                1369, 1092, 870, 693 };
/* 256 * -ln((i + 0.5) / 16): exponential click intervals (mean ~ 1.0) */
static const int16_t NLN[16] = { 887, 606, 475, 389, 325, 273, 231, 194, 162, 133, 108, 85, 63, 43, 25, 8 };

static inline int32_t clampi(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }
/* hold creep, Q14 1.0 .. 1.25 over HELD_FULL calls (4 s) */
static inline int32_t rx_creep(uint32_t held) { return 16384 + (held >= HELD_FULL ? 4096 : (int32_t)(held * 3) >> 1); }

/* rod levels from RODS: rod 2 over the first third of the travel, then rod 3, then rod 4 */
static void rod_targets(int32_t rods, int32_t t[3])
{
    const int32_t x = rods * 3;
    for (int i = 0; i < 3; i++) t[i] = clampi(x - i * 32767, 0, 32767);
}

static void note_on(void *self, const int16_t *q)
{
    rx_voice *v = &voices[op1_voice_index(self)];
    const uint32_t rng = v->run.rng ? v->run.rng : 0x9E3779B9u + (uint32_t)op1_voice_index(self) * 0x85EBCA6Bu;
    static const rx_voice z;
    *v = z;
    v->run.rng = (rng * 1664525u) | 1u;                                   /* odd: the LCG's full period */
    v->run.tab = rx_sin;
    for (int i = 0; i < 16; i++) v->run.nln[i] = NLN[i];
    v->run.next = RX_NO_CLICK;
    v->mean = RX_NO_CLICK;
    v->boost = BOOST_ONE;
    rod_targets(clampi(q[2], 0, 32767), v->rl);
    v->gate = 1;
    v->frng = rng * 22695477u + 1u;
    { const int32_t vg = op1_voice_vel_gain(self); v->strike = (vg * vg) >> 15; }   /* velocity squared */
}

static void note_off(void *self) { voices[op1_voice_index(self)].gate = 0; }

static void render(void *self, int16_t *out, const int16_t *q)
{
    rx_voice *v = &voices[op1_voice_index(self)];
    const int32_t *inc = OP1_PITCH_INC;
    rx_run *r = &v->run;
    const int32_t cool = clampi(q[0], 0, 32767), rad = clampi(q[1], 0, 32767);
    const int32_t rods = clampi(q[2], 0, 32767), heat = clampi(q[3], 0, 32767);
    const int32_t vel = op1_voice_vel_gain(self);

    /* ---- core temperature (once per block) ---- */
    if (v->held == 0) v->temp = (vel * 16384) >> 15;                      /* a hard key press starts warmer */
    {
        const int32_t fuel  = 18022 + ((rods * 14745) >> 15);             /* 0.55 .. 1.0 with the rods pulled */
        const int32_t velf  = 26214 + ((vel * 6553) >> 15);               /* 0.8 .. 1.0 */
        const int32_t creep = rx_creep(v->held);                          /* Q14 1.0 .. 1.25 held */
        int32_t h = heat << 1;                                            /* Q16 */
        h = (h * fuel) >> 15; h = (h * velf) >> 15; h = (h * creep) >> 14;
        const int32_t c = 9830 + ((cool * 55705) >> 15);                  /* 0.15 .. 1.0, Q16 */
        const int32_t loss = ((c >> 4) * (v->temp >> 4)) >> 8;
        v->temp = clampi(v->temp + (((h - loss) * 5 + 512) >> 10), 0, T_MAX);   /* tau = 0.3 s (cool) .. 2 s (hot): 5/1024 a call, rounded */
    }
    if (!v->crit && v->temp > T_ONE) v->crit = 1;
    else if (v->crit && v->temp < T_LEAVE) v->crit = 0;
    {                                                                     /* glide about 45 ms: the model's 1/16 of 2.9 ms is 65/2048 of 1.45 ms */
        const int32_t d = (v->crit ? BOOST_CRIT : BOOST_ONE) - v->boost;   /* (rounded up going up, so it arrives) */
        v->boost += (d > 0 ? d * 65 + 2047 : d * 65) >> 11;
    }
    v->held = v->held < 65535 ? v->held + 1 : 65520;   /* saturates in 65520..65535, so (held & 15) still cycles */
    v->last = ++rx_blocks;

    /* ---- loop settings (once per block) ---- */
    int32_t rt[3]; rod_targets(rods, rt);
    for (int i = 0; i < 3; i++) v->rl[i] += ((rt[i] - v->rl[i]) * 137) >> 10;   /* about 10 ms: the model's 1/4 of 2.9 ms is 137/1024 of 1.45 ms */
    if (inc[0] != v->inc0) {                                              /* tame the index above ~1 kHz (aliasing) */
        v->inc0 = inc[0];
        v->ks0 = inc[0] > 50000000 ? 50000000 / (inc[0] >> 12) : 4096;     /* 2^32 * 1027 Hz / 88200 */
    }
    int32_t ks = v->ks0;
    const int32_t boost = v->boost;
    const int32_t ib = BOOST_ONE + (boost - BOOST_ONE) * 2 / 3;           /* rods' index: 1.27x while critical */
    ks = (ks * (2867 + ((clampi(v->temp, 0, T_ONE) * 1229) >> 16))) >> 12;   /* warming up: 0.7x cold .. 1x hot */
    const int32_t cr = clampi(((boost - BOOST_ONE) * 2561) >> 10, 0, 4096);   /* 0 .. 1 (Q12) into critical */

    /* ---- the living core (2026-11-04): movement once per call. Below critical it goes on the rods and a little on
       the loop; while critical it moves after the loop (drive, bias, tone): the critical loop sits at the edge of chaos,
       and a 6 % wobble of its gain or of the rods there turns to noise (Mac model, off-harmonic -32 -> -17 dB) ---- */
    const int32_t calm = 4096 - cr;                                       /* Q12: 1 below critical, 0 in it */
    const int32_t inst = clampi(((clampi(v->temp, 0, T_ONE) - 36045) * 73) >> 6, 0, 32768);   /* 0 at 0.55 .. 1 at critical */
    if ((v->held & ((inst > 24576 || v->crit) ? 7 : inst > 8192 ? 15 : 31)) == 0)   /* flux targets every 46 .. 12 ms */
        for (int i = 0; i < 4; i++) { v->frng = v->frng * 22695477u + 1u; v->wt[i] = (int16_t)((int32_t)(v->frng >> 16) - 32768); }
    for (int i = 0; i < 4; i++) v->w[i] += (v->wt[i] - v->w[i]) >> 5;    /* glide tau 46 ms */
    const int32_t depth = ((1300 + ((inst * 5000) >> 15)) * calm) >> 12;   /* rod flux 4 % cold .. 19 % at the edge */
    v->bph += (30u + (uint32_t)((inst * 310) >> 15)) * 62331u;           /* breathing 0.3 Hz cold .. 3.4 Hz: 0.01 Hz x 2^32 / 689.06 / 100 */
    const int32_t bs = OP1_SINE_TABLE[v->bph >> 19];
    if (v->crit && !v->crit0) v->flare = 32767;                          /* the trip */
    v->crit0 = v->crit;
    v->flare = (v->flare * 32679) >> 15;                                  /* tau 0.5 s */
    v->strike = (v->strike * 32374) >> 15;                                /* tau 120 ms */
    /* Q15 factors: loop gain, rods' index, each rod's flux */
    /* (the strike stays small on the rods: they feed the loop, and 2x there at note on ran it into chaos at Nyquist) */
    const int32_t fmfb = 32768 + (((((1000 + ((inst * 1700) >> 15)) * bs) >> 15) * calm) >> 12) + ((v->flare * 9000) >> 15);
    const int32_t fidx = 32768 + (((((2000 + ((inst * 5000) >> 15)) * bs) >> 15) * calm) >> 12) + ((v->strike * 12000) >> 15)
                         + ((v->flare * 30000) >> 15);
    /* phase offset = s * m << 4, m = beta * 1303.8 (beta in radians, s in Q15) */
    int32_t mfb = (((heat * LOOP_M) >> 15) * boost) >> 12;                  /* loop: up to 1.2 rad, 1.7 rad critical */
    mfb = (mfb * ks) >> 12;
    mfb = (mfb * fmfb) >> 15;
    int32_t mr[3];
    {
        static const int16_t M0[3] = { 2608, 1956, 1565 };                  /* rod 2 -> 1 2.0 rad, 3 -> 2 1.5, 4 -> 3 1.2 */
        for (int i = 0; i < 3; i++) {
            int32_t m = ((((M0[i] * v->rl[i]) >> 15) * ib >> 12) * ks) >> 12;
            m = (m * (32768 + ((depth * v->w[i]) >> 15))) >> 15;
            mr[i] = clampi((m * fidx) >> 15, 0, 32767);
        }
    }
    const int32_t m2 = mr[0], m3 = mr[1], m4 = mr[2];
    /* the coolant runs at half scale (Q14), so its gains are doubled: every per-sample product is 16 x 16 bits */
    const int32_t g1 = mfb << 1, g2 = (mfb * v->rl[0]) >> 14, g3 = (mfb * v->rl[1]) >> 14, g4 = (mfb * v->rl[2]) >> 14;
    const int32_t ci = clampi(cool + ((((cool < 30000 ? 2600 : 0) * v->w[0] >> 15) * calm) >> 12), 0, 32767);   /* the coolant wanders */
    const int32_t a = LP[ci >> 11] + (((LP[(ci >> 11) + 1] - LP[ci >> 11]) * (ci & 2047)) >> 11);
    const int32_t nz = (rad * 4634) >> 15;                                /* noise into the coolant, up to 0.2 x sqrt 2 (Q14): white noise
                                                                             at 88.2 kHz is 3 dB thinner in the band than the model's */
    /* overdrive: x = y * drive into the cubic soft clip 1.5x - 0.5x^3 (clamped at |x| = 1: a full sine reaches the
       knee at every drive >= 1, so it needs no make-up gain), then a one-pole tone lowpass. While critical it throbs
       with the breathing and wanders with the flux. (Its bias, off-centre clipping for even harmonics, went on
       2026-11-04 with its DC step to pay for the runaway rod: the half-rate rod brings the new harmonics.) */
    int32_t drive = 2048 + ((heat * 10240) >> 15);                        /* Q11: 1x .. 6x */
    drive = (drive * (4096 + (cr >> 1))) >> 12;                           /* 1.5x more while critical */
    drive = (drive * (32768 + v->strike)) >> 15;                          /* a hard key barks: up to 2x at note on */
    drive = (drive * (32768 + ((((9000 * bs) >> 15) + ((5000 * v->w[1]) >> 15)) * cr >> 12))) >> 15;
    /* heat darkens half as much as in 3.2 below critical and more once critical; the flare and the strike open it */
    int32_t tone44 = 30000 - ((heat * 3500) >> 15) - ((cr * 5500) >> 12) + (v->flare >> 2) + (v->strike >> 2)
                     + ((((1800 * bs) >> 15) * cr) >> 12);                /* the model's one-pole at 44.1 kHz */
    tone44 = clampi(tone44, 20000, 30000);
    const int32_t ti = (tone44 - 20000) >> 9, tf8 = (tone44 - 20000) & 511;       /* the same Hz at 88.2 kHz (TONE88) */
    const int32_t tone = TONE88[ti] + (((TONE88[ti + 1] - TONE88[ti]) * tf8) >> 9);
    /* ---- the runaway (2026-11-04, the owner's pick D): while critical a rod at half rod 1's pitch phase-modulates it
       (period doubling: an octave-down growl, 0.77 rad, up to 1.5 rad deep in meltdown), and the pitch sags under the
       load (-20 cents at critical, about -60 deep, wobbling with the flux), coming back up as the core cools ---- */
    {
        const int32_t deep = clampi(v->temp - T_ONE, 0, T_ONE);           /* 0 at critical .. 1 at twice it */
        v->sub += (((cr * 1000) >> 12) + (deep >> 6) - v->sub) >> 5;     /* glide 46 ms */
        r->ms8 = (int16_t)clampi((v->sub * ks) >> 9, 0, 32767);           /* x 8: the kernel takes (s0 ms8) >> 15 */
        const int32_t aw = v->w[3] < 0 ? -v->w[3] : v->w[3];
        const int32_t gt = -((cr * 380) >> 12) - (deep >> 7) - ((aw * ((cr * 120) >> 12)) >> 15);   /* Q15 of the step */
        v->sag += (gt - v->sag) >> 7;                                     /* glide 0.19 s */
        r->dinc = ((v->inc0 >> 8) * v->sag) >> 7;                         /* inc x sag (inc0 >> 8 keeps it in 32 bits) */
    }
    /* Geiger rate: 0 .. 60 clicks/s with RADIATION squared, times 0.4 .. 1.6 with the temperature (every 16 calls = 23 ms) */
    if ((v->held & 15) == 1) {
        const int32_t rq = (rad * rad) >> 15;
        const int32_t tf = 13107 + (((clampi(v->temp, 0, 2 * T_ONE) >> 1) * 19661) >> 15);
        const int32_t milli = ((((rq * tf) >> 15) * 1875) >> 10);         /* clicks per 1000 s */
        v->mean = milli > 0 ? clampi(88200000 / milli, 32, 1 << 23) : RX_NO_CLICK;   /* samples at 88.2 kHz, at most 95 s */
    }
    const int32_t mean = v->mean;
    int32_t vg = (((vel * 12256) >> 13) * op1_preset_trim(q)) >> 13;
    if (vg > 65535) vg = 65535;
    const int32_t og = (vg * 7301) >> 15;                                 /* output gain, Q14: -1 dB matches the Mac model */

    r->g4 = (int16_t)g4; r->g3 = (int16_t)g3; r->g2 = (int16_t)g2; r->g1 = (int16_t)g1;
    r->m4 = (int16_t)m4; r->m3 = (int16_t)m3; r->m2 = (int16_t)m2;
    r->nz = (int16_t)nz; r->a = (int16_t)a; r->drive = (int16_t)drive; r->og = (int16_t)og;
    r->tone = (int16_t)tone;
    r->mean4 = mean == RX_NO_CLICK ? 0 : mean >> 4;
    if (mean == RX_NO_CLICK) r->next = RX_NO_CLICK;
    else if (r->next > mean) r->next = mean;
#if defined(__bfin__) && !defined(RX_C)
    rx_ring(r);
    rx_render(r, out, inc, rx_block);
#else
    rx_render(r, out, inc, rx_block_c);
#endif
}

const op1_engine_t ENGINE_reactor = { note_on, render, note_off, { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE,
                                                                     OP1_PRESS_NONE } };

/* ======================= screen (GUI thread) =======================
 * reactor.svg (gen_svg.py, traced from reactor-art.webp) holds the still drawing in groups "s0".. (one part of the
 * plant each: desk, ground, dome, chimney, tower, pipes, vessel), the rods ("rod1".."rod4", 2..4 drawn fully in) and the
 * button's cap ("cap"). Drawn here each frame (reactor_geom.h): the dial needles, the gauge needles, the rods' stems, the
 * steam, the stack's bands and the readouts (no title: the owner, 2026-11-03).
 * The shake (2026-11-08, the owner's pick: OP1-Emulator docs/design/reactor-meltdown/spec.md): as the hottest sounding
 * voice nears critical the plant buzzes, and through critical and the runaway it judders, a kick on the flare. Every
 * part shakes on its own (a sine at its own rate mixed with a per-frame LFSR jolt): rods jump, the chimney sways about
 * its foot, the vessel rattles, the pipes buzz, the gauge needles bounce, the dial needles jitter, and the whole plant
 * shifts a little. The readouts never move. After the last voice stops rendering the shake dies away in about 1 s and
 * everything is then exactly at rest (identity transforms: the old drawing, pixel for pixel). */
static void *n_rod[4], *n_cap, *n_grp[RS_NGROUP];
static int32_t g_disp[4], rod_disp[3];   /* displayed gauge values and rod levels, Q15, eased */
static uint32_t frame;
static uint8_t was_shaking;
static const uint8_t C_WHITE[4] = { 255, 255, 255, 255 }, C_BLUE[4] = { 105, 142, 255, 255 },
                     C_GREEN[4] = { 0, 237, 149, 255 }, C_RED[4] = { 255, 58, 93, 255 };

/* the shake's state: amount Q16, time in GUI ticks (R246's frame_counter, one per audio block, 689.06 a second) */
#define SK_ONE      65536
#define SK_GAP      172                   /* a frame gap over 250 ms: the screen was away */
#define SK_QUIET    40                    /* no voice rendered for 58 ms: nothing sounds */
#define SK_REST     1300                  /* under 2 % with nothing driving it: exactly at rest */
static struct {
    int32_t  amt;                         /* how hard the plant shakes, Q16 */
    uint32_t tick, blocks, quiet, t;      /* last GUI tick, rx_blocks then, ticks since it moved, shake clock */
    uint32_t lfsr;                        /* the jolts' generator (deterministic) */
    uint8_t  init;
} sk;
#ifdef OP1_HOST
static uint32_t rx_host_tick;             /* host: the GUI tick from the frames' dt (tests may jump it) */
#define RX_GUI_TICK() (rx_host_tick)
#else
#define RX_GUI_TICK() (*(volatile uint32_t *)0xFF8016E0)   /* R246 frame_counter: one tick per audio block */
#endif

/* the target from the engine: a faint buzz from temperature 0.6 to critical (0.12 .. 0.32), then 0.55 at critical
   growing to 1 at twice critical (the runaway), plus 0.35 x the flare (Q15) on going critical */
static int32_t sk_target(int32_t tmax, int crit, int32_t flare)
{
    if (!crit) {
        const int32_t b = clampi(((tmax - 39322) * 5) >> 1, 0, 65536);   /* 0 at 0.6 .. 1 at 1.0, Q16 */
        return b ? 7864 + (int32_t)(((int64_t)b * 13107) >> 16) : 0;    /* a buzz you can see: 0.12 .. 0.32 */
    }
    const int32_t deep = clampi(tmax - T_ONE, 0, T_ONE);
    return clampi(36045 + ((deep * 29491) >> 16) + ((flare * 22938) >> 15), 0, SK_ONE);
}
/* one step: el = ticks since the last frame. Rises with tau 46 ms, falls with tau 0.29 s (1 s to rest). Pure: the
   host test drives it. */
static void sk_step(uint32_t el, int32_t target, int hot)
{
    if (el > SK_GAP && !hot) { sk.amt = 0; return; }                 /* back on the screen with nothing hot: at rest */
    if (el > 69) el = 69;
    const int32_t k = clampi((int32_t)el * (target > sk.amt ? 128 : 20), 0, 4096);   /* Q12 a frame */
    sk.amt += (int32_t)(((int64_t)(target - sk.amt) * k) >> 12);
    if (!target && sk.amt < SK_REST) sk.amt = 0;
    sk.amt = clampi(sk.amt, 0, SK_ONE);
}

static void rx_screen_init(void *scr)
{
    static const char *const ids[4] = { "rod1", "rod2", "rod3", "rod4" };
    for (int i = 0; i < 4; i++) n_rod[i] = op1_find_node(scr, ids[i]);
    n_cap = op1_find_node(scr, "cap");
    for (int i = 0; i < RS_NGROUP; i++) {                            /* "s0" .. : the plant's parts */
        char id[4] = { 's', (char)('0' + i % 10), 0, 0 };
        if (i >= 10) { id[1] = (char)('0' + i / 10); id[2] = (char)('0' + i % 10); }
        n_grp[i] = op1_find_node(scr, id);
    }
    sk.amt = 0; sk.init = 0;                                         /* a new parse starts at rest */
    was_shaking = 1;                                                 /* the first frame sets every transform */
}

static inline int32_t tsin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }   /* a: 8192 a turn, Q15 */
static inline int32_t tcos(int32_t a) { return tsin(a + 2048); }

/* this frame's offsets (Q10 px), per part (gen_svg.py PARTS: desk, ground, dome, chimney, tower, pipes, vessel) and
   for the rods, dial needles and gauge needles; the chimney also sways (Q15 sin / cos about its foot) */
#define SK_NPART 7
static int32_t ox[SK_NPART], oy[SK_NPART], rdx[4], rdy[4], dial_a[4], dial_x[4], gauge_a[4], ch_s, ch_c = 32767;
#define CH_X  (96 << 10)                  /* the chimney's foot (screen px, Q10) */
#define CH_Y  (69 << 10)
static int32_t jolt(void)                                            /* Q15 -32768 .. 32767 */
{
    sk.lfsr ^= sk.lfsr << 13; sk.lfsr ^= sk.lfsr >> 17; sk.lfsr ^= sk.lfsr << 5;
    return (int32_t)(sk.lfsr >> 16) - 32768;
}
/* a shake value in -1 .. 1 (Q15): a sine at the part's rate (Hz x 100 a tick: 2^13 / 689.06 / 100 per Hz) mixed
   with this frame's jolt */
static int32_t wobble(int k, int32_t hz)
{
    const int32_t ph = (int32_t)(((uint32_t)(((uint64_t)sk.t * (uint32_t)hz * 1189u) >> 10) + (uint32_t)k * 1777u) & 8191u);
    return (tsin(ph) * 5 + jolt() * 3) >> 3;
}
static int32_t kick;                     /* Q15: the flare's extra jolt on the whole plant (set per frame) */
static void sk_offsets(void)
{
    static const int16_t AX[SK_NPART] = { 500, 700, 2600, 0, 2600, 0, 1200 };    /* Q10 px at full shake, on top */
    static const int16_t AY[SK_NPART] = { 400, 500, 1800, 0, 1600, 0, 1300 };    /* of the whole plant's (pipes: the
                                                                                    vessel's, so their joins hold) */
    static const int16_t HZ[SK_NPART] = { 1130, 1370, 870, 610, 1010, 1730, 1490 };   /* Hz x 100 */
    const int32_t a0 = sk.amt >> 1;                                  /* Q15 */
    const int32_t a = (a0 * (65536 - a0)) >> 15;                     /* eased up: a buzz of 0.2 shows as 0.36 */
    if (a0 < 13107 && (frame % 3)) return;                           /* a buzz changes every 3rd frame (~10 Hz) */
    const int32_t ga = (a * (32768 + kick)) >> 15;                   /* the whole plant, more on the flare's kick */
    int32_t gx = (ga * ((2500 * wobble(11, 790)) >> 15)) >> 15, gy = (ga * ((1600 * wobble(12, 930)) >> 15)) >> 15;
    if (a0 > 22938 && (sk.lfsr & 0x700) == 0)                        /* a lurch: about one frame in 8, 5 px across */
        gx += (gx < 0 ? -1 : 1) * ((a * 5120) >> 15);
    for (int p = 0; p < SK_NPART; p++) {
        ox[p] = gx + ((a * ((AX[p] * wobble(p, HZ[p])) >> 15)) >> 15);
        const int32_t w = wobble(p + 20, HZ[p] + 230);
        oy[p] = gy + ((a * ((AY[p] * (p >= 5 ? (w < 0 ? -w : w) : w)) >> 15)) >> 15);   /* pipes, vessel: only down */
        oy[p] = clampi(oy[p], p >= 5 ? 0 : -1300, p == 0 ? 900 : 1300);   /* the desk stays clear of the readouts */
    }
    ox[5] = ox[6]; oy[5] = oy[6];                                    /* the pipes ride on the vessel */
    {                                                                /* the chimney sways about its foot, +-6 degrees */
        const int32_t ang = (a * ((137 * wobble(3, HZ[3])) >> 15)) >> 15;  /* 8192 a turn: +-6 degrees */
        ch_s = tsin(ang); ch_c = tcos(ang);
        ox[3] = ox[1]; oy[3] = oy[1];                                /* its foot stays on its block (the ground's) */
    }
    for (int i = 0; i < 4; i++) {
        const int32_t j = jolt();                                    /* the rods jump up in their sockets */
        rdx[i] = (a * ((500 * wobble(30 + i, 1610 + i * 170)) >> 15)) >> 15;
        rdy[i] = -((a * ((2000 * (j < 0 ? -j : j)) >> 15)) >> 15);
        dial_a[i] = (a * ((228 * wobble(40 + i, 1900 + i * 110)) >> 15)) >> 15;   /* +-10 degrees */
        dial_x[i] = (a * ((500 * wobble(44 + i, 2100)) >> 15)) >> 15;
        if (frame & 1) gauge_a[i] = (a * ((137 * wobble(50 + i, 1300 + i * 140)) >> 15)) >> 15;  /* +-6 degrees, every 2nd frame */
    }
}
static void sk_zero(void)
{
    for (int p = 0; p < SK_NPART; p++) ox[p] = oy[p] = 0;
    for (int i = 0; i < 4; i++) rdx[i] = rdy[i] = dial_a[i] = dial_x[i] = gauge_a[i] = 0;
    ch_s = 0; ch_c = 32767;
}
/* a part's point moved: the chimney turned about its foot, then its offset */
static void sk_move(int p, int32_t *x, int32_t *y)
{
    if (p == 3 && ch_s) {
        const int32_t dx = *x - CH_X, dy = *y - CH_Y;
        *x = CH_X + (int32_t)(((int64_t)dx * ch_c - (int64_t)dy * ch_s) >> 15);
        *y = CH_Y + (int32_t)(((int64_t)dx * ch_s + (int64_t)dy * ch_c) >> 15);
    }
    *x += ox[p]; *y += oy[p];
}
static void segp(void *r, int p, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    sk_move(p, &a.x, &a.y); sk_move(p, &b.x, &b.y);
    op1_raster_line(r, &a, &b, w, c);
}
static inline int32_t pct(int32_t q) { const int32_t v = (q * 100 + 16384) >> 15; return v > 99 ? 99 : v; }
#define P_DESK 0
#define P_TOWER 4
#define P_VESSEL 6

static void rx_screen_update(void *scr, int dt)
{
#ifndef OP1_HOST
    op1_dbg("reactor screen", -1, 0);                                /* frame start (an emulator break times frames) */
#endif
    int16_t kn[4];
    op1_synth_knobs_live(OP1_SCREEN_SLOT & 7, kn);
    int32_t k[4];
    for (int i = 0; i < 4; i++) k[i] = kn[i] < 0 ? 0 : kn[i];
    frame++;
#ifdef OP1_HOST
    rx_host_tick += (uint32_t)((dt * 689 + 500) / 1000);
#endif

    /* the sounding voices (only while R246 renders them: rx_blocks stops when none does; before, the last voice to
       render counted forever): the hottest temperature, any critical, the loudest click, the hottest flare */
    int32_t tmax = 0, clk = 0, crit = 0, flare = 0;
    const uint32_t now = rx_blocks, tk = RX_GUI_TICK();
    const uint32_t el = sk.init ? tk - sk.tick : 0;
    if (el > SK_GAP) sk.quiet = now - sk.blocks < el ? SK_QUIET : 0;   /* back after a gap: a voice sounding all along
                                                                        rendered at least a block a tick */
    else sk.quiet = now != sk.blocks ? 0 : sk.quiet + el;
    sk.blocks = now; sk.tick = tk;
    if (!sk.init) { sk.lfsr = 0x2545F491u; sk.init = 1; }
    if (sk.quiet < SK_QUIET)
        for (int i = 0; i < OP1_VOICES; i++) {
            const rx_voice *v = &voices[i];
            if (now - v->last > 4 * OP1_VOICES) continue;
            if (v->temp > tmax) tmax = v->temp;
            if (v->run.click > clk) clk = v->run.click;
            crit |= v->crit;
            if (v->crit && v->flare > flare) flare = v->flare;
        }
    const int32_t target = sk_target(tmax, crit, flare);
    kick = (flare * 19661) >> 15;                                    /* up to 0.6 more on the whole plant */
    sk_step(el, target, target > 0);
    sk.t += el;
    const int blink = crit && ((frame >> 3) & 1);

    /* gauges: coolant, radiation (with the clicks), rods, core temperature (critical at 80 % of the scale) */
    int32_t g[4];
    g[0] = k[0];
    g[1] = ((k[1] * (13107 + ((clampi(tmax, 0, T_ONE) * 19660) >> 16))) >> 15) + (clk >> 1);
    g[2] = k[2];
    g[3] = ((tmax >> 2) * 13107) >> 13;
    for (int i = 0; i < 4; i++) {
        g[i] = clampi(g[i], 0, 32767);
        g_disp[i] += (g[i] - g_disp[i]) >> (i == 1 && g[i] > g_disp[i] ? 1 : 2);
    }
    int32_t rt[3];
    rod_targets(k[2], rt);
    for (int i = 0; i < 3; i++) rod_disp[i] += (rt[i] - rod_disp[i]) >> 2;

    if (sk.amt) sk_offsets(); else sk_zero();
    const int shaking = sk.amt != 0;
    if (shaking || was_shaking)                                      /* the parts (identity again at rest) */
        for (int i = 0; i < RS_NGROUP; i++) {
            if (!n_grp[i]) continue;
            const int p = RS_GROUP_PART[i];
            int32_t m[9] = { 1024, 0, 0, 0, 1024, 0, ox[p], oy[p], 1024 };
            if (p == 3 && ch_s) {                                    /* x' = c x - s y + tx, y' = s x + c y + ty */
                m[0] = ch_c >> 5; m[1] = ch_s >> 5; m[3] = -(ch_s >> 5); m[4] = ch_c >> 5;
                m[6] = ox[p] + CH_X - (int32_t)(((int64_t)CH_X * m[0] + (int64_t)CH_Y * m[3]) >> 10);
                m[7] = oy[p] + CH_Y - (int32_t)(((int64_t)CH_X * m[1] + (int64_t)CH_Y * m[4]) >> 10);
            }
            op1_node_set_transform(n_grp[i], m);
        }

    /* rods 1..4: the capsules move up from the drawn place (2..4 with RODS), with the vessel and their own jump */
    int32_t top[4];
    top[0] = RG_ROD1_TOP;
    for (int i = 0; i < 3; i++) top[i + 1] = RG_ROD_IN - (int32_t)(((int64_t)(RG_ROD_IN - RG_ROD_OUT) * rod_disp[i]) >> 15);
    for (int i = 0; i < 4; i++) {
        if (!n_rod[i] || (!i && !shaking && !was_shaking)) continue;
        const int32_t m[9] = { 1024, 0, 0, 0, 1024, 0, ox[P_VESSEL] + rdx[i],
                               top[i] - (i ? RG_ROD_IN : RG_ROD1_TOP) + oy[P_VESSEL] + rdy[i], 1024 };
        op1_node_set_transform(n_rod[i], m);
    }
    if (n_cap) {
        if (shaking || was_shaking) {
            const int32_t m[9] = { 1024, 0, 0, 0, 1024, 0, ox[P_DESK], oy[P_DESK], 1024 };
            op1_node_set_transform(n_cap, m);
        }
        op1_node_set_visible(n_cap, !blink);
    }
    was_shaking = shaking;
    op1_screen_draw(scr, dt);
    void *r = op1_raster();

    for (int i = 0; i < 4; i++) {                                         /* rod stems and T bars */
        const int32_t x = RG_ROD_X[i] + rdx[i], y = top[i] - RG_T_GAP + rdy[i];
        segp(r, P_VESSEL, RG_ROD_X[i], RG_STEM_Y0, x, y, 0x3C0, C_WHITE);
        segp(r, P_VESSEL, x - RG_T_HALF, y, x + RG_T_HALF, y, 0x400, C_WHITE);
    }
    static const uint8_t *const kc[4] = { C_BLUE, C_GREEN, C_WHITE, C_RED };
    for (int i = 0; i < 4; i++) {                                         /* dial needles: -135..+135 degrees */
        const int32_t a = ((k[i] * 6144) >> 15) - 3072 + dial_a[i], s = tsin(a), c = tcos(a), x = RG_DIAL_X[i] + dial_x[i];
        segp(r, P_DESK, x + ((RG_DIAL_R0 * s) >> 15), RG_DIAL_Y[i] - ((RG_DIAL_R0 * c) >> 15),
             x + ((RG_DIAL_R1 * s) >> 15), RG_DIAL_Y[i] - ((RG_DIAL_R1 * c) >> 15), RG_DIAL_W, kc[i]);
    }
    for (int i = 0; i < 4; i++) {                                         /* gauge arcs and ticks (not in the SVG) */
        int32_t px = RG_GAUGE_X[i] + ((RG_GAUGE_R * tcos(RG_ARC_A0)) >> 15), py = RG_GAUGE_Y[i] - ((RG_GAUGE_R * tsin(RG_ARC_A0)) >> 15);
        for (int j = 1; j <= 22; j++) {
            const int32_t a = RG_ARC_A0 + (RG_ARC_A1 - RG_ARC_A0) * j / 22;
            const int32_t x = RG_GAUGE_X[i] + ((RG_GAUGE_R * tcos(a)) >> 15), y = RG_GAUGE_Y[i] - ((RG_GAUGE_R * tsin(a)) >> 15);
            segp(r, P_DESK, px, py, x, y, 0x266, C_WHITE);
            px = x; py = y;
        }
        for (int j = 0; j < 5; j++) {
            const int32_t c = tcos(RG_TICK_A[j]), s = tsin(RG_TICK_A[j]), ri = RG_GAUGE_R - RG_GAUGE_TICK;
            segp(r, P_DESK, RG_GAUGE_X[i] + ((RG_GAUGE_R * c) >> 15), RG_GAUGE_Y[i] - ((RG_GAUGE_R * s) >> 15),
                 RG_GAUGE_X[i] + ((ri * c) >> 15), RG_GAUGE_Y[i] - ((ri * s) >> 15), 0x266, C_WHITE);
        }
    }
    for (int i = 0; i < 4; i++) {                 /* gauge needles: from the window's bottom centre, bouncing within the scale */
        const int32_t a = clampi(RG_GAUGE_A0 + (((RG_GAUGE_A1 - RG_GAUGE_A0) * g_disp[i]) >> 15) + gauge_a[i],
                                 RG_GAUGE_A1, RG_GAUGE_A0);
        segp(r, P_DESK, RG_GAUGE_X[i], RG_GAUGE_YMAX, RG_GAUGE_X[i] + ((RG_GAUGE_R1 * tcos(a)) >> 15),
             RG_GAUGE_Y[i] - ((RG_GAUGE_R1 * tsin(a)) >> 15), 0x500, kc[i]);
    }
    {                                                                     /* steam: taller and faster when hot */
        const int32_t h = clampi(tmax, 0, 3 * T_ONE / 2) * 2 / 3;         /* 0 .. T_ONE */
        const int32_t len = (RG_STEAM_LMAX >> 2) + (int32_t)(((int64_t)(RG_STEAM_LMAX - (RG_STEAM_LMAX >> 2)) * h) >> 16);
        const int32_t ph = (int32_t)(frame * (40 + (h >> 10)));
        for (int k2 = 0; k2 < 2; k2++) {
            int32_t px = RG_STEAM_X[k2], py = RG_STEAM_Y0;
            for (int j = 1; j <= 8; j++) {
                const int32_t y = RG_STEAM_Y0 - len * j / 8;
                const int32_t x = RG_STEAM_X[k2] + (((RG_STEAM_AMP * tsin(ph + j * 1100 + k2 * 4096)) >> 15) * j) / 8;
                segp(r, P_TOWER, px, py, x, y, 0x2C0, C_BLUE);
                px = x; py = y;
            }
        }
    }
    if (!blink)                                                           /* the stack's bands, with the chimney */
        for (int i = 0; i < 2; i++) segp(r, 3, RG_BAND_X0, RG_BAND_Y[i], RG_BAND_X1, RG_BAND_Y[i], 0x480, C_RED);

    static const char *const names[4] = { "COOLING ", "RADIATION ", "RODS ", "HEAT " };
    for (int i = 0; i < 4; i++) {                                         /* the readouts: never shake */
        char b[16];
        sq_num(sq_cat(b, names[i]), (uint32_t)pct(k[i]), 1);
        sq_text(r, RG_LABEL_X[i], RG_LABEL_Y, RG_LABEL_S, b, 0x280, kc[i]);
    }
#ifndef OP1_HOST                                                     /* frame end: shake Q16, tmax >> 4 */
    op1_dbg("reactor screen", sk.amt, (int)(uint32_t)clampi(tmax >> 4, 0, 65535));
#endif
}

const op1_screen_t SCREEN_reactor = { rx_screen_init, rx_screen_update };
