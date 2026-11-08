/* MBIRA: a thumb piano. Each note plucks a steel tine: four decaying partials (modal synthesis, sines with their own
 * decay) plus a short click, through a wooden box, with optional bottle-cap buzz.
 *   blue   STRIKE   soft thumb (dark, round) .. hard nail (bright click, strong upper partials)
 *   green  TINE     the tine's material: wood bar (1 : 4 : 9.8 : 17) .. steel tine (1 : 2 : 6.2 : 15.6, the kalimba's
 *                   weak octave from the box) .. glass (1 : 2.76 : 5.40 : 8.93, bell-like)
 *   white  BOX      the box resonance (a low band-pass about 220 Hz fed by the tine) and its thump
 *   red    BUZZ     bottle caps on a wire rattle against the tine while it swings wide (sizzle that follows the
 *                   fundamental and dies with it)
 * Page 2 (press the encoder; the words are saved with the sound, 0 = off / natural):
 *   blue   word 5  RING     how long the tine rings: 1.6 s (0) .. 8 s
 *   green  word 6  DOUBLE   a second tine tuned up to 12 cents sharp: slow beating, a chorus of two tines
 *   white  word 7  WAH      the player's hand over the sound hole: a band-pass sweeping 400 Hz .. 1.5 kHz at
 *                           5 Hz, depth = word
 * The partial decays and pitches are set once per block; amplitudes ramp across the block, so knob changes never
 * jump at the first sample. Notes above the tine's range lose the partials that would pass 19 kHz.
 * Rate (2026-11-03): a voice call is 128 samples at 88.2 kHz (689 calls/s) on R246, and every rate-bound constant
 * here is derived for that (it was written for 44.1 kHz / 344 calls/s, which halved every decay, put the box at 440 Hz
 * and the wah an octave high at 10 Hz). The click's low-pass is the design's too; its length still uses the 44.1 kHz
 * constants, 0.5 .. 2.5 ms (keeping the design would double the burst's samples per note; README "Rate (2026-11-03)").
 * Cost (the unit adds SDRAM stalls to bcore's count, so the budget is tight): the block is built in passes over a
 * buffer, two partials per pass; a partial 60 dB under the fundamental (or under -80 dBFS) is dropped for the rest of
 * the note; the divisions run only when TINE, RING, DOUBLE or the pitch change; the box runs at 11 kHz, the wah and the
 * buzz noise at 44.1 kHz (sample pairs), the buzz contact every 8 samples (the noise skipped while the caps are off the tine). A restrike resets
 * the voice's partials (no second set rings on). Coefficient and increment caches survive restrikes when their
 * control/pitch keys match, so the first render does not repeat unchanged divisions. On the Blackfin the hot loops are
 * in mbira_dsp.S (the partial passes, box sums and output, wah, buzz), bit-identical to the C loops here (the host
 * build's); the coefficient divisions use DIVS / DIVQ (div16) and a short long division (udiv15).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include "op1_plugin.h"
#include "mbira_noise.h"

#define NP 5                                  /* partials 0..3 + the double tine */
#define Q24 (1 << 24)

typedef struct {
    uint32_t ph[NP];                          /* partial phases */
    uint32_t pinc[NP];                        /* previous block's phase increments (ramped from) */
    int fresh;                                /* first block of a note: no ramp */
    int coeff_valid;                          /* cached across restrikes; keys below own invalidation */
    int32_t amp[NP];                          /* partial amplitudes, Q24 */
    int32_t click;                            /* click envelope, Q15 */
    uint32_t noise;                           /* the buzz's noise */
    int32_t npos;                             /* the click's place in MB_NOISE */
    int32_t clp, nprev, zlp;                  /* click low-pass, previous noise sample (buzz high-pass), buzz low-pass */
    int32_t blo, bbd, bo, bp;                 /* box band-pass, its last two outputs */
    int32_t wlo, wbd, wo, wp;                 /* wah band-pass, its last two outputs */
    uint32_t wph;                             /* wah LFO */
    int32_t box, buzz, wah, dmix;             /* previous block's mix levels, Q15 */
    int32_t level;                            /* screen: fundamental amplitude, Q15 */
    int32_t rat[NP], dk[NP];                  /* partial ratios (Q16) and decays per block (Q18) ... */
    int32_t kt, kring, kdbl, khz, knew;       /* ... for these TINE, RING, DOUBLE and pitch (knew: changed) */
    uint32_t inc[NP], kinc0;                  /* partial phase increments for this pitch */
} __attribute__((aligned(32))) voice_t;      /* whole cache lines: a voice's state is 8 lines, the six adjacent */

/* the live partials of one block: phase pointer, increment (gliding by d per sample), amplitude (Q24) and its step */
typedef struct { uint32_t *ph, c; int32_t d, a, st; } part_t;
/* scratch of one render (the audio thread only, one voice at a time), off the stack: the block, a pass's gains per
 * group, the gliding increments, the box's interpolation per group, the wah's band-pass outputs, the buzz contact per
 * group, and the per-block values the C works with (the live partials, their steps and ends, the asm kernels'
 * state words); in SDRAM each stage touch()es the lines it writes first */
typedef struct {
    uint32_t gp[OP1_BLOCK / 8];                            /* before acc: the passes write back the word in front */
    int32_t acc[OP1_BLOCK];                                /* of acc unchanged, so it is ours (gp[15]) */
#ifdef __bfin__
    uint32_t ia[OP1_BLOCK + 8], ib[OP1_BLOCK + 8];        /* 129 words used; whole lines */
    int32_t bd[OP1_BLOCK / 4], ww[OP1_BLOCK / 2 + 8], gzs[OP1_BLOCK / 8];
#endif
    part_t live[NP];
    int32_t end[NP], step[NP], dinc[NP];
    int32_t st[3], gin[8];
} __attribute__((aligned(32))) scratch_t;
/* Everything a render reads and writes besides the sine table, in one place in the data cache: the six voices and
 * the scratch, 124 lines, put at run time where they share no cache set with the table's first quadrant. The OP-1's
 * data cache is 2-way with 256 sets of 32 bytes (set = address bits 5..12); the quadrant (0x0101E5E8, 4098 bytes) has
 * sets 47..175, so these start at set MB_SET0 = 176 and end at set 43, and the table, the voices and the scratch never
 * evict each other whatever address the loader gives the plugin (the stack and R246 take the other way). The pool
 * costs up to 8 KB of arena for that (sized for an image base that is only word-aligned).
 * With the kennel core's L1 pool (op1_l1.h, OP1_L1_API 2) the same hot_t lives in OP1_L1_SYNTH instead: the voices
 * are the persistent state (op1_l1_claim_state: the core copies them in from mb_pool's hot_t, their home, and back
 * when another engine takes the partition or the pool goes off), the scratch follows them. note_on and render call
 * mb_claim() first and reach everything through the pointer it returns, for that call only; note_off and the screen
 * never touch hot_t (the screen reads mb_level / mb_click / mb_pitch). Same samples either way. */
typedef struct { voice_t v[OP1_VOICES]; scratch_t s; } hot_t;
_Static_assert(__builtin_offsetof(voice_t, bp) - __builtin_offsetof(voice_t, blo) == 12, "mb_box's state: blo, bbd, bo, bp");
#define MB_SET0 176
#ifdef __bfin__
_Static_assert(sizeof(hot_t) <= (256 - 129) * 32, "hot_t must fit in the sets the quadrant leaves");
_Static_assert(__builtin_offsetof(hot_t, s) == sizeof(voice_t) * OP1_VOICES && sizeof(hot_t) <= 8192,
               "the voices (state) first, the scratch after them, all in OP1_L1_SYNTH");
#endif
/* The code of a voice call (render, note_on, their helpers) in one section, .text.mb_hot, so the lines a call runs
 * lie together; what a call runs only when a knob or the pitch changes (the coefficients, the partials' increments)
 * and the processing that is off at the defaults (wah, buzz) out of line in .text.mb_rare (the packer's merge.ld
 * keeps .text.* in each object's order). */
#ifdef __bfin__
#define MB_HOT __attribute__((section(".text.mb_hot")))
#define MB_RARE __attribute__((section(".text.mb_rare"), noinline))
#else
#define MB_HOT
#define MB_RARE __attribute__((noinline))
#endif
static uint8_t mb_pool[sizeof(hot_t) + 8192 - 4] __attribute__((aligned(32)));   /* -4: the image base may be only word-aligned */
static hot_t *mb_home_at;
static inline hot_t *mb_home(void)
{
    hot_t *h = mb_home_at;
    if (!h) h = mb_home_at = (hot_t *)(void *)(mb_pool + ((MB_SET0 * 32u - (uint32_t)(uintptr_t)mb_pool) & 8191u));
    return h;
}
/* In the pool, the click's noise table (MB_NOISE, 2 KB) follows hot_t: the strike's burst reads 256 bytes of it per
 * voice, 48 cache lines when six notes start in one block. Copied in whenever the partition is fresh to us. */
#define MB_L1_BYTES (sizeof(hot_t) + sizeof MB_NOISE)
#ifdef __bfin__
_Static_assert(sizeof(hot_t) % 32 == 0 && MB_L1_BYTES <= 8192, "the noise after hot_t, in OP1_L1_SYNTH");
#endif
static MB_RARE void noise_in(int32_t *d)
{
    const int32_t *q = (const int32_t *)(const void *)MB_NOISE;
    for (unsigned i = 0; i < sizeof MB_NOISE / 4; i++) d[i] = q[i];
}
/* where this call works: the L1 pool's copy, or the home when there is no pool / another engine still holds it */
static inline hot_t *mb_claim(void)
{
    hot_t *h = mb_home();
#if defined(__bfin__)
    int fresh;                                 /* the scratch is written before it is read; the noise is copied in */
    hot_t *l = op1_l1_claim_state(OP1_L1_SYNTH, h, sizeof h->v, MB_L1_BYTES, &fresh);
    if (l) {
        h = l;
        if (fresh) noise_in((int32_t *)(void *)(l + 1));
    }
#endif
    return h;
}
/* the click's noise: the L1 copy after hot_t while this call works in the pool, else MB_NOISE */
static inline const int16_t *mb_noise(hot_t *h, int sdram) { return sdram ? MB_NOISE : (const int16_t *)(void *)(h + 1); }
#ifndef __bfin__
#define voices (mb_home()->v)                  /* host harnesses (mbira_restrike_test.c): always the home there */
#endif
/* the render's globals in one place (one base address, two cache lines): the sine table it reads (below) and the
 * screen's mirrors (written by render only; the screen reads mb_level) */
static struct {
    const int16_t *tab;
    volatile int32_t click[OP1_VOICES], level[OP1_VOICES];
    volatile uint32_t pitch[OP1_VOICES], blocks;
} mb_g __attribute__((aligned(32)));
#define mb_tab (mb_g.tab)
#define mb_click (mb_g.click)
#define mb_level (mb_g.level)
#define mb_pitch (mb_g.pitch)
#define mb_blocks (mb_g.blocks)

/* partial ratios (Q16) and levels (Q15) of the three tines: wood, steel, glass */
static const int32_t RATIO[3][4] = {
    { 65536, 262144, 642253, 1114112 },
    { 65536, 131072, 406323, 1022362 },
    { 65536, 180879, 353894, 585236 },
};
static const int32_t LEVEL[3][4] = {
    { 32767, 9830, 4915, 1638 },
    { 32767, 5243, 13107, 4915 },
    { 32767, 16384, 9830, 6554 },
};

static inline int32_t clampi(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }
/* op1_sat16 as min / max: gcc turns op1_sat16 of a product into compares and branches */
static inline int16_t sat16(int32_t x) { return (int16_t)clampi(x, -32768, 32767); }
static inline int32_t knob(const int16_t *p, int i) { return p[i] < 0 ? 0 : p[i]; }
/* op1_voice_index without its division by 36: the voice objects are 36 bytes apart, and (36 i x 1821) >> 16 = i for
 * i = 0 .. 5 (65556 i < 65536 (i + 1)) */
static inline int vindex(const void *self)
{
#ifdef __bfin__
    return (int)((((uint32_t)(uintptr_t)self - OP1_VOICE_BASE) * 1821u) >> 16);
#else
    return op1_voice_index(self);
#endif
}
/* op1_voice_vel_gain for voice vi */
static inline int32_t vel_gain(int vi)
{
    const int32_t v = OP1_VOICE_VEL[vi] >> 8;
    return v > 0 && v <= 127 ? v * 258 : 32767;
}
/* sin(ph) = OP1_SINE_TABLE[ph >> 19], read from the table's first quadrant only (entries 0..2048, 4 KB of the 16 KB):
 * the table is lrint(32767 sin(2 pi k / 8192)) on the unit, exactly symmetric (s[4096 + k] = -s[k], s[2048 + i] =
 * s[2048 - i]), so this is the same value; a block's partials then touch 129 cache lines of it, not all 512.
 * mb_tab: those 2,049 entries, set at each render: the core's L1 copy (op1_l1_sine_quarter, one cycle, no cache) or
 * OP1_SINE_TABLE itself (the same numbers); mbira_dsp.S gets it as an argument */
static inline const int16_t *sine_tab(void)
{
#if defined(__bfin__)
    const int16_t *q = op1_l1_sine_quarter();
    if (q) return q;
#endif
    return OP1_SINE_TABLE;
}
static inline int32_t sine(uint32_t ph)
{
    const int32_t k = (int32_t)(ph << 1) >> 20;                            /* -2048 .. 2047 around the half-wave's peak */
    const int32_t v = mb_tab[k < 0 ? -k : k];
    return (int32_t)ph < 0 ? -v : v;
}
static inline uint32_t lcg(uint32_t *s) { return *s = *s * 1664525u + 1013904223u; }
/* read one word of each 32-byte line of [p, p + bytes) before writing there: the OP-1's data cache does not allocate
 * a line on a write miss, so writing first sends every store to SDRAM on its own (and the line is filled anyway at
 * the first read after it). Not needed in L1 (no cache there): the callers skip it when they work in the pool. */
static inline void touch(const void *p, int bytes)
{
    for (const volatile int32_t *q = p; bytes > 0; q += 8, bytes -= 32) (void)*q;
}
/* a 16 x 16 product (both operands within int16), as one Blackfin multiply with no sign extensions around it */
static inline int32_t mul16(int32_t a, int32_t b)
{
#ifdef __bfin__
    int32_t r;
    __asm__("%0 = %1.L * %2.L (IS);" : "=d"(r) : "d"(a), "d"(b));
    return r;
#else
    return (int16_t)a * (int16_t)b;
#endif
}

/* tine morph: TINE 0..32767 across wood -> steel -> glass */
static void tine(int32_t t, int k, int32_t *ratio, int32_t *level)
{
    const int a = t < 16384 ? 0 : 1;
    const int32_t f = t < 16384 ? t * 2 : (t - 16384) * 2;                 /* 0..32767 within the pair */
    /* Exact signed Q15 product, decomposed to avoid the 64-bit helper. */
    const int32_t d=RATIO[a+1][k]-RATIO[a][k];
    *ratio = RATIO[a][k] + (d>>15)*f + (((d&32767)*f)>>15);
    *level = LEVEL[a][k] + (((LEVEL[a + 1][k] - LEVEL[a][k]) * f) >> 15);
}

/* exact quotients for the coefficient divisions, faster than libgcc's 32-step __divsi3 (about 180 packets):
 * div16: n / d for 0 <= n < 2^30, 0 < d < 32768 and a quotient under 32768, with the Blackfin divide primitive
 * (DIVS + 15 DIVQ, the 32 / 16 integer division of the programming reference); udiv15: n / d for a quotient under
 * 32768 (any d), 15 steps of long division */
static inline int32_t div16(int32_t n, int32_t d)
{
#ifdef __bfin__
    int32_t q = n << 1;
    __asm__("DIVS(%0, %1);\n\t"
            "DIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\t"
            "DIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\t"
            "DIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\t"
            "%0 = %0.L (Z);" : "+d"(q) : "d"(d) : "ASTAT");
    return q;
#else
    return n / d;
#endif
}
static inline int32_t udiv15(uint32_t n, uint32_t d)
{
    uint32_t q = 0;
    for (int b = 14; b >= 0; b--)
        if ((n >> b) >= d) { n -= d << b; q += 1u << b; }
    return (int32_t)q;
}

/* a partial's phase increment: ((uint64_t)x * r) >> 16 for a ratio r < 2^21 (Q16), 0 past 0x38000000 (19.3 kHz),
 * from 16-bit halves without libgcc's 64-bit multiply: x r >> 16 = a rh 2^16 + a rl + b rh + (b rl >> 16) */
static inline uint32_t incmul(uint32_t x, uint32_t r)
{
    const uint32_t a = x >> 16, b = x & 0xFFFF, rh = r >> 16, rl = r & 0xFFFF;
    const uint32_t ah = a * rh;                                             /* < 2^21 */
    if (ah > 0x3800) return 0;
    const uint32_t t = a * rl;                                              /* < 2^32 */
    if (t > 0x38000000u) return 0;
    const uint32_t s = (ah << 16) + t + b * rh + ((b * rl) >> 16);           /* <= 0x70000000 + 2^22: no wrap */
    return s > 0x38000000u ? 0 : s;
}

/* per-block decay for a decay time tau in ms (amplitude falls by e over tau): the fraction a block takes off, Q18,
 * 128 / (tau * 88.2) rounded (a block is 1.451 ms; tau within 1.3 % at its 10,000 ms, 0.1 % at 1,600); tau stays under
 * 10,100 */
static inline int32_t decay(int32_t tau_ms)
{
    if (tau_ms < 20) tau_ms = 20;
    return div16(380432 + (tau_ms >> 1), tau_ms);                          /* 2^18 x 128 / 88.2 = 380,432 */
}

static MB_HOT void mb_note_on(void *self, const int16_t *p)
{
    hot_t *const h = mb_claim();
    const int vi = vindex(self);
    voice_t *v = &h->v[vi];
    if (h == mb_home_at) touch(v, sizeof *v);                              /* the strike writes most of it */
    const int32_t strike = knob(p, 0), t = knob(p, 1);
    const int32_t bright = 6554 + ((strike * 26214) >> 15);               /* 0.2 .. 1.0 */
    for (int k = 0; k < 4; k++) {
        /* note_on needs the level only; avoid calculating an unused ratio. */
        const int bank=t<16384?0:1;
        const int32_t f=t<16384?t*2:(t-16384)*2;
        int32_t a = LEVEL[bank][k] + (((LEVEL[bank+1][k]-LEVEL[bank][k])*f)>>15);
        for (int j = 0; j < k; j++) a = (a * bright) >> 15;               /* upper partials follow the strike */
        /* with the double tine or the wah on (the costly voices), an upper partial that would start 30 dB or more
         * under the fundamental (32767) is not started (it would fade over about a second) */
        if (k && a < 1036 && (p[6] > 0 || p[7] > 0)) a = 0;
        v->amp[k] = a << 9;
        v->ph[k] = 0;
    }
    v->amp[4] = (v->amp[0] >> 4) * 11;                                     /* the double tine, 0.69 of the first */
    v->ph[4] = 0x40000000u;
    v->fresh = 1;
    v->click = 32767;
    v->noise = 0x9E3779B9u ^ ((uint32_t)vi * 0x85EBCA6Bu);
    v->npos = (vi * 167) & (MB_NOISE_LEN - 1);                             /* each voice its own stretch of noise */
    v->clp = v->nprev = v->zlp = 0;
    v->blo = v->bbd = v->bo = v->bp = v->wlo = v->wbd = v->wo = v->wp = 0;
    v->box = knob(p, 2);
    v->buzz = knob(p, 3);
    v->wah = p[7] < 0 ? 0 : p[7];
    v->dmix = p[6] > 0 ? 11469 + ((p[6] * 4000) >> 15) : 0;
}

static void mb_note_off(void *self) { (void)self; }   /* the firmware's release envelope ends the note */

/* Share a small, bounded history across voice slots: fast melodies often
 * reuse a pitch on a different slot. The voice-local fast path stays unchanged. */
#define COEFF_CACHE 16
/* the keys apart from the values: a miss (every block while TINE or RING moves) scans 10 cache lines, not 30 */
static struct {int valid;int32_t t,ring,dbl,hz;} coeff_cache[COEFF_CACHE] __attribute__((aligned(32)));
static struct {int32_t rat[NP],dk[NP];} coeff_val[COEFF_CACHE];
static unsigned coeff_next;

/* the partials' ratios and per-block decay factors, recomputed only when TINE, RING, DOUBLE or (above 262 Hz, to
 * 8 Hz) the pitch changes, so the divisions stay out of the steady state: coeffs() is the voice's own check, this
 * the rest (out of line) */
static MB_RARE void coeffs_new(voice_t *v, int32_t t, int32_t ring, int32_t dbl, int32_t hk)
{
    v->coeff_valid = 1;
    v->kt = t; v->kring = ring; v->kdbl = dbl; v->khz = hk; v->knew = 1;
    for(unsigned i=0;i<COEFF_CACHE;i++){
        if(coeff_cache[i].valid && coeff_cache[i].t==t && coeff_cache[i].ring==ring &&
           coeff_cache[i].dbl==dbl && coeff_cache[i].hz==hk){
            for(int k=0;k<NP;k++){v->rat[k]=coeff_val[i].rat[k];v->dk[k]=coeff_val[i].dk[k];}
            return;
        }
    }
    int32_t tau0 = 1600 + ((ring * 6400) >> 15);                           /* ms: 1.6 .. 8 s */
    if (hk)                                                                 /* high tines ring shorter */
        tau0 = (hk < 32768 ? div16(tau0 * 262, hk) : (tau0 * 262) / hk) + tau0 / 4;
    const int32_t d0 = decay(tau0);
    for (int k = 0; k < NP; k++) {
        int32_t r, l;
        tine(t, k < 4 ? k : 0, &r, &l);
        if (k == 4) r += (r >> 10) * ((dbl * 7) >> 15);                    /* up to +0.7 %: 12 cents */
        v->rat[k] = r;
        /* ~2 / (ratio + 1); the quotient is under 2 tau0 (r >= 1) */
        v->dk[k] = k == 0 || k == 4 ? d0 : decay(udiv15((uint32_t)tau0 * 131072u, (uint32_t)(r + 65536)) + 40);
    }
    unsigned slot=coeff_next++&(COEFF_CACHE-1);
    coeff_cache[slot].t=t;coeff_cache[slot].ring=ring;
    coeff_cache[slot].dbl=dbl;coeff_cache[slot].hz=hk;
    for(int k=0;k<NP;k++){coeff_val[slot].rat[k]=v->rat[k];coeff_val[slot].dk[k]=v->dk[k];}
    coeff_cache[slot].valid=1;
}
static inline void coeffs(voice_t *v, int32_t t, int32_t ring, int32_t dbl, int32_t hz)
{
    const int32_t hk = hz > 262 ? hz & ~7 : 0;
    if (__builtin_expect(v->coeff_valid && !((t ^ v->kt) | (ring ^ v->kring) | (dbl ^ v->kdbl) | (hk ^ v->khz)), 1))
        return;
    coeffs_new(v, t, ring, dbl, hk);
}
/* the partials' pitches, when the pitch or the ratios change */
static MB_RARE void pitches(voice_t *v, uint32_t inc0)
{
    v->kinc0 = inc0;
    v->knew = 0;
    for (int k = 0; k < NP; k++) v->inc[k] = incmul(inc0, (uint32_t)v->rat[k]);   /* past ~19 kHz: silent */
}

/* the live partials into the block, two per pass (one load and store of the block for both): phases gliding to the
 * new pitch (only when it moved), amplitudes ramping (Q24) in steps of 8 samples (a step is under 0.4 % of the level,
 * even for the fastest decay), each at half level (four partials: headroom); the first
 * pass writes the block, the others add */
/* the per-8 gains of a partial (Q14 in a pair, Q15 alone), worked out before the sample loop so it carries only the
 * phases, increments and the block pointer */
static inline void gains(int32_t *g, const part_t *x, int sh)
{
    int32_t a = x->a;
    const int32_t s8 = x->st * 8;
    for (int i = 0; i < OP1_BLOCK / 8; i++) { g[i] = a >> sh; a += s8; }
}
#ifdef __bfin__
/* mbira_dsp.S: the same passes for a steady pitch, scheduled by hand */
void mb_pass2(int32_t *acc, const uint32_t *gains, uint32_t *ph, uint32_t ca, uint32_t cb, int first, const int16_t *tab);
void mb_pass1(int32_t *acc, const uint32_t *gains, uint32_t *ph, uint32_t ca, int first, const int16_t *tab);
void mb_pass2g(int32_t *acc, const uint32_t *gains, uint32_t *ph, const uint32_t *ia, const uint32_t *ib, int first,
               const int16_t *tab);
void mb_pass1g(int32_t *acc, const uint32_t *gains, uint32_t *ph, const uint32_t *ia, int first, const int16_t *tab);
void mb_ramp(uint32_t *dst, uint32_t c, int32_t d);
void mb_box(const int32_t *acc, int32_t *bd, int32_t *st, int32_t bx, int32_t dbx8);
void mb_out(const int32_t *acc, int16_t *out, const int32_t *bd, int32_t g);
void mb_boxadd(int32_t *acc, const int32_t *bd);
void mb_buzz(int32_t *acc, const int32_t *gz, uint32_t *st);
void mb_gz(int32_t *gz, const int32_t *in);
int mb_burst(int32_t *acc, int32_t *st, int32_t cfa, const int16_t *noise);
void mb_wah1(const int32_t *acc, int32_t *ww, int32_t *st, int32_t wf);
void mb_wah2(const int32_t *acc, int16_t *out, const int32_t *ww, int32_t w, int32_t dw2, int32_t g);

/* the passes over the live list: a pair while two are left, else one (the first pass writes the block) */
static inline void partials(scratch_t *s, part_t *l, int n, int glide, int sdram)
{
    if (glide && sdram) { touch(s->ia, sizeof s->ia); touch(s->ib, sizeof s->ib); }
    uint32_t *const gp = s->gp;
    for (int i = 0; i < n; i += 2) {
        const int first = i == 0;
        part_t *const x = &l[i];
        if (i + 1 < n) {
            part_t *const y = &l[i + 1];
            uint32_t ph[2] = { *x->ph, *y->ph };
            int32_t a = x->a, b = y->a;                                     /* the gains as gains() makes them */
            const int32_t sa = x->st * 8, sb = y->st * 8;
            for (int j = 0; j < OP1_BLOCK / 8; j++) { gp[j] = (uint32_t)(b >> 10) << 16 | (uint32_t)(a >> 10); a += sa; b += sb; }
            if (__builtin_expect(glide, 0)) {
                mb_ramp(s->ia, x->c, x->d);
                mb_ramp(s->ib, y->c, y->d);
                mb_pass2g(s->acc, gp, ph, s->ia, s->ib, first, mb_tab);
            } else
                mb_pass2(s->acc, gp, ph, x->c, y->c, first, mb_tab);
            *x->ph = ph[0];
            *y->ph = ph[1];
        } else {
            uint32_t ph = *x->ph;
            gains((int32_t *)gp, x, 9);
            if (__builtin_expect(glide, 0)) {
                mb_ramp(s->ia, x->c, x->d);
                mb_pass1g(s->acc, gp, &ph, s->ia, first, mb_tab);
            } else
                mb_pass1(s->acc, gp, &ph, x->c, first, mb_tab);
            *x->ph = ph;
        }
    }
}
#else
static inline void pass2(int32_t *acc, part_t *x, part_t *y, int first, int glide)
{
    int32_t gx[OP1_BLOCK / 8], gy[OP1_BLOCK / 8];
    gains(gx, x, 10);                                                      /* Q14: the two products sum within 2^30 */
    gains(gy, y, 10);
    uint32_t pa = *x->ph, ca = x->c, pb = *y->ph, cb = y->c;
    const int32_t da = x->d, db = y->d;
    int32_t *q = acc;
    for (int i = 0; i < OP1_BLOCK / 8; i++) {
        const int32_t ga = gx[i], gb = gy[i];
        for (int j = 0; j < 8; j++) {
            pa += ca;
            pb += cb;
            if (glide) { ca += (uint32_t)da; cb += (uint32_t)db; }
            const int32_t s = (mul16(sine(pa), ga) + mul16(sine(pb), gb)) >> 15;
            if (first) *q = s; else *q += s;
            q++;
        }
    }
    *x->ph = pa;
    *y->ph = pb;
}
static inline void pass1(int32_t *acc, part_t *x, int first, int glide)
{
    int32_t gx[OP1_BLOCK / 8];
    gains(gx, x, 9);
    uint32_t pa = *x->ph, ca = x->c;
    const int32_t da = x->d;
    int32_t *q = acc;
    for (int i = 0; i < OP1_BLOCK / 8; i++) {
        const int32_t ga = gx[i];
        for (int j = 0; j < 8; j++) {
            pa += ca;
            if (glide) ca += (uint32_t)da;
            const int32_t s = mul16(sine(pa), ga) >> 16;
            if (first) *q = s; else *q += s;
            q++;
        }
    }
    *x->ph = pa;
}
static void partials(scratch_t *s, part_t *l, int n, int glide, int sdram)
{
    int32_t *const acc = s->acc;
    (void)sdram;
    for (int i = 0; i < n; i += 2) {
        const int first = i == 0;
        if (i + 1 < n) {
            if (glide) { if (first) pass2(acc, &l[i], &l[i + 1], 1, 1); else pass2(acc, &l[i], &l[i + 1], 0, 1); }
            else { if (first) pass2(acc, &l[i], &l[i + 1], 1, 0); else pass2(acc, &l[i], &l[i + 1], 0, 0); }
        } else {
            if (glide) { if (first) pass1(acc, &l[i], 1, 1); else pass1(acc, &l[i], 0, 1); }
            else { if (first) pass1(acc, &l[i], 1, 0); else pass1(acc, &l[i], 0, 0); }
        }
    }
}
#endif

/* click: noise burst through a low-pass, while it lasts; the noise from MB_NOISE (nz: its L1 copy or itself; the same
 * generator's output, read from the voice's place, so no generator runs per sample). Called while click > 8 or its low-pass is not yet 0 */
static MB_HOT __attribute__((noinline)) void click_run(voice_t *v, scratch_t *s, int32_t strike, const int16_t *nz)
{
    int32_t *const acc = s->acc;
    int32_t click = v->click, clp = v->clp;
    /* per sample: 1 ms (hard) .. 5 ms at 44.1 kHz, 0.5 .. 2.5 ms at the voice's 88.2 kHz (left so: README "Rate") */
    const int32_t cf = 32034 + (((32767 - strike) * 586) >> 15);
    /* cf >= 32034 is what mb_burst's untested runs rely on (click >= 24 stays > 8 for 16 samples, checked for every
     * cf from 32034 up: OP1-Emulator run/mbira-spike-2026-10-28/clickk.c): a faster click decay needs a new bound */
    /* click low-pass, Q15: the design's 1 - exp(-2 pi fc / 44100) = 3000 + 24000 STRIKE (674 Hz .. 12.2 kHz) as the same
     * fc at 88.2 kHz, 1 - sqrt(1 - a), at STRIKE = 4096 i, interpolated (within 0.7 %) */
    static const int16_t CLP_A[9] = { 1536, 3152, 4860, 6681, 8639, 10770, 13131, 15818, 19019 };
    const int32_t ci = strike >> 12;
    const int32_t clp_a = CLP_A[ci] + (((CLP_A[ci + 1] - CLP_A[ci]) * (strike & 4095)) >> 12);
    int i = 0;
    if (click > 8) {                                                        /* the burst, ending in the block */
#ifdef __bfin__
        int32_t *const st3 = s->st;
        st3[0] = click; st3[1] = clp; st3[2] = v->npos;
        i = mb_burst(acc, st3, clp_a << 16 | cf, nz);
        click = st3[0];
        clp = st3[1];
#else
        (void)s;
        for (; i < OP1_BLOCK && click > 8; i++) {
            const int32_t n = nz[(v->npos + i) & (MB_NOISE_LEN - 1)];
            clp += (((mul16(n, click) >> 15) - clp) * clp_a) >> 15;
            click = mul16(click, cf) >> 15;
            acc[i] += clp >> 1;
        }
#endif
        v->npos = (v->npos + i) & (MB_NOISE_LEN - 1);
    }
    for (; i < OP1_BLOCK; i++) { clp -= clp >> 4; acc[i] += clp >> 1; }   /* its tail */
    if (click <= 8 && clp > -16 && clp < 16) clp = 0;                       /* the tail's last LSBs: done */
    v->click = click;
    v->clp = clp;
}

/* buzz: band-passed noise (high-pass difference, one-pole low-pass at 4.9 kHz), one noise sample per sample pair
 * (44.1 kHz, as designed: the same filter per sample at 88.2 kHz would be 9..10 dB quieter in band), while the
 * fundamental swings past 70 %; the caps'
 * contact rises smoothly from 0 at 70 % to full at the peak, so the sizzle has no hard on / off edges; the contact
 * (with the tine's level and BUZZ) is set every 8 samples, and the noise skipped while it is 0 (half of each swing;
 * the filter's state then waits, which the contact's slow rise hides). ph, c: the fundamental's phase and increment
 * at the block's start, a its amplitude (before this block's step) */
static MB_RARE void buzz_run(voice_t *v, scratch_t *s, uint32_t ph, uint32_t c, int32_t a, int32_t bz, int32_t dbuzz,
                             int sdram)
{
    int32_t *const acc = s->acc;
    const int32_t dinc0 = s->dinc[0], step0 = s->step[0];
#ifdef __bfin__
    int32_t *const gzs = s->gzs;
    if (sdram) touch(gzs, sizeof s->gzs);
    int32_t *const gin = s->gin;
    gin[0] = (int32_t)ph; gin[1] = (int32_t)c; gin[2] = dinc0 << 3; gin[3] = a + step0; gin[4] = step0 * 8;
    gin[5] = bz; gin[6] = dbuzz * 8; gin[7] = (int32_t)(uintptr_t)mb_tab;
    mb_gz(gzs, gin);                                                        /* the contact per group, then mb_buzz */
    uint32_t *const st3 = (uint32_t *)s->st;
    st3[0] = v->noise; st3[1] = (uint32_t)v->nprev; st3[2] = (uint32_t)v->zlp;
    mb_buzz(acc, gzs, st3);
    v->noise = st3[0];
    v->nprev = (int32_t)st3[1];
    v->zlp = (int32_t)st3[2];
#else
    (void)sdram;
    uint32_t noise = v->noise;
    int32_t nprev = v->nprev, zlp = v->zlp;
    for (int i = 0; i < OP1_BLOCK; i += 8) {
        const int32_t sn = sine(ph + c), as = sn < 0 ? -sn : sn;
        const int32_t g = as > 22937 ? ((as - 22937) * 3) + ((as - 22937) >> 3) : 0;   /* 0 .. ~32700, Q15 */
        const int32_t gz = (((((a + step0) >> 10) * g) >> 15) * bz) >> 15;
        ph += c << 3;
        c += (uint32_t)dinc0 << 3;
        a += step0 * 8;
        bz += dbuzz * 8;
        if (!gz) continue;                                                  /* caps off the tine: silent */
        for (int j = i; j < i + 8; j += 2) {
            const int32_t n = (int32_t)lcg(&noise) >> 16;
            zlp += (((n - nprev) >> 1) - zlp) >> 1;
            const int32_t z = (zlp * gz) >> 15;
            acc[j] += z;
            acc[j + 1] += z;
            nprev = n;
        }
    }
    v->noise = noise;
    v->nprev = nprev;
    v->zlp = zlp;
#endif
}

/* wah: band-pass sweep, mixed in by depth; centre this block 400 Hz .. 1.5 kHz (Chamberlin 2 sin(pi f / 44100), Q13:
 * the pairs come at 44.1 kHz), run on sample pairs at half scale (the sum in the update then stays under 2^31 without a clamp), its output
 * interpolated between the last two (one pair late); then the voice's gain g into out. The box is in acc already */
static MB_RARE void wah_run(voice_t *v, scratch_t *s, int16_t *out, int32_t wah0, int32_t dwah, int32_t g, int sdram)
{
    const int32_t *const acc = s->acc;
    const int32_t wf = 467 + (((sine(v->wph) + 32768) * 1290) >> 16);     /* Q13: 400 .. 1,509 Hz within 0.3 % */
#ifdef __bfin__
    int32_t *const ww = s->ww, *const st2 = s->st;
    if (sdram) touch(ww, sizeof s->ww);
    st2[0] = v->wlo;
    st2[1] = v->wbd;
    ww[0] = v->wp;
    ww[1] = v->wo;
    mb_wah1(acc, ww, st2, wf);
    mb_wah2(acc, out, ww, wah0, dwah * 2, g);
    v->wlo = st2[0];
    v->wbd = st2[1];
    v->wp = ww[OP1_BLOCK / 2];
    v->wo = ww[OP1_BLOCK / 2 + 1];
#else
    (void)s; (void)sdram;
    int32_t wlo = v->wlo, wbd = v->wbd, wo = v->wo, wp = v->wp, w = wah0;
    for (int i = 0; i < OP1_BLOCK; i += 2) {
        const int32_t y0 = acc[i], y1 = acc[i + 1], wm = w >> 3;
        out[i] = sat16(((y0 + (((((wp + wo) >> 1) - y0) * wm) >> 12)) * g) >> 15);
        out[i + 1] = sat16(((y1 + (((wo - y1) * wm) >> 12)) * g) >> 15);
        wlo = clampi(wlo + ((wf * wbd) >> 13), -131071, 131071);
        wbd = clampi(wbd + ((wf * (((y0 + y1) >> 2) - wlo - ((2500 * wbd) >> 13))) >> 13), -131071, 131071);
        wp = wo;
        wo = wbd * 3;
        w += dwah * 2;
    }
    v->wlo = wlo; v->wbd = wbd; v->wo = wo; v->wp = wp;
#endif
}

static MB_HOT void mb_render_body(void *self, int16_t *out, const int16_t *p);
/* mbira_dsp.S zeroes L0..L3 (linear DAG addressing); the caller's values come back on return, as Gin's asm does */
static MB_HOT void mb_render(void *self, int16_t *out, const int16_t *p)
{
#ifdef __bfin__
    uint32_t l0, l1, l2, l3;
    __asm__ volatile("%0 = L0;\n\t%1 = L1;\n\t%2 = L2;\n\t%3 = L3;" : "=d"(l0), "=d"(l1), "=d"(l2), "=d"(l3));
    mb_render_body(self, out, p);
    __asm__ volatile("L0 = %0;\n\tL1 = %1;\n\tL2 = %2;\n\tL3 = %3;" : : "d"(l0), "d"(l1), "d"(l2), "d"(l3));
#else
    mb_render_body(self, out, p);
#endif
}

static MB_HOT __attribute__((noinline)) void mb_render_body(void *self, int16_t *out, const int16_t *p)
{
    hot_t *const h = mb_claim();
    const int sdram = h == mb_home_at;                                      /* not in the L1 pool: touch() first */
    scratch_t *const s = &h->s;
    const int vi = vindex(self);
    voice_t *const v = &h->v[vi];
    mb_tab = sine_tab();
    const int32_t strike = knob(p, 0), t = knob(p, 1), box = knob(p, 2), buzz = knob(p, 3);
    const int32_t ring = p[5] < 0 ? 0 : p[5], dbl = p[6] < 0 ? 0 : p[6], wah = p[7] < 0 ? 0 : p[7];
    const uint32_t inc0 = (uint32_t)OP1_PITCH_INC[0];
    coeffs(v, t, ring, dbl, (int32_t)(((inc0 >> 16) * 44100u) >> 15));                       /* Hz: inc0 = 2^32 f / 88200 */
    if (__builtin_expect((inc0 ^ v->kinc0) | v->knew, 0)) pitches(v, inc0);                    /* the partials' pitches, when they change */
    /* partial amplitude steps and pitch glides for this block */
    int32_t *const end = s->end, *const step = s->step, *const dinc = s->dinc;
    const int fresh = v->fresh;
    for (int k = 0; k < NP; k++) {
        const uint32_t ik = v->inc[k];
        if (!ik) v->amp[k] = 0;
        const int32_t a = v->amp[k];
        const int32_t e = a - (int32_t)((((uint32_t)a >> 8) * (uint32_t)v->dk[k]) >> 10);
        step[k] = (e - a) >> 7;
        end[k] = a + step[k] * OP1_BLOCK;
        if (fresh) v->pinc[k] = ik;                                         /* pitches glide across the block */
        dinc[k] = (int32_t)(ik - v->pinc[k]) >> 7;
    }
    /* mix levels ramp from the previous block's values. The strike's block (the note's first, the one that also runs
     * the click and the coefficients) leaves out the double tine and the buzz: both come in over the next block from 0
     * (a 1.45 ms fade starting 1.45 ms after the pluck), so six notes struck together cost less in that block */
    v->fresh = 0;
    const int32_t dmix1 = fresh ? 0 : dbl > 0 ? 11469 + ((dbl * 4000) >> 15) : 0;   /* double tine level, Q15 */
    const int32_t dmix0 = fresh ? 0 : v->dmix;
    const int32_t buzz1 = fresh ? 0 : buzz;
    const int32_t box0 = v->box, dbox = (box - box0) >> 7, buzz0 = fresh ? 0 : v->buzz;
    const int32_t wah0 = v->wah;

    /* the tine: partial 0 always (it writes the block), the others while they sound (a partial below -80 dBFS is
     * dropped for the rest of the note), the double tine scaled by its mix level */
    int32_t *const acc = s->acc;
    if (sdram) { touch(acc, sizeof s->acc); touch(s->gp, sizeof s->gp); }
    const uint32_t ph0 = v->ph[0], c0 = v->pinc[0];
    const int32_t amp0 = v->amp[0];
    part_t *const live = s->live;
    int n = 0, glide = 0;
    for (int k = 0; k < 4; k++)
        if (!k || v->amp[k]) {
            live[n++] = (part_t){ &v->ph[k], v->pinc[k], dinc[k], v->amp[k], step[k] };
            glide |= dinc[k];
        }
    if ((v->amp[4] | end[4]) && (dmix0 | dmix1)) {                        /* level x mix as one ramp */
        const int32_t a0 = ((v->amp[4] >> 8) * dmix0) >> 7, a1 = ((end[4] >> 8) * dmix1) >> 7;
        live[n++] = (part_t){ &v->ph[4], v->pinc[4], dinc[4], a0, (a1 - a0) >> 7 };
        glide |= dinc[4];
    } else v->ph[4] += v->pinc[4] * OP1_BLOCK + (uint32_t)dinc[4] * (OP1_BLOCK * (OP1_BLOCK - 1) / 2);
    partials(s, live, n, glide != 0, sdram);

    if (__builtin_expect(v->click > 8 || v->clp, 0)) click_run(v, s, strike, mb_noise(h, sdram));
    /* box: band-pass about 220 Hz (f = 0.125 at 11 kHz, damping 0.25), run on the average of 8 samples (its nulls at
     * 11 kHz keep the decimation clean) and interpolated back between its last two outputs, 8 samples late */
#ifdef __bfin__
    /* the box's filter in mbira_dsp.S (mb_box, the C loop below): bd[] = the interpolation's start and step per group;
     * its output is added by mb_out without the wah (after the buzz: the sum is the same), else by mb_boxadd */
    int32_t *const bd = s->bd;
    if (box0 | box) {
        if (sdram) touch(bd, sizeof s->bd);
        mb_box(acc, bd, &v->blo, box0, dbox * 8);                         /* blo, bbd, bo, bp: in this order */
        if (wah0 | wah) mb_boxadd(acc, bd);
    } else v->bo = v->bp = 0;
#else
    if (box0 | box) {
        int32_t blo = v->blo, bbd = v->bbd, bo = v->bo, bp = v->bp, bx = box0;
        for (int i = 0; i < OP1_BLOCK; i += 8) {
            int32_t *q = acc + i, in = 0;
            for (int j = 0; j < 8; j++) in += q[j];
            const int32_t d = (bo - bp) >> 3;
            int32_t o = bp;
            for (int j = 0; j < 8; j++) { o += d; q[j] += o; }
            bp = bo;
            blo = clampi(blo + (bbd >> 3), -131071, 131071);
            bbd = clampi(bbd + (((in >> 3) - blo - (bbd >> 2)) >> 3), -131071, 131071);
            bo = (((bbd * (bx >> 2)) >> 13) * 3) >> 2;
            bx += dbox * 8;
        }
        v->blo = blo; v->bbd = bbd; v->bo = bo; v->bp = bp;
    } else v->bo = v->bp = 0;
#endif
    if (__builtin_expect(buzz0 | buzz1, 0)) buzz_run(v, s, ph0, c0, amp0, buzz0, (buzz1 - buzz0) >> 7, sdram);
    /* the wah (out of line), else the voice's gain into out */
    const int32_t g = vel_gain(vi) >> 1;   /* about -4 dBFS a note (level pass 2026-10-29: was >> 2; +6 dB is the most: acc * g must fit 32 bits, mb_out too) */
    if (__builtin_expect(wah0 | wah, 0)) wah_run(v, s, out, wah0, (wah - wah0) >> 7, g, sdram);
    else {
        v->wo = v->wp = 0;
#ifdef __bfin__
        mb_out(acc, out, (box0 | box) ? bd : 0, g);
#else
        for (int i = 0; i < OP1_BLOCK; i++) out[i] = sat16((acc[i] * g) >> 15);
#endif
    }
    v->wph += 31072000u;                                                    /* 4.99 Hz at 689 blocks/s */

    for (int k = 0; k < NP; k++) {
        v->pinc[k] = v->inc[k];
        v->amp[k] = end[k] < (4 << 9) || (k && end[k] < (end[0] >> 10)) ? 0 : end[k];   /* -80 dBFS, or 60 dB under the fundamental: gone */
    }
    v->box = box; v->buzz = buzz1; v->wah = wah; v->dmix = dmix1;
    mb_level[vi] = v->amp[0] >> 9;
    mb_click[vi] = v->click;
    mb_pitch[vi] = inc0;
    mb_blocks++;
}

const op1_engine_t ENGINE_mbira = { mb_note_on, mb_render, mb_note_off,
                                    { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_NONE } };

/* ======================= screen (GUI thread) =======================
 * mbira.svg and mbira_geom.h (OP1-Emulator docs/design/engine-screens/night/build_night2.py): a kalimba in three-
 * quarter view with a thumb plucking a tine, traced from the night concept, and four meters on the right (labels and
 * 12 dim ticks each). Drawn here:
 *   the meters' lit ticks: STRIKE (blue), TINE (green), BOX (white), BUZZ (red), 0 .. 12 with the knob as the voices
 *     hear it (op1_synth_knobs_live: LFO included); page 2 of a knob: its label turns to RING / DOUBLE / WAH (nodes
 *     l0..l2 / p0..p2) and its meter shows the page-2 word
 *   while a note rings: white vibration arcs by the plucked tine's tip (bigger with the level), sound rings
 *     spreading from the sound hole, and with BUZZ the caps rattle (red ticks round them) */
#include "mbira_geom.h"
#pragma GCC push_options
#pragma GCC optimize("Os")                             /* GUI code: size, not speed */
#define QP(v) ((int32_t)((v) * 1024))
static const uint8_t M_BLUE[4] = { 0x69, 0x8E, 0xFF, 0xFF }, M_GREEN[4] = { 0x00, 0xED, 0x95, 0xFF },
                     M_WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, M_RED[4] = { 0xFF, 0x3A, 0x5D, 0xFF };
static const uint8_t *const M_COL[4] = { M_BLUE, M_GREEN, M_WHITE, M_RED };
static const int32_t M_ROW[4][2] = { { MB_ROW0_Y0, MB_ROW0_Y1 }, { MB_ROW1_Y0, MB_ROW1_Y1 }, { MB_ROW2_Y0, MB_ROW2_Y1 },
                                     { MB_ROW3_Y0, MB_ROW3_Y1 } };
static void *mb_lab[3][2];                                    /* page-1 / page-2 labels of the first three meters */
static uint32_t mb_frame, mb_rng = 7;

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    const op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}

static void arc_of(void *r, int32_t cx, int32_t cy, int32_t rad, int32_t a0, int32_t a1, int32_t w, const uint8_t *c)
{                                                            /* arc, Q10 centre / radius, angles 0..8192 (y down) */
    int32_t px = cx + ((rad * OP1_SINE_TABLE[(a0 + 2048) & 8191]) >> 15), py = cy + ((rad * OP1_SINE_TABLE[a0 & 8191]) >> 15);
    for (int32_t a = a0 + 512; a <= a1; a += 512) {
        const int32_t x = cx + ((rad * OP1_SINE_TABLE[(a + 2048) & 8191]) >> 15), y = cy + ((rad * OP1_SINE_TABLE[a & 8191]) >> 15);
        ln(r, px, py, x, y, w, c);
        px = x; py = y;
    }
}

static void mb_screen_init(void *scr)
{
    static const char *const ids[3][2] = { { "l0", "p0" }, { "l1", "p1" }, { "l2", "p2" } };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 2; j++) {
            mb_lab[i][j] = op1_find_node(scr, ids[i][j]);
            op1_node_set_visible(mb_lab[i][j], j == 0);
        }
}

static void mb_screen_update(void *scr, int dt)
{
    int16_t kn[4];
    op1_synth_knobs_live(OP1_SCREEN_SLOT & 7, kn);
    const int16_t *words = op1_synth_knobs(OP1_SCREEN_SLOT & 7);
    int32_t v[4];
    for (int i = 0; i < 4; i++) {
        const int page2 = i < 3 && op1_eng_page[i];
        v[i] = page2 ? words[5 + i] : kn[i];
        if (v[i] < 0) v[i] = 0;
        if (i < 3) { op1_node_set_visible(mb_lab[i][0], !page2); op1_node_set_visible(mb_lab[i][1], page2); }
    }
    op1_screen_draw_cached(scr, dt, mb_lab[0][0]);
    void *r = op1_raster();
    mb_frame++;

    /* the meters: 0 .. 12 lit ticks */
    for (int i = 0; i < 4; i++) {
        const int n = (v[i] * 12 + 16383) >> 15;
        for (int t = 0; t < n && t < 12; t++) {
            const int32_t x = MB_TICK_X0 + t * MB_TICK_DX;
            ln(r, x, M_ROW[i][0], x, M_ROW[i][1], 0x6CC, M_COL[i]);
        }
    }

    /* the loudest voice drives the vibration, the sound rings and the caps */
    int32_t loud = 0;
    for (int vo = 0; vo < OP1_VOICES; vo++) if (mb_level[vo] > loud) loud = mb_level[vo];
    if (loud < 256) return;
    const int32_t lv = loud > 32767 ? 32767 : loud;
    /* white vibration arcs by the tip: two, 3 .. 9 px out, shimmering */
    const int32_t sh = (OP1_SINE_TABLE[(mb_frame * 2600) & 8191] * QP(1)) >> 15;
    for (int a = 0; a < 2; a++) {
        const int32_t rad = QP(4) + a * QP(3) + ((lv * QP(3)) >> 15) + sh;
        arc_of(r, MB_TIP_X, MB_TIP_Y, rad, 6144 + 512, 8192 + 1024, 0x600, M_WHITE);
    }
    /* sound rings from the hole: one ring spreading 6 .. 26 px over 20 frames, again while it rings */
    const int32_t ph = (int32_t)(mb_frame % 20);
    arc_of(r, MB_HOLE_X, MB_HOLE_Y, QP(6) + ph * QP(1), 2048 + 1024, 4096 + 2048, 0x500, M_WHITE);
    /* caps: up to 6 red rattle ticks round each, with BUZZ */
    const int ticks = (kn[3] < 0 ? 0 : kn[3]) * 6 >> 15;
    for (int c = 0; c < 2; c++) {
        const int32_t cx = c ? MB_CAP1_X : MB_CAP0_X, cy = c ? MB_CAP1_Y : MB_CAP0_Y;
        for (int t = 0; t < ticks; t++) {
            mb_rng = mb_rng * 1664525u + 1013904223u;
            const int32_t a = (int32_t)(t * 1365 + (mb_rng >> 22)) & 8191;
            const int32_t s = OP1_SINE_TABLE[a], co = OP1_SINE_TABLE[(a + 2048) & 8191];
            ln(r, cx + ((QP(11) >> 5) * co >> 10), cy + ((QP(11) >> 5) * s >> 10),
               cx + ((QP(14) >> 5) * co >> 10), cy + ((QP(14) >> 5) * s >> 10), 0x500, M_RED);
        }
    }
}
#pragma GCC pop_options

const op1_screen_t SCREEN_mbira = { mb_screen_init, mb_screen_update };
