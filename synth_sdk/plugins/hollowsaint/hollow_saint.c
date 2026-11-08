/* Cathedral of the Hollow Saint (plugin id hollow_saint): a hurdy-gurdy for the original OP-1, kennel synth plugin.
 *
 * The four controls (all four sound-shaping; pushes open second pages where noted):
 *   blue  (knob 0) DRONE: off (0) .. subtle and dark .. rich and resonant (middle) .. dense, with a sub-octave (high).
 *                  Push for page 2: the drone's tuning, FOLLOW (the lowest held note) or a fixed key C .. B (word 5).
 *   green (knob 1) STRINGS: soft hollow, almost flute-like (low) .. nasal hurdy-gurdy (middle) .. bright and cutting
 *                  (high).
 *   white (knob 2) WHEEL BUZZ: smooth (0) .. rhythmic buzzing on the wheel strokes (middle) .. a continuous, gritty
 *                  rattle with accents (high). Push for page 2: the stroke rate, steady / 1/4 / 1/8 / 1/8T / 1/16 of
 *                  the OP-1's tempo (word 7).
 *   orange (knob 3) CATHEDRAL: dry and close (0) .. hollow, resonant, old-world (middle) .. dark, expansive and
 *                  otherworldly, with a distant bell at each note (high).
 *
 * Rates: the voice is built at half the render rate (64 samples per block), as Hundekraft's EVA does, which halves
 * the cost; its content ends below ~10 kHz (the instrument's nasal and buzz regions are 1 .. 5 kHz). The room and the
 * bells run at a quarter (they are dark by design). The output is interpolated 2x with a 4-tap half-band interpolator
 * (-1 9 9 -1)/16, multiply-free, two half-rate samples of latency (0.05 ms). On R246 a voice render call is 128
 * samples at 88.2 kHz, 689 calls a second, so the voice runs at 44.1 kHz and the room at 22.05 kHz.
 * kennel rate fix (2026-11-03): the plugin was written for 128 samples at 44.1 kHz a call (voice 22.05 kHz, room
 * 11.025 kHz, 344.5 calls a second), so on the unit every filter in Hz, per-sample decay, per-block fade and
 * smoothing, the tape's transport and the free-running wheel ran 2x, and the fixed drone key, the trompette's floor,
 * the high-string switch and the screen's keys read the pitch an octave off. Every such constant below is now
 * derived at the real rates, toward the values these comments give. The shared room's diffusers and comb (RA1, RA2,
 * RR) live in the bus at twice the old lengths (owner 2026-11-03); the non-shared build's per-voice lines (CA1, CA2,
 * CR and the body combs at the voice rate) keep theirs, so there they are half as long as written.
 *
 * The string: a bowed string's waveform is close to a sawtooth (Helmholtz motion); its nasal reedy colour comes from a
 * narrow pulse and the instrument body. Per sample: a polyBLEP saw, a polyBLEP pulse (two saws, one offset by the pulse
 * width), a hollow sine pair (fundamental + a little third harmonic) for the soft end, and bow noise that is loud at
 * the moment the string slips under the wheel (the first quarter of each cycle) and quieter while it sticks. Then a
 * one-pole tone low-pass that follows the pitch and a Chamberlin band-pass "nasal formant" around 0.9 .. 2.1 kHz.
 * A slow random pitch drift (a few cents per voice) keeps it organic.
 *
 * The drone (bourdons): two bowed drone strings, root and fifth (+2 / -3 cents: they beat slowly), polyBLEP saws
 * through a resonant two-pole low-pass, plus a sine an octave below the root at high settings. The OP-1 gives an engine
 * only its voices, each multiplied by its own envelope after render, so a drone cannot run on its own: it sounds while
 * a note sounds. Within that, ONE voice owns the drone at a time: the held voice with the lowest pitch (none held: the
 * last owner keeps it through its release). A chord gets one drone, not six, and only that voice pays for it. When
 * ownership moves, the new owner copies the old one's oscillator and filter state (rewound to the start of the block
 * when the old owner has already rendered it) and the two cross-fade over ~40 ms: with a fixed key the hand-over is
 * seamless (same pitch, same phase); in FOLLOW it glides to the new bass note's drone.
 *   FOLLOW: root an octave below the owner's note, fifth above that root (the note itself is the root's octave).
 *   Fixed key: root in octave 2 (C2 .. B2), fifth above; it ignores the played notes, octave and transpose.
 *
 * The wheel buzz (trompette and chien): on the instrument a buzzing bridge (the "dog") on the trompette string lifts and
 * slaps the soundboard once per string cycle, but only while the wheel pushes the string hard enough; players push in
 * rhythm with short wrist strokes (coups de poignet). Here: the trompette, an octave above the drone root (FOLLOW: the
 * owner's own note; folded up to ~120 Hz or higher), runs with the drone owner; once per trompette cycle a slap (its size
 * jittered per cycle: the rattle) excites a bridge resonance (1.7 .. 3.2 kHz) with a little noise inside the slap only,
 * so the buzz stays pitched and harmonic at any setting. Its size follows the wheel: a stroke envelope locked to the
 * OP-1's beat clock (read once per block; when the clock stands still, e.g. tape stopped, a free-running phase at the
 * same tempo takes over) plus a steady part, through a pressure threshold, so each stroke kicks the buzz in. The
 * strokes also swell the string a little, and at high settings each melody-string cycle gets a random roughness (grit).
 *
 * The cathedral (per voice; the engine never sees the voice sum, and the OP-1's own FX slot stays free for a real
 * reverb): two short body combs (4.0 and 5.1 ms, one with inverted feedback: odd resonances, hollow) for the old-world
 * body in the middle; two allpass diffusers into a 47 ms feedback comb damped by a low-pass in its loop (up to ~1.6 s;
 * the non-shared build's lines are half as long on R246, 23 ms and up to ~0.8 s: see Rates)
 * for the space at the top, a darkening low-pass, and a faint church-bell strike at each note (hum, minor-third tierce,
 * nominal and an inharmonic upper partial of the note, decaying 0.4 .. 3 s) fed only into the room, so it sounds
 * distant. The tail lasts as long as the voice's own release: that is the OP-1's envelope, after render. Every
 * feedback is below 1 and the room is damped; the body combs are bounded by their input scale (|y| <= 8191 / 0.3 fits
 * int16), the diffusers and the room are written with saturation: nothing can run away. A line that has been off is
 * cleared before it runs again, so no stale tail replays. Fully left, the whole stage is bypassed.
 * Bow lift: after note off the voice's sources (string, drone, buzz) fade, as the wheel lets go of the strings, faster
 * the higher CATHEDRAL is (never at 0 %: the plain OP-1 release; ~2.4 s at 25 %, ~0.6 s at 50 %, ~150 ms at 100 %),
 * while the body, the room and the bell ring on through the release (T2) that the OP-1 applies after render.
 *
 * Fixed point: Q15 throughout (32768 = 1.0); signals stay within +-65535 before each multiply by a Q15 gain so every
 * product fits in 31 bits. No division or 64-bit arithmetic per sample (a few divisions per block: polyBLEP, filter
 * coefficients). Every knob-driven gain steps across the block in 8 sub-blocks from the previous block's value (no
 * zipper); filter coefficients step once per block from knobs smoothed over ~6 ms.
 * Per-voice state lives in this file's static array (indexed by op1_voice_index), not in the 32-byte voice object;
 * the firmware-owned first word of the voice object is never touched.
 *
 * Copyright (C) 2026 the hollow_saint authors. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and
 * NOTICE of the kennel SDK). The polyBLEP saw follows the formulation in Hundekraft's EVA (eva.c, same licence).
 */
#include "op1_plugin.h"
#include "hollow_saint_geom.h"

#define NV  OP1_VOICES
#define BLK OP1_BLOCK                    /* 128 samples per render call (88.2 kHz on R246, 689 calls a second) */
#define HB  (BLK / 2)                    /* 64 voice samples per block (44.1 kHz) */
#define QB  (BLK / 4)                    /* 32 room samples per block (22.05 kHz) */

static inline int32_t clampi(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline int32_t knob(const int16_t *k, int i) { return k[i] < 0 ? 0 : k[i]; }
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
static inline uint32_t lcg(uint32_t *s) { return *s = *s * 1664525u + 1013904223u; }
static inline int32_t sine(uint32_t ph) { return OP1_SINE_TABLE[ph >> 19]; }   /* 8192-entry table, Q15 */
/* a 16 x 16 product: one cycle on the Blackfin, where an int32 product is a 3-cycle 32 x 32 multiply. Both operands
 * must fit int16 (the comment at each use says why); the host test builds check it on every call */
static inline int32_t m16(int32_t a, int32_t b)
{
#ifdef OP1_HOST
    if (a != (int16_t)a || b != (int16_t)b) __builtin_trap();
#endif
    return (int32_t)(int16_t)a * (int16_t)b;
}

/* a 5-point table over a knob (0..32767): t[0] at 0 %, t[4] at 100 %; entries within +-65535 */
static inline int32_t tab5(const int32_t *t, int32_t k)
{
    const int i = k >> 13;
    const int32_t f = (k & 8191) << 1;                                    /* Q14 */
    return t[i] + (((t[i + 1] - t[i]) * f) >> 14);
}

/* polyBLEP sawtooth: t = the phase as Q15, dt = the phase step as Q15 (1 .. 16384), inv8 = (2^30 / dt) >> 8.
 * Exhaustively checked over every dt and phase: the output stays within -32674 .. 32767 and x within
 * -32768 .. 32766, so both fit int16 */
static inline int32_t blep_saw(uint32_t ph, int32_t dt, int32_t inv8)
{
    const int32_t t = (int32_t)(ph >> 17);
    int32_t s = (int32_t)(ph >> 16) - 32768;
    if (UNLIKELY(t < dt)) {
        const int32_t x = (t * inv8) >> 7;
        s -= x + x - (m16(x, x) >> 15) - 32768;
    } else if (UNLIKELY(t > 32767 - dt)) {
        const int32_t x = ((t - 32768) * inv8) >> 7;
        s -= (m16(x, x) >> 15) + x + x + 32768;
    }
    return s;
}
static inline int32_t blep_inv(int32_t dt) { return (int32_t)((1u << 30) / (uint32_t)dt) >> 8; }

/* one-pole low-pass coefficient (Q15) for a cutoff in Hz at 44.1 kHz (clamped 20 .. 9000): 1 - e^-x, x = 2 pi fc / fs,
 * with e^-x = 1 / (1 + x + x^2/2 + x^3/6 + x^4/24) (within 1 % of the coefficient up to 9 kHz). Per block only:
 * one division. For the 22.05 kHz room, pass twice the frequency. */
static int32_t lp_coef(int32_t hz)
{
    hz = clampi(hz, 20, 9000);
    const int32_t x = (hz * 4781 + 1024) >> 11;                           /* 2 pi / 44100 in Q14 = 2.3343 */
    const int32_t x2 = (x * x) >> 14, x3 = (x2 * x) >> 14, x4 = (x3 * x) >> 14;   /* x <= 21010: all fit */
    const int32_t den = 16384 + x + (x2 >> 1) + ((x3 * 683) >> 12) + ((x4 * 171) >> 12);   /* Q14 */
    return clampi(32768 - (int32_t)((1u << 29) / (uint32_t)den), 1, 32767);
}
/* Chamberlin SVF frequency coefficient 2 sin(pi fc / fs) at 44.1 kHz, Q15: 2 (t - t^3 / 6), t = pi fc / fs (within
 * 0.5 % up to 3.5 kHz) */
static inline int32_t svf_coef(int32_t hz)
{
    const int32_t t = (clampi(hz, 20, 3500) * 4781 + 1024) >> 11;         /* pi / 44100 in Q15 = 2.3343 */
    const int32_t t3 = (((t * t) >> 15) * t) >> 15;
    return 2 * t - ((t3 * 10923) >> 15);
}

/* One-entry memos (one per call site and voice) for the per-block divisions: each is a ~100-cycle libgcc call on the
 * Blackfin, and its input seldom changes from one block to the next. m[0] = the last input (0 = none yet: no input
 * is 0 here, lp_coef's is clamped first), m[1] = its result */
static inline int32_t memo_blep_inv(int32_t dt, int32_t *m)
{
    if (m[0] != dt) { m[0] = dt; m[1] = blep_inv(dt); }
    return m[1];
}
static inline int32_t memo_lp_coef(int32_t hz, int32_t *m)
{
    hz = clampi(hz, 20, 9000);
    if (m[0] != hz) { m[0] = hz; m[1] = lp_coef(hz); }
    return m[1];
}
/* the room's saturation: two min/max instructions, no branch */
static inline int16_t sat16(int32_t x) { return (int16_t)clampi(x, -32768, 32767); }

/* ---------------------------------------------------------------- STRINGS: the knob's map (5 points, 0..100 %) */
static const int32_t T_FLUTE[5] = { 27853, 14746, 0, 0, 0 };             /* hollow sine pair */
static const int32_t T_REED[5]  = { 3277, 14746, 19661, 16384, 14746 };  /* pulse */
static const int32_t T_SAW[5]   = { 0, 3277, 9830, 16384, 21299 };       /* saw */
static const int32_t T_PW[5]    = { 16384, 9830, 5243, 3932, 3277 };     /* pulse width, Q15 of a cycle: square .. 10 % */
static const int32_t T_LPK[5]   = { 3, 6, 12, 24, 40 };                  /* tone low-pass, multiples of the pitch */
static const int32_t T_NASAL[5] = { 0, 9830, 21299, 16384, 11469 };      /* formant gain (x2 below: 0 .. 1.3) */
static const int32_t T_FHZ[5]   = { 900, 1100, 1300, 1650, 2100 };       /* formant centre, Hz */
static const int32_t T_NOISE[5] = { 1100, 900, 750, 750, 900 };          /* bow noise */
static const int32_t T_LEVEL[5] = { 19285, 21408, 30027, 24156, 19762 }; /* loudness compensation */

/* ---------------------------------------------------------------- DRONE: the knob's map */
static const int32_t T_DLEVEL[5] = { 0, 9000, 16000, 21000, 24500 };    /* drone level against the string */
static const int32_t T_DHZ[5]    = { 260, 420, 780, 1350, 2200 };       /* low-pass cutoff: dark .. dense */
static const int32_t T_DQ[5]     = { 30000, 24000, 17000, 19000, 22000 };   /* damping: resonant in the middle */
static const int32_t T_DFIFTH[5] = { 16384, 18000, 20300, 22300, 23600 };   /* the fifth against the root */
static const int32_t T_DSUB[5]   = { 0, 0, 0, 6000, 14000 };              /* sub-octave sine */
/* fixed drone keys: phase increments of C2 .. B2 per voice sample (2^32 = one cycle per sample at 44.1 kHz, the
 * voice rate on R246) */
static const uint32_t KEY_INC[12] = { 6370030u, 6748811u, 7150117u, 7575285u, 8025735u, 8502970u,
                                      9008582u, 9544261u, 10111792u, 10713070u, 11350103u, 12025015u };
#define DRONE_FADE 1171                  /* drone gain step per block: 0 .. 1 in 28 blocks (~40 ms) */
#define ROOT_DET   38                    /* +2 cents, Q15 fraction of the increment */
#define FIFTH_DET  (-57)                 /* -3 cents */
/* page-2 word 5 (blue pushed): 13 zones, 0 = FOLLOW, 1 .. 12 = C .. B */
static inline int drone_key(const int16_t *k) { return (int)((knob(k, 5) * 13) >> 15); }

/* ---------------------------------------------------------------- WHEEL BUZZ: the knob's map */
static const int32_t T_BLEVEL[5]  = { 0, 18000, 27000, 31000, 32767 };  /* buzz level against the string */
static const int32_t T_BBASE[5]   = { 0, 0, 4915, 14746, 29491 };        /* steady wheel pressure: 0, 0, .15, .45, .9 */
static const int32_t T_BSTROKE[5] = { 0, 16384, 27853, 24576, 16384 };   /* stroke depth: .5, .85, .75, .5 */
static const int32_t T_BJIT[5]    = { 3277, 6554, 11469, 16384, 22938 }; /* per-cycle slap jitter: the rattle */
static const int32_t T_BNOISE[5]  = { 2000, 4000, 7000, 10000, 12500 };  /* noise inside the slap */
static const int32_t T_BHZ[5]     = { 1700, 2000, 2400, 2800, 3200 };    /* bridge resonance */
static const int32_t T_BQ[5]      = { 8192, 6554, 5461, 4681, 4096 };    /* its damping: Q 4 .. 8 */
static const int32_t T_BTONE[5]   = { 0, 5000, 8000, 9500, 10500 };      /* the trompette string's own tone */
static const int32_t T_GRIT[5]    = { 0, 0, 0, 4500, 9000 };             /* melody per-cycle roughness */
static const int32_t T_SWELL[5]   = { 0, 2500, 4500, 4000, 3000 };       /* the strokes swelling the string */
#define BUZZ_THRESH 9830                 /* wheel pressure (Q15) where the dog starts to lift: 0.3 */
#define TROMP_MIN   11686994u            /* 120 Hz (between A#2 and B2, not on a note, so rounding never decides):
                                          * the trompette is folded up to at least this (increment per voice
                                          * sample, 44.1 kHz) */
/* page-2 word 7 (white pushed): 7 zones, steady / 1 bar / 1/2 / 1/4 / 1/8 / 1/8T / 1/16; strokes per beat x4 */
#define NZONES 7
static const uint8_t STROKES4[NZONES] = { 0, 1, 2, 4, 8, 12, 16 };
static inline int stroke_zone(const int16_t *k) { return (int)((knob(k, 7) * NZONES) >> 15); }
/* Crank & Tremor: the wheel's strokes also move the voice's level and the string's nasal tone. Low on the knob a
 * smooth, breathing swell (a raised cosine of the stroke phase); from the middle the mechanical stroke shape; high, a
 * deep pulse under the aggressive buzz. Steady (no strokes) leaves both still */
static const int32_t T_TREM[5]   = { 0, 8000, 12000, 14000, 16000 };     /* level dip between strokes (Q15) */
static const int32_t T_TSHARP[5] = { 0, 0, 20000, 32767, 32767 };       /* smooth .. mechanical stroke shape */
static const int32_t T_TTONE[5]  = { 0, 4000, 7000, 8500, 9000 };       /* nasal formant pulse, +-27 % at most */

/* stroke envelope from the stroke phase (Q32): a 7 % rise, then a squared fall over the rest */
static inline int32_t stroke_env(uint32_t sp)
{
    const int32_t p = (int32_t)(sp >> 16);                                 /* 0 .. 65535 */
    if (p < 4588) return clampi((p * 7314) >> 10, 0, 32767);
    const int32_t r = ((65536 - p) * 17617) >> 15;                         /* 0 .. 32767 */
    return (r * r) >> 15;
}

/* ---------------------------------------------------------------- CATHEDRAL: the knob's map */
static const int32_t T_CWET[5]  = { 0, 8192, 14746, 18022, 20316 };      /* wet: 0, .25, .45, .55, .62 */
static const int32_t T_CDRY[5]  = { 32767, 31130, 28180, 25559, 22938 }; /* dry: 1, .95, .86, .78, .7 */
static const int32_t T_CBFB[5]  = { 0, 14746, 22938, 21299, 18022 };     /* body comb feedback .45 .7 .65 .55 */
static const int32_t T_CBODY[5] = { 0, 26214, 32767, 19661, 13107 };     /* body in the wet */
static const int32_t T_CRFB[5]  = { 9830, 14746, 20316, 24576, 26870 };  /* room feedback .3 .45 .62 .75 .82 */
static const int32_t T_CROOM[5] = { 0, 6554, 16384, 29491, 39322 };      /* room in the wet (up to 1.2) */
static const int32_t T_CDAMP[5] = { 4500, 3800, 3000, 2300, 1700 };      /* room damping, Hz */
static const int32_t T_CDARK[5] = { 8000, 8000, 7000, 4500, 2800 };      /* low-pass, Hz (blended in from 25 %) */
static const int32_t T_CBELL[5] = { 0, 0, 0, 3500, 9000 };               /* the distant bell's strike */
#define BOW_LIFT 1274                    /* after note off, at CATHEDRAL 100 %: the bow lifts in ~150 ms (Q17 per
                                          * block: 1 - sqrt(1 - 634 / 32768), the 344.5-block design's step) */
#define CB1 89                           /* body combs: 4.04 ms, 5.12 ms at 22.05 kHz (HS_CAT11K); per voice
                                          * sample without it (2.0, 2.6 ms: memory, not changed) */
#define CB2 113
#define CA1 78                           /* non-shared build: diffusers at 22.05 kHz, 3.5 ms, 5.4 ms (written as 7.1,
                                          * 10.9 ms: memory, not changed) */
#define CA2 120
#define CR  516                          /* non-shared build: room comb at 22.05 kHz, 23.4 ms (written as 46.8 ms) */
#define RA1 156                          /* HS_SHARED_ROOM: diffusers at 22.05 kHz, 7.1 ms, 10.9 ms (hs_bus.S: L 312 / 480) */
#define RA2 240
#define RR  1032                         /* HS_SHARED_ROOM: room comb at 22.05 kHz, 46.8 ms (hs_bus.S: L 2064) */
#define AP_G 19661                       /* diffuser gain 0.6 */
/* bell partials against the note: hum 0.5, tierce 1.19 (a minor third), nominal 2.0, upper 2.66 (Q12); their decay
 * shifts per 22.05 kHz sample (3 s, 1.5 s, 0.74 s, 0.37 s) and weights */
static const uint32_t BELL_RATIO[4] = { 2048, 4874, 8192, 10895 };
static const uint8_t  BELL_DECAY[4] = { 16, 15, 14, 13 };
#define BELL_CUT 0x0CCCCCCDu             /* partials at or above 0.05 cycles per 88.2 kHz sample (4.41 kHz) left out */
static const int32_t  BELL_W[4]     = { 22938, 16384, 19661, 11469 };

/* ---------------------------------------------------------------- LO-FI RELIC (green pushed): the knob's map over its
 * page-2 word 6, the relic's age (unset: 8192, where the firmware starts a fresh word). An old tape in a forgotten room:
 * wow and flutter, warm saturation, a lower sample rate and fewer bits, a dark tape head, hiss, dust and a
 * transport that sags now and then */
static const int32_t T_RWOW[5]   = { 40, 70, 110, 160, 230 };            /* wow depth, Q15 of the pitch (2 .. 12 cents) */
static const int32_t T_RFLUT[5]  = { 10, 18, 28, 40, 55 };               /* flutter depth (0.5 .. 3 cents) */
static const int32_t T_RDRIVE[5] = { 4915, 6144, 7373, 9011, 11059 };    /* into the saturator, Q12: 1.2 .. 2.7 */
static const int32_t T_RMAKE[5]  = { 20024, 16661, 14951, 13981, 13528 }; /* 1 / (1.5 x drive), x1.1 .. 1.67 for the squash */
static const int32_t T_RHZ[5]    = { 5600, 4600, 3700, 3000, 2400 };     /* the tape head's low-pass (two poles), Hz */
static const int32_t T_RRATE[5]  = { 22050, 16000, 12000, 9500, 7500 };  /* sample rate, Hz */
static const int32_t T_RBITS[5]  = { 4, 5, 6, 7, 8 };                    /* bits dropped below int16: 12 .. 8 left */
static const int32_t T_RHISS[5]  = { 40, 70, 110, 160, 220 };            /* hiss gain on +-16384 noise */
static const int32_t T_RPOPS[5]  = { 2, 3, 5, 8, 12 };                   /* dust: pops per second per voice */
static const int32_t T_RPOP[5]   = { 1500, 2500, 3500, 4500, 6000 };     /* their size */
static const int32_t T_RSAG[5]   = { 600, 1000, 1600, 2400, 3300 };      /* transport sag: level dip (Q15) */
#define RELIC_FADE 1366                  /* on / off cross-fade step per block: 24 blocks, ~35 ms */
static inline int32_t relic_age(const int16_t *k) { return k[6] > 0 ? k[6] : 8192; }
#define RELIC_POP 23170                  /* a pop's decay per sample: sqrt 1/2 (it halved per 22.05 kHz sample) */

/* ramped gains: each steps across the block from the previous block's value */
enum { G_FLUTE, G_REED, G_SAW, G_NOISE, G_NASAL, G_OUT, G_DRONE, G_BUZZ, G_WET, G_DRY, G_BODY, G_ROOM, NG };
#define GAIN(i, sb) (g0[i] + ((dg[i] * (sb)) >> 3))   /* sub-block sb = 1 .. 8 */

enum { M_INV, M_RINV, M_FINV, M_TONE, M_DAMP, M_DARK, M_RELIC };
typedef struct {
    uint32_t ph;                 /* melody string phase (2^32 = one cycle) */
    uint32_t rng;                /* per-voice noise */
    int32_t drift, drift_t;      /* pitch drift and its target, Q16 (x2 of a Q15 fraction of the increment, +-~4 cents) */
    int32_t tone;                /* tone low-pass state */
    int32_t flp, fbp;            /* nasal formant SVF states */
    int32_t dc8;                 /* DC blocker (one-pole ~14 Hz), Q8 */
    int32_t ks[4];               /* knobs, smoothed per block */
    int32_t g[NG];               /* gains reached at the end of the previous block */
    /* drone (only the owner, and a voice fading its drone out, run it) */
    uint32_t dph[3];             /* root, fifth, sub-octave phases */
    uint32_t dinc[3];            /* their increments per 44.1 kHz sample in the last block (to rewind a copy) */
    int32_t dlp, dbp;            /* drone low-pass (SVF) */
    int32_t dgain;               /* ownership fade, Q15 */
    /* trompette and dog (with the drone owner) */
    uint32_t tph, tinc;          /* trompette phase and its last increment (44.1 kHz) */
    int32_t slap, rattle;        /* this cycle's slap and its noise envelope */
    int32_t rlp, rbp;            /* bridge resonance (SVF) */
    int32_t tlp;                 /* trompette tone low-pass */
    int32_t amp;                 /* smoothed dog amplitude (Q15) */
    int32_t mjit;                /* melody grit: this cycle's gain */
#ifdef HS_HIGH_HALF
    int32_t sprev;               /* the string's last 44.1 kHz sample (the 22.05 kHz string's interpolation) */
#endif
    int32_t bowg;                /* bow lift after note off, Q15 */
    /* cathedral */
    int16_t cb1[CB1], cb2[CB2];
#ifndef HS_SHARED_ROOM
    int16_t ca1[CA1], ca2[CA2], cr[CR];  /* HS_SHARED_ROOM: the bus's room_a1 / room_a2 / room_r instead */
#endif
    uint16_t ib1, ib2, ia1, ia2, ir;
    int32_t rdl;                 /* room damping low-pass */
    int32_t bodyp, roomp;        /* the last body / room samples (rate changes) */
    int32_t dark;                /* darkening low-pass */
    uint32_t bph[4];             /* bell partial phases (22.05 kHz) */
    int32_t benv[4];             /* their envelopes, Q23 */
    int32_t up[3];               /* the interpolator's last three 44.1 kHz samples */
    int32_t memo[7][2];          /* memo_blep_inv / memo_lp_coef, indexed by M_* */
    /* Lo-Fi Relic */
    int32_t rmix;                /* its cross-fade, Q15 (0: off, and the stage is skipped) */
    int32_t relp, relp2, rhold, rpop;   /* tape-head low-pass (two poles), the held sample, the current pop */
    uint32_t rph, rrng;          /* sample-rate phase, its own noise */
    uint8_t cat_on, bell_pending;
    uint8_t held, fresh;
} voice_t;

static voice_t V[NV];
/* scratch for the voice being rendered (render is never re-entered: one audio interrupt) */
#ifndef HS_SHARED_ROOM
static int32_t bodyb[HB];        /* the body combs' output */
#endif
static int32_t roomb[QB];        /* the room at 22.05 kHz */
static int32_t bellw[QB];        /* the bells' sum at 22.05 kHz */
#ifndef HS_SHARED_ROOM
static const int32_t bellb[QB];  /* silence, read while no bell rings */
#endif

/* shared: block counting (voices render in ascending order, so an index not above the last one starts a new block),
 * each voice's last pitch and the drone owner */
static uint32_t blk;
static int last_vi = NV;
static uint32_t v_blk[NV], v_inc[NV];
static int owner = -1;
/* shared wheel: the stroke phase when the beat clock is not moving (advanced once per block) */
static uint32_t wheel_free;
static uint8_t wheel_new_block, relic_new_block;
static uint32_t beat_n, beat_last;       /* beats counted from the clock's phase wrapping (for 1 bar and 1/2) */
/* the relic's tape transport, shared by every voice (one tape): wow, flutter and sag, advanced once per block */
static uint32_t relic_wow, relic_wow2, relic_flut, relic_rng = 0x6C8E9CF5u;
static int32_t relic_sag, relic_sag_t;
static volatile int32_t hs_relic_wob;    /* for the screen: the transport's wobble (display only) */
/* for the screen (written by the audio interrupt, read by the GUI thread; display only) */
static volatile uint32_t hs_blocks;      /* voice renders: changes while anything sounds */
static volatile int32_t hs_stroke;       /* the wheel's stroke envelope (Q15) */
static volatile int32_t hs_dog;          /* the dog's lift x the buzz level (Q15) */
static volatile uint32_t hs_bell_n;      /* bell strikes */
static volatile int32_t hs_bell_lvl;     /* the last strike's size (Q15) */

/* ---------------------------------------------------------------- the string's loop (section 1 of render) */
typedef struct { uint32_t pw, doff; int32_t ta, ff, grit, dt, inv8; const int32_t *tmod; int nsb; int32_t *out; } string_t;
static int32_t hb[HB];           /* the voice at 44.1 kHz (scratch for the voice being rendered) */
/* kennel: two optional cuts in the string, off by default.
 * HS_BOW_CHEAP: the bow noise takes one LCG step per two samples. The first sample's noise is the step's high half
 * (as before), the second's the 16-bit sum of its low and high halves (the low half alone is a weak LCG; the sum is as
 * uniform and follows the high bits); the slip / stick shift is decided once per pair, on the first sample's phase.
 * HS_HIGH_HALF: notes above HS_HIGH_MIDI (inc[0] above that note's 88.2 kHz increment) run the string at 22.05 kHz
 * (32 samples, sub-blocks of 4, dt doubled, the tone and formant coefficients for that rate, the noise gain x 1/sqrt 2
 * so its density stays), then come back to 44.1 kHz as the room does: (prev + cur) >> 1, prev kept in the voice */
#ifndef HS_HIGH_MIDI
#define HS_HIGH_MIDI 72                  /* C5 */
#endif
#define HS_MIDI_INC(m) ((uint32_t)(440.0 / OP1_VOICE_RATE * 4294967296.0 * __builtin_exp2(((m) - 69) / 12.0) + 0.5))

/* inlined at four call sites with constant flute / rough, so neither is tested per sample */
static inline __attribute__((always_inline)) void string_run(voice_t *v, const string_t *st, const int32_t *inc,
                                                             const int32_t *g0, const int32_t *dg, const int flute,
                                                             const int rough)
{
    const uint32_t pw = st->pw, doff = st->doff;
    const int32_t ta = st->ta, ff = st->ff, grit = st->grit, dt = st->dt, inv8 = st->inv8;
    const int32_t fq = 10923;                                              /* formant damping: Q = 3 */
    int32_t mjit = v->mjit;
    uint32_t ph = v->ph, rng = v->rng;
    int32_t tone = v->tone, flp = v->flp, fbp = v->fbp;
#ifdef HS_HIGH_HALF
    const int nsb = st->nsb;                                               /* 8, or 4 at 22.05 kHz */
    int32_t *o = st->out;
#else
    const int nsb = 8;
    int32_t *o = hb;
#endif
#ifdef HS_BOW_CHEAP
    int32_t nzb = 0;                                                       /* the pair's second noise product */
#endif
    const int32_t *ip = inc;
    for (int sb = 1; sb <= 8; sb++) {
        /* gains: |ga| <= 21852, gp <= 9008, gn <= 1100, gf <= 16392 (the tables x T_LEVEL), so all fit int16 */
        const int32_t gf = GAIN(G_FLUTE, sb), gr = GAIN(G_REED, sb), gs = GAIN(G_SAW, sb), gn = GAIN(G_NOISE, sb);
        /* saw * gs + pulse * gr with pulse = (saw - saw2) / 2: two products instead of three */
        const int32_t ga = gs + (gr >> 1), gp = gr >> 1;
        /* formant: unity at the centre, x2; tmod: the tremor's tone pulse (32768 = none, so no change without it) */
        const int32_t gbq = (((fq * GAIN(G_NASAL, sb)) >> 14) * st->tmod[sb - 1]) >> 15;
#ifdef HS_HIGH_HALF
        const int32_t gnh = nsb == 8 ? gn : (gn * 23170) >> 15;
#define gn gnh
#endif
        for (int n = 0; n < nsb; n++, ip += 2) {
            const uint32_t pold = ph;
            ph += (uint32_t)ip[0] + (uint32_t)ip[1] + doff;
            if (rough && UNLIKELY(ph < pold)) mjit = 32767 - (m16((int32_t)(lcg(&rng) >> 17), grit) >> 15);
            const int32_t saw = blep_saw(ph, dt, inv8);
            const int32_t saw2 = blep_saw(ph - pw, dt, inv8);              /* the pulse's second edge */
#ifdef HS_BOW_CHEAP
            int32_t nzp = nzb;
            if (!(n & 1)) {
                const uint32_t r = lcg(&rng) ^ 0x80000000u;
                const int sh = 1 + ((ph >= 0x40000000u) << 1);
                nzp = m16((int16_t)(r >> 16) >> sh, gn) >> 15;
                nzb = m16((int16_t)(r + (r >> 16)) >> sh, gn) >> 15;
            }
            int32_t raw = (m16(saw, ga) >> 15) - (m16(saw2, gp) >> 15) + nzp;
#else
            int32_t nz = (int32_t)(lcg(&rng) >> 17) - 16384;               /* +-16384 */
            nz >>= (ph >= 0x40000000u) << 1;                               /* the slip: noisy; the stick: quiet */
            int32_t raw = (m16(saw, ga) >> 15) - (m16(saw2, gp) >> 15) + (m16(nz, gn) >> 15);
#endif
            if (flute) {
                /* negative: a rising saw's fundamental is -sin, so the sine pair adds to the saw and pulse
                 * instead of cancelling them where they cross-fade */
                const int32_t fl = -sine(ph) - (m16(sine(ph * 3u), 6554) >> 15);   /* +-39322 */
                raw += (fl * gf) >> 15;
            }
            if (rough) raw = (raw * mjit) >> 15;                           /* grit */
            tone += (((raw - tone) >> 1) * ta) >> 14;
            /* nasal formant: Chamberlin SVF on half the raw signal, band output normalised (x fq) and x2 */
            flp += (ff * fbp) >> 15;
            const int32_t hp = (raw >> 1) - flp - ((fq * fbp) >> 15);
            fbp += (ff * hp) >> 15;
            *o++ = tone + ((fbp * gbq) >> 15);
        }
#ifdef HS_HIGH_HALF
#undef gn
#endif
    }
    v->ph = ph; v->rng = rng; v->mjit = mjit;
    v->tone = tone; v->flp = flp; v->fbp = fbp;
}

#if defined(__bfin__) && !defined(HS_C_REF)
/* kennel: hs_string.S runs string_run's loop on the Blackfin, the same samples bit for bit (the C above stays for host
 * builds and, with HS_C_REF, as the reference). string_asm fills its block (byte offset = HK_* x 4, hs_string.S K_*):
 * the state (the phase and the LCG state held XOR 0x80000000), the constants and the per-sub-block gains */
enum { HK_DOFF, HK_PW, HK_CM, HK_LA, HK_LC, HK_K30, HK_AM3, HK_AM1, HK_MJIT, HK_FF, HK_FQ, HK_TA, HK_PHOLD, HK_NB,
       HK_KF, HK_SPARE, HK_PH, HK_RNG, HK_TONE, HK_FLP, HK_FBP, HK_GRIT, HK_INC, HK_RAW, HK_HB, HK_SINE,
       HK_DT, HK_INV8, HK_NIN1, HK_NIN2, HK_HALF, HK_HINC, HK_SPREV, HK_GBQ, HK_SUB = HK_GBQ + 8, HK_N = HK_SUB + 3 * 8 };
void hs_string_plain(int32_t *k);
void hs_string_f(int32_t *k);
void hs_string_r(int32_t *k);
void hs_string_fr(int32_t *k);
static int32_t hs_k[HK_N];
static int32_t hs_raw[1 + HB + 1];   /* the source between the passes: a spare element in front, one read past the end */
static inline __attribute__((always_inline)) void string_asm(voice_t *v, const string_t *st, const int32_t *inc,
                                                             const int32_t *g0, const int32_t *dg,
                                                             void (*kern)(int32_t *))
{
    int32_t *const k = hs_k;
    const int32_t fq = 10923;
    const int32_t cm = 32768 - 2 * st->dt - 1;                             /* the polyBLEP test's c - 1, both halves */
    k[HK_DOFF] = (int32_t)st->doff; k[HK_PW] = (int32_t)st->pw;
    k[HK_CM] = (int32_t)(((uint32_t)cm << 16) | ((uint32_t)cm & 0xFFFFu));
    k[HK_LA] = 1664525; k[HK_LC] = 1013904223; k[HK_K30] = (int32_t)0xC0000000u; k[HK_AM3] = -3; k[HK_AM1] = -1;
    k[HK_MJIT] = v->mjit; k[HK_FF] = st->ff; k[HK_FQ] = fq; k[HK_TA] = st->ta;
    k[HK_PH] = k[HK_PHOLD] = (int32_t)(v->ph ^ 0x80000000u); k[HK_KF] = (1 << 16) | 6554;
    k[HK_DT] = st->dt; k[HK_INV8] = st->inv8; k[HK_RNG] = (int32_t)(v->rng ^ 0x80000000u);
    k[HK_TONE] = v->tone; k[HK_FLP] = v->flp; k[HK_FBP] = v->fbp; k[HK_GRIT] = st->grit;
    k[HK_INC] = (int32_t)inc; k[HK_RAW] = (int32_t)hs_raw; k[HK_SINE] = (int32_t)(OP1_SINE_TABLE + 4096);
#ifdef HS_HIGH_HALF
    static int32_t hinc[HB + 2];                                           /* hs_string.S sums the increments here */
    const int nsb = st->nsb;
    k[HK_HB] = (int32_t)st->out;
    k[HK_HALF] = nsb != 8; k[HK_HINC] = (int32_t)hinc; k[HK_SPREV] = v->sprev;
#else
    const int nsb = 8;
    k[HK_HB] = (int32_t)hb;
#endif
#ifdef HS_BOW_CHEAP
    k[HK_NIN1] = nsb >> 1;                                                 /* pass 1 runs pairs */
#else
    k[HK_NIN1] = nsb;
#endif
    k[HK_NIN2] = nsb;
    for (int sb = 1; sb <= 8; sb++) {
        const int32_t gf = GAIN(G_FLUTE, sb), gr = GAIN(G_REED, sb), gs = GAIN(G_SAW, sb);
        int32_t gn = GAIN(G_NOISE, sb);
        if (nsb != 8) gn = (gn * 23170) >> 15;
        const int32_t ga = gs + (gr >> 1), gp = gr >> 1;
        int32_t *const s = k + HK_SUB + 3 * (sb - 1);
        s[0] = (int32_t)(((uint32_t)ga << 16) | ((uint32_t)gp & 0xFFFFu));
        s[1] = (int32_t)((1u << 16) | ((uint32_t)gn & 0xFFFFu));
        s[2] = (int32_t)(((uint32_t)-gf << 16) | ((uint32_t)gf & 0xFFFFu));
        k[HK_GBQ + sb - 1] = (((fq * GAIN(G_NASAL, sb)) >> 14) * st->tmod[sb - 1]) >> 15;
    }
    kern(k);
    v->ph = (uint32_t)k[HK_PH] ^ 0x80000000u; v->rng = (uint32_t)k[HK_RNG] ^ 0x80000000u; v->mjit = k[HK_MJIT];
    v->tone = k[HK_TONE]; v->flp = k[HK_FLP]; v->fbp = k[HK_FBP];
#ifdef HS_HIGH_HALF
    v->sprev = k[HK_SPREV];
#endif
}
#define HS_STRING_ASM 1                  /* the 22.05 kHz string's increments and interpolation are hs_string.S's */
#define STRING_VARIANT(name, flute, rough) \
    static __attribute__((noinline)) void name(voice_t *v, const string_t *st, const int32_t *inc, const int32_t *g0, \
                                               const int32_t *dg) { string_asm(v, st, inc, g0, dg, hs_##name); }
#else
/* the four variants as functions of their own: kept small, the polyBLEP's rare corrections stay within a short
 * conditional jump of the loop (in one large render they land beyond it, behind a 5-cycle jump per test) */
#define STRING_VARIANT(name, flute, rough) \
    static __attribute__((noinline)) void name(voice_t *v, const string_t *st, const int32_t *inc, const int32_t *g0, \
                                               const int32_t *dg) { string_run(v, st, inc, g0, dg, flute, rough); }
#endif
STRING_VARIANT(string_plain, 0, 0)
STRING_VARIANT(string_f, 1, 0)
STRING_VARIANT(string_r, 0, 1)
STRING_VARIANT(string_fr, 1, 1)

/* kennel: hs_voice.S (Blackfin) runs the per-sample loops of drone_run, buzz_run and relic_run with the same
 * arithmetic; these functions fill its tables (word layouts in hs_voice.S) and keep the C loops as the host build and
 * the reference (HS_C_REF builds them on the Blackfin too) */
#if defined(__bfin__) && !defined(HS_C_REF)
#define HS_ASM 1
void hs_drone(int32_t *b, const uint32_t *ri, int32_t *k);
void hs_buzz(int32_t *b, const uint32_t *ri, int32_t *k);
void hs_relic(int32_t *h, int32_t *k);
void hs_relic_lite(int32_t *h, int32_t *k);   /* kennel: HS_RELIC_LITE */
/* kennel: hs_bus.S, the shared stage's loops (body_voice, bus_voice, bus_stage; word layouts there) */
void hs_body(int32_t *hb, int32_t *k, const int32_t *bl);
void hs_share(int32_t *hb, const int32_t *k, int taping);
void hs_tape_in(int32_t *acc, int32_t *rb, int32_t gin);
void hs_tape_out(const int32_t *rb, int32_t *o, const int32_t *k);
void hs_room(int32_t *rx, int32_t *k);
void hs_room_out(int32_t *o, const int32_t *k);
void hs_bells(int32_t *w, int32_t *k);        /* kennel: the bell partials into w (k: table, n, { ph, bi, e } x n) */
#define HS_LCG_C ((int32_t)0xDA29F01Bu)  /* 1013904223 / 1664525 mod 2^32: (r + c) * 1664525 = the LCG step */
#endif

/* ---------------------------------------------------------------- section 2 of render: the drone and the trompette
 * (functions of their own for the same reason as the string's) */
static __attribute__((noinline)) void drone_run(voice_t *v, const uint32_t *ri, const int32_t *g0, const int32_t *dg,
                                                const int32_t d)
{
    const uint32_t r0 = ri[0], f0 = r0 + (r0 >> 1);
    const uint32_t roff = (uint32_t)((int32_t)(r0 >> 15) * ROOT_DET), foff = (uint32_t)((int32_t)(f0 >> 15) * FIFTH_DET);
    const int32_t rdt = clampi((int32_t)((r0 + roff) >> 17), 1, 16384), fdt = clampi((int32_t)((f0 + foff) >> 17), 1, 16384);
    const int32_t rinv = memo_blep_inv(rdt, v->memo[M_RINV]), finv = memo_blep_inv(fdt, v->memo[M_FINV]);
    const int32_t fifth = tab5(T_DFIFTH, d), sub = tab5(T_DSUB, d);
    const int32_t fd = svf_coef(tab5(T_DHZ, d)), qd = tab5(T_DQ, d);
    uint32_t p0 = v->dph[0], p1 = v->dph[1], p2 = v->dph[2];
    int32_t dlp = v->dlp, dbp = v->dbp;
#ifdef HS_ASM
    {   /* kennel: the loop below in hs_voice.S */
        int32_t k[33];
        k[0] = (int32_t)roff; k[1] = (int32_t)foff;
        k[2] = (int32_t)(((uint32_t)rdt << 17) + 0x80000000u); k[3] = (int32_t)(((uint32_t)rdt << 18) - 1u);
        k[4] = (int32_t)(((uint32_t)fdt << 17) + 0x80000000u); k[5] = (int32_t)(((uint32_t)fdt << 18) - 1u);
        k[6] = fifth; k[7] = fd; k[8] = qd; k[9] = sub; k[10] = 65535; k[11] = -65535; k[12] = 0;
        k[13] = (int32_t)p2; k[14] = (int32_t)(uintptr_t)OP1_SINE_TABLE; k[15] = 0;
        k[16] = rdt; k[17] = rinv; k[18] = fdt; k[19] = finv;
        k[20] = (int32_t)(p0 ^ 0x80000000u); k[21] = (int32_t)(p1 ^ 0x80000000u); k[22] = dlp; k[23] = dbp;
        for (int sb = 1; sb <= 8; sb++) k[23 + sb] = GAIN(G_DRONE, sb);
        k[32] = 0;
        hs_drone(hb, ri, k);
        p0 = (uint32_t)k[20] ^ 0x80000000u; p1 = (uint32_t)k[21] ^ 0x80000000u; p2 = (uint32_t)k[13];
        dlp = k[22]; dbp = k[23];
    }
#else
    int32_t *b = hb;
    for (int sb = 1; sb <= 8; sb++) {
        const int32_t gd = GAIN(G_DRONE, sb);
        for (int n = 0; n < 8; n++, b++) {
            const uint32_t r = ri[(sb - 1) * 8 + n];
            p0 += r + roff;
            p1 += r + (r >> 1) + foff;
            p2 += r >> 1;
            const int32_t x = (blep_saw(p0, rdt, rinv) >> 1) + (m16(blep_saw(p1, fdt, finv), fifth) >> 16);
            /* resonant two-pole low-pass: Chamberlin SVF on half the signal, low-pass output x2 */
            dlp += (fd * dbp) >> 15;
            const int32_t hp = (x >> 1) - dlp - ((qd * dbp) >> 15);
            dbp += (fd * hp) >> 15;
            const int32_t y = dlp * 2 + (m16(sine(p2), sub) >> 15);
            *b += (clampi(y, -65535, 65535) * gd) >> 15;
        }
    }
#endif
    v->dph[0] = p0; v->dph[1] = p1; v->dph[2] = p2;
    v->dinc[0] = r0 + roff; v->dinc[1] = f0 + foff; v->dinc[2] = r0 >> 1;
    v->dlp = dlp; v->dbp = dbp;
}

static __attribute__((noinline)) void buzz_run(voice_t *v, const uint32_t *ri, const int32_t *g0, const int32_t *dg,
                                               const int32_t *senv, const int32_t bz)
{
    const int32_t base = tab5(T_BBASE, bz), depth = tab5(T_BSTROKE, bz), jit = tab5(T_BJIT, bz);
    const int32_t bnz = tab5(T_BNOISE, bz), btone = tab5(T_BTONE, bz);
    const int32_t fr = svf_coef(tab5(T_BHZ, bz)), rq = tab5(T_BQ, bz);
    const int32_t tlc = 7767;                                          /* trompette tone low-pass, ~1.9 kHz */
    /* an octave above the root, folded up to at least ~120 Hz: a trompette never sits lower, and below that the
     * slaps stop fusing into a buzz and come apart as ticks */
    int tsh = 1;
    while (tsh < 6 && (ri[0] << tsh) < TROMP_MIN) tsh++;
    uint32_t tph = v->tph, rng = v->rng;
    int32_t slap = v->slap, rattle = v->rattle, rlp = v->rlp, rbp = v->rbp, tlp = v->tlp, amp = v->amp;
#ifdef HS_ASM
    {   /* kennel: the loop below in hs_voice.S; the sub-blocks' amp, gz and tg computed here */
        int32_t k[51];
        k[0] = tsh; k[1] = HS_LCG_C; k[2] = 1664525; k[3] = -16384; k[4] = 32768; k[5] = tlc;
        k[6] = fr; k[7] = rq; k[8] = k[9] = 0; k[10] = tlp; k[11] = rlp; k[12] = rattle;
        k[13] = 65535; k[14] = -65535; k[15] = (28378 << 16) | 31727; k[16] = jit; k[17] = bnz; k[18] = 32767;
        /* k[15]: the slap's and the rattle's decays per sample, sqrt 3/4 and sqrt 15/16 (hs_voice.S) */
        k[19] = (int32_t)tph; k[20] = (int32_t)rng; k[21] = slap; k[22] = rbp; k[23] = 0;
        for (int sb = 0; sb < 8; sb++) {
            const int32_t drive = base + ((depth * senv[sb]) >> 15);
            amp += ((clampi(((drive - BUZZ_THRESH) * 5) >> 1, 0, 32767) - amp) * 9598 + 16384) >> 15;   /* 1 - sqrt 1/2 */
            k[24 + 3 * sb] = (btone * (19661 + ((amp * 13107) >> 15))) >> 15;
            k[25 + 3 * sb] = GAIN(G_BUZZ, sb + 1);
            k[26 + 3 * sb] = amp;
        }
        k[48] = k[49] = k[50] = 0;
        hs_buzz(hb, ri, k);
        tph = (uint32_t)k[19]; rng = (uint32_t)k[20]; slap = k[21]; rattle = k[12]; rlp = k[11]; rbp = k[22]; tlp = k[10];
    }
#else
    int32_t *b = hb;
    for (int sb = 0; sb < 8; sb++) {
        /* wheel pressure through the dog's threshold: 0 below it, full 0.4 above it */
        const int32_t drive = base + ((depth * senv[sb]) >> 15);
        amp += ((clampi(((drive - BUZZ_THRESH) * 5) >> 1, 0, 32767) - amp) * 9598 + 16384) >> 15;   /* 1 - sqrt 1/2 */
        const int32_t gz = GAIN(G_BUZZ, sb + 1);
        const int32_t tg = (btone * (19661 + ((amp * 13107) >> 15))) >> 15;   /* string tone 0.6 .. 1 with pressure */
        for (int n = 0; n < 8; n++, b++) {
            const uint32_t told = tph;
            tph += ri[sb * 8 + n] << tsh;
            if (UNLIKELY(tph < told)) {                                /* a new cycle: the dog slaps */
                const int32_t u = (int32_t)(lcg(&rng) >> 17);
                slap = m16(amp, 32767 - (m16(u, jit) >> 15)) >> 15;
                rattle = m16(slap, bnz) >> 15;
            }
            const int32_t nz = (int32_t)(lcg(&rng) >> 17) - 16384;
            const int32_t exc = slap + (m16(nz, rattle) >> 14);
            slap = m16(slap, 28378) >> 15;                             /* ~0.2 ms: sqrt 3/4 per sample */
            rattle = m16(rattle, 31727) >> 15;                         /* ~0.7 ms: sqrt 15/16 */
            /* bridge resonance: SVF band-pass on a quarter of the excitation, normalised (x rq) and x4 */
            rlp += (fr * rbp) >> 15;
            const int32_t hp = (exc >> 2) - rlp - ((rq * rbp) >> 15);
            rbp += (fr * hp) >> 15;
            const int32_t ring = (rbp * rq) >> 13;
            /* the trompette's own tone: a plain saw, darkened */
            tlp += (((((int32_t)(tph >> 16) - 32768) - tlp) >> 1) * tlc) >> 14;
            const int32_t y = ring + (exc >> 1) + ((tlp * tg) >> 15);
            *b += (clampi(y, -65535, 65535) * gz) >> 15;
        }
    }
#endif
    v->tph = tph; v->tinc = ri[HB - 1] << tsh; v->rng = rng;
    v->slap = slap; v->rattle = rattle; v->rlp = rlp; v->rbp = rbp; v->tlp = tlp; v->amp = amp;
}

/* ---------------------------------------------------------------- the Lo-Fi Relic stage, on hb (+-65535) at 44.1 kHz:
 * saturation, a lower sample rate, fewer bits with dither, dust, the tape head's low-pass, hiss, the transport's sag;
 * cross-faded with the dry voice from rm0 to rm1 across the block (so switching never clicks), unity for small signals */
static __attribute__((noinline)) void relic_run(voice_t *v, int32_t *h0, const int32_t age, const int32_t rm0, const int32_t rm1)
{
    if (!rm0) { v->relp = v->relp2 = v->rhold = v->rpop = 0; v->rph = 0; v->rrng ^= 0x2545F491u; }   /* switched on: no stale state */
    const int32_t drive = tab5(T_RDRIVE, age);                             /* <= 11059 */
    const int32_t make = (tab5(T_RMAKE, age) * (32767 - ((tab5(T_RSAG, age) * relic_sag) >> 15))) >> 15;
    const int32_t a = memo_lp_coef(tab5(T_RHZ, age), v->memo[M_RELIC]);
    const uint32_t rinc = (uint32_t)tab5(T_RRATE, age) * 97392u;          /* Hz -> Q32 per sample (2^32 / 44100) */
    const int sh = (int)tab5(T_RBITS, age);
    const int32_t mask = -(1 << sh), half = 1 << (sh - 1);
    const int32_t hiss = tab5(T_RHISS, age), popsz = tab5(T_RPOP, age);
    const uint32_t pthr = (uint32_t)tab5(T_RPOPS, age) * 97392u;           /* pops per second -> chance per sample */
    int32_t lp = v->relp, lp2 = v->relp2, hold = v->rhold, pop = v->rpop;
    uint32_t rph = v->rph, rng = v->rrng;
    const int32_t dm = rm1 - rm0;
#ifdef HS_ASM
    {   /* kennel: the loop below in hs_voice.S */
        int32_t k[29];
        k[0] = 1664525; k[1] = HS_LCG_C; k[2] = (int32_t)rinc; k[3] = sh - 32; k[4] = hold - half;
        k[5] = mask; k[6] = (int32_t)(((uint32_t)RELIC_POP << 16) | (uint32_t)a); k[7] = lp2; k[8] = -16384; k[9] = hiss; k[10] = make; k[11] = (int32_t)pthr;
        k[12] = drive; k[13] = 32767; k[14] = half; k[15] = popsz;
        /* k[6]: a (<= 32767) in the low half, RELIC_POP in the high half (hs_voice.S's pop decay) */
        for (int sb = 1; sb <= 8; sb++) k[15 + sb] = rm0 + ((dm * sb) >> 3);
        k[24] = (int32_t)rng; k[25] = lp; k[26] = pop; k[27] = (int32_t)rph;
#ifdef HS_RELIC_LITE
        k[4] = hold & mask; k[28] = hold;
        hs_relic_lite(h0, k);
        hold = k[28];
#else
        hs_relic(h0, k);
        hold = k[4] + half;
#endif
        lp = k[25]; lp2 = k[7]; pop = k[26]; rph = (uint32_t)k[27]; rng = (uint32_t)k[24];
    }
#else
    int32_t *h = h0;                                                       /* kennel: hb, or the shared bus */
    for (int sb = 1; sb <= 8; sb++) {
        const int32_t mix = rm0 + ((dm * sb) >> 3);
        for (int n = 0; n < 8; n++, h++) {
            const int32_t x = *h;
            /* warm saturation: a cubic, (3 s - s^3) / 2, on the driven signal limited to full scale. x >> 1 and the
             * drive fit int16, s and s^2 are within +-32767 */
            const int32_t s = clampi(m16(x >> 1, drive) >> 12, -32767, 32767);
            const int32_t s2 = m16(s, s) >> 15;
            const int32_t y = (3 * s - (m16(s2, s) >> 15)) >> 1;
            /* the lower sample rate: hold y until the phase wraps */
            const uint32_t old = rph;
            rph += rinc;
            if (rph < old) hold = y;
            /* fewer bits, dithered (so a fading note turns to grain, not to a gate), and the dust */
            const uint32_t r = lcg(&rng);
            if (UNLIKELY(r < pthr)) pop = (r & 0x100) ? popsz : -popsz;
#ifdef HS_RELIC_LITE
            /* kennel: the lighter tape (owner 2026-10-29): no dither, one tape-head pole; the LCG still steps (the
             * hiss and the pops) */
            const int32_t q = clampi((hold & mask) + pop, -32768, 32767);
            pop = m16(pop, RELIC_POP) >> 15;
            lp = clampi(lp + (m16((q - lp) >> 1, a) >> 14), -32768, 32767);
            const int32_t nz = (int32_t)(r >> 17) - 16384;
            const int32_t wet = (m16(clampi(lp + (m16(nz, hiss) >> 15), -32768, 32767), make) >> 14);
#else
            const int32_t q = clampi(((hold + (int32_t)(r >> (32 - sh)) - half) & mask) + pop, -32768, 32767);
            pop = m16(pop, RELIC_POP) >> 15;
            /* the tape head: two dark one-pole low-passes (inputs and states within int16, so their differences
             * >> 1 are too) */
            lp = clampi(lp + (m16((q - lp) >> 1, a) >> 14), -32768, 32767);
            lp2 = clampi(lp2 + (m16((lp - lp2) >> 1, a) >> 14), -32768, 32767);
            const int32_t nz = (int32_t)(r >> 17) - 16384;
            const int32_t wet = (m16(clampi(lp2 + (m16(nz, hiss) >> 15), -32768, 32767), make) >> 14);
#endif
            *h = x + ((((wet - x) >> 1) * mix) >> 14);
        }
    }
#endif
    v->relp = lp; v->relp2 = lp2; v->rhold = hold; v->rpop = pop; v->rph = rph; v->rrng = rng;
}

/* kennel: the five cathedral lines (contiguous int16 arrays, word-aligned, 916 entries) cleared with 32-bit stores */
#ifndef HS_SHARED_ROOM
typedef uint32_t __attribute__((may_alias)) hs_w32;
_Static_assert(__builtin_offsetof(voice_t, cb1) % 4 == 0 && (CB1 + CB2 + CA1 + CA2 + CR) % 2 == 0, "cat_clear");
_Static_assert(__builtin_offsetof(voice_t, cr) + CR * 2 == __builtin_offsetof(voice_t, cb1) + (CB1 + CB2 + CA1 + CA2 + CR) * 2,
               "cat_clear: the lines are contiguous");
static void cat_clear(voice_t *v)
{
    hs_w32 *w = (hs_w32 *)(void *)v->cb1;
    for (int i = 0; i < (CB1 + CB2 + CA1 + CA2 + CR) / 2; i++) w[i] = 0;
}
#endif

static void take_drone(int to)
{
    const int from = owner;
    owner = to;
    if (from < 0 || from == to) return;
    voice_t *a = &V[from], *b = &V[to];
    const int ahead = v_blk[from] == blk;                  /* the old owner has rendered this block already */
    for (int j = 0; j < 3; j++) b->dph[j] = a->dph[j] - (ahead ? a->dinc[j] * HB : 0u);
    b->dlp = a->dlp; b->dbp = a->dbp;
    b->tph = a->tph - (ahead ? a->tinc * HB : 0u);
    b->slap = a->slap; b->rattle = a->rattle; b->rlp = a->rlp; b->rbp = a->rbp; b->tlp = a->tlp; b->amp = a->amp;
}

#ifdef HS_SHARED_ROOM
/* ---------------------------------------------------------------- kennel: the shared stage (owner 2026-10-29)
 * To fit the unit's CPU budget: one room for all voices (HS_SHARED_ROOM), one relic tape for all voices
 * (HS_SHARED_RELIC), the body combs at 22.05 kHz (HS_CAT11K). A synth plugin only renders voices, and R246
 * multiplies each voice's output by its own stock envelope after render (bus += out x env), so:
 * - each voice keeps its body combs (short and close to the dry sound: no added delay) and adds its room input
 *   (its body and bells, x its level x envelope) into BUS.racc; with the shared tape it also adds its finished signal
 *   (after section 4) x envelope into BUS.acc;
 * - the first voice of the next block runs the room (and the tape) on those sums, one block (1.45 ms) late;
 * - every voice of that block adds a share of the result, share = its envelope / the sum of the voices' squared
 *   envelopes (floored at 0.5^2, so a lone release fades out over its second half instead of holding the tail up to
 *   its last sample): after R246's envelopes the shares add up to one. The tape saturates, so it hears the sum divided
 *   by the sum of the envelopes (a lone note drives it as before) and gets the envelopes' sum back after it.
 * Sound changes against the per-voice original: the room's tail rings on through other voices and its own release
 * instead of following each voice's envelope; the strokes' tremor no longer pulses the room; the tape hears a chord
 * together (it saturates and wobbles as one) after the darkening; with the tape shared the room hears the untaped
 * body; the room input is clamped where several loud voices meet. */
#ifdef HS_CAT11K
#define BB1 CB1                          /* the body combs at 22.05 kHz: 4.04 ms, 5.12 ms (hs_bus.S: L 178 / 226) */
#define BB2 CB2
#else
#define BB1 CB1
#define BB2 CB2
#endif
#define BUS_FLOOR 8192                   /* 0.5^2 in Q15 */
#define BUS_DEAD  32                     /* below this squared-envelope sum the voices are over: a new note starts clean */
#define ROOM_MAX  8036                   /* the room input's bound in section 3 (6826 + 1210) */
static struct {
    int32_t racc[QB];                    /* this block's room input: each voice's body and bells x level x envelope (x4) */
    int32_t out[HB];                     /* the shared stage's result, shared out to this block's voices */
    int32_t s_acc, e_acc, inv;           /* this block's sums of squared / plain end envelopes; 2^26 / the last (floored) */
    int32_t kc, g[2];                    /* CATHEDRAL smoothed; wet and room gains reached */
#ifdef HS_SHARED_RELIC
    int32_t acc[HB];                     /* this block's voices after section 4, x envelope */
    int32_t gd[8];                       /* the direct path per sub-block: 1 - the tape's mix */
#endif
    uint8_t fresh, live;                 /* live: BUS.out holds sound this block (else the voices skip their share) */
#ifdef HS_SHARED_RELIC
    uint8_t taping;                      /* the tape runs: the voices add to BUS.acc */
#endif
} BUS = { .fresh = 1 };
static voice_t BV;                       /* the shared room's filters and the tape (voice_t's fields of those names) */
static int16_t room_a1[RA1], room_a2[RA2], room_r[RR];   /* the shared room's lines (only BV's room: none per voice) */
static uint8_t bus_new_block;
#ifdef HS_SHARED_RELIC
static int32_t bus_rb[HB];               /* the tape's input and output */
#endif
#ifdef HS_ASM
static int32_t bus_room[QB + 1];         /* kennel: hs_room's room samples: [0] the last block's last, [1..32] this block's */
#endif

/* the bells of one voice (struck at its first render, as in section 3) into bellw at 22.05 kHz; 0 when silent */
static const int32_t *bell_voice(voice_t *v, const int32_t *inc, const int32_t c, const int32_t vel)
{
    if (v->bell_pending) {
        v->bell_pending = 0;
        const int32_t bl = (tab5(T_CBELL, c) * vel) >> 15;
        if (bl) { hs_bell_lvl = bl; hs_bell_n++; }
        for (int j = 0; j < 4; j++) { v->bph[j] = 0; v->benv[j] = bl ? ((bl * BELL_W[j]) >> 15) << 8 : 0; }
    }
#ifdef HS_ASM
    {   /* kennel: the partials' sample loop in hs_bus.S (hs_bells), the decays here */
        int32_t bk[14];
        int nb = 0, idx[4];
        for (int j = 0; j < 4; j++) {
            if (!v->benv[j]) continue;
            const uint32_t u = ((uint32_t)inc[0] >> 12) * BELL_RATIO[j];
            if (u >= BELL_CUT) { v->benv[j] = 0; continue; }
            bk[2 + 3 * nb] = (int32_t)v->bph[j]; bk[3 + 3 * nb] = (int32_t)(u << 2); bk[4 + 3 * nb] = v->benv[j] >> 8;
            idx[nb++] = j;
            int32_t en = v->benv[j];
            for (int r = 0; r < QB; r += 8) en -= (en >> (BELL_DECAY[j] - 3)) + 4;
            v->benv[j] = en < 256 ? 0 : en;
        }
        if (!nb) return 0;
        bk[0] = (int32_t)(uintptr_t)OP1_SINE_TABLE; bk[1] = nb;
        hs_bells(bellw, bk);
        for (int i = 0; i < nb; i++) v->bph[idx[i]] = (uint32_t)bk[2 + 3 * i];
        return bellw;
    }
#else
    int any = 0;
    for (int j = 0; j < 4; j++) {
        if (!v->benv[j]) continue;
        const uint32_t u = ((uint32_t)inc[0] >> 12) * BELL_RATIO[j];
        if (u >= BELL_CUT) { v->benv[j] = 0; continue; }
        if (!any) { for (int q = 0; q < QB; q++) bellw[q] = 0; any = 1; }
        uint32_t ph = v->bph[j];
        const uint32_t bi = u << 2;
        const int32_t e = v->benv[j] >> 8;
        for (int q = 0; q < QB; q++) { ph += bi; bellw[q] += m16(sine(ph), e) >> 15; }
        v->bph[j] = ph;
        int32_t en = v->benv[j];
        for (int r = 0; r < QB; r += 8) en -= (en >> (BELL_DECAY[j] - 3)) + 4;
        v->benv[j] = en < 256 ? 0 : en;
    }
    return any ? bellw : 0;
#endif
}

/* section 3 per voice: the body combs mixed in as in the original (dry x gdry + body x gbw), and the voice's room
 * input (its body pair / 8 + bells / 16, section 3's x) x its level x envelope x4 into BUS.racc */
static void body_voice(voice_t *v, const int32_t c, const int32_t *g0, const int32_t *dg, const int32_t go,
                       const int32_t *bl)
{
    const int32_t bfb = tab5(T_CBFB, c);
    int16_t *p1 = v->cb1 + v->ib1, *p2 = v->cb2 + v->ib2;
    int16_t *const e1 = v->cb1 + BB1, *const e2 = v->cb2 + BB2;
    int32_t bp = v->bodyp;
    const int32_t w = (go * clampi(OP1_VOICE_ENV[BLK - 1], 0, 32767)) >> 15;   /* level x envelope (the block's end) */
#if defined(HS_ASM) && defined(HS_CAT11K)
    {   /* kennel: the loop below in hs_bus.S */
        int32_t k[25];
        k[0] = (int32_t)(uintptr_t)v->cb1; k[1] = (int32_t)(uintptr_t)p1;
        k[2] = (int32_t)(uintptr_t)v->cb2; k[3] = (int32_t)(uintptr_t)p2;
        k[4] = bp; k[5] = w; k[6] = (int32_t)(uintptr_t)BUS.racc;
        for (int sb = 1; sb <= 8; sb++) {
            const int32_t gw = GAIN(G_WET, sb);
            k[5 + 2 * sb] = GAIN(G_DRY, sb);
            k[6 + 2 * sb] = (int32_t)(((uint32_t)((GAIN(G_BODY, sb) * gw) >> 15) << 16) | ((uint32_t)bfb & 0xFFFFu));
        }
        k[23] = k[24] = 0;
        hs_body(hb, k, bl);
        p1 = (int16_t *)(uintptr_t)k[1]; p2 = (int16_t *)(uintptr_t)k[3]; bp = k[4];
        (void)e1; (void)e2;
    }
#else
    for (int sb = 1; sb <= 8; sb++) {
        const int32_t gw = GAIN(G_WET, sb), gdry = GAIN(G_DRY, sb);
        const int32_t gbw = (GAIN(G_BODY, sb) * gw) >> 15;
        for (int n = 0; n < 8; n += 2) {
            const int m = (sb - 1) * 8 + n, q = m >> 1;
            int32_t b0, b1, rx;
#ifdef HS_CAT11K
            {   /* one comb sample per pair, on the pair's mean / 8; the body back to 44.1 kHz between pairs */
                const int32_t x = (hb[m] + hb[m + 1]) >> 4;
                const int32_t y1 = x + (m16(bfb, *p1) >> 15), y2 = x - (m16(bfb, *p2) >> 15);
                *p1++ = (int16_t)y1; *p2++ = (int16_t)y2;
                if (p1 == e1) p1 = v->cb1;
                if (p2 == e2) p2 = v->cb2;
                b1 = (y1 + y2) >> 1; b0 = (bp + b1) >> 1; bp = b1;
                rx = b1 >> 2;
            }
#else
            {
                int32_t x = hb[m] >> 3;
                int32_t y1 = x + (m16(bfb, *p1) >> 15), y2 = x - (m16(bfb, *p2) >> 15);
                *p1++ = (int16_t)y1; *p2++ = (int16_t)y2;
                if (p1 == e1) p1 = v->cb1;
                if (p2 == e2) p2 = v->cb2;
                b0 = (y1 + y2) >> 1;
                x = hb[m + 1] >> 3;
                y1 = x + (m16(bfb, *p1) >> 15); y2 = x - (m16(bfb, *p2) >> 15);
                *p1++ = (int16_t)y1; *p2++ = (int16_t)y2;
                if (p1 == e1) p1 = v->cb1;
                if (p2 == e2) p2 = v->cb2;
                b1 = (y1 + y2) >> 1;
                rx = (b0 + b1) >> 3;
            }
#endif
            if (bl) rx += bl[q] >> 4;
            BUS.racc[q] += (rx * w) >> 13;
            hb[m]     = ((hb[m] * gdry) >> 15) + (m16(b0, gbw) >> 12);
            hb[m + 1] = ((hb[m + 1] * gdry) >> 15) + (m16(b1, gbw) >> 12);
        }
    }
#endif
    v->ib1 = (uint16_t)(p1 - v->cb1); v->ib2 = (uint16_t)(p2 - v->cb2);
    v->bodyp = bp;
}

/* the first voice of a block: the room (and the tape) on the last block's sums, into BUS.out */
static void bus_stage(const int16_t *k)
{
    if (BUS.fresh) BUS.kc = knob(k, 3);
    BUS.kc += ((knob(k, 3) - BUS.kc) * 9598 + 16384) >> 15;              /* 1 - sqrt 1/2 a block */
    const int32_t c = BUS.kc;
    const int32_t t[2] = { tab5(T_CWET, c), tab5(T_CROOM, c) };
    if (BUS.fresh) { BUS.g[0] = t[0]; BUS.g[1] = t[1]; BUS.fresh = 0; }
    const int32_t g0[2] = { BUS.g[0], BUS.g[1] }, dg[2] = { t[0] - BUS.g[0], t[1] - BUS.g[1] };
    BUS.g[0] = t[0]; BUS.g[1] = t[1];
    const int32_t S = BUS.s_acc;
    BUS.s_acc = 0;
    BUS.inv = (int32_t)((1u << 26) / (uint32_t)(S > BUS_FLOOR ? S : BUS_FLOOR));   /* one division per block */
    const int32_t nE = BUS.e_acc > 8192 ? BUS.e_acc : 8192;               /* the envelopes' sum, at least 0.25 */
    BUS.e_acc = 0;
    (void)nE;
    int32_t *o = BUS.out;
    if (BUS.live) for (int m = 0; m < HB; m++) o[m] = 0;
    BUS.live = 0;
#ifdef HS_SHARED_RELIC
    {   /* the tape on the sum / the envelopes' sum, at about the level a voice gives it before its gain (x4) */
        int32_t *b = BUS.acc;
        const int32_t rm0 = BV.rmix;
        BV.rmix = clampi(rm0 + (op1_eng_page[1] != 0 ? RELIC_FADE : -RELIC_FADE), 0, 32767);
        const int32_t rm1 = BV.rmix;
        BUS.taping = (rm0 | rm1) != 0;
        if (rm0 | rm1) {
            BUS.live = 1;
            const int32_t gin = (int32_t)((1u << 29) / (uint32_t)nE);      /* 4 / the envelopes' sum, Q12 (<= 65536) */
#ifdef HS_ASM
            hs_tape_in(b, bus_rb, gin);                                    /* kennel: hs_bus.S; also clears BUS.acc */
#else
            for (int m = 0; m < HB; m++) bus_rb[m] = clampi((clampi(b[m], -32767, 32767) * gin) >> 12, -65535, 65535);
#endif
            relic_run(&BV, bus_rb, relic_age(k), rm0 ? 32767 : 0, 32767);
        }
#ifdef HS_ASM
        {   /* kennel: the tape's output in hs_bus.S */
            int32_t tk[10];
            tk[0] = nE >> 2; tk[9] = 0;
            for (int sb = 1; sb <= 8; sb++) {
                const int32_t mix = rm0 + (((rm1 - rm0) * sb) >> 3);
                BUS.gd[sb - 1] = 32767 - mix;
                tk[sb] = mix;
            }
            if (rm0 | rm1) hs_tape_out(bus_rb, o, tk);
        }
#else
        for (int sb = 1; sb <= 8; sb++) {
            const int32_t mix = rm0 + (((rm1 - rm0) * sb) >> 3);
            BUS.gd[sb - 1] = 32767 - mix;
            if (!(rm0 | rm1)) continue;
            for (int m = (sb - 1) * 8; m < sb * 8; m++)                   /* x the envelopes' sum / 4 */
                o[m] = (((clampi(bus_rb[m], -32767, 32767) * (nE >> 2)) >> 15) * mix) >> 15;
        }
        if (rm0 | rm1) for (int m = 0; m < HB; m++) b[m] = 0;
#endif
    }
#endif
    int32_t *rx = BUS.racc;
    voice_t *v = &BV;
    if (S < BUS_DEAD) v->cat_on = 0;                                      /* a new note after silence: no stale tail */
    if (!(g0[0] | t[0])) { v->cat_on = 0; for (int q = 0; q < QB; q++) rx[q] = 0; return; }
    BUS.live = 1;
    if (!v->cat_on) {
        for (int i = 0; i < RA1; i++) room_a1[i] = 0;
        for (int i = 0; i < RA2; i++) room_a2[i] = 0;
        for (int i = 0; i < RR; i++) room_r[i] = 0;
        v->ia1 = v->ia2 = v->ir = 0;
        v->rdl = v->roomp = v->dark = 0;
        v->cat_on = 1;
    }
    /* diffusers and the room (22.05 kHz), as section 3 */
    {
        const int32_t rfb = tab5(T_CRFB, c);
        const int32_t rdamp = memo_lp_coef(tab5(T_CDAMP, c) * 2, v->memo[M_DAMP]);
        int16_t *pa1 = room_a1 + v->ia1, *pa2 = room_a2 + v->ia2, *pr = room_r + v->ir;
        int16_t *const ea1 = room_a1 + RA1, *const ea2 = room_a2 + RA2, *const er = room_r + RR;
        int32_t rdl = v->rdl;
#ifdef HS_ASM
        {   /* kennel: the loop below in hs_bus.S (the room samples into bus_room[1..]) */
            int32_t rk[12];
            rk[0] = (int32_t)(uintptr_t)room_a1; rk[1] = (int32_t)(uintptr_t)pa1;
            rk[2] = (int32_t)(uintptr_t)room_a2; rk[3] = (int32_t)(uintptr_t)pa2;
            rk[4] = (int32_t)(uintptr_t)room_r; rk[5] = (int32_t)(uintptr_t)(pr == room_r ? er - 1 : pr - 1);
            rk[6] = rdl; rk[7] = rfb; rk[8] = rdamp; rk[9] = (int32_t)(uintptr_t)bus_room; rk[10] = ROOM_MAX; rk[11] = -ROOM_MAX;
            bus_room[0] = v->roomp;
            hs_room(rx, rk);
            pa1 = (int16_t *)(uintptr_t)rk[1]; pa2 = (int16_t *)(uintptr_t)rk[3]; pr = (int16_t *)(uintptr_t)rk[5];
            rdl = rk[6];
            (void)ea1; (void)ea2; (void)roomb;
        }
#else
        for (int q = 0; q < QB; q++) {
            const int32_t x = clampi(rx[q], -ROOM_MAX, ROOM_MAX);
            rx[q] = 0;
            int32_t a = *pa1;
            int32_t w = x - (m16(AP_G, a) >> 15);
            int32_t z = a + (m16(AP_G, w) >> 15);
            *pa1++ = sat16(w);
            a = *pa2;
            w = z - (m16(AP_G, a) >> 15);
            z = a + ((AP_G * w) >> 15);
            *pa2++ = sat16(w);
            const int32_t r = *pr;
            rdl += (((r - rdl) >> 1) * rdamp) >> 14;
            *pr++ = sat16(z + ((rfb * rdl) >> 15));
            roomb[q] = r;
            if (pa1 == ea1) pa1 = room_a1;
            if (pa2 == ea2) pa2 = room_a2;
            if (pr == er) pr = room_r;
        }
#endif
        v->ia1 = (uint16_t)(pa1 - room_a1); v->ia2 = (uint16_t)(pa2 - room_a2); v->ir = (uint16_t)(pr - room_r);
        v->rdl = rdl;
    }
    /* the room into BUS.out: x16 as section 3 (>> 11), / 4 for the input's x4 (>> 13); darkened as section 4 */
    {
        const int32_t kd = clampi((c - 8192) * 2, 0, 32767);
        const int32_t dcoef = 32767 - (((32767 - memo_lp_coef(tab5(T_CDARK, c), v->memo[M_DARK])) * kd) >> 15);
        int32_t rp = v->roomp, dark = v->dark;
#ifdef HS_ASM
        {   /* kennel: the loop below in hs_bus.S */
            int32_t ok[15];
            ok[0] = (int32_t)(uintptr_t)bus_room; ok[1] = 0; ok[2] = dark; ok[3] = dcoef;
            for (int sb = 1; sb <= 8; sb++)
                ok[3 + sb] = (((g0[1] + ((dg[1] * sb) >> 3)) * (g0[0] + ((dg[0] * sb) >> 3))) >> 15);
            ok[12] = 32767; ok[13] = -32767; ok[14] = 0;
            hs_room_out(o, ok);
            rp = bus_room[QB]; dark = ok[2];
        }
#else
        for (int sb = 1; sb <= 8; sb++) {
            const int32_t grw = (((g0[1] + ((dg[1] * sb) >> 3)) * (g0[0] + ((dg[0] * sb) >> 3))) >> 15);
            for (int n = 0; n < 8; n += 2) {
                const int m = (sb - 1) * 8 + n, q = m >> 1;
                const int32_t r1 = roomb[q], r0 = (rp + r1) >> 1;
                rp = r1;
                int32_t y = m16(r0, grw) >> 13;
                dark += (((y - dark) >> 1) * dcoef) >> 14;
                o[m] = clampi(o[m] + dark, -32767, 32767);
                y = m16(r1, grw) >> 13;
                dark += (((y - dark) >> 1) * dcoef) >> 14;
                o[m + 1] = clampi(o[m + 1] + dark, -32767, 32767);
            }
        }
#endif
        v->roomp = rp; v->dark = dark;
    }
}

static void bus_process(const int16_t *k)
{
    bus_stage(k);
}

/* each voice after section 4: (with the tape shared) its finished signal x envelope into the sum and its direct path
 * x (1 - the tape's mix); its share of the shared stage */
static void bus_voice(void)
{
    const int16_t *env = OP1_VOICE_ENV;
    const int32_t eend = clampi(env[BLK - 1], 0, 32767);
    BUS.s_acc += (eend * eend) >> 15;
    BUS.e_acc += eend;
#ifdef HS_SHARED_RELIC
    if (!BUS.live && !BUS.taping) return;
#else
    if (!BUS.live) return;
#endif
#ifdef HS_ASM
    {   /* kennel: the loop below in hs_bus.S */
        int32_t k[13];
        k[0] = BUS.inv; k[1] = (int32_t)(uintptr_t)BUS.out; k[3] = (int32_t)(uintptr_t)env; k[12] = 0;
#ifdef HS_SHARED_RELIC
        k[2] = (int32_t)(uintptr_t)BUS.acc;
        for (int i = 0; i < 8; i++) k[4 + i] = BUS.gd[i];
        hs_share(hb, k, BUS.taping);
#else
        k[2] = 0;
        for (int i = 0; i < 8; i++) k[4 + i] = 0;
        hs_share(hb, k, 0);
#endif
    }
#else
    for (int m = 0; m < HB; m++) {
        const int32_t e = clampi(env[2 * m], 0, 32767);
        int32_t y = hb[m];                                                 /* within +-26000 */
#ifdef HS_SHARED_RELIC
        if (BUS.taping) {
            BUS.acc[m] += (y * e) >> 15;
            y = (y * BUS.gd[m >> 3]) >> 15;
        }
#endif
        const int32_t sh = (e * BUS.inv) >> 15;                            /* the share, Q11 */
        hb[m] = clampi(y + ((BUS.out[m] * sh) >> 11), -26000, 26000);
    }
#endif
}
#elif defined(HS_SHARED_RELIC) || defined(HS_CAT11K)
#error "HS_SHARED_RELIC and HS_CAT11K need HS_SHARED_ROOM"
#endif

static void hs_note_on(void *self, const int16_t *k)
{
    const int vi = op1_voice_index(self);
    if (vi < 0 || vi >= NV) return;
    voice_t *v = &V[vi];
    /* OP1_PITCH_INC is not this voice's pitch yet (the firmware writes it before render): nothing here reads it */
    v->rng = 0x9E3779B9u ^ ((uint32_t)vi * 0x85EBCA6Bu) ^ (v->rng << 7);   /* varies from note to note */
    v->ph = lcg(&v->rng) & 0x3FFFFFFFu;     /* random start in the first quarter: no two voices line up */
    v->drift = v->drift_t = (((int32_t)(lcg(&v->rng) >> 24) - 128) >> 1) * 2;   /* +-64 = +-3.4 cents (Q1) */
    v->tone = v->flp = v->fbp = 0;
#ifdef HS_HIGH_HALF
    v->sprev = 0;
#endif
    v->dc8 = 0;
    for (int i = 0; i < 4; i++) v->ks[i] = knob(k, i);
    v_inc[vi] = 0;                          /* its pitch is unknown until it renders */
    if (owner < 0) {                        /* the first note ever: a fresh drone */
        v->dph[0] = 0; v->dph[1] = 0x55555555u; v->dph[2] = 0;
        v->dlp = v->dbp = 0;
        v->tph = 0; v->slap = v->rattle = v->rlp = v->rbp = v->tlp = v->amp = 0;
    }
    /* the owner keeps its drone state when it is re-triggered (a stolen or repeated note): no restart */
    v->mjit = 32767;
    v->bowg = 32767;
    v->bell_pending = 1;                    /* struck at the first render (the velocity and pitch are known there) */
    v->held = 1;
    v->fresh = 1;
}

static void hs_note_off(void *self)
{
    const int vi = op1_voice_index(self);
    if (vi < 0 || vi >= NV) return;
    V[vi].held = 0;
}

/* kennel: hs_post.S (Blackfin) runs the bound, section 4 and section 5 loops of hs_render with the same arithmetic;
 * the C loops below stay as the host build and the reference (HS_C_REF builds them on the Blackfin too) */
#if defined(__bfin__) && !defined(HS_C_REF)
#define HS_ASM 1
void hs_bound(int32_t *hb);
void hs_dark(int32_t *hb, int32_t *st, const int32_t *k);    /* st = { dark, dc8 }; k = { kd, dcoef, go[8], spare } */
void hs_up(const int32_t *hb, int16_t *out, int32_t *up);
#endif

static void hs_render(void *self, int16_t *out, const int16_t *k)
{
    const int vi = op1_voice_index(self);
    if (vi < 0 || vi >= NV) { for (int n = 0; n < BLK; n++) out[n] = 0; return; }
    voice_t *v = &V[vi];
    const int32_t *inc = OP1_PITCH_INC;

    /* shared, once per block: the block count; then this voice's pitch and the drone's owner */
    if (vi <= last_vi) {
        blk++; wheel_new_block = 1; relic_new_block = 1;
#ifdef HS_SHARED_ROOM
        bus_new_block = 1;
#endif
    }
    last_vi = vi;
    v_inc[vi] = (uint32_t)inc[0];
    {
        int best = -1;
        uint32_t lo = 0xFFFFFFFFu;
        for (int i = 0; i < NV; i++)
            if (V[i].held && v_inc[i] && v_inc[i] < lo) { lo = v_inc[i]; best = i; }
        if (best < 0 && owner < 0) best = vi;
        if (best == vi && owner != vi) take_drone(vi);     /* the new owner takes over when it renders */
    }

    /* knobs: ~6 ms smoothing (1 - sqrt 1/2 a block, four blocks); a fresh note starts at the current position */
    for (int i = 0; i < 4; i++) v->ks[i] += ((knob(k, i) - v->ks[i]) * 9598 + 16384) >> 15;
    const int32_t d = v->ks[0], s = v->ks[1], bz = v->ks[2], c = v->ks[3];
#ifdef HS_SHARED_ROOM
    if (bus_new_block) { bus_new_block = 0; bus_process(k); }             /* kennel: the shared stage, once a block */
#endif

    /* the wheel: stroke phase at the block start and its step per 44.1 kHz sample, from the beat clock */
    uint32_t sp = 0, sinc = 0;
    const int zone = stroke_zone(k);
    const uint32_t s4 = STROKES4[zone];                                    /* strokes per beat x4 */
    if (s4) {
        op1_beat_t bc;
        op1_beat_clock(&bc);
        const int32_t len_s = bc.len > 0 ? clampi(bc.len >> 14, 4410, 176400) : 22050;  /* samples per beat */
        static int32_t len_memo;                                           /* shared: the same clock for all voices */
        static uint32_t per_sample;
        if (len_memo != len_s) { len_memo = len_s; per_sample = 0xFFFFFFFFu / (uint32_t)len_s; }
        sinc = s4 & 3 ? (per_sample >> 2) * s4 : per_sample * (s4 >> 2);
        if (bc.len > 0 && bc.step > 0) {
            const int32_t next_s = clampi(bc.next >> 14, 0, len_s);
            static int32_t next_memo = -1, nlen_memo;
            static uint32_t bp15;                                          /* beat phase */
            if (next_memo != next_s || nlen_memo != len_s) {
                next_memo = next_s; nlen_memo = len_s;
                bp15 = 32768u - (((uint32_t)next_s << 15) / (uint32_t)len_s);
            }
            if (wheel_new_block) {                                         /* count beats: the phase wrapped */
                if (bp15 < beat_last) beat_n++;
                beat_last = bp15;
            }
            /* the stroke phase over a 4-beat span (for 1 bar and 1/2; whole strokes per beat drop the beat count) */
            sp = ((((beat_n & 3u) << 15) + bp15) * s4) << 15;
            if (bc.step != 0x100000) sinc = (sinc >> 10) * (uint32_t)(clampi(bc.step, 0, 0x400000) >> 10);
            wheel_free = sp;                                               /* the free phase continues from here */
        } else {
            if (wheel_new_block) wheel_free += sinc * (BLK / 2);           /* a block is 64 output frames */
            sp = wheel_free;
        }
        /* sinc is per output frame = per 44.1 kHz voice sample */
    }
    wheel_new_block = 0;
    int32_t senv[8];                                                       /* stroke envelope per sub-block */
    for (int sb = 0; sb < 8; sb++) senv[sb] = sinc ? stroke_env(sp + sinc * (uint32_t)(sb * 8 + 4)) : 32767;
    /* Crank & Tremor per sub-block: tlev = the level (Q15, 32768 = untouched), tmod = the nasal tone (32768 = none) */
    int32_t tlev[8], tmod[8];
    const int32_t trem = tab5(T_TREM, bz), ttone = tab5(T_TTONE, bz);
    if (sinc && (trem | ttone)) {
        const int32_t sharp = tab5(T_TSHARP, bz);
        for (int sb = 0; sb < 8; sb++) {
            const int32_t smooth = (32768 + sine(sp + sinc * (uint32_t)(sb * 8 + 4) + 0x40000000u)) >> 1;   /* cos */
            const int32_t te = smooth + (((senv[sb] - smooth) * sharp) >> 15);   /* 0 .. 32767, 32767 on a stroke */
            tlev[sb] = 32768 - ((trem * (32767 - te)) >> 15);
            tmod[sb] = 32768 + ((ttone * (te - 16384)) >> 14);
        }
    } else {
        for (int sb = 0; sb < 8; sb++) tlev[sb] = tmod[sb] = 32768;
    }

    /* Lo-Fi Relic: on while green's page 2 is (op1_eng_page[1]); the tape transport moves once per block */
    const int relic_on = op1_eng_page[1] != 0;
    const int32_t age = relic_age(k);
    if (relic_new_block) {
        relic_new_block = 0;
        relic_wow += 3419u << 10;                                          /* 0.55 Hz at 689 blocks a second */
        relic_wow2 += 1429u << 10;                                         /* 0.23 Hz */
        relic_flut += 40408u << 10;                                        /* 6.5 Hz */
        if ((lcg(&relic_rng) >> 24) < 1) relic_sag_t = (int32_t)(lcg(&relic_rng) >> 17);   /* a new sag every ~0.4 s */
        relic_sag += (relic_sag_t - relic_sag) >> 6;
    }
    int32_t wob = 0;                                                       /* this voice's pitch wobble, Q15 */
#ifdef HS_SHARED_RELIC
    (void)relic_on;
    const int32_t rm1 = BV.rmix;                                           /* kennel: the shared tape's mix */
#else
    const int32_t rm0 = v->rmix;
    v->rmix = clampi(rm0 + (relic_on ? RELIC_FADE : -RELIC_FADE), 0, 32767);
    const int32_t rm1 = v->rmix;
#endif
    if (rm1) {
        const int32_t w = (sine(relic_wow) * 3 + sine(relic_wow2)) >> 2;  /* two slow sines: irregular */
        const int32_t fl = sine(relic_flut) + ((int32_t)(lcg(&relic_rng) >> 18) - 8192);   /* flutter, a little rough */
        wob = (((tab5(T_RWOW, age) * w) >> 15) + ((tab5(T_RFLUT, age) * fl) >> 15)) * rm1 >> 15;
        hs_relic_wob = wob;
    }

    /* pitch: drift wanders to a new random target every ~0.15 s on average (~0.2 s glide) */
    if ((lcg(&v->rng) >> 23) < 5) v->drift_t = (((int32_t)(lcg(&v->rng) >> 24) - 128) * 3 >> 2) * 2;   /* +-96, Q1 */
    v->drift += (v->drift_t - v->drift) >> 7;
    const int32_t drift = (v->drift >> 1) + wob;
    /* the drift as a constant phase offset per sample for this block (the pitch's own per-sample changes, LFO or
     * bend, still come through inc[n]; the drift is a few cents of the block's first increment) */
    const uint32_t doff = (uint32_t)((int32_t)((uint32_t)inc[0] >> 15) * drift) << 1;   /* per 44.1 kHz sample */
    int32_t dt = (int32_t)((uint32_t)inc[0] >> 16);                       /* phase step per 44.1 kHz sample, Q15 */
    dt += (dt * drift) >> 15;
    dt = clampi(dt, 1, 16384);
#ifndef HS_HIGH_HALF
    const int32_t inv8 = memo_blep_inv(dt, v->memo[M_INV]);
#endif

    /* bow pressure from velocity: brighter and noisier when played harder */
    const int32_t vel = op1_voice_vel_gain(self);                          /* Q15 */
    const int32_t bow = 18000 + ((vel * 14767) >> 15);                     /* 0.55 .. 1.0 */

    /* bow lift after note off: k = c^2 x BOW_LIFT per block */
    if (!v->held) v->bowg -= (v->bowg * ((((c * c) >> 15) * BOW_LIFT) >> 15) + 65536) >> 17;
    const int32_t bowg = v->bowg;

    /* block targets. The string's loudness compensation is folded into its source gains (the filters are linear), so
     * it does not move the drone */
    int32_t t[NG];
    const int32_t lvl = (tab5(T_LEVEL, s) * bowg) >> 15;
    t[G_FLUTE] = (tab5(T_FLUTE, s) * lvl) >> 15;
    t[G_REED]  = (tab5(T_REED, s) * lvl) >> 15;
    t[G_SAW]   = (tab5(T_SAW, s) * lvl) >> 15;
    t[G_NOISE] = (((tab5(T_NOISE, s) * bow) >> 15) * lvl) >> 15;
    t[G_NASAL] = tab5(T_NASAL, s);
#ifdef HS_TEST_SOLO                          /* host tests only: 1 = the drone alone, 2 = the buzz alone */
    t[G_FLUTE] = t[G_REED] = t[G_SAW] = t[G_NOISE] = t[G_NASAL] = 0;
#endif
    t[G_OUT]   = (12360 * (12000 + ((vel * 20767) >> 15))) >> 15;        /* level x velocity; 12360: +4.0 dB (level pass
                                                                          * 2026-10-31, was 7800), the loudest preset ~1 dB under the +-26000 clamp */
    v->dgain = clampi(v->dgain + (vi == owner ? DRONE_FADE : -DRONE_FADE), 0, 32767);
    const int32_t own = (v->dgain * bowg) >> 15;
    t[G_DRONE] = (tab5(T_DLEVEL, d) * own) >> 15;
    t[G_BUZZ]  = bz < 1000 ? 0 : (tab5(T_BLEVEL, bz) * own) >> 15;     /* the bottom 3 %: off */
#if defined(HS_TEST_SOLO) && HS_TEST_SOLO == 1
    t[G_BUZZ] = 0;
#elif defined(HS_TEST_SOLO) && HS_TEST_SOLO == 2
    t[G_DRONE] = 0;
#endif
    t[G_WET]  = tab5(T_CWET, c);
    t[G_DRY]  = tab5(T_CDRY, c);
    t[G_BODY] = tab5(T_CBODY, c);
    t[G_ROOM] = tab5(T_CROOM, c);

    if (v->fresh) { for (int i = 0; i < NG; i++) v->g[i] = t[i]; v->fresh = 0; }
    int32_t g0[NG], dg[NG];
    for (int i = 0; i < NG; i++) { g0[i] = v->g[i]; dg[i] = t[i] - v->g[i]; v->g[i] = t[i]; }

    /* ======== 1. the string, 44.1 kHz, into hb (the loop specialised on whether the sine pair and the grit run) */
    {
        string_t st;
        st.pw = (uint32_t)tab5(T_PW, s) << 17;
        const int32_t f0x = (dt * 11025) >> 13;                            /* the pitch in Hz (dt * 44100 / 32768) */
#ifdef HS_HIGH_HALF
        /* kennel: above HS_HIGH_MIDI the string runs at 22.05 kHz: two 44.1 kHz increments per step (the pairs summed
         * here), dt and the drift offset doubled, the coefficients for the halved rate (twice the frequency for lp_coef,
         * t = pi fc / 11025 for the SVF) */
        const int high = (uint32_t)inc[0] > HS_MIDI_INC(HS_HIGH_MIDI);
#ifndef HS_STRING_ASM
        int32_t hinc[HB];
#endif
        const int32_t *sinc = inc;
        if (high) {
#ifndef HS_STRING_ASM
            for (int m = 0; m < HB / 2; m++) {
                hinc[2 * m] = (int32_t)((uint32_t)inc[4 * m] + (uint32_t)inc[4 * m + 1]);
                hinc[2 * m + 1] = (int32_t)((uint32_t)inc[4 * m + 2] + (uint32_t)inc[4 * m + 3]);
            }
            sinc = hinc;
#endif
            st.dt = clampi(2 * dt, 1, 16384);
            st.doff = doff << 1;
            st.ta = memo_lp_coef(((tab5(T_LPK, s) * f0x * (bow >> 5)) >> 10) * 2, v->memo[M_TONE]);
            const int32_t th = (clampi(tab5(T_FHZ, s), 20, 3500) * 4781 + 512) >> 10;   /* pi / 22050 in Q15 */
            st.ff = 2 * th - (((((th * th) >> 15) * th) >> 15) * 10923 >> 15);
            st.nsb = 4; st.out = hb + HB / 2;
        } else {
            st.dt = dt; st.doff = doff;
            st.ta = memo_lp_coef((tab5(T_LPK, s) * f0x * (bow >> 5)) >> 10, v->memo[M_TONE]);
            st.ff = svf_coef(tab5(T_FHZ, s));
            st.nsb = 8; st.out = hb;
        }
        st.inv8 = memo_blep_inv(st.dt, v->memo[M_INV]);
        st.grit = tab5(T_GRIT, bz);
        st.tmod = tmod;
#else
        st.ta = memo_lp_coef((tab5(T_LPK, s) * f0x * (bow >> 5)) >> 10, v->memo[M_TONE]);
        st.ff = svf_coef(tab5(T_FHZ, s));
        st.grit = tab5(T_GRIT, bz);
        st.dt = dt; st.inv8 = inv8; st.doff = doff; st.tmod = tmod;
        const int32_t *const sinc = inc;
#endif
        const int flute = (g0[G_FLUTE] | t[G_FLUTE]) != 0;                 /* the sine pair only while it is heard */
        const int rough = st.grit || v->mjit != 32767;                     /* grit on, or its last cycle to finish */
        if (flute) { if (rough) string_fr(v, &st, sinc, g0, dg); else string_f(v, &st, sinc, g0, dg); }
        else       { if (rough) string_r(v, &st, sinc, g0, dg); else string_plain(v, &st, sinc, g0, dg); }
#ifdef HS_HIGH_HALF
        if (!high) v->sprev = hb[HB - 1];
#ifndef HS_STRING_ASM
        else {                             /* back to 44.1 kHz in place: hb[2m], hb[2m + 1] from hb[32 + m] */
            int32_t p = v->sprev;
            for (int m = 0; m < HB / 2; m++) { const int32_t y = hb[HB / 2 + m]; hb[2 * m] = (p + y) >> 1; hb[2 * m + 1] = y; p = y; }
            v->sprev = p;
        }
#endif
#endif
    }

    /* ======== 2. drone and trompette (the owner, and a former owner while it fades out), 44.1 kHz, added to hb */
    uint32_t ri[HB];                                                       /* drone root increment per sample */
    const int run_drone = (g0[G_DRONE] | t[G_DRONE]) != 0, run_buzz = (g0[G_BUZZ] | t[G_BUZZ]) != 0;
    if (run_drone || run_buzz) {
        const int key = drone_key(k);
        /* per 44.1 kHz sample: a fixed key's increment; FOLLOW: an octave below the note, i.e. half its increment
         * over two 88.2 kHz samples */
        if (key) for (int m = 0; m < HB; m++) ri[m] = KEY_INC[key - 1];
        else for (int m = 0; m < HB; m++) ri[m] = ((uint32_t)inc[2 * m] + (uint32_t)inc[2 * m + 1]) >> 1;
        if (wob) for (int m = 0; m < HB; m++) ri[m] += (uint32_t)((int32_t)(ri[m] >> 15) * wob);   /* the relic's tape */
    }
    if (run_drone) {
        drone_run(v, ri, g0, dg, d);
    } else {
        /* not running: its phases did not move this block, so a copy from it needs no rewind */
        v->dinc[0] = v->dinc[1] = v->dinc[2] = 0;
    }
    if (run_buzz) {
        buzz_run(v, ri, g0, dg, senv, bz);
    } else {
        v->tinc = 0;
    }
#ifdef HS_ASM
    hs_bound(hb);                                                          /* kennel: the loop below, hs_post.S */
#else
    for (int m = 0; m < HB; m++) hb[m] = clampi(hb[m], -65535, 65535);    /* the voice, bounded once */
#endif
#ifndef HS_SHARED_RELIC
    if (rm0 | rm1) relic_run(v, hb, age, rm0, rm1);                            /* before the cathedral: the room hears it */
#endif

    /* ======== 3. cathedral: body combs (44.1 kHz), diffusers, room and bell (22.05 kHz), mixed back into hb */
#ifdef HS_SHARED_ROOM
    {   /* kennel: the body per voice, the room shared (bus_stage) */
        const int32_t *bw = bell_voice(v, inc, c, vel);
        if ((g0[G_WET] | t[G_WET]) || bw) {
            if (!v->cat_on) {
                for (int i = 0; i < CB1; i++) v->cb1[i] = 0;
                for (int i = 0; i < CB2; i++) v->cb2[i] = 0;
                v->ib1 = v->ib2 = 0;
                v->bodyp = 0;
                v->cat_on = 1;
            }
            body_voice(v, c, g0, dg, t[G_OUT], bw);
        } else {
            v->cat_on = 0;
        }
    }
#else
    if (v->bell_pending) {
        v->bell_pending = 0;
        const int32_t bl = (tab5(T_CBELL, c) * vel) >> 15;
        if (bl) { hs_bell_lvl = bl; hs_bell_n++; }
        for (int j = 0; j < 4; j++) { v->bph[j] = 0; v->benv[j] = bl ? ((bl * BELL_W[j]) >> 15) << 8 : 0; }
    }
    const int bells = (v->benv[0] | v->benv[1] | v->benv[2] | v->benv[3]) != 0;
    if ((g0[G_WET] | t[G_WET]) || bells) {
        if (!v->cat_on) {                                                  /* off since: clear, no stale tail */
            cat_clear(v);                                                  /* kennel: one 32-bit pass */
            v->rdl = v->bodyp = v->roomp = 0;
            v->cat_on = 1;
        }
        /* body combs on an eighth of the voice: |x| <= 8191, feedback <= 0.7, so |y| <= 27304 (int16, no saturation) */
        {
            const int32_t bfb = tab5(T_CBFB, c);                           /* <= 22938 */
            int16_t *p1 = v->cb1 + v->ib1, *p2 = v->cb2 + v->ib2;
            int16_t *const e1 = v->cb1 + CB1, *const e2 = v->cb2 + CB2;
            /* in runs that end where a line wraps (at most three per block), so the sample loop has no wrap test */
            for (int m = 0; m < HB; ) {
                int n = HB - m;
                if (e1 - p1 < n) n = e1 - p1;
                if (e2 - p2 < n) n = e2 - p2;
                for (const int end = m + n; m < end; m++) {
                    const int32_t x = hb[m] >> 3;
                    const int32_t y1 = x + (m16(bfb, *p1) >> 15);
                    const int32_t y2 = x - (m16(bfb, *p2) >> 15);          /* inverted: odd resonances, hollow */
                    *p1++ = (int16_t)y1;
                    *p2++ = (int16_t)y2;
                    bodyb[m] = (y1 + y2) >> 1;
                }
                if (p1 == e1) p1 = v->cb1;
                if (p2 == e2) p2 = v->cb2;
            }
            v->ib1 = (uint16_t)(p1 - v->cb1); v->ib2 = (uint16_t)(p2 - v->cb2);
        }
        /* the bell partials (22.05 kHz increments; partials above ~4.4 kHz left out), then the room */
        {
            uint32_t binc[4], bph[4];
            int32_t benv[4];
            int nb = 0;
            for (int j = 0; j < 4; j++) {
                if (!v->benv[j]) continue;
                const uint32_t u = ((uint32_t)inc[0] >> 12) * BELL_RATIO[j];
                if (u >= BELL_CUT) { v->benv[j] = 0; continue; }
                binc[nb] = u << 2; bph[nb] = v->bph[j]; benv[nb] = v->benv[j]; nb++;
            }
            /* the bells summed first, partial by partial (bellb stays all zero while no bell rings). Each term is at
             * most benv >> 8 <= 6299 (a strike <= 8999 x BELL_W), so the sum stays within +-19346 and its sixteenth
             * within +-1210; sine and benv >> 8 fit int16 */
            const int32_t *bl = bellb;
            if (nb) {
#ifdef HS_ASM
                int32_t bk[14];                                            /* kennel: hs_bells (hs_bus.S) */
                bk[0] = (int32_t)(uintptr_t)OP1_SINE_TABLE; bk[1] = nb;
                for (int j = 0; j < nb; j++) { bk[2 + 3 * j] = (int32_t)bph[j]; bk[3 + 3 * j] = (int32_t)binc[j]; bk[4 + 3 * j] = benv[j] >> 8; }
                hs_bells(bellw, bk);
                for (int j = 0; j < nb; j++) bph[j] = (uint32_t)bk[2 + 3 * j];
#else
                for (int q = 0; q < QB; q++) bellw[q] = 0;
                for (int j = 0; j < nb; j++) {
                    uint32_t ph = bph[j];
                    const uint32_t bi = binc[j];
                    const int32_t e = benv[j] >> 8;
                    for (int q = 0; q < QB; q++) { ph += bi; bellw[q] += m16(sine(ph), e) >> 15; }
                    bph[j] = ph;
                }
#endif
                bl = bellw;
            }
            const int32_t rfb = tab5(T_CRFB, c);
            const int32_t rdamp = memo_lp_coef(tab5(T_CDAMP, c) * 2, v->memo[M_DAMP]);   /* the coefficient at 22.05 kHz */
            int16_t *pa1 = v->ca1 + v->ia1, *pa2 = v->ca2 + v->ia2, *pr = v->cr + v->ir;
            int16_t *const ea1 = v->ca1 + CA1, *const ea2 = v->ca2 + CA2, *const er = v->cr + CR;
            int32_t rdl = v->rdl;
            for (int q = 0; q < QB; ) {                                    /* in runs between wraps, as above */
                int n = QB - q;
                if (ea1 - pa1 < n) n = ea1 - pa1;
                if (ea2 - pa2 < n) n = ea2 - pa2;
                if (er - pr < n) n = er - pr;
                for (const int end = q + n; q < end; q++) {
                    /* |x| <= 6826 + 1210; the first diffuser's w then stays within +-27697: its products fit int16 */
                    const int32_t x = ((bodyb[2 * q] + bodyb[2 * q + 1]) >> 3) + (bl[q] >> 4);
                    int32_t a = *pa1;
                    int32_t w = x - (m16(AP_G, a) >> 15);
                    int32_t z = a + (m16(AP_G, w) >> 15);
                    *pa1++ = sat16(w);
                    a = *pa2;
                    w = z - (m16(AP_G, a) >> 15);
                    z = a + ((AP_G * w) >> 15);                            /* w here can exceed int16 */
                    *pa2++ = sat16(w);
                    const int32_t r = *pr;
                    rdl += (((r - rdl) >> 1) * rdamp) >> 14;
                    *pr++ = sat16(z + ((rfb * rdl) >> 15));
                    roomb[q] = r;
                }
                if (pa1 == ea1) pa1 = v->ca1;
                if (pa2 == ea2) pa2 = v->ca2;
                if (pr == er) pr = v->cr;
            }
            /* bell envelopes: decayed per block (the shift per sample x 32) */
            for (int j = 0, i = 0; j < 4; j++) {
                if (!v->benv[j]) continue;
                v->bph[j] = bph[i++];
                int32_t e = v->benv[j];
                for (int r = 0; r < QB; r += 8) e -= (e >> (BELL_DECAY[j] - 3)) + 4;   /* 4 steps of 8 samples */
                v->benv[j] = e < 256 ? 0 : e;
            }
            v->ia1 = (uint16_t)(pa1 - v->ca1); v->ia2 = (uint16_t)(pa2 - v->ca2); v->ir = (uint16_t)(pr - v->cr);
            v->rdl = rdl;
        }
        /* mix: dry x gdry + (body x gbody + room x groom) x gwet; the body is an eighth of the voice and the room a
         * sixteenth of the body pair, so x8 (>> 12) and x16 (>> 11) restore the scale */
        {
            int32_t rp = v->roomp;
            for (int sb = 1; sb <= 8; sb++) {
                const int32_t gw = GAIN(G_WET, sb), gdry = GAIN(G_DRY, sb);
                const int32_t gbw = (GAIN(G_BODY, sb) * gw) >> 15, grw = (GAIN(G_ROOM, sb) * gw) >> 15;
                for (int n = 0; n < 8; n += 2) {
                    const int m = (sb - 1) * 8 + n, q = m >> 1;
                    const int32_t r1 = roomb[q], r0 = (rp + r1) >> 1;      /* the room back to 44.1 kHz */
                    rp = r1;
                    /* bodyb within +-27304, gbw <= 32767, the room int16, grw <= 39322 x 20316 >> 15 = 24380 */
                    hb[m]     = ((hb[m] * gdry) >> 15) + (m16(bodyb[m], gbw) >> 12) + (m16(r0, grw) >> 11);
                    hb[m + 1] = ((hb[m + 1] * gdry) >> 15) + (m16(bodyb[m + 1], gbw) >> 12) + (m16(r1, grw) >> 11);
                }
            }
            v->roomp = rp;
        }
    } else {
        v->cat_on = 0;
    }
#endif

    /* ======== 4. darkening low-pass (blended in from 25 % CATHEDRAL), DC blocker, level x velocity, the strokes
     * swelling the string; 44.1 kHz */
    {
        const int32_t kd = clampi((c - 8192) * 2, 0, 32767);
        const int32_t dcoef = 32767 - (((32767 - memo_lp_coef(tab5(T_CDARK, c), v->memo[M_DARK])) * kd) >> 15);   /* 32767: transparent */
        const int32_t swell = tab5(T_SWELL, bz);
#ifdef HS_ASM
        /* kennel: the sample loops below in hs_post.S, with the eight sub-block gains computed here */
        int32_t hk[11], st[2] = { v->dark, v->dc8 };
        hk[0] = kd; hk[1] = dcoef; hk[10] = 0;
        for (int sb = 1; sb <= 8; sb++) {
            int32_t go = GAIN(G_OUT, sb);
            go += (go * ((swell * senv[sb - 1]) >> 15)) >> 15;
            hk[sb + 1] = (go * tlev[sb - 1]) >> 15;
        }
        hs_dark(hb, st, hk);
        v->dark = st[0]; v->dc8 = st[1];
#else
        int32_t dark = v->dark, dc8 = v->dc8;
        for (int sb = 1; sb <= 8; sb++) {
            int32_t go = GAIN(G_OUT, sb);
            go += (go * ((swell * senv[sb - 1]) >> 15)) >> 15;
            go = (go * tlev[sb - 1]) >> 15;                                /* Crank & Tremor (32768: no change) */
            int32_t *h = hb + (sb - 1) * 8;
            if (kd) {
                for (int n = 0; n < 8; n++) {
                    int32_t y = clampi(h[n], -65535, 65535);
                    dark += (((y - dark) >> 1) * dcoef) >> 14; y = dark;
                    dc8 += (y * 256 - dc8) >> 9;
                    y -= dc8 >> 8;
                    h[n] = clampi((y * go) >> 15, -26000, 26000);          /* the interpolator then stays in int16 */
                }
            } else {
                for (int n = 0; n < 8; n++) {
                    int32_t y = clampi(h[n], -65535, 65535);
                    dark = y;
                    dc8 += (y * 256 - dc8) >> 9;
                    y -= dc8 >> 8;
                    h[n] = clampi((y * go) >> 15, -26000, 26000);
                }
            }
        }
        v->dark = dark; v->dc8 = dc8;
#endif
    }

#ifdef HS_SHARED_ROOM
    bus_voice();                                                           /* kennel: the shared stage's share */
#endif

    /* ======== 5. 2x interpolation to 88.2 kHz: each 44.1 kHz sample, then the half-band midpoint after it
     * ((-a + 9 b + 9 c - d) / 16, |out| <= 26000 x 20 / 16 = 32500: int16 without saturation) */
#ifdef HS_ASM
    hs_up(hb, out, v->up);                                                 /* kennel: the loop below, hs_post.S */
#else
    {
        int32_t a = v->up[0], b = v->up[1], cc = v->up[2];
        const int32_t *h = hb;
        int16_t *o = out;
        for (int m = 0; m < HB; m += 2, h += 2, o += 4) {                 /* two samples per pass */
            const int32_t d0 = h[0], d1 = h[1];
            o[0] = (int16_t)b;
            o[1] = (int16_t)((((b + cc) * 9) - a - d0) >> 4);
            o[2] = (int16_t)cc;
            o[3] = (int16_t)((((cc + d0) * 9) - b - d1) >> 4);
            a = cc; b = d0; cc = d1;
        }
        v->up[0] = a; v->up[1] = b; v->up[2] = cc;
    }
#endif
    v_blk[vi] = blk;
    hs_blocks++;
    if (vi == owner) {
        hs_stroke = senv[4];
        hs_dog = run_buzz ? (v->amp * t[G_BUZZ]) >> 15 : 0;
    }
}

/* presses: blue page 2 = the drone key, green page 2 = the Lo-Fi Relic (on while it is open; green then sets its age),
 * white page 2 = the stroke rate. Red has none: its page word would be white's (word 7) */
const op1_engine_t ENGINE_hollow_saint = { hs_note_on, hs_render, hs_note_off,
                                           { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_NONE } };

/* ======================= screen (GUI thread) =======================
 * hollow_saint.svg (tools/gen_screen.py; positions in hollow_saint_geom.h): a hurdy-gurdy seen from above in a gothic
 * arch. Moving parts:
 *   wheel, crank   turn while notes sound, surging on the wheel strokes, coasting to a stop after the last note
 *   s0 .. s4       the strings (drone, drone, melody, melody, trompette) part to show them vibrate: drones with DRONE,
 *                  the melody strings with STRINGS, the trompette with the dog's lift
 *   dog            rattles with the buzz
 *   kp0 .. kp5     a lit key for each held note (its pitch on the instrument's two chromatic octaves from G)
 *   p0 .. p7       the rose window's petals: CATHEDRAL lights them one by one
 *   halo, bell     the saint's halo widens with CATHEDRAL and flares at a bell strike; the bell swings
 *   m0 .. m3       the four knob meters; blue page 2 shows kbd + kdot (the drone key, or the FOLLOW arrow), white
 *                  page 2 shows r0 .. r4 (the stroke rate: steady, 1/4, 1/8, 1/8T, 1/16) */
#define ONE 1024
static void *n_wheel, *n_crank, *n_dog, *n_bell, *n_halo, *n_kbd, *n_kdot, *n_relic, *n_rage, *n_rage0, *n_dust;
static void *n_m[4], *n_p[8], *n_s[5], *n_kp[6], *n_r[NZONES], *n_rl[2];
static uint32_t sc_last_blocks, sc_bell_n, sc_seed = 0x2F6E2B1u;
static int32_t sc_act, sc_wang, sc_wvel, sc_bang, sc_bvel, sc_glow, sc_reel;

static inline uint32_t sc_rnd(void) { return (sc_seed = sc_seed * 1664525u + 1013904223u) >> 17; }   /* 0 .. 32767 */
static void sc_move(void *n, int32_t tx, int32_t ty)                 /* Q10 */
{
    if (!n) return;
    const int32_t m[9] = { ONE, 0, 0, 0, ONE, 0, tx, ty, ONE };
    op1_node_set_transform(n, m);
}
static void sc_scale(void *n, int32_t sx, int32_t sy, int32_t cx, int32_t cy)   /* about (cx, cy) in SVG units, Q10 */
{
    if (!n) return;
    const int32_t m[9] = { sx, 0, 0, 0, sy, 0, cx * (ONE - sx), cy * (ONE - sy), ONE };
    op1_node_set_transform(n, m);
}
static void sc_rotate(void *n, int32_t ang, int32_t cx, int32_t cy)  /* ang: Q16 of a turn */
{
    if (!n) return;
    const uint32_t i = ((uint32_t)ang >> 3) & 8191;
    op1_node_rotate(n, cx * ONE, cy * ONE, OP1_SINE_TABLE[i], OP1_SINE_TABLE[(i + 2048) & 8191]);
}
static void sc_show(void *n, int on) { if (n) op1_node_set_visible(n, on); }

/* the key a pitch falls on: semitones above G3, folded into the instrument's 24 keys */
static int sc_key(uint32_t inc)
{
    static const uint32_t G3 = 9544261u;                              /* G3 increment at 88.2 kHz (OP1_PITCH_INC) */
    /* half-semitone boundaries above G3 within an octave: G3 * 2^((k + 0.5) / 12), k = 0 .. 11 */
    static const uint32_t B[12] = { 9823929u, 10408090u, 11026988u, 11682686u, 12377375u, 13113372u,
                                    13893134u, 14719262u, 15594515u, 16521813u, 17504251u, 18545108u };
    int oct = 0;
    while (inc >= 2u * G3 && oct < 8) { inc >>= 1; oct++; }
    while (inc < G3 && oct > -8) { inc <<= 1; oct--; }
    int k = 0;
    while (k < 12 && inc >= B[k]) k++;
    const int semis = 12 * oct + k;                                    /* k = 12: the next G */
    return ((semis % 24) + 24) % 24;
}

static void hs_screen_init(void *scr)
{
    static const char *const m[4] = { "m0", "m1", "m2", "m3" };
    static const char *const p[8] = { "p0", "p1", "p2", "p3", "p4", "p5", "p6", "p7" };
    static const char *const st[5] = { "s0", "s1", "s2", "s3", "s4" };
    static const char *const kp[6] = { "kp0", "kp1", "kp2", "kp3", "kp4", "kp5" };
    static const char *const r[NZONES] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6" };
    n_wheel = op1_find_node(scr, "wheel");
    n_crank = op1_find_node(scr, "crank");
    n_dog = op1_find_node(scr, "dog");
    n_bell = op1_find_node(scr, "bell");
    n_halo = op1_find_node(scr, "halo");
    n_kbd = op1_find_node(scr, "kbd");
    n_kdot = op1_find_node(scr, "kdot");
    for (int i = 0; i < 4; i++) n_m[i] = op1_find_node(scr, m[i]);
    for (int i = 0; i < 8; i++) n_p[i] = op1_find_node(scr, p[i]);
    for (int i = 0; i < 5; i++) n_s[i] = op1_find_node(scr, st[i]);
    for (int i = 0; i < 6; i++) { n_kp[i] = op1_find_node(scr, kp[i]); sc_show(n_kp[i], 0); }
    for (int i = 0; i < NZONES; i++) { n_r[i] = op1_find_node(scr, r[i]); sc_show(n_r[i], 0); }
    n_relic = op1_find_node(scr, "relic");
    n_rage = op1_find_node(scr, "rage");
    n_rage0 = op1_find_node(scr, "rage0");
    n_dust = op1_find_node(scr, "dust");
    n_rl[0] = op1_find_node(scr, "rl0");
    n_rl[1] = op1_find_node(scr, "rl1");
    sc_show(n_kbd, 0); sc_show(n_kdot, 0);
    sc_show(n_relic, 0); sc_show(n_rage, 0); sc_show(n_rage0, 0); sc_show(n_dust, 0);
    sc_show(n_rl[0], 0); sc_show(n_rl[1], 0);
}

static void hs_screen_update(void *scr, int dt)
{
    const int slot = OP1_SCREEN_SLOT & 7;
    int16_t kn[4];
    op1_synth_knobs_live(slot, kn);
    int32_t k[4];
    for (int i = 0; i < 4; i++) k[i] = kn[i] < 0 ? 0 : kn[i];
    const int16_t *words = op1_synth_knobs(slot);

    /* activity: the audio has rendered since the last frame */
    const uint32_t blocks = hs_blocks;
    const int on = blocks != sc_last_blocks;
    sc_last_blocks = blocks;
    sc_act += ((on ? 32767 : 0) - sc_act) >> 3;

    /* the wheel: ~1 turn a second while playing, up to ~2.4 on a stroke, coasting down after the last note */
    const int32_t stroke = on ? hs_stroke : 0;
    const int32_t target = (sc_act * (1092 + ((((stroke * k[2]) >> 15) * 1500) >> 15))) >> 15;   /* Q16 turn / frame */
    sc_wvel += (target - sc_wvel) >> (target > sc_wvel ? 3 : 5);
    sc_wang += sc_wvel;
    sc_rotate(n_wheel, sc_wang, HS_WHEEL_X, HS_WHEEL_Y);
    sc_rotate(n_crank, sc_wang, HS_CRANK_X, HS_CRANK_Y);

    /* strings: part their arcs (0.15 .. ~2.6 x 0.6 px), with a shimmer */
    const int32_t dog = on ? hs_dog : 0;
    int32_t amp[5];
    amp[0] = amp[1] = (sc_act * ((k[0] * 2200) >> 15)) >> 15;
    amp[2] = amp[3] = (sc_act * (900 + ((k[1] * 1100) >> 15))) >> 15;
    amp[4] = ((sc_act * 300) >> 15) + ((dog * 2400) >> 15);
    for (int i = 0; i < 5; i++) {
        const int32_t sy = 154 + ((amp[i] * (24000 + (int32_t)(sc_rnd() >> 2))) >> 15);
        sc_scale(n_s[i], ONE, sy, 0, HS_STRING_Y[i]);
    }
    /* the dog rattles: a random lift up to ~1.5 px with the buzz */
    sc_move(n_dog, 0, -(int32_t)(((sc_rnd() * (uint32_t)dog) >> 15) * 1536 >> 15));

    /* lit keys: one per held voice */
    for (int i = 0; i < 6; i++) {
        const int lit = V[i].held && v_inc[i];
        sc_show(n_kp[i], lit);
        if (lit) sc_move(n_kp[i], HS_KEY_X[sc_key(v_inc[i])] * (ONE / 4), 0);
    }

    /* CATHEDRAL: petals, the halo, the bell */
    const int petals = (int)((k[3] * 8 + 16383) >> 15);
    for (int i = 0; i < 8; i++) sc_show(n_p[i], i < petals);
    if (hs_bell_n != sc_bell_n) {                                     /* a strike: the bell swings, the halo flares */
        sc_bell_n = hs_bell_n;
        sc_bvel += 120 + ((hs_bell_lvl * 400) >> 15);
        sc_glow = 32767;
    }
    sc_bvel -= sc_bang >> 7;                                           /* a heavy pendulum (~1.2 s), slowly damped */
    sc_bvel -= sc_bvel >> 6;
    sc_bang = clampi(sc_bang + sc_bvel, -4600, 4600);                  /* at most ~25 degrees */
    sc_rotate(n_bell, sc_bang, HS_BELL_X, HS_BELL_Y);
    sc_glow -= sc_glow >> 4;
    const int32_t hs = 870 + ((k[3] * 260) >> 15) + ((sc_glow * 120) >> 15);
    sc_scale(n_halo, hs, hs, HS_HALO_X, HS_HALO_Y);

    /* meters, and the page-2 icons in place of the blue and white meters */
    const int pg0 = op1_eng_page[0] != 0, pg1 = op1_eng_page[1] != 0, pg2 = op1_eng_page[2] != 0;
    for (int i = 0; i < 4; i++) {
        const int hide = (i == 0 && pg0) || (i == 1 && pg1) || (i == 2 && pg2);
        sc_show(n_m[i], !hide);
        if (!hide) sc_scale(n_m[i], (k[i] * ONE) >> 15, ONE, HS_METER_X[i], HS_METER_Y);
    }
    sc_show(n_kbd, pg0); sc_show(n_kdot, pg0);
    if (pg0) {
        const int key = drone_key(words);
        /* C .. B on the icon: naturals in the lower half, sharps on the dividing lines; FOLLOW above the arrow */
        static const int8_t KX[12] = { 0, 1, 1, 2, 2, 3, 4, 4, 5, 5, 6, 6 };
        static const uint8_t SHARP[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
        if (!key) sc_move(n_kdot, (HS_KBD_X - 8) * ONE, (HS_KBD_Y - 3) * ONE);
        else {
            const int pc = key - 1;
            const int32_t x = HS_KBD_X * ONE + KX[pc] * HS_KBD_W * ONE + (SHARP[pc] ? 0 : HS_KBD_W * ONE / 2);
            sc_move(n_kdot, x, (HS_KBD_Y + (SHARP[pc] ? 3 : 9)) * ONE);
        }
    }
    const int zone = stroke_zone(words);
    for (int i = 0; i < NZONES; i++) sc_show(n_r[i], pg2 && i == zone);

    /* Lo-Fi Relic (green page 2): the worn cassette, its reels turning while notes sound (unevenly, with the tape's
     * wobble), its age meter, and dust drifting in the cathedral */
    sc_show(n_relic, pg1); sc_show(n_rage, pg1); sc_show(n_rage0, pg1); sc_show(n_dust, pg1);
    sc_show(n_rl[0], pg1); sc_show(n_rl[1], pg1);
    if (pg1) {
        sc_reel += (sc_act * (1400 + hs_relic_wob * 8)) >> 15;          /* Q16 turn per frame */
        sc_rotate(n_rl[0], sc_reel, HS_REEL_X[0], HS_REEL_Y);
        sc_rotate(n_rl[1], sc_reel + 21845, HS_REEL_X[1], HS_REEL_Y);
        sc_scale(n_rage, (relic_age(words) * ONE) >> 15, ONE, HS_AGE_X, HS_METER_Y);
        if ((sc_rnd() & 7) == 0)                                       /* the dust shifts now and then */
            sc_move(n_dust, ((int32_t)(sc_rnd() & 3) - 1) * (ONE / 2), ((int32_t)(sc_rnd() & 3) - 1) * (ONE / 2));
    }

    op1_screen_draw(scr, dt);
}

const op1_screen_t SCREEN_hollow_saint = { hs_screen_init, hs_screen_update };
