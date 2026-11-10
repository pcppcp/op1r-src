/* moai: what the audio (moai.c) and the GUI thread (moai_ui.c: knobs, views, screen) share. The two
 * are separate compilation units so that the audio code lies together at the start of the image, in the same place
 * whatever the screen code grows to: on the OP-1 it runs from SDRAM through the 16 KB instruction cache
 * (docs/PERFORMANCE.md, v3.26). The shared state is defined in moai.c. */
#ifndef MOAI_H
#define MOAI_H
#include "op1_plugin.h"
#include "moai_tables.h"

#define CHUNK       32                  /* filter coefficient / contour update interval (samples: 0.363 ms at 88.2 kHz) */
#define F           5461                /* internal full scale of the ladder: states and differences stay in 16 bits */
#define CLIP_L      8192                /* saturator limit = 1.5 F (a power of two: clamped by a saturating shift) */
#define CLIP_K      2731                /* 2^39 / (3 L^2): x^2 / (3 L^2) in Q15 = ((x^2 >> 12) * CLIP_K) >> 12 */
#define TRACK_SHIFT 1                   /* keyboard tracking 1/2 */
#define MAX_POS     (MM_G_N * 64 - 1)   /* cutoff position range, Q10 octaves above 20 Hz */
#define OUT_GAIN    23851               /* ladder output (F) -> 16-bit sample, Q12 (as v1: F -> 0.97 full scale) */

typedef struct { int32_t s1, s2, s3, s4, t[4]; } lad_t;                /* integrator states, Q16 (value << 16); t: the
                                                                           stages' clipped outputs (transistor ladder) */
typedef struct { int32_t ggk, c12, c34, invk; } lad_coef_t;             /* Q15 pairs, per chunk: lo | hi << 16 (fewer registers) */

typedef struct {
    uint32_t ph[3];                     /* oscillator phases, 2^32 = one cycle (free running) */
    int32_t  drift[3];                  /* slow pitch drift per oscillator, ratio - 1 in Q16 (about +-2 cents) */
    lad_t    lad;
    int32_t  env;                       /* contour, Q22 (1.0 = 1 << 22) */
    int32_t  pos;                       /* smoothed cutoff position, Q10 octaves above 20 Hz */
    uint32_t rng;                       /* drift */
    uint32_t nrng;                      /* noise */
    uint32_t key_inc;                   /* the key's phase increment the next two were computed for */
    uint32_t key_rcp;                   /* 2^30 / (key_inc >> 8): polyBLEP reciprocal (1 % is plenty: shared by all 3) */
    int32_t  key_oct;                   /* the key's pitch in octaves from C4, Q10 */
    uint8_t  stage;                     /* 0 attack, 1 decay / sustain, 2 release */
    uint8_t  fresh;                     /* first block of a note: no smoothing */
    uint8_t  tr;                        /* the last block ran the transistor stages (lad.t valid) */
    uint8_t  dtick;                     /* the drift steps on every other block (the design's 2.9 ms) */
    uint32_t serial;                    /* mm_serial at this voice's last render (counts the sounding voices) */
    int32_t  g3;                        /* osc 3 gain, Q15: fades to 0 while osc 3 is switched off for CPU */
    int32_t  yprev;                     /* the ladder's last output (feedback), Q16 */
    int32_t  dc[3];                     /* output DC blockers (after the filter, after the saturation): the DC they remove,
                                           Q16; [2]: the saturation's low-pass state */
} mm_voice_t;

extern mm_voice_t V[OP1_VOICES];

static inline int32_t clampk(int32_t k) { return k < 0 ? 0 : (k > 32767 ? 32767 : k); }
static inline uint32_t rnd(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }

/* ---- oscillator setups ("DNA": the home view's blue knob) ------------------------------------------------- */

/* The Minimoog's waves (osc 3 has a reverse saw instead of the shark tooth; any oscillator may take any wave here) */
enum { W_TRI, W_SHARK, W_SAW, W_RSAW, W_SQUARE, W_WIDE, W_NARROW, N_WAVES, W_NOISE = N_WAVES };
/* ranges: off, LO, 32' .. 2' (8' plays the key). LO is 6 octaves under 8' (4 Hz at C4, still following the key as
   on the Model D) and its frequency knob spans 4x as far (0.65 .. 26 Hz at C4): in the mix it ticks and thumps (and
   pings the filter with emphasis up); osc 3 in LO is the modulation source (see "modulation" below) */
enum { R_OFF, R_LO, R32, R16, R8, R4, R2, N_RANGES };
static const int8_t RANGE_OCT[N_RANGES] = { 0, -6, -2, -1, 0, 1, 2 };
#define LO_FREQ_X 4                     /* LO: the frequency knob's cents count 4x */

/* One setup = per oscillator: range, wave, interval (semitones), volume (quarters); the noise (quarters); then in %
 * of the knob: cutoff, emphasis, contour, detune, drive, saturation, feedback. Turning the setup knob sets all of them
 * (v3.19: before, a setup kept the cutoff .. detune of the sound before it), so a number always sounds the same; the
 * views edit them from there. The home knobs, detune and drive go into the stored words when you turn to it, as a
 * preset's (mm_ui.h), never when a sound loads. */
typedef struct { uint8_t r, w; int8_t semi; uint8_t vol; } osc_t;
typedef struct { osc_t o[3]; uint8_t noise, cut, emp, con, det, drv, sat, fb; } setup_t;   /* (names only in the
                                                                        * comments: the plugin shows numbers, docs/PRESETS.md) */
#define N_SETUPS 32
#define OFF_ { R_OFF, W_SAW, 0, 0 }
extern const setup_t SETUPS[N_SETUPS];
#ifdef MM_DEFINE_TABLES                         /* the one copy of the tables: moai.c */
const setup_t SETUPS[N_SETUPS] = {
    /* the default of v1 .. v3.11 */
    { { { R8,  W_SAW, 0, 4 },    { R8,  W_SAW, 0, 4 },    OFF_ },                  0, 27, 49, 40, 35,  0,  0,  0 },  /* CLASSIC */
    /* basses */
    { { { R16, W_SAW, 0, 4 },    { R16, W_SAW, 0, 4 },    { R32, W_SAW, 0, 3 } },  0, 24, 25, 35, 30, 25,  0,  0 },  /* MOOG BS: the archetype: three saws, 16' 16' 32' */
    { { { R16, W_TRI, 0, 4 },    { R16, W_SQUARE, 0, 2 }, { R32, W_TRI, 0, 3 } },  0, 38,  0, 30, 20,  0,  0,  0 },  /* SUB BASS */
    { { { R16, W_SQUARE, 0, 4 }, { R16, W_SAW, 0, 4 },    { R32, W_SQUARE, 0, 3 } }, 0, 28, 40, 30, 20, 20,  0,  0 },  /* FUNK BS: 70s funk: square + saw */
    { { { R16, W_NARROW, 0, 4 }, { R16, W_WIDE, 0, 4 },   { R32, W_SQUARE, 0, 3 } }, 0, 30, 20, 30, 20, 15,  0,  0 },  /* PULSE BS */
    { { { R16, W_SAW, 0, 4 },    OFF_,                    OFF_ },                  0, 18, 85, 30,  0, 45, 25,  0 },  /* ACID: one oscillator: resonance does the work */
    { { { R16, W_SAW, 0, 4 },    { R8,  W_SAW, 0, 2 },    { R32, W_SQUARE, 0, 3 } }, 0, 36, 20, 30, 20, 15,  0,  0 },  /* OCT BASS */
    { { { R16, W_SAW, 0, 4 },    { R16, W_SAW, 7, 3 },    { R32, W_SAW, 0, 3 } },  0, 25, 25, 35, 25, 20,  0,  0 },  /* FIFTH BS */
    /* leads */
    { { { R8,  W_SAW, 0, 4 },    { R8,  W_SAW, 0, 4 },    { R16, W_SAW, 0, 2 } },  0, 45, 30, 40, 35, 30,  0,  0 },  /* SAW LEAD: the 70s solo lead */
    { { { R8,  W_SQUARE, 0, 4 }, { R8,  W_SAW, 0, 4 },    { R16, W_SQUARE, 0, 2 } }, 0, 45, 20, 40, 30, 20,  0,  0 },  /* SQ LEAD */
    { { { R8,  W_SAW, 0, 4 },    { R8,  W_SAW, 7, 3 },    { R16, W_SAW, 0, 2 } },  0, 50, 20, 45, 30, 25,  0,  0 },  /* 5TH LEAD: osc 2 a fifth up */
    { { { R8,  W_SAW, 0, 4 },    { R4,  W_SAW, 0, 3 },    { R2,  W_SAW, 0, 1 } },  0, 45, 50, 40, 35, 70, 45, 25 },  /* SCREAMER */
    { { { R8,  W_TRI, 0, 4 },    { R8,  W_SAW, 0, 2 },    OFF_ },                  0, 35, 10, 45, 30,  0,  0,  0 },  /* SOFT LD */
    { { { R8,  W_TRI, 0, 4 },    { R4,  W_TRI, 0, 1 },    OFF_ },                  1, 50, 30, 50, 20,  0,  0,  0 },  /* WHISTLE */
    /* woodwinds, brass, strings, pads */
    { { { R8,  W_TRI, 0, 4 },    { R8,  W_SQUARE, 0, 1 }, OFF_ },                  2, 40, 10, 75, 20,  0,  0,  0 },  /* FLUTE */
    { { { R8,  W_SQUARE, 0, 4 }, { R8,  W_SQUARE, 0, 3 }, { R16, W_TRI, 0, 2 } },  0, 35, 10, 70, 15, 10,  0,  0 },  /* CLARINET */
    { { { R8,  W_NARROW, 0, 4 }, { R8,  W_NARROW, 0, 3 }, { R16, W_WIDE, 0, 1 } }, 0, 45, 15, 70, 15, 10,  0,  0 },  /* OBOE */
    { { { R8,  W_WIDE, 0, 4 },   { R8,  W_SAW, 0, 3 },    { R16, W_NARROW, 0, 2 } }, 0, 45, 15, 70, 20, 15, 15,  0 },  /* REED */
    { { { R8,  W_SAW, 0, 4 },    { R8,  W_SAW, 0, 4 },    { R8,  W_WIDE, 0, 3 } }, 0, 30, 10, 70, 30, 35, 15,  0 },  /* BRASS */
    { { { R8,  W_SAW, 0, 4 },    { R8,  W_SAW, 0, 4 },    { R4,  W_SAW, 0, 2 } },  0, 55,  0, 90, 60,  0,  0,  0 },  /* STRINGS */
    { { { R8,  W_TRI, 0, 4 },    { R8,  W_SAW, 0, 3 },    { R16, W_TRI, 0, 3 } },  0, 40,  5, 90, 45,  0,  0,  0 },  /* SOFT PAD */
    { { { R8,  W_SHARK, 0, 4 },  { R8,  W_SHARK, 0, 4 },  { R16, W_RSAW, 0, 3 } }, 0, 45, 20, 50, 35, 20,  0, 20 },  /* SHARK */
    /* organs and keys */
    { { { R16, W_TRI, 0, 4 },    { R8,  W_TRI, 0, 3 },    { R4,  W_TRI, 0, 3 } },  0, 65,  0,  0, 10, 10, 30,  0 },  /* ORGAN: drawbars 16' 8' 4' */
    { { { R8,  W_NARROW, 0, 4 }, { R4,  W_NARROW, 0, 2 }, OFF_ },                  0, 55, 30, 20, 10, 20, 25,  0 },  /* CLAV */
    { { { R16, W_SQUARE, 0, 4 }, { R8,  W_SQUARE, 0, 4 }, { R4,  W_SQUARE, 0, 3 } }, 0, 45, 10, 35, 20, 15,  0,  0 },  /* SQ STACK */
    /* intervals and chords: one key, three notes */
    { { { R8,  W_SAW, 0, 4 },    { R4,  W_SAW, 0, 3 },    { R16, W_SAW, 0, 3 } },  0, 45, 15, 45, 30, 15,  0,  0 },  /* OCTAVES */
    { { { R8,  W_SAW, 0, 4 },    { R8,  W_SAW, 5, 4 },    { R16, W_SAW, 0, 2 } },  0, 45, 15, 45, 25, 15,  0,  0 },  /* FOURTHS */
    { { { R8,  W_SAW, 0, 4 },    { R8,  W_SAW, 3, 4 },    { R8,  W_SAW, 7, 4 } },  0, 45, 10, 50, 25, 10,  0,  0 },  /* MINOR */
    { { { R8,  W_SAW, 0, 4 },    { R8,  W_SAW, 4, 4 },    { R8,  W_SAW, 7, 4 } },  0, 45, 10, 50, 25, 10,  0,  0 },  /* MAJOR */
    /* noise */
    { { { R8,  W_SAW, 0, 4 },    { R8,  W_SAW, 0, 4 },    OFF_ },                  2, 40, 20, 60, 30,  0,  0,  0 },  /* BREATH */
    { { { R8,  W_TRI, 0, 4 },    OFF_,                    OFF_ },                  3, 40, 40, 15,  0, 25,  0,  0 },  /* PERC */
    { { OFF_,                    OFF_,                    OFF_ },                  4, 45, 85, 95,  0,  0,  0,  0 },  /* WIND: noise through the filter: raise emphasis */
};
#endif

/* The presets (docs/modern_patch_library.csv, generated by gen_presets.py) are stored after the setups:
 * entries 0..31 are the setups, 32.. the presets. A preset is a whole sound: its oscillators, and in % of
 * the knob its cutoff, emphasis, contour, detune and drive (written into the stored words when you turn to it, as a
 * setup writes its drive: mm_ui.h) and its saturation, contour amount and modulation (session values, derived from
 * the entry like a setup's character, so a saved sound gets them back). */
typedef struct { osc_t o[3]; uint8_t cut, emp, con, det, drv, sat, amt, pit, flt; } preset_t;
#include "moai_presets.h"
#undef OFF_
_Static_assert(N_PRESETS <= 4 * (N_SETUPS - 1), "the setup word holds 4 presets per slot");
#define N_ENTRIES (N_SETUPS + N_PRESETS)
_Static_assert(N_ORDER == N_ENTRIES, "gen_presets.py: one number per entry");
/* The blue knob does not step through the entries but through their numbers (000 .., docs/PRESETS.md): one shuffled
 * list of setups and presets, the initial sound at 000, wrapping around at both ends. */
static inline int entry_step(int e, int n)
{
    int p = (MM_POS[e] + n) % N_ENTRIES;
    return MM_ORDER[p < 0 ? p + N_ENTRIES : p];
}
/* The setup word: a setup is its 1024-wide slot (as since v2: any value in it), a preset one of four odd values in a
 * slot (offsets 1013 .. 1019: preset p in slot p % 31; not the last slot, whose values sit at the knob's end where the
 * core clips a turn up), which a sound saved before presets existed practically never holds (the knob was turned by
 * whole clicks). The blue knob steps through the entries (mm_ui.h) and writes these exact values. */
#define PRESET_OFS   1013
#define PRESET_SLOTS (N_SETUPS - 1)
int entry_of(int32_t w);
int32_t entry_word(int e);
static inline const preset_t *preset_of(int e) { return e >= N_SETUPS ? &PRESETS[e - N_SETUPS] : 0; }
static inline int32_t pct(int v) { return v * 32767 / 100; }

/* a wave as weights: wave = c1 * saw(p) - c2 * saw(p + w) + c3 * tri(p); kind = which parts the loop computes */
typedef struct { int32_t c1, c2, c3; uint32_t w; int kind; } wave_t;
enum { K_SAW, K_PULSE, K_TRI, K_SHARK, K_NOISE };
void wave_of(wave_t *o, int wave);

/* the same shapes without the edge correction, +-32768 (the screen draws them) */
static inline int32_t wave_raw(const wave_t *w, uint32_t p)
{
    int32_t a = (int32_t)(p ^ 0x80000000u) >> 16;
    int32_t s = (w->c1 * a) >> 15;
    if (w->c2) s -= (w->c2 * ((int32_t)((p + w->w) ^ 0x80000000u) >> 16)) >> 15;
    if (w->c3) s += (w->c3 * (2 * (a < 0 ? -a : a) - 32768)) >> 15;
    return s;
}

/* log2(v), v >= 1, in Q10 */
int32_t ulog2_q10(uint32_t v);

/* ---- parameters ------------------------------------------------------------------------------------------- *
 * Seven values are saved with the sound (the engine's knob words): the four home knobs (words 0..3: setup, cutoff,
 * emphasis, contour) and three analog settings (words 5..7: detune, drive, drift). Everything else lives in P[] for
 * the session: the oscillators come from the setup, attack / decay / sustain / release / amount from the contour
 * knob, until a view edits them; turning the setup (or the contour knob) sets them again. After loading a sound they
 * are derived anew from its seven values. P[] also mirrors the home knobs while another view borrows the knobs. */
enum {
    P_SETUP, P_CUTOFF, P_EMPH, P_CONTOUR,                /* home knobs (words 0..3) */
    P_DETUNE, P_DRIVE, P_DRIFT,                          /* analog (words 5..7) */
    P_O1_RANGE, P_O1_WAVE, P_O1_FREQ, P_O1_VOL,          /* oscillators (session, from the setup) */
    P_O2_RANGE, P_O2_WAVE, P_O2_FREQ, P_O2_VOL,
    P_O3_RANGE, P_O3_WAVE, P_O3_FREQ, P_O3_VOL,
    P_NOISE, P_FEEDBACK,                                 /* session */
    P_AMOUNT, P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE,   /* contour (session, from the contour knob) */
    P_SAT,                                               /* session: saturation after the filter */
    P_MOD_MIX, P_MOD_PITCH, P_MOD_FILT,                  /* session: modulation (osc 3 in LO / noise), the noise row */
    P_N
};
#define P_O(i, f) (P_O1_RANGE + 4 * (i) + (f))           /* f: 0 range, 1 wave, 2 freq, 3 volume */
/* value kinds: 0..32767 knob range; a step index 0..n-1; cents -800..800 (semitone turns, fine with shift) */
enum { K_CONT, K_STEP, K_CENTS };
typedef struct { char name[9]; uint8_t kind, n; } pdesc_t;
extern const pdesc_t PD[P_N];
#ifdef MM_DEFINE_TABLES
const pdesc_t PD[P_N] = {
    { "SETUP", K_CONT, 0 }, { "CUTOFF", K_CONT, 0 }, { "EMPHASIS", K_CONT, 0 }, { "CONTOUR", K_CONT, 0 },
    { "DETUNE", K_CONT, 0 }, { "DRIVE", K_CONT, 0 }, { "DRIFT", K_CONT, 0 },
    { "RANGE", K_STEP, N_RANGES }, { "WAVE", K_STEP, N_WAVES }, { "FREQ", K_CENTS, 0 }, { "VOLUME", K_CONT, 0 },
    { "RANGE", K_STEP, N_RANGES }, { "WAVE", K_STEP, N_WAVES }, { "FREQ", K_CENTS, 0 }, { "VOLUME", K_CONT, 0 },
    { "RANGE", K_STEP, N_RANGES }, { "WAVE", K_STEP, N_WAVES }, { "FREQ", K_CENTS, 0 }, { "VOLUME", K_CONT, 0 },
    { "NOISE", K_CONT, 0 }, { "FEEDBACK", K_CONT, 0 },
    { "AMOUNT", K_CONT, 0 }, { "ATTACK", K_CONT, 0 }, { "DECAY", K_CONT, 0 }, { "SUSTAIN", K_CONT, 0 }, { "RELEASE", K_CONT, 0 },
    { "SATURATE", K_CONT, 0 },
    { "MOD MIX", K_CONT, 0 }, { "MOD OSC", K_CONT, 0 }, { "MOD FILT", K_CONT, 0 },
};
#endif
#define FREQ_MAX 800                                     /* cents: +-8 semitones */
extern int16_t P[P_N];

static inline int32_t pclamp(int id, int32_t v)
{
    switch (PD[id].kind) {
    case K_STEP:  return v < 0 ? 0 : (v >= PD[id].n ? PD[id].n - 1 : v);
    case K_CENTS: return v < -FREQ_MAX ? -FREQ_MAX : (v > FREQ_MAX ? FREQ_MAX : v);
    default:      return clampk(v);
    }
}
/* a knob offset (LFO, 0..32767 scale) in the parameter's own units */
static inline int32_t poffset(int id, int32_t off)
{
    switch (PD[id].kind) {
    case K_STEP:  return (off * PD[id].n) >> 15;
    case K_CENTS: return (off * 2 * FREQ_MAX) >> 15;
    default:      return off;
    }
}

/* the views (mm_ui.h switches them) and what their four knobs edit */
enum { V_HOME, V_ADV, V_OSC1, V_OSC2, V_OSC3, V_FILTER, V_CONTOUR, V_ANALOG, V_NOISE, N_VIEWS };   /* V_NOISE: the
   oscillator view's 4th row (the Minimoog's mixer: three oscillators and the noise) */
extern volatile uint8_t mm_view;                         /* V_HOME until a click */
extern volatile int8_t mm_map[4];            /* the visible view's knobs */
/* The OP-1 LFO moves a knob word by at most part of its range: its offset counts LFO_GAIN times here, so that the
 * LFO's full amount sweeps the whole range (the sound number on blue excepted: one sound per 1024 already) */
#define LFO_GAIN 2
extern volatile int16_t mm_lfo[4];                       /* the OP-1 LFO's offset per knob (x LFO_GAIN), ... */
extern volatile uint8_t mm_lfo_seq;                      /* ... and a count of the blocks (the screen: is it fresh?) */

/* home + Shift (mm_ui.h): the knob words 0..3 hold the core's Shift clicks until the next frame puts them back, and a
   preset's word sits at the top of its slot, so one click crosses into the next sound. While the screen has Shift
   held on home (mm_home_hold, frames counted in mm_ui_beat) the audio thread takes the home words from P[0..3] (what
   the screen accepted), never from the words: else it derived the other sound for a few blocks, and its contour reset
   AMT (a user saw DET do nothing and AMT move, 2026-11-08). */
extern volatile uint8_t mm_home_hold;
extern volatile uint8_t mm_ui_beat;
extern int16_t mm_derived_setup;                        /* what P's oscillator / contour values come from */
extern int32_t mm_derived_contour;
void sync_derived(int32_t setup_word, int32_t contour_word);        /* audio and GUI thread: same result */

/* detune (word 5, 0 = never set: 5 cents) -> ratio - 1 in Q16 for osc 2 (osc 3 gets half of it, flat) */
static inline int32_t detune_q16(int32_t w) { return w > 0 ? (1514 * ((w * w) >> 15)) >> 15 : 190; }
#define DRIFT_DEFAULT 8192                                                    /* v2's drift, ~2 cents */
static inline int32_t drift_amount(int32_t w) { return w > 0 ? w : DRIFT_DEFAULT; }
/* Above DRIVE_TR the ladder runs as a transistor ladder: each stage is clip(in) - clip(state), as in the per-stage
 * tanh models (Huovilainen, D'Angelo & Valimaki: Csound, VCV), instead of only the input clipped. Driven, a real
 * ladder gets brighter and buzzier, not just compressed (docs/PERFORMANCE.md, v3.5); clean or lightly driven, the
 * two agree within a few dB, so the extra clips (4 per sample, +43 cycles) are only paid in DRIVE's top half (v3.6). */
#ifndef GG_TR
#define GG_TR 2183                                                      /* 2 kHz at 88.2 kHz: above, the delayed t overshoots */
#endif
#ifndef DRIVE_TR
#define DRIVE_TR 16384                                                  /* 50 % */
#endif
static inline int32_t drive_q12(int32_t w) { return 4096 + ((clampk(w) * 3) >> 3); } /* 1 .. 4x */
static inline int32_t level_comp(const uint16_t *T, int32_t v)  /* T: 9 levels over the knob, Q15 */
{
    int32_t i = clampk(v) >> 12, f = clampk(v) & 4095;
    return T[i] - (((T[i] - T[i + (i < 8)]) * f) >> 12);
}
/* the output level for the drive: 1 / sqrt(drive), the knob changes the tone more than the volume */
static const uint16_t DRIVE_COMP[9] = { 32767, 26487, 22254, 19214, 16997, 15335, 14066, 13077, 12295 };   /* v3.7: for the 4 dB cooler input */
static inline int32_t time_index(int32_t t) { return (clampk(t) * 115) >> 15; }      /* -> MM_DEC: 4 ms .. 4 s */

/* feedback gain, Q12: 4.0 x 103 % at the top; 0 below 0.8 % (what the screen shows as 0 %: the ladder without
   resonance is a shorter loop) */
static inline int32_t emph_k(int32_t emp) { return emp < 256 ? 0 : (emp + (emp >> 5)) >> 1; }
static inline int32_t make_up(int32_t k) { return 4096 + ((k * 1400) >> 12); }  /* passband make-up for the resonance, Q12 */

/* contour times: decay / release steps per chunk (Q16), attack step per chunk (Q22), sustain (Q22) */
typedef struct { int32_t att, dcoef, rcoef, sus, depth; } envc_t;
void contour_coefs(envc_t *e, const int16_t *q);

extern uint8_t mm_last;                 /* the voice that started last (the screen follows it) */
#define SCOPE_N 1024
extern int16_t mm_scope[SCOPE_N];       /* the latest note's output, a ring (the oscillator view's scope) */
extern uint16_t mm_scope_w;             /* next write position */
extern uint8_t mm_scope_sh;             /* it keeps every (1 << mm_scope_sh)th sample (the scope sets it: long periods) */

/* CPU guard: with many voices sounding (poly chords, unison) osc 3 is switched off for all voices: off from
 * OSC3_OFF_AT voices, on again at OSC3_ON_AT or fewer (hysteresis), with a 23 ms fade. A voice sounds if it rendered
 * within the last OP1_VOICES renders (every sounding voice renders once per audio callback) or was just started. */
#define OSC3_OFF_AT 4
#define OSC3_ON_AT  2
extern uint32_t mm_serial;              /* render calls, all voices */
extern uint8_t mm_osc3_off;             /* osc 3 switched off for CPU (the screen shows it) */
extern uint8_t mm_osc3_why;             /* why: 1 poly (4+ notes), 2 unison (the voices share a pitch) */

#ifndef OP1_HOST
#define MM_SLOT (*(volatile int *)0xFF8016D4 & 7)        /* the synth slot that plays (and that the screen shows) */
#else
#define MM_SLOT (host_slot & 7)
#endif
static inline int16_t *live_words(void) { return (int16_t *)op1_synth_knobs(MM_SLOT); }

/* modulation depths (moai.c, "modulation") and the saturation's gain (moai.c, "saturation") */
#define MOD_PITCH_MAX 1200              /* cents */
#define MOD_FILT_MAX  3072              /* Q10 octaves */
static inline int32_t mod_depth(int32_t v, int32_t max) { int32_t t = (clampk(v) * clampk(v)) >> 15; return (t * max) >> 15; }
static inline int32_t sat_gain(int32_t v) { int32_t t = (clampk(v) * clampk(v)) >> 15; return 4096 + ((t * 7) >> 3); }

#endif
