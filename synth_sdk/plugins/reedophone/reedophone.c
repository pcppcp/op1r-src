/* reedophone: a clarinet by physical modelling (no samples, no stored waveforms).
 *
 *   blue  : breath   - blowing pressure (dynamics): soft and dark .. loud and bright. With the stock envelope and
 *                      velocity it sets how far above the reed's oscillation threshold the player blows. The
 *                      output gain makes up most of the level it changes (only the level: the engine is as is).
 *   green : growl    - centre: none. Down: a hummed growl (the player hums into the mouthpiece), up: flutter-
 *                      tongue (the tongue rattling in the air stream); both ends go on into multiphonic chaos
 *   white : bore     - brightness of the bore (cutoff of the tone-hole lattice reflection)
 *   red   : air      - breath noise in the air jet
 *
 * Model (dimensionless, after Guillemain, Kergomard & Voinier, JASA 118 (2005), "Real-time synthesis of
 * clarinet-like instruments using digital impedance models"):
 *   reed : x'' / wr^2 + qr x' / wr + x = p                       (2nd-order, explicit: x(n) from p(n-1))
 *   flow : u = zeta (1 - gamma + x)+ sgn(gamma - p) sqrt|gamma - p|, with p = u + 2 p-  -> closed form:
 *          u = sgn(V) A (sqrt(A^2 + 4|V|) - A) / 2,  A = zeta (1 - gamma + x),  V = gamma - 2 p-
 *   bore : waveguide, round trip = half a period, on every note (a clarinet's low register carried all the way
 *          up: no register vent, so the tone does not jump at written B4 as on the real instrument).
 *          Open end: reflection -g LP(fc), the tone-hole lattice; LP phase delay compensated in the loop length.
 *   out  : the wave leaving the open end ((1 + R) e), radiation high-pass, a body EQ and a gentle low-pass.
 * Blowing pressure gamma = oscillation threshold (from the linear stability of the flow at the bore's impedance
 * peak, computed per pitch) + an excess set by breath x envelope: soft notes sit just above threshold (near
 * sine, slow build-up), loud ones well above (beating reed, bright). All constants were fitted to recordings of a
 * clarinet (tools/: clar_eval.py scores spectra, attack, noise and level; fit.py runs CMA-ES on tools/proto.c).
 *
 * Copyright (C) 2026 nvmsocool. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"
#include "reed_params.h"
#include "reed_tune.h"                 /* TUNE[midi][dyn]: measured tuning error (tools/calib.py) */

/* growl knob ends (tools: RV_CFLAGS to try others) */
#ifndef HUM_DMAX_Q14
#define HUM_DMAX_Q14 21299            /* hum depth at the bottom: 1.3 x the breath */
#endif
#ifndef HUM_JMAX_Q15
#define HUM_JMAX_Q15 1966             /* + up to 6 % hum pitch wander */
#endif
#ifndef FLT_DMAX_Q14
#define FLT_DMAX_Q14 20480            /* flutter depth at the top: 1.25 (the air is cut off) */
#endif
#ifndef FLT_JMAX_Q15
#define FLT_JMAX_Q15 8192             /* + up to 25 % flutter rate wander */
#endif

#define LN 1024                      /* samples per delay line (one direction), power of 2 */
#define LM (LN - 1)
#define SIG 0x52454544u              /* "REED": voice state in st[] belongs to this engine */

typedef struct {
    uint32_t vtable;                 /* firmware-owned */
    uint32_t sig;
    uint8_t  gate, fresh, rel_init, tongued;   /* tongued: this attack starts with the reed held shut */
    int32_t  rel_e;                  /* envelope at the start of the release (ratio reference) */
    uint32_t inc;                    /* pitch the delays were set up for */
    int16_t  kn[4];                  /* knobs the derived values were computed for */
    uint32_t pad2;
} rv_voice_t;
typedef char rv_voice_fits[sizeof(rv_voice_t) <= OP1_VOICE_SIZE ? 1 : -1];

typedef struct {
    int16_t q[LN][2];                /* waves, Q13, interleaved: forward (reed -> open end), back. One write
                                      * pointer; every tap a circular pointer. */
    uint32_t w;                      /* common write index */
    int32_t x1, x2, pprev;           /* reed displacement history, last mouthpiece pressure (Q13) */
    int32_t lps;                     /* end low-pass state (Q13 << 15) */
    int32_t gam, g1;                 /* blowing pressure and its first smoothing stage (Q13 << 16), block ends */
    int32_t tongue;                  /* samples left with the reed held shut by the tongue */
    uint32_t seed; int32_t nz;       /* breath noise: xorshift32, one-pole colour (Q15) */
    uint32_t hph, fph;               /* growl: hum and flutter phases */
    int32_t hlp, flp, hj, fj;        /* hum and tongue smoothing (Q15), their pitch wander (Q15) */
    int32_t hx, hy;                  /* radiation high-pass: input (Q13), output (Q21) */
    int32_t ex, ey, lx, ly;          /* body EQ and output low-pass, x1 | x2 << 16 (Q12) */
    /* per pitch / knobs */
    int32_t lbb, lbf_i;              /* integer delays */
    int32_t lc[4];                   /* Lagrange coefficients of the fractional read (Q14) */
    int32_t lpc, gq15;               /* end low-pass coefficient (1 - a) and loss gain, Q15 */
    int32_t zeta;                    /* Q15 */
    int32_t gth;                     /* oscillation threshold, Q13 */
    int32_t noise;                   /* Q15 */
    uint32_t hinc, finc;             /* growl: hum pitch (a tritone below the note, folded to 85..170 Hz), flutter rate */
    int32_t hum_d, flt_d;            /* depths (pressure modulation, Q14) */
    int32_t hjit, fjit;              /* pitch / rate wander (Q15 fraction): grows into chaos at the knob's ends */
    int32_t outg;                    /* output gain, Q8: OUT_GAIN with the growl's level make-up */
    uint32_t P;                      /* period, Q16 samples */
    int32_t tau, midi_q8, corr;      /* end low-pass phase delay (Q16), pitch, tuning correction applied (cents Q4) */
    int32_t dyn_scale;               /* breath knob, Q15 (1.0 at 26214) */
    int32_t rt;                      /* loop length the delays are set for (Q16 samples), 0 after a reset */
    int32_t age[2];                  /* the reads' ages behind the newest sample, forward / back line (Q16) */
} rv_state_t;
static rv_state_t st[OP1_VOICES];
/* for the screen (reedscr.c, GUI thread): gates, MIDI notes, a note-on count, a render tick per voice (to skip
 * stopped voices) and how far the stock envelope has risen toward its peak (Q15: the attack's progress) */
volatile int16_t rv_scr_att[OP1_VOICES];
volatile uint8_t rv_scr_gate[OP1_VOICES], rv_scr_note[OP1_VOICES];
volatile uint32_t rv_scr_notes, rv_scr_tick[OP1_VOICES];

/* ---------------------------------------------------------------- integer math helpers */
#ifdef __bfin__
static inline int msb32(uint32_t v) { return (int32_t)v < 0 ? 31 : 30 - __builtin_bfin_norm_fr1x32((int32_t)v); }
static inline int norm30(int32_t v) { return __builtin_bfin_norm_fr1x32(v); }        /* 30 - msb, 0 < v < 2^31 */
#else
static inline int msb32(uint32_t v) { return 31 - __builtin_clz(v); }
static inline int norm30(int32_t v) { return __builtin_clz((uint32_t)v) - 1; }
#endif
/* 16 x 16 -> 32 multiply: one MAC on the Blackfin (left alone, gcc uses the multi-cycle 32-bit multiply). Both
 * operands must fit int16: the host build checks that and aborts (tools/clar_eval.py and the tests run it). */
#ifdef __bfin__
static inline int32_t m16(int32_t a, int32_t b) { return (int16_t)a * (int16_t)b; }   /* gcc emits the MAC */
#else
#include <stdlib.h>
static inline int32_t m16(int32_t a, int32_t b)
{
    if (a != (int16_t)a || b != (int16_t)b) abort();
    return a * b;
}
#endif
static inline int32_t clampi(int32_t x, int32_t lo, int32_t hi) { x = x < lo ? lo : x; return x > hi ? hi : x; }   /* max, min */
#define sat16(x) clampi((x), -32768, 32767)
/* a (int32) x b (Q15) without 64-bit math */
static inline int32_t mul32x15(int32_t a, int32_t b) { return (a >> 15) * b + (((a & 0x7fff) * b) >> 15); }

/* sqrt(i * 2^24) - 32768 for i = 64..256 (w = v << 2k has its top bit at 30 or 31) */
static const uint16_t SQT[193] = {
#include "reed_sqrt.h"
};
static inline uint32_t isqrt_fast(uint32_t v)   /* ~sqrt(v), 4 <= v < 2^31, relative error < 3e-5 */
{
    int k = (31 - msb32(v)) >> 1;
    uint32_t w = v << (2 * k), i = (w >> 24) - 64, f = (w >> 8) & 0xffff;
    uint32_t a = SQT[i] + 32768u, b = SQT[i + 1] + 32768u;
    return (a + (((b - a) * f) >> 16)) >> k;
}
/* log2(1 + i/32) Q16 and 2^(i/32) Q15, i = 0..32 */
static const uint32_t L2T[33] = { 0, 2909, 5732, 8473, 11136, 13727, 16248, 18704, 21098, 23433, 25711, 27936, 30109, 32234, 34312, 36346, 38336, 40286, 42196, 44068, 45904, 47705, 49472, 51207, 52911, 54584, 56229, 57845, 59434, 60997, 62534, 64047, 65536 };
static const uint32_t E2T[33] = { 32768, 33486, 34219, 34968, 35734, 36516, 37316, 38133, 38968, 39821, 40693, 41584, 42495, 43425, 44376, 45348, 46341, 47356, 48393, 49452, 50535, 51642, 52773, 53928, 55109, 56316, 57549, 58809, 60097, 61413, 62757, 64132, 65536 };
static int32_t log2_q16(uint32_t v)             /* v > 0 */
{
    int e = msb32(v);
    uint32_t m = e >= 16 ? v >> (e - 16) : v << (16 - e);   /* 1.16 */
    uint32_t fr = m & 0xffff, i = fr >> 11, f = fr & 0x7ff;
    return (e << 16) + (int32_t)(L2T[i] + (((L2T[i + 1] - L2T[i]) * f) >> 11));
}
static uint32_t exp2_q15(int32_t x)             /* 2^(x / 65536) in Q15, x <= 16 << 16 */
{
    int32_t n = x >> 16; uint32_t fr = (uint32_t)x & 0xffff, i = fr >> 11, f = fr & 0x7ff;
    uint32_t m = E2T[i] + (((E2T[i + 1] - E2T[i]) * f) >> 11);
    return n >= 0 ? m << n : (n > -31 ? m >> -n : 0);
}
/* breath knob level make-up: 3/4 of the measured sustain level change against the default (80 %), undone at the
 * output. Octaves, Q16, at these knob values (linear between). */
static const int32_t BLUE_K[8]  = { 0, 4096, 8192, 12288, 16384, 20480, 26214, 32767 };
static const int32_t BLUE_MK[8] = { 147768, 142053, 120827, 81640, 46535, 24492, 0, -22859 };
static int32_t blue_mk(int32_t k)
{
    int i = 0; while (i < 6 && k > BLUE_K[i + 1]) i++;
    return BLUE_MK[i] + (int32_t)(((int64_t)(BLUE_MK[i + 1] - BLUE_MK[i]) * (k - BLUE_K[i])) / (BLUE_K[i + 1] - BLUE_K[i]));
}
/* sin(2 pi ph / 2^32), Q15, interpolated */
static int32_t sin_q15(uint32_t ph)
{
    uint32_t i = ph >> 19, f = (ph >> 3) & 0xffff;
    int32_t a = OP1_SINE_TABLE[i], b = OP1_SINE_TABLE[(i + 1) & 8191];
    return a + (((b - a) * (int32_t)f) >> 16);
}
/* atan(t) for t in Q15 [0, 1] -> radians Q16 */
static const uint16_t ATT[33] = { 0, 2047, 4091, 6126, 8150, 10158, 12147, 14114, 16055, 17968, 19850, 21699, 23512, 25289, 27028, 28727, 30386, 32003, 33580, 35115, 36608, 38060, 39472, 40842, 42172, 43464, 44716, 45931, 47109, 48251, 49359, 50432, 51472 };
static int32_t atan_q16(int32_t y, int32_t x)   /* atan2 for x > 0, y >= 0 */
{
    int inv = y > x; int32_t t = inv ? (int32_t)(((int64_t)x << 15) / y) : (int32_t)(((int64_t)y << 15) / x);
    uint32_t i = (uint32_t)t >> 10, f = (uint32_t)t & 1023;
    if (i >= 32) { i = 31; f = 1024; }
    int32_t a = ATT[i] + (int32_t)(((ATT[i + 1] - ATT[i]) * f) >> 10);
    return inv ? 102944 - a : a;                 /* pi/2 = 102944 (Q16) */
}

/* ---------------------------------------------------------------- per pitch / knob setup (per block, on change) */
#define L2_A4 1661526                            /* log2(2^32 * 440 / 44100) * 65536 */
static void setup(rv_voice_t *v, rv_state_t *s, uint32_t inc, const int16_t *k)
{
    if (inc < 400000u) inc = 400000u;            /* ~4 Hz: keeps every delay inside its line */
    int32_t l2 = log2_q16(inc);
    int32_t midi_q8 = (69 << 8) + (int32_t)(((int64_t)(l2 - L2_A4) * 12) >> 8);
    int32_t oct_q15 = (int32_t)(((int64_t)(midi_q8 - (70 << 8)) * 32768) / (12 << 8));
    /* period in Q16 samples, f0 in Hz Q8 */
    uint32_t P = (uint32_t)((((uint64_t)1) << 48) / inc);
    uint32_t f0_q8 = (uint32_t)(((uint64_t)inc * 44100u) >> 24);
    /* end low-pass: cutoff = max(FC, FCK f0) x bore knob (2^-1 .. 2^1) */
    uint32_t fc_q8 = (uint32_t)FC_HZ << 8, fl = (uint32_t)(((uint64_t)f0_q8 * FCK_Q8) >> 8);
    if (fl > fc_q8) fc_q8 = fl;
    fc_q8 = (uint32_t)(((uint64_t)fc_q8 * exp2_q15(((int32_t)k[2] - 16384) * 4)) >> 15);
    if (fc_q8 > (16000u << 8)) fc_q8 = 16000u << 8;
    /* a = exp(-2 pi fc / SR) = 2^(-2 pi fc / SR * log2 e) */
    int32_t xw = (int32_t)(((uint64_t)fc_q8 * 3449u) >> 16);          /* Q16: fc 2 pi log2(e) / 44100; 2^8 x 0.0526 */
    int32_t a = (int32_t)exp2_q15(-xw);                                 /* Q15 */
    s->lpc = 32768 - a; if (s->lpc > 32767) s->lpc = 32767;
    /* LP response at f0: H = (1-a) / (1 - a e^-jw) */
    int32_t sn = sin_q15(inc), cs = sin_q15(inc + 0x40000000u);
    int32_t re = 32768 - ((a * cs) >> 15), im = (a * sn) >> 15;        /* re > 0 */
    if (im < 0) im = 0;
    int32_t th = atan_q16(im, re);                                      /* phase lag, rad Q16 */
    /* tau = th / w0 = th / (2 pi) * P  (Q16 samples) */
    uint32_t tau = (uint32_t)(((uint64_t)th * 10430u >> 16) * (uint64_t)P >> 16);   /* 65536 / 2pi = 10430.4 */
    uint32_t den = (uint32_t)(((int64_t)re * re + (int64_t)im * im) >> 15);          /* |1 - a e^-jw|^2, Q15 */
    /* |H| = (1-a) / sqrt(den), Q15 (isqrt of Q30) */
    int32_t mag = (int32_t)isqrt_fast((uint32_t)((((uint64_t)(32768 - a) * (32768 - a)) << 15) / (den ? den : 1)) | 4);
    if (mag > 32767) mag = 32767;
    /* loss g = 1 - LOSS (f0 / 261.6)^k */
    int32_t pw = (int32_t)exp2_q15((int32_t)(((int64_t)(midi_q8 - (60 << 8)) * LOSS_K_Q12 / 12) >> 4));   /* Q15 */
    s->gq15 = 32768 - (int32_t)(((int64_t)LOSS_Q15 * pw) >> 15);
    if (s->gq15 > 32767) s->gq15 = 32767;
    if (s->gq15 < 16384) s->gq15 = 16384;
    int32_t R = (int32_t)(((int64_t)s->gq15 * mag) >> 15);              /* |reflection| at f0, Q15 */
    if (R > 32700) R = 32700;
    /* embouchure */
    int32_t z = ZETA_Q15 + (int32_t)(((int64_t)ZSLOPE * oct_q15) >> 15);
    s->zeta = clampi(z, 600, 32767);
    /* threshold: (3g - 1) / (2 sqrt g) = c = (1 - R) / (zeta (1 + R));  sqrt g = (c + sqrt(c^2 + 3)) / 3 */
    int32_t c = (int32_t)(((int64_t)(32768 - R) << 27) / ((int64_t)s->zeta * (32768 + R)));     /* Q12 */
    if (c > 32767) c = 32767;
    int32_t sq = (c + (int32_t)isqrt_fast((uint32_t)(c * c) + (3u << 24) + 4)) / 3;               /* Q12 */
    s->gth = (sq * sq) >> 11;                                                                   /* Q13 */
    s->P = P; s->tau = (int32_t)tau; s->midi_q8 = midi_q8; s->corr = 0x40000000;   /* forces set_delays */
    /* knobs */
    s->dyn_scale = (int32_t)(((int64_t)k[0] << 15) / 26214);
    s->noise = (int32_t)(((int64_t)NOISE_Q15 * k[3]) >> 13);
    {   /* growl knob: a = distance from the centre (Q15) */
        int32_t ah = k[1] < 16384 ? (16384 - (int32_t)k[1]) * 2 : 0, af = k[1] > 16384 ? ((int32_t)k[1] - 16384) * 2 : 0;
        ah = (ah * ah) >> 15; af = (af * af) >> 15;      /* squared: more of the knob for the subtle, realistic range */
        int32_t sq = (int32_t)isqrt_fast(((uint32_t)ah << 15) | 4);                       /* sqrt(ah), Q15 */
        int32_t a3 = (((ah * ah) >> 15) * ah) >> 15, f3 = (((af * af) >> 15) * af) >> 15;
        s->hum_d = (int32_t)((((int64_t)ah * sq) >> 15) * HUM_DMAX_Q14 >> 15);            /* depth ~ a^1.5 */
        s->hjit = ah ? 164 + ((HUM_JMAX_Q15 * a3) >> 15) : 0;                              /* 0.5 % .. */
        s->flt_d = (af * FLT_DMAX_Q14) >> 15;
        s->fjit = af ? 983 + ((FLT_JMAX_Q15 * f3) >> 15) : 0;                              /* 3 % .. */
        s->finc = 2142397u + (uint32_t)((973910 * (int64_t)af) >> 15);                    /* 22 .. 32 Hz */
        /* level make-up: the chaos is quieter (measured: -10.6 dB at full hum ~ a^3, -3.4 dB at full flutter ~ a^1.5) */
        int32_t fs = (int32_t)isqrt_fast(((uint32_t)af << 15) | 4);
        int32_t mk = (int32_t)(((int64_t)a3 * 115400 + (int64_t)((af * (int64_t)fs) >> 15) * 37000) >> 15);   /* octaves Q16 */
        mk += blue_mk(k[0]);                                                               /* + the breath knob's */
        s->outg = (int32_t)(((int64_t)OUT_GAIN * exp2_q15(mk)) >> 15); if (s->outg > 32767) s->outg = 32767;
        uint32_t h = (uint32_t)(((uint64_t)inc * 46341u) >> 16);                           /* a tritone below */
        while (h > 16556555u) h >>= 1;                                                     /* fold into 85 .. 170 Hz */
        while (h < 8278277u) h <<= 1;
        s->hinc = h;
    }
    v->inc = inc; for (int i = 0; i < 4; i++) v->kn[i] = k[i];
}

/* loop lengths for the period corrected by `corr` (cents Q4, from the tuning table) */
static void set_delays(rv_state_t *s, int32_t corr)
{
    s->corr = corr;
    /* loop length (Q16): round trip = P/2, minus the low-pass phase delay */
    uint32_t P = (uint32_t)(((uint64_t)s->P * exp2_q15((corr * 3495) >> 10)) >> 15);   /* x 2^(cents / 1200): Q16 = corr * 3.413 */
    int32_t rt = (int32_t)((P >> 1) - (uint32_t)s->tau);
    if (rt < (6 << 16)) rt = 6 << 16;
    s->rt = rt;
    s->lbb = ((rt - 65536) >> 1) >> 16;                                  /* back: integer, delay lbb + 1 */
    int32_t lf = rt - 65536 - (s->lbb << 16);                            /* forward: the rest, fractional */
    s->lbf_i = lf >> 16; int32_t frac = lf & 0xffff;
    if (s->lbb > LN - 4) s->lbb = LN - 4;
    if (s->lbf_i < 1) { s->lbf_i = 1; frac = 0; }
    if (s->lbf_i > LN - 4) s->lbf_i = LN - 4;
    s->age[0] = (s->lbf_i << 16) + frac - 65536; s->age[1] = s->lbb << 16;
    /* 4-point Lagrange around lbf_i (taps at delay i-1, i, i+1, i+2), f in Q15 */
    int32_t f = frac >> 1, fm1 = f - 32768, fm2 = f - 65536, fp1 = f + 32768;
    int32_t t01 = (int32_t)(((int64_t)f * fm1) >> 15);                  /* f (f-1) */
    s->lc[0] = (int32_t)(-(((int64_t)t01 * fm2) >> 15) / 6) >> 1;
    s->lc[1] = (int32_t)((((int64_t)fp1 * fm1 >> 15) * fm2) >> 15) / 2 >> 1;
    s->lc[2] = (int32_t)(-((((int64_t)fp1 * f >> 15) * fm2) >> 15) / 2) >> 1;
    s->lc[3] = (int32_t)((((int64_t)fp1 * t01) >> 15) / 6) >> 1;
}

/* A slur (legato note change) with the air column sounding: the wave in each line is time-scaled so that the
 * sample its read was at (old age a0) is where the new read is (age a1), and the newer samples follow. The reads
 * then continue the same waveform at the new pitch; jumping the read positions puts a step into it: a click.
 * Going up, the scale (old samples per new one) is 1 at the read, so the waveform's slope does not jump there
 * either (with a constant scale it would, at a steep edge: still a click): from the newest sample to 3/4 of the way
 * it is 1 + K, K = 8/7 (r - 1), r = a0 / a1, then ramps down to 1 at the read. Old age for new age a:
 * a (1 + K) - K 2 (a - b)^2 / a1 past b = 3/4 a1, the square summed up as it goes (no 64-bit multiply per sample).
 * Going down (and past 3 octaves up) the scale is a constant r: measured clean. Ages 0 (newest, at w) .. a1 + 3. */
static inline void warp_set(rv_state_t *s, int c, int32_t w, int32_t a, uint32_t src)   /* age a <- old age src (Q16) */
{
    int32_t i = (int32_t)(src >> 16), f = (int32_t)((src & 0xffff) >> 1);              /* Q15 */
    if (i > LN - 2) { i = LN - 2; f = 0; }
    int32_t p0 = s->q[(w - i) & LM][c], p1 = s->q[(w - i - 1) & LM][c];
    s->q[(w - a) & LM][c] = (int16_t)(p0 + (((p1 - p0) * f) >> 15));
}
static void warp(rv_state_t *s, const int32_t *a0)
{
    const int32_t w = (int32_t)s->w;
    for (int c = 0; c < 2; c++) {
        int32_t a1 = s->age[c];
        if (a1 < 65536 || a0[c] < 65536 || a1 == a0[c]) continue;                /* a read on the newest samples */
        uint32_t r = (uint32_t)(((uint64_t)(uint32_t)a0[c] << 16) / (uint32_t)a1);   /* old / new, Q16 */
        if (r > (16u << 16)) r = 16u << 16;
        int32_t amax = (a1 >> 16) + 3;
        if (r < 65536 || r > (8u << 16)) {
            /* in place: stretching reads younger ages than the one written, so go down; shrinking older: up */
            int up = r > 65536, a = up ? 0 : amax, da = up ? 1 : -1;
            for (int n = 0; n <= amax; n++, a += da) warp_set(s, c, w, a, (uint32_t)a * r);
            continue;
        }
        int32_t K = (int32_t)((r - 65536) * 8 / 7), rate = 65536 + K;           /* Q16 */
        int32_t b = (a1 >> 2) * 3, ab = (b >> 16) + 1, e = (ab << 16) - b;       /* first age past b, its distance */
        int64_t T = (int64_t)K * (int32_t)((((uint64_t)(uint32_t)e * (uint32_t)e) << 1) / (uint32_t)a1);  /* Q32 */
        int64_t D = (((int64_t)K * (2 * e + 65536)) << 17) / a1;                 /* T(a + 1) - T(a), Q32 */
        int64_t DD = ((int64_t)K << 34) / a1;                                    /* its step, Q32 */
        for (int a = 0; a <= amax; a++) {                                        /* reads ages >= a: up */
            int32_t src = a * rate;
            if (a >= ab) { src -= (int32_t)(T >> 16); T += D; D += DD; }
            warp_set(s, c, w, a, (uint32_t)src);
        }
    }
}

/* ---------------------------------------------------------------- engine */
static void rv_reset(rv_state_t *s)
{
    for (int i = 0; i < LN; i++) s->q[i][0] = s->q[i][1] = 0;
    s->w = 0; s->rt = 0; s->tongue = 0; s->x1 = s->x2 = s->pprev = 0; s->lps = 0; s->gam = s->g1 = 0;
    s->nz = 0; s->hx = s->hy = 0; s->hph = s->fph = 0; s->hlp = s->flp = s->hj = s->fj = 0; s->ex = s->ey = s->lx = s->ly = 0;
}

static void rv_note_on(void *self, const int16_t *params)
{
    rv_voice_t *v = (rv_voice_t *)self; (void)params;
    int vi = op1_voice_index(self); if (vi < 0 || vi >= OP1_VOICES) vi = 0;
    rv_state_t *s = &st[vi];
    /* a voice still sounding keeps its air column (a slur); otherwise start clean */
    v->tongued = 0;
    if (v->sig != SIG || s->gam < (32 << 16)) { rv_reset(s); s->seed = 0x9E3779B9u * (uint32_t)(vi + 1); v->tongued = 1; }
    v->sig = SIG; v->gate = 1; v->fresh = 1; v->rel_init = 0; v->inc = 0;
    rv_scr_gate[vi] = 1; rv_scr_notes++;
}

#define TONGUE_FADE_SMP 768                     /* output fade-in after the tongue's release (17 ms) */
#define FADE_RCP ((((int32_t)32768 << 10) + TONGUE_FADE_SMP - 1) / TONGUE_FADE_SMP)   /* x = e * FADE_RCP >> 10 */
#ifndef HOLD_P_Q13
#define HOLD_P_Q13 6963                         /* blowing pressure cap while the tongue holds the reed (0.85) */
#endif
#ifndef OPEN_RAMP_SMP
#define OPEN_RAMP_SMP 1102                      /* the cap lifts over 25 ms after the release */
#endif
#ifndef TONGUE_SMP
#define TONGUE_SMP 2048                         /* tongue hold at a fresh start, every note (46 ms) */
#endif
#define NO_TONGUE (-(1 << 20))                   /* tongue counter of an untongued (slurred) start: no fade */

static void rv_note_off(void *self)
{
    ((rv_voice_t *)self)->gate = 0;
    int vi = op1_voice_index(self); if (vi >= 0 && vi < OP1_VOICES) rv_scr_gate[vi] = 0;
}

/* blowing pressure for the block: per-sample ramp of the block's breath step, with breath noise (Q13) */
static void breath(rv_state_t *s, int16_t *g, int32_t gam, int32_t gstep)
{
    int32_t nz = s->nz; uint32_t seed = s->seed; const int16_t noise = (int16_t)s->noise;
    for (int n = 0; n < OP1_BLOCK; n++) {
        gam += gstep;
        int32_t gm = clampi(gam >> 16, 0, 16383);
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;   /* xorshift32 */
        nz = (m16((int32_t)seed >> 16, 9830) + m16(nz, 22938)) >> 15;   /* coloured noise, 0.3 new / 0.7 old */
        g[n] = (int16_t)(gm + (m16(gm, m16(noise, nz) >> 15) >> 15));   /* noise <= 2^12 */
    }
    /* growl: the mouth pressure modulated by a hum (green below centre) or the tongue (above). Pitch / rate wander
     * per block, a slow random walk (more of it nearer the knob's ends). All products 16 x 16. */
    if (s->hum_d) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        s->hj += (((int32_t)seed >> 16) - s->hj) >> 3;
        const uint32_t hi = s->hinc + (uint32_t)((int32_t)(s->hinc >> 15) * ((s->hjit * s->hj) >> 15));
        uint32_t hph = s->hph; int32_t hlp = s->hlp; const int32_t hd = s->hum_d;
        for (int n = 0; n < OP1_BLOCK; n++) {     /* a buzz: sawtooth through a ~800 Hz low-pass (glottal pulses) */
            hph += hi;
            hlp += m16(((int32_t)hph >> 18) - hlp, 3500) >> 15;                 /* Q13 */
            int32_t mod = m16(hd, hlp) >> 13, p = g[n];                          /* Q14 */
            g[n] = (int16_t)clampi(p + (m16(p, mod) >> 14), 0, 16383);
        }
        s->hph = hph; s->hlp = hlp;
    } else if (s->flt_d) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        s->fj += (((int32_t)seed >> 16) - s->fj) >> 2;
        const uint32_t fi = s->finc + (uint32_t)((int32_t)(s->finc >> 15) * ((s->fjit * s->fj) >> 15));
        uint32_t fph = s->fph; int32_t flp = s->flp; const int32_t fd = s->flt_d;
        for (int n = 0; n < OP1_BLOCK; n++) {     /* the tongue touches the air stream ~45 % of each cycle */
            fph += fi;
            int32_t tri = (int32_t)fph >> 17; tri = tri < 0 ? -tri : tri;       /* 0 .. 16384 */
            int32_t pul = clampi(((tri - 9011) * 73) >> 5, 0, 16383);
            flp += m16(pul - flp, 1400) >> 15;                                   /* Q14, smoothed */
            int32_t mod = -(m16(fd, flp - 3686) >> 14), p = g[n];               /* about its mean (0.225) */
            g[n] = (int16_t)clampi(p + (m16(p, mod) >> 14), 0, 16383);
        }
        s->fph = fph; s->flp = flp;
    }
    s->nz = nz; s->seed = seed;
}

/* Per-block inputs of the sample loops. The Blackfin runs them in assembly (rv_asm.h); the C below is the
 * reference, bit-exact with it (tools/asm_check.c compares the two on the simulator). */
typedef struct {
    int16_t k[16];                   /* B1 A1 | A2 zeta | c3 c2 | c1 c0 | 0 0 | 0 0 | lpc gq | EQ b0 b1 */
    int16_t f[8];                    /* EQ b2 -a1 | -a2 0 | LP b0 b1 | b2 -a1  (offset 32) */
    int16_t f2[4];                   /* LP -a2, RADHP, OUT_GAIN, 0      (offset 48) */
    int16_t *g;                      /* 56: breath in, (e + r) / 2 out */
    int16_t *out;                    /* 60 */
    const uint32_t *sqd;             /* 64: SQD - 64 entries */
    int16_t *base;                   /* 68: the lines */
    int16_t *ip[4];                  /* 72: start pointers: unused, back read, fractional read, write */
    int32_t x12, pp, lps;            /* 88: x1 | x2 << 16, pprev (low half), end low-pass */
    int32_t hy, ex, ey, lx, ly;      /* 100: radiation high-pass, filter states (x1 | x2 << 16) */
} rv_io_t;

static const uint32_t SQD[192] = {
#include "reed_sqd.h"
};
static inline int32_t sat16i(int64_t x) { return x > 32767 ? 32767 : (x < -32768 ? -32768 : (int32_t)x); }
static inline int32_t neg16(int32_t x) { return x == -32768 ? 32767 : -x; }
static inline int32_t pk(int32_t lo, int32_t hi) { return (int32_t)((uint32_t)(uint16_t)lo | ((uint32_t)hi << 16)); }
#define LO(x) ((int32_t)(int16_t)(x))
#define HI(x) ((int32_t)(x) >> 16)

/* the air column: reed, flow, bore, open end. Writes (e + r) / 2 over g. */
static __attribute__((unused)) void bore_c(rv_io_t *io)
{
    int16_t *Q = io->base;                         /* q[i][2] = forward, back */
    const int16_t *K = io->k;
    uint32_t r1 = (uint32_t)(io->ip[1] - Q) >> 1;
    uint32_t r2 = (uint32_t)(io->ip[2] - Q) >> 1, wq = (uint32_t)(io->ip[3] - Q) >> 1;
    int32_t x1 = LO(io->x12), x2 = HI(io->x12), pp = LO(io->pp), ph = HI(io->pp), lps = io->lps;
    for (int n = 0; n < OP1_BLOCK; n++) {
        int32_t gm = io->g[n];
        int32_t pmin = Q[2 * r1 + 1];
        int32_t x = sat16i(((int64_t)K[0] * pp + (int64_t)K[1] * x1 + (int64_t)K[2] * x2) >> 14);
        x2 = x1; x1 = x;
        int32_t tA = sat16i((8192 - gm) + x);
        int32_t A = sat16i((K[3] * tA) >> 15); if (A < 0) A = 0;
        int32_t V = sat16i(gm - sat16i(2 * pmin)), av = V == -32768 ? 32767 : (V < 0 ? -V : V);
        int32_t v = (av << 15) + A * A + 4;
        int k = (norm30(v) + 1) >> 1;
        uint32_t wv = (uint32_t)v << (2 * k), t = SQD[(wv >> 24) - 64];
        int32_t f = (int32_t)((wv << 8) >> 17);
        uint32_t S = ((t & 0xffff) + ((((int32_t)t >> 16) * f) >> 15)) >> k;
        int32_t d = (int32_t)S - A; if (d < 0) d = 0;
        int32_t u = (int32_t)(((uint32_t)d * (uint32_t)A) >> 14); if (V < 0) u = -u;
        int32_t afw = sat16i(pmin + u);
        pp = sat16i(2 * pmin + u);
        Q[2 * wq] = (int16_t)afw;
        r1 = (r1 + 1) & LM;
        int32_t e = sat16i(((int64_t)K[4] * Q[2 * r2] + (int64_t)K[5] * Q[2 * ((r2 + 1) & LM)]
                          + (int64_t)K[6] * Q[2 * ((r2 + 2) & LM)] + (int64_t)K[7] * Q[2 * ((r2 + 3) & LM)]) >> 14);
        r2 = (r2 + 1) & LM;
        int32_t ly = (int16_t)(lps >> 15);
        int64_t l = (int64_t)lps + (int64_t)K[12] * e - (int64_t)K[12] * ly;
        lps = l > 2147483647 ? 2147483647 : (l < -2147483647 - 1 ? -2147483647 - 1 : (int32_t)l);
        int32_t r = neg16(sat16i(((int16_t)(lps >> 15) * K[13]) >> 15));
        Q[2 * wq + 1] = (int16_t)r;
        io->g[n] = (int16_t)((e + r) >> 1);
        wq = (wq + 1) & LM;
    }
    io->x12 = pk(x1, x2); io->pp = pk(pp, ph); io->lps = lps;
}

/* radiation high-pass, body EQ, output low-pass and gain: g -> out. g[-1] holds the previous block's last input. */
static __attribute__((unused)) void radiate_c(rv_io_t *io)
{
    const int16_t *F = io->f, *F2 = io->f2, *K = io->k;
    int32_t hy = io->hy;
    int32_t ex1 = LO(io->ex), ex2 = HI(io->ex), ey1 = LO(io->ey), ey2 = HI(io->ey);
    int32_t lx1 = LO(io->lx), lx2 = HI(io->lx), ly1 = LO(io->ly), ly2 = HI(io->ly);
    for (int n = 0; n < OP1_BLOCK; n++) {
        int32_t hin = hy + (io->g[n] - io->g[n - 1]) * 512;
        hy = (int32_t)(int16_t)(hin >> 15) * F2[1] + (((hin & 0x7fff) * F2[1]) >> 15);
        int32_t y = sat16i(hy >> 9);
        int32_t yq = sat16i(((int64_t)K[14] * y + (int64_t)K[15] * ex1 + (int64_t)F[0] * ex2 + (int64_t)F[1] * ey1 + (int64_t)F[2] * ey2) >> 14);
        ex2 = ex1; ex1 = y; ey2 = ey1; ey1 = yq;
        int32_t yl = sat16i(((int64_t)F[4] * yq + (int64_t)F[5] * lx1 + (int64_t)F[6] * lx2 + (int64_t)F[7] * ly1 + (int64_t)F2[0] * ly2) >> 14);
        lx2 = lx1; lx1 = yq; ly2 = ly1; ly1 = yl;
        io->out[n] = (int16_t)sat16i((yl * F2[2]) >> 8);
    }
    io->hy = hy; io->ex = pk(ex1, ex2); io->ey = pk(ey1, ey2); io->lx = pk(lx1, lx2); io->ly = pk(ly1, ly2);
}

#if defined(__bfin__) && !defined(RV_NOASM)
#include "rv_asm.h"
#define BORE(io) bore_asm(io)
#define RADIATE(io) radiate_asm(io)
#else
#define BORE(io) bore_c(io)
#define RADIATE(io) radiate_c(io)
#endif

static void run_block(rv_state_t *s, int16_t *out, int32_t gstep, int16_t zeta)
{
    int16_t gb[OP1_BLOCK + 2];
    rv_io_t io;
    io.g = gb + 2; io.out = out; io.sqd = SQD - 64; io.base = &s->q[0][0];
    breath(s, io.g, s->gam, gstep);
    io.k[0] = REED_B1; io.k[1] = REED_A1; io.k[2] = REED_A2; io.k[3] = zeta;
    io.k[4] = (int16_t)s->lc[3]; io.k[5] = (int16_t)s->lc[2]; io.k[6] = (int16_t)s->lc[1]; io.k[7] = (int16_t)s->lc[0];
    io.k[8] = io.k[9] = io.k[10] = io.k[11] = 0;
    io.k[12] = (int16_t)s->lpc; io.k[13] = (int16_t)s->gq15; io.k[14] = EQ_C[0]; io.k[15] = EQ_C[1];
    io.f[0] = EQ_C[2]; io.f[1] = -EQ_C[3]; io.f[2] = -EQ_C[4]; io.f[3] = 0;
    io.f[4] = OLP_C[0]; io.f[5] = OLP_C[1]; io.f[6] = OLP_C[2]; io.f[7] = -OLP_C[3];
    io.f2[0] = -OLP_C[4]; io.f2[1] = RADHP_Q15; io.f2[2] = (int16_t)s->outg; io.f2[3] = 0;
    uint32_t w = s->w;
    io.ip[0] = &s->q[0][0];
    io.ip[1] = &s->q[(w - s->lbb) & LM][1];
    io.ip[2] = &s->q[(w + 1 - s->lbf_i - 2) & LM][0];
    io.ip[3] = &s->q[(w + 1) & LM][0];
    io.x12 = pk(s->x1, s->x2); io.pp = pk(s->pprev, 0); io.lps = s->lps;
    BORE(&io);
    s->w = (w + OP1_BLOCK) & LM;
    s->x1 = LO(io.x12); s->x2 = HI(io.x12); s->pprev = LO(io.pp); s->lps = io.lps;
    gb[1] = (int16_t)s->hx;
    io.hy = s->hy; io.ex = s->ex; io.ey = s->ey; io.lx = s->lx; io.ly = s->ly;
    RADIATE(&io);
    s->hx = io.g[OP1_BLOCK - 1]; s->hy = io.hy; s->ex = io.ex; s->ey = io.ey; s->lx = io.lx; s->ly = io.ly;
}

static void rv_render(void *self, int16_t *out, const int16_t *k)
{
    rv_voice_t *v = (rv_voice_t *)self;
    int vi = op1_voice_index(self); if (vi < 0 || vi >= OP1_VOICES) vi = 0;
    rv_state_t *s = &st[vi];
    rv_scr_tick[vi]++;
    if (v->sig != SIG) { rv_reset(s); v->sig = SIG; v->gate = 1; v->fresh = 1; v->tongued = 1; v->inc = 0; }
    uint32_t inc = (uint32_t)OP1_PITCH_INC[0];
    int32_t rt0 = s->rt, a0[2] = { s->age[0], s->age[1] };   /* the loop sounding (rt0 0: none yet) */
    if (inc != v->inc || k[0] != v->kn[0] || k[1] != v->kn[1] || k[2] != v->kn[2] || k[3] != v->kn[3]) setup(v, s, inc, k);
    rv_scr_note[vi] = (uint8_t)clampi((s->midi_q8 + 128) >> 8, 0, 127);

    /* dynamics: the stock envelope (velocity included, ~half scale at its peak) read doubled, x breath knob */
    int32_t env = OP1_VOICE_ENV[OP1_BLOCK - 1] * 2; if (env > 32767) env = 32767;
    rv_scr_att[vi] = (int16_t)clampi((int32_t)(((int64_t)env << 15) / op1_voice_vel_gain(self)), 0, 32767);
    int32_t dyn = (int32_t)(((int64_t)env * s->dyn_scale) >> 15); if (dyn > 40959) dyn = 40959;   /* <= 1.25 */
    if (v->fresh) {                               /* a tongued start: the same hold for every note */
        v->fresh = 0;
        s->tongue = v->tongued ? TONGUE_SMP : NO_TONGUE;
    }
    {   /* tuning: TUNE over midi (TUNE_M0 + i) and dynamics (j / 4), bilinear, cents Q4 */
        int32_t mq = s->midi_q8 - (TUNE_M0 << 8); mq = clampi(mq, 0, ((TUNE_NM - 1) << 8) - 1);
        int32_t dq = dyn >> 3; dq = clampi(dq, 0, ((TUNE_ND - 1) << 10) - 1);   /* Q10 in steps of 1/4 */
        int32_t mi = mq >> 8, mf = mq & 255, dj = dq >> 10, dfr = dq & 1023;
        int32_t t0 = TUNE[mi][dj] + (((TUNE[mi][dj + 1] - TUNE[mi][dj]) * dfr) >> 10);
        int32_t t1 = TUNE[mi + 1][dj] + (((TUNE[mi + 1][dj + 1] - TUNE[mi + 1][dj]) * dfr) >> 10);
        int32_t c = t0 + (((t1 - t0) * mf) >> 8);
        if (c - s->corr > 3 || s->corr - c > 3) set_delays(s, c);
    }
    if (rt0 && (s->rt - rt0 > 32768 || rt0 - s->rt > 32768)) warp(s, a0);   /* a slur: more than half a sample */
    int32_t di = dyn >> 10, df = dyn & 1023;      /* dyn^vcurve, Q14 */
    int32_t dp = DYN_POW[di] + (((DYN_POW[di < 40 ? di + 1 : 40] - DYN_POW[di]) * df) >> 10);
    int32_t tgt = s->gth + D_P_Q13 + (((D_F_Q13 - D_P_Q13) * dp) >> 14);
    if (!v->gate) {                               /* release: the player stops blowing as the envelope falls */
        if (!v->rel_init) { v->rel_e = env > 64 ? env : 64; v->rel_init = 1; }
        int32_t r = (int32_t)(((int64_t)env << 15) / v->rel_e); if (r > 32768) r = 32768;
        tgt = (tgt * r) >> 15;
    }
    /* tongued start: while the tongue holds the reed, the pressure stays below the reed's closing point (above it
     * the released reed would stay pinned shut), then the cap lifts over a fixed ramp: the same for every note */
    if (s->tongue > -OPEN_RAMP_SMP) {
        int32_t e = s->tongue > 0 ? 0 : (-s->tongue) & ~(OP1_BLOCK - 1);     /* samples since the release */
        int32_t cap = HOLD_P_Q13 + (int32_t)(((16383 - HOLD_P_Q13) * e) / OPEN_RAMP_SMP);
        if (tgt > cap) tgt = cap;
    }
    if (tgt < 0) tgt = 0;
    /* breath: follows the envelope (its attack is the note's) through a 2 ms smoother while held, two 50 ms
     * stages in the release; stepped per block, ramped per sample (Q29, coefs Q30) */
    int32_t tgt16 = tgt << 16, a1 = v->gate ? 0 : REL_B30, a2 = v->gate ? ATT2_B30 : REL_B30;
    int32_t g1n = a2 ? tgt16 + (int32_t)(((int64_t)(s->g1 - tgt16) * a2) >> 30) : tgt16;
    int32_t gamn = g1n + (int32_t)(((int64_t)(s->gam - g1n) * a1) >> 30);
    int32_t gstep = (gamn - s->gam) >> 7;
    s->g1 = g1n;

    /* tongue: reed held shut (whole blocks) while pressure builds. Its release sends a pressure pulse out of the
     * bell, a click, so the output fades in over TONGUE_FADE_SMP from there (before it the output is silent). */
    int16_t zeta = (int16_t)s->zeta;
    int32_t t0 = s->tongue;
    if (t0 > 0) zeta = 0;
    if (t0 > -(TONGUE_FADE_SMP + OP1_BLOCK) || t0 > -(OPEN_RAMP_SMP + OP1_BLOCK)) s->tongue -= OP1_BLOCK;
    run_block(s, out, gstep, zeta);
    if (t0 <= 0) {
        int32_t e0 = (-t0) & ~(OP1_BLOCK - 1);    /* samples since the release, at the start of this block */
        if (e0 < TONGUE_FADE_SMP)
            for (int n = 0; n < OP1_BLOCK && e0 + n < TONGUE_FADE_SMP; n++) {
                int32_t x = ((e0 + n) * FADE_RCP) >> 10;                 /* Q15 */
                x = (((x * x) >> 15) * (3 * 32768 - 2 * x)) >> 15;       /* smoothstep (x^2 (3 - 2x) <= 2^30) */
                out[n] = (int16_t)((out[n] * x) >> 15);
            }
    }
    s->gam = gamn;
}

const op1_engine_t ENGINE_reedophone = { rv_note_on, rv_render, rv_note_off,
                                         { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE } };
