/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* "pew": a drawbar organ through a rotary speaker, on four macro knobs (2026-10-29: every position is a registration
 * an organist would use). pew_macro.h has the maps the screen shares.
 * blue   (knob 0) REGISTER: classic registrations from mellow to full, 008000000 -> 888000000 -> 888800000 ->
 *                 888808008 -> 888888888 (16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1'), each bar moving
 *                 straight between them in whole drawbar steps (a real drawbar's nine positions, 3 dB apart)
 * green  (knob 1) PERCUSSION: off, then the soft second harmonic (4') with the slow decay, through to the sharp third
 *                 (2 2/3') with the fast decay, then the key click comes up. Single trigger as on a Hammond: it
 *                 strikes only when no key was held, and decays for all the keys held
 * white  (knob 2) DRIVE / CHORUS: clean, then the scanner vibrato on the upper bars up to a chorus (the lower bars
 *                 stay dry, so the two beat), then the tube overdrive up to a growl (asymmetric, louder in)
 * red    (knob 3) LESLIE in three zones: brake (both rotors spin down and stop), slow (chorale), fast (tremolo); the
 *                 horn spins up in about half a second, the heavier drum takes a couple of seconds, braking takes
 *                 about a second (horn) and two (drum). A stopped rotor leaves a steady, undetuned sound.
 * page 2 (press blue / green / white; preset words 5, 6, 7): push the low, middle or high bars further out
 * The organ is three shared wave tables, one 16' period of 512 points: low (16', 5 1/3', 8') through the drum, upper
 * (the other six bars) and the percussion (4' and / or 2 2/3') through the horn, which wobbles in pitch (Doppler,
 * plus the scanner vibrato). The tables hold the sounding level too (no level pass per sample) and are rebuilt only
 * when a drawbar step or the percussion's harmonic changes, 16 points per block into a second copy that then takes
 * over, so no block does much of it. The percussion's level is one per block for all keys. Follows velocity.
 */
#include "hk/engine.h"
#include "pew_tables.h"
#include "pew_macro.h"

#define PT 513                                        /* table points: 512 and the first again */

typedef struct {
    uint32_t ph, hph;   /* drum side, horn side (the horn's pitch wobbles) */
    int32_t clk;        /* blocks since the note-on: the key click sounds in blocks 0 and 1 */
    int32_t gate;       /* the key is held (for the percussion's single trigger) */
} voice_t;

static voice_t voices[ENGINE_VOICES];
static uint32_t horn_ph, drum_ph, vib_ph;             /* rotor angles and the scanner's, 2^32 per turn */
static int32_t horn_speed, drum_speed;                /* phase per block */
static int last_voice = ENGINE_VOICES;
volatile uint32_t pew_blocks;                         /* rendered blocks, for the screen */
volatile uint32_t pew_horn, pew_drum;                 /* rotor angles, for the screen */
volatile int32_t pew_speed;                           /* the horn's speed (phase per block), for the screen */
volatile uint32_t pew_strikes;                        /* note-ons, for the screen's notes */
volatile uint8_t pew_held[ENGINE_VOICES];             /* each voice's key is down, for the screen's keys */
/* the knobs as this block hears them (after the slot LFO), for the screen */
volatile int16_t pew_knobs[4];

/* ---- the wave tables ---- */
/* low = Q15 sum of the low bars' sines, upper and percussion as a pair per point, percussion << 16 | upper (the
 * kernels' pw_mh layout); two copies, `cur` sounds, the other is being built */
static int16_t t_lo[2][PT];
static uint32_t t_mh[2][PT];
static int cur;
static uint8_t have[10], want[10], next[10];          /* 9 drawbar steps and the percussion's harmonic (0..8) */
static int build_at = -1;                             /* the next point of the copy being built, -1 when none is */
static int built;                                     /* the sounding copy holds `have` (none at first: silence) */
static int32_t comp = 4096, comp_next = 4096;         /* Q12 loudness compensation of the sounding registration */

/* a drawbar step's amplitude, Q15: 3 dB a step, 0 at 0 */
static const int16_t bar_amp[9] = {0, 2896, 4096, 5793, 8192, 11585, 16384, 23170, 32767};
static const uint8_t partial[9] = {1, 3, 2, 4, 6, 8, 10, 12, 16};   /* of the 16' fundamental, per bar */
#define LO_BAR 10922                                  /* a full low bar in the table: three fit in a 16-bit point */
#define UP_BAR 4000                                   /* a full upper bar: six, with the percussion, in 30,000 */
#define PERC_PEAK 12000                               /* the percussion's sine at most (build_start() caps it) */

/* reads one word per 32-byte line before a loop writes them: the data cache does not allocate on a write miss, so
   a line written first goes to SDRAM store by store (kennel ARCHITECTURE.md "MBIRA data cache") */
static inline void touch(const void *p, unsigned bytes)
{
    /* one aligned word in each line the range covers, inside the range */
    const uintptr_t end = (uintptr_t)p + bytes;
    for (uintptr_t q = (uintptr_t)p; q < end; q = (q & ~(uintptr_t)31) + 32) (void)*(const volatile uint32_t *)(q & ~(uintptr_t)3);
}

#define BUILD_STEP 16                                 /* table points built per block */
/* the copy being built: its bars (partial, amplitude with the level folded in) and the percussion's two sines */
static int32_t b_step[11];
static int16_t b_amp[11];                             /* under 16 bits: the build's products are 16 x 16 */
static int b_nlo, b_n;

static void build_start(void)
{
    /* the sounding level, 24,000 times the loudness compensation, folded into the tables: no level pass per sample */
    int32_t lvl = (24000 * comp_next) >> 12;
    if (lvl > 31000) lvl = 31000;
    int n = 0;
    for (int b = 0; b < 9; b++) {
        if (b == 3) b_nlo = n;
        if (!next[b]) continue;
        b_step[n] = partial[b] * 2;
        b_amp[n] = (int16_t)((((bar_amp[next[b]] * (b < 3 ? LO_BAR : UP_BAR)) >> 15) * lvl) >> 15);
        n++;
    }
    if (b_nlo > n) b_nlo = n;
    /* the percussion's peak: PERC_PEAK, less where the upper bars leave less room (upper + percussion is a 16-bit sum
       in pew_osc: the bound of both stays under 32,000, so it never wraps) */
    int32_t up = 0;
    for (int b = b_nlo; b < n; b++) up += b_amp[b];
    const int32_t pk = up + PERC_PEAK > 32000 ? 32000 - up : PERC_PEAK;
    const int32_t p3 = (next[9] * pk) >> 3, p2 = pk - p3;
    b_n = n;
    b_step[n] = 8, b_amp[n] = (int16_t)p2;            /* the percussion: 4' (partial 4), 2 2/3' (partial 6) */
    b_step[n + 1] = 12, b_amp[n + 1] = (int16_t)p3;
}

static int32_t acc[3][BUILD_STEP];                    /* a slice's sums: low, upper, percussion */

/* points from .. to - 1 (at most BUILD_STEP) of the copy being built: bar by bar over the slice, the sine's index
   stepping by the bar's partial */
static void build(int from, int to)
{
    const int n = to - from;
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < n; i++) acc[k][i] = 0;
    for (int b = 0; b < b_n + 2; b++) {
        int32_t *a = acc[b < b_nlo ? 0 : b < b_n ? 1 : 2];
        const int16_t amp = b_amp[b];
        const int32_t step = b_step[b];
        uint32_t idx = (uint32_t)(from * step);
        for (int i = 0; i < n; i++, idx += (uint32_t)step) a[i] += amp * pw_sin[idx & 1023];
    }
    int16_t *lo = t_lo[cur ^ 1] + from;
    uint32_t *mh = t_mh[cur ^ 1] + from;
    touch(lo, (unsigned)n * 2);
    touch(mh, (unsigned)n * 4);
    for (int i = 0; i < n; i++) {
        lo[i] = (int16_t)(acc[0][i] >> 15);
        mh[i] = (uint32_t)((acc[2][i] >> 15) & 0xFFFF) << 16 | (uint32_t)((acc[1][i] >> 15) & 0xFFFF);
    }
}

/* the loudness compensation of a registration, Q12: (E_ref / E)^(1/4) within 0.8 .. 1.3, E the tables' power and
 * E_ref the 888000000 registration's, so a thin registration is not much quieter than a full one (found by bisection:
 * no division) */
static int32_t loudness(const uint8_t *bar)
{
    int32_t e = 0;                                    /* in (table units / 16)^2: under 4.2 million */
    for (int b = 0; b < 9; b++) {
        const int32_t x = ((bar_amp[bar[b]] * (b < 3 ? LO_BAR : UP_BAR)) >> 15) >> 4;
        e += x * x;
    }
    const int32_t ref = 3 * (LO_BAR >> 4) * (LO_BAR >> 4);
    if (e <= 0) return 4096;
    int32_t lo = 3277, hi = 5325;                     /* 0.8 .. 1.3 */
    for (int k = 0; k < 12; k++) {
        const int32_t m = (lo + hi) >> 1;
        const int32_t m2 = (m * m) >> 12, m4 = (m2 * m2) >> 12;      /* Q12 */
        if ((((e >> 6) * m4) >> 6) > ref) hi = m; else lo = m;      /* e m^4 against ref, 32 bits */
    }
    return lo;
}

/* once a block: start, continue or finish a table build, BUILD_STEP points a block (33 blocks, about 95 ms, from a
   drawbar step to its sound); a build runs to its end, the next one then starts from wherever the knobs are by then */
static int fresh;                                     /* a note started this block with no key held */
static int fresh_build;                               /* the build under way started with such a note */

static void tables(void)
{
    if (build_at < 0) {
        int same = built;
        for (int k = 0; k < 10; k++) same &= want[k] == have[k];
        if (same) return;
        for (int k = 0; k < 10; k++) next[k] = want[k];
        build_at = 0;
        fresh_build = fresh;
        comp_next = loudness(next);
        build_start();
    }
    /* faster when it matters: the first tables are built whole (by pew_prepare() at the screen's load; here, in
       one block, only if that did not run), so the first note sounds at once; while a phrase starts (a note with no key held: a preset just
       loaded, a knob moved between phrases) 64 points a block, so the new sound is there within 8 blocks (23 ms) */
    if (fresh) fresh_build = 1;
    const int until = !built ? PT : build_at + (fresh_build ? 4 : 1) * BUILD_STEP;
    do {
        const int to = build_at + BUILD_STEP < PT ? build_at + BUILD_STEP : PT;
        build(build_at, to);
        build_at = to;
    } while (build_at < PT && build_at < until);
    if (build_at == PT) {
        cur ^= 1;
        for (int k = 0; k < 10; k++) have[k] = next[k];
        comp = comp_next;
        build_at = -1;
        built = 1;
    }
}

/* ---- the key click: one noise burst for every key, rebuilt when its level steps, 64 samples a block ---- */
static int32_t clk_buf[2 * ENGINE_BLOCK];
static int32_t clk_have = -1, clk_lvl, clk_c;
static int clk_at = -1;
static uint32_t clk_noise;

static void click(int32_t lvl)
{
    if (clk_at < 0) {
        if (lvl == clk_have) return;
        clk_lvl = lvl, clk_at = 0, clk_c = 32767, clk_noise = 0x9E3779B9u;
    }
    uint32_t noise = clk_noise;
    int32_t c = clk_c;
    int32_t *o = clk_buf + clk_at;
    touch(o, 64 * 4);
    for (int i = 0; i < 64; i++) {
        noise = noise * 1664525u + 1013904223u;
        o[i] = (((int32_t)(noise >> 17) - 16384) * ((c * clk_lvl) >> 15)) >> 15;
        c -= c >> 5;
    }
    clk_noise = noise, clk_c = c, clk_at += 64;
    if (clk_at == 2 * ENGINE_BLOCK) clk_have = clk_lvl, clk_at = -1;
}

/* ---- the percussion: one envelope for all keys (Q15), struck when no key was held ---- */
static int32_t perc_env;

static inline int32_t read(const int16_t *w, uint32_t ph)
{
    const uint32_t i = ph >> 23;                          /* 512 points */
    const int32_t f = (int32_t)((ph >> 8) & 0x7FFF);
    const int32_t a = w[i];
    return a + (((w[i + 1] - a) * f) >> 15);
}

static inline int32_t rsin(uint32_t ph) { return pw_sin[ph >> 22]; }   /* coarse sine, enough for an LFO */

#if defined(__bfin__) && !defined(PEW_C_LOOP)
/* pew_dsp.S: the sample loop below in two passes, bit for bit (PEW_C_LOOP keeps the C loop on the Blackfin too, for
 * kern_check.py's reference) */
#define PEW_KERNELS 1
void pew_osc(uint32_t *mh, uint32_t *st);
void pew_out(int16_t *out, const uint32_t *mh, const int32_t *c);
/* the passes' scratch, its 17 lines read (touch) before pew_osc writes them: the data cache does not allocate on a
   write miss, so a line written first goes to SDRAM store by store (kennel ARCHITECTURE.md "MBIRA data cache") */
static struct {
    uint32_t mh[ENGINE_BLOCK + 2];                      /* a spare word for pew_osc in front, pew_out reads one ahead */
} __attribute__((aligned(32))) scr;
#endif

/* the macros, once a block (first voice): knob words -> the shared state */
static int32_t m_perc, m_dec, m_vib, m_drive, m_click;
/* the block's values for every voice: percussion level, horn pitch wobble, tremolo amps, knees, drive's gain */
static int32_t b_pl, b_hv, b_ha, b_da, b_kp, b_kn, b_gf = 32768;

static void macros(const int16_t *knobs)
{
    /* REGISTER and the pushes: whole drawbar steps */
    int32_t lv[9];
    pew_registration(knobs[0], &knobs[5], lv);
    for (int b = 0; b < 9; b++) {
        const int32_t s = (lv[b] + 2048) >> 12;
        want[b] = (uint8_t)(s > 8 ? 8 : s < 0 ? 0 : s);
    }
    /* PERCUSSION: 0 .. 10 % off; to 45 % the soft second rises (slow decay); to 75 % it becomes the sharp third
       (level up, decay fast); above, the key click comes up from its resting level */
    int32_t g = knobs[1] < 0 ? 0 : knobs[1];
    int32_t harm = 0, lvl, dec, clk;
    if (g < 3277) lvl = 0, dec = 32557, clk = 0;
    else if (g < 14746) lvl = ((g - 3277) * 46811) >> 15, dec = 32557, clk = 0;               /* 0 .. 0.5 */
    else if (g < 24576) {
        const int32_t u = ((g - 14746) * 13653) >> 15;                                         /* 0 .. 1, Q12 */
        lvl = 16384 + ((u * 16383) >> 12);
        dec = 32557 - ((u * (32557 - 31981)) >> 12);
        harm = (u * 8 + 2048) >> 12;
        clk = 0;
    } else lvl = 32767, dec = 31981, harm = 8, clk = (g - 24576) * 4;                         /* 0 .. 32764 */
    want[9] = (uint8_t)(harm > 8 ? 8 : harm);
    m_perc = lvl > 32767 ? 32767 : lvl;
    m_dec = dec;
    /* the key click: a little always, up to full at the top of green (click level 0 .. 8191; halved, as the keys of
       a chord click together) */
    if (clk > 32767) clk = 32767;
    m_click = ((1200 + ((clk * (8191 - 1200)) >> 15)) >> 1) & ~511;   /* in 8 steps: rebuilt on a step only */
    /* DRIVE / CHORUS: 0 .. 45 % the scanner vibrato's depth, then the drive */
    int32_t w = knobs[2] < 0 ? 0 : knobs[2];
    m_vib = w >= 14746 ? 14000 : (w * 31115) >> 15;                                          /* 0 .. 14000 */
    m_drive = w <= 14746 ? 0 : ((w - 14746) * 29789) >> 14;                                  /* 0 .. 32767 */
    if (m_drive > 32767) m_drive = 32767;
}

/* the first tables, built whole outside the audio callback: screen.c calls it from the screen's load (the GUI thread,
   at boot, before any note), so no audio block pays for it. tables() builds them whole itself if this did not run (a
   host test with no screen); a no-op once anything is built or building */
void pew_prepare(const int16_t *knobs)
{
    if (built || build_at >= 0) return;
    macros(knobs);
    for (int k = 0; k < 10; k++) next[k] = want[k];
    comp_next = loudness(next);
    build_start();
    for (int i = 0; i < PT; i += BUILD_STEP) build(i, i + BUILD_STEP < PT ? i + BUILD_STEP : PT);
    for (int k = 0; k < 10; k++) have[k] = next[k];
    comp = comp_next;
    cur ^= 1;
    built = 1;
}

void engine_note_on(void *self, const int16_t *knobs)
{
    voice_t *v = &voices[ENGINE_VOICE(self)];
    int held = 0;
    for (int i = 0; i < ENGINE_VOICES; i++) held |= voices[i].gate;
    if (!held) perc_env = 32767, fresh = 1;           /* single trigger; tables() hurries a build */
    v->gate = 1;
    pew_held[ENGINE_VOICE(self)] = 1;
    v->ph = v->hph = 0;
    v->clk = 0;
    pew_strikes++;
    (void)knobs;
}

void engine_note_off(void *self, const int16_t *knobs)
{
    voices[ENGINE_VOICE(self)].gate = 0;
    pew_held[ENGINE_VOICE(self)] = 0;
    (void)knobs;
}

void engine_render(void *self, int16_t *out, const int16_t *knobs)
{
    for (int i = 0; i < 4; i++) pew_knobs[i] = knobs[i];
    const int vi = ENGINE_VOICE(self);
    voice_t *v = &voices[vi];
    int vel = ENGINE_VELOCITY(vi);
    if (vel < 1 || vel > 127) vel = 100;

    /* once a block (voices render in ascending order within a block): macros, tables, click, rotors, envelopes */
    const int first = vi <= last_voice;
    if (first) {
        macros(knobs);
        tables();
        fresh = 0;
        click(m_click);
        perc_env = (perc_env * m_dec) >> 15;
        const int32_t k3 = knobs[3] < 0 ? 0 : knobs[3];
        const int32_t target = pew_leslie_target(k3);
        const int braking = target == 0;
        /* speeding up: horn ~0.2 s, drum ~0.75 s. Braking: horn ~1 s to a stop (an arithmetic shift of a negative
           step is at least -1, so the speed reaches 0 exactly), drum >> 7 (~2 s) */
        horn_speed += (target - horn_speed) >> 6;
        drum_speed += (((target >> 3) * 7) - drum_speed) >> (braking ? 7 : 8);
        horn_ph += (uint32_t)horn_speed;
        drum_ph += (uint32_t)drum_speed;
        vib_ph += 85500000u;                                       /* the scanner: 6.86 Hz */
        pew_horn = horn_ph;
        pew_speed = horn_speed;
        pew_drum = drum_ph;
        /* the percussion's level this block, Q15 (the other paths' levels are in the tables) */
        b_pl = (perc_env * m_perc) >> 15;

        /* horn: tremolo 0.7..1, Doppler a few cents, plus the scanner vibrato; drum: tremolo 0.55..1 */
        int32_t hs = rsin(horn_ph), hc = rsin(horn_ph + 0x40000000u), ds = rsin(drum_ph);
        /* the modulation depth eases to 0 as a rotor slows below ~0.34 Hz (2^22 phase per block), so a stopped rotor
           sits at the middle of the tremolo with no detune whatever angle it stopped at; full depth above */
        if (horn_speed < 0x400000) {
            const int32_t f = horn_speed >> 7;                          /* Q15, 0 .. 32767 */
            hs = (hs * f) >> 15;
            hc = (hc * f) >> 15;
        }
        if (drum_speed < 0x400000) ds = (ds * (drum_speed >> 7)) >> 15;
        /* the horn's pitch: inc + (inc >> 16) * hv >> 5, |hv| <= 16048 (Doppler hc >> 4 = 1.7 cents at most, the vibrato
           up to about 11.5 cents) */
        b_hv = (hc >> 4) + ((rsin(vib_ph) * m_vib) >> 15);
        const int32_t horn_amp = 27852 + ((hs * 4915) >> 15);          /* Q15 0.70 .. 1.0 */
        const int32_t drum_amp = 25395 + ((ds * 7372) >> 15);          /* Q15 0.55 .. 1.0 */
        /* drive: the sum gains x 1 .. x 2 in (the amps x 0.5 .. 1 into a sum taken >> 14), the knee comes down from
           24,000 to 11,000 (the negative side to 8,000: even harmonics), the output gain up to x 1.25 */
        const int32_t din = 16384 + (m_drive >> 1);
        b_ha = (horn_amp * din) >> 15, b_da = (drum_amp * din) >> 15;
        b_kp = 24000 - ((m_drive * 13000) >> 15), b_kn = -(24000 - ((m_drive * 16000) >> 15));
        b_gf = 32768 + (m_drive >> 2);
    }
    last_voice = vi;
    const int32_t pl = b_pl, hv = b_hv, ha = b_ha, da = b_da, knee_p = b_kp, knee_n = b_kn;

    int32_t gain2 = ((((30000 * vel) >> 7) * b_gf) >> 15) * 2;
    if (gain2 > 65535) gain2 = 65535;
    const int32_t *cb = v->clk < 2 ? clk_buf + v->clk * ENGINE_BLOCK : 0;
    v->clk++;
    if (v->clk > 2) v->clk = 2;

    uint32_t ph = v->ph, hph = v->hph;
    const int16_t *lo_t = t_lo[cur];
    const uint32_t *mh_t = t_mh[cur];
#ifdef PEW_KERNELS
    {
        /* the ramps as level << 9 (the high half is level >> 7, 0 <= level < 2^22); L0..L3 come back as the caller
         * had them, as mbira.c does */
        uint32_t l0, l1, l2, l3;
        __asm__ volatile("%0 = L0;\n\t%1 = L1;\n\t%2 = L2;\n\t%3 = L3;" : "=d"(l0), "=d"(l1), "=d"(l2), "=d"(l3));
        if (first)
            for (unsigned i = 0; i < sizeof scr / 4; i += 8) (void)((volatile uint32_t *)&scr)[i];   /* touch */
        uint32_t st[7] = { ph, hph, (uint32_t)hv, (uint32_t)(uintptr_t)ENGINE_PITCH, (uint32_t)(uintptr_t)lo_t,
                           (uint32_t)(uintptr_t)mh_t, (uint32_t)pl };
        pew_osc(scr.mh, st);
        const int32_t c[5] = { (int32_t)((uint32_t)ha << 16 | (uint32_t)da), gain2, (int32_t)(uintptr_t)cb, knee_p,
                               knee_n };
        pew_out(out, scr.mh + 1, c);
        __asm__ volatile("L0 = %0;\n\tL1 = %1;\n\tL2 = %2;\n\tL3 = %3;" : : "d"(l0), "d"(l1), "d"(l2), "d"(l3));
        ph = st[0];
        hph = st[1];
    }
#else
    for (int i = 0; i < ENGINE_BLOCK; i++) {
        const uint32_t inc = ENGINE_PITCH[i] >> 1;                 /* table = 16' = an octave below */
        ph += inc;
        hph += inc + (uint32_t)(((int32_t)(inc >> 16) * hv) >> 5);   /* Doppler and vibrato on the horn */
        const uint32_t j = hph >> 23;
        const int32_t f = (int32_t)((hph >> 8) & 0x7FFF);
        const int32_t ua = (int16_t)mh_t[j], ub = (int16_t)mh_t[j + 1];
        const int32_t pa = (int16_t)(mh_t[j] >> 16), pb = (int16_t)(mh_t[j + 1] >> 16);
        const int32_t up = (int16_t)(ua + (((ub - ua) * f) >> 15)), pc = (int16_t)(pa + (((pb - pa) * f) >> 15));
        const int32_t lo = read(lo_t, ph);
        const int32_t hi = (int16_t)(up + ((pc * pl) >> 15));
        int32_t y = (lo * da + hi * ha) >> 14;
        if (cb) y += cb[i];
        /* overdrive like a pushed tube amp: a quarter of the swing past the knees */
        if (y > knee_p) y = knee_p + ((y - knee_p) >> 2);
        if (y < knee_n) y = knee_n + ((y - knee_n) >> 2);
        out[i] = engine_sat16((int32_t)(((int64_t)y * gain2) >> 16));
    }
#endif
    v->ph = ph;
    v->hph = hph;
    pew_blocks++;
}
