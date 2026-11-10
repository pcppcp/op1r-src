/* moai: a minimal Minimoog-style synth voice for the OP-1 (every key is a full voice, 6 voices).
 *
 * Per voice: three band-limited oscillators set up as on a Minimoog (range LO, 32' .. 2', wave, interval) -> mixer,
 * driven into a 4-pole transistor-ladder low-pass filter with resonance and a contour (filter envelope) with keyboard
 * tracking; osc 3 in LO and the noise modulate the pitch and the filter, as the Model D's controllers do.
 * The amplitude envelope and the level are the OP-1's own (the envelope tab), as for every engine.
 *
 *   page 1 (filter)    blue   CUTOFF    20 Hz .. 20 kHz at C4 (exponential); the key's pitch moves it half way
 *                      green  EMPHASIS  resonance; 100 % self-oscillates (a sine at the cutoff pitch)
 *                      white  CONTOUR   how far the filter envelope opens the filter: 0 .. 8 octaves
 *                      red    DECAY     contour decay and release, 4 ms .. 2.4 s; the top quarter raises the
 *                                       sustain level instead (100 % = the filter stays open while the key is held)
 *   page 2 (press)     blue   SETUP     the oscillator setup, one of 32 (SETUPS in moai.h: ranges, waves, intervals of
 *                                       the three oscillators), shown on the screen; a few clicks per setup
 *                      green  DETUNE    osc 2 up, osc 3 down: 5 .. 40 cents of thickness
 *                      white  OSC 3     osc 3 level (its range, wave and interval come with the setup)
 *   Page 2 values that were never touched read 0: setup CLASSIC (v1's sound: 8' saw, 8' saw, 16' saw), 5 cents,
 *   osc 3 off. Velocity: level and contour depth.
 *
 * The ladder is a zero-delay-feedback (trapezoidal) design: the feedback is solved within the sample, so the
 * resonance sits exactly on the cutoff pitch and self-oscillation starts at a feedback gain of 4 at every cutoff.
 * The input is soft-clipped (the Minimoog's overdriven filter input). Every stage keeps the remainder of its
 * multiplication (error feedback), so slow cutoffs do not stall in a dead zone. Oscillators use polyBLEP for the
 * edges of saw and pulse. Like analog oscillators they run free (a note does not restart them) and drift slowly in
 * pitch, a little and independently.
 *
 * Speed (the audio interrupt is shared with the FX, the tape and the other instruments): the block is rendered in
 * passes, one per oscillator into the output buffer and then the filter in place, so each loop keeps its state in
 * registers. Every per-sample product is 16 x 16 bits (one cycle on the Blackfin; 32 x 32 takes three), pitch is
 * a linear ramp per block (no multiply per sample), and the divisions are reciprocal tables.
 *
 * Rate (2026-11-03): R246 renders a voice call as 128 samples at 88.2 kHz, 689 calls/s, and the tables
 * (gen_tables.py), the key tracking, the contour, the smoothing, the drift, the DC blockers, the saturation's
 * low-pass, the oscillators' 16 kHz limit and the LO readout are derived for that (they were for 44.1 kHz and 344
 * calls/s: the cutoff sat half an octave high, the resonance an octave over the knob, the contour ran twice as fast).
 *
 * Integer only; no 64-bit or division per sample. Voice state is a static array indexed by voice (the voice
 * object has only 32 bytes). Copyright (C) 2026 moai contributors.
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#define MM_DEFINE_TABLES                        /* SETUPS, PD, the presets: defined here (moai.h) */
#include "moai.h"

/* Code layout (docs/PERFORMANCE.md, v3.26): what runs per note or when the sound changes is cold, out of the run of
 * code that every callback goes through (GCC puts it in .text.unlikely, after the GUI code) */
#define MM_COLD __attribute__((cold))
/* the per-sample loops start on a 32-byte cache line; the transistor ladder's (DRIVE's top half) lie apart from the
 * rest, so the run of code a sound without them goes through stays short */
#define MM_LINE __attribute__((aligned(32)))
#ifdef __bfin__
#define MM_TR   __attribute__((section(".text.mm_tr")))
#else
#define MM_TR                                           /* (the host tests: Mach-O names sections otherwise) */
#endif

/* Blackfin multiplies (one cycle each, unlike a 32 x 32 multiply): written in assembly so the compiler cannot widen
 * them, with an exact C twin for the host (the tests compare both builds bit for bit). lo / hi = the 16-bit halves.
 *   MUL16(a, b)  integer lo(a) * lo(b)                       FM_LL(a, b)  fractional lo(a) * lo(b) * 2
 *   FM_HL(a, b)  fractional hi(a) * lo(b) * 2                FM_HH(a, b)  fractional hi(a) * hi(b) * 2
 *   FM_LLP / FM_HLP: FM_LL / FM_HL with b a packed pair (its low half, not range checked); MUL16LH: lo(a) * hi(b)
 * (fractional = Q15 x Q15 -> Q31; on a Q16 value hi(a) is its integer part, so hi(a) * Q15 coefficient = Q16)
 *   SUBS / ADDS  a -/+ b saturated to 32 bits                  SHL3S(a), SHL2S(a)  a << 3, << 2 saturated to 32 bits
 *   MAC4(...)    four fractional products summed on the accumulator (the ladder's state sum)
 * The host tests (-DMM_CHECK) trap when a 16-bit operand is out of range; on the OP-1 only the low half is used. */
#ifdef MM_CHECK
static inline int16_t i16(int32_t v) { if (v < -32768 || v > 32767) __builtin_trap(); return (int16_t)v; }
#else
#define i16(v) ((int16_t)(v))
#endif
#if defined(__bfin__)
static inline int32_t MUL16(int32_t a, int32_t b) { int32_t r; __asm__("%0 = %1.L * %2.L (IS);" : "=d"(r) : "d"(a), "d"(b)); return r; }
static inline int32_t FM_LL(int32_t a, int32_t b) { int32_t r; __asm__("%0 = %1.L * %2.L;" : "=d"(r) : "d"(a), "d"(b)); return r; }
static inline int32_t FM_HL(int32_t a, int32_t b) { int32_t r; __asm__("%0 = %1.H * %2.L;" : "=d"(r) : "d"(a), "d"(b)); return r; }
static inline int32_t FM_HH(int32_t a, int32_t b) { int32_t r; __asm__("%0 = %1.H * %2.H;" : "=d"(r) : "d"(a), "d"(b)); return r; }
#define FM_LLP FM_LL
#define FM_HLP FM_HL
static inline int32_t MUL16LH(int32_t a, int32_t b) { int32_t r; __asm__("%0 = %1.L * %2.H (IS);" : "=d"(r) : "d"(a), "d"(b)); return r; }
static inline int32_t SUBS(int32_t a, int32_t b)  { int32_t r; __asm__("%0 = %1 - %2 (S);" : "=d"(r) : "d"(a), "d"(b)); return r; }
static inline int32_t ADDS(int32_t a, int32_t b)  { int32_t r; __asm__("%0 = %1 + %2 (S);" : "=d"(r) : "d"(a), "d"(b)); return r; }
static inline int32_t SHL3S(int32_t a)            { int32_t r; __asm__("%0 = %1 << 3 (S);" : "=d"(r) : "d"(a)); return r; }
static inline int32_t SHL2S(int32_t a)            { int32_t r; __asm__("%0 = %1 << 2 (S);" : "=d"(r) : "d"(a)); return r; }
/* hi(s1) lo(c12) + hi(s2) hi(c12) + hi(s3) lo(c34) + hi(s4) hi(c34), fractional, on the accumulator */
static inline int32_t MAC4(int32_t s1, int32_t s2, int32_t s3, int32_t s4, int32_t c12, int32_t c34)
{
    int32_t r;
    __asm__("A0 = %1.H * %5.L;\n\tA0 += %2.H * %5.H;\n\tA0 += %3.H * %6.L;\n\t%0 = (A0 += %4.H * %6.H);"
            : "=D"(r) : "d"(s1), "d"(s2), "d"(s3), "d"(s4), "d"(c12), "d"(c34) : "A0");   /* D: an even register (A0 -> R0, R2, ...) */
    return r;
}
#else
static inline int32_t sat32(int64_t v) { return v > 2147483647 ? 2147483647 : (v < -2147483647 - 1 ? -2147483647 - 1 : (int32_t)v); }
static inline int32_t MUL16(int32_t a, int32_t b) { return (int32_t)i16(a) * (int32_t)i16(b); }
static inline int32_t FM_LL(int32_t a, int32_t b) { return sat32(2 * (int64_t)((int32_t)i16(a) * (int32_t)i16(b))); }
static inline int32_t FM_HL(int32_t a, int32_t b) { return sat32(2 * (int64_t)((a >> 16) * (int32_t)i16(b))); }
static inline int32_t FM_HH(int32_t a, int32_t b) { return sat32(2 * (int64_t)((a >> 16) * (b >> 16))); }
static inline int32_t FM_LLP(int32_t a, int32_t b) { return sat32(2 * (int64_t)((int32_t)i16(a) * (int32_t)(int16_t)b)); }
static inline int32_t FM_HLP(int32_t a, int32_t b) { return sat32(2 * (int64_t)((a >> 16) * (int32_t)(int16_t)b)); }
static inline int32_t MUL16LH(int32_t a, int32_t b) { return (int32_t)i16(a) * (b >> 16); }
static inline int32_t SUBS(int32_t a, int32_t b)  { return sat32((int64_t)a - b); }
static inline int32_t ADDS(int32_t a, int32_t b)  { return sat32((int64_t)a + b); }
static inline int32_t SHL3S(int32_t a)            { return sat32((int64_t)a * 8); }
static inline int32_t SHL2S(int32_t a)            { return sat32((int64_t)a * 4); }
static inline int32_t MAC4(int32_t s1, int32_t s2, int32_t s3, int32_t s4, int32_t c12, int32_t c34)
{
    int64_t a0 = 2 * (int64_t)((s1 >> 16) * (int16_t)c12) + 2 * (int64_t)((s3 >> 16) * (int16_t)c34);
    int64_t a1 = 2 * (int64_t)((s2 >> 16) * (c12 >> 16)) + 2 * (int64_t)((s4 >> 16) * (c34 >> 16));
    return sat32(a0 + a1);
}
#endif

mm_voice_t V[OP1_VOICES];

/* voice index and velocity gain as op1_voice_index / op1_voice_vel_gain, without their division by 0x24
 * (offset * 1821 >> 16 is exact for the multiples of 0x24 up to 6 voices) */
static inline int vidx(const void *self) { return (int)((((uintptr_t)self - (uintptr_t)OP1_VOICE_BASE) * 1821u) >> 16); }
static inline int32_t vel_gain(int vi)
{
    int32_t v = OP1_VOICE_VEL[vi] >> 8;
    return v > 0 && v <= 127 ? v * 258 : 32767;
}

/* ---- oscillators ---------------------------------------------------------------------------------------- */

/* band-limited saw, +-16384, from phase p (edge at the wrap), increment inc and r = 2^30 / (inc >> 8) */
static inline int32_t saw_bl(uint32_t p, uint32_t inc, uint32_t r)
{
    int32_t s = (int32_t)(p ^ 0x80000000u) >> 17;
    if (__builtin_expect(p + inc < 2 * inc, 0)) {        /* within one sample of the edge (one compare, rare) */
        if (p < inc) {                                   /* just after the edge: t = p / inc (Q14) */
            int32_t u = 16384 - (int32_t)(((p >> 8) * r) >> 16);
            s += MUL16(u, u) >> 14;
        } else {                                         /* just before it: t = (2^32 - p) / inc */
            int32_t u = 16384 - (int32_t)((((0u - p) >> 8) * r) >> 16);
            s -= MUL16(u, u) >> 14;
        }
    }
    return s;
}

static inline int32_t tri_of(uint32_t p)                 /* +-16384 */
{
    int32_t a = (int32_t)(p ^ 0x80000000u) >> 17;
    return 2 * (a < 0 ? -a : a) - 16384;
}

int entry_of(int32_t w)
{
    w = clampk(w);
    int32_t o = (w & 1023) - PRESET_OFS;
    if (o >= 0 && o <= 6 && !(o & 1) && (w >> 10) < PRESET_SLOTS) {
        int p = (o >> 1) * PRESET_SLOTS + (w >> 10);
        if (p < N_PRESETS) return N_SETUPS + p;
    }
    return w >> 10;
}
MM_COLD int32_t entry_word(int e)
{
    e = e < 0 ? 0 : (e >= N_ENTRIES ? N_ENTRIES - 1 : e);
    if (e < N_SETUPS) return e * 1024 + 512;
    e -= N_SETUPS;
    return (e % PRESET_SLOTS) * 1024 + PRESET_OFS + 2 * (e / PRESET_SLOTS);
}

void wave_of(wave_t *o, int wave)
{
    static const struct { int16_t c1, c2, c3; uint32_t w; uint8_t kind; } T[] = {
        { 0, 0, 32767, 0, K_TRI },                      /* triangle */
        { 16384, 0, 16384, 0, K_SHARK },                /* shark tooth: half saw, half triangle */
        { 32767, 0, 0, 0, K_SAW },                      /* saw */
        { -32767, 0, 0, 0, K_SAW },                     /* reverse saw */
        { 19661, 19661, 0, 0x80000000u, K_PULSE },      /* square (0.6: as loud as the saw) */
        { 22938, 22938, 0, 0x4CCCCCCDu, K_PULSE },      /* wide pulse, 30 % */
        { 26214, 26214, 0, 0x1EB851ECu, K_PULSE },      /* narrow pulse, 12 % */
        { 23170, 0, 0, 0, K_NOISE },                    /* noise (white, 0.7) */
    };
    o->c1 = T[wave].c1; o->c2 = T[wave].c2; o->c3 = T[wave].c3; o->w = T[wave].w; o->kind = T[wave].kind;
}

/* one oscillator sample (host tests; the engine uses osc_pass) */
static inline __attribute__((unused)) int32_t wave_at(const wave_t *w, uint32_t p, uint32_t inc, uint32_t r)
{
    int32_t s = (w->c1 * saw_bl(p, inc, r)) >> 15;
    if (w->c2) s -= (w->c2 * saw_bl(p + w->w, inc, r)) >> 15;
    if (w->c3) s += (w->c3 * tri_of(p)) >> 15;
    return s;
}

/* One oscillator over the block into buf (add: accumulate). a1..a3 = the wave weights with the level folded in
 * (Q15), inc ramps by dinc per sample. kind and add are constants at every call: one tight loop per case. */
static inline __attribute__((always_inline)) void osc_loop(int16_t *buf, uint32_t *php, uint32_t inc, int32_t dinc,
    uint32_t r, int32_t a1, int32_t a2, int32_t a3, uint32_t w, uint32_t *rng, const int kind, const int add)
{
    uint32_t p = *php, n32 = *rng;
    for (int n = 0; n < OP1_BLOCK; n++) {
        int32_t s;
        if (kind == K_NOISE) {
            n32 = n32 * 1664525u + 1013904223u;
            s = MUL16(a1, (int32_t)(n32 >> 17) - 16384);
        } else {
            p += inc; inc = (uint32_t)((int32_t)inc + dinc);
            if (kind == K_TRI) s = MUL16(a3, tri_of(p));
            else {
                s = MUL16(a1, saw_bl(p, inc, r));
                if (kind == K_PULSE) s -= MUL16(a2, saw_bl(p + w, inc, r));
                if (kind == K_SHARK) s += MUL16(a3, tri_of(p));
            }
        }
        s >>= 15;
        buf[n] = (int16_t)(add ? buf[n] + s : s);
    }
    *php = p; *rng = n32;
}

/* Two oscillators of the same kind in one pass (one load / add / store of buf for both). a = weights of the first,
 * b of the second (Q15, levels folded in), each its own pulse offset, increment, ramp and reciprocal. */
static inline __attribute__((always_inline)) void osc_loop2(int16_t *buf, uint32_t *pa, uint32_t ia, int32_t da, uint32_t ra,
    const int32_t *a, uint32_t wa, uint32_t *pb, uint32_t ib, int32_t db, uint32_t rb, const int32_t *b, uint32_t wb,
    const int kind, const int add)
{
    uint32_t p = *pa, q = *pb;
    int32_t a1 = a[0], a2 = a[1], a3 = a[2], b1 = b[0], b2 = b[1], b3 = b[2];
    for (int n = 0; n < OP1_BLOCK; n++) {
        p += ia; ia = (uint32_t)((int32_t)ia + da);
        q += ib; ib = (uint32_t)((int32_t)ib + db);
        int32_t s;
        if (kind == K_TRI) s = MUL16(a3, tri_of(p)) + MUL16(b3, tri_of(q));
        else {
            s = MUL16(a1, saw_bl(p, ia, ra)) + MUL16(b1, saw_bl(q, ib, rb));
            if (kind == K_PULSE) s -= MUL16(a2, saw_bl(p + wa, ia, ra)) + MUL16(b2, saw_bl(q + wb, ib, rb));
            if (kind == K_SHARK) s += MUL16(a3, tri_of(p)) + MUL16(b3, tri_of(q));
        }
        s >>= 15;
        buf[n] = (int16_t)(add ? buf[n] + s : s);
    }
    *pa = p; *pb = q;
}

static uint32_t rcp30(uint32_t inc)                      /* 2^30 / (inc >> 8), from a table (no division) */
{
    uint32_t x = (inc >> 8) | 1;
    int m = 31;
    while (!(x >> m)) m--;
    uint32_t xn = m >= 15 ? x >> (m - 15) : x << (15 - m);              /* normalised to [2^15, 2^16) */
    int32_t i = (int32_t)(xn - 32768) >> 9, f = (int32_t)(xn & 511);
    uint32_t r = MM_RCP[i] - (uint32_t)((((int32_t)MM_RCP[i] - (int32_t)MM_RCP[i + 1]) * f) >> 9);
    return m >= 15 ? r >> (m - 15) : r << (15 - m);
}

/* kennel: on the Blackfin the saw, pulse, triangle and shark passes are moai_osc.S (gen_osc.py), bit for bit
 * osc_loop / osc_loop2 above; noise stays in C. MM_OSC_C builds the C instead (harness/tier2/moai_fc_guest.c compares
 * them). The layout is the assembly's: what its loop reads every sample first (P + 0..60). */
typedef struct {
    uint32_t c80; int32_t m8192;                                  /* 2^31, -8192 */
    int32_t dinc, w, a1, a2, a3;                                  /* the first oscillator */
    int32_t dinc2, w2, b1, b2, b3;                                /* the second (pairs) */
    uint32_t p, inc, r, q, inc2, r2;
} osc_args_t;
#if defined(__bfin__) && !defined(MM_OSC_C)
#define MM_OSC_ASM 1
#define OS_DECL(k) void os1_##k##_set(int16_t *, osc_args_t *); void os1_##k##_add(int16_t *, osc_args_t *); \
                   void os2_##k##_set(int16_t *, osc_args_t *); void os2_##k##_add(int16_t *, osc_args_t *);
OS_DECL(saw) OS_DECL(pulse) OS_DECL(tri) OS_DECL(shark)
#undef OS_DECL
static void osc_asm(int16_t *buf, osc_args_t *x, int kind, int pair, int add)
{
#define OS_CALL(k) (pair ? (add ? os2_##k##_add : os2_##k##_set) : (add ? os1_##k##_add : os1_##k##_set))(buf, x)
    switch (kind) {
    case K_SAW: OS_CALL(saw); break;
    case K_PULSE: OS_CALL(pulse); break;
    case K_TRI: OS_CALL(tri); break;
    default: OS_CALL(shark); break;
    }
#undef OS_CALL
}
#endif

static void osc_pass(int16_t *buf, const wave_t *wv, uint32_t r, uint32_t *php, uint32_t inc, int32_t dinc,
                     int32_t lvl, uint32_t *rng, int add)
{
    int32_t a1 = (wv->c1 * lvl) >> 15, a2 = (wv->c2 * lvl) >> 15, a3 = (wv->c3 * lvl) >> 15;
    uint32_t w = wv->w;
#ifdef MM_OSC_ASM
    if (wv->kind != K_NOISE) {
        osc_args_t x = { 0x80000000u, -8192, dinc, (int32_t)w, a1, a2, a3, 0, 0, 0, 0, 0, *php, inc, r, 0, 0, 0 };
        osc_asm(buf, &x, wv->kind, 0, add);
        *php = x.p;
        return;
    }
#endif
#define OSC(kind) (add ? osc_loop(buf, php, inc, dinc, r, a1, a2, a3, w, rng, kind, 1) \
                       : osc_loop(buf, php, inc, dinc, r, a1, a2, a3, w, rng, kind, 0))
    switch (wv->kind) {
    case K_SAW: OSC(K_SAW); break;
    case K_PULSE: OSC(K_PULSE); break;
    case K_TRI: OSC(K_TRI); break;
    case K_SHARK: OSC(K_SHARK); break;
    default: OSC(K_NOISE); break;
    }
#undef OSC
}

/* phase increment x (1 + q / 2^16) for q a small ratio offset (|q| < 2^12: +-100 cents), also for a signed slope */
static inline int32_t scale_inc(int32_t i, int32_t q) { return i + (((i >> 13) * q) >> 3); }
/* x 2^(semi/12) (semi 0..12): Q14 ratio applied in two halves (per block, 32-bit) */
static inline int32_t semi_inc(int32_t i, int semi)
{
    int32_t m = MM_SEMI[semi];
    return (i >> 14) * m + (((i & 16383) * m) >> 14);
}
/* the same for an increment (>= 0), unsigned and held at INC_SAT: the top keys times an interval or a pitch
 * modulation pass 2^31. An oscillator from INC_MAX up (above 16 kHz) stays off; INC_SAT is above it by more than the
 * fine tune and detune move it (scale_inc, |q| < 2^12), and as far under 2^31 */
#define INC_MAX 0x2FFFFFFF                                      /* 16.5 kHz at 88.2 kHz */
#define INC_SAT 0x6FFFFFFF
static inline int32_t semi_inc_sat(int32_t i, int semi)
{
    uint32_t m = MM_SEMI[semi], r = ((uint32_t)i >> 14) * m + ((((uint32_t)i & 16383) * m) >> 14);
    return r > INC_SAT ? INC_SAT : (int32_t)r;
}

/* ---- filter ----------------------------------------------------------------------------------------------- */

/* cubic soft clip on a Q16 value: x - x^3 / (3 L^2) up to +-L, output +-F. kk: CLIP_K in its high half */
static inline int32_t sclipk(int32_t x, int32_t kk)
{
    x = SHL2S(x) >> 2;                                               /* clamp to +-2^29 = +-L in Q16 */
    int32_t xi = x >> 16;
    int32_t q = MUL16LH(MUL16(xi, xi) >> 12, kk) >> 12;            /* x^2 / (3 L^2), Q15 */
    return x - FM_HL(x, q);
}
static inline int32_t sclip(int32_t x) { return sclipk(x, CLIP_K << 16); }
/* the same curve for the transistor stages, 6 instructions and no constant register (the input clip's CLIP_K, x 4
 * by a shift: one data register more for the loop): x^2 / (3 L^2) from x << 2 (the saturating shift is the
 * clamp: beyond +-L the stage goes on at 2/3 slope instead of turning back, never reached in practice) */
static inline int32_t stclip(int32_t x, int32_t kk)                  /* kk: CLIP_K in its high half (no constant register) */
{
    int32_t s = SHL2S(x);
    int32_t q = FM_HH(FM_HH(s, s), kk);                            /* hi: x^2 / (12 L^2), Q15 */
    return x - SHL2S(FM_HH(x, q));
}

/* Four trapezoidal one-pole stages in series, v = gg (in - s), y = s + v, s += 2 v, with the feedback
 * u = x - k y4 solved inside the sample: y4 = G4 u + S (S from the states only), so u = (x - k S) / (1 + k G4). */
static void lad_coef(lad_coef_t *c, int32_t gg, int32_t k)       /* once per chunk */
{
    int32_t om = 32767 - gg;                                                   /* 1 - gg (one LSB short: 16 bits) */
    if (gg > MM_GG_K0) {                                                       /* no resonance up in the open range */
        const int32_t d = MM_GG_K1 - gg;
        k = d <= 0 ? 0 : MUL16(k, (d * (MM_GG_KA + ((d * MM_GG_KB) >> 16))) >> 8) >> 15;
    }
    int32_t g2 = MUL16(gg, gg) >> 15, g3 = MUL16(g2, gg) >> 15, g4 = MUL16(g3, gg) >> 15;
    int32_t c4 = om, c3 = MUL16(gg, om) >> 15, c2 = MUL16(g2, om) >> 15, c1 = MUL16(g3, om) >> 15;
    c->ggk = gg | (k << 16);                                                    /* k: Q12 = k / 8 in Q15 */
    c->c12 = c1 | (c2 << 16); c->c34 = c3 | (c4 << 16);
    int32_t t = MUL16(k, g4) >> 12, i = t >> 11, f = t & 2047;                 /* k G4, Q15: 1/16 per table step */
    int32_t inv = MM_INV[i] - (MUL16((int32_t)MM_INV[i] - (int32_t)MM_INV[i + 1], f) >> 11);   /* 1 / (1 + k G4), Q15 */
    c->invk = inv | (CLIP_K << 16);                                             /* + the saturator's constant */
}

/* one sample: x = input, Q16 (F = full scale); returns y4, Q16. A stage is d = in - s, v = gg d, y = s + v,
 * s = y + v: four instructions with the Q16 states (v = hi(d) * gg, fractional, is already Q16). The feedback gain
 * k S saturates at 32768 (then u is clipped by sclip anyway: inv >= 0.37). */
static inline __attribute__((always_inline)) int32_t lad_step(lad_t *l, const lad_coef_t *c, int32_t x,
    int32_t *restrict t, const int with_res, const int with_tr)
{
    int32_t u;
    if (with_res) {
        int32_t S = MAC4(l->s1, l->s2, l->s3, l->s4, c->c12, c->c34);
        int32_t num = SUBS(x, SHL3S(FM_HH(S, c->ggk)));
        u = sclipk(FM_HLP(num, c->invk), c->invk);
    } else u = sclipk(x, c->invk);                                  /* k = 0: no state sum, no solve (inv = 1) */
    int32_t v, y;
    if (with_tr) {      /* the transistor stages: clip(in) - clip(state), the state's clip taken as the stage's clipped
                         * output one sample ago (t): 4 clips a sample instead of 7, as close to the reference model.
                         * t stays in memory (the voice's lad_t), not registers: one load and one store per stage,
                         * which the Blackfin issues beside the arithmetic, and the loop keeps its hardware loop */
        v = FM_HLP(SUBS(u, t[0]), c->ggk); y = l->s1 + v; l->s1 = y + v; y = t[0] = stclip(y, c->invk);
        v = FM_HLP(SUBS(y, t[1]), c->ggk); y = l->s2 + v; l->s2 = y + v; y = t[1] = stclip(y, c->invk);
        v = FM_HLP(SUBS(y, t[2]), c->ggk); y = l->s3 + v; l->s3 = y + v; y = t[2] = stclip(y, c->invk);
        v = FM_HLP(SUBS(y, t[3]), c->ggk); y = l->s4 + v; l->s4 = y + v; t[3] = stclip(y, c->invk);
        return y;
    }
    v = FM_HLP(SUBS(u, l->s1), c->ggk); y = l->s1 + v; l->s1 = y + v;
    v = FM_HLP(SUBS(y, l->s2), c->ggk); y = l->s2 + v; l->s2 = y + v;
    v = FM_HLP(SUBS(y, l->s3), c->ggk); y = l->s3 + v; l->s3 = y + v;
    v = FM_HLP(SUBS(y, l->s4), c->ggk); y = l->s4 + v; l->s4 = y + v;
    return y;
}

/* ---- tables ------------------------------------------------------------------------------------------------ */

static inline int32_t lerp_tab(const uint16_t *t, int32_t pos)   /* pos: Q10 octaves, 64 per table step */
{
    int32_t i = pos >> 6, f = pos & 63;
    return t[i] + (((int32_t)t[i + 1] - (int32_t)t[i]) * f >> 6);
}

int32_t ulog2_q10(uint32_t v)
{
    int m = 31;
    while (m > 0 && !(v >> m)) m--;
    uint32_t f = (v << (31 - m)) & 0x7FFFFFFFu;          /* 31 fraction bits */
    int32_t i = (int32_t)(f >> 26), fr = (int32_t)((f >> 11) & 0x7FFF);
    return m * 1024 + MM_LOG2[i] + (((int32_t)MM_LOG2[i + 1] - (int32_t)MM_LOG2[i]) * fr >> 15);
}
/* phase increment -> octaves relative to C4 (Q10) */
static int32_t pitch_oct(uint32_t inc) { return inc ? ulog2_q10(inc) - MM_C4_LOG2 : -10 * 1024; }

int16_t P[P_N];
volatile uint8_t mm_view;
volatile int8_t mm_map[4] = { P_SETUP, P_CUTOFF, P_EMPH, P_CONTOUR };
volatile int16_t mm_lfo[4];
volatile uint8_t mm_lfo_seq;

MM_COLD static void derive_osc(int16_t *q, int e)               /* entry (setup or preset) -> the oscillators, noise, ... */
{
    const preset_t *pr = preset_of(e);
    const setup_t *st = &SETUPS[e & (N_SETUPS - 1)];
    const osc_t *o = pr ? pr->o : st->o;
    for (int i = 0; i < 3; i++) {
        q[P_O(i, 0)] = o[i].r; q[P_O(i, 1)] = o[i].w;
        q[P_O(i, 2)] = (int16_t)(o[i].semi * 100); q[P_O(i, 3)] = (int16_t)(o[i].vol * 8191);
    }
    if (pr) {                                             /* a preset: its saturation and modulation, no feedback */
        q[P_NOISE] = 0; q[P_SAT] = (int16_t)pct(pr->sat); q[P_FEEDBACK] = 0;
        q[P_MOD_MIX] = 0; q[P_MOD_PITCH] = (int16_t)pct(pr->pit); q[P_MOD_FILT] = (int16_t)pct(pr->flt);
        return;
    }
    q[P_NOISE] = (int16_t)(st->noise * 8191);
    q[P_SAT] = (int16_t)pct(st->sat); q[P_FEEDBACK] = (int16_t)pct(st->fb);             /* (the drive: mm_ui.h) */
    q[P_MOD_MIX] = q[P_MOD_PITCH] = q[P_MOD_FILT] = 0;                                   /* no modulation */
}
/* the contour knob: one knob from no contour, through plucks, to slow swells. Amount up over the first 40 %,
 * decay 15 ms .. 2.4 s over the first 75 %, then sustain up to 70 %, and in the last 15 % a slower attack.
 * On a preset the amount is scaled so that at the preset's own contour it is the preset's amount (and still 0 at 0). */
static inline int32_t contour_amount(int32_t u) { int32_t a = (clampk(u) * 5) >> 1; return ((a > 32767 ? 32767 : a) * 27853) >> 15; }
MM_COLD static void derive_contour(int16_t *q, int32_t u, int e)
{
    u = clampk(u);
    int32_t a = contour_amount(u);
    const preset_t *pr = preset_of(e);
    if (pr) {
        int32_t at = contour_amount(pct(pr->con));
        a = at > 0 ? a * pct(pr->amt) / at : pct(pr->amt);
        if (a > 32767) a = 32767;
    }
    q[P_AMOUNT] = (int16_t)a;
    int32_t d = u <= 24576 ? 6268 + ((u * 963) >> 10) : 29400;
    q[P_DECAY] = (int16_t)d;
    q[P_RELEASE] = (int16_t)(d < 8548 ? 8548 : d);
    q[P_SUSTAIN] = (int16_t)(u > 22937 ? ((u - 22937) * 2333) >> 10 : 0);
    q[P_ATTACK] = (int16_t)(u > 27852 ? ((u - 27852) * 4167) >> 10 : 0);
}
volatile uint8_t mm_home_hold, mm_ui_beat;
int16_t mm_derived_setup = -1;                           /* what P's oscillator / contour values come from */
int32_t mm_derived_contour = -1;
void sync_derived(int32_t setup_word, int32_t contour_word)          /* audio and GUI thread: same result */
{
    int e = entry_of(setup_word), np = 0;
    if (e != mm_derived_setup) {                          /* another sound: its contour too (a preset's amount) */
        np = 1;
        derive_osc(P, e); mm_derived_setup = (int16_t)e;
    }
    if (np || contour_word != mm_derived_contour) { derive_contour(P, contour_word, e); mm_derived_contour = contour_word; }
}

/* The ladder's input level and where the make-up goes (v3.7). The input clip is also the resonance's limiter (the
 * feedback is in the clipped sum, as on the real ladder): a hot input crushes the resonance. Until v3.6 the make-up
 * was an input gain (up to 2.4x at full emphasis), so the more emphasis, the harder the input clipped and the less
 * of the resonance's whistle came through: at 85 % the harmonic at the cutoff stood only 3 .. 9 dB above no emphasis.
 * Now the input runs 4 dB lower (IN_LVL) and the make-up is an output gain before the output stage (MU_OUT: the
 * bass stays, the whistle sits on top, +20 .. 30 dB). DRIVE still pushes the input into the clip. */
#define IN_LVL 6720                                     /* mix -> ladder input / 4 at 1x drive, Q15 (v3.6: 10922) */
#define MU_OUT 13312                                    /* 1 / 0.615 (the input's 4 dB back), Q13 */

void contour_coefs(envc_t *e, const int16_t *q)
{
    int32_t ai = time_index(q[P_ATTACK]);
    e->att = q[P_ATTACK] < 400 ? (1 << 22) / 6 + 1 : (int32_t)MM_DEC[ai] << 4;   /* MM_DEC: Q18; 6 chunks: 2.2 ms */
    e->dcoef = MM_DEC[time_index(q[P_DECAY])];
    e->rcoef = MM_DEC[time_index(q[P_RELEASE])];
    e->sus = clampk(q[P_SUSTAIN]) << 7;
}

uint8_t mm_last = 0xFF;
int16_t mm_scope[SCOPE_N] __attribute__((aligned(4)));
uint16_t mm_scope_w;
uint8_t mm_scope_sh;
uint32_t mm_serial;
uint8_t mm_osc3_off;
uint8_t mm_osc3_why;

static int sounding_voices(void)
{
    int n = 0;
    for (int i = 0; i < OP1_VOICES; i++)
        if (V[i].fresh || (V[i].serial && mm_serial - V[i].serial < OP1_VOICES)) n++;
    return n;
}

/* stage cycle counts for tools/bench (built with -DMM_PROF on the Blackfin simulator only) */
#ifdef MM_PROF
uint32_t mm_prof[6]; static uint32_t mm_prof_t;
static inline uint32_t mm_cyc(void) { uint32_t c; __asm__ volatile("%0 = CYCLES;" : "=d"(c)); return c; }
#define PROF(i) do { uint32_t t_ = mm_cyc(); if ((i) > 0) mm_prof[(i) - 1] += t_ - mm_prof_t; mm_prof_t = t_; } while (0)
#else
#define PROF(i) do { } while (0)
#endif

/* ---- engine ------------------------------------------------------------------------------------------------ */

MM_COLD static void mm_note_on(void *self, const int16_t *params)
{
    int vi = vidx(self);
    mm_voice_t *v = &V[vi];
    (void)params;
    if (!v->rng) v->rng = 0x9E3779B9u * (uint32_t)(vi + 1);
    for (int i = 0; i < 3; i++) v->ph[i] += rnd(&v->rng);         /* free-running oscillators: any phase */
    if (!v->nrng) v->nrng = v->rng ^ 0x5A5A5A5Au;
    v->lad = (lad_t){ 0, 0, 0, 0, { 0, 0, 0, 0 } };
    v->env = 0; v->stage = 0; v->fresh = 1;
    v->yprev = 0; v->dc[0] = v->dc[1] = v->dc[2] = 0;
    mm_last = (uint8_t)vi;
}

/* one oscillator, ready to render: its wave, increment / ramp, polyBLEP reciprocal, level, phase index */
typedef struct { wave_t wv; uint32_t r, inc; int32_t dinc, lvl; int k; } oscp_t;

/* oscillator k: range, interval and fine (cents), detune / drift q (Q16), level (Q15); i0 / di: the pitch's increment
 * and ramp, r0 its polyBLEP reciprocal. 0 = off or above 16 kHz */
static int osc_prep(oscp_t *o, int k, int wave, int range, int32_t cents, int32_t q, int32_t lvl,
                    int32_t i0, int32_t di, uint32_t r0)
{
    if (range == R_OFF || lvl <= 0) return 0;
    wave_of(&o->wv, wave);
    int oct = RANGE_OCT[range];
    if (range == R_LO) {                                          /* the rate: 4x the span, in whole octaves + +-6 */
        cents *= LO_FREQ_X;
        while (cents > 600) { cents -= 1200; oct++; }
        while (cents < -600) { cents += 1200; oct--; }
    }
    int32_t semi = (((cents + 850) * 1311) >> 17) - 8;            /* nearest semitone (x / 100 by a multiply: exact here) */
    q += (cents - semi * 100) * 38;                               /* fine: 37.9 per cent in Q16 */
    if (semi < 0) { semi += 12; oct--; }
    int32_t inc = scale_inc(semi_inc_sat(i0, semi), q), dinc = scale_inc(semi_inc(di, semi), q);
    uint32_t r = r0, m = MM_ISEMI[semi];
    r = (r >> 15) * m + (((r & 32767) * m) >> 15);                /* 1 / increment, for this interval */
    if (oct > 0) {
        if (inc >= (INC_MAX >> oct)) return 0;                    /* the octave up would be above 16 kHz */
        inc <<= oct; dinc *= 1 << oct; r >>= oct;
    } else if (oct < 0) {
        inc >>= -oct; dinc >>= -oct; r <<= -oct;
    }
    if (inc >= INC_MAX) return 0;
    o->inc = (uint32_t)inc; o->dinc = dinc; o->r = r; o->lvl = lvl; o->k = k;
    return 1;
}

/* two prepared oscillators of the same kind into buf in one pass */
static void osc_pass2(int16_t *buf, mm_voice_t *v, const oscp_t *x, const oscp_t *y, int add)
{
    int32_t a[3] = { (x->wv.c1 * x->lvl) >> 15, (x->wv.c2 * x->lvl) >> 15, (x->wv.c3 * x->lvl) >> 15 };
    int32_t b[3] = { (y->wv.c1 * y->lvl) >> 15, (y->wv.c2 * y->lvl) >> 15, (y->wv.c3 * y->lvl) >> 15 };
    uint32_t *pa = &v->ph[x->k], *pb = &v->ph[y->k];
#ifdef MM_OSC_ASM
    {
        osc_args_t o = { 0x80000000u, -8192, x->dinc, (int32_t)x->wv.w, a[0], a[1], a[2],
                         y->dinc, (int32_t)y->wv.w, b[0], b[1], b[2], *pa, x->inc, x->r, *pb, y->inc, y->r };
        osc_asm(buf, &o, x->wv.kind, 1, add);
        *pa = o.p; *pb = o.q;
        return;
    }
#endif
#define OSC2(kind) (add ? osc_loop2(buf, pa, x->inc, x->dinc, x->r, a, x->wv.w, pb, y->inc, y->dinc, y->r, b, y->wv.w, kind, 1) \
                        : osc_loop2(buf, pa, x->inc, x->dinc, x->r, a, x->wv.w, pb, y->inc, y->dinc, y->r, b, y->wv.w, kind, 0))
    switch (x->wv.kind) {
    case K_SAW: OSC2(K_SAW); break;
    case K_PULSE: OSC2(K_PULSE); break;
    case K_TRI: OSC2(K_TRI); break;
    default: OSC2(K_SHARK); break;
    }
#undef OSC2
}

/* ---- modulation: the Model D's controllers ----
 * The source is osc 3 while it is in LO (its shape at its phase, read as a control signal: in the audio ranges it would
 * be FM, which this engine does not do) mixed with noise (a new random value per chunk) by MOD MIX. MOD OSC bends
 * oscillators 1 and 2 up to +-1 octave (a ramp per block: vibrato, trills with the square, sirens with the saws); MOD
 * FILT moves the cutoff up to +-3 octaves (per chunk: wah, wobble, a gate-like chop with the square). Osc 3 still
 * runs as the source with its volume at 0, or while it rests for CPU. Session values, from the noise row. */
static int32_t mod_shape(int wave, uint32_t p)                    /* osc 3's wave as a control signal, +-32768 */
{
    int32_t s = (int32_t)(p ^ 0x80000000u) >> 16;                 /* the rising saw */
    int32_t t = 2 * (s < 0 ? -s : s) - 32768;                     /* the triangle */
    switch (wave) {
    case W_TRI: return t;
    case W_SHARK: return (s + t) >> 1;
    case W_SAW: return s;
    case W_RSAW: return -s;
    case W_SQUARE: return p < 0x80000000u ? 32767 : -32767;
    case W_WIDE: return p < 0x4CCCCCCDu ? 32767 : -32767;
    default: return p < 0x1EB851ECu ? 32767 : -32767;
    }
}
/* increment i x 2^(c / 1200), any c (per block), held at INC_SAT (the octaves down first: in range) */
static int32_t cents_inc(int32_t i, int32_t c)
{
    int oct = 0;
    while (c < 0) { c += 1200; oct--; }
    while (c >= 1200) { c -= 1200; oct++; }
    if (oct < 0) i >>= -oct;
    int32_t semi = (c * 1311) >> 17;                              /* c / 100, exact for 0 .. 1199 */
    i = scale_inc(semi_inc_sat(i, semi), (c - semi * 100) * 38);
    return oct <= 0 ? i : i >= (INC_SAT >> oct) ? INC_SAT : i << oct;
}

/* saturation after the filter: the output stage's soft clip driven harder (gain, Q15 / 8: 1 .. 8x) and off centre
 * (bias, Q16: even harmonics, the warm side); its own DC blocker after it (the bias and the asymmetric clip leave
 * DC that follows the level), then a one-pole low-pass that closes as the saturation rises (16 kHz .. 5 kHz): an
 * analog stage's limited bandwidth, which takes the fizz off the clipped edges (the "digital" sound of a bare clip).
 * Per block. */
typedef struct { int32_t g, b, lp, mu; } sat_t;                 /* mu: the output make-up (all paths), Q13 */
/* output level x 1 / sqrt(gain), Q15: about even loudness */
static const uint16_t SAT_COMP[9] = { 32767, 31110, 27330, 23261, 19759, 16956, 14746, 12994, 11585 };
static void sat_coef(sat_t *s, int32_t v)
{
    s->g = sat_gain(v);
    s->b = ((clampk(v) * 1229) >> 15) << 16;                 /* up to 0.15 of the clip's limit */
    const int32_t c = clampk(v), i = c >> 12;                   /* one-pole coefficient, Q15: 1 - exp(-2 pi fc / fs) */
    s->lp = MM_SAT_LP[i] - (((MM_SAT_LP[i] - MM_SAT_LP[i + (i < 8)]) * (c & 4095)) >> 12);
}

/* one chunk through drive, (feedback,) ladder, AC coupling, (saturation,) soft output stage and level, in place.
 * with_fb / with_res / with_sat are constants in the eight instances below (docs/PERFORMANCE.md, "audio paths"):
 * without the feedback path, the resonance or the saturation the loop is shorter. Real functions (not inlined into
 * render): the sample loop gets the hardware loop and the registers to itself. */
static inline __attribute__((always_inline)) void filter_chunk_t(int16_t *b, lad_t *lad, const lad_coef_t *lc, int32_t gxog,
    int32_t fb, const sat_t *st, int32_t *pyp, int32_t *pdc, const int with_fb, const int with_res, const int with_sat,
    const int with_tr)
{
    lad_t l = *lad;
    int32_t yp = *pyp, dc = pdc[0], dc2 = pdc[1], lp = pdc[2], invk = lc->invk, sb = st->b;
    int32_t mufb = (st->mu & 0xFFFF) | (fb << 16);                         /* output make-up | feedback: one register */
    int32_t sgla = (st->g & 0xFFFF) | (st->lp << 16);                      /* saturation gain | its low-pass: one register */
    for (int n = 0; n < CHUNK; n++) {
        int32_t x = SHL2S(FM_LLP(b[n], gxog));                                /* drive: x 4 after the gain / 4 */
        if (with_fb) x = ADDS(x, SHL2S(FM_HH(yp - dc, mufb)));                  /* feedback: the AC-coupled output, x 0..3 */
        yp = lad_step(&l, lc, x, lad->t, with_res, with_tr);                                   /* Q16 in, Q16 out */
        int32_t y = yp - dc; dc += y >> 10;                                   /* AC coupling: 14 Hz high-pass at 88.2 kHz */
        y = SHL2S(FM_HLP(y, mufb));                                              /* make-up: the bass the resonance takes */
        if (with_sat) {
            y = sclipk(ADDS(SHL3S(FM_HLP(y, sgla)), sb), invk);                  /* saturation: harder, off centre */
            y -= dc2; dc2 += y >> 10;                                         /* its DC out */
            lp += FM_HH(y - lp, sgla); y = lp;                                  /* its bandwidth */
            b[n] = (int16_t)(SHL3S(FM_HH(y, gxog)) >> 16);
        } else b[n] = (int16_t)(SHL3S(FM_HH(sclipk(y, invk), gxog)) >> 16);   /* soft output stage, x og, 16 bits */
    }
    lad->s1 = l.s1; lad->s2 = l.s2; lad->s3 = l.s3; lad->s4 = l.s4; *pyp = yp; pdc[0] = dc; if (with_sat) { pdc[1] = dc2; pdc[2] = lp; }
}
/* kennel: on the Blackfin the sixteen loops are moai_fc.S (gen_fc.py), bit for bit the C above. MM_FC_C builds the
 * C instead; MM_FC_REF builds both, the C as ref_<name> (harness/tier2/moai_fc_guest.c compares them). */
#ifdef MM_FC_REF
#define FC_C(name) ref_##name
#else
#define FC_C(name) name
#endif
#if defined(__bfin__) && !defined(MM_FC_C)
#define FC_ASM(name) void name(int16_t *b, lad_t *lad, const lad_coef_t *lc, int32_t gxog, int32_t fb, const sat_t *st, \
        int32_t *pyp, int32_t *pdc);
#else
#define FC_ASM(name)
#endif
#if !defined(__bfin__) || defined(MM_FC_C) || defined(MM_FC_REF)
#define FC_DEF(name, with_fb, with_res, with_sat, with_tr, where) \
    static void __attribute__((noinline)) MM_LINE where FC_C(name)(int16_t *b, lad_t *lad, const lad_coef_t *lc, int32_t gxog, int32_t fb, \
        const sat_t *st, int32_t *pyp, int32_t *pdc) { filter_chunk_t(b, lad, lc, gxog, fb, st, pyp, pdc, with_fb, with_res, with_sat, with_tr); }
#else
#define FC_DEF(name, with_fb, with_res, with_sat, with_tr, where)
#endif
#define FILTER_CHUNK(name, with_fb, with_res, with_sat, with_tr, where) \
    FC_ASM(name) FC_DEF(name, with_fb, with_res, with_sat, with_tr, where)
FILTER_CHUNK(fc_plain, 0, 0, 0, 0, )
FILTER_CHUNK(fc_sat, 0, 0, 1, 0, )
FILTER_CHUNK(fc_res, 0, 1, 0, 0, )
FILTER_CHUNK(fc_res_sat, 0, 1, 1, 0, )
FILTER_CHUNK(fc_fb, 1, 0, 0, 0, )
FILTER_CHUNK(fc_fb_sat, 1, 0, 1, 0, )
FILTER_CHUNK(fc_fb_res, 1, 1, 0, 0, )
FILTER_CHUNK(fc_fb_res_sat, 1, 1, 1, 0, )
FILTER_CHUNK(fc_tr, 0, 0, 0, 1, MM_TR)
FILTER_CHUNK(fc_sat_tr, 0, 0, 1, 1, MM_TR)
FILTER_CHUNK(fc_res_tr, 0, 1, 0, 1, MM_TR)
FILTER_CHUNK(fc_res_sat_tr, 0, 1, 1, 1, MM_TR)
FILTER_CHUNK(fc_fb_tr, 1, 0, 0, 1, MM_TR)
FILTER_CHUNK(fc_fb_sat_tr, 1, 0, 1, 1, MM_TR)
FILTER_CHUNK(fc_fb_res_tr, 1, 1, 0, 1, MM_TR)
FILTER_CHUNK(fc_fb_res_sat_tr, 1, 1, 1, 1, MM_TR)
#undef FILTER_CHUNK

MM_COLD static void mm_note_off(void *self) { V[vidx(self)].stage = 2; }

MM_LINE static void mm_render(void *self, int16_t *out, const int16_t *params)
{
    int vi = vidx(self);
    mm_voice_t *v = &V[vi];
    const int32_t *inc = OP1_PITCH_INC;
    int32_t vel = vel_gain(vi);                                   /* Q15 */
    PROF(0);

    /* ---- this block's parameters: the stored words, P[], and the LFO on the visible view's knobs ----
     * The LFO (and MIDI CC) act on the knob words the firmware passes: on the home view they are the home knobs
     * themselves; on another view the knobs are borrowed, so their offset from the live words goes to the
     * parameters that view's knobs edit. */
    int16_t q[P_N];
    const int16_t *lw = live_words();
    int home = mm_view == V_HOME;
    for (int i = 0; i < P_N; i++) q[i] = P[i];
    int32_t lo[4];                                                /* the LFO offsets, LFO_GAIN x (not the sound number) */
    for (int k = 0; k < 4; k++) {
        int32_t o = params[k] - lw[k];
        if (home ? k : mm_map[k] != P_SETUP) o *= LFO_GAIN;
        lo[k] = o > 32767 ? 32767 : (o < -32767 ? -32767 : o);
        mm_lfo[k] = (int16_t)lo[k];
    }
    static uint8_t beat_seen, idle;                                 /* blocks since the screen's last frame */
    if (mm_ui_beat != beat_seen) { beat_seen = mm_ui_beat; idle = 0; } else if (idle < 255) idle++;
    const int hold = home && mm_home_hold && idle < 64;            /* home + Shift on screen (moai.h): P's words */
    const int16_t *hw = hold ? P : lw;
    if (home) for (int i = 0; i < 4; i++) q[i] = (int16_t)clampk(hw[i] + lo[i]);
    for (int i = 0; i < 3; i++) q[P_DETUNE + i] = (int16_t)clampk(params[5 + i]);
    mm_lfo_seq++;
    int32_t base_setup = home ? hw[0] : P[P_SETUP], base_contour = home ? hw[3] : P[P_CONTOUR];
    sync_derived(base_setup, base_contour);
    for (int i = 0; i < P_N; i++) if (i >= P_O1_RANGE) q[i] = P[i];          /* (re)derived values */
    if (!home)
        for (int k = 0; k < 4; k++) {
            int id = mm_map[k];
            if (id >= 0 && lo[k]) q[id] = (int16_t)pclamp(id, q[id] + poffset(id, lo[k]));
        }
    int e0 = entry_of(base_setup), e = e0;
    if (q[P_SETUP] != clampk(base_setup)) {                                     /* LFO on the setup: an entry per 1024 */
        e = entry_step(e, (q[P_SETUP] - clampk(base_setup) + 512) >> 10);
        if (e != e0) derive_osc(q, e);
    }
    if (q[P_CONTOUR] != base_contour || (e != e0 && (preset_of(e) || preset_of(e0))))
        derive_contour(q, q[P_CONTOUR], e);                                     /* LFO on the contour (or a preset's) */

    PROF(1);
    /* ---- CPU guard: osc 3 off while many voices sound ---- */
    v->serial = ++mm_serial;
    int nv = sounding_voices();
    if (nv >= OSC3_OFF_AT) {
        mm_osc3_off = 1;
        int uni = 1;                                              /* unison: the voices within 1/32 (~1/2 semitone) */
        uint32_t ki = (uint32_t)inc[0];
        for (int i = 0; i < OP1_VOICES; i++)
            if (i != vi && !V[i].fresh && V[i].serial && mm_serial - V[i].serial < OP1_VOICES) {
                uint32_t d = V[i].key_inc > ki ? V[i].key_inc - ki : ki - V[i].key_inc;
                if (d > (ki >> 5)) uni = 0;
            }
        mm_osc3_why = uni ? 2 : 1;
    } else if (nv <= OSC3_ON_AT) mm_osc3_off = 0;
    int32_t g3t = mm_osc3_off ? 0 : 32767;
    if (v->fresh) v->g3 = g3t;                                    /* a new note starts as it should be */
    else v->g3 = v->g3 < g3t ? (v->g3 + 2048 > g3t ? g3t : v->g3 + 2048) : (v->g3 - 2048 < g3t ? g3t : v->g3 - 2048);   /* 23 ms */

    /* ---- oscillators and noise: one pass each into out[] (half scale: all of them stay in 16 bits) ---- */
    int32_t dk = detune_q16(q[P_DETUNE]), dr = drift_amount(q[P_DRIFT]);
    if ((v->dtick ^= 1))                                          /* every other block: the design's 2.9 ms step */
        for (int i = 0; i < 3; i++) {                             /* drift: leaky random walk (~2 cents at 8192), ~1.5 s */
            int32_t d = v->drift[i];
            d += ((((int32_t)((rnd(&v->rng) >> 27) & 15) - 8) * dr) >> 13) - (d >> 9);
            v->drift[i] = d;
        }
    int32_t i0 = inc[0], di = (inc[OP1_BLOCK - 1] - inc[0]) >> 7;  /* pitch: a linear ramp over the block */
    if ((uint32_t)i0 != v->key_inc) {                             /* a new key (or a bend): its reciprocal and octave */
        v->key_inc = (uint32_t)i0; v->key_rcp = rcp30((uint32_t)i0); v->key_oct = pitch_oct((uint32_t)i0);
    }
    const int32_t qd[3] = { v->drift[0], dk + v->drift[1], -(dk >> 1) + v->drift[2] };

    /* ---- modulation: the source at the four chunks and at the block's end (osc 3's phase before its pass) ---- */
    int32_t mod[5] = { 0, 0, 0, 0, 0 }, mpd = mod_depth(q[P_MOD_PITCH], MOD_PITCH_MAX), mfd = mod_depth(q[P_MOD_FILT], MOD_FILT_MAX);
    oscp_t m3;
    int lo3 = 0;
    if (mpd || mfd) {
        int32_t mix = clampk(q[P_MOD_MIX]);
        lo3 = q[P_O3_RANGE] == R_LO && osc_prep(&m3, 2, q[P_O3_WAVE], R_LO, q[P_O3_FREQ], qd[2], 1, i0, di, v->key_rcp);
        int32_t n0 = 0, n1 = 0;
        for (int c = 0; c < 5; c++) {
            int32_t s = lo3 ? mod_shape(q[P_O3_WAVE], v->ph[2] + m3.inc * (uint32_t)(c * CHUNK)) : 0;
            if (mix > 64) {                                       /* noise: one value per two chunks (the design's 0.73 ms),
                                                                   * the first again at the end */
                int32_t n = c == 4 ? n0 : (c & 1) ? n1 : (n1 = (int32_t)(rnd(&v->rng) >> 16) - 32768);
                if (!c) n0 = n;
                s += (((n - s) >> 1) * mix) >> 14;
            }
            mod[c] = s;
        }
    }
    int32_t pi0 = i0, pdi = di;                                   /* oscillators 1 and 2: the pitch modulated */
    uint32_t prcp = v->key_rcp;
    if (mpd) {
        pi0 = cents_inc(i0, (mod[0] * mpd) >> 15);
        pdi = (cents_inc(i0 + di * OP1_BLOCK, (mod[4] * mpd) >> 15) - pi0) >> 7;
        prcp = rcp30((uint32_t)(pi0 + pdi * (OP1_BLOCK / 2)));
    }
    PROF(2);
    oscp_t os[3];
    int no = 0, wrote = 0;
    for (int i = 0; i < 3; i++) {
        int32_t lvl = q[P_O(i, 3)] >> 1;                          /* 0 .. 16383: a full saw = +-8192 */
        if (i == 2) lvl = (lvl * v->g3) >> 15;
        no += i < 2 ? osc_prep(&os[no], i, q[P_O(i, 1)], q[P_O(i, 0)], q[P_O(i, 2)], qd[i], lvl, pi0, pdi, prcp)
                    : osc_prep(&os[no], i, q[P_O(i, 1)], q[P_O(i, 0)], q[P_O(i, 2)], qd[i], lvl, i0, di, v->key_rcp);
    }
    if (lo3 && !(no && os[no - 1].k == 2))                        /* osc 3 silent: its phase runs on as the source */
        v->ph[2] += m3.inc * OP1_BLOCK + (uint32_t)m3.dinc * (OP1_BLOCK * (OP1_BLOCK - 1) / 2);
    for (int j = 0; j < no; wrote = 1) {                          /* two of the same kind share a pass */
        if (j + 1 < no && os[j].wv.kind == os[j + 1].wv.kind) { osc_pass2(out, v, &os[j], &os[j + 1], wrote); j += 2; }
        else { osc_pass(out, &os[j].wv, os[j].r, &v->ph[os[j].k], os[j].inc, os[j].dinc, os[j].lvl, &v->nrng, wrote); j++; }
    }
    if (q[P_NOISE] > 64) {
        wave_t nw; wave_of(&nw, W_NOISE);
        osc_pass(out, &nw, 0, &v->ph[0], 0, 0, q[P_NOISE] >> 1, &v->nrng, wrote);
        wrote = 1;
    }
    if (!wrote) for (int n = 0; n < OP1_BLOCK; n++) out[n] = 0;

    PROF(3);
    /* ---- filter / contour setup ---- */
    int32_t base = ((q[P_CUTOFF] * 10 * 1024) >> 15) + (v->key_oct >> TRACK_SHIFT); /* Q10 octaves above 20 Hz */
    envc_t ec; contour_coefs(&ec, q);
    int32_t depth = ((q[P_AMOUNT] >> 2) * (16384 + (vel >> 1))) >> 15;         /* Q10 octaves, velocity-scaled */
    int32_t k = emph_k(q[P_EMPH]);
    int32_t gx = (IN_LVL * drive_q12(q[P_DRIVE])) >> 14;                       /* mix -> ladder input / 4, Q15 */
    int32_t fb = (q[P_FEEDBACK] * 24576) >> 15;                                 /* output -> input, 0 .. 3 (Q15 / 4) */
    int32_t og = (OUT_GAIN * vel) >> 15;                                        /* Q12 = og / 8 in Q15 */
    og -= (og * ((q[P_FEEDBACK] * 19661) >> 15)) >> 15;                         /* feedback is loud: down to 0.4 (-8 dB) */
    sat_t st = { 0, 0, 0, (make_up(k) * MU_OUT) >> 12 };                       /* up to 3.9x: Q13 */
    int sat = q[P_SAT] > 64;
    if (sat) { sat_coef(&st, q[P_SAT]); og = (og * level_comp(SAT_COMP, q[P_SAT])) >> 15; }   /* saturation is loud */
    if (q[P_DRIVE] > 0) og = (og * level_comp(DRIVE_COMP, q[P_DRIVE])) >> 15;                 /* and so is drive */
    int tr = q[P_DRIVE] > DRIVE_TR ? 8 : 0;                                    /* driven: the transistor stages */

    lad_t lad = v->lad;
    int32_t pos = v->pos, yp = v->yprev, dc[3] = { v->dc[0], v->dc[1], v->dc[2] }, gxog = (gx & 0xFFFF) | (og << 16);
    for (int c = 0; c < OP1_BLOCK / CHUNK; c++) {
        /* contour */
        int32_t env = v->env;
        if (v->stage == 0) {
            env += ec.att;
            if (env >= (1 << 22)) { env = 1 << 22; v->stage = 1; }
        } else if (v->stage == 1) {
            env += (((ec.sus - env) >> 8) * ec.dcoef) >> 10;              /* MM_DEC: Q18 */
        } else {
            env -= ((env >> 8) * ec.rcoef) >> 10;
        }
        v->env = env;

        int32_t target = base + (((env >> 12) * depth) >> 10) + ((mod[c] * mfd) >> 15);
        target = target < 0 ? 0 : (target > MAX_POS ? MAX_POS : target);
        pos = v->fresh ? target : pos + ((target - pos) >> 3) + ((target - pos) >> 7);   /* 2.5 ms smoothing: no zipper */
        v->fresh = 0;
        int32_t gg = lerp_tab(MM_G, pos);
        lad_coef_t lc; lad_coef(&lc, gg, k);
        int trc = tr && gg < GG_TR ? 8 : 0;                         /* the transistor stages: driven, below GG_TR */
        if (trc && !v->tr) { lad.t[0] = stclip(lad.s1, lc.invk); lad.t[1] = stclip(lad.s2, lc.invk); lad.t[2] = stclip(lad.s3, lc.invk); lad.t[3] = stclip(lad.s4, lc.invk); }
        v->tr = (uint8_t)trc;

        int16_t *b = out + c * CHUNK;
        static void (*const FC[16])(int16_t *, lad_t *, const lad_coef_t *, int32_t, int32_t, const sat_t *, int32_t *, int32_t *) = {
            fc_plain, fc_sat, fc_res, fc_res_sat, fc_fb, fc_fb_sat, fc_fb_res, fc_fb_res_sat,
            fc_tr, fc_sat_tr, fc_res_tr, fc_res_sat_tr, fc_fb_tr, fc_fb_sat_tr, fc_fb_res_tr, fc_fb_res_sat_tr };
        FC[trc | (fb ? 4 : 0) | (lc.ggk >> 16 ? 2 : 0) | sat](b, &lad, &lc, gxog, fb, &st, &yp, dc);   /* k after lad_coef: 0 up in the open range too */
    }
    if (vi == mm_last) {                                          /* the scope: one voice, 64 word copies a block */
        int w = mm_scope_w, sh = mm_scope_sh, m = OP1_BLOCK >> sh;
        if (!sh && !((uintptr_t)out & 3) && !(w & (OP1_BLOCK - 1))) {   /* (32-bit accesses must be aligned) */
            uint32_t *d = (uint32_t *)(void *)&mm_scope[w];
            const uint32_t *o = (const uint32_t *)(const void *)out;
            for (int n = 0; n < OP1_BLOCK / 2; n++) d[n] = o[n];
        } else for (int n = 0; n < m; n++) mm_scope[(w + n) & (SCOPE_N - 1)] = out[n << sh];
        mm_scope_w = (uint16_t)((w + m) & (SCOPE_N - 1));
    }
    PROF(4);
    v->lad = lad;
    v->pos = pos; v->yprev = yp; v->dc[0] = dc[0]; v->dc[1] = dc[1]; v->dc[2] = dc[2];
}

#ifdef OP1_HOST
void mm_bench_sat(int v) { P[P_SAT] = (int16_t)v; }       /* the benchmark: a session value (tools/bench) */
void mm_bench_mod(int on)                                 /* ... and osc 3 in LO (silent) as a vibrato and a wah */
{
    P[P_O3_RANGE] = on ? R_LO : R_OFF; P[P_O3_WAVE] = W_TRI; P[P_O3_VOL] = 0;
    P[P_MOD_PITCH] = on ? 8000 : 0; P[P_MOD_FILT] = on ? 16000 : 0;
}
int mm_bench_preset(int p, int16_t *k)                     /* ... preset p's words, as turning to it writes them */
{
    if (p >= N_PRESETS) return 0;
    const preset_t *pr = &PRESETS[p];
    int16_t w[8] = { (int16_t)entry_word(N_SETUPS + p), (int16_t)pct(pr->cut), (int16_t)pct(pr->emp), (int16_t)pct(pr->con),
                     0x5301, (int16_t)(pr->det ? pct(pr->det) : 1), (int16_t)(pr->drv ? pct(pr->drv) : 1), 0 };
    for (int i = 0; i < 8; i++) k[i] = w[i];
    return 1;
}
#endif

const op1_engine_t ENGINE_moai = { mm_note_on, mm_render, mm_note_off,
    { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE } };   /* presses: view switching (mm_ui.h) */
