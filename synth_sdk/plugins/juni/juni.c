/* JUNI: a six-voice polysynth in the manner of the early-80s one-DCO polys (kennel synth plugin, juni.knl).
 * Per voice: one DCO (saw, pulse with PWM and a square sub an octave down, all from one phase), a 4-pole ladder
 * low-pass with resonance, a high-pass, and the ensemble chorus built into the voice. T2 is the only envelope:
 * the amp after render, and the filter's sweep through OP1_VOICE_ENV (one envelope for both, as those polys had).
 *
 * Every knob is a macro whose whole travel is usable ("everything is a sweet spot"):
 * blue   (knob 0) TONE: the cutoff, with key tracking so the tone stays even across the keyboard
 * green  (knob 1) SHAPE: a path through curated oscillator mixes, each level-matched:
 *                 saw + sub .. saw .. saw + pulse .. PWM strings .. saw + PWM + sub
 * white  (knob 2) SWEEP: the envelope's amount on the cutoff, and a little resonance with it
 * red    (knob 3) CHORUS: off .. I (slow, deep) .. II (faster) .. I+II (fast, shallow)
 * Pressed (page 2, saved with the sound; 0 is the neutral setting):
 * blue   RESONANCE: added to SWEEP's; the top is "just singing" at every cutoff (KSELF)
 * green  HPF: flat .. thin (one pole, up to ~800 Hz)
 * white  PWM RATE: 0.9 Hz .. 7 Hz
 *
 * The chorus: a periodic voice delayed by d samples is the same waveform read d * step behind in phase, so the two
 * chorus taps are two more reads of the DCO at lagging phases instead of a delay line (no memory, no interpolation).
 * They are mixed in before the filter (nearly linear, slowly moving: close enough), with opposite LFO phases (the
 * stereo pair, summed: the notches move against each other instead of one comb sweeping), and each voice's LFO
 * starts at its own phase, so a chord drifts like an ensemble. Rates and depths are approximations of the classic
 * BBD chorus, tuned by ear, not measured from a unit.
 *
 * Synthesis runs 64 internal samples a call at 44.1 kHz (R246's voice call is 128 samples at 88.2 kHz, 689 calls/s;
 * as Oxbow), control steps every 8 internal samples, and a four-point interpolation makes the 128 voice samples.
 * Rates (2026-11-03): the constants were made for a 22.05 kHz internal rate until then, so on the unit the filter,
 * HPF, PWM and chorus LFOs ran at twice their design and key tracking read the note 12 semitones low (which the
 * octave-high cutoff table hid); FS_INC and gen_tables.py now use 44,100 and NOTE_OFF_Q8 88.2 kHz steps, and the
 * chorus delays are counted at 44.1 kHz (chorus_pass). Instrument state (the LFOs, the macros) advances once per
 * block, from the first voice rendered in it (voices render in ascending order, as EVA and Oxbow do).
 *
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include "op1_plugin.h"
#include "juni_tables.h"

#define NS (OP1_BLOCK / 2)                      /* internal samples per block, 44.1 kHz */
#define CTL 8                                   /* internal samples per control step */
#define NC (NS / CTL)                           /* control steps per block */
#define FS_INC(hz) ((uint32_t)((hz) * 4294967296.0 / 44100.0))   /* LFO phase step per internal sample (44.1 kHz) */
/* PWM RATE's LFO step per internal sample: 0.9 Hz + rate^2 * 6.1 Hz (rate Q15; FS_INC(6.1) in steps of 2^12, so the
 * top stays within 0.1 % of 7 Hz) */
#define PWM_INC(rate) (FS_INC(0.9) + (((uint32_t)(((rate) * (rate)) >> 15) * (uint32_t)(FS_INC(6.1) >> 12)) >> 3))

typedef struct {
    uint32_t p;                                 /* DCO phase at half the note's rate (bit 31 = the sub) */
    int32_t prev;                               /* the previous sample, still open to a polyBLEP correction */
    int32_t y1, y2, y3, y4;                     /* the ladder */
    int32_t hp;                                 /* the high-pass's low-pass state */
    int32_t env0;                               /* the stock envelope at the end of the last block, Q15 */
    int32_t up3, up2, up1;                      /* the upsampler's history */
    int32_t note8;                              /* the note (Q8 semitones), for key tracking */
    int32_t velg;                               /* velocity, Q15 */
    uint8_t fresh;
} voice_t;

static voice_t vc[OP1_VOICES];
static int last_voice = OP1_VOICES;

/* each voice card's small cutoff offset (Q8 semitones, about +-0.3), so a chord isn't six copies */
static const int16_t VCUT8[OP1_VOICES] = {-60, 40, 20, -45, 70, -25};

/* SHAPE's path: saw, pulse, sub levels (Q14, level-matched by ear and RMS) and PWM depth (Q15) at five points */
static const int16_t SHAPE_PTS[5][4] = {
    {  9000,     0, 11000,     0 },             /* saw + sub: round, fat */
    { 20000,     0,  3000,     0 },             /* saw */
    { 10000,  9000,     0,  6000 },             /* saw + pulse, a little PWM */
    {  4000, 13000,     0, 26000 },             /* PWM strings */
    {  8500,  9000,  6500, 22000 },             /* saw + PWM + sub: everything */
};

/* ---- instrument state, once per block */
static int32_t as, ap, ab;                      /* saw, pulse, sub levels, Q14 */
static uint32_t pw_blk;                         /* this block's pulse width (phase offset) */
static uint32_t pwm_ph;
static int32_t pwm_depth;
static uint32_t lfo_s, lfo_f;                   /* chorus LFOs (slow, fast) at the block's start */
static uint32_t inc_s;                          /* slow LFO's step */
static int32_t dep_s, dep_f;                    /* chorus depths, Q8 samples */
static int32_t dry_g, wet_g;                    /* Q15 */
static int32_t cut_base, env_amt, reso_q15, ghp, out_g;

static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int32_t knob(const int16_t *k, int i) { return k[i] < 0 ? 0 : k[i]; }

/* (a * b) << 1 for 16-bit a and b: one fractional multiply on Blackfin (a plain int product is a 32 x 32 one) */
#ifdef __bfin__
static inline __attribute__((always_inline)) int32_t fmul(int32_t a, int32_t b)
{ return __builtin_bfin_mult_fr1x32((short)a, (short)b); }
#else
static inline int32_t fmul(int32_t a, int32_t b)
{ int32_t p = (int32_t)(int16_t)a * (int16_t)b; return p == 0x40000000 ? 0x7FFFFFFF : p * 2; }
#endif

/* the ladder's input stage: 1.5 v - 0.5 v^3 for v = x / 49152 in -1..1, flat beyond; slope 1 at 0, out +-32767.
 * Softer than tanh and a third of its work (no table) */
static inline __attribute__((always_inline)) int32_t soft(int32_t x)
{
    const int32_t s = clampi(x, -49152, 49152);
    const int32_t v = fmul(s >> 1, 21845) >> 15;                 /* s * 2 / 3: v * 32768 */
    const int32_t v3 = fmul(fmul(v, v) >> 16, v) >> 16;          /* v^3 * 32768 */
    return s - (v3 >> 1);
}

/* the bit length of x (0 for 0) */
static inline int32_t bitlen(uint32_t x)
{
#ifdef __bfin__
    return x >> 31 ? 32 : 31 - (int32_t)__builtin_bfin_norm_fr1x32((int32_t)x);
#else
    return x ? 32 - __builtin_clz(x) : 0;
#endif
}

/* triangle, Q15 signed */
static inline int32_t tri(uint32_t ph)
{
    int32_t p = (int32_t)((ph + 0x40000000u) >> 15);
    return p < 65536 ? p - 32768 : 98303 - p;
}

/* a curve between two points by u (0..32767) */
static inline int32_t lerp(int32_t a, int32_t b, int32_t u) { return a + (((b - a) * u) >> 15); }

/* SHAPE -> the saw, pulse, sub levels (Q14) and PWM depth: four segments between the five points */
typedef struct { int32_t s, q, b, pwd; } mix_t;
static void shape_mix(int32_t shape, mix_t *m)
{
    int32_t i = shape >> 13, u = (shape & 8191) << 2;
    if (i > 3) { i = 3; u = 32767; }
    m->s = lerp(SHAPE_PTS[i][0], SHAPE_PTS[i + 1][0], u);
    m->q = lerp(SHAPE_PTS[i][1], SHAPE_PTS[i + 1][1], u);
    m->b = lerp(SHAPE_PTS[i][2], SHAPE_PTS[i + 1][2], u);
    m->pwd = lerp(SHAPE_PTS[i][3], SHAPE_PTS[i + 1][3], u);
}

/* CHORUS: 0 .. 0.1 off, then I fades in (wet c over 0.1 .. 0.35), I -> II, II -> I+II */
typedef struct { uint32_t inc_s; int32_t dep_s, dep_f, c; } chorus_t;
static void chorus_map(int32_t ch, chorus_t *o)
{
    int32_t u;
    o->c = ch < 3277 ? 0 : clampi((ch - 3277) * 4, 0, 32767);
    o->dep_s = 80 * 256; o->dep_f = 0;                              /* depths, Q8 internal samples: 1.8 ms */
    if (ch < 16384) {                                               /* I, then towards II */
        u = ch < 8192 ? 0 : (ch - 8192) << 2;
        o->inc_s = (uint32_t)lerp(FS_INC(0.5) >> 4, FS_INC(0.83) >> 4, u) << 4;
    } else if (ch < 24576) {                                        /* II */
        o->inc_s = FS_INC(0.83);
    } else {                                                        /* II -> I+II */
        u = (ch - 24576) << 2;
        o->inc_s = FS_INC(0.83);
        o->dep_s = lerp(80 * 256, 0, u);
        o->dep_f = lerp(0, 10 * 256, u);                            /* 0.23 ms */
    }
}

static volatile uint32_t jn_blocks;             /* for the screen: rendered blocks */
static volatile uint32_t jn_vblk[OP1_VOICES];   /* for the screen: jn_blocks when each voice last sounded */

static void block_shared(const int16_t *k)
{
    int32_t tone = knob(k, 0), shape = knob(k, 1), sweep = knob(k, 2), ch = knob(k, 3);
    int32_t res = knob(k, 5), hpf = knob(k, 6), rate = knob(k, 7), i;
    mix_t m;
    chorus_t co;

    jn_blocks++;
    shape_mix(shape, &m);
    as = m.s; ap = m.q; ab = m.b; pwm_depth = m.pwd;
    /* PWM: one LFO for the instrument; width 50 % .. 90 % (never thin enough to vanish) */
    pwm_ph += PWM_INC(rate) * NS;
    i = (((tri(pwm_ph) + 32768) >> 1) * pwm_depth) >> 15;
    pw_blk = 0x80000000u + ((uint32_t)(i < 26214 ? i : 26214) << 16);   /* up to +40 % */

    /* TONE: MIDI 36 .. 118 at middle C, linear in semitones (the ear's scale); the bottom is dark, never gone */
    cut_base = 36 * 256 + ((tone * (82 * 256)) >> 15);
    /* SWEEP: the envelope's reach, squared for fine control near zero: 0 .. 60 semitones */
    env_amt = (((sweep * sweep) >> 15) * (60 * 256)) >> 15;
    /* resonance: page 2, plus up to 35 % with SWEEP; at most 92 % of self-oscillation */
    reso_q15 = clampi(res + ((sweep * 11469) >> 15), 0, 32767);
    reso_q15 = (reso_q15 * 30147) >> 15;
    /* HPF: flat at 0, then a one-pole up to ~800 Hz (3597: 816 Hz at 44.1 kHz); squared so the low end has room */
    ghp = (((hpf * hpf) >> 15) * 3597) >> 15;
    /* resonance thins the bass of a ladder: give some of it back, so turning RESONANCE doesn't drop the level.
     * The ladder's full swing (+-65536 here) landed within about 0.7 dB of full scale at the old 14000 + 4000; the +4.5 dB
     * of the level pass keeps the presets' voice peaks under -1 dBFS, an extreme knob set can reach op1_sat16 */
    out_g = 23506 + ((reso_q15 * 6716) >> 15);                 /* +4.5 dB (level pass 2026-10-29, was 14000 + 4000) */

    lfo_s += inc_s * NS;
    lfo_f += FS_INC(8.0) * NS;
    chorus_map(ch, &co);
    inc_s = co.inc_s; dep_s = co.dep_s; dep_f = co.dep_f;
    wet_g = (co.c * 11469) >> 15;                                   /* each tap up to 0.35 */
    dry_g = 32767;                              /* the wet path is high-passed, so it doesn't pile up in the bass: no
                                                 * normalisation needed (level within 0.8 dB from off to I+II) */
}

/* the polyBLEP event: a jump of h at t (0..32767) before the current sample */
static inline __attribute__((always_inline)) void blep(int32_t *prev, int32_t *cur, int32_t h, int32_t t)
{
    int32_t v = 32767 - t, h16 = h >> 4;
    *prev += (h16 * ((t * t) >> 15)) >> 12;
    *cur -= (h16 * ((v * v) >> 15)) >> 12;
}


/* the event position: t = x / step in 0..32767, by the step's reciprocal (sh, r) */
static inline __attribute__((always_inline)) int32_t frac_of(uint32_t x, uint32_t sh, uint32_t r)
{
    uint32_t t = ((x >> sh) * r) >> 15;
    return t > 32767u ? 32767 : (int32_t)t;
}

/* the DCO through the block into x: the half-rate phase p (bit 31 = the sub), saw and pulse read p << 1.
 * The pulse is saw(ph) - saw(ph + pw), so the mix is (s + q) * saw(ph) - q * saw(ph + pw) + the sub (+-2b):
 * two 16-bit products (s + q < 2^15). Each edge gets a polyBLEP correction, one sample late. */
static void dco_pass(voice_t *v, int32_t *x, const uint32_t *incs, uint32_t pw, uint32_t sh, uint32_t r)
{
    uint32_t p = v->p;
    int32_t prev = v->prev;
    const int32_t sq = as + ap, q = ap, b2 = 2 * ab, hw = -4 * (as + ap), hp = 4 * ap;
    for (int c = 0; c < NC; c++) {
        const uint32_t inc = incs[c], lim = inc << 1;   /* a wrap leaves the phase under one step */
        for (int i = 0; i < CTL; i++) {
            p += inc;
            const uint32_t ph = p << 1;
            const int32_t sub = (int32_t)p < 0 ? b2 : -b2;
            int32_t cur = ((fmul(sq, (int32_t)(ph >> 16) - 32768) - fmul(q, (int32_t)((ph + pw) >> 16) - 32768)) >> 15) + sub;
            int32_t o = prev;
            if (ph < lim)                       /* wrapped: saw and pulse drop, the sub flips */
                blep(&o, &cur, hw + 2 * sub, frac_of(ph, sh, r));
            if (ph + pw < lim)                  /* the pulse rises */
                blep(&o, &cur, hp, frac_of(ph + pw, sh, r));
            prev = cur;
            *x++ = o;
        }
    }
    v->p = p;
    v->prev = prev;
}

/* the chorus, at half rate (22.05 kHz: a BBD's wet signal is dark anyway). Per voice the last HIST half-rate samples
 * and this block's 32, in one line (no wrap to mask); each half-rate sample is the mean of two (a gentle
 * anti-alias). Two taps with opposite LFOs, linearly interpolated, then the wet sum interpolated back to 44.1 kHz.
 * da, db: each tap's delay at the steps' starts (and the block's end), Q8 internal samples (44.1 kHz).
 * Rate (2026-11-03): the delays were designed as 22.05 kHz samples; now 154 +- 80 internal samples (3.5 ms +- 1.8 ms
 * at 44.1 kHz), up to 117 half-rate samples back: a 120-sample history (5.4 ms; dline32 1,824 bytes over six voices,
 * was 1,152 with 64; the owner approved the memory and the longer copy, 2026-11-03). */
#define JN_CHO_C (154 * 256)                    /* the taps' centre delay, Q8 internal samples: 3.5 ms */
#define HIST 120
_Static_assert(HIST >= ((JN_CHO_C + 80 * 256) >> 9) + 1, "the history holds the longest tap (117) and the sample before it");
#define NH (NS / 2)
static int32_t dline32[OP1_VOICES][(HIST + NH) / 2];        /* int16 samples, word-aligned for the copy */
static int32_t wlast[OP1_VOICES];                           /* the last block's last wet sample */
static int32_t whp[OP1_VOICES];                             /* the wet high-pass's state */
static void chorus_pass(int vi, int32_t *x, const int32_t *da, const int32_t *db)
{
    int16_t *dl = (int16_t *)dline32[vi];
    int32_t w[NH];
    const int32_t dry = dry_g, wet = wet_g;
    for (int j = 0; j < NH; j++)
        dl[HIST + j] = (int16_t)clampi((x[2 * j] + x[2 * j + 1]) >> 2, -32767, 32767);
    int32_t hl = whp[vi];
    for (int c = 0; c < NC; c++) {              /* 4 half-rate samples per step */
        int32_t a = da[c] >> 1, b = db[c] >> 1;
        const int32_t sa = (da[c + 1] - da[c]) >> 3, sb = (db[c + 1] - db[c]) >> 3;
        const int16_t *base = dl + HIST + c * (CTL / 2);
        for (int i = 0; i < CTL / 2; i++) {
            const int16_t *ta = base + i - (a >> 8), *tb = base + i - (b >> 8);   /* >= dl + 3: delays <= 117 */
            const int32_t a0 = ta[0], b0 = tb[0];
            const int32_t y = a0 + (fmul((ta[-1] - a0) >> 1, (a & 255) << 7) >> 15)
                            + b0 + (fmul((tb[-1] - b0) >> 1, (b & 255) << 7) >> 15);
            hl += ((y - hl) * 2253) >> 15;                        /* ~250 Hz at 22.05 kHz: the bass stays dry (no comb on it) */
            w[c * (CTL / 2) + i] = fmul((y - hl) >> 1, wet);       /* the taps, scaled by the wet level, << 14 */
            a += sa; b += sb;
        }
    }
    whp[vi] = hl;
    int32_t wp = wlast[vi];
    for (int j = 0; j < NH; j++) {
        const int32_t wj = w[j];
        /* both terms << 14 (headroom: the sum stays under 2^31 at any dry gain) */
        x[2 * j] = ((fmul(clampi(x[2 * j] >> 1, -32767, 32767), dry) >> 1) + (wp >> 1) + (wj >> 1)) >> 14;
        x[2 * j + 1] = ((fmul(clampi(x[2 * j + 1] >> 1, -32767, 32767), dry) >> 1) + wj) >> 14;
        wp = wj;
    }
    wlast[vi] = wp;
    int32_t *h = dline32[vi];
    for (int i = 0; i < HIST / 2; i++) h[i] = h[i + NH / 2];   /* keep the last HIST */
}

/* the ladder: the soft input stage, the 4th pole fed back one sample late (KSELF accounts for that); then the HPF.
 * States at half scale (+-16383), so a pole's step is one 16-bit fractional multiply, rounded. */
static void ladder_pass(voice_t *v, int32_t *x, const int32_t *gs, const int32_t *ks)
{
    int32_t y1 = v->y1, y2 = v->y2, y3 = v->y3, y4 = v->y4, hp = v->hp;
    const int32_t gh = ghp;
    for (int c = 0; c < NC; c++) {
        const int32_t g = gs[c], k = ks[c];
        for (int i = 0; i < CTL; i++, x++) {
            const int32_t u = soft(*x - (fmul(k, y4) >> 12)) >> 1;   /* k * (2 y4) >> 12 */
            y1 += (fmul(g, u - y1) + 32768) >> 16;
            y2 += (fmul(g, y1 - y2) + 32768) >> 16;
            y3 += (fmul(g, y2 - y3) + 32768) >> 16;
            y4 += (fmul(g, y3 - y4) + 32768) >> 16;
            hp += (fmul(gh, y4 - hp) + 32768) >> 16;
            *x = (y4 - hp) * 2;
        }
    }
    v->y1 = y1; v->y2 = y2; v->y3 = y3; v->y4 = y4; v->hp = hp;
}

static void juni_render(void *self, int16_t *out, const int16_t *knobs)
{
    const int vi = op1_voice_index(self);
    voice_t *v = &vc[vi];
    int32_t half[NS], gs[NC], ks[NC], da[NC + 1], db[NC + 1];
    uint32_t incs[NC];
    if (vi <= last_voice) block_shared(knobs);
    last_voice = vi;

    uint32_t inc = (uint32_t)OP1_PITCH_INC[0];   /* the 88.2 kHz step: per 44.1 kHz sample it turns p at half the note's rate */
    if (inc > 0x3FFFFFFFu) inc = 0x3FFFFFFFu;
    if (v->fresh) {                             /* the firmware writes pitch and velocity around note_on */
        int32_t L = bitlen(inc) - 1, m, n8;
        v->fresh = 0;
        m = L >= 15 ? (int32_t)(inc >> (L - 15)) : (int32_t)(inc << (15 - L));   /* 2^15 .. 2^16 */
        m -= 32768;
        n8 = 12 * 256 * L + LOG2_Q8[m >> 9] + (((LOG2_Q8[(m >> 9) + 1] - LOG2_Q8[m >> 9]) * (m & 511)) >> 9);
        v->note8 = n8 + NOTE_OFF_Q8;
        v->velg = op1_voice_vel_gain(self);
    }
    /* the event positions: t = x / (2 * inc), by a reciprocal made once per block */
    const uint32_t minc = inc << 1;
    const uint32_t sh = bitlen(minc) > 16 ? (uint32_t)(bitlen(minc) - 16) : 0;
    const uint32_t r = minc >= 32768u ? (1u << 30) / (minc >> sh) : 0;

    /* the filter: key tracking (60 %), velocity (up to -8 semitones), the voice card's offset */
    const int32_t cut0 = cut_base + (((v->note8 - 60 * 256) * 154) >> 8) + (((v->velg - 32767) * 8) >> 7)
                       + VCUT8[vi];
    /* the cutoff never closes more than 10 semitones under the note: TONE's bottom is dark, never gone */
    const int32_t nfloor = clampi(v->note8 - 10 * 256, 24 * 256, CUT_MAX * 256 - 1);
    const int32_t env1 = clampi(OP1_VOICE_ENV[OP1_BLOCK - 1], 0, 32767);
    const int32_t e0 = v->env0, de = env1 - e0;
    v->env0 = env1;

    /* the control steps: pitch (glide and bend move inside the block), cutoff, resonance, the chorus delays */
    const uint32_t vph = (uint32_t)vi * 0x2AAAAAAAu;   /* each voice's chorus LFOs start at their own phase */
    for (int c = 0; c <= NC; c++) {
        if (wet_g) {
            const uint32_t t = lfo_s + vph + inc_s * (uint32_t)(CTL * c);
            const uint32_t tf = lfo_f + vph + FS_INC(8.0) * (uint32_t)(CTL * c);
            const int32_t m = (dep_s * tri(t) + dep_f * tri(tf)) >> 15;
            da[c] = JN_CHO_C + m;               /* 3.5 ms +- up to 1.8 ms */
            db[c] = JN_CHO_C - m;
        }
        if (c == NC) break;
        const uint32_t ci = (uint32_t)OP1_PITCH_INC[c * 2 * CTL];
        incs[c] = ci > 0x3FFFFFFFu ? 0x3FFFFFFFu : ci;
        const int32_t env = e0 + ((de * (2 * c + 1)) >> 4);
        int32_t n = clampi(cut0 + ((env * env_amt) >> 15), nfloor, CUT_MAX * 256 - 1);
        const int32_t ni = n >> 8, nf = n & 255;
        gs[c] = CUT_G[ni] + (((CUT_G[ni + 1] - CUT_G[ni]) * nf) >> 8);
        const int32_t kq = KSELF[ni] + (((KSELF[ni + 1] - KSELF[ni]) * nf) >> 8);
        /* below MIDI 48 the resonance tapers to 55 %: a low resonant cutoff booms below the notes */
        const int32_t rq = n >= 48 * 256 ? reso_q15 : (reso_q15 * (18022 + (((n - 24 * 256) * 39322) >> 14))) >> 15;
        ks[c] = (kq * rq) >> 15;                /* Q12 */
    }

    dco_pass(v, half, incs, pw_blk, sh, r);
    if (wet_g) chorus_pass(vi, half, da, db);
    ladder_pass(v, half, gs, ks);

    /* level, velocity (75 .. 100 %), then four-point interpolation up to 44.1 kHz */
    const int32_t gain = (out_g * (24576 + (v->velg >> 2))) >> 15;
    int32_t h3 = v->up3, h2 = v->up2, h1 = v->up1;
    for (int i = 0; i < NS; i++) {
        const int32_t y = (clampi(half[i], -65535, 65535) * gain) >> 15;   /* an HPF transient can pass 2^16 */
        out[2 * i] = op1_sat16(h2);
        out[2 * i + 1] = op1_sat16((-h3 + 9 * h2 + 9 * h1 - y) >> 4);
        h3 = h2; h2 = h1; h1 = y;
    }
    v->up3 = h3; v->up2 = h2; v->up1 = h1;
    jn_vblk[vi] = jn_blocks;
}

static void juni_note_on(void *self, const int16_t *knobs)
{
    const int vi = op1_voice_index(self);
    voice_t *v = &vc[vi];
    (void)knobs;
    v->fresh = 1;
    if (v->env0 < 64) {                         /* a silent voice starts at rest, not where it stopped */
        v->y1 = v->y2 = v->y3 = v->y4 = v->hp = 0;
        v->up3 = v->up2 = v->up1 = 0;
        for (int i = 0; i < (HIST + NH) / 2; i++) dline32[vi][i] = 0;
        wlast[vi] = 0; whp[vi] = 0;
    }
}

const op1_engine_t ENGINE_juni = { juni_note_on, juni_render, 0,
                                   { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_NONE } };

/* ======================= screen (GUI thread) =======================
 * juni.svg + juni_geom.h (gen_svg.py, traced from juni-art.png, the owner's mockup A): a midsummer night on a fjord.
 * The SVG holds the still parts and is drawn through the static-art cache; the parts that move with the sound are
 * drawn here from the same trace, in the encoder colours:
 *   blue  TONE    the sun (disc, rays, halo arcs) sinks wholly into the water at 0 and stands high at the top;
 *                 dim at 0, the encoder blue at the default, whiter at the top; the halo grows with it
 *   green SHAPE   the cliffs' ridge lines carry the DCO's waveform (saw, pulse with the PWM, sub) as their teeth
 *   white SWEEP   the bonfire: the mockup's size at the default SWEEP, smaller / taller with it, flaring with the
 *                 voices' envelope; the tongues flicker and sway, the sparks rise
 *   red   CHORUS  the reflection's dashes slide and stretch with the chorus LFOs (rows in opposite phase), and
 *                 with the voices' envelope, so the water moves whenever notes sound
 * Page 2: RESONANCE widens, brightens and grows the sun's halo, ringing near the top (the filter close to
 * self-oscillation); HPF thins and dims the green waterlines (less weight at the bottom); PWM RATE is the rate of the
 * screen's PWM LFO, which moves the ridge teeth's pulse width and breathes their depth.
 * The screen keeps its own LFO phases (render doesn't run while nothing sounds). */
#include "juni_geom.h"

static const uint8_t C_BLUE[4] = { 0x69, 0x8E, 0xFF, 0xFF }, C_GREEN[4] = { 0x00, 0xED, 0x95, 0xFF };
static const uint8_t C_WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, C_RED[4] = { 0xFF, 0x3A, 0x5D, 0xFF };
static const uint8_t C_REDDIM[4] = { 0x99, 0x23, 0x38, 0xFF };
static uint32_t s_pwm, s_lfo_s, s_lfo_f, s_lfo_e, s_spark, s_frame;
static int32_t s_flame = 0;                     /* the voices' envelope, smoothed, Q15 */

static inline int32_t ssin(uint32_t ph) { return OP1_SINE_TABLE[ph >> 19]; }   /* phase 2^32 = one turn */
static inline int32_t scos(uint32_t ph) { return OP1_SINE_TABLE[(ph >> 19) + 2048 & 8191]; }

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)   /* Q10 */
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}

/* a sun line, cut at the clip line (the sun sets into the fjord); width w (Q10), colour s_sunc */
static uint8_t s_sunc[4], s_haloc[4];
static int32_t s_halow;
static void ln_sky(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    const int32_t h = JS_SUN_CLIP;
    if (y0 > h && y1 > h) return;
    if (y0 > h) { x0 = x1 + (int32_t)(((int64_t)(x0 - x1) * (h - y1)) / (y0 - y1)); y0 = h; }
    else if (y1 > h) { x1 = x0 + (int32_t)(((int64_t)(x1 - x0) * (h - y0)) / (y1 - y0)); y1 = h; }
    ln(r, x0, y0, x1, y1, w, c);
}

/* a colour between a and b, u Q10 */
static void mixc(uint8_t *o, const uint8_t *a, const uint8_t *b, int32_t u)
{
    for (int i = 0; i < 3; i++) o[i] = (uint8_t)(a[i] + (((b[i] - a[i]) * u) >> 10));
    o[3] = 0xFF;
}

#define DEG10(d) ((uint32_t)(int32_t)(d) * 1193046u)       /* 0.1 degree -> phase units (mod 2^32) */

/* an arc of radius rad (Q10) around (cx, cy) from a0 to a1 (phase units, 0 = up, clockwise), sky only */
static void arc(void *r, int32_t cx, int32_t cy, int32_t rad, uint32_t a0, uint32_t a1, int n, int32_t w,
                const uint8_t *c)
{
    int32_t px = cx + ((rad >> 4) * ssin(a0) >> 11), py = cy - ((rad >> 4) * scos(a0) >> 11);
    for (int i = 1; i <= n; i++) {
        const uint32_t a = a0 + (uint32_t)((int32_t)(a1 - a0) / n * i);
        const int32_t x = cx + ((rad >> 4) * ssin(a) >> 11), y = cy - ((rad >> 4) * scos(a) >> 11);
        ln_sky(r, px, py, x, y, w, c);
        px = x; py = y;
    }
}

static void draw_sun(void *r, int32_t tone, int32_t res)
{
    static const uint8_t C_BLUEDIM[4] = { 0x22, 0x2F, 0x55, 0xFF };
    /* TONE: wholly under the horizon at 0, the mockup's place at the default (18000), JS_SUN_UP higher at the top */
    const int32_t cx = JS_SUN_X, cy = JS_SUN_Y + (tone < 18000 ? (int32_t)((int64_t)JS_SUN_DOWN * (18000 - tone) / 18000)
                                                              : -(int32_t)((int64_t)JS_SUN_UP * (tone - 18000) / 14767));
    /* its colour: dim at 0, the encoder blue at the default, a third of the way to white at the top */
    if (tone < 18000) mixc(s_sunc, C_BLUEDIM, C_BLUE, tone * 1024 / 18000);
    else mixc(s_sunc, C_BLUE, C_WHITE, (tone - 18000) * 340 / 14767);
    /* the halo: 0.75 .. 1.0 .. 1.04 with TONE, x 1.06 with RESONANCE, ringing (0.015) above 3/4 of it; RESONANCE also
     * widens it (up to 1.6 x) and whitens it (up to 45 %) */
    int32_t gh = tone < 18000 ? 768 + 256 * tone / 18000 : 1024 + 41 * (tone - 18000) / 14767;   /* Q10 */
    gh = (gh * (1024 + 61 * res / 32767)) >> 10;
    if (res > 24576) gh += ((res - 24576) * 15 / 8192) * ssin(s_frame * 0x0B000000u) >> 15;
    s_halow = JS_W_BLUE + JS_W_BLUE * 614 / 1024 * res / 32767;
    mixc(s_haloc, s_sunc, C_WHITE, 460 * res / 32767);
    arc(r, cx, cy, JS_SUN_R, DEG10(-1150), 0, 10, JS_W_BLUE, s_sunc);  /* the disc, in two halves (an arc spans */
    arc(r, cx, cy, JS_SUN_R, 0, DEG10(1150), 10, JS_W_BLUE, s_sunc);   /* under 180 degrees); its foot is cut */
    /* the rays: a little longer with more TONE (the mockup's length at the default) */
    const int32_t g = 870 + (tone >> 7) * 155 / 256 * 2;              /* Q10: 0.87 .. 1.18, 1.0 at the default */
    for (int i = 0; i < JS_NRAYS; i++) {
        const uint32_t a = DEG10(JS_RAY_A[i]);
        const int32_t r0 = JS_RAY_R0[i], r1 = r0 + (((JS_RAY_R1[i] - r0) >> 4) * g >> 6);
        ln_sky(r, cx + ((r0 >> 4) * ssin(a) >> 11), cy - ((r0 >> 4) * scos(a) >> 11),
               cx + ((r1 >> 4) * ssin(a) >> 11), cy - ((r1 >> 4) * scos(a) >> 11), JS_W_BLUE, s_sunc);
    }
    for (int i = 0; i < JS_NARCS; i++) {
        const int32_t span = JS_ARC_A1[i] - JS_ARC_A0[i];
        arc(r, cx, cy, (JS_ARC_R[i] >> 4) * gh >> 6, DEG10(JS_ARC_A0[i]), DEG10(JS_ARC_A1[i]), 2 + span / 60, s_halow,
            s_haloc);
    }
}

/* the ridges: each point pushed out along its normal by the waveform, 0 .. JS_RIDGE_AMP (Q10 px); one line per
 * table step. Levels are normalised once per frame, so the points need no division */
static void draw_ridges(void *r, const mix_t *m, uint32_t pw)
{
    const int32_t amp = (JS_RIDGE_AMP >> 4) * (1024 + ((tri(s_pwm) * 205) >> 15)) >> 6;   /* breathing: 0.8 .. 1.2 */
    const int32_t tot = m->s + m->q + m->b > 0 ? m->s + m->q + m->b : 1;
    const int32_t ls = (m->s << 15) / tot, lq = (m->q << 15) / tot, lb = (m->b << 15) / tot;   /* Q15, sum 1 */
    const uint32_t pwh = pw >> 16;
    for (int k = 0; k < JS_NRIDGES; k++) {
        int32_t px = 0, py = 0;
        for (int i = JS_RIDGE_START[k]; i < JS_RIDGE_START[k + 1]; i++) {
            const uint32_t ph2 = JS_RPH[i], ph = (ph2 << 1) & 0xFFFF;          /* half rate: bit 15 = the sub */
            const int32_t saw = (int32_t)ph - 32768;
            const int32_t pls = ph < 65536 - pwh ? -32768 : 32767;            /* high for the last (1 - pw) */
            const int32_t sub = ph2 & 0x8000 ? 32767 : -32767;
            const int32_t v = (ls * saw + lq * pls + lb * sub) >> 15;          /* -32768 .. 32767 */
            const int32_t off = ((((v + 32768) >> 4) * (amp >> 4)) >> 8) * JS_RWIN[i] >> 8;   /* Q10 */
            const int32_t x = (JS_RX[i] << 6) + ((off * JS_RNX[i]) >> 7), y = (JS_RY[i] << 6) + ((off * JS_RNY[i]) >> 7);
            if (i > JS_RIDGE_START[k]) ln(r, px, py, x, y, JS_W_GREEN, C_GREEN);
            px = x; py = y;
        }
    }
}

/* the bonfire: hs = height scale (Q10), env = the flare (Q15) */
static void draw_fire(void *r, int32_t sweep, int32_t env)
{
    /* SWEEP: 0.55 at 0, the mockup's 1.0 at the default (11000), 1.3 at the top */
    int32_t hs = sweep < 11000 ? 563 + sweep * 461 / 11000 : 1024 + (sweep - 11000) * 307 / 21767;
    hs += (hs * (env >> 5) >> 10) * 180 >> 10;   /* the envelope: up to 18 % */
    const int32_t ws = 512 + (hs >> 1);                               /* wider as it grows, half as fast */
    const int32_t tk = (1 << 20) / JS_FIRE_H;                         /* Q4 height -> Q20 fraction */
    /* flicker and sway are functions of time and place, not of the stroke, so the traced pieces stay joined */
    const uint32_t t0 = s_frame * 0x05000000u;
    const int32_t sw = (ssin(s_frame * 0x02C00000u) * (600 + (env >> 5))) >> 15;   /* Q10 px at the tips */
    const int32_t fl = 32 + (env >> 10);                                             /* flicker depth, /1024 */
    for (int k = 0; k < JS_NFIRE; k++) {
        const uint8_t *c = JS_FIRE_RED[k] ? C_RED : C_WHITE;
        int32_t px = 0, py = 0;
        for (int i = JS_FIRE_START[k]; i < JS_FIRE_START[k + 1]; i++) {
            const int32_t t = (-JS_FY[i] * tk) >> 10;                /* Q10 height fraction */
            const uint32_t pl = t0 + (uint32_t)JS_FX[i] * 0x00C00000u;
            const int32_t hk = hs + ((hs * fl >> 10) * ssin(pl) >> 15) + ((hs * fl >> 11) * ssin(pl * 3u + 0x1234567u) >> 15);
            const int32_t x = JS_FIRE_X + ((JS_FX[i] * ws) >> 4) + ((sw * (t * t >> 10)) >> 10);
            const int32_t y = JS_FIRE_Y + ((JS_FY[i] * hk) >> 4);
            if (i > JS_FIRE_START[k]) ln(r, px, py, x, y, JS_W_WHITE, c);
            px = x; py = y;
        }
    }
    /* sparks: rise from where the mockup has them, faster with the envelope, then start again lower */
    s_spark += 0x01000000u + (uint32_t)env * 0x200u;
    for (int i = 0; i < JS_NSPARKS; i++) {
        const int32_t u = (int32_t)((s_spark + (uint32_t)i * 0x2492492Au) >> 20) * 5;    /* 0 .. 20 px, Q10 */
        const int32_t dx = (ssin(s_spark * 2u + (uint32_t)i * 0x51000000u) * 1500) >> 15;
        const int32_t x0 = JS_FIRE_X + ((JS_SX0[i] * ws) >> 4) + dx, x1 = JS_FIRE_X + ((JS_SX1[i] * ws) >> 4) + dx;
        const int32_t y0 = JS_FIRE_Y + ((JS_SY0[i] * hs) >> 4) - u + OP1_Q10(10);
        const int32_t y1 = JS_FIRE_Y + ((JS_SY1[i] * hs) >> 4) - u + OP1_Q10(10);
        if (y0 > JS_SPARK_TOP && y1 > JS_SPARK_TOP) ln(r, x0, y0, x1, y1, JS_W_WHITE, C_WHITE);
    }
}

/* the reflection: each dash slides (up to 8 px) and stretches (up to 45 %) with the chorus LFOs and the envelope;
 * the rows travel as a wave, neighbours in opposite phase */
static void draw_water(void *r, const chorus_t *co, int32_t env)
{
    const int32_t ks = co->c * (co->dep_s >> 8) / 80;              /* the slow LFO's share, Q15 */
    const int32_t kf = co->c * (co->dep_f >> 8) / 10;              /* the fast one's (I+II) */
    const int32_t ke = env;                                         /* the voices: always some, with or without chorus */
    int32_t amt = ks + kf + ke;
    if (amt > 32767) amt = 32767;
    const uint8_t *c = co->c ? C_RED : C_REDDIM;
    for (int i = 0; i < JS_NDASH; i++) {
        const uint32_t row = JS_DROW[i], ph = row * 0x26000000u + (uint32_t)JS_DX[i] * 0x00090000u;
        const int32_t sgn = row & 1 ? -1 : 1;
        const int32_t sh = sgn * (((OP1_Q10(8) >> 5) * (ks >> 5) >> 5) * tri(s_lfo_s + ph) >> 15)
                         + (((OP1_Q10(2) >> 5) * (kf >> 5) >> 5) * tri(s_lfo_f + ph) >> 15)
                         + sgn * (((OP1_Q10(6) >> 5) * (ke >> 5) >> 5) * tri(s_lfo_e + ph) >> 15);
        const int32_t st = 1024 + ((460 * amt >> 15) * tri(s_lfo_s + s_lfo_e + ph + 0x40000000u) >> 15);   /* Q10 */
        const int32_t cx = (JS_DX[i] << 6) + sh, h = ((JS_DH[i] << 6) >> 4) * st >> 6, y = JS_DY[i] << 6;
        ln(r, cx - h, y, cx + h, y, JS_W_RED, c);
    }
}

/* the green waterlines: HPF thins them to 0.55 of their width and dims them (the bottom loses its weight) */
static void draw_waterlines(void *r, int32_t hpf)
{
    static const uint8_t C_GREENDIM[4] = { 0x00, 0x7A, 0x4C, 0xFF };
    uint8_t c[4];
    mixc(c, C_GREEN, C_GREENDIM, hpf * 1024 / 32767);
    const int32_t w = JS_W_GREEN - JS_W_GREEN * 461 / 1024 * hpf / 32767;
    for (int k = 0; k < JS_NWL; k++)
        for (int i = JS_WL_START[k] + 1; i < JS_WL_START[k + 1]; i++)
            ln(r, JS_WLX[i - 1] << 6, JS_WLY[i - 1] << 6, JS_WLX[i] << 6, JS_WLY[i] << 6, w, c);
}

static void juni_screen_init(void *scr) { (void)scr; }

static void juni_screen_update(void *scr, int dt)
{
    int16_t k[4];
    mix_t m;
    chorus_t co;
    op1_synth_knobs_live(OP1_SCREEN_SLOT & 7, k);
    for (int i = 0; i < 4; i++) if (k[i] < 0) k[i] = 0;
    shape_mix(k[1], &m);
    chorus_map(k[3], &co);
    s_frame++;
    /* the screen's LFOs, per frame (~60 a second): one frame = 735 internal samples at 44.1 kHz (the audio's steps) */
    const int16_t *p2 = op1_synth_knobs(OP1_SCREEN_SLOT & 7);         /* page 2: RESONANCE, HPF, PWM RATE */
    const int32_t res = knob(p2, 5), hpf = knob(p2, 6), rate = knob(p2, 7);
    s_pwm += PWM_INC(rate) * 735u;                                     /* as render */
    s_lfo_s += co.inc_s * 735u;
    s_lfo_f += FS_INC(8.0) * 735u;
    s_lfo_e += FS_INC(1.1) * 735u;
    const int32_t pwi = (((tri(s_pwm) + 32768) >> 1) * m.pwd) >> 15;
    const uint32_t pw = 0x80000000u + ((uint32_t)(pwi < 26214 ? pwi : 26214) << 16);
    /* the loudest sounding voice's envelope (a voice that rendered in the last few blocks sounds) */
    int32_t env = 0;
    const uint32_t blocks = jn_blocks;
    for (int i = 0; i < OP1_VOICES; i++)
        if (blocks - jn_vblk[i] < 8u && vc[i].env0 > env) env = vc[i].env0;
    s_flame += (env - s_flame) >> 2;

    op1_screen_draw_cached(scr, dt, 0);                 /* juni.svg is all still: the whole of it is cached */
    void *r = op1_raster();
    draw_waterlines(r, hpf);
    draw_sun(r, k[0], res);
    draw_water(r, &co, s_flame);
    draw_ridges(r, &m, pw);
    draw_fire(r, k[2], s_flame);
}

const op1_screen_t SCREEN_juni = { juni_screen_init, juni_screen_update };
