/* EVA: a space choir. A warm, sung 1980s film-score pad: two slightly detuned oscillators (a saw and a soft pulse),
 * a lowpass that opens with the breath, a gentle vowel colour on top of it, slow vibrato with a little drift per voice
 * and a wide ensemble chorus; the astronaut's helmet radio is the character on top (kennel synth plugin, eva.knl).
 * blue   (knob 0) PRESSURE: brightness and attack: soft, slow and distant (low) .. bright, quick and close (high)
 * green  (knob 1) FREQ: the vowel colour, oo - ah - ee: two wide formant bands blended with the lowpass
 * white  (knob 2) OXYGEN: air and width: breath noise in the attack, the ensemble chorus's depth, a slow swell
 * orange (knob 3) COM: the radio as a colour. Up to 40 %: a warm band-limit and soft saturation. Above 40 %: the
 *                  channel gets thinner (300 Hz .. 3 kHz), hiss rises, the carrier drops out now and then, and the
 *                  Quindar tones come in, louder with COM: 2525 Hz for 250 ms when a note starts with no other note
 *                  held, 2475 Hz for 250 ms and a squelch tail when the last note is released (in the voice's
 *                  release, so a short release cuts it). Above 75 % the signal is crushed (sample rate and bits).
 *                  Orange page 2 (press COM; word 7): the beep's octave, three steps of the word (OP1_PRESS_PAGE_STEPS(3): one detent per step): down (below 10923),
 *                  the original (10923 .. 21845, also an unset word) and up (21846 and over); the tones stay fixed
 *                  (1262 / 1238 Hz, 2525 / 2475 Hz, 5050 / 4950 Hz), they do not follow the key.
 * The engine never sees the voice sum, so the "shared" radio is shared state (the swell, dropouts, the beep), one owner
 * voice for each beep, advanced once per block by the first voice rendered (voices render in ascending order).
 * Everything runs at 44.1 kHz, 64 samples a call (half R246's 88.2 kHz voice rate, which halves the cost; the
 * oscillators are polyBLEP, the pad's lowpass ends below 6 kHz), the output is interpolated 2x to the 128.
 * Rates (2026-11-03): EVA was designed as 22.05 kHz internal and 344.5 calls/s; R246 calls a voice 689 times a second
 * with 128 samples at 88.2 kHz, so every rate-bound constant ran 2x on the unit (vibrato 10 Hz, formants and lowpasses
 * an octave up, the Quindar tones an octave up for half as long). They are re-derived at 44.1 kHz and 689 calls/s;
 * the pitch (two PITCH_INC steps per internal sample) was right. The ensemble's delay line runs at half rate (22.05
 * kHz, pairs averaged in, the wet interpolated back), so its 384 samples hold the designed 10 / 14 ms taps. Every
 * knob-driven value moves from the previous block's
 * value to the new one in steps (the first at the old value): 8 for the level and the carrier, 4 for the filters
 * and the noises, so a knob step never jumps at the first sample; the
 * crush's hold and step change at block boundaries behind the stepped mix. Phases and LFOs are uint32 that wrap by
 * design (2^32 is one cycle), nothing else counts. Follows note velocity.
 * Cost: the block is built in passes over a buffer (source, pad, ensemble, radio), each with only its own state; the
 * pitch reciprocal updates only when pitch changes; vowel ratios use exact reciprocal seeds; the polyBLEP runs only near an edge; the delay line is a circle
 * (the taps read through circular DAG registers). On the Blackfin the passes are eva_dsp.S's hand-scheduled kernels, bit for
 * bit the C loops here (which stay the host's path and the 11 kHz-and-up source); no library division in the audio path.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include "op1_plugin.h"
#include "eva_tables.h"
#include "eva_geom.h"

#define FS 32768
/* The code a voice call runs (render, its helpers, eva_dsp.S's per-block kernels) in one section, .text.ev_hot, after
 * everything else in eva.c and before eva_dsp.S's rare kernels (.text.ev_rare): one run of lines with no cold code
 * between (the packer's merge.ld keeps .text.* in each object's order). */
#ifdef __bfin__
#define EV_HOT __attribute__((section(".text.ev_hot")))
#else
#define EV_HOT
#endif
#define HALF (OP1_BLOCK / 2)           /* the voice runs at 44.1 kHz, HALF samples per block */
#define QLEN 11025                        /* Quindar beep, 250 ms (44.1 kHz samples) */
#define QFADE 64                          /* 1.5 ms fade in and out (a power of two: no division per sample) */
#define QFADE_SH 6                        /* log2(QFADE) */
#define SQLEN 4096                        /* squelch tail after the end beep, 93 ms (a power of two) */
#define SQLEN_SH 12                       /* log2(SQLEN) */
#define SUB 8                             /* 44.1 kHz samples per parameter step (16 voice samples) */
#define DLY 384                           /* chorus delay line per voice, 17 ms at half rate (22.05 kHz; a circle: the
                                             taps reach 375 back) */

enum { P_LPA, P_F1, P_F2, P_D1, P_D2, P_G1, P_G2, P_NOISE, P_AMP, P_CHO, P_HP, P_RLP, P_SAT, P_HISS, P_CRUSH,
       P_GATE, NP };

typedef struct {
    uint32_t ph1, ph2, vph, noise, c1ph, c2ph;
    int32_t vib, drift, dtarget, env;     /* vibrato depth, pitch drift and its target (Q15), attack envelope (Q15) */
    int32_t a1, a2;                       /* the pad's lowpass, two one-poles */
    int32_t lo1, bd1, lo2, bd2;           /* the vowel's formant bands */
    uint32_t dw;                          /* the chorus line's write index, 0 .. DLY - 1 (a multiple of HALF / 2) */
    int32_t wl;                           /* the chorus's last wet sample (22.05 kHz), for the interpolation */
    int32_t hs1, hs2, l1, l2;             /* radio band-limit */
    int32_t last;                         /* last 44.1 kHz output sample, for the interpolation */
    int32_t held_s, cnt;                  /* crush: held sample, counter (its hold and step: crush_hold / crush_step) */
    int32_t p[NP];                        /* previous block's targets */
    int32_t kf, vc[6];                    /* the vowel's coefficients (F1, F2, dampings, gains) for FREQ = kf */
    uint32_t kinc;                        /* the pitch inv is for */
    int32_t inv;                          /* (2^30 / dt) >> 8 at that pitch */
} voice_t;

/* The engine's state that only the audio path touches (render, note_on): one block, the L1 pool's state lease
 * (op1_l1_claim_state, ev_claim below) when the engine holds the synth partition, else ev_home in SDRAM. note_off
 * (VoiceCtl's thread) and the screen never touch it: what note_off needs (COM, the beep octave) is mirrored in ev_co,
 * what it writes (held, the beep) lives outside. */
typedef struct {
    int16_t dl[OP1_VOICES][DLY];          /* the chorus delay lines (first: 32-byte aligned, whole words) */
    voice_t v[OP1_VOICES];
    int32_t last_voice;
    uint32_t rng;
    uint32_t br_ph, br_inc;               /* the slow swell, 2^32 per cycle */
    int32_t drop_left;                    /* blocks left in a dropout */
    int32_t gate_s;                       /* the carrier, smoothed per block (~12 ms) */
    int32_t kc, kdrop, com_t[5];          /* COM's radio values (com_coefs) for COM = kc and the dropout kdrop */
} hot_t;
/* kf = -1: no vowel coefficients yet (vc is a function of kf alone, as inv is of kinc: note_on keeps both) */
_Static_assert(OP1_VOICES == 6, "ev_home's initialiser names six voices");
static hot_t ev_home __attribute__((aligned(32))) = {
    .v = { [0].kf = -1, [1].kf = -1, [2].kf = -1, [3].kf = -1, [4].kf = -1, [5].kf = -1 },
    .last_voice = OP1_VOICES, .rng = 0x2545F491u, .br_inc = 747500u, .gate_s = FS - 1, .kc = -1 };
static hot_t *ev_h = &ev_home;            /* this call's state: set by ev_claim at the start of render and note_on */
#define voices (ev_h->v)
#define last_voice (ev_h->last_voice)
#define br_ph (ev_h->br_ph)
#define br_inc (ev_h->br_inc)
#define drop_left (ev_h->drop_left)
#define gate_s (ev_h->gate_s)
#define kc (ev_h->kc)
#define kdrop (ev_h->kdrop)
#define com_t (ev_h->com_t)
#define ev_dl (ev_h->dl)
/* Quindar: one owner voice; with the held flags (note_on / note_off): one small block */
static struct {
    int owner;
    uint32_t ph, inc;
    int32_t left, level, sleft, slevel;
    uint8_t held[OP1_VOICES];
} ev_q = { .owner = -1 };
#define q_owner (ev_q.owner)
#define q_ph (ev_q.ph)
#define q_inc (ev_q.inc)
#define q_left (ev_q.left)
#define q_level (ev_q.level)
#define sq_left (ev_q.sleft)
#define sq_level (ev_q.slevel)
#define held (ev_q.held)
/* for note_off and the screen (written by render and note_on): COM | the beep octave << 16 each voice last saw (note_off
 * gets no knobs); rendered blocks, breath swell, carrier gate, beep */
static volatile struct {
    uint32_t co[OP1_VOICES];
    uint32_t blocks;
    int32_t breath, gate, beep;
} ev_m = { .gate = FS - 1 };
#define ev_co (ev_m.co)
#define eva_blocks (ev_m.blocks)
#define eva_breath (ev_m.breath)
#define eva_gate (ev_m.gate)
#define eva_beep (ev_m.beep)

#define QUINDAR_FROM 13107                /* COM 40 %: below it the radio colours the voice but stays quiet */
#define CRUSH_FROM 24576                  /* COM 75 % */
/* COM's ranges as multiply-and-shift (a division by a constant is a library call on the Blackfin) */
static inline int32_t com_warm(int32_t c) { return c < QUINDAR_FROM ? (c * 5) >> 1 : 32767; }                  /* 0..1 over 0 .. 40 % */
static inline int32_t com_radio(int32_t c) { return c > QUINDAR_FROM ? ((c - QUINDAR_FROM) * 27307) >> 14 : 0; } /* 0..1 above */
static inline int32_t beep_level(int32_t c) { return c > QUINDAR_FROM ? 1500 + (((c - QUINDAR_FROM) * 15834) >> 15) : 0; }
/* the squelch's gain, b * 2 / 3, as multiply-and-shift (b = beep_level(c) <= 11000 for every knob value: exact there,
 * eva_coefficient_test.c checks each one) */
static inline int32_t squelch_gain(int32_t b) { return ((b * 2) * 43691) >> 17; }


static inline int32_t clamp(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }
/* the crush's hold length and quantiser step for a block: from the previous block's P_CRUSH target (p0[P_CRUSH]) */
static inline int32_t crush_hold(const int32_t *p0) { return 2 + 2 * ((p0[P_CRUSH] * 3) >> 15); }   /* 22.05 .. 7.35 kHz */
static inline int32_t crush_step(const int32_t *p0) { return 1 << (3 + ((p0[P_CRUSH] * 6) >> 15)); }
/* Orange page 2: the beep's octave from word 7 in three steps (OP1_PRESS_PAGE_STEPS(3): one detent per step, the
 * word at each step's centre): 0 down, 1 the original, 2 up. An unset word (0, older sounds) is the original. */
static unsigned beep_octave(const int16_t *k) { unsigned s = k[7] <= 0 ? 1u : ((unsigned)k[7] * 3u) >> 15; return s > 2u ? 2u : s; }
/* the Quindar increments at 44.1 kHz (2525 Hz start, 2475 Hz end) an octave down (1262 / 1238 Hz), as is, or up
 * (5050 / 4950 Hz) */
static uint32_t beep_increment(uint32_t inc, unsigned oct) { return oct == 0 ? inc >> 1 : oct == 1 ? inc : inc << 1; }
static inline int32_t knob(const int16_t *k, int i) { return k[i] < 0 ? 0 : k[i]; }
/* ev_sin's first quarter (257 entries): ev_sin itself, or its copy in the L1 pool (ev_claim). The table is a sine
 * whose quarters mirror exactly (ev_sin[512 - i] = ev_sin[i], ev_sin[512 + i] = -ev_sin[i], gen_tables.py checks it),
 * so the fold reads the same value as ev_sin[n] for n = 0 .. 1024 */
static const int16_t *ev_sq_at = ev_sin;
static inline int32_t sin_at(const int16_t *q, uint32_t n)
{
    const uint32_t j = n & 255, h = n >> 8;
    const int32_t a = q[h & 1 ? 256 - j : j];
    return h & 2 ? -a : a;
}
static EV_HOT __attribute__((noinline)) int32_t sinq(uint32_t ph)  /* Q15 */
{
    const int16_t *const q = ev_sq_at;
    const uint32_t i = ph >> 22;
    const int32_t a = sin_at(q, i);
    return a + (((sin_at(q, i + 1) - a) * (int32_t)((ph >> 7) & 0x7FFF)) >> 15);
}
static inline uint32_t lcg(uint32_t *s) { return *s = *s * 1664525u + 1013904223u; }

/* soft clip: identity near 0, reaches +-32768 with zero slope at +-1.5 full scale */
static inline int32_t sclip(int32_t x)
{
    x = clamp(x, -49151, 49151);
    const int32_t h = x >> 1;
    const int32_t x2 = (h * h) >> 13;
    const int32_t x3 = ((x2 >> 1) * h) >> 13;
    return x - ((x3 * 4855) >> 15);
}

/* polyBLEP saw, Q15 (+-32768): t = the phase as Q15, dt = the phase step as Q15, inv8 = (2^30 / dt) >> 8 */
static inline int32_t blep_saw(uint32_t ph, int32_t dt, int32_t inv8)
{
    const int32_t t = (int32_t)(ph >> 17);
    int32_t s = (int32_t)(ph >> 16) - 32768;
    if (t < dt) {
        const int32_t x = (t * inv8) >> 7;
        s -= x + x - ((x * x) >> 15) - 32768;
    } else if (t > 32768 - dt) {
        const int32_t x = ((t - 32768) * inv8) >> 7;
        s -= ((x * x) >> 15) + x + x + 32768;
    }
    return s;
}

/* a one-pole coefficient k (Q15) made for 22.05 kHz -> the same pole at 44.1 kHz, 1 - sqrt(1 - k) (ev_lp2x, within
 * 0.7 % below k = 0.97, ~12 kHz; k = 1, open, stays open). Per block, not per sample */
static inline int32_t lp2x(int32_t k)
{
    if (k >= 32767) return 32767;
    const int32_t i = k >> 9, a = ev_lp2x[i];
    return a + (((ev_lp2x[i + 1] - a) * (k & 511)) >> 9);
}
/* a one-pole coefficient (Q15) from a brightness 0..32767: about 300 Hz .. 5.6 kHz (the curve made at 22.05 kHz, then
 * taken to 44.1 kHz) */
static inline int32_t lp_coef(int32_t b) { return lp2x(2600 + ((((((b * b) >> 15) + b) >> 1) * 23600) >> 15)); }

/* With numerator n<2^24, floor(2^24/hz) underestimates the quotient
 * by at most one. Split the product into bounded 32-bit pieces, then correct
 * the quotient once. Every FREQ value retains the original exact division. */
_Static_assert(EV_VOWEL_HZ_MIN>=257 && EV_VOWEL_HZ_MAX<=7000 && EV_VOWEL_BW_MAX<1024,
               "EVA coefficient arithmetic requires its proven table bounds");
static int32_t vowel_ratio(int32_t bw,int32_t hz) {
 uint32_t n=(uint32_t)bw<<14,r=ev_recip_hz[hz-EV_VOWEL_HZ_MIN];
 uint32_t q=(((n>>8)*r+(((n&255)*r)>>8))>>16);
 return (int32_t)(q+(n-q*(uint32_t)hz>=(uint32_t)hz));
}
/* Cost (the unit's worst block is the one whose code and data are cold, about a line fill per distinct 32-byte line a
 * voice call touches): the paths that run rarely (a FREQ or COM change, a pitch change, the once-per-block shared
 * state, the source above 11 kHz, a beeping voice) are out of line (EV_RARE) and the per-call glue is kept small. */
#define EV_RARE __attribute__((noinline))
/* FREQ: oo .. ah .. ee; exact coefficients refreshed when FREQ moves. */
static EV_RARE void vowel_coefs(voice_t *v, int32_t f)
{
    {
        v->kf = f;
        const int32_t pos = f * 2;
        int vi = pos >> 15;
        int32_t fr = pos & 0x7FFF;
        if (vi >= 2) { vi = 1; fr = FS - 1; }
        for (int k = 0; k < 2; k++) {
            const int a = vi * 2 + k, b = a + 2;
            const int32_t hz = ev_hz[a] + (((ev_hz[b] - ev_hz[a]) * fr) >> 15);
            const int32_t bw = ev_bw[a] + (((ev_bw[b] - ev_bw[a]) * fr) >> 15);
            const int32_t x = (hz * 4781) >> 11;                        /* pi hz / 44100, Q15 */
            const int32_t x3 = (((x * x) >> 15) * x) >> 15;
            /* x3 is 0..81 for the vowel table: exact floor(x3/6). */
            v->vc[k] = x - ((x3 * 10923) >> 16);                                      /* 2 sin(pi hz / 44100), Q14 */
            v->vc[2 + k] = vowel_ratio(bw,hz);                             /* damping 1/Q, Q14 */
            v->vc[4 + k] = ((ev_level[a] + (((ev_level[b] - ev_level[a]) * fr) >> 15)) * v->vc[2 + k]) >> 14;  /* unity peak */
        }
    }
}
/* COM: warm up to 40 %, then the radio (the same for every voice: worked out when COM or the dropout changes) */
static EV_RARE void com_coefs(int32_t c)
{
    kc = c;
    kdrop = drop_left != 0;
    const int32_t w = com_warm(c), r = com_radio(c);
    com_t[0] = ((30 + ((w * 130) >> 15) + ((r * 170) >> 15)) * 4781) >> 11;               /* 30 .. 160 .. 330 Hz (Q14 2 pi f / 44100) */
    com_t[1] = lp2x(32767 - ((w * 7800) >> 15) - ((r * 11000) >> 15));                   /* open .. ~5 kHz .. ~2.6 kHz */
    com_t[2] = 4096 + ((w * 3000) >> 15) + ((r * 3500) >> 15);                           /* Q12: 1 .. 1.7 .. 2.6 */
    com_t[3] = r ? 200 + ((r * 1600) >> 15) + (drop_left ? (r * 2500) >> 15 : 0) : 0;
    com_t[4] = c > CRUSH_FROM ? (c - CRUSH_FROM) * 4 : 0;                                 /* 0..1 above 75 % */
}
static EV_HOT void targets(voice_t *v, int vi, const int16_t *knobs, int32_t *t)
{
    const int32_t p = knob(knobs, 0), f = knob(knobs, 1), o = knob(knobs, 2), c = knob(knobs, 3);
    /* PRESSURE and the attack: the lowpass opens with both */
    const int32_t bright = 8192 + ((p * 24575) >> 15);                 /* 0.25 .. 1 */
    const int32_t bloom = 13107 + ((v->env * 19660) >> 15);             /* 0.4 .. 1 while the note swells in */
    t[P_LPA] = lp_coef((bright * bloom) >> 15);
    if (f != v->kf) vowel_coefs(v, f);
    for (int k = 0; k < 2; k++) {
        t[P_F1 + k] = v->vc[k];
        t[P_D1 + k] = v->vc[2 + k];
        t[P_G1 + k] = v->vc[4 + k];
    }
    /* OXYGEN: breath in the attack, the chorus, the swell */
    t[P_NOISE] = (o * (1600 + (((32767 - v->env) * 9000) >> 15))) >> 15;
    t[P_CHO] = 13107 + ((o * 19660) >> 15);                             /* 0.4 .. 1 */
    const int32_t sw = 16384 + (sinq(br_ph) >> 1);                      /* 0..1 */
    t[P_AMP] = (v->env * (FS - 1 - ((((o * sw) >> 15) * 8192) >> 15))) >> 15;   /* the swell dips 25 % at most */
    if (c != kc || (drop_left != 0) != kdrop) com_coefs(c);
    t[P_HP] = com_t[0];
    t[P_RLP] = com_t[1];
    t[P_SAT] = com_t[2];
    t[P_HISS] = com_t[3];
    t[P_CRUSH] = com_t[4];
    t[P_GATE] = gate_s;
    ev_co[vi] = (uint32_t)c | beep_octave(knobs) << 16;
}

#ifdef __bfin__
void eva_clear(int16_t *dl);
int32_t eva_inv(int32_t d);                  /* floor(2^22 / d), 1 <= d < 2^15 (dt0 = inc0 >> 17 always is) */
/* L1 pool (op1_plugin.h, kennel/kn_l1.h): while this engine holds the synth partition, hot_t (the state lease, at
 * offset 0) and after it ev_scr_t: the sine table (copied in when fresh), the source buffer x and the pad / voice
 * buffer pb (per-block scratch, each fully written by eva_saw / eva_pul / eva_pad before it is read; a spare word on
 * either side, never used) and the kernels' parameter block k (written before each kernel call). Without the pool:
 * ev_home, ev_sin and ev_scr_home. Same samples either way. */
#define HALF_ (OP1_BLOCK / 2)
typedef struct {
    int16_t sq[272];                         /* ev_sin's first quarter, 257 entries */
    int32_t x[2 + HALF_], xpad[6];
    int32_t pb[2 + HALF_], pbpad[6];
    int32_t k[24];
} ev_scr_t;
#define EV_L1_BYTES (OP1_L1_ALIGN(sizeof(hot_t)) + sizeof(ev_scr_t))
_Static_assert(EV_L1_BYTES <= 8192, "EVA's lease fits OP1_L1_SYNTH");
_Static_assert(sizeof(hot_t) % 4 == 0, "the state is whole words");
static ev_scr_t ev_scr_home;                 /* the scratch without the pool (its sq is not used: ev_sin is) */
static ev_scr_t *ev_s = &ev_scr_home;        /* this call's scratch (ev_claim) */
#define ev_k_at (ev_s->k)
static EV_RARE void ev_fill(ev_scr_t *s) { for (int i = 0; i < 257; i++) s->sq[i] = ev_sin[i]; }
#endif
/* the start of every render and note_on: where this call's state (and scratch) is; used for this call only */
static EV_HOT void ev_claim(void)
{
#ifdef __bfin__
    int fresh;
    uint8_t *const t = op1_l1_claim_state(OP1_L1_SYNTH, &ev_home, sizeof ev_home, EV_L1_BYTES, &fresh);
    hot_t *h = &ev_home;
    ev_scr_t *s = &ev_scr_home;
    const int16_t *q = ev_sin;
    if (t) {
        h = (hot_t *)(void *)t;
        s = (ev_scr_t *)(void *)(t + OP1_L1_ALIGN(sizeof(hot_t)));
        q = s->sq;
        if (fresh) ev_fill(s);
    }
    ev_h = h;
    ev_s = s;
    ev_sq_at = q;
#endif
}

static EV_HOT void eva_note_on(void *self, const int16_t *knobs)
{
    ev_claim();
    const int vi = op1_voice_index(self);
    voice_t *v = &voices[vi];
    int alone = 1;
    for (int i = 0; i < OP1_VOICES; i++) if (i != vi && held[i]) alone = 0;
    held[vi] = 1;
    v->noise = 0x9E3779B9u ^ ((uint32_t)vi * 0x85EBCA6Bu);   /* per voice, not per address: reproducible */
    v->ph1 = lcg(&v->noise);                                   /* free-running phases: the voices don't line up */
    v->ph2 = lcg(&v->noise);
    v->c1ph = (uint32_t)vi * 0x2AAAAAAAu;
    v->c2ph = (uint32_t)vi * 0x15555555u + 0x40000000u;
    v->vph = 0;
    v->vib = 0;
    v->drift = v->dtarget = 0;
    v->env = 0;
    v->a1 = v->a2 = v->lo1 = v->bd1 = v->lo2 = v->bd2 = 0;
    /* The taps read 166..375 samples behind the write: until this note has written a slot, a read of it must give 0
     * (as the doubled 512-sample line gave before): the whole circle is cleared when dw restarts at zero. */
#ifdef __bfin__
    eva_clear(ev_dl[vi]);                                         /* eva_dsp.S: the same, a word at a time */
    _Static_assert(DLY % 2 == 0 && DLY == 384, "eva_clear clears 192 words");
#else
    for (int i = 0; i < DLY; i++) ev_dl[vi][i] = 0;
#endif
    v->dw = 0;
    v->wl = 0;
    v->hs1 = v->hs2 = v->l1 = v->l2 = v->last = 0;
    v->held_s = v->cnt = 0;
    targets(v, vi, knobs, v->p);
    const int32_t c = (int32_t)(ev_co[vi] & 0xFFFF);
    if (alone && beep_level(c)) {                                 /* key up: start beep */
        q_owner = vi;
        q_ph = 0;
        q_inc = beep_increment(245918542u, ev_co[vi] >> 16);      /* 2525 Hz at 44.1 kHz */
        q_left = QLEN;
        q_level = beep_level(c);
        sq_left = 0;
    }
}

static void eva_note_off(void *self)
{
    const int vi = op1_voice_index(self);
    held[vi] = 0;
    for (int i = 0; i < OP1_VOICES; i++) if (held[i]) return;
    const uint32_t co = ev_co[vi];
    const int32_t c = (int32_t)(co & 0xFFFF);
    if (!beep_level(c)) return;
    q_owner = vi;                                                 /* key down: end beep, then squelch */
    q_ph = 0;
    q_inc = beep_increment(241048979u, co >> 16);           /* 2475 Hz at 44.1 kHz */
    q_left = QLEN;
    q_level = beep_level(c);
    sq_left = SQLEN;
    sq_level = squelch_gain(beep_level(c));
}

/* radio: a 2+2 pole band-limit and the saturation, ramped per sub-block; fin: also the crush (crush: on) and
 * velocity and the 2x interpolation into out (when no beep follows) */
#ifdef __bfin__
/* eva_dsp.S: the radio's passes (see its header); x is ev_pb + 1 (a word on either side) */
void eva_rhp(int32_t *pb, int32_t *st, const int32_t *hp);
void eva_rlp(int32_t *pb, int32_t *st, const int32_t *k);
void eva_rsat(int32_t *pb);
void eva_rout(const int32_t *pb, int16_t *out, int32_t *last, int32_t gain);
void eva_rcrush(const int32_t *pb, int16_t *out, int32_t *st, const int32_t *k);
/* dst[sb * stride] = p + ((dd * sb) >> sh) for sb = 0 .. n - 1 (RAMP / RAMP4 below), without the multiplies; one copy
 * of the code for every caller (dd = 0 gives p throughout) */
static EV_HOT __attribute__((noinline)) void ev_ramp(int32_t *dst, int stride, int n, int sh, int32_t p, int32_t dd)
{
    int32_t a = 0;
    for (int sb = 0; sb < n; sb++) { dst[sb * stride] = p + (a >> sh); a += dd; }
}
/* COM above 75 %: after the band-limit, the saturation, then the crush, the gain and the 2x interpolation into out */
static EV_RARE void radio_crush(voice_t *v, int32_t *x, const int32_t *p0, const int32_t *d, int16_t *out, int32_t gain)
{
    int32_t *const k = ev_k_at, st[3];
    eva_rsat(x);
    k[0] = crush_hold(p0); k[1] = ~(crush_step(p0) - 1); k[2] = gain;
    ev_ramp(k + 3, 1, 4, 2, p0[P_CRUSH], d[P_CRUSH]);
    st[0] = v->last; st[1] = v->cnt; st[2] = v->held_s;
    eva_rcrush(x, out, st, k);
    v->last = st[0]; v->cnt = st[1]; v->held_s = st[2];
}
static EV_HOT __attribute__((noinline)) void radio(voice_t *v, int32_t *x, const int32_t *p0, const int32_t *d, int fin, int crush, int16_t *out,
                         int32_t gain)
{
    int32_t *const k = ev_k_at;
    ev_ramp(k, 1, 4, 2, p0[P_HP], d[P_HP]);
    ev_ramp(k + 5, 2, 4, 2, p0[P_RLP], d[P_RLP]);
    ev_ramp(k + 6, 2, 4, 2, p0[P_SAT], d[P_SAT]);
    eva_rhp(x, &v->hs1, k);                                         /* st = { hs1, hs2 } */
    eva_rlp(x, &v->l1, k + 5);                                      /* st = { l1, l2 } */
    if (!fin) eva_rsat(x);
    else if (!crush) eva_rout(x, out, &v->last, gain);
    else radio_crush(v, x, p0, d, out, gain);
}
#else
static inline void radio(voice_t *v, int32_t *x, const int32_t *p0, const int32_t *d, int fin, int crush, int16_t *out,
                         int32_t gain)
{
    int32_t hs1 = v->hs1, hs2 = v->hs2, l1 = v->l1, l2 = v->l2, last = v->last, cnt = v->cnt, held_s = v->held_s;
    const int32_t hold_n = crush_hold(p0), step = crush_step(p0);
    for (int sb = 0; sb < 4; sb++) {                                  /* 4 steps of 16: these move only with COM */
        const int32_t hp = p0[P_HP] + ((d[P_HP] * sb) >> 2), rlp = p0[P_RLP] + ((d[P_RLP] * sb) >> 2);
        const int32_t sat = p0[P_SAT] + ((d[P_SAT] * sb) >> 2), cr = p0[P_CRUSH] + ((d[P_CRUSH] * sb) >> 2);
        for (int i = sb * 16; i < sb * 16 + 16; i++) {
            int32_t y = clamp(x[i], -65535, 65535);
            hs1 += ((y - hs1) * hp) >> 14;
            y -= hs1;
            hs2 += ((y - hs2) * hp) >> 14;
            l1 += ((y - hs2 - l1) * rlp) >> 15;                          /* within +-2^17: l2 * sat stays in 2^31 */
            l2 += ((l1 - l2) * rlp) >> 15;
            y = sclip((l2 * sat) >> 12);
            if (fin) {                                                  /* sclip and gain: within +-22710, no clamp */
                if (crush) {
                    if (++cnt >= hold_n) {
                        cnt = 0;
                        held_s = y & ~(step - 1);
                    }
                    y += ((held_s - y) * cr) >> 15;
                }
                y = (y * gain) >> 15;
                out[2 * i] = (int16_t)((last + y) >> 1);
                out[2 * i + 1] = (int16_t)y;
                last = y;
            } else x[i] = y;
        }
    }
    v->hs1 = hs1; v->hs2 = hs2; v->l1 = l1; v->l2 = l2;
    if (fin) v->last = last;
    if (crush) { v->cnt = cnt; v->held_s = held_s; }
}
#endif

#ifdef __bfin__
/* eva_dsp.S: the passes below, scheduled by hand (see its header); scratch of one render, off the stack */
void eva_ens(int32_t *pb, const int32_t *k);
void eva_saw(int32_t *y, uint32_t *ph, const int32_t *k);
void eva_pul(int32_t *x, uint32_t *ph, const int32_t *k);
void eva_vowel(int32_t *pb, const int32_t *x, int32_t *k);
void eva_noise(int32_t *buf, uint32_t *noise, const int32_t *k);
void eva_pad(const int32_t *x, int32_t *pb, int32_t *k);
/* one formant band: its state lb = { lo, bd } and the ramps of F, D, G (p0 / d offset to the band) into the kernel's
 * parameters, the band added into pb */
static EV_HOT __attribute__((noinline)) void vowel_band(int32_t *lb, int32_t *pb, const int32_t *x, int32_t *vk,
                                                 const int32_t *p0, const int32_t *d)
{
    vk[0] = lb[0];
    vk[1] = lb[1];
    ev_ramp(vk + 2, 3, 4, 2, p0[P_F1], d[P_F1]);
    ev_ramp(vk + 3, 3, 4, 2, p0[P_D1], d[P_D1]);
    for (int sb = 0; sb < 4; sb++) vk[4 + 3 * sb] = ((p0[P_G1] + ((d[P_G1] * sb) >> 2)) * 26000) >> 15;
    eva_vowel(pb, x, vk);
    lb[0] = vk[0];
    lb[1] = vk[1];
}
#endif

static EV_HOT void eva_render_body(void *self, int16_t *out, const int16_t *knobs);
/* eva_dsp.S zeroes the L registers it uses (linear DAG addressing); the caller's L0..L3 come back on return, as
 * mbira.c does */
static EV_HOT void eva_render(void *self, int16_t *out, const int16_t *knobs)
{
#ifdef __bfin__
    uint32_t l0, l1, l2, l3;
    __asm__ volatile("%0 = L0;\n\t%1 = L1;\n\t%2 = L2;\n\t%3 = L3;" : "=d"(l0), "=d"(l1), "=d"(l2), "=d"(l3));
    eva_render_body(self, out, knobs);
    __asm__ volatile("L0 = %0;\n\tL1 = %1;\n\tL2 = %2;\n\tL3 = %3;" : : "d"(l0), "d"(l1), "d"(l2), "d"(l3));
#else
    eva_render_body(self, out, knobs);
#endif
}

/* shared state, once per block (the first voice rendered) */
static EV_RARE void ev_shared(const int16_t *knobs)
{
    br_ph += br_inc;
    const int32_t o = knob(knobs, 2);
    br_inc = 747500u + (uint32_t)((o * 1500) >> 15) * 512u;      /* the swell: 0.12 .. 0.24 Hz at 689 calls/s */
    const int32_t c = knob(knobs, 3);
    const int32_t r = com_radio(c);
    if (drop_left) drop_left--;
    else if (r && (int32_t)(lcg(&ev_h->rng) >> 16) < ((((r * r) >> 15) * 120) >> 15))      /* (out of 2^16: half the odds per call) */
        drop_left = 12 + (int32_t)((lcg(&ev_h->rng) >> 26) + (lcg(&ev_h->rng) >> 27));         /* 17 .. 154 ms */
    gate_s += ((drop_left ? FS - 1 - ((r * 29000) >> 15) : FS - 1) - gate_s) >> 3;            /* ~12 ms */
}

/* the polyBLEP's 1 / dt: one division when the pitch changes (the vibrato's +-0.4 % doesn't matter at the edge) */
static EV_RARE void ev_pitch(voice_t *v, uint32_t inc0)
{
    v->kinc = inc0;
    const int32_t dt0 = (int32_t)(inc0 >> 17);
#ifdef __bfin__
    v->inv = dt0 > 0 ? eva_inv(dt0) : 0;                          /* eva_dsp.S: the same, without libgcc's division */
#else
    v->inv = dt0 > 0 ? ((1 << 30) / dt0) >> 8 : 0;
#endif
}

/* the source at 11 kHz and up (i2 >= 2^30): the polyBLEP on every sample */
static EV_RARE void ev_source_high(int32_t *x, uint32_t *p1, uint32_t *p2, uint32_t i1, uint32_t i2, int32_t dt1,
                                   int32_t dt2, int32_t inv1, int32_t inv2)
{
    uint32_t ph1 = *p1, ph2 = *p2;
    for (int i = 0; i < HALF; i++) {
        ph1 += i1;
        ph2 += i2;
        const int32_t saw = blep_saw(ph1, dt1, inv1);
        const int32_t pul = (blep_saw(ph2, dt2, inv2) - blep_saw(ph2 + 0x80000000u, dt2, inv2)) >> 1;
        x[i] = (saw * 19000 + pul * 12000) >> 15;
    }
    *p1 = ph1; *p2 = ph2;
}

/* the owner voice of a Quindar beep or squelch: the radio without the output, the crush in C, the tone or the
 * squelch, then velocity and the 2x interpolation into out */
static EV_RARE void ev_beep(voice_t *v, int32_t *pb, const int32_t *p0, const int32_t *d, int crushing, int16_t *out,
                            int32_t gain, uint32_t *noisep)
{
    radio(v, pb, p0, d, 0, 0, out, gain);
    /* crush: sample-and-hold and coarse steps, mixed in above 75 % */
    if (crushing) {
        int32_t cnt = v->cnt, held_s = v->held_s;
        const int32_t hold_n = crush_hold(p0), step = crush_step(p0);
        for (int sb = 0; sb < 8; sb++) {
            const int32_t crush = p0[P_CRUSH] + ((d[P_CRUSH] * sb) >> 3);
            for (int i = sb * SUB; i < sb * SUB + SUB; i++) {
                if (++cnt >= hold_n) {
                    cnt = 0;
                    held_s = pb[i] & ~(step - 1);
                }
                pb[i] += ((held_s - pb[i]) * crush) >> 15;
            }
        }
        v->cnt = cnt; v->held_s = held_s;
    }
    /* the Quindar tones and the squelch */
    uint32_t noise = *noisep;
    for (int i = 0; i < HALF; i++) {
        if (q_left > 0) {
            const int32_t env = q_left < QFADE ? q_left : QLEN - q_left < QFADE ? QLEN - q_left : QFADE;
            pb[i] += (((sinq(q_ph) * q_level) >> 15) * env) >> QFADE_SH;
            q_ph += q_inc;
            q_left--;
        } else if (sq_left > 0) {
            pb[i] += ((((int32_t)lcg(&noise) >> 16) * sq_level >> 15) * sq_left) >> SQLEN_SH;
            sq_left--;
        }
    }
    *noisep = noise;
    /* out: velocity, interpolated 2x */
    int32_t last = v->last;
    for (int i = 0; i < HALF; i++) {
        const int32_t y = (pb[i] * gain) >> 15;
        out[2 * i] = (int16_t)clamp((last + y) >> 1, -32768, 32767);
        out[2 * i + 1] = (int16_t)clamp(y, -32768, 32767);
        last = y;
    }
    v->last = last;
}

static EV_HOT void eva_render_body(void *self, int16_t *out, const int16_t *knobs)
{
    ev_claim();
    const int vi = op1_voice_index(self);
    voice_t *const v = &voices[vi];
#ifdef __bfin__
    ev_scr_t *const es = ev_s;
#endif

    if (vi <= last_voice) ev_shared(knobs);                          /* shared state: once per block */
    last_voice = vi;

    /* the attack: PRESSURE sets how fast the note swells in (~0.6 s soft .. ~15 ms close). ka was made per 2.9 ms
     * call; per 1.45 ms call the same curve is 1 - sqrt(1 - ka) = ka / 2 + ka^2 / 8 (within 0.4 %), rounded up, so
     * the swell reaches full level */
    const int32_t pr = knob(knobs, 0), ka0 = 158 + ((((pr * pr) >> 15) * 6200) >> 15);
    const int32_t ka = (ka0 >> 1) + ((ka0 * ka0) >> 18);
    v->env += ((FS - 1 - v->env) * ka + 32767) >> 15;

    int32_t t[NP], d[NP];
    targets(v, vi, knobs, t);
    const int32_t *p0 = v->p;
    for (int k = 0; k < NP; k++) d[k] = t[k] - p0[k];
#define RAMP(k, sb) (p0[k] + ((d[k] * (sb)) >> 3))   /* sub-block sb (0..7) of 8: the first at the old value */
#define RAMP4(k, sb) (p0[k] + ((d[k] * (sb)) >> 2))  /* the same in 4 sub-blocks of 16 */
    eva_breath = knob(knobs, 2);
    eva_gate = t[P_GATE];
    eva_beep = q_left > 0 && q_owner >= 0;

    /* pitch: slow vibrato (fades in over ~0.5 s) and a little random drift per voice (per 1.45 ms call) */
    if (v->vib < 20480) v->vib += 60;
    if ((lcg(&v->noise) >> 24) < 1) v->dtarget = (int32_t)(lcg(&v->noise) >> 17) - 16384;   /* new target ~ every 0.4 s */
    v->drift += (v->dtarget - v->drift) >> 7;
    const int32_t m = ((sinq(v->vph) * v->vib) >> 15) + ((v->drift * 9830) >> 15);   /* Q15 of +-0.4 % */
    v->vph += 31165250u;                                          /* 5 Hz at 689.0625 calls/s */
    v->c1ph += 2300000u;                                          /* chorus LFOs: 0.37 Hz, 0.53 Hz */
    v->c2ph += 3300000u;
    const int32_t cm1 = sinq(v->c1ph), cm2 = sinq(v->c2ph);
    /* chorus taps (Q8 half-rate samples, 22.05 kHz): 10 ms +- 2.5 ms and 14 ms +- 3 ms, set per block */
    const int32_t dd1 = (221 << 8) + ((cm1 * (55 << 8)) >> 15), dd2 = (309 << 8) + ((cm2 * (66 << 8)) >> 15);

    const uint32_t inc0 = (uint32_t)OP1_PITCH_INC[0] + (uint32_t)OP1_PITCH_INC[1];
    const uint32_t i1 = inc0 + (uint32_t)(((int32_t)(inc0 >> 16) * m) >> 7);
    const uint32_t i2 = i1 + (i1 >> 9) + (i1 >> 11);              /* the pulse ~6 cents up */
    const int32_t dt1 = (int32_t)(i1 >> 17), dt2 = (int32_t)(i2 >> 17);
    /* the pulse's 1 / dt from the saw's (6 cents up: 1 / (1 + 2^-9 + 2^-11) ~ 1 - 2^-9 - 2^-11) */
    if (inc0 != v->kinc) ev_pitch(v, inc0);
    const int32_t inv1 = v->inv, inv2 = inv1 - (inv1 >> 9) - (inv1 >> 11);

    /* the voice is built in passes over the block (44.1 kHz), each with only its own state in registers */
#ifdef __bfin__
    int32_t *const x = es->x + 1;                               /* eva_dsp.S: a spare word on either side */
#else
    int32_t x[HALF];
#endif
    uint32_t noise = v->noise;
    /* the source: a saw and a soft pulse, detuned */
#ifdef __bfin__
    if (i2 < 0x40000000u) {                                       /* eva_dsp.S: the loop below, the saw then the pulse */
        int32_t *const k = es->k;
        k[0] = (int32_t)i1; k[1] = (int32_t)((i1 + i1) ^ 0x80000000u); k[2] = dt1; k[3] = inv1;
        k[4] = (int32_t)i2; k[5] = (int32_t)(i2 + i2); k[6] = (int32_t)(i2 << 2); k[7] = dt2; k[8] = inv2;
        eva_saw(x, &v->ph1, k);
        eva_pul(x, &v->ph2, k + 4);
    }
#else
    if (i2 < 0x40000000u) {                                       /* below 11 kHz: one test per oscillator for an edge */
        uint32_t ph1 = v->ph1, ph2 = v->ph2;
        for (int i = 0; i < HALF; i++) {
            ph1 += i1;
            ph2 += i2;
            /* away from an edge the polyBLEP saw is the plain ramp and the pulse a plain square (+-16384, so its
             * 12000 / 32768 share is +-6000 exactly) */
            const int32_t saw = ph1 + i1 < i1 + i1 ? blep_saw(ph1, dt1, inv1) : (int32_t)(ph1 >> 16) - 32768;
            const uint32_t q = ph2 << 1;
            if (q + i2 + i2 < (i2 << 2)) {                         /* near the up edge (ph2 ~ 0) or the down edge */
                const int32_t pul = ph2 + i2 < i2 + i2 ? (blep_saw(ph2, dt2, inv2) - ((int32_t)((ph2 + 0x80000000u) >> 16) - 32768)) >> 1
                                                      : (((int32_t)(ph2 >> 16) - 32768) - blep_saw(ph2 + 0x80000000u, dt2, inv2)) >> 1;
                x[i] = (saw * 19000 + pul * 12000) >> 15;
            }
            else
                x[i] = ((saw * 19000) >> 15) + (ph2 & 0x80000000u ? 6000 : -6000);
        }
        v->ph1 = ph1; v->ph2 = ph2;
    }
#endif
    else ev_source_high(x, &v->ph1, &v->ph2, i1, i2, dt1, dt2, inv1, inv2);
    /* the pad: breath, two one-pole lowpasses, then the vowel's two bands added (each fed a quarter of the source: no clamps
     * needed), one pass each */
#ifdef __bfin__
    int32_t *const pb = es->pb + 1;                                /* the pad, then the voice from here on */
#else
    int32_t pb[HALF];                                             /* the pad, then the voice from here on */
#endif
#ifdef __bfin__
    {                                                             /* eva_dsp.S: the loops below */
        if (p0[P_NOISE] | t[P_NOISE]) {
            int32_t *const k = es->k;
            k[0] = 8; k[1] = SUB;
            ev_ramp(k + 2, 1, 8, 3, p0[P_NOISE], d[P_NOISE]);
            eva_noise(x, &noise, k);
        }
        int32_t *const k = es->k;
        k[0] = v->a1; k[1] = v->a2;
        ev_ramp(k + 2, 1, 8, 3, p0[P_LPA], d[P_LPA]);
        eva_pad(x, pb, k);
        v->a1 = k[0]; v->a2 = k[1];
#else
    {
        int32_t a1 = v->a1, a2 = v->a2;
        if (p0[P_NOISE] | t[P_NOISE])                               /* with breath, added to the source here */
            for (int sb = 0; sb < 8; sb++) {
                const int32_t lpa = RAMP(P_LPA, sb), nz = RAMP(P_NOISE, sb);
                for (int i = sb * SUB; i < sb * SUB + SUB; i++) {
                    const int32_t in = x[i] + ((((int32_t)lcg(&noise) >> 16) * nz) >> 15);
                    x[i] = in;
                    a1 += ((in - a1) * lpa) >> 15;
                    a2 += ((a1 - a2) * lpa) >> 15;
                    pb[i] = a2;
                }
            }
        else
            for (int sb = 0; sb < 8; sb++) {
                const int32_t lpa = RAMP(P_LPA, sb);
                for (int i = sb * SUB; i < sb * SUB + SUB; i++) {
                    a1 += ((x[i] - a1) * lpa) >> 15;
                    a2 += ((a1 - a2) * lpa) >> 15;
                    pb[i] = a2;
                }
            }
        v->a1 = a1; v->a2 = a2;
#endif
#ifdef __bfin__
        vowel_band(&v->lo1, pb, x, es->k, p0, d);                    /* eva_dsp.S: the loop below, per band */
        vowel_band(&v->lo2, pb, x, es->k, p0 + 1, d + 1);
#else
        for (int k = 0; k < 2; k++) {
            int32_t lo = k ? v->lo2 : v->lo1, bd = k ? v->bd2 : v->bd1;
            for (int sb = 0; sb < 4; sb++) {                          /* 4 steps of 16: these move only with FREQ */
                const int32_t f = RAMP4(P_F1 + k, sb), q = RAMP4(P_D1 + k, sb);
                const int32_t g = (RAMP4(P_G1 + k, sb) * 26000) >> 15;         /* the vowel: peaks on the lowpass */
                for (int i = sb * 16; i < sb * 16 + 16; i++) {
                    lo += (f * bd) >> 14;
                    bd += (f * ((x[i] >> 2) - lo - ((q * bd) >> 14))) >> 14;
                    pb[i] += (bd * g) >> 13;
                }
            }
            if (k) { v->lo2 = lo; v->bd2 = bd; } else { v->lo1 = lo; v->bd1 = bd; }
        }
#endif
    }
    /* the ensemble at half rate (22.05 kHz): each pair of samples goes into a 17 ms delay line as their mean; two
     * modulated taps of it; the wet sample is interpolated back to 44.1 kHz (the pair's first output gets the mean of
     * the last pair's wet and this one's, the second this one's); then the level (attack, swell) and the carrier */
    {
        /* the line is a circle of DLY half-rate samples: each one is written, then the taps read 166 .. 375 samples back
         * (the slots from before this note are zero: note_on clears the line) */
        uint32_t dw = v->dw;
        int16_t *dl = ev_dl[vi];
        const int32_t fa = dd1 & 255, fb = dd2 & 255;
#ifdef __bfin__
        {
            int32_t *k = es->k;
            k[0] = (int32_t)(uintptr_t)dl;
            k[1] = (int32_t)dw;
            k[2] = (dd1 >> 8) + 1;                                    /* ra[-1], rb[-1]: samples behind the write */
            k[3] = (dd2 >> 8) + 1;
            k[4] = (int32_t)((uint32_t)(256 - fb) << 16 | (uint32_t)(256 - fa));
            k[5] = (int32_t)((uint32_t)fb << 16 | (uint32_t)fa);
            for (int sb = 0; sb < 8; sb++) {
                k[6 + 2 * sb] = RAMP(P_CHO, sb) >> 1;
                k[7 + 2 * sb] = (RAMP(P_AMP, sb) * RAMP(P_GATE, sb)) >> 15;
            }
            k[22] = v->wl;                                            /* the last wet sample, in and out */
            eva_ens(pb, k);
            v->wl = k[22];
        }
#else
        uint32_t ia = dw + DLY - (uint32_t)(dd1 >> 8), ib = dw + DLY - (uint32_t)(dd2 >> 8);   /* a, b: slots of ra[0] */
        if (ia >= DLY) ia -= DLY;
        if (ib >= DLY) ib -= DLY;
        int32_t wp = v->wl;
        for (int sb = 0; sb < 8; sb++) {                          /* SUB / 2 = 4 pairs per sub-block */
            const int32_t cho = RAMP(P_CHO, sb) >> 1, ag = (RAMP(P_AMP, sb) * RAMP(P_GATE, sb)) >> 15;
            for (int j = sb * (SUB / 2); j < (sb + 1) * (SUB / 2); j++) {
                const int32_t y0 = pb[2 * j], y1 = pb[2 * j + 1];
                dl[dw + j] = (int16_t)clamp((y0 + y1) >> 1, -32768, 32767);
                const int32_t a = dl[ia], b = dl[ib];
                const int32_t ta = a + (((dl[ia ? ia - 1 : DLY - 1] - a) * fa) >> 8);
                const int32_t tb = b + (((dl[ib ? ib - 1 : DLY - 1] - b) * fb) >> 8);
                if (++ia == DLY) ia = 0;
                if (++ib == DLY) ib = 0;
                const int32_t wet = (ta + tb) >> 1, e0 = (wp + wet) >> 1;
                pb[2 * j] = ((y0 + (((e0 - y0) * cho) >> 15)) * ag) >> 15;
                pb[2 * j + 1] = ((y1 + (((wet - y1) * cho) >> 15)) * ag) >> 15;
                wp = wet;
            }
        }
        v->wl = wp;
#endif
        dw += HALF / 2;
        v->dw = dw < DLY ? dw : 0;
    }
    /* radio: hiss, a 2+2 pole band-limit, saturation */
#ifdef __bfin__
    if (p0[P_HISS] | t[P_HISS]) {                                 /* eva_dsp.S: the loop below */
        int32_t *const k = es->k;
        k[0] = 4; k[1] = 16;
        ev_ramp(k + 2, 1, 4, 2, p0[P_HISS], d[P_HISS]);
        eva_noise(pb, &noise, k);
    }
#else
    if (p0[P_HISS] | t[P_HISS])
        for (int sb = 0; sb < 4; sb++) {                          /* noise: 4 steps are enough */
            const int32_t hiss = RAMP4(P_HISS, sb);
            for (int i = sb * 16; i < sb * 16 + 16; i++) pb[i] += (((int32_t)lcg(&noise) >> 16) * hiss) >> 15;
        }
#endif
    const int crushing = (p0[P_CRUSH] | t[P_CRUSH]) != 0;
    const int32_t gain = (22710 * op1_voice_vel_gain(self)) >> 15;   /* +3.6 dB (level pass 2026-10-29, was 15000) */
    if (vi == q_owner && (q_left | sq_left) > 0) ev_beep(v, pb, p0, d, crushing, out, gain, &noise);   /* both >= 0 */
    else radio(v, pb, p0, d, 1, crushing, out, gain);            /* nothing follows: straight into out */
#undef RAMP
#undef RAMP4
    for (int k = 0; k < NP; k++) v->p[k] = t[k];
    v->noise = noise;
    eva_blocks++;
}

const op1_engine_t ENGINE_eva = { eva_note_on, eva_render, eva_note_off,
                                  { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_PAGE_STEPS(3) } };

/* ======================= screen (GUI thread) =======================
 * eva.svg (gen_svg.py, from the artist's drawing eva-art.svg; numbers in eva_geom.h):
 *   m0..m3  the four meters (blue PRESSURE, green FREQ, white OXYGEN, red COM): 0..16 lit ticks with the knob. Each
 *           mask (black box + dim ticks) slides right one tick per lit tick; a full meter moves it off screen; the
 *           SVG's black cover hides the dim ticks pushed past the last one
 *   cross   the visor crosshair follows FREQ across the visor, from the left eye to the right eye
 *   fog     the visor fogs up from its bottom edge with the breathing: the engine's breath level while notes play,
 *           else a slow breath scaled by OXYGEN
 *   strip   the red radio strip on the visor: off at COM 0, blinks while a Quindar beep plays, drops out with the
 *           carrier; the red meter flickers with COM (and falls to 0..2 ticks during a dropout) */
#define ONE 1024
static void *e_mask[4], *e_cross, *e_fog, *e_strip;
static uint32_t last_blocks, frame, seed = 0x1234567u;
static int32_t act, fog;
static void *e_com_label, *e_oct_label;

static void move(void *e, int32_t tx, int32_t ty)                  /* Q10 */
{
    if (!e) return;
    const int32_t m[9] = { ONE, 0, 0, 0, ONE, 0, tx, ty, ONE };
    op1_node_set_transform(e, m);
}

static void eva_screen_init(void *scr)
{
    static const char *const masks[4] = { "m0", "m1", "m2", "m3" };
    for (int i = 0; i < 4; i++) e_mask[i] = op1_find_node(scr, masks[i]);
    e_cross = op1_find_node(scr, "cross");
    e_fog = op1_find_node(scr, "fog");
    e_strip = op1_find_node(scr, "strip");
    e_com_label=op1_find_node(scr,"com-label");
    e_oct_label = op1_find_node(scr, "oct-label");
}

static inline uint32_t rnd(void) { return (seed = seed * 1664525u + 1013904223u) >> 16; }

static void eva_screen_update(void *scr, int dt)
{
    int16_t kn[4];
    op1_synth_knobs_live(OP1_SCREEN_SLOT & 7, kn);
    int32_t k[4];
    for (int i = 0; i < 4; i++) k[i] = kn[i] < 0 ? 0 : kn[i];
    const uint32_t blocks = eva_blocks;
    const int on = blocks != last_blocks;
    last_blocks = blocks;
    act += ((on ? ONE : 0) - act) >> 3;
    frame++;

    const int dropout = on && eva_gate < 24000;
    /* red flicker: a few frames out of many at low COM, often near the top; any dropout */
    const int flick = dropout || (on && k[3] > 4096 && (int32_t)(rnd() & 0x7FFF) < ((k[3] - 4096) >> 3));
    for (int i = 0; i < 4; i++) {
        int32_t n = (k[i] * 16 + 16383) >> 15;
        if (i == 3 && flick) n = dropout ? (int32_t)(rnd() % 3) : n - 1 - (int32_t)(rnd() % 3);
        if (n < 0) n = 0;
        move(e_mask[i], n >= 16 ? OP1_Q10(200) : n * EVA_TICK_DX, 0);
    }

    /* crosshair: from the left eye (FREQ 0, the artist's spot) to the right eye (100 %) */
    move(e_cross, k[1] * EVA_CROSS_SPAN / 32767, 0);

    /* fog: 0..1 (Q10), scaled up from the visor's bottom edge */
    int32_t target;
    if (act > ONE / 2) {
        target = eva_breath >> 5;
    } else {                                                        /* resting breath, ~0.25 Hz at 60 frames/s */
        const int32_t s = OP1_SINE_TABLE[(frame * 34) & 8191], inh = s > 0 ? s : 0;
        target = ((k[2] >> 5) * (128 + ((inh * 896) >> 15))) >> 10;
    }
    fog += (target - fog) >> 2;
    const int32_t f = fog < 20 ? 20 : fog > ONE ? ONE : fog, sxf = 154 + ((f * 870) >> 10);
    if (e_fog) {
        const int32_t m[9] = { sxf, 0, 0, 0, f, 0, EVA_FOG_X - ((sxf * EVA_FOG_X) >> 10),
                               EVA_FOG_Y - ((f * EVA_FOG_Y) >> 10), ONE };
        op1_node_set_transform(e_fog, m);
    }

    /* radio strip: off at COM 0, blinks with a beep, gone with the carrier */
    const int show = k[3] > 0 && !dropout && !(on && eva_beep && (frame & 4)) && !(flick && (frame & 1));
    move(e_strip, 0, show ? 0 : OP1_Q10(-200));

    /* orange page 2: the label reads OCT and the row's ticks show the beep octave (1 down, 8 the original, 15 up) */
    const unsigned bo = beep_octave(op1_synth_knobs(OP1_SCREEN_SLOT & 7));
    const int page = op1_eng_page[3] != 0;
    if (e_com_label) op1_node_set_visible(e_com_label, !page);
    if (e_oct_label) op1_node_set_visible(e_oct_label, page);
    if (page) move(e_mask[3], (bo == 0 ? 1 : bo == 1 ? 8 : 15) * EVA_TICK_DX, 0);
    op1_screen_draw(scr, dt);
}

const op1_screen_t SCREEN_eva = { eva_screen_init, eva_screen_update };
