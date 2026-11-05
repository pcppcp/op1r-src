/* "quad" LFO: four LFOs in one LFO slot (6 waveforms, tempo sync, smoothing, phase offset, 6 types), each routed to a
 * synth / drum engine knob, an envelope stage or an FX knob.
 *
 * Runs in the MIDI LFO class (lfo type 4): the DB row "quad" resolves to it via the lfo_types name compare hook, and
 * the MIDI LFO vtable apply / trigger entries are redirected here (hook.S, make_firmware.py). Anything without the
 * quad marker keeps the original MIDI LFO code.
 *
 * Parameter words (8 x 16 bit, saved with the patch) = one 128-bit little-endian bit stream (bit 0 = w[0] bit 0):
 *   bits 0..119   4 LFOs x 30 bits, LFO i at bit 30 i:  rate 6 | sync 1 | wave 3 | smooth 3 | offset 3 | type 3 |
 *                 target 5 | amount 6 (+32)
 *                 rate 0..63: free 0.05 .. 20 Hz (exponential), sync: tempo division index (QL_DIVS)
 *                 wave 0 sine, 1 triangle, 2 ramp up, 3 ramp down, 4 square, 5 sample & hold, 6 lorenz x, 7 lorenz z
 *                 (Lorenz attractor, docs/62: one wing loop per cycle, the offset field is the chaos rho 28 .. 14)
 *                 smooth 0..7: one-pole slew, 2^smooth blocks; offset 0..7: start phase in 1/8 cycles
 *                 amount -31..31 (full = +-32767); type see QT_*
 *                 target 0..11: synth (drum) knob blue..red, envelope A D S R, FX knob blue..red;
 *                 12..23: LFO j (0..3) x (frequency, amount, phase offset) = 12 + 3 j + k (not its own: skipped
 *                 by the white encoder, ignored if stored - a self target alone would reach no knob)
 *                 24..26: engine page 2 blue / green / white, 27..29: FX page 2 blue / green / white (plugin engines
 *                 / FX only: words 5..7 of the knob words, 0..32767; engine page 2 is synth only, none on drums)
 *   bits 120..127 marker 0xA5 (w[7] = 0xA5xx, negative: never a stock MIDI LFO word)
 * Older quad formats (op1r <= 15, per-word markers) are not read any more.
 * LFO targets (modulation from the modulator's value of the previous block, so order and feedback are harmless):
 *   frequency  x 2^(+-2 octaves) at full amount;  amount  x (0 .. 2);  phase offset  +-1/2 cycle.
 *   Several LFOs on the same target add up. The modulator's own amount scales its effect.
 * Types: VOIC  per-voice phase, runs while the voice plays (never reset)
 *        FREE  one shared free-running phase
 *        GATE  per-voice phase, restarts at the voice's note-on
 *        MONO  shared phase, restarts at every note-on
 *        SHOT  per voice, restarts at note-on, stops after one cycle (holds the end value)
 *        RAND  per voice, a new random value at each note-on, held
 *   FX targets and drums have no per-voice state: there VOIC = FREE, GATE = MONO, 1SHT / RAND are shared.
 * Note-on: VoiceCtl calls the voice LFO object's vt+0x1c trigger(this, words) for every synth (0x019673DC) and
 * drum (0x0196735E) note-on while the LFO is active (lfo_active); for the MIDI LFO class these are bare RTS.
 *
 * Controls on the LFO page (encoder press blue..red selects LFO 1..4; turns edit the selected one):
 *   blue frequency / division, green amount, white target (21: own F / A / O skipped, wraps), red type (6, wraps)
 *   Shift held (table of all four): blue phase offset, green smoothing, white waveform, red sync on / off
 *
 * Firmware calls (fw 246, docs/10):
 *   synth knobs   Ranges::knob_range_synth(knob, &mod, engine, 0xFF801724)   (apply pass 0)
 *   envelope      0x019F69B0(stage, &mod, 0xFF801724)                         (apply pass 1)
 *   FX knobs      0x01A1DE68(&mod, 0x01DE5C50 + fx_type*0x30 + knob*6, 0xFF801724 + knob*2)
 *                 (FX-object apply, pass 2 = synth FX, 5 = drum FX)
 *   drum knobs    Ranges 0x01A1E57C(knob, &mod, drum engine 0xFF801704[drum slot], 0xFF801724)   (pass 3)
 *   drum env      0x01A1DE68(&mod, 0x01DE56C8 + stage*6, 0xFF801724 + stage*2)                     (pass 4)
 * mod = signed Q15 offset in knob units. The firmware calls apply once per ACTIVE voice and block: per-voice LFOs
 * advance there; shared LFOs advance by elapsed time from the Blackfin CYCLES counter (block length learned from
 * the per-voice calls), so they keep running between notes (screen included). */
#include "../op1_synth.h"

#ifndef OP1_HOST
#define KNOB_RANGE_SYNTH ((void (*)(int, int16_t *, int, void *))0x01A1DFEC)
#define KNOB_RANGE_DRUM  ((void (*)(int, int16_t *, int, void *))0x01A1E57C)
#define DRUM_ENGINE()    (((const int *)0xFF801704)[*(volatile int *)0xFF8016D8 & 7])
#define DRUM_FX_TYPE()   (((const int *)0xFF801754)[*(volatile int *)0xFF8016D8 & 7])
#define DRUM_ENV_RANGE(k) ((const int16_t *)(0x01DE56C8 + (k) * 6))
#define FX_KNOB_MOD      ((void (*)(int16_t *, const int16_t *, int16_t *))0x01A1DE68)   /* (&mod, range, &param) */
#define FX_RANGE(type, k) ((const int16_t *)(0x01DE5C50 + (type) * 0x30 + (k) * 6))
#define FX_PARAM(k)      ((int16_t *)0xFF801724 + (k))
#define ENV_MOD          ((void (*)(int, int16_t *, void *))0x019F69B0)
#define STAGE_PARAMS     ((void *)0xFF801724)
#define SYNTH_LFO_OBJ    ((void *const *)0x01AAAD6C)          /* [6] per-voice LFO objects */
#define SYNTH_ENGINE()   (((const int *)0xFF8016E4)[*(volatile int *)0xFF8016D4 & 7])
#define SYNTH_FX_TYPE()  (((const int *)0xFF801734)[*(volatile int *)0xFF8016D4 & 7])
#define PAGE_POS()       (*(volatile int *)0x01DE4C40 == 1)
#define PAGE_WORDS(pos)  ((const int16_t *)(((pos) ? 0xFF801644 : 0xFF8014C4) + \
                          (*(volatile int *)((pos) ? 0xFF8016D8 : 0xFF8016D4) & 7) * 16))
#define TEMPO_RATE()     (*(volatile uint32_t *)0x01AADAD4)   /* metronome: phase inc per block for 8 cycles per beat */
#define SHIFT_HELD()     (*(volatile uint8_t *)0x01DD7F78)
/* LFO switched on (Shift + T4) in the current slot: synth 0x01AAAC28[synth slot], drum 0x01AAAC38[drum slot 0x01AAAC10]
 * (getters 0x0196726C / 0x0196724C; the stock note-on 0x019673DC / 0x0196735E checks the same bytes) */
#define LFO_ON(pos)      (((volatile uint8_t *)((pos) ? 0x01AAAC38 : 0x01AAAC28))[(pos) ? (*(volatile int *)0x01AAAC10 & 7) \
                                                                                        : (*(volatile int *)0xFF8016D4 & 7)])
extern int mx_engine_plugin(int n), mx_fx_plugin(int n);            /* dispatcher.c */
#define ENGINE_PLUGIN(sel) mx_engine_plugin((sel) - OP1_ENGINE_MAGIC)
#define FX_PLUGIN(sel)     mx_fx_plugin((sel) - OP1_ENGINE_MAGIC)
#else                                                          /* host test: harness/quadlfo_test.c */
void host_knob_range_synth(int, int16_t *, int, void *);
void host_fx_knob_mod(int16_t *, const int16_t *, int16_t *);
extern int16_t host_fx_range[8][3], host_fx_params[8];
void host_env_mod(int, int16_t *, void *);
extern void *host_lfo_obj[6];
extern int16_t host_lfo_words[8];
#define KNOB_RANGE_SYNTH host_knob_range_synth
void host_knob_range_drum(int, int16_t *, int, void *);
#define KNOB_RANGE_DRUM  host_knob_range_drum
#define DRUM_ENGINE()    3
#define DRUM_FX_TYPE()   5
#define DRUM_ENV_RANGE(k) ((const int16_t *)host_fx_range[k])
#define FX_KNOB_MOD      host_fx_knob_mod
#define FX_RANGE(type, k) ((void)(type), (const int16_t *)host_fx_range[k])
#define FX_PARAM(k)      (&host_fx_params[k])
#define ENV_MOD          host_env_mod
extern int16_t host_stage[8]; extern int host_eng_plugin, host_fx_plugin;
#define STAGE_PARAMS     ((void *)host_stage)
#define ENGINE_PLUGIN(sel) ((void)(sel), host_eng_plugin)
#define FX_PLUGIN(sel)     ((void)(sel), host_fx_plugin)
#define SYNTH_LFO_OBJ    ((void *const *)host_lfo_obj)
#define SYNTH_ENGINE()   14
#define SYNTH_FX_TYPE()  5
#define PAGE_POS()       (host_pos)
#define PAGE_WORDS(pos)  ((const int16_t *)host_lfo_words)
extern uint32_t host_cycles, host_tempo_rate; extern int host_shift, host_pos;
#define TEMPO_RATE()     (host_tempo_rate)
#define SHIFT_HELD()     (host_shift)
extern int host_lfo_on[2];
#define LFO_ON(pos)      (host_lfo_on[pos])
#define READ_CYCLES()    (host_cycles)
#endif
#ifndef READ_CYCLES
static inline uint32_t READ_CYCLES(void) { uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c)); return c; }
#endif
enum { QL_SYNTH = 0, QL_ENV = 1, QL_FX = 2 };

enum { QT_VOICE, QT_FREE, QT_GATE, QT_MONO, QT_1SHT, QT_RAND, QT_N };
static const char *const ql_type_name[QT_N] = { "VOIC", "FREE", "GATE", "MONO", "SHOT", "RAND" };
#define QL_NTGT 30                                           /* 12 knob / env / fx, 4 LFOs x 3, 3 + 3 page 2 */
#define QL_P2   24                                           /* first page-2 target (engine 24..26, FX 27..29) */
#define QL_MARK 0xA5

typedef struct { uint8_t wave, sync, smooth, off, rate, type, tgt, tl, tk; int8_t amt; int16_t a128; uint32_t inc, phoff; } ql_lfo;
typedef struct { uint32_t ph, cnt; int16_t val, rnd; uint8_t done; int32_t lx, ly, lz; } ql_st;   /* lx..lz: Lorenz, Q16 (lz 0 = start) */
static inline int ql_is_lfo(const ql_lfo *o) { return o->tgt >= 12 && o->tgt < QL_P2; }   /* LFO -> LFO target */
static inline int ql_p2(const ql_lfo *o) { return o->tgt >= QL_P2; }                      /* page-2 word 5 + k */
/* page-2 word (5..7) of a plugin engine / FX: + mod, 0..32767 (engine words: 0 = unset = 8192, kept >= 1) */
static void ql_p2_mod(int16_t *w, int16_t mod, int engine)
{
    int v = *w;
    if (engine && !v) v = 8192;
    v += mod;
    *w = (int16_t)(v < engine ? engine : v > 32767 ? 32767 : v);
}

/* page-2 values as modulated last (0 engine, 1 synth FX, 2 drum FX; words 5..7) and when (CYCLES): plugin screens show
 * them through op1_page2_live while the LFO applies them (the stage copy is not what the screens read) */
static volatile int16_t ql_p2_shown[3][3];
static volatile uint32_t ql_p2_at[3][3];
static volatile uint8_t ql_p2_ok[3][3];                        /* set when applied, cleared when stale (CYCLES wraps)*/
static ql_st sh_st[2][4];                                     /* shared state per page (synth, drum) */
static ql_st v_st[6][4];                                      /* per synth voice */
static volatile int16_t disp_all[2][4];                       /* last per-voice output (before amount), screen */
static volatile uint32_t ph_cyc_all[2];                       /* CYCLES at the last shared update per page */
static volatile uint32_t cpb = 1161000;                       /* cycles per 128-sample block (learned; start: 400 MHz) */
static uint32_t voice_cyc[6];
static uint32_t ql_rng = 0x9E3779B9u;
volatile uint8_t ql_sel[2];                                   /* selected LFO per page (synth/drum) */

/* op1r <= 15 quad words: a 0x4000 / 0x6000 marker in each of words 4..7. Not decoded any more, but still a quad LFO
 * (never handed to the MIDI LFO code, whose range asserts would lock the OP-1): they read as defaults (silent) and
 * become the current format, all four LFOs at defaults, on the first edit. */
static inline int ql_old_ctrl(int16_t c) { uint16_t m = (uint16_t)c & 0xE000; return m == 0x4000 || m == 0x6000; }
static int ql_is_old(const int16_t *w)
{
    return ((uint16_t)w[7] >> 8) != QL_MARK && ql_old_ctrl(w[4]) && ql_old_ctrl(w[5]) && ql_old_ctrl(w[6]) && ql_old_ctrl(w[7]);
}
int ql_is_quad(const int16_t *w) { return ((uint16_t)w[7] >> 8) == QL_MARK || ql_is_old(w); }
/* bit stream over the 8 words */
static uint32_t ql_get(const int16_t *w, int pos, int n)
{
    uint32_t v = 0;
    for (int k = 0; k < n; k++, pos++) v |= (uint32_t)(((uint16_t)w[pos >> 4] >> (pos & 15)) & 1) << k;
    return v;
}
static void ql_put(int16_t *w, int pos, int n, uint32_t v)
{
    for (int k = 0; k < n; k++, pos++) {
        uint16_t m = (uint16_t)(1u << (pos & 15)), x = (uint16_t)w[pos >> 4];
        w[pos >> 4] = (int16_t)((v >> k) & 1 ? x | m : x & ~m);
    }
}
static int clampq(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* tempo divisions (cycle length): shift of the metronome rate (8 cycles per beat), triplet = x 3/2 */
#define QL_DIVS 11
static const char *const ql_div_name[QL_DIVS] = { "4/1", "2/1", "1/1", "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32" };
static const uint8_t ql_div_shift[QL_DIVS] = { 7, 6, 5, 4, 3, 3, 2, 2, 1, 1, 0 };

/* phase increment per 128-sample block: 0.05 Hz * 400^(x/32768) */
static uint32_t ql_inc(int32_t x)
{
    if (x < 0) x = 0;
    if (x > 32767) x = 32767;
    int32_t oct = (x * 17702) >> 10;                              /* x * log2(400) / 32768, Q16 */
    int32_t f = oct & 0xFFFF;                                     /* 2^f ~ 1 + f (0.6565 + 0.3435 f) */
    uint32_t frac = 65536 + (((uint32_t)f * (uint32_t)(43025 + ((f * 22511) >> 16))) >> 16);   /* < 2^32 */
    uint32_t base = 623203;                                       /* 0.05 Hz * 2^32 * 128 / 44100 */
    return (uint32_t)(((uint64_t)base * frac) >> 16) << (oct >> 16);
}
static uint32_t ql_div_inc(int r)                                /* tempo division r (cycle length) */
{
    if (r >= QL_DIVS) r = QL_DIVS - 1;
    uint32_t inc = TEMPO_RATE() >> ql_div_shift[r];
    return (r == 5 || r == 7 || r == 9) ? inc + (inc >> 1) : inc;   /* triplets: 3 per 2 */
}
/* waveform value, Q15 */
static int32_t ql_shape(int wave, uint32_t phase, uint32_t cnt, int i)
{
    switch (wave) {
    case 1: { int32_t q = (int32_t)((phase + 0x40000000u) >> 16); return (q < 32768 ? q : 65535 - q) * 2 - 32767; }
    case 2: return (int32_t)phase >> 16;
    case 3: return ~((int32_t)phase >> 16);                       /* -32768 .. 32767 */
    case 4: return phase < 0x80000000u ? 32767 : -32767;
    case 5: { uint32_t x = (cnt + (uint32_t)i * 0x9E37u) * 2654435761u; x ^= x >> 15; return (int16_t)(x >> 16); }
    default: return OP1_SINE_TABLE[phase >> 19];                  /* sin(2 pi phase / 2^32) */
    }
}
/* Lorenz attractor (docs/62): dx = 10 (y - x), dy = x (rho - z) - y, dz = x y - 8/3 z; state Q16, products from Q8
 * copies (no 64-bit multiply). adv = phase advance of this step (2^32 = one LFO cycle = one wing loop ~ 0.7 time
 * units); Euler sub-steps of <= 0.01, at most 16 (faster: slowed down). rho_k 0..7 = 28 .. 14 (from 24 down it settles into a wing,
 * faster the lower). Returns Q15. */
#define QL_L1 65536
static int32_t ql_lorenz(ql_st *s, uint32_t adv, int rho_k, int out_z, int rnd_start)
{
    if (!s->lz || s->lx > 100 * QL_L1 || s->lx < -100 * QL_L1 || s->ly > 100 * QL_L1 || s->ly < -100 * QL_L1 ||
        s->lz > 100 * QL_L1 || s->lz < 0) {                          /* start / runaway (products stay in 32 bit) */
        s->lx = QL_L1 + (rnd_start ? (s->rnd & 0x3FFF) * 16 : 0); s->ly = QL_L1; s->lz = 20 * QL_L1;
    }
    uint32_t dt = (uint32_t)(((adv >> 8) * 179u) >> 16);          /* time units Q16: adv / 2^32 * 0.7 * 2^16 */
    int n = (int)((dt + 654) / 655);                                /* sub-steps of <= 0.01 */
    if (n > 16) { n = 16; dt = 16 * 655; }
    int32_t h = n ? (int32_t)(dt / (uint32_t)n) : 0, rho = (28 * 256) - rho_k * 512;   /* Q8: 28 .. 14 */
    int32_t x = s->lx, y = s->ly, z = s->lz;
    for (int k = 0; k < n; k++) {
        int32_t xs = x >> 8, ys = y >> 8, zs = z >> 8;
        int32_t dx = 10 * (y - x), dy = xs * (rho - zs) - y, dz = xs * ys - ((z >> 8) * 683);
        x += ((dx >> 8) * h) >> 8; y += ((dy >> 8) * h) >> 8; z += ((dz >> 8) * h) >> 8;
    }
    s->lx = x; s->ly = y; s->lz = z;
    int32_t v = out_z ? (((z - 24 * QL_L1) >> 4) * 1638) >> 12 : ((x >> 4) * 1638) >> 12;   /* x / 20, (z - 24) / 20 */
    return v > 32767 ? 32767 : v < -32767 ? -32767 : v;
}
/* one-pole slew towards x; k = elapsed blocks Q16 */
static int16_t ql_slew(int16_t y, int32_t x, int sm, uint32_t k)
{
    if (!sm) return (int16_t)x;
    k >>= sm;
    if (k >= 65536) return (int16_t)x;
    int32_t d = x - y;                                            /* |d| < 2^16 */
    if (k <= 32768) return (int16_t)(y + ((d * (int32_t)k) >> 16));   /* fits 32 bit */
    return (int16_t)(y + (int32_t)(((int64_t)d * (int32_t)k) >> 16));
}
/* (a * k) >> 16 mod 2^32, exact, without a 64-bit multiply (libgcc on the Blackfin) */
static inline uint32_t ql_mulq16(uint32_t a, uint32_t k)
{
    uint32_t kl = k & 0xFFFF;
    return a * (k >> 16) + (a >> 16) * kl + (((a & 0xFFFF) * kl) >> 16);
}

static inline int16_t ql_out(int16_t val, int a128) { return (int16_t)(((int32_t)val * a128) >> 7); }

/* decode / encode LFO i (bit stream, see top) */
static const uint8_t ql_def_rate[4] = { 19, 31, 42, 54 };       /* ~0.3 / 1 / 3 / 8 Hz (= make_firmware.py defaults) */
static void ql_dec(const int16_t *w, int i, ql_lfo *o)
{
    if (ql_is_old(w)) {                                           /* old format: defaults, amount 0 */
        o->rate = ql_def_rate[i]; o->sync = 0; o->wave = 0; o->smooth = 0; o->off = 0; o->type = QT_VOICE;
        o->tgt = (uint8_t)i; o->tl = o->tk = 0; o->amt = 0; o->a128 = 0; o->inc = ql_inc((o->rate << 9) | 0x100); o->phoff = 0;
        return;
    }
    int b = 30 * i;
    o->rate = (uint8_t)ql_get(w, b, 6); o->sync = (uint8_t)ql_get(w, b + 6, 1);
    o->wave = (uint8_t)ql_get(w, b + 7, 3); o->smooth = (uint8_t)ql_get(w, b + 10, 3);
    o->off = (uint8_t)ql_get(w, b + 13, 3); o->type = (uint8_t)clampq((int)ql_get(w, b + 16, 3), 0, QT_N - 1);
    o->tgt = (uint8_t)clampq((int)ql_get(w, b + 19, 5), 0, QL_NTGT - 1);
    int lt = o->tgt >= 12 && o->tgt < QL_P2;
    o->tl = lt ? (uint8_t)((o->tgt - 12) / 3) : 0; o->tk = lt ? (uint8_t)((o->tgt - 12) % 3) : 0;
    o->amt = (int8_t)clampq((int)ql_get(w, b + 24, 6) - 32, -31, 31); o->a128 = (int16_t)(o->amt * 127 / 31);
    if (o->sync) o->rate = (uint8_t)clampq(o->rate, 0, QL_DIVS - 1);
    o->inc = o->sync ? ql_div_inc(o->rate) : ql_inc((o->rate << 9) | 0x100);
    o->phoff = (uint32_t)o->off << 29;
}
static void ql_enc(const ql_lfo *o, int16_t *w, int i)
{
    int b = 30 * i;
    ql_put(w, b, 6, o->rate); ql_put(w, b + 6, 1, o->sync); ql_put(w, b + 7, 3, o->wave); ql_put(w, b + 10, 3, o->smooth);
    ql_put(w, b + 13, 3, o->off); ql_put(w, b + 16, 3, o->type); ql_put(w, b + 19, 5, o->tgt);
    ql_put(w, b + 24, 6, (uint32_t)(o->amt + 32)); ql_put(w, 120, 8, QL_MARK);
}
/* LFO -> LFO modulation: src[i] = LFO i's value (before amount) in this context, from the previous block */
static void ql_mod(ql_lfo *L, const int16_t *src)
{
    int32_t acc[4][3] = { { 0 } };
    for (int i = 0; i < 4; i++)
        if (ql_is_lfo(&L[i]) && L[i].tl != i) acc[L[i].tl][L[i].tk] += ql_out(src[i], L[i].a128);
    for (int j = 0; j < 4; j++) {
        int32_t f = clampq(acc[j][0], -32767, 32767), a = clampq(acc[j][1], -32767, 32767), o = clampq(acc[j][2], -32767, 32767);
        if (f) {                                                      /* 2^(f * 2 / 32768) octaves */
            int32_t x = f * 4, e = x >> 16, fr = x & 0xFFFF;         /* Q16 octaves: integer + fraction */
            uint32_t m = 65536 + (((uint32_t)fr * (uint32_t)(43025 + ((fr * 22511) >> 16))) >> 16);
            uint64_t inc = ((uint64_t)L[j].inc * m) >> 16;
            inc = e >= 0 ? inc << e : inc >> -e;
            L[j].inc = inc > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)inc;
        }
        if (a) L[j].a128 = (int16_t)clampq((L[j].a128 * (32768 + a)) >> 15, -127, 127);
        if (o) L[j].phoff += (uint32_t)o << 16;
    }
}

static inline int ql_group(const ql_lfo *o) { return o->tgt < 12 ? o->tgt >> 2 : o->tgt >= QL_P2 + 3 ? QL_FX : QL_SYNTH; }   /* (LFO targets: callers skip them) */
static inline int ql_dest(const ql_lfo *o)  { return o->tgt < 12 ? o->tgt & 3 : (o->tgt - QL_P2) % 3; }
/* shared state (no per-voice phase): drums, FX targets, FREE and MONO */
static inline int ql_shared(int pos, const ql_lfo *o) { return pos || ql_group(o) == QL_FX || o->type == QT_FREE || o->type == QT_MONO; }

/* advance one state by k blocks (Q16) */
static void ql_step(ql_st *s, const ql_lfo *o, uint32_t k, int i)
{
    int32_t x;
    if (o->type == QT_RAND && o->wave < 6) x = s->rnd;               /* RAND: held value; Lorenz: a random start */
    else {
        uint32_t adv = 0;
        if (!(o->type == QT_1SHT && s->done)) {
            adv = k == 65536 ? o->inc : ql_mulq16(o->inc, k);
            uint32_t n = s->ph + adv;
            if (n < s->ph) { s->cnt++; if (o->type == QT_1SHT) { s->done = 1; n = 0xFFFFFFFFu; } }
            s->ph = n;
        }
        x = o->wave >= 6 ? ql_lorenz(s, adv, o->off, o->wave == 7, o->type == QT_RAND) : ql_shape(o->wave, s->ph + o->phoff, s->cnt, i);
    }
    s->val = ql_slew(s->val, x, o->smooth, k);
}
static void ql_restart(ql_st *s)
{
    s->ph = 0; s->done = 0; s->cnt++;
    ql_rng = ql_rng * 1664525u + 1013904223u; s->rnd = (int16_t)(ql_rng >> 16);
    s->lz = 0;                                                       /* Lorenz: from the start point again */
}

/* elapsed time since `since` in blocks, Q16 (capped at 64 blocks: no jumps after long pauses) */
static uint32_t blocks_q16(uint32_t now, uint32_t since)
{
    uint32_t d = now - since, c = cpb;
    if (d > 64 * c) d = 64 * c;
    return (uint32_t)(((uint64_t)d << 16) / c);
}
/* advance the shared states of a page to now (audio thread) */
static void ql_advance_pos(int pos, const ql_lfo *L)
{
    uint32_t now = READ_CYCLES();
    if (now - ph_cyc_all[pos] < (cpb >> 2)) return;              /* already advanced this block (other voice / pass) */
    uint32_t b = blocks_q16(now, ph_cyc_all[pos]);
    ph_cyc_all[pos] = now;
    for (int i = 0; i < 4; i++) ql_step(&sh_st[pos][i], &L[i], b, i);
}
static void ql_dec_all(const int16_t *w, ql_lfo *L) { for (int i = 0; i < 4; i++) ql_dec(w, i, &L[i]); }
/* decoded + LFO-modulated parameters for the shared states of a page (src: shared values, per-voice LFOs: last voice) */
static void ql_dec_shared(const int16_t *w, int pos, ql_lfo *L)
{
    int16_t src[4];
    ql_dec_all(w, L);
    for (int i = 0; i < 4; i++) src[i] = ql_shared(pos, &L[i]) ? sh_st[pos][i].val : disp_all[pos][i];
    ql_mod(L, src);
}

/* audio thread: the words decoded once per change (per page), not per voice / pass / block. has_mod: some LFO
 * targets another one (the copies then get ql_mod). Synced rates follow the tempo, so they are refreshed. */
static struct { int16_t w[8]; uint8_t valid, has_mod, any_sync; ql_lfo L[4]; } qc[2];
static const ql_lfo *ql_cached(const int16_t *w, int pos)
{
    int i, same = qc[pos].valid;
    for (i = 0; same && i < 8; i++) same = qc[pos].w[i] == w[i];
    if (!same) {
        for (i = 0; i < 8; i++) qc[pos].w[i] = w[i];
        ql_dec_all(qc[pos].w, qc[pos].L);
        qc[pos].has_mod = qc[pos].any_sync = 0;
        for (i = 0; i < 4; i++) {
            if (ql_is_lfo(&qc[pos].L[i]) && qc[pos].L[i].tl != i && qc[pos].L[i].a128) qc[pos].has_mod = 1;
            if (qc[pos].L[i].sync) qc[pos].any_sync = 1;
        }
        qc[pos].valid = 1;
    } else if (qc[pos].any_sync)
        for (i = 0; i < 4; i++) if (qc[pos].L[i].sync) qc[pos].L[i].inc = ql_div_inc(qc[pos].L[i].rate);
    return qc[pos].L;
}
/* cached parameters, LFO-modulated into buf when needed; src: shared values, per-voice LFOs from v_st[v] (v >= 0) or
 * the last voice (v < 0) */
static const ql_lfo *ql_params(const int16_t *w, int pos, int v, ql_lfo *buf)
{
    const ql_lfo *C = ql_cached(w, pos);
    if (!qc[pos].has_mod) return C;
    int16_t src[4];
    for (int i = 0; i < 4; i++) {
        buf[i] = C[i];
        src[i] = ql_shared(pos, &C[i]) ? sh_st[pos][i].val : (v >= 0 ? v_st[v][i].val : disp_all[pos][i]);
    }
    ql_mod(buf, src);
    return buf;
}

int ql_apply_synth(void *self, const int16_t *w, int pass)
{
    if (pass != 0 && pass != 1) return 0;                         /* synth knobs 0, envelope 1: nothing else here */
    if (!LFO_ON(0)) return 0;                                     /* LFO off: nothing runs, nothing applied */
    int engine = SYNTH_ENGINE();
    int applied = 0, v = 0;
    for (int k = 0; k < 6; k++) if (SYNTH_LFO_OBJ[k] == self) { v = k; break; }
    ql_lfo Lb[4], Lvb[4];
    const ql_lfo *L = ql_params(w, 0, -1, Lb);                    /* shared states' parameters */
    const ql_lfo *Lv = ql_params(w, 0, v, Lvb);                   /* this voice's parameters (its own LFO values) */
    if (pass == 0) {
        /* learn the block length: each voice object is called once per block while active */
        uint32_t now = READ_CYCLES(), d = now - voice_cyc[v];
        if (d > cpb / 2 && d < cpb * 2) cpb += ((int32_t)(d - cpb)) / 16;
        voice_cyc[v] = now;
        ql_advance_pos(0, L);
        for (int i = 0; i < 4; i++) if (!ql_shared(0, &Lv[i])) { ql_step(&v_st[v][i], &Lv[i], 65536, i); disp_all[0][i] = v_st[v][i].val; }
    }
    for (int i = 0; i < 4; i++) {
        int sh = ql_shared(0, &L[i]), g = ql_group(&L[i]);
        int a128 = sh ? L[i].a128 : Lv[i].a128;
        if (!a128 || g == QL_FX || ql_is_lfo(&L[i])) continue;
        int16_t mod = ql_out(sh ? sh_st[0][i].val : v_st[v][i].val, a128);
        if (ql_p2(&L[i])) {                                       /* engine page 2: plugin engines only */
            if (pass == 0 && ENGINE_PLUGIN(((int16_t *)STAGE_PARAMS)[4])) {
                int16_t *pw = (int16_t *)STAGE_PARAMS + 5 + ql_dest(&L[i]);
                ql_p2_mod(pw, mod, 1); applied = 1;
                ql_p2_shown[0][ql_dest(&L[i])] = *pw; ql_p2_at[0][ql_dest(&L[i])] = READ_CYCLES(); ql_p2_ok[0][ql_dest(&L[i])] = 1;
            }
        } else if (g == QL_SYNTH && pass == 0) { KNOB_RANGE_SYNTH(ql_dest(&L[i]), &mod, engine, STAGE_PARAMS); applied = 1; }
        else if (g == QL_ENV && pass == 1) { ENV_MOD(ql_dest(&L[i]), &mod, STAGE_PARAMS); applied = 1; }
    }
    return applied;
}

/* drums: pass 3 = drum engine knobs (per drum voice), pass 4 = drum envelope. Shared states by elapsed time. */
static int ql_drum(const int16_t *w, int pass)
{
    if (pass != 3 && pass != 4) return 0;
    if (!LFO_ON(1)) return 0;
    ql_lfo Lb[4]; const ql_lfo *L = ql_params(w, 1, -1, Lb);
    ql_advance_pos(1, L);
    int engine = DRUM_ENGINE(), applied = 0;
    for (int i = 0; i < 4; i++) {
        int g = ql_group(&L[i]);
        if (!L[i].a128 || g == QL_FX || ql_is_lfo(&L[i]) || ql_p2(&L[i])) continue;   /* (no engine page 2 on drums) */
        int16_t mod = ql_out(sh_st[1][i].val, L[i].a128);
        int k = ql_dest(&L[i]);
        if (g == QL_SYNTH && pass == 3) { KNOB_RANGE_DRUM(k, &mod, engine, STAGE_PARAMS); applied = 1; }
        else if (g == QL_ENV && pass == 4) { FX_KNOB_MOD(&mod, DRUM_ENV_RANGE(k), FX_PARAM(k)); applied = 1; }
    }
    return applied;
}
int ql_apply_drum(void *self, const int16_t *w, int pass) { (void)self; return ql_drum(w, pass); }

int ql_apply_fx(void *self, const int16_t *w, int pass)
{
    (void)self;
    if (pass == 3 || pass == 4) return ql_drum(w, pass);         /* drum engine / envelope via this object */
    if (pass != 2 && pass != 5) return 0;                         /* 2 = synth FX, 5 = drum FX */
    int pos = pass == 5;
    if (!LFO_ON(pos)) return 0;
    int fxtype = pos ? DRUM_FX_TYPE() : SYNTH_FX_TYPE();
    int applied = 0;
    ql_lfo Lb[4]; const ql_lfo *L = ql_params(w, pos, -1, Lb);
    ql_advance_pos(pos, L);
    for (int i = 0; i < 4; i++) {
        if (ql_group(&L[i]) != QL_FX || ql_is_lfo(&L[i]) || !L[i].a128) continue;
        int16_t mod = ql_out(sh_st[pos][i].val, L[i].a128);
        int k = ql_dest(&L[i]);
        if (ql_p2(&L[i])) {                                       /* FX page 2: plugin FX only */
            if (!FX_PLUGIN(*FX_PARAM(4))) continue;
            ql_p2_mod(FX_PARAM(5 + k), mod, 0);
            ql_p2_shown[1 + pos][k] = *FX_PARAM(5 + k); ql_p2_at[1 + pos][k] = READ_CYCLES(); ql_p2_ok[1 + pos][k] = 1;
        } else FX_KNOB_MOD(&mod, FX_RANGE(fxtype, k), FX_PARAM(k));
        applied = 1;
    }
    return applied;
}

/* page-2 word k (0..2) as a screen should show it: the LFO-modulated value while the quad LFO applies it (within
 * ~32 blocks), else the stored word. fx 0: synth engine; fx 1: FX at pos (0 synth, 1 drum; master: no quad) */
int16_t ql_page2_live(int fx, int pos, int k, int16_t stored)
{
    if (k < 0 || k > 2 || (fx && pos != 0 && pos != 1)) return stored;
    int g = fx ? 1 + pos : 0;
    if (ql_p2_ok[g][k] && READ_CYCLES() - ql_p2_at[g][k] < cpb * 32) return ql_p2_shown[g][k];
    ql_p2_ok[g][k] = 0;
    return stored;
}
#ifndef OP1_HOST
int16_t op1_page2_live(int fx, int pos, int k, int16_t stored) { return ql_page2_live(fx, pos, k, stored); }   /* plugin export */
#endif

/* note-on (LFO object vt+0x1c, called by VoiceCtl for each note-on; v = synth voice, -1 = drums) */
static void ql_trigger(int pos, int v, const int16_t *w)
{
    for (int i = 0; i < 4; i++) {
        ql_lfo o; ql_dec(w, i, &o);
        if (o.type == QT_VOICE || o.type == QT_FREE) continue;
        if (ql_shared(pos, &o)) ql_restart(&sh_st[pos][i]);
        else if (v >= 0) ql_restart(&v_st[v][i]);
    }
}
void ql_trig_synth(void *self, const int16_t *w)
{
    if (!ql_is_quad(w)) return;                                   /* MIDI LFO: original is a bare RTS */
    int v = -1;
    for (int k = 0; k < 6; k++) if (SYNTH_LFO_OBJ[k] == self) { v = k; break; }
    ql_trigger(0, v, w);
}
void ql_trig_drum(void *self, const int16_t *w) { (void)self; if (ql_is_quad(w)) ql_trigger(1, -1, w); }
void ql_trig_fx(void *self, const int16_t *w) { (void)self; (void)w; }   /* shared restarts happen in the voice trigger */

/* ---- controls (KeyHandler thread, via dispatcher.c key/knob hooks) ---- */
static int32_t step_acc[4];
void ql_press(int pos, int knob) { ql_sel[pos & 1] = (uint8_t)(knob & 3); }
static int ql_steps(int knob, int delta)                          /* one step per detent (1024) */
{
    int32_t *acc = &step_acc[knob & 3];
    if ((*acc > 0 && delta < 0) || (*acc < 0 && delta > 0)) *acc = 0;
    *acc += delta;
    int st = *acc / 1024;
    *acc -= st * 1024;
    return st;
}
static int wrapq(int v, int n) { return ((v % n) + n) % n; }
void ql_turn(int16_t *w, int pos, int knob, int delta, int shift)
{
    int i = ql_sel[pos & 1] & 3, st = ql_steps(knob, delta);
    if (!st) return;
    if (ql_is_old(w)) {                                           /* first edit of an old set: all four to defaults */
        ql_lfo d[4];
        for (int k = 0; k < 4; k++) ql_dec(w, k, &d[k]);
        for (int k = 0; k < 8; k++) w[k] = 0;
        for (int k = 0; k < 4; k++) ql_enc(&d[k], w, k);
    }
    ql_lfo o; ql_dec(w, i, &o);
    if (!shift) switch (knob) {
        case 0: o.rate = (uint8_t)clampq(o.rate + st, 0, o.sync ? QL_DIVS - 1 : 63); break;
        case 1: o.amt = (int8_t)clampq(o.amt + st, -31, 31); break;
        case 2: {                                                 /* 27 targets (drums 24): own F / A / O skipped */
            /* turning order: each page 2 right after its page 1 (stored numbers unchanged: saved sounds keep them) */
            static const uint8_t order[QL_NTGT] = { 0, 1, 2, 3, 24, 25, 26, 4, 5, 6, 7, 8, 9, 10, 11, 27, 28, 29,
                                                    12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23 };
            int p = 0, d = st > 0 ? 1 : -1;
            while (p < QL_NTGT - 1 && order[p] != o.tgt) p++;
            for (int n = st < 0 ? -st : st; n--; ) {
                int t;
                do { p = wrapq(p + d, QL_NTGT); t = order[p]; }
                while ((t >= 12 && t < QL_P2 && (t - 12) / 3 == i) || (pos && t >= QL_P2 && t < QL_P2 + 3));
            }
            o.tgt = order[p];
            break;
        }
        default: o.type = (uint8_t)wrapq(o.type + st, QT_N); break;
    } else switch (knob) {
        case 0: o.off = (uint8_t)wrapq(o.off + st, 8); break;
        case 1: o.smooth = (uint8_t)clampq(o.smooth + st, 0, 7); break;
        case 2: o.wave = (uint8_t)clampq(o.wave + st, 0, 7); break;
        default:
            if (st > 0 && !o.sync) { o.sync = 1; o.rate = 4; }    /* sync on: 1/4 */
            else if (st < 0 && o.sync) { o.sync = 0; o.rate = 31; }   /* free: ~1 Hz */
    }
    ql_enc(&o, w, i);
}

/* ======================= screen (GUI thread) =======================
 * display/quadlfo.svg (display/gen_quadlfo_svg.py, geometry in quadlfo_geom.h): per LFO a row with a dot in the
 * encoder colour and a short lane line. Code draws per row: amount span + moving output tick on the lane, then
 * frequency / division (blue), amount in % (green), target glyph (synth: wave, drum page: drum, envelope, fx) with
 * a dot in the target knob / stage colour, and the type (red). The selected row is bright with a bar under its
 * dot, the others lilac. Shift: table of all four (offset, smoothing, waveform, sync). */
#include "quadlfo_geom.h"

static void qline(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}

static void ql_screen_init(void *scr) { (void)scr; }            /* no animated SVG nodes */

#include "../screens/stroke_font.h"
#define QT_OFF_X 30                                              /* Shift table columns (text top-left x) */
#define QT_SMOOTH_X 104
#define QT_WAVE_X 128
#define QT_SYNC_X 172
static char *ql_num(char *p, int v) { if (v >= 10) p = ql_num(p, v / 10); *p++ = (char)('0' + v % 10); *p = 0; return p; }
/* Hz of a free frequency word as text: "0.05" .. "9.99", "10.0" .. "20.0" */
static void ql_hz_text(char *t, uint32_t inc)
{
    uint32_t c = (uint32_t)(((uint64_t)inc * 4410000u) >> 39);   /* Hz * 100 = inc * 44100 * 100 / (128 * 2^32) */
    if (c >= 1000) { c /= 10; t = ql_num(t, c / 10); *t++ = '.'; ql_num(t, c % 10); }
    else { t = ql_num(t, c / 100); *t++ = '.'; *t++ = (char)('0' + c / 10 % 10); *t++ = (char)('0' + c % 10); *t = 0; }
}
/* waveform glyph: 24 x 12 px, (x, y) = left end of the centre line, Q10 */
static void ql_wave_glyph(void *r, int wave, int32_t x, int32_t y, const uint8_t *c)
{
    static const int8_t sh[4] = { 4, -3, 6, -5 };                  /* sample & hold: fixed steps */
    static const int8_t lor[2][25] = {                               /* a real Lorenz x / z stretch (docs/62), px */
        { 4, 4, 2, 1, 0, 0, 0, 0, -1, -1, -2, -4, -5, -4, -2, 0, 1, 1, 2, 2, 3, 4, 4, 3, 2 },
        { 3, 4, 3, 1, 0, -1, -2, -3, -4, -4, -3, -1, 4, 5, 3, 1, 0, -1, -2, -1, 0, 2, 3, 3, 1 } };
    int32_t px = 0, py = 0;
    for (int k = 0; k <= 24; k++) {
        uint32_t a = (uint32_t)k * (0xFFFFFFFFu / 24);
        int32_t v = wave >= 6 ? lor[wave - 6][k] * 5461 : wave == 5 ? sh[k < 24 ? k / 6 : 3] * 5000 : ql_shape(wave, a, 0, 0);
        if (wave == 2 || wave == 3) v = (wave == 2 ? 2 : -2) * ((int32_t)(a >> 17) - 16384);   /* ramps without the wrap */
        int32_t nx = x + OP1_Q10(k), ny = y - (v * OP1_Q10(6) >> 15);
        if (k) {
            int step = wave == 4 || wave == 5;
            if (step && ny != py) qline(r, px, py, px, ny, 0x400, c);                   /* vertical edges */
            qline(r, px, step ? ny : py, nx, ny, 0x400, c);
        }
        px = nx; py = ny;
    }
}
/* Shift held: one row per LFO: phase offset in degrees (blue), smoothing (green), waveform (white), sync (red).
 * The lane lines of the SVG are covered with black. */
static void ql_table(void *r, const int16_t *w, int sel)
{
    static const uint8_t black[4] = { 0, 0, 0, 0xFF };
    for (int i = 0; i < 4; i++) {
        int32_t y = OP1_Q10(ql_lane_y[i]), ty = y - OP1_Q10(6);
        ql_lfo o; ql_dec(w, i, &o);
        char t[8];
        qline(r, OP1_Q10(QL_LANE_X0 - 1), y, OP1_Q10(QL_LANE_X1 + 1), y, 0x600, black);
        qline(r, OP1_Q10(QL_LANE_MID), y - OP1_Q10(5), OP1_Q10(QL_LANE_MID), y + OP1_Q10(5), 0x600, black);
        if (i == sel)
            qline(r, OP1_Q10(QL_DOT_X - 7), y + OP1_Q10(10), OP1_Q10(QL_DOT_X + 7), y + OP1_Q10(10), 0x600, ql_col[i]);
        if (o.wave >= 6) { t[0] = 'R'; ql_num(t + 1, 28 - 2 * o.off); }   /* Lorenz: chaos rho */
        else ql_num(t, o.off * 45);
        sf_text(r, OP1_Q10(QT_OFF_X), ty, OP1_Q10(2), t, 0x400, ql_col[0]);
        ql_num(t, o.smooth);
        sf_text(r, OP1_Q10(QT_SMOOTH_X), ty, OP1_Q10(2), t, 0x400, ql_col[1]);
        ql_wave_glyph(r, o.wave, OP1_Q10(QT_WAVE_X), y, ql_col[2]);
        sf_text(r, OP1_Q10(QT_SYNC_X), ty, OP1_Q10(2), o.sync ? "SYNC" : "FREE", 0x400, o.sync ? ql_col[3] : ql_dim);
    }
}

/* target glyph, 12 x 10 px, (x, y) = left end at the row centre: synth wave, drum (circle, centre dot, stick),
 * envelope (ADSR), fx */
static void ql_glyph(void *r, int pos, int g, int32_t gx, int32_t y, const uint8_t *c)
{
    if (g == QL_SYNTH && pos) {                                   /* drum: green circle, dot, line down from it */
        int32_t cx = gx + OP1_Q10(6), rr = OP1_Q10(5);
        for (int k = 0; k < 12; k++) {
            int32_t a0 = k * 8192 / 12, a1 = (k + 1) * 8192 / 12;
            qline(r, cx + (OP1_SINE_TABLE[a0] * rr >> 15), y - (OP1_SINE_TABLE[(a0 + 2048) & 8191] * rr >> 15),
                     cx + (OP1_SINE_TABLE[a1 & 8191] * rr >> 15), y - (OP1_SINE_TABLE[(a1 + 2048) & 8191] * rr >> 15), 0x300, ql_col[1]);
        }
        qline(r, cx - 0x180, y, cx + 0x180, y, 0x500, ql_col[1]);
        qline(r, cx, y, cx, y + OP1_Q10(9), 0x300, ql_col[1]);
        return;
    }
    switch (g) {
    case QL_SYNTH:                                                   /* wave */
        qline(r, gx, y, gx + OP1_Q10(3), y - OP1_Q10(4), 0x400, c);
        qline(r, gx + OP1_Q10(3), y - OP1_Q10(4), gx + OP1_Q10(9), y + OP1_Q10(4), 0x400, c);
        qline(r, gx + OP1_Q10(9), y + OP1_Q10(4), gx + OP1_Q10(12), y, 0x400, c);
        break;
    case QL_ENV:                                                     /* ADSR */
        qline(r, gx, y + OP1_Q10(4), gx + OP1_Q10(3), y - OP1_Q10(4), 0x400, c);
        qline(r, gx + OP1_Q10(3), y - OP1_Q10(4), gx + OP1_Q10(6), y, 0x400, c);
        qline(r, gx + OP1_Q10(6), y, gx + OP1_Q10(9), y, 0x400, c);
        qline(r, gx + OP1_Q10(9), y, gx + OP1_Q10(12), y + OP1_Q10(4), 0x400, c);
        break;
    default:                                                         /* fx: the text "fx" from lines */
        qline(r, gx + OP1_Q10(2), y + OP1_Q10(5), gx + OP1_Q10(2), y - OP1_Q10(3), 0x480, c);
        qline(r, gx + OP1_Q10(2), y - OP1_Q10(3), gx + OP1_Q10(4), y - OP1_Q10(5), 0x480, c);
        qline(r, gx + OP1_Q10(4), y - OP1_Q10(5), gx + OP1_Q10(6), y - OP1_Q10(5), 0x480, c);
        qline(r, gx, y - OP1_Q10(1), gx + OP1_Q10(5), y - OP1_Q10(1), 0x480, c);
        qline(r, gx + OP1_Q10(8), y - OP1_Q10(1), gx + OP1_Q10(14), y + OP1_Q10(5), 0x480, c);
        qline(r, gx + OP1_Q10(14), y - OP1_Q10(1), gx + OP1_Q10(8), y + OP1_Q10(5), 0x480, c);
    }
}

static void ql_screen_update(void *scr, int dt)
{
    int pos = PAGE_POS();
    const int16_t *w = PAGE_WORDS(pos);
    int sel = ql_sel[pos] & 3;
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    ql_lfo L[4], Lm[4]; ql_dec_all(w, L); ql_dec_shared(w, pos, Lm);   /* text: set values, lane: modulated */
    int lfo_on = LFO_ON(pos);
    uint32_t b = blocks_q16(READ_CYCLES(), ph_cyc_all[pos]);
    if (lfo_on && b > (4u << 16)) ql_advance_pos(pos, Lm);       /* audio idle: advance here (LFO off: ticks centred) */
    if (SHIFT_HELD()) { ql_table(r, w, sel); return; }
    for (int i = 0; i < 4; i++) {
        const ql_lfo *o = &L[i];
        int32_t y = OP1_Q10(ql_lane_y[i]), ty = y - OP1_Q10(6);
        int on = i == sel;
        const uint8_t *tc = on ? 0 : ql_lilac;
        /* amount span: centre +- |amount| (dim lane colour), output tick */
        int a = Lm[i].a128 < 0 ? -Lm[i].a128 : Lm[i].a128;
        int32_t span = (int32_t)(((int64_t)a * OP1_Q10(QL_LANE_HALF)) >> 7);
        if (span) {
            uint8_t dc[4];
            for (int k = 0; k < 3; k++) dc[k] = (uint8_t)(ql_col[i][k] * 3 / 8);
            dc[3] = 0xFF;
            qline(r, OP1_Q10(QL_LANE_MID) - span, y, OP1_Q10(QL_LANE_MID) + span, y, QL_SPAN_W, dc);
        }
        int16_t out = lfo_on ? ql_out(ql_shared(pos, o) ? sh_st[pos][i].val : disp_all[pos][i], Lm[i].a128) : 0;
        int32_t x = OP1_Q10(QL_LANE_MID) + (int32_t)(((int64_t)out * OP1_Q10(QL_LANE_HALF)) >> 15);
        qline(r, x, y - OP1_Q10(QL_TICK_H), x, y + OP1_Q10(QL_TICK_H), 0x800, ql_col[i]);
        if (on) qline(r, OP1_Q10(QL_DOT_X - 7), y + OP1_Q10(10), OP1_Q10(QL_DOT_X + 7), y + OP1_Q10(10), 0x600, ql_col[i]);
        /* frequency / division, amount % */
        char t[8];
        if (o->sync) { const char *n = ql_div_name[o->rate]; int k = 0; while ((t[k] = n[k])) k++; }
        else ql_hz_text(t, o->inc);
        sf_text(r, OP1_Q10(QL_FREQ_X), ty, OP1_Q10(2), t, 0x400, tc ? tc : ql_col[0]);
        int pc = o->amt * 100 / 31; char *p = t;
        if (pc < 0) { *p++ = '-'; pc = -pc; }
        ql_num(p, pc);
        sf_text(r, OP1_Q10(QL_AMT_X), ty, OP1_Q10(2), t, 0x400, tc ? tc : ql_col[1]);
        /* target: glyph + dot in the knob / stage colour; LFO targets: F / A / O + dot in the target LFO's colour */
        int dcol = ql_dest(o);
        if (ql_is_lfo(o)) {
            static const char *const kp[3] = { "F", "A", "O" };
            dcol = (o->tgt - 12) / 3;
            sf_text(r, OP1_Q10(QL_GLYPH_X + 2), ty, OP1_Q10(2), kp[(o->tgt - 12) % 3], 0x400, tc ? tc : ql_white);
        } else ql_glyph(r, pos, ql_group(o), OP1_Q10(QL_GLYPH_X), y, tc ? tc : ql_white);
        int32_t dx = OP1_Q10(QL_TDOT_X);                                 /* dot: two crossing bars (no zero-length lines) */
        if (ql_p2(o)) {                                                  /* page 2: two dots above each other ( : ) */
            for (int k = -1; k <= 1; k += 2) {
                int32_t yy = y + k * OP1_Q10(5);                         /* same size as the single dot */
                qline(r, dx - OP1_Q10(3), yy, dx + OP1_Q10(3), yy, OP1_Q10(4), ql_col[dcol]);
                qline(r, dx, yy - OP1_Q10(3), dx, yy + OP1_Q10(3), OP1_Q10(4), ql_col[dcol]);
            }
        } else {
            qline(r, dx - OP1_Q10(3), y, dx + OP1_Q10(3), y, OP1_Q10(4), ql_col[dcol]);
            qline(r, dx, y - OP1_Q10(3), dx, y + OP1_Q10(3), OP1_Q10(4), ql_col[dcol]);
        }
        sf_text(r, OP1_Q10(QL_TYPE_X), ty, OP1_Q10(2), ql_type_name[o->type], 0x400, tc ? tc : ql_col[3]);
    }
}

const op1_screen_t SCREEN_quadlfo = { ql_screen_init, ql_screen_update };
