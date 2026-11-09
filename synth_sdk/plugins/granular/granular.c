#pragma GCC optimize ("no-tree-loop-distribute-patterns") /* no implicit libc calls */
/* OP1R port of displayname808 Granular 0.10.0 (original engine: pcppcp).
 * GPL-3.0-or-later with the OP1R firmware permission; see LICENSE and NOTICE.
 * Firmware boundary and tests added for this port. No Opus runtime required. */
#include "op1_plugin.h"
#include "bridge.h"
typedef uint32_t u32; typedef int32_t s32; typedef uint16_t u16; typedef uint8_t u8;
#define BLOCK 128
#define VOICES 6
#define MAX_GRAINS 4
#define FRAC 10
#define INC_C4 25480370u
#define WIN_BITS 11
#define SHAPES 6
#define BAR_N 80
static struct { volatile s32 len; volatile int slot; short peak[BAR_N], top; } S = {0,-1,{0},1};
static u32 sample_generation, voice_generation[6];
static const uint8_t *sample_seen;
static u32 sample_hash;
static volatile u8 sync_on;
static const short *slot_sample(int slot) { const uint8_t *sb=op1_sample_buffer(slot); return sb?op1_sample_data(sb):0; }
static s32 recorded_length(const uint8_t *sb) {
    if (!sb) return 0;
    const short *p=op1_sample_peaks(sb);
    for(int i=OP1_PEAK_BUCKETS-1;i>=0;i--) if(p[i]>96) {
        s32 n=(i+1)*256; return n>OP1_SAMPLE_MAX?OP1_SAMPLE_MAX:n;
    }
    return 0;
}
static void scan(void) {
    const int slot=op1_current_slot()&7;
    const uint8_t *sb=op1_sample_buffer(slot);
    const short *pk=sb?op1_sample_peaks(sb):0;
    const s32 len=recorded_length(sb); u32 hash=2166136261u;
    if(pk) for(int i=0;i<OP1_PEAK_BUCKETS;i++) hash=(hash^(u16)pk[i])*16777619u;
    if(sb!=sample_seen || hash!=sample_hash || slot!=S.slot) {sample_generation++; sample_seen=sb; sample_hash=hash;}
    S.top=1;
    const int nb=(len+255)/256;
    for(int i=0;i<BAR_N;i++) {
        int m=0; int a=i*nb/BAR_N,b=(i+1)*nb/BAR_N;
        for(int j=a;j<=b && j<nb;j++) if(pk && pk[j]>m)m=pk[j];
        S.peak[i]=(short)m; if(m>S.top)S.top=(short)m;
    }
    S.slot=slot; S.len=len;
}
static s32 sample_len(int slot) { return S.slot==(slot&7)?S.len:0; }
typedef struct {
    u32 pos;            /* 22.10 sample position */
    u32 rate;           /* 22.10 a sample */
    u32 wph;            /* window phase, 0..2^31 = the half sine */
    u32 winc;           /* its step (0: free) */
    s32 left;           /* the samples it still sounds (worked out when it starts) */
    s32 sh;             /* its shape (0.10.0): kept for its life, so a shape change never cuts into a grain */
} grain_t;

typedef struct {
    grain_t g[MAX_GRAINS];
    s32 next;           /* samples to the next grain */
    s32 len;            /* the recording's usable length */
    u32 rng;
    s32 index;          /* 0-5 */
    volatile u32 blocks;    /* renders: the page shows a voice's grains only while it renders */
    /* 0.8.6: what a block works out from the knobs and the pitch, kept while they stay the same (they mostly do): the
     * software divisions and 64-bit products it took cost more than the grains of a sparse voice */
    u32 k_size;         /* (u16)par[1] | (u16)par[3] << 16 the values below are for (0xFFFFFFFF: none yet) */
    s32 size, interval, gain;
    u32 winc, life;     /* a grain's window step and its life in samples, from the size */
    u32 r_pitch, r_base;    /* pitch[0] and the base the rate is for */
    s32 r_st;           /* and the semitones (-1000: none yet) */
    u32 rate;
    u32 k_sync;         /* 0.10.0: the clock sync the size is for: 0 off, else the tempo << 4 | the step + 1 */
} gvoice_t;

static gvoice_t *voice_of[VOICES];
/* the block's sum of a voice's grains: in fast memory when there is room (0.8.6; every grain adds into it every
 * sample, and from ordinary memory its 16 lines missed the cache each block), else here */
static s32 acc_mem[BLOCK + 1];
static s32 *acc_buf = acc_mem + 1;      /* a word before it (0.9.3): grain_loop_asm's first store is acc[-1], its own value */
#include "window_tables.h"
static const short *const WIN[SHAPES] = {window_tables[0],window_tables[1],window_tables[2],window_tables[3],window_tables[4],window_tables[5]};
static const char *const SHAPE_NAME[SHAPES] = {"hann","gauss","tukey","tri","pluck","swell"};
static inline int shape_of(s32 k) { return k <= 0 ? 0 : k >= 32767 ? 5 : (int)((u32)k*6>>15); }
static const short *wfast;
static int wshape = -1;
/* pitch (0.8.0, shift + blue, knob 5): -24 to +24 semitones, 512 a semitone around 16384; 0 (every older sound) is
 * none. The grains' rate times 2^(st/12), Q16 from a table made at the start. */
#define PITCH_ST 24

/* OP1R's secondary-word editor starts an unset word at 8192 and moves
 * 256 per physical detent. Zero remains neutral for existing presets. */
static inline int pitch_st(s32 word)
{
    if(word<=0)return 0;
    int d=word-8192;
    int st=d<0?-((-d+128)/256):(d+128)/256;
    return st < -24 ? -24 : st > 24 ? 24 : st;
}

static inline u32 rnd(gvoice_t *v) { v->rng = v->rng * 1664525u + 1013904223u; return v->rng; }
static inline short sat16(s32 x)                                    /* as a min and a max: no branch (0.8.7) */
{
    x = x < 32767 ? x : 32767;
    x = x > -32768 ? x : -32768;
    return (short)x;
}
/* floor(x g / 2^sh) without overflow, for 0 <= g < 2^15 and |x >> sh| < 2^15: x's high part times g, plus its low
 * part's product shifted (x x g = (x >> sh) x g x 2^sh + (x & mask) x g, the low part 0 or more); two 16 x 16 products */
static inline s32 mul_shr_s(s32 x, short g, int sh)
{
    return (s32)(short)(x >> sh) * g + (((s32)(short)(x & ((1 << sh) - 1)) * g) >> sh);
}

static void v_init(void *voice, int index)
{
    gvoice_t *v = voice;
    for (int i = 0; i < MAX_GRAINS; i++) v->g[i].winc = 0;
    v->next = 0; v->len = 0; v->rng = 0x9E3779B9u * (u32)(index + 1); v->index = index; v->blocks = 0;
    v->k_size = 0xFFFFFFFFu; v->r_st = -1000;                           /* nothing kept yet */
    if (index >= 0 && index < VOICES) voice_of[index] = v;
}

static void v_note_on(void *voice, const short *knobs)
{
    gvoice_t *v = voice;
    (void)knobs;
    for (int i = 0; i < MAX_GRAINS; i++) v->g[i].winc = 0;
    v->next = 0;
    v->len = sample_len(op1_current_slot());
    v->rng ^= (u32)(v->index + 1) * 2654435761u + 12345u;
}



/* ---------------------------------------------------------------- the cpu meter (0.9.5) --------------------- */

/* Setting "cpu meter" on: the audio work timed by the CPU's cycle counter, a block at a time, and shown on the page:
 * now, a second's average and the worst block since it went on, and what that worst block did. Off (the default):
 * a test of one byte where it would time. */
#define meter 0 /* Use OP1R clock + T1 resource monitor. */
enum { M_TOTAL, M_LOOP, M_SAMPLES, M_LINES, M_SPAWNS, M_RECALC, M_SLOWWIN, M_VOICES, M_N };
static u32 m_cur[M_N], m_worst[M_N], m_last[M_N];   /* the audio writes them; the page reads (a torn read: a frame) */
static u32 m_acc, m_n, m_avg;
static inline u32 cyc(void)
{
#ifdef __bfin__
    u32 c;
    __asm__ __volatile__("%0 = CYCLES;" : "=d"(c));
    return c;
#else
    return 0;
#endif
}


/* floor(n / d) for n < d x 2^32 (the quotient fits 32 bits), a bit a step (0.9.5): the same as the 64-bit division,
 * which libgcc does in ~2,800 cycles (458 instructions and up to 17 calls) and this in about 32 x 5 */
static u32 div64_32(unsigned long long n, u32 d)
{
    u32 hi = (u32)(n >> 32), lo = (u32)n, q = 0;
    if (hi >= d) return (u32)(n / d);                                   /* never here: the rate's quotient is small */
    for (int i = 0; i < 32; i++) {
        const u32 top = hi >> 31;
        hi = hi << 1 | lo >> 31;
        lo <<= 1;
        q <<= 1;
        if (top || hi >= d) { hi -= d; q |= 1; }
    }
    return q;
}

/* (u64)a * b >> sh as op1r's 64-bit products had it, for a < 2^16 and b < 2^31 >> 0, in 32-bit steps: a x (b's high
 * part) plus the low part's product shifted (the same bits; the Blackfin has no 64-bit multiply, __muldi3 is a call) */
static inline u32 mul_shr(u32 a, u32 b, int sh, u32 lomask) { return a * (b >> sh) + ((a * (b & lomask)) >> sh); }

static void spawn(gvoice_t *v, const short *par, u32 rate, s32 size, int shp)
{
    int i;
    for (i = 0; i < MAX_GRAINS; i++) if (!v->g[i].winc) break;
    if (i == MAX_GRAINS) return;                                        /* all busy: this grain is skipped */
    if (meter) m_cur[M_SPAWNS]++;
    grain_t *g = &v->g[i];
    const s32 len = v->len;
    s32 start = (s32)((u32)len < 0x80000000u ? mul_shr((u16)par[0], (u32)len, 15, 0x7FFF)
                                              : (((unsigned long long)(u16)par[0] * (u32)len) >> 15));
    const unsigned long long span64 = ((unsigned long long)(u32)size * rate) >> FRAC;
    const s32 span = span64 > (u32)len ? len : (s32)span64;
    start -= span >> 1;                                                 /* centred: the window's peak lands on the position */
    const s32 spray = (s32)(((u32)par[2] * 66150u) >> 15);             /* up to 1.5 s */
    if (spray) start += (s32)(rnd(v) % (u32)(2 * spray + 1)) - spray;
    /* kept inside the sample, whole (0.7.2): near either end the grain moves in rather than wrapping round to the
     * other end (heard as a click at position 0, and drawn at the end) or being cut off before its window ends */
    if (start > len - 2 - span) start = len - 2 - span;
    if (start < 0) start = 0;
    u32 r = rate;                                                       /* +-1 % detune with the spray up */
    if (par[2] > 4096) {
        const s32 dt = (s32)(rnd(v) >> 22) - 512;
        r = (u32)((s32)rate + ((s32)(rate >> 10) * dt * (par[2] >> 8) >> 12));
    }
    g->pos = (u32)start << FRAC;
    g->rate = r;
    g->wph = 0;
    g->winc = v->winc;                                                  /* 2^31 / size: the window spans size samples */
    g->sh = shp;
    /* its life, once (0.1.0 tested both ends every sample): the window's end, ceil(2^31 / winc) samples (v->life), or
     * the recording's, the samples while pos >> FRAC < len - 1: ceil(d / r), d the room left. That is less than the
     * life only when d <= (life - 1) x r, so the division is done only then (0.8.6: near the sample's end) */
    const s32 life = (s32)v->life;
    const u32 end = (u32)(len - 1) << FRAC;
    if (g->pos >= end) g->left = 0;
    else {
        const u32 d = end - g->pos, l1 = (u32)life - 1;
        if (r < 65536u && l1 < 65536u && d > l1 * r) g->left = life;   /* l1 x r < 2^32: exact */
        else { const s32 room = (s32)((d + r - 1) / r); g->left = room < life ? room : life; }
    }
    if (!g->left) g->winc = 0;
}

/* a grain over n samples (n <= left) added into acc */
/* the loop by hand (grain_loop.S, 0.9.3): the same numbers as grain_c below in 16 instructions a sample for its 31;
 * used only when the start's self-test found the two agree on every sample (use_asm) */
struct gk { u32 pos, rate, wph, winc; const short *data; const short *win; s32 *acc; s32 n; };
#ifdef __bfin__
void grain_loop_asm(struct gk *k);
#endif
static u8 use_asm;

static __attribute__((noinline)) void grain_c(grain_t *g, const short *data, s32 *acc, int n, const short *win)
{
    u32 pos = g->pos, wph = g->wph;
    const u32 rate = g->rate, winc = g->winc;
    /* 0.8.6, the same output in fewer cycles:
     * - the interpolation as (a x 1024 + b x f - a x f) >> 10: the same number as a + ((b - a) x f >> 10), but 16 x 16
     *   products (a cycle each) for a 32 x 32 one (several on the Blackfin; b - a needs 17 bits);
     * - the window's product 16 x 16 too: the interpolated sample lies between a and b, so it is a short;
     * - the next sample's addresses worked out a sample ahead: an address register loaded from a data register stalls
     *   the instructions just after it that use it, twice a sample before */
    const short *p = data + (pos >> FRAC), *w = win + (wph >> (31 - WIN_BITS));
    s32 *const end = acc + n;
    while (acc != end) {
        const short a = p[0], b = p[1], ww = *w;
        const short f = (short)(pos & ((1 << FRAC) - 1));
        pos += rate;
        wph += winc;
        if (acc + 1 != end) { p = data + (pos >> FRAC); w = win + (wph >> (31 - WIN_BITS)); }
        const short s = (short)(((s32)a * (1 << FRAC) + (s32)b * f - (s32)a * f) >> FRAC);
        *acc++ += ((s32)s * ww) >> 15;
    }
    g->pos = pos; g->wph = wph;
}

/* a grain over n samples (1 <= n <= left) added into acc */
static void grain_run(grain_t *g, const short *data, s32 *acc, int n, const short *win)
{
    u32 t0 = 0;
    const u32 p0 = g->pos;
    if (meter) t0 = cyc();
#ifdef __bfin__
    if (use_asm) {
        struct gk k = { g->pos, g->rate, g->wph, g->winc, data, win, acc, n };
        grain_loop_asm(&k);
        g->pos = k.pos; g->wph = k.wph;
    } else
#endif
        grain_c(g, data, acc, n, win);
    if (meter) {                                                        /* its time, samples and sample-memory lines */
        m_cur[M_LOOP] += cyc() - t0;
        m_cur[M_SAMPLES] += (u32)n;
        m_cur[M_LINES] += (((g->pos >> FRAC) + 1) >> 4) - ((p0 >> FRAC) >> 4) + 1;
    }
    if (!(g->left -= n)) g->winc = 0;                                   /* the grain is done */
}

/* The start's self-test of grain_loop_asm against grain_c (0.9.3): every grain shape, lengths 1 to 128 (the loop's
 * one-sample path too), slow to fast rates, the window from both ends, full-scale and edge samples; the sums and
 * the positions compared word for word. Any difference, and the engine keeps the C loop. */
static int asm_selftest(void)
{
#ifdef __bfin__
    static short td[512];
    static s32 ta[BLOCK + 1], tb[BLOCK + 1];
    u32 x = 0x2545F491u;
    for (int i = 0; i < 512; i++) {
        x = x * 1664525u + 1013904223u;
        td[i] = (short)(i % 37 == 0 ? 32767 : i % 41 == 0 ? -32768 : (s32)(x >> 16) - 32768);
    }
    static const s32 NS[] = { 1, 2, 3, 7, 64, 127, 128 };
    static const u32 RATES[] = { 16, 300, 512, 1023, 1024, 3000 };
    for (int sh = 0; sh < SHAPES; sh++)
        for (unsigned ni = 0; ni < sizeof NS / sizeof NS[0]; ni++)
            for (unsigned ri = 0; ri < sizeof RATES / sizeof RATES[0]; ri++) {
                const s32 n = NS[ni];
                const u32 rate = RATES[ri];
                if ((unsigned long long)rate * (u32)n > 500ull << FRAC) continue;    /* within the test's 512 samples */
                const u32 size = (u32)n + 7 * ri + 441 * (ni & 1);
                grain_t g = { 777u + 1000u * ri, rate, 0, 0x80000000u / size, n, 0 };
                if (ni & 2) g.wph = 0x80000000u - g.winc * (u32)n;      /* the window's far end */
                grain_t h = g;
                const int off = 1 + (int)(ni * 5 % (BLOCK + 1 - n));
                for (int i = 0; i <= BLOCK; i++) ta[i] = tb[i] = (s32)(i * 40503u) - 2000000;
                struct gk k = { g.pos, g.rate, g.wph, g.winc, td, WIN[sh], ta + off, n };
                grain_loop_asm(&k);
                grain_c(&h, td, tb + off, n, WIN[sh]);
                if (k.pos != h.pos || k.wph != h.wph) return 0;
                for (int i = 0; i <= BLOCK; i++) if (ta[i] != tb[i]) return 0;
            }
    return 1;
#else
    return 0;
#endif
}

/* the block out, through the gains (in a function of its own: inside v_render the compiler, seeing the gains are
 * small, made the products 32 x 32 again) */
static __attribute__((noinline)) void gains_out(short *out, s32 *acc, short gain, short sg)
{
    /* each sum cleared as it is read (0.8.8, as op1r's): the sum is ready at 0 for the next voice without a pass of
     * its own, 128 stores less a voice */
    if (sg == 8192)                                                     /* 1x: (x x 8192) >> 13 is x */
        for (int n = 0; n < BLOCK; n++) { const s32 a = acc[n]; acc[n] = 0; out[n] = sat16(mul_shr_s(a, gain, 15)); }
    else
        for (int n = 0; n < BLOCK; n++) { const s32 a = acc[n]; acc[n] = 0; out[n] = sat16(mul_shr_s(mul_shr_s(a, gain, 15), sg, 13)); }
}

/* ---------------------------------------------------------------- clock sync (0.10.0) ------------------------ */

/* White push on the T1 page: the grain size in time with the OP-1's tempo (core 2.21's tempo service). Green then
 * steps through note lengths instead of milliseconds: the middle of its turn a beat (a quarter note), turned down
 * divisions of it, up multiples. Kept in the sound (its engine data), so a preset brings it back. */
#define SYNC_STEPS 15
static const u16 SYNC_TICKS[SYNC_STEPS] = {                             /* 96 ticks a beat */
    6, 8, 12, 16, 24, 32, 48, 96, 192, 288, 384, 576, 768, 1152, 1536 };
static const char *const SYNC_NAME[SYNC_STEPS] = {
    "1/64", "1/32t", "1/32", "1/16t", "1/16", "1/8t", "1/8", "1/4", "1/2", "3/4", "1 bar", "1.5 bar", "2 bar",
    "3 bar", "4 bar" };
static volatile u32 tempo_mbpm = 120000;        /* the tempo, thousandths of a BPM, as the last block found it */
static inline int sync_step(s32 k1)
{
    const int i = (int)(((u32)(k1 < 0 ? 0 : k1) * SYNC_STEPS) >> 15);
    return i >= SYNC_STEPS ? SYNC_STEPS - 1 : i;
}
/* the step's length in samples: ticks x 44100 x 60 / (bpm x 96) = ticks x 27,562,500 / mbpm; at most 12 seconds */
static s32 sync_size(int step, u32 mbpm)
{
    const u32 s = div64_32((unsigned long long)SYNC_TICKS[step] * 27562500u, mbpm);
    return s < 64 ? 64 : s > 529200 ? 529200 : (s32)s;
}
static void tempo_read(void)
{
    op1_beat_t b; op1_beat_clock(&b);
    static s32 previous_len;
    if (b.len > 0 && b.len != previous_len) {
        previous_len=b.len;
        u32 t = div64_32(43352064000000ull, (u32)b.len);
        tempo_mbpm = t < 20000 ? 20000 : t > 400000 ? 400000 : t;
    }
}
static void v_render(void *voice, short *out, const short *par, const unsigned *pitch)
{
    gvoice_t *v = voice;
    const u32 t_start = meter ? cyc() : 0;
    const int slot = op1_current_slot();
    const short *data = slot_sample(slot);
    v->blocks++;
    tempo_read();
    sync_on = ENGINE_granular.sync[slot & 7];
    const s32 newlen = sample_len(slot);
    if (v->len != newlen || voice_generation[v->index] != sample_generation) {
        for (int i=0;i<MAX_GRAINS;i++) v->g[i].winc=0;
        v->next=0; voice_generation[v->index]=sample_generation;
    }
    v->len = newlen;                                          /* every block: a new recording changes it */
    if (!data || v->len < 256) {                                        /* nothing sampled (or not looked at) yet */
        for (int n = 0; n < BLOCK; n++) out[n] = 0;
        return;
    }
    /* the keyboard's pitch, steady a block: the sample's own speed on the key it was sampled at (rate 512 = half a
     * sample a 88.2 kHz output sample, its speed: rate = 512 x f / base, f = step x 88200 / 2^32 Hz, base in
     * milli-Hz), else on C4 as op1r's */
    const u32 base = 0; /* OP1R sampler recordings use C4, as its native granular. */
    const int st = pitch_st(par[5]);
    if (pitch[0] != v->r_pitch || base != v->r_base || st != v->r_st) {    /* 0.8.6: worked out on a change only */
        u32 rate = base ? div64_32(((unsigned long long)pitch[0] * 11025000u) >> 20, base) : pitch[0] / (INC_C4 >> FRAC);
        if (meter) m_cur[M_RECALC]++;
        if (st) rate = (u32)(((unsigned long long)rate * semi_q16[st + PITCH_ST]) >> 16);
        if (rate < 16) rate = 16;
        v->rate = rate; v->r_pitch = pitch[0]; v->r_base = base; v->r_st = st;
    }
    const u32 rate = v->rate;
    const u32 ks = (u32)(u16)par[1] | (u32)(u16)par[3] << 16;
    const u32 ksync = sync_on ? tempo_mbpm << 4 | (u32)(sync_step(par[1]) + 1) : 0;
    if (ks != v->k_size || ksync != v->k_sync) {                        /* 0.8.6: as the rate */
        /* sizes are worked out in 44.1 kHz samples (the screen shows those); R246 renders voices at 88.2 kHz: twice as
         * many output samples (issue 36, as op1r's granular since op1r 57) */
        const s32 size = 2 * (ksync ? sync_size(sync_step(par[1]), tempo_mbpm) : 441 + (s32)(((u32)par[1] * 16000u) >> 15));
        const s32 ovl8 = 4 + (par[3] >> 10);                            /* overlap x 8: 4..35 */
        s32 interval = size * 8 / ovl8;
        if (interval < 64) interval = 64;
        s32 gain = ovl8 <= 8 ? 32767 : 32767 * 8 / ovl8 + 6000;        /* for the overlapping grains */
        if (gain > 32767) gain = 32767;
        v->size = size; v->interval = interval; v->gain = gain;
        v->winc = 0x80000000u / (u32)size;
        v->life = (0x80000000u + v->winc - 1) / v->winc;
        v->k_size = ks; v->k_sync = ksync;
        if (meter) m_cur[M_RECALC]++;
    }
    const s32 size = v->size, interval = v->interval, gain = v->gain;
    const s32 sg = par[7] > 0 ? par[7] : 8192;                         /* the sample gain, Q13 */
    const int shp = shape_of(par[6]);
    const int ws = wshape;                                              /* the shape in fast memory now */
    if (meter && !(wfast && ws == shp)) m_cur[M_SLOWWIN]++;             /* the window from ordinary memory */
    s32 next = v->next;
    s32 *const acc = acc_buf;           /* the voices render one after another; it is 0 here: gains_out leaves it so */
    /* in stretches between grain starts: a grain starts where the countdown runs out, at that stretch's first
     * sample; every grain then runs over the whole stretch at once */
    for (int n = 0; n < BLOCK; ) {
        if (--next <= 0) { spawn(v, par, rate, size, shp); next = interval; }
        const int seg = next < BLOCK - n ? next : BLOCK - n;
        for (int i = 0; i < MAX_GRAINS; i++) {
            grain_t *g = &v->g[i];
            if (g->winc) grain_run(g, data, acc + n, g->left < seg ? g->left : seg,
                                   wfast && ws == g->sh ? wfast : WIN[g->sh]);   /* the shape it started with */
        }
        next -= seg - 1;
        n += seg;
    }
    /* the gains as op1r's, (acc x gain) >> 15, then (x sg) >> 13, but never wrapping (0.8.7): loud grains overlapping
     * sum past 16 bits, and the 32-bit products wrapped round (a sign-flipped crackle where it should clip). Now in
     * exact halves (mul_shr_s), the same numbers wherever the products fitted, the true ones where they did not */
    /* Match native OP1R's velocity placement and avoid a second output pass. */
    const s32 vel_gain=(gain*op1_voice_vel_gain(OP1_VOICE(v->index)))>>15;
    gains_out(out, acc, (short)vel_gain, (short)sg);                        /* both 1-32767 */
    v->next = next;
    if (meter) { m_cur[M_TOTAL] += cyc() - t_start; m_cur[M_VOICES]++; }
}


#define GR_RGB(c) (c)
#define GR_COLOR_E(i) (screen_col[(i)])
#define GR_COLOR_TEXT 0xFFFFFF
static const int screen_col[4]={0x4D9EFF,0x00ED95,0xFFFFFF,0xFF3A5D};
static u32 gui_tenths;
static void shown_knobs(short k[8]) {
    short q[8]; const short *w=op1_knobs_shown(0,0,op1_synth_knobs(OP1_SCREEN_SLOT),q);
    for(int i=0;i<8;i++) k[i]=w[i];
    op1_synth_knobs_live(OP1_SCREEN_SLOT,k);
}
#ifndef OP1_HOST
#define GR_TEXT_SIZE 0xA4
#define gr_text_ctor(o,x,y,s,c) (((void *(*)(void *,s32,s32,s32,const u8 *,s32))0x01A61904)(o,x,y,s,c,0))
#define gr_text_set(o,s) (((void (*)(void *,const char *))0x01A61AAC)(o,s))
#define gr_text_draw(o,r,m) (((void (*)(void *,void *,const s32 *))0x01A6259C)(o,r,m))
#else
#define GR_TEXT_SIZE OP1_TEXT_SIZE
#define gr_text_ctor(o,x,y,s,c) op1_text_ctor(o,x,y,s,c)
#define gr_text_set(o,s) op1_text_set(o,s)
#define gr_text_draw(o,r,m) op1_text_draw(o,r,m)
#endif
static u32 text_obj[10][GR_TEXT_SIZE/4];
static char text_seen[10][64]; static u8 text_ready[10];
static void page_text(int i,int x,int y,int sz,int c,const char *s) {
    const u8 rgba[4]={(u8)(c>>16),(u8)(c>>8),(u8)c,255};
    static const s32 ident[9]={1024,0,0,0,1024,0,0,0,1024};
    if(!text_ready[i]) {gr_text_ctor(text_obj[i],x*1024,y,sz,rgba);text_ready[i]=1;}
    int same=1; for(int j=0;j<64;j++) {if(s[j]!=text_seen[i][j]){same=0;break;}if(!s[j])break;}
    if(!same){gr_text_set(text_obj[i],s);int j=0;for(;j<63&&s[j];j++)text_seen[i][j]=s[j];text_seen[i][j]=0;}
    gr_text_draw(text_obj[i],op1_raster(),ident);
}
static u32 tempo_phase(u32 ticks96) {
    op1_beat_t b;op1_beat_clock(&b); if(b.len<=0)return 0;
    /* The native clock exposes one beat; long divisions retain that beat marker. */
    u32 elapsed=(u32)b.len-(u32)(b.next<0?0:b.next>b.len?b.len:b.next);
    unsigned long long len=(unsigned long long)(u32)b.len*ticks96/96;
    return len?(u32)(((unsigned long long)elapsed<<32)/len):0;
}
#define U(v) ((v) * 256)                         /* units x 256 */
#define C_POS   GR_RGB(0x4D9EFF)
#define C_SIZE  GR_RGB(0x00ED95)
#define C_WHITE GR_RGB(0xFFFFFF)
#define C_GRAIN GR_RGB(0xFF3A5D)
/* the columns' text: colour ids (page text takes no RGB); the white column in the font's white, as the white wave */
static const int TXT4[4] = { 0x4D9EFF, 0x00ED95, 0xFFFFFF, 0xFF3A5D };
/* shift + an encoder: green the grain shape, orange the sample gain (blue, white none) */

/* when each encoder was last turned or pushed (ms): its label and value fade away when not in use */
/* Use of an encoder: the key thread sets poke[i] at every turn (opus_on_knob, any page, shift or not) and push; the
 * page stamps it in its own time, tenths of a second from the OP-1's clock (opus_clock), the frames between its
 * seconds dividing each second. Nothing shows until an encoder is used. */
static volatile u8 poke[4];
#define SHOW_T  30                             /* tenths of a second shown after a use, then gone (no dim) */

static u32 tenths(void) { return gui_tenths; }
static void line(int x0,int y0,int x1,int y1,int w,int c) {
    uint8_t rgba[4]={(u8)(c>>16),(u8)(c>>8),(u8)c,255};
    op1_pt a={x0*4,y0*4},b={x1*4,y1*4};
    op1_raster_line(op1_raster(),&a,&b,w*4,rgba);
}
static char *num(char *p, s32 v, int dec)      /* v in 1/10^dec: 125, 2 -> "1.25" */
{
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v || n <= dec);
    while (n) { *p++ = t[--n]; if (n == dec && dec) *p++ = '.'; }
    return p;
}
static char *cat(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static void ms_text(char *s, s32 samples)      /* "120ms", "1.25s" */
{
    const s32 ms = (samples * 10 + 220) / 441;
    char *p = ms < 1000 ? cat(num(s, ms, 0), "ms") : cat(num(s, ms / 10, 2), "s");
    *p = 0;
}

/* the page: four dotted waves, one an encoder, each moving with what its knob does (in the waveform's place);
 * the labels and values under them. The waves drift while a note plays and lie still in silence.
 *   blue  (position): the wave slides sideways as the position moves; the live grains ride it as white marks, at
 *                     their place in the recording, as tall as their window is now
 *   green (size):     its waves as long as the grains
 *   orange (density): its dots as close together as the grains overlap; on page 2 its height is the sample gain
 * Above them, the recording as a thin dotted bar, the position in blue (0.4.0). */
#define SIN_N   256
static short sin_t[SIN_N];                     /* sin(2 pi i / 256), x 4096 */
#define WAVE_Y0 47                             /* the first wave's middle: the four centred on y 74, the window's middle (2-146) */
#define WAVE_DY 18                             /* the waves' spacing */
#define AMP     3                              /* a wave's height, units */
#define COL_X0  16                             /* the four columns: 80 units apart, the same margin both sides */
#define COL_DX  80

/* op1r: a dot covers more than a pixel (1.5 x 1.5 units): the port's 0.25-unit-long dots were antialiased to
 * about a third of their colour on the OP-1's panel */
static void dot(int x, int y, int c) { line(x - 0xC0, y, x + 0xC0, y, 0x180, c); }   /* 1.5 x 1.5 units */
static void dot_s(int x, int y, int c) { dot(x, y, c); }   /* the bar's: as the waves' (op1r: the 0.75 ones were dim too) */
#define C_SIL    GR_RGB(0xFFFFFF)            /* the recording's bar: white (op1r; the port's light grey 0xC4C6CC read dim) */
#define BAR_Y    14                            /* the bar's middle, above the waves */
#define BAR_H    5                             /* its half height at the loudest */

/* A wave's y (units x 256): ph the phase there in 1/65536 of a cycle, amp units x 256; linear between the table's
 * steps (0.8.0: the waves move by less than a step a frame, so each frame is a little different, never a jump) */
static int wave_at(int y0, u32 ph, int amp)
{
    const int i = (int)(ph >> 8) & (SIN_N - 1), f = (int)(ph & 255);
    const int sn = sin_t[i] + (((sin_t[(i + 1) & (SIN_N - 1)] - sin_t[i]) * f) >> 8);
    return U(y0) + amp * sn / 4096;
}

/* the grain shape's value (Q15) at t (1/65536 of the grain), a shape change crossfaded over a few frames */
static int shape_prev, shape_now, shape_mix = 256;
static int win_at(u32 t)
{
    const int i = (int)((t & 0xFFFF) >> (16 - WIN_BITS));
    const int a = WIN[shape_now][i];
    if (shape_mix >= 256) return a;
    return WIN[shape_prev][i] + (((a - WIN[shape_prev][i]) * shape_mix) >> 8);
}

/* the page's clock (0.8.2): the engine's block call counts the audio blocks (64 samples at 44.1 kHz: 689 a second)
 * while its sound is the current one, so the page moves by time, not by how often the OS redraws it (it redraws
 * faster while a knob turns: everything sped up, then fell back) */
static volatile u32 ticks, ours_at;
static volatile u32 all_blocks;                /* every audio block (opus_on_block), ours or not */
#define JIT_T 184                              /* the spray's dots take a new place every 184 blocks (0.27 s) */

/* the spray's jitter at a dot: two random sets, one every JIT_T blocks, the dot sliding from one to the next */
static int jitter(int dot_i, u32 group, int sub, int reach)
{
    if (!reach) return 0;
    u32 a = 0x2545F491u + group * 2654435761u + (u32)dot_i * 40503u, b = a + 2654435761u;
    a = a * 1664525u + 1013904223u; a ^= a >> 13; a *= 0x5bd1e995u; a ^= a >> 15;
    b = b * 1664525u + 1013904223u; b ^= b >> 13; b *= 0x5bd1e995u; b ^= b >> 15;
    /* each dot's place as a fraction (-1 to 1, Q15) scaled by the reach: turning white only spreads or draws them in
     * (0.8.4: 0.8.3 took the random number modulo the reach, so every small change of the reach threw every dot to a
     * new place: the dots raced while white turned) */
    const s32 fa = (s32)(a >> 16) - 32768, fb = (s32)(b >> 16) - 32768;
    const s32 f = fa + (fb - fa) * sub / JIT_T;
    return (int)((f * reach) >> 15);
}

/* 2^(st/12) in Q16 for st in 1/256 of a semitone (a smoothed pitch), between the table's semitones */
static u32 ratio_q16(int st8)
{
    int i = (st8 + PITCH_ST * 256) >> 8, f = (st8 + PITCH_ST * 256) & 255;
    if (i < 0) { i = 0; f = 0; }
    if (i >= 2 * PITCH_ST) { i = 2 * PITCH_ST; f = 0; }
    const u32 a = semi_q16[i], b = semi_q16[i + (i < 2 * PITCH_ST)];
    return a + (u32)(((s32)(b - a) * f) >> 8);
}

/* every value the waves show follows its knob smoothly (a quarter of the way a frame) instead of jumping */
static inline void follow(s32 *v, s32 target, int first) { *v = first ? target : *v + ((target - *v) >> 2); }

static void page(void)
{
    short k[8];
    shown_knobs(k);                                               /* the LFOs in, as the stock screens */
    const int sh = op1_shift_held();
    const s32 len = sample_len(op1_current_slot());
    const s32 pos = (s32)(((unsigned long long)(u16)k[0] * (u32)len) >> 15);
    const int syn = ENGINE_granular.sync[op1_current_slot() & 7];
    const s32 size = syn ? sync_size(sync_step(k[1]), tempo_mbpm) : 441 + (s32)(((u32)k[1] * 16000u) >> 15);
    const s32 spray = (s32)(((u32)k[2] * 66150u) >> 15);
    const s32 ovl8 = 4 + (k[3] >> 10);
    const s32 gq = k[7] > 0 ? k[7] : 8192;

    /* a note playing: a voice rendered since the last frame */
    static u32 seen[VOICES];
    int live[VOICES], any = 0;
    u32 vdt = 0;                                                        /* the most blocks a voice rendered since */
    for (int vi = 0; vi < VOICES; vi++) {
        const gvoice_t *v = voice_of[vi];
        live[vi] = v && v->blocks != seen[vi];
        if (v) { const u32 d = v->blocks - seen[vi]; if (d > vdt) vdt = d; seen[vi] = v->blocks; }
        any |= live[vi];
    }

    /* the time since the last frame, in audio blocks: the engine's block calls, else the voices' renders (the same
     * length); none in silence (0.8.3: 0.8.2 counted a frame's worth then, so in silence the page still went by
     * frames and sped up while a knob turned). Only time moves the waves: in silence they stand still and only
     * ease to the knobs. */
    static u32 last_t, tclk;
    const u32 tn = ticks;
    u32 dt = tn - last_t;
    last_t = tn;
    if (dt == 0) dt = vdt;
    if (dt > 200) dt = 200;
    tclk += dt;

    /* the smoothed values (x 16), the pitch in 1/256 semitone, the orange wave's height: a step for each 23 blocks
     * (a 30th of a second) gone by, so they ease at the same speed however often the page is drawn */
    const int nst = (int)((dt + 11) / 23) < 1 ? 1 : (int)((dt + 11) / 23) > 8 ? 8 : (int)((dt + 11) / 23);   /* at least one */
    static s32 s0, s1, s2, s3, st8, amp4;
    static int first = 1;
    for (int n = 0; n < nst; n++) {
        follow(&s0, (s32)(u16)k[0] * 16, first);
        follow(&s1, (s32)k[1] * 16, first);
        follow(&s2, (s32)k[2] * 16, first);
        follow(&s3, (s32)k[3] * 16, first);
        follow(&st8, pitch_st(k[5]) * 256, first);
        follow(&amp4, sh ? (s32)(U(1) + gq * U(5) / 32767) : U(AMP), first);
        first = 0;
    }
    const int shp = shape_of(k[6]);                                     /* a new shape: crossfade to it */
    if (shp != shape_now) { shape_prev = shape_now; shape_now = shp; shape_mix = 0; }
    else if (shape_mix < 256) shape_mix += 32 * nst;
    const u32 ratio = ratio_q16(st8);

    /* the drift: its speed eases in when a note starts and out when it stops, faster with the pitch up; 33 a block
     * is 0.35 of a cycle a second */
    static s32 vel;
    static u32 drift;                                                   /* 1/65536 of a cycle */
    const s32 vt = any ? (s32)((33u * ratio) >> 16) : 0;
    for (int n = 0; n < nst; n++) vel += (vt - vel) >> 3;
    drift += (u32)vel * dt;

    /* above the waves: the recording as a thin dotted bar (a column of dots every 4 units, as tall as the sound
     * there, up to 5 units each side); the position a blue column of its own, sliding */
    if (len >= 256) {
        int cb = C_SIL;
        if (syn) {                              /* clock sync (0.10.0): the bar flashes white at the size's pace */
            u32 n = SYNC_TICKS[sync_step(k[1])];
            while (n < 24) n <<= 1;             /* a sixteenth at the fastest: quicker only flickers */
            const u32 ph = tempo_phase(n) >> 24;                   /* 0-255 over the cycle */
            const int b = ph >= 128 ? 0 : 255 - (int)ph * 2;            /* white at its start, gone by half way */
            const int lo = 0xB0;                                        /* between flashes: a light grey (op1r; the port's 0x50 read too dim) */
            const int g = lo + (((255 - lo) * b) >> 8);
            cb = GR_RGB(g << 16 | g << 8 | g);
        }
        const int yc = BAR_Y, top = S.top > 0 ? S.top : 1;
        const int xp = U(2) + (int)(((long long)s0 * U(316)) >> 19);   /* s0 / 32768 / 16 of the way */
        int xa = (xp + 128) >> 8;                                       /* the arrow's flat side, in pixels */
        if (xa > 319 - (BAR_H + 1)) xa = 319 - (BAR_H + 1);
        for (int c = 0, x = 2; c < BAR_N; c++, x += 4) {
            if (x >= xa - 1 && x <= xa + BAR_H + 2) continue;           /* the arrow's own dots stand in for these */
            const int h = S.peak[c] * BAR_H / top;
            if (h < 1) dot_s(U(x), U(yc), cb);                         /* silence: one dot */
            else line(U(x), U(yc - h), U(x), U(yc + h), 0x200, cb);    /* op1r: a solid column, 2 units wide (the stacked dots read grey) */
        }
        /* the play head (0.9.7): an arrow pointing on, of the bar's own dots on its two-pixel grid (columns of 7, 5, 3
         * and 1 dots: 13 pixels tall, as the marker was), its flat side at the position; the blue encoder's colour,
         * the orange's while frozen */
        {
            const int c = ENGINE_granular.frozen ? GR_COLOR_E(3) : GR_COLOR_E(0);
            for (int j = 0; j <= BAR_H + 1; j++) {                      /* op1r: solid, one column per unit (the dots read dim) */
                const int hh = BAR_H + 1 - j;
                if (hh > 0) line(U(xa + j), U(yc - hh), U(xa + j), U(yc + hh), 0x140, c);
                else dot_s(U(xa + j), U(yc), c);
            }
        }
    }

    /* blue: position, the wave sliding with it (four cycles over the knob's turn); with the pitch up its waves get
     * shorter (an octave halves them) */
    const int yb = WAVE_Y0;
    const u32 pb = (u32)(((unsigned long long)s0 * 4 * 65536) >> 19);
    const u32 bper8 = (56u << 24) / ratio;                              /* units x 256 */
    const u32 bstep = (1u << 24) / bper8;                               /* phase a unit */
    for (int x = 2; x < 320; x += 4) dot(U(x), wave_at(yb, pb + drift + (u32)x * bstep, U(AMP)), C_POS);
    if (len >= 256)                                                     /* the live grains on it, as tall as the shape */
        for (int vi = 0; vi < VOICES; vi++) {
            if (!live[vi]) continue;
            const gvoice_t *v = voice_of[vi];
            for (int i = 0; i < MAX_GRAINS; i++) {
                const grain_t *g = &v->g[i];
                if (!g->winc) continue;
                const int x8 = U(2) + (int)(((long long)g->pos * U(316)) / ((long long)len << FRAC));
                const int h = U(1) + ((win_at(g->wph >> 15) * U(AMP + 3)) >> 15);
                const int y = wave_at(yb, pb + drift + (u32)(((unsigned long long)x8 * bstep) >> 8), U(AMP));
                line(x8, y - h, x8, y + h, 0x180, C_WHITE);
            }
        }

    /* green: grain size, a row of the grain shape, each as long as a grain (24 units at the shortest, 200 at the
     * longest): hann draws round bumps, pluck sharp spikes, swell ramps */
    const u32 gper8 = U(24) + (u32)(((long long)s1 * 176) >> 11);      /* s1 / 32768 / 16 x 176 units x 256 */
    const u32 gstep = (1u << 24) / gper8;
    for (int x = 2; x < 320; x += 4)
        dot(U(x), U(yb + WAVE_DY) + U(AMP) - ((win_at(drift + (u32)x * gstep) * U(2 * AMP)) >> 15), C_SIZE);

    /* the spray's own clock (0.8.5): it runs faster the more the spray, 4x at the top, so the dots dance quicker; a
     * clock of its own (x 256) rather than a shorter step, so a change of speed never moves a dot by itself */
    static u32 jclk;
    jclk += dt * (u32)(256 + ((s2 * 3) >> 11));                         /* s2 / 32768 / 16 x 768 */

    /* white: spray, the dots scattered as far as it reaches, sliding to new places on the spray clock */
    const int reach = (int)(((long long)s2 * U(7)) >> 19);
    for (int x = 2, n = 0; x < 320; x += 4, n++)
        dot(U(x), wave_at(yb + 2 * WAVE_DY, drift * 2 + (u32)x * (65536 / 56), U(AMP)) + jitter(n, jclk / (JIT_T * 256), (int)(jclk % (JIT_T * 256) / 256), reach), C_WHITE);

    /* orange: density, the dots as close as the grains overlap (8 units apart at 0.5x, 2 at 4.3x); with shift held
     * its height is the sample gain */
    const int ovq = 64 + (int)(s3 >> 10);                               /* the overlap x 8, x 16 */
    const int step = U(8) - (ovq - 64) * U(6) / (31 * 16);
    for (int x = U(2); x < U(320); x += step)
        dot(x, wave_at(yb + 3 * WAVE_DY, drift + 32768u + (u32)(((unsigned long long)x * (65536 / 56)) >> 8), (int)amp4), C_GRAIN);

    /* the bottom row: a label and a value a knob. Each fades away when its encoder is left alone: full colour
     * for SHOW_T tenths of a second after a turn, then not drawn. */
    static const char *const LAB1[4] = { "pos", "size", "spray", "dens" };
    static const char *const LAB2[4] = { "pitch", "shape", "spray", "gain" };
    static char val[4][12];
    static u32 used[4];
    static u8 ever[4];
    const u32 now = tenths();
    for (int i = 0; i < 4; i++)
        if (poke[i]) { poke[i] = 0; used[i] = now; ever[i] = 1; }
    *cat(num(val[0], (pos + 220) / 441, 2), "s") = 0;
    if (syn) *cat(val[1], SYNC_NAME[sync_step(k[1])]) = 0;
    else ms_text(val[1], size);
    ms_text(val[2], spray);
    *cat(num(val[3], ovl8 * 10 / 8, 1), "x") = 0;
    if (sh) {                                                       /* pitch: "+7st", "0st", "-12st" */
        const int st = pitch_st(k[5]);
        char *p = val[0];
        if (st > 0) *p++ = '+';
        if (st < 0) *p++ = '-';
        *cat(num(p, st < 0 ? -st : st, 0), "st") = 0;
    }
    if (sh || op1_eng_page[1]) { char *p = cat(val[1], SHAPE_NAME[shape_of(k[6])]); *p = 0; }
    if (sh || op1_eng_page[3]) *cat(num(val[3], gq * 10 / 8192, 1), "x") = 0;
    for (int i = 0; i < 4; i++) {
        const u32 age = ever[i] ? now - used[i] : SHOW_T;
        const int col = TXT4[i];
        const int gone = 0; (void)age;
        const int c = col;
        const char *l = (sh || (i==1 && op1_eng_page[1]) || (i==3 && op1_eng_page[3])) ? LAB2[i] : LAB1[i];
        page_text( i, COL_X0 + COL_DX * i, 112, 12, c, gone ? " " : l);
        page_text( 4 + i, COL_X0 + COL_DX * i, 126, 12, c, gone ? " " : val[i]);
    }
}


static gvoice_t native_voices[6];
static void native_note_on(void *self,const short *params) {
    int i=op1_voice_index(self); if(i<0||i>=6)return;
    scan();
    if(!voice_of[i])v_init(&native_voices[i],i);
    v_note_on(&native_voices[i],params);
    voice_generation[i]=sample_generation;
}
static void native_render(void *self,short *out,const short *params) {
    int i=op1_voice_index(self); if(i<0||i>=6)return;
    /* The stock renderer may enter with circular address generators in use. */
#ifdef __bfin__
    u32 l0,l1,l2,l3;
    __asm__ volatile("%0=L0; %1=L1; %2=L2; %3=L3; L0=0; L1=0; L2=0; L3=0;":"=d"(l0),"=d"(l1),"=d"(l2),"=d"(l3));
#endif
    int fresh=0; u8 *fast=op1_l1_claim(OP1_L1_SYNTH,native_voices,4640,&fresh);
    acc_buf=fast?(s32 *)fast+1:acc_mem+1;
    int shp=shape_of(params[6]);
    if(fast) {
        if(fresh)for(int n=-1;n<BLOCK;n++)acc_buf[n]=0;
        short *fw=(short *)(fast+544);
        if(fresh || wshape!=shp) { for(int j=0;j<2048;j++)fw[j]=WIN[shp][j]; }
        wfast=fw;wshape=shp;
    } else {wfast=0;wshape=-1;}
    v_render(&native_voices[i],out,params,(const unsigned *)OP1_PITCH_INC);
#ifdef __bfin__
    __asm__ volatile("L0=%0; L1=%1; L2=%2; L3=%3;"::"d"(l0),"d"(l1),"d"(l2),"d"(l3));
#endif
}
static void native_service(void) {
    static int tested;
    if(!tested) {
        tested=1;
#ifdef __bfin__
        u32 l0,l1,l2,l3;
        __asm__ volatile("%0=L0; %1=L1; %2=L2; %3=L3; L0=0; L1=0; L2=0; L3=0;":"=d"(l0),"=d"(l1),"=d"(l2),"=d"(l3));
#endif
        use_asm=(u8)asm_selftest(); ENGINE_granular.asm_ok=use_asm;
#ifdef __bfin__
        __asm__ volatile("L0=%0; L1=%1; L2=%2; L3=%3;"::"d"(l0),"d"(l1),"d"(l2),"d"(l3));
#endif
    }
#ifdef __bfin__
    static u32 last; u32 now=cyc();
    if((u32)(now-last)<8000000u && S.slot==(op1_current_slot()&7))return;
    last=now;
#endif
    scan();
}
int gr_key(int code,int pressed,int page);            /* keys.c */
void gr_service(void);
struct gr_bridge ENGINE_granular = {
    .x={.engine={native_note_on,native_render,0,{OP1_PRESS_SHIFT,OP1_PRESS_PAGE_STEPS(6),OP1_PRESS_BROWSE,OP1_PRESS_PAGE}},
        .key=gr_key,.service=gr_service,.shift_page=(1<<1)|(1<<3)},   /* Shift: shape (green), gain (red) */
    .service=native_service
};
static void screen_init(void *scr) {
    (void)scr; for(int i=0;i<SIN_N;i++)sin_t[i]=(short)(op1_sin(i*8192/SIN_N)>>3);
}
static void screen_update(void *scr,int dt) {
    static u32 elapsed;
    elapsed+=(u32)(dt>0?dt:33);gui_tenths=elapsed/100;
    scan();tempo_read();
    ticks=0;for(int i=0;i<6;i++)if(native_voices[i].blocks>ticks)ticks=native_voices[i].blocks;
    op1_screen_draw(scr,dt);page();
    page_text(8,16,28,10,GR_COLOR_TEXT,ENGINE_granular.frozen?"freeze":" ");
    page_text(9,164,28,10,GR_COLOR_TEXT,ENGINE_granular.sync[op1_current_slot()&7]?"sync":" ");
}
const op1_screen_t SCREEN_granular={screen_init,screen_update};
