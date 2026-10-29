/* arc: Duddell's singing arc (William Duddell, London 1899-1900). A carbon arc lamp shunted by an LC circuit
 * oscillates through the arc's negative resistance; Duddell played it from a keyboard. Each voice is a
 * Van der Pol oscillator  x'' - mu (1 - x^2) x' + w^2 x = 0  (a "lamp") plus a second, coupled lamp.
 *   page 1  blue  = current mu: clean sine -> reedy buzz -> screaming relaxation oscillation
 *           green = carbon: asymmetric electrode (even harmonics, a DC bias that the DC blocker removes)
 *           white = hiss: plasma noise injected into the oscillator (phase noise: pitch wanders at high values)
 *           red   = strike: pitch sag on attack (depth and 50 ms..1 s recovery) + an ignition noise burst; while a
 *                   key is held the lamp also sputters: random re-strikes (dip, crackle, short dropout), 0..6 / s
 *   page 2  blue  = 2nd lamp tuning ratio 1/2..2 with detents at 1/2 2/3 3/4 1 5/4 4/3 3/2 5/3 2 (word 5)
 *   (press) green = coupling: 0 = 2nd lamp off; weak = beats / locking, strong = chaos (word 6)
 *           white = brightness: key-tracked output tilt, dark .. bright (word 7; unset = neutral)
 *
 * DSP (integers only). The oscillator runs in its own time tau = w t, so the waveform does not change across the
 * keyboard: v' = eps (1 - x^2) v - x, x' = v, semi-implicit (symplectic) Euler with step h = G * theta per sample
 * (theta = 2 pi f / fs). The limit cycle period grows with eps (2 pi at eps = 0, ~12 at eps = 7) and the
 * discretisation shifts it again at high notes, so G comes from a 2-D table (eps knob x pitch, 1/8 octaves)
 * that gen_tables.py measured by running this very code (arc_calib.c): notes stay in tune at every setting.
 * The table is stored compressed (lossless, saves 3 KB of code space) and decoded into RAM once: by the screen's init
 * (GUI thread, normally before any note), else row by row on first use in render (bounded, once per boot).
 * Stability: x is clamped to +-3, v saturates at +-16; h*eps <= 0.3 (at high notes the lamp gets cleaner instead
 * of blowing up, which also keeps aliasing down) and the per-step damping factor is clamped at -0.75.
 * Cost: all products are 16 x 16 bit (one MAC on the Blackfin): x, v enter them as Q12 / Q11 views, h as a Q15
 * mantissa with one per-block shift per lamp. Pass 1 runs the lamps and writes the raw arc current into out[],
 * pass 2 shapes it (carbon, DC blocker, tilt, gain), so neither loop runs out of registers. On the Blackfin both
 * passes and the portrait points run in arc_dsp.S (hand-scheduled, bit-exact with the C loops, which the host builds and -DARC_C_REF keep).
 * Voice state: 2 x (x, v) + rng + DC + tilt + strike env + gate = 32 bytes.
 * Copyright (C) 2026 ultralazr. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include "op1_plugin.h"
#include "arc_tables.h"

typedef struct {
    uint32_t vtable;          /* +0x00 firmware */
    int32_t  x1, v1;          /* +0x04 main lamp: x Q29, v Q27 (saturating: |x| < 4, |v| < 16) */
    int32_t  x2, v2;          /* +0x0c second lamp */
    uint32_t rng;             /* +0x14 xorshift32 plasma noise */
    int32_t  dc;              /* +0x18 DC blocker state, Q20 */
    int32_t  lp;              /* +0x1c brightness tilt lowpass, Q20 */
    int16_t  env;             /* +0x20 strike envelope, Q15 */
    uint8_t  gate;            /* +0x22 key held (screen) */
    uint8_t  live;            /* +0x23 bit 0: voice has run (screen); bit 1: env is a sputter re-strike */
} arc_voice_t;
typedef char arc_voice_fits[sizeof(arc_voice_t) == OP1_VOICE_SIZE ? 1 : -1];

#define HE_MAX 9830           /* h * eps <= 0.3 (Q15): above that the period stops being monotonic in h */
#define A_MIN  (-24576)       /* damping per step >= -0.75: no sign flip of v, ever */
#define HMAX   29491          /* h <= 0.9 (symplectic Euler is stable below 2) */
#ifdef __bfin__
/* 16 x 16 -> 32 multiply as one MAC instruction (left to itself, gcc often sign-extends and uses the multi-cycle
 * 32-bit multiply), saturating add as one instruction */
static inline int32_t m16(int32_t a, int32_t b)
{ int32_t r; __asm__("%0 = %1.L * %2.L (IS);" : "=d"(r) : "d"(a), "d"(b)); return r; }
static inline int32_t m16h(int32_t a, int32_t b)
{ int32_t r; __asm__("%0 = %1.L * %2.H (IS);" : "=d"(r) : "d"(a), "d"(b)); return r; }
#define M16(a, b) m16((int32_t)(a), (int32_t)(b))
#define MH(a, b)  m16h((int32_t)(a), (int32_t)(b))         /* a.L * b.H: two constants share one register */
#define SADD(a, b) __builtin_bfin_add_fr1x32((a), (b))
#define SHL3(a)    __builtin_bfin_shl_fr1x32((a), 3)        /* saturating << 3 */
#else
#define M16(a, b) ((int32_t)(int16_t)(a) * (int16_t)(b))    /* 16 x 16 -> 32 (one MAC on the Blackfin) */
#define MH(a, b)  ((int32_t)(int16_t)(a) * (int16_t)((uint32_t)(b) >> 16))
static inline int32_t SADD(int32_t a, int32_t b)
{
    int32_t s = (int32_t)((uint32_t)a + (uint32_t)b);
    return ((a ^ s) & (b ^ s)) < 0 ? (a < 0 ? (int32_t)0x80000000 : 0x7fffffff) : s;
}
static inline int32_t SHL3(int32_t a) { return a > 0x0FFFFFFF ? 0x7fffffff : (a < -0x10000000 ? (int32_t)0x80000000 : a * 8); }
#endif

/* ---- shared with the screen (written by render, read by the GUI thread) ---- */
#define ARC_TN 256                                  /* phase portrait ring buffer (decimated) */
int16_t arc_trace[ARC_TN][4] __attribute__((aligned(8)));   /* x1, v1, x2, v2: Q12, v scaled ~ 1 / (1 + 0.6 eps);
                                                               aligned: arc_dsp.S writes a point as two words */
volatile uint16_t arc_tpos;                         /* next write index */
volatile uint8_t arc_tvoice = 0xff;                 /* voice that writes the trace (most recent note) */
volatile int16_t arc_level[OP1_VOICES];             /* output peak per voice (arc flicker) */

#ifdef ARC_CALIB                                    /* gen_tables.py: fixed G, record x1 per sample */
uint16_t arc_cal_gp; int32_t *arc_cal_rec;
#endif

#ifdef __bfin__
static int msb32(uint32_t v)                        /* index of the top set bit (v > 0): SIGNBITS */
{ return (int32_t)v < 0 ? 31 : 30 - __builtin_bfin_norm_fr1x32((int32_t)v); }
#else
static int msb32(uint32_t v) { int e = 0; while (v > 1) { v >>= 1; e++; } return e; }   /* <= 31 steps */
#endif
static uint32_t isqrt32(uint32_t v)                 /* floor(sqrt(v)), 16 steps (per block only) */
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}

/* pitch: inc (2^32 = 1 cycle/sample) = m * 2^(e - 15), m in [2^15, 2^16); times ratio (Q14, 0.5..2) */
typedef struct { uint32_t m; int e; } pitch_t;
static pitch_t pitch_of(uint32_t inc, int32_t r14)
{
    pitch_t p;
    if (inc < (1u << 18)) inc = 1u << 18;           /* < 2.6 Hz */
    p.e = msb32(inc); p.m = inc >> (p.e - 15);
    p.m = (p.m * (uint32_t)r14) >> 14;
    if (p.m >= 1u << 16) { p.m >>= 1; p.e++; } else if (p.m < 1u << 15) { p.m <<= 1; p.e--; }
    return p;
}

/* ARC_GPZ (arc_tables.h, gen_tables.py: encode) -> arc_gp rows < n. Row k needs row k - 1 only, rows are decoded in
 * order and the decode is idempotent (same values written), so the GUI (screen init) and render (interrupt) may both
 * run it, even interleaved. Worst case in render: all 33 rows once (~3200 short steps), normally none. */
static uint16_t arc_gp[ARC_KN][ARC_PN];
static volatile uint8_t arc_gp_n;                   /* rows decoded */
__attribute__((noinline)) static void gp_decode(int n)
{
    for (int k = arc_gp_n; k < n; k++) {
        const int8_t *z = ARC_GPZ + ARC_GPROW[k];
        const uint16_t *up = arc_gp[k > 0 ? k - 1 : 0];
        int32_t g = 0, ul = 0, n = *z++;                /* n stored nodes, then the tail repeats node n - 1 */
        for (int p = 0; p < ARC_PN; p++) {
            if (p < n) {
                int32_t d = *z++, u = k > 0 ? up[p] : 0;
                if (d == -128) { d = (uint8_t)z[0] + z[1] * 256; z += 2; }   /* escape: int16, low byte first */
                g += d + u - ul; ul = u;
            }
            arc_gp[k][p] = (uint16_t)g;
        }
        if (arc_gp_n < k + 1) arc_gp_n = (uint8_t)(k + 1);
    }
}

/* G * 2 pi (Q12) for knob row (ki, kf Q15) at pitch p: table nodes every 1/8 octave from inc 2^19 (5 Hz) */
static uint32_t gp_of(int ki, int32_t kf, pitch_t p)
{
#ifdef ARC_CALIB
    if (arc_cal_gp) return arc_cal_gp;
#endif
    int o = p.e - 19, pi; int32_t pf;
    if (o < 0) { pi = 0; pf = 0; }
    else if (o >= ARC_OCT) { pi = ARC_PN - 2; pf = 32767; }
    else { pi = o * 8 + (int)((p.m >> 12) & 7); pf = (int32_t)(p.m & 4095) << 3; }
    if (ki + 2 > arc_gp_n) gp_decode(ki + 2);       /* table rows ki, ki + 1 (normally decoded by the screen) */
    const uint16_t *a = arc_gp[ki], *b = arc_gp[ki + 1];
    int32_t g0 = a[pi] + ((((int32_t)a[pi + 1] - a[pi]) * pf) >> 15);
    int32_t g1 = b[pi] + ((((int32_t)b[pi + 1] - b[pi]) * pf) >> 15);
    return (uint32_t)(g0 + (((g1 - g0) * kf) >> 15));
}

/* per-block lamp coefficients. h = (m * Gp) * 2^(e - 59) is kept as a Q15 mantissa and one shift: h = hm >> sh.
 * Every lamp product (h v -> x, h x -> v, coupling -> v) uses that same shift. x is Q29 and v Q27 and both
 * saturate (|x| < 4: the limit cycle is 2, a wall for noise-driven excursions; |v| < 16). */
typedef struct { int32_t h, sh, he, hk, sg, g; } lamp_t;
static int32_t shl(uint32_t v, int k) { return (int32_t)(k >= 0 ? v << k : (k > -32 ? v >> -k : 0)); }
static void lamp_setup(lamp_t *L, uint32_t inc, int32_t r14, int oct, int32_t sag, int ki, int32_t kf, int32_t eps12,
                       int32_t k12, int32_t sigc)
{
    pitch_t p = pitch_of(inc, r14);
    p.e += oct;                                     /* 2nd lamp runs at half rate: twice the step */
    uint32_t P = ((p.m * gp_of(ki, kf, p)) >> 15) * (uint32_t)sag;
    if (!P) P = 1;
    int s = 58 - (msb32(P) + p.e), h;               /* hm in [2^14, 2^15) */
    if (s < 0) { s = 0; h = HMAX; }                 /* h >= 1: clamp (very high notes) */
    else {
        if (s > 24) s = 24;
        h = shl(P, p.e - 44 + s);
        if (s == 0 && h > HMAX) h = HMAX;
    }
    L->h = h; L->sh = s;
    int32_t he = ((h * eps12) >> 12) >> s;
    L->he = he > HE_MAX ? HE_MAX : he;
    L->hk = (h * k12) >> 10;                        /* 4 h k: k <= 0.25 (Q12 <= 1024), dv is Q10 */
    /* noise per step ~ sqrt(h): the phase wander per cycle is the same on every key */
    uint32_t rt = isqrt32((uint32_t)h << 15) >> (s >> 1);
    if (s & 1) rt = (rt * 23170u) >> 15;
    L->sg = (int32_t)(((uint32_t)sigc * rt) >> 18); /* Q12 */
    int32_t g = s >= 1 ? h >> (s - 1) : 2 * h;      /* tilt lowpass g = 2h: corner ~2x the fundamental */
    L->g = g > HMAX ? HMAX : g;
}

/* one semi-implicit Euler step of a lamp. xs = x as Q12, nz = noise (int16), cpl = coupling (Q27 before >> sh),
 * K = h | he << 16, N = sg | hk << 16 (packed: the dual loop would run out of registers otherwise) */
#define LAMP(x, v, xs, K, sh, N, nz, cpl) do {                                                 \
        int32_t f_ = 2048 - (M16(xs, xs) >> 13);          /* 1 - x^2 in Q11, > -15 as |x| < 4 */ \
        int32_t a_ = (MH(f_, K) + 1024) >> 11;            /* h eps (1 - x^2), Q15, rounded */  \
        if (a_ < A_MIN) a_ = A_MIN;                                                            \
        (v) = SADD((v), M16((v) >> 16, a_) * 2);                                               \
        (v) = SADD((v), (((cpl) - M16(xs, K)) >> (sh)) + M16(nz, N));                          \
        (x) = SADD((x), SHL3(M16((v) >> 16, K) >> (sh)));    /* Q26 step -> Q29, saturating */ \
    } while (0)

/* page-2 ratio (word 5): breakpoints (word, ratio Q14), flat around the simple ratios; 0 = unset = 1:1 */
static int32_t ratio_of(int32_t w)
{
    if (w <= 0) return 16384;
    for (int i = 1; i < ARC_RN; i++)
        if (w <= ARC_RW[i]) {
            int32_t w0 = ARC_RW[i - 1], w1 = ARC_RW[i], r0 = ARC_RR[i - 1], r1 = ARC_RR[i];
            return w1 == w0 ? r1 : r0 + (r1 - r0) * (w - w0) / (w1 - w0);   /* per block */
        }
    return ARC_RR[ARC_RN - 1];
}

#if defined(__bfin__) && !defined(ARC_C_REF)
/* arc_dsp.S: pass 2 scheduled by hand (bit-exact with the C loop below, which the host builds keep). Ring words:
 * -8192, 32767, 16383, -16383, btq, 32767, -32767, dg. The audio thread only, one voice at a time. */
typedef struct { int32_t dc, lp, ga, k, ring[8]; } ArcShape;
void arc_shape(int16_t *out, ArcShape *s);
static ArcShape arc_sh = { 0, 0, 0, 0, { -8192, 32767, 16383, -16383, 0, 32767, -32767, 0 } };
/* arc_dsp.S: pass 1 (the lamps), the same arithmetic as the loops in arc_render. k: the state in / out, xs / xt of the
 * last step for the portrait, scratch, and the per-block ring words (filled by arc_render, see arc_dsp.S) */
typedef struct {
    int32_t x1, v1, x2, v2; uint32_t r; int32_t s[5], n, m;
    int16_t *tp, *tb; int32_t tl;                   /* the portrait ring: next point, start, bytes */
    int32_t ring3[7], ring1[6], ring2[19];
} ArcRun;
void arc_lamp1(ArcRun *k, int16_t *out);            /* one lamp: m chunks of n samples */
void arc_lamp2(ArcRun *k, int16_t *out);            /* both lamps: m chunks of n sample pairs */
static int16_t arc_tdummy[4] __attribute__((aligned(8)));   /* an untraced voice's portrait points */
static ArcRun arc_run = { .ring3 = { 12288, -12288, 10923, 0, 0, 32767, -32767 } };
#endif

static inline int32_t clampk(int32_t k) { return k < 0 ? 0 : (k > 32767 ? 32767 : k); }

/* portrait ordinate (trace points only): (v - eps (x - x^3/3)) / (1 + eps/3). Small eps: v, the circle of a sine.
 * Large eps: -3 times the Lienard variable, so the relaxation cycle shows as the squarish loop of two slow cubic
 * branches joined by fast jumps. Q12 in/out, x clipped to +-3 for the cubic. The Blackfin build runs the same in
 * arc_dsp.S (LIEN). */
#if !defined(__bfin__) || defined(ARC_C_REF)
static int16_t lienard(int32_t xs, int32_t v, int32_t eps12, int32_t vmul)
{
    if (xs > 12288) xs = 12288; else if (xs < -12288) xs = -12288;
    int32_t x3 = (((xs * xs) >> 12) * xs) >> 12, F = xs - ((x3 * 10923) >> 15);
    int32_t y = (((v >> 15) - ((eps12 * F) >> 12)) * vmul) >> 12;
    return (int16_t)(y > 32767 ? 32767 : (y < -32767 ? -32767 : y));
}
#endif

static void arc_note_on(void *self, const int16_t *params)
{
    arc_voice_t *v = (arc_voice_t *)self;
    v->x1 = 0; v->v1 = 2 << 27;                     /* on the sine limit cycle, output starts at 0: no click */
    v->x2 = 1 << 30; v->v2 = 0;                     /* 2nd lamp a quarter cycle away: it pulls into phase */
    if (!v->rng) v->rng = 0x9E3779B9u ^ (uint32_t)(uintptr_t)self;
    v->dc = (M16(-8192, clampk(params[1])) >> 16) * 256;   /* carbon's DC at x = 0: output starts at 0 */
    v->lp = 0;
    v->env = 32767; v->gate = 1; v->live = 1;           /* ignition (not a sputter) */
    arc_tvoice = (uint8_t)op1_voice_index(self);
}

static void arc_note_off(void *self) { ((arc_voice_t *)self)->gate = 0; }

static void arc_render(void *self, int16_t *out, const int16_t *params)
{
    arc_voice_t *v = (arc_voice_t *)self;
    const int32_t *inc = OP1_PITCH_INC;
    int32_t mu = clampk(params[0]), carbon = clampk(params[1]), hiss = clampk(params[2]), strike = clampk(params[3]);
    int32_t wr = clampk(params[5]), wc = clampk(params[6]), wb = clampk(params[7]);
    int vi = op1_voice_index(self);

    /* blue: eps = 0.04 .. 7 (quadratic: most of the travel is in the musical sine..reed range) */
    int32_t u2 = (mu * mu) >> 15;
    int32_t eps12 = 164 + ((u2 * 28508) >> 15);
    int ki = (int)(mu >> 10); int32_t kf = (mu & 1023) << 5;
    /* red: strike sag depth up to 0.5 (-12 semitones; 1 - (1 - red)^2, so the lower half is already clearly
     * audible), recovery 50 ms .. 1 s (per-block decay; the pitch steps per block are phase-continuous).
     * Sputter: while the key is held the lamp re-strikes at random, 0 .. 6 per second (red^2): the envelope jumps
     * to 35..90 % and decays 4x faster than the ignition, with a level dip (a short dropout) on top. */
    int32_t sq = 32767 - strike, env0 = v->env, depth = (32767 - ((sq * sq) >> 15)) >> 1;
    int32_t dec = 1927 - ((strike * 1832) >> 15);
    int sput = v->live & 2;
    if (sput) dec <<= 2;
    int32_t env1 = env0 - ((env0 * dec) >> 15); if (env1 < 16) env1 = 0, sput = 0;
    if (v->gate && strike) {
        uint32_t q = v->rng; q ^= q << 13; q ^= q >> 17; q ^= q << 5; v->rng = q;
        if ((q & 0xffff) < (uint32_t)((((strike * strike) >> 15) * 1141) >> 15)) {   /* 6 / s / 344.5 blocks, Q16 */
            int32_t amp = 11469 + (int32_t)(((q >> 16) * 18022) >> 16);              /* 0.35 .. 0.9 */
            if (amp > env1) env1 = amp, sput = 2;
        }
    }
    v->env = (int16_t)env1; v->live = (uint8_t)((v->live & 1) | sput);
    int32_t sag = 32767 - ((depth * ((env0 + env1) >> 1)) >> 15);
    /* sputter dropout: level dip up to 85 % with the re-strike envelope, ramped across the block (no click) */
    int32_t dip0 = sput ? 32767 - ((env0 * 27853) >> 15) : 32767, dip1 = sput ? 32767 - ((env1 * 27853) >> 15) : 32767;
    /* white: plasma noise sigma (continuous-time, Q15) up to 0.45, plus the strike's ignition burst */
    int32_t h2k = (hiss * hiss) >> 15;
    int32_t sigc = ((h2k * 14745) >> 15) + ((((depth * env1) >> 15) * 3) >> 1);
    if (sigc > 32767) sigc = 32767;
    /* page 2 */
    int32_t r14 = ratio_of(wr);
    /* coupling: dissipative, k (v2 - v1) (the lamps share the arc current). Unlike a spring coupling k (x2 - x1) it
     * adds no stiffness, so the played note stays in tune at every ratio, and it pulls the pair into phase (a
     * spring coupling locks relaxation oscillators in anti-phase: the mix cancels and jumps an octave). It damps
     * an unlocked lamp by k, so k <= eps / 4 keeps both singing (amplitude >= 1.7). k = 0..0.25, quadratic. */
    int32_t k12 = ((wc * wc) >> 15) >> 5;
    if (k12 > eps12 >> 2) k12 = eps12 >> 2;
    int32_t mix = wc << 2; if (mix > 16384) mix = 16384;   /* 2nd lamp level: equal mix from 1/8 knob */
    int dual = wc > 0;
    int32_t wbb = wb ? wb : 8192;                   /* unset word = neutral brightness */
    int32_t bt = wbb <= 8192 ? 1024 + ((wbb * 3072) >> 13) : 4096 + (((wbb - 8192) * 6144) / 24575);

    uint32_t im = (uint32_t)inc[OP1_BLOCK / 2];     /* pitch per block (bends / LFO are smooth) */
    lamp_t A, B;
    lamp_setup(&A, im, 16384, 0, sag, ki, kf, eps12, k12, sigc);
    if (dual) lamp_setup(&B, im, r14, 1, sag, ki, kf, eps12, k12, sigc); else B = A;

    /* level: the table's loudness correction across mu, velocity, and headroom for 2 lamps / bright / carbon / hiss */
    int32_t gn = ARC_GAIN[ki] + (((ARC_GAIN[ki + 1] - ARC_GAIN[ki]) * kf) >> 15);              /* Q12 */
    int32_t gv = (op1_voice_vel_gain(self) * gn) >> 12;
    gv = (gv * (4096 + (mix >> 4))) / (4096 + ((carbon * 1229) >> 15) + ((h2k * 1638) >> 15) + (bt > 4096 ? bt - 4096 : 0));
    if (gv > 32767) gv = 32767;                     /* ^ per block (division) */

    /* trace: the most recent voice writes a point every `chunk` samples (64..128 points per cycle) */
    int traced = vi == arc_tvoice, chunk = OP1_BLOCK;
    if (traced) {
        int k = 32 - msb32(im | 1) - 6;
        chunk = k < 1 ? 2 : (k > 7 ? OP1_BLOCK : 1 << k);
    }
    /* portrait with the effective eps (h eps is capped at 0.3 at high notes); per-block divisions, traced voice only */
    int32_t epsd = 0, vmul = 0;
    if (traced) {
        epsd = A.he >= HE_MAX && (A.h >> A.sh) > 0 ? (HE_MAX * 4096) / (A.h >> A.sh) : eps12;
        if (epsd > eps12) epsd = eps12;
        vmul = (4096 * 4096) / (4096 + epsd / 3);                         /* portrait scale, see lienard() */
    }

    /* pass 1: the lamps -> raw arc current y (Q12) + plasma hiss riding on |x| (no onset click) into out[] */
    int32_t x1 = v->x1, v1 = v->v1, x2 = v->x2, v2 = v->v2;
    uint32_t r = v->rng;
#define PACK(lo, hi) ((int32_t)(((uint32_t)(hi) << 16) | ((uint32_t)(lo) & 0xffff)))
    const int32_t O = PACK(h2k >> 2, mix);         /* hiss level | 2nd lamp mix */
    const int sh1 = A.sh, sh2 = B.sh;
#if defined(__bfin__) && !defined(ARC_C_REF)
    {   /* ring words: CLK = 2^29 + 2048 he + 1024, E = he << 16, AMINK = 2^18 + A_MIN, G = sg | -h << 16,
         * H = -sh | h << 16 */
        ArcRun *k = &arc_run;
        const int32_t amk = (1 << 18) + A_MIN, clk1 = (1 << 29) + 2048 * A.he + 1024, e1 = PACK(-sh1, A.he),
                      g1 = PACK(A.sg, -A.h), h1 = PACK(-sh1, A.h);
        const int m = OP1_BLOCK >> msb32((uint32_t)chunk);
        k->x1 = x1; k->v1 = v1; k->x2 = x2; k->v2 = v2; k->r = r; k->m = m;
        if (traced) {
            k->tp = arc_trace[arc_tpos]; k->tb = arc_trace[0]; k->tl = (int32_t)sizeof arc_trace;
            k->ring3[3] = epsd; k->ring3[4] = vmul;
        } else {
            k->tp = k->tb = arc_tdummy; k->tl = (int32_t)sizeof arc_tdummy;
        }
        if (!dual) {
            int32_t *w = k->ring1;
            w[0] = O; w[1] = clk1; w[2] = e1; w[3] = amk; w[4] = g1; w[5] = h1;
            k->n = chunk;
            arc_lamp1(k, out);
        } else {
            int32_t *w = k->ring2;
            w[0] = PACK(-B.hk, 0); w[1] = (1 << 29) + 2048 * B.he + 1024; w[2] = PACK(-sh2, B.he); w[3] = amk;
            w[4] = PACK(B.sg, -B.h); w[5] = PACK(-sh2, B.h); w[6] = PACK(A.hk, 0);
            w[7] = w[14] = clk1; w[8] = w[15] = e1; w[9] = w[16] = amk; w[10] = w[17] = g1; w[11] = w[18] = h1;
            w[12] = O; w[13] = (int32_t)0xFFFEFFFE;
            k->n = chunk >> 1;
            arc_lamp2(k, out);
        }
        if (traced) arc_tpos = (uint16_t)((arc_tpos + m) & (ARC_TN - 1));
        x1 = k->x1; v1 = k->v1; x2 = k->x2; v2 = k->v2; r = k->r;
    }
#else
    const int32_t K1 = PACK(A.h, A.he), N1 = PACK(A.sg, A.hk), K2 = PACK(B.h, B.he), N2 = PACK(B.sg, B.hk);
    for (int16_t *o = out; o < out + OP1_BLOCK; ) {
        int32_t xs = 0, xt = 0;
        int16_t *e = o + chunk;
        if (!dual)
            for (; o < e; o++) {
                r ^= r << 13; r ^= r >> 17; r ^= r << 5;
                xs = x1 >> 17;
                LAMP(x1, v1, xs, K1, sh1, N1, r >> 16, 0);
#ifdef ARC_CALIB
                if (arc_cal_rec) *arc_cal_rec++ = x1;
#endif
                *o = (int16_t)(xs + (M16(M16(r, O) >> 15, xs < 0 ? -xs : xs) >> 13));
            }
        else
            for (; o < e; o += 2) {                    /* 2nd lamp at half rate (CPU), linearly interpolated */
                r ^= r << 13; r ^= r >> 17; r ^= r << 5;
                int32_t xp = x2 >> 17, dv = (v2 >> 17) - (v1 >> 17);   /* dv: Q10 */
                xt = xp;
                LAMP(x2, v2, xt, K2, sh2, N2, r >> 8, -MH(dv, N2));
                xt = x2 >> 17; xp = (xp + xt) >> 1;
                xs = x1 >> 17; dv = MH((v2 >> 17) - (v1 >> 17), N1);   /* held for the pair */
                LAMP(x1, v1, xs, K1, sh1, N1, r >> 16, dv);
                o[0] = (int16_t)(xs + (MH((xp - xs) >> 1, O) >> 14) + (M16(M16(r, O) >> 15, xs < 0 ? -xs : xs) >> 13));
                xs = x1 >> 17;
                LAMP(x1, v1, xs, K1, sh1, N1, r, dv);
                o[1] = (int16_t)(xs + (MH((xt - xs) >> 1, O) >> 14) + (M16(M16(r >> 16, O) >> 15, xs < 0 ? -xs : xs) >> 13));
            }
        if (traced) {
            int t = arc_tpos;
            arc_trace[t][0] = (int16_t)xs; arc_trace[t][1] = lienard(xs, v1, epsd, vmul);
            arc_trace[t][2] = (int16_t)(dual ? xt : xs);
            arc_trace[t][3] = dual ? lienard(xt, v2, epsd, vmul) : arc_trace[t][1];
            arc_tpos = (uint16_t)((t + 1) & (ARC_TN - 1));
        }
    }
#endif
    v->x1 = x1; v->v1 = v1; v->x2 = x2; v->v2 = v2; v->rng = r;

    /* pass 2: carbon (y += carbon/2 (y^2 - 2): a 2nd harmonic for a sine), DC blocker ~7 Hz, key-tracked tilt
     * (one-pole at ~2x the fundamental: out = lp + bt (y - lp)), gain */
    int32_t dc = v->dc, lp = v->lp, pk = 0;
    const int16_t cb = (int16_t)carbon, g = (int16_t)A.g, btq = (int16_t)bt;
    int32_t ga = ((gv * dip0) >> 15) << 16, dg = ((((gv * dip1) >> 15) << 16) - ga) >> 7;   /* gain ramp, Q16 */
#if defined(__bfin__) && !defined(ARC_C_REF)
    arc_sh.dc = dc; arc_sh.lp = lp; arc_sh.ga = ga;
    arc_sh.k = (int32_t)(((uint32_t)(uint16_t)g << 16) | (uint16_t)cb); arc_sh.ring[4] = btq; arc_sh.ring[7] = dg;
    arc_shape(out, &arc_sh);
    dc = arc_sh.dc; lp = arc_sh.lp;
#else
    for (int n = 0; n < OP1_BLOCK; n++) {
        const int16_t gq = (int16_t)(ga >> 16); ga += dg;
        int32_t y = out[n];
        int32_t q = (M16(y, y) >> 12) - 8192; if (q > 32767) q = 32767;
        y += M16(q, cb) >> 16;
        dc += (y * 256 - dc) >> 10; y -= dc >> 8;
        if (y > 16383) y = 16383; else if (y < -16383) y = -16383;
        lp += M16(y - (lp >> 8), g) >> 7;
        int32_t l = lp >> 8, o = l + (M16(y - l, btq) >> 12);
        if (o > 32767) o = 32767; else if (o < -32767) o = -32767;
        out[n] = (int16_t)(SHL3(M16(o, gq)) >> 16);   /* = op1_sat16(o * gq >> 13), branch-free */
    }
#endif
    v->dc = dc; v->lp = lp;
    for (int n = 0; n < OP1_BLOCK; n += 8) { int32_t a = out[n] < 0 ? -out[n] : out[n]; if (a > pk) pk = a; }
    arc_level[vi] = (int16_t)pk;
}

const op1_engine_t ENGINE_arc = { arc_note_on, arc_render, arc_note_off,
                                  { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_NONE } };

/* ======================= screen (GUI thread) =======================
 * duddell.svg (gen_svg.py): lamp-holder clamps and four dials. Drawn here, back to front:
 *   - the phase portrait (x vs v) of the most recent voice from arc_trace, a blue comet trail (older = dimmer):
 *     a circle for a clean sine, a squarish relaxation loop as blue mu rises; hiss makes it fuzzy;
 *   - the 2nd lamp's portrait in green while coupling is on, and its ratio top right (stroke font);
 *   - the two carbons (they hide the loop where it passes behind them); the positive (left) tip burns a crater
 *     and both tips glow green with carbon;
 *   - the arc between the tips: size and brightness follow the output level, white jitter follows hiss;
 *   - red sparks on a strike (count / reach = red knob x the strike envelope).
 * Dials show page 1, or the page-2 value with a ring while that knob is on page 2. */
#include "seq/seq_text.h"
#include "arc_geom.h"
#pragma GCC push_options
#pragma GCC optimize("Os")                             /* GUI code: size, not speed (code space is short) */

static void *bars[4];
static int32_t lvl;                                  /* arc brightness, Q15: fast attack, slow fade */
static uint32_t srng = 0x2545F491u;
static const uint8_t COL_BLUE[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, COL_GREEN[4] = { 0x00, 0xED, 0x95, 0xFF },
                     COL_WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, COL_RED[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     COL_ROD[4] = { 0xDF, 0xD9, 0xFF, 0xFF }, COL_BLACK[4] = { 0, 0, 0, 0xFF };
static const uint8_t *const COL_K[4] = { COL_BLUE, COL_GREEN, COL_WHITE, COL_RED };

static inline int32_t tsin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }   /* table[k] = sin(2 pi k / 8192) */
static inline int32_t tcos(int32_t a) { return tsin(a + 2048); }
static int32_t rnd(int32_t n)                        /* 0 .. n-1, n <= 65536 */
{ srng ^= srng << 13; srng ^= srng >> 17; srng ^= srng << 5; return (int32_t)(((srng >> 16) * (uint32_t)n) >> 16); }
#define ln sq_line                                    /* seq/seq_text.h: one raster line */
static void shade(uint8_t *o, const uint8_t *c, int32_t l)          /* l = 0..256 */
{ if (l > 256) l = 256; if (l < 0) l = 0; for (int i = 0; i < 3; i++) o[i] = (uint8_t)((c[i] * l) >> 8); o[3] = 0xFF; }
static void ring(void *r, int32_t cx, int32_t cy, int32_t rad, int32_t w, const uint8_t *c)   /* Q10 */
{
    int32_t px = cx + rad, py = cy;
    for (int a = 512; a <= 8192; a += 512) {
        int32_t qx = cx + ((rad * tcos(a)) >> 15), qy = cy + ((rad * tsin(a)) >> 15);
        ln(r, px, py, qx, qy, w, c); px = qx; py = qy;
    }
}

/* portrait point: x (Q12, 2.0 = 8192) -> +-92 px around the gap, v (display-scaled Q12) -> +-40 px, clipped */
static int32_t ptx(int32_t x)
{ int32_t p = OP1_Q10(AG_CX) + ((x * 23) >> 1); return p < OP1_Q10(3) ? OP1_Q10(3) : (p > OP1_Q10(317) ? OP1_Q10(317) : p); }
static int32_t pty(int32_t v)
{ int32_t p = OP1_Q10(AG_CY) - v * 5; return p < OP1_Q10(3) ? OP1_Q10(3) : (p > OP1_Q10(120) ? OP1_Q10(120) : p); }

static void portrait(void *r, int k, int n, int32_t bright, const uint8_t *col)   /* last n points of lamp k */
{
    int tp = arc_tpos;
    const int16_t *q = arc_trace[(tp - n) & (ARC_TN - 1)];
    int32_t px = ptx(q[k]), py = pty(q[k + 1]);
    uint8_t c[4] = { 0, 0, 0, 0xFF };
    for (int i = n - 1; i >= 1; i--) {
        q = arc_trace[(tp - i) & (ARC_TN - 1)];
        int32_t qx = ptx(q[k]), qy = pty(q[k + 1]);
        if (((n - i) & 15) == 1) shade(c, col, bright * (n - i) / n);   /* comet: fades in 16-point steps */
        ln(r, px, py, qx, qy, 0x500, c);
        px = qx; py = qy;
    }
}

/* one carbon from its holder end xo to its tip xt (dir +1: tip to the right), crater = flat tip half-height */
static void carbon(void *r, int32_t xo, int32_t xt, int dir, int32_t crater, int32_t glow)
{
    int32_t y = OP1_Q10(AG_CY), hq = OP1_Q10(AG_ROD_H), xc = xt - dir * OP1_Q10(7);   /* cone starts at xc */
    ln(r, xo, y, xc, y, 2 * hq + OP1_Q10(1), COL_BLACK);                      /* hide the loop behind the rod */
    ln(r, xc, y, xt, y, hq + crater + OP1_Q10(1), COL_BLACK);
    ln(r, xo, y - hq, xc, y - hq, 0x600, COL_ROD); ln(r, xo, y + hq, xc, y + hq, 0x600, COL_ROD);
    uint8_t c[4];                                                              /* hot tip: rod colour -> green */
    for (int i = 0; i < 3; i++) c[i] = (uint8_t)((COL_ROD[i] * (256 - glow) + COL_GREEN[i] * glow) >> 8);
    c[3] = 0xFF;
    ln(r, xc, y - hq, xt, y - crater, 0x600, c); ln(r, xc, y + hq, xt, y + crater, 0x600, c);
    if (crater > 0x200) ln(r, xt, y - crater, xt, y + crater, 0x600, c);
    if (glow > 24) {                                                           /* glowing core inside the cone */
        shade(c, COL_GREEN, glow);
        ln(r, xc + dir * OP1_Q10(1), y, xt - dir * OP1_Q10(1), y, OP1_Q10(2), c);
    }
}

static void arc_screen_init(void *scr)
{
    static const char *ids[4] = { "bar0", "bar1", "bar2", "bar3" };
    for (int i = 0; i < 4; i++) bars[i] = op1_find_node(scr, ids[i]);
    gp_decode(ARC_KN);                              /* tuning table into RAM (else render does it on first use) */
}

static void ratio_text(char *s, int32_t r14)        /* "3:2" on a detent, else "1.26" */
{
    static const struct { uint16_t q; char t[4]; } det[] = { { 8192, "1:2" }, { 10923, "2:3" }, { 12288, "3:4" },
        { 16384, "1:1" }, { 20480, "5:4" }, { 21845, "4:3" }, { 24576, "3:2" }, { 27307, "5:3" }, { 32768, "2:1" } };
    for (unsigned i = 0; i < sizeof det / sizeof det[0]; i++)
        if (det[i].q == r14) { for (int j = 0; j < 4; j++) s[j] = det[i].t[j]; return; }
    int32_t c = (r14 * 100 + 8192) >> 14;           /* hundredths */
    s[0] = (char)('0' + c / 100); s[1] = '.'; s[2] = (char)('0' + c / 10 % 10); s[3] = (char)('0' + c % 10); s[4] = 0;
}

static void arc_screen_update(void *scr, int dt)
{
    int slot = OP1_SCREEN_SLOT & 7;
    int16_t k[4];
    sq_synth_knobs_live(slot, k);
    const int16_t *w = op1_synth_knobs(slot);
    int32_t wr = clampk(w[5]), wc = clampk(w[6]), wb = clampk(w[7]);

    /* level: loudest voice since the last frame (render writes, we consume: silent voices read 0) */
    int32_t tgt = 0;
    for (int i = 0; i < OP1_VOICES; i++) { int32_t a = arc_level[i]; if (a > tgt) tgt = a; arc_level[i] = 0; }
    tgt = tgt > 22795 ? 32767 : (tgt * 23) >> 4;    /* a -5 dBFS note = full arc */
    lvl = tgt > lvl ? lvl + ((tgt - lvl) >> 1) : lvl - (lvl >> 3) - 16;
    if (lvl < 0) lvl = 0;
    int32_t hiss = clampk(k[2]);
    int32_t flick = lvl - (int32_t)(((uint32_t)rnd(lvl + 1) * (uint32_t)(4096 + hiss)) >> 15);

    for (int i = 0; i < 4; i++) if (bars[i]) {      /* dials: -135..+135 degrees; page 2 shows the paged word */
        int32_t val = k[i];
        if (i < 3 && op1_eng_page[i]) val = i == 0 ? (wr ? wr : 8192) : (i == 1 ? wc : (wb ? wb : 8192));
        int32_t a = ((val * 6144) >> 15) - 3072;
        sq_node_rotate(bars[i], OP1_Q10(ag_dial_x[i]), OP1_Q10(AG_DIAL_Y), tsin(a), tcos(a));
    }
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    uint8_t c[4];

    /* portraits: brightness 30 % idle .. 100 % with the arc */
    int32_t br = 76 + ((lvl * 180) >> 15);
    if (wc > 0) portrait(r, 2, 128, (br * 3) >> 2, COL_GREEN);
    portrait(r, 0, ARC_TN, br, COL_BLUE);

    /* carbons: crater on the positive tip with carbon, both tips glow green with carbon x level */
    int32_t carb = clampk(k[1]), glow = ((carb >> 7) * (96 + (lvl >> 8))) >> 8;   /* 0..256 */
    carbon(r, OP1_Q10(AG_L_X0), OP1_Q10(AG_L_TIP), 1, (carb * 3) >> 5, glow);   /* crater 0..3 px (Q10) */
    carbon(r, OP1_Q10(AG_R_X0), OP1_Q10(AG_R_TIP), -1, 0, glow >> 1);

    /* the arc: bows upward (hot plasma rises), jagged with hiss, three glow layers */
    int32_t bulge = 0;
    if (flick > 900) {
        int32_t x0 = OP1_Q10(AG_L_TIP + 1), x1 = OP1_Q10(AG_R_TIP - 1), y = OP1_Q10(AG_CY);
        bulge = OP1_Q10(1) + ((flick * 9) >> 5);    /* up to ~10 px */
        int32_t jit = OP1_Q10(1) / 2 + ((hiss * 3) >> 5);
        int32_t px[7], py[7];
        for (int i = 0; i <= 6; i++) {
            px[i] = x0 + (x1 - x0) * i / 6;
            py[i] = y - ((bulge * tsin(i * 4096 / 6)) >> 15) + (i && i < 6 ? rnd(2 * jit + 1) - jit : 0);
        }
        static const int16_t wid[3] = { 0x1800, 0xa00, 0x480 }, lit[3] = { 50, 130, 256 };
        for (int l = 0; l < 3; l++) {
            shade(c, COL_WHITE, (lit[l] * (64 + (flick >> 7))) >> 8);
            for (int i = 0; i < 6; i++) ln(r, px[i], py[i], px[i + 1], py[i + 1], wid[l], c);
        }
        /* glare: 12 short rays around the arc, flickering lengths */
        int32_t gx = OP1_Q10(AG_CX), gy = y - bulge / 2;
        for (int i = 0; i < 12; i++) {
            int32_t a = i * 683 + 341, ca = tcos(a), sa = tsin(a);
            int32_t r0 = OP1_Q10(13) + ((flick * 3) >> 5), r1 = r0 + OP1_Q10(2) + ((flick >> 4) * (256 + rnd(256)) >> 8);
            if (sa > 9000) r0 += OP1_Q10(3), r1 += OP1_Q10(3);   /* keep clear of the carbons below */
            shade(c, COL_WHITE, 40 + (flick >> 8));
            ln(r, gx + ((r0 * ca) >> 15), gy + ((r0 * sa) >> 15), gx + ((r1 * ca) >> 15), gy + ((r1 * sa) >> 15), 0x400, c);
        }
    }

    /* strike sparks from the most recent voice */
    int tv = arc_tvoice;
    if (tv < OP1_VOICES) {
        const arc_voice_t *v = (const arc_voice_t *)OP1_VOICE(tv);
        int32_t amt = (v->live & 1) && v->env > 0 ? (v->env * clampk(k[3])) >> 15 : 0;
        if (amt > 1500) {
            int n = 2 + (amt >> 12);
            for (int i = 0; i < n; i++) {
                int32_t a = 4096 + 512 + rnd(3072);    /* upwards fan: 202..338 degrees */
                int32_t len = OP1_Q10(8) + (((amt >> 5) * (512 + rnd(512))) >> 4);
                int32_t cx = OP1_Q10(AG_CX), cy = OP1_Q10(AG_CY) - bulge / 2;
                int32_t s0 = len >> 1, ca = tcos(a), sa = tsin(a);
                shade(c, COL_RED, 96 + (amt >> 8));
                ln(r, cx + (((s0 >> 2) * ca) >> 13), cy + (((s0 >> 2) * sa) >> 13), cx + (((len >> 2) * ca) >> 13),
                   cy + (((len >> 2) * sa) >> 13), 0x500, c);   /* len up to ~70 px Q10: x sin would overflow */
            }
        }
    }

    /* page-2 rings around the paged dials; 2nd lamp ratio while coupled or while the ratio is being edited */
    for (int i = 0; i < 3; i++)
        if (op1_eng_page[i]) ring(r, OP1_Q10(ag_dial_x[i]), OP1_Q10(AG_DIAL_Y), OP1_Q10(16), 0x400, COL_K[i]);
    if (wc > 0 || op1_eng_page[0]) {
        char s[5]; ratio_text(s, ratio_of(wr));
        shade(c, COL_GREEN, wc > 0 ? 256 : 120);
        sq_text(r, OP1_Q10(284), OP1_Q10(8), 1536, s, 0x500, c);   /* 1.5 px per grid unit */
    }
}

#pragma GCC pop_options
const op1_screen_t SCREEN_arc = { arc_screen_init, arc_screen_update };
