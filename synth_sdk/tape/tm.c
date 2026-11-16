/* Free tape machine, v1 (docs/82): the four tracks play with their own speed, loop length, offset and direction.
 * R246's tape player keeps running (transport, position, loop markers, UI); while free is selected and the tape plays,
 * tm_block (audio ISR, from fr_snap right after tape_play_mix) overwrites its output L / R 0xFF800D34 / DB4 with ours.
 * Data: our own single-track chunk cache ((TM_SLOTS + 1) x 64 KB from heap 0: a spare to read into; only while free is on), filled by tm_service
 * from op1r's tape_service (main loop): a chunk R246's clip cache holds is copied from there (fresh recordings),
 * else read from tape_c.raw (R246's layout: track t, chunk c at TRACK_BASE[t] + c * 0x10000).
 * Heads: Q32.32 samples. Each track has its own loop (in / out in samples, on / off: s1 / s2 / s3 on the screen,
 * screens/tfscreen.c); without it the head runs free from the transport position. Each block every head moves by the
 * transport speed x the track's speed; a locate or a (re)start resyncs them to the transport.
 * Mix: 4-point Hermite interpolation, R246's per-block track gains (tm_g, from its mix loops), saturating sums.
 * shortcut: no output filter / start-stop ramp of R246's player yet; upgrade if start / stop clicks on the device. */
#include <stdint.h>
#include "slots.h"

#define TM_SLOTS   48                                         /* 3 MB, heap 0; ELSE-MARIE needs 12 x 3 (first, current, next) */
#define TM_CHUNKS  0x1E5                                         /* chunks per track (R246) */
#define CHUNK      0x8000                                        /* samples */

typedef struct { int32_t speed; int32_t lin, lout; uint8_t lon, lset, mute, stop; } tm_track;   /* stop: Shift + STOP / PLAY, per track */   /* lset: in set (marker shown); mute: Shift + T */   /* speed Q16 (negative = reverse); own loop in / out (samples), on */
volatile tm_track tm_tr[4] = { { 0x10000, 0, 0, 0, 0, 0 }, { 0x10000, 0, 0, 0, 0, 0 }, { 0x10000, 0, 0, 0, 0, 0 }, { 0x10000, 0, 0, 0, 0, 0 } };
#ifdef TAPE_FREE
volatile uint8_t tm_on = 1;                                      /* test builds (OP1R_TAPEFREE=1): on */
#else
volatile uint8_t tm_on;                                          /* the free machine selected */
#endif

#ifdef OP1_HOST
extern uint8_t host_tm_xport[0x80]; extern int32_t host_tm_loop[2]; extern uint8_t host_tm_loopon;
extern int16_t host_tm_out[2][64]; extern void *host_tm_malloc(unsigned);
extern int host_tm_read(int t, int chunk, int16_t *dst);         /* fills 64 KB, returns 1 */
#define X          host_tm_xport
#define LOOP_S     host_tm_loop[0]
#define LOOP_E     host_tm_loop[1]
#define LOOP_ON    host_tm_loopon
#define OUT_L      host_tm_out[0]
#define OUT_R      host_tm_out[1]
#define MALLOC     host_tm_malloc
static int16_t host_bus[64];
#define BUS        host_bus
#else
#define X          ((volatile uint8_t *)0xFF802960)              /* R246's transport (docs/29) */
#define LOOP_S     (*(volatile int32_t *)0x01B4EF34)
#define LOOP_E     (*(volatile int32_t *)0x01B4EF38)
#define LOOP_ON    (*(volatile uint8_t *)0xFF8029C5)             /* loop on (docs/26; 0x018C17F4 is not it) */
#define OUT_L      ((volatile int16_t *)0xFF800D34)
#define OUT_R      ((volatile int16_t *)0xFF800DB4)
#define MALLOC     ((void *(*)(unsigned))0x01A9F348)             /* heap 0 */
#define BUS        ((const volatile int16_t *)0xFF800BB4)        /* the instrument bus R246 records */
#define Y_LSEEK    ((int (*)(int, int, int))0x01A9A0E4)
#define Y_READ     ((int (*)(int, void *, unsigned))0x01A99C90)
#define TAPE_FD    (*(volatile int *)0x01B4EF44)
#define TRACK_BASE ((volatile int *)0x018C1810)
#define CLIP_IDX   (*(uint8_t **volatile *)0x01B4EF48)           /* chunk -> R246 clip entry */
#endif
#ifndef OP1_HOST
#define SEL_TRACK  (*(volatile int32_t *)0xFF802964)
#define QUEUE_CLIP_EARLY ((void (*)(int))0x01A54CD4)       /* R246's queue_clip (slot machine record start) */
#endif
#define MODE       (*(volatile int32_t *)(X + 0x00))             /* 0 stop, 1 rec, 2 play, 3 wind */
#define TSPEED     (*(volatile int32_t *)(X + 0x08))             /* 1x = 0x08000000 (emulator: 64 samples per block); winding 0x40000000 = 8x */
#define TPOS       (*(volatile int32_t *)(X + 0x14))             /* samples << 6 */
/* R246's per-block track gains (a stack array in tape_play_mix: L[4], R[4]), copied by op1r's mix-loop hooks (hook.S
 * mix_hook_fwd / rev); X + 0x38 / 0x40 are only inputs to that computation (docs/82) */
volatile int16_t tm_g[8] = { 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767 };
#define GAIN_L(t)  (tm_g[t])
#define GAIN_R(t)  (tm_g[4 + (t)])

typedef struct { int16_t *data; volatile int16_t t, chunk; volatile uint8_t state; volatile uint8_t gen; volatile uint8_t refresh; uint32_t used; } tm_slot;
static tm_slot slot[TM_SLOTS];                                   /* state 0 free, 1 wanted, 2 loaded; refresh: loaded, reread
                                                                  * wanted (keeps playing the old data meanwhile: no gap) */
static int16_t *spare;                                           /* tm_service reads + masks here, then swaps it with the slot's
                                                                  * data: the ISR never plays a buffer being read (raw file audio) */
static volatile int8_t tm_rd = -1;                               /* the slot being read into the spare (recording mirrors into it) */
#ifdef OP1_HOST
#ifndef TM_IRQ_MODEL                                             /* (tmswap_test.c models the audio interrupt) */
#define TM_IRQ(k)                                                /* host test: an audio interrupt may arrive here */
static uint32_t tm_irq_off(void) { return 0; }
static void tm_irq_on(uint32_t m) { (void)m; }
#endif
#else
#define TM_IRQ(k)
static uint32_t tm_irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m) : : "memory"); return m; }
static void tm_irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m) : "memory"); }
#endif
static void tm_refresh_all(void)                                 /* reread every held chunk (recording, clip edits) */
{
    for (int i = 0; i < TM_SLOTS; i++) if (slot[i].t >= 0) { slot[i].refresh = 1; slot[i].gen++; }
}
static int8_t map[4][TM_CHUNKS];                                 /* chunk -> slot + 1 (0 none) */
static int64_t head[4];
static uint32_t blk;
static uint8_t rec_seen;
volatile int8_t tm_scrub;                                        /* -1 / +1: < / > held on our screen (tfscreen.c) */
volatile uint8_t tm_scrub_track;                                 /* the active track: only it scrubs (and sounds) */
volatile uint8_t tm_hold_heads;                                  /* heads restored at boot (tfscreen.c): no resync yet */
volatile uint8_t tm_live;                                        /* free on with its cache: R246's gather skipped (tm_l1.S) */
volatile uint8_t tm_sel;                                         /* the active track (tfscreen.c): what FREE records */
static int32_t scrub_v;                                          /* current scrub speed, Q16 (ramps to +-8x) */
int tm_scrubbing(void) { return scrub_v != 0; }
volatile uint32_t tm_miss, tm_loads;

/* buffers written per sample in the audio path: in heap 0 once tm_alloc ran (our .bss page is write-back without
 * DIRTY in R246's DCPLBs, docs/19: stores there may trap; device: 12 slots cost fr 38 %) */
static int16_t tm_v_bss[4][64];
static int16_t (*tm_v)[64] = tm_v_bss;                           /* this block's samples per track, pre-fader */
typedef struct { int32_t cm[4][64], chl[4][64], chr[4][64]; } sl_hot_t;   /* the slot machine's channel buses */
static sl_hot_t sl_hot_bss, *sl_hot = &sl_hot_bss;
typedef struct { const int16_t *p[4]; uint32_t g01, g23; } sl_m4;   /* tape/sl_mix.S: four slots, gains Q15 (<= 32767) */
static const int16_t sl_zero[64];
static void sl_mac4_c(int32_t *acc, const sl_m4 *m)              /* the same in C (host; tape/sl_mix.S on the device) */
{
    const int32_t g0 = (int16_t)m->g01, g1 = (int16_t)(m->g01 >> 16), g2 = (int16_t)m->g23, g3 = (int16_t)(m->g23 >> 16);
    for (int i = 0; i < 64; i++) {
        const int64_t a = (int64_t)m->p[0][i] * g0 + (int64_t)m->p[1][i] * g1 + (int64_t)m->p[2][i] * g2 + (int64_t)m->p[3][i] * g3;
        acc[i] += (int32_t)(a >> 15);
    }
}
typedef struct { const int16_t *p[8]; int16_t g[8]; int32_t add; } sl_m8;
typedef struct { const int16_t *src; uint32_t fr, step; int32_t dir, gl, gr, bin, ga; int8_t s, c, str; uint32_t ph, tr; int32_t dph; } sl_dr;   /* a direct block (str: stretch) */
static int32_t sl_mpk[4];
volatile uint8_t tm_sl_peak[12][(120 * 44100 >> 11) + 1];
static void sl_direct(int32_t *accL, int32_t *accR, const sl_dr *d)   /* linear interpolation, 64 samples, straight from the
                                                                  * chunk (the caller checked the span), added into L / R */
{
    const int16_t *p = d->src; uint32_t fr = d->fr; const uint32_t st = d->step; const int dir = d->dir;
    const int32_t gl = d->gl, gr = d->gr; int32_t n = 0, pk = 0;
    if (d->str) {                                                /* stretch: two grains around the 1x position, as the general
                                                                  * path, without its wraps / seams / lookups (the span is checked) */
        const uint32_t M = ((uint32_t)SL_G << 16) - 1, tr = d->tr; uint32_t ph = d->ph; const int32_t dph = d->dph;
        for (int i = 0; i < 64; i++) {
            int32_t acc = 0;
            for (int k = 0; k < 2; k++) {
                const uint32_t pp = (ph + (k ? (uint32_t)SL_G << 15 : 0)) & M;
                const int32_t off = (int32_t)(pp >> 16), f = (int32_t)(pp & 0xFFFF) >> 1, a = n + dir * off;
                const int32_t x0 = p[a], x = x0 + ((p[a + dir] - x0) * f >> 15);
                acc += x * ((off < SL_G / 2 ? off : SL_G - off) << 5) >> 15;
            }
            const int32_t aa = acc < 0 ? -acc : acc; if (aa > pk) pk = aa;
            accL[i] += acc * gl >> 15; accR[i] += acc * gr >> 15;
            ph = (ph + (uint32_t)dph) & M;
            fr += tr; n += dir * (int32_t)(fr >> 16); fr &= 0xFFFF;
        }
    } else if (gl == gr) {
        for (int i = 0; i < 64; i++) {
            const int32_t x0 = p[n], x = x0 + ((p[n + dir] - x0) * (int32_t)(fr >> 1) >> 15), y = x * gl >> 15;
            const int32_t a = x0 < 0 ? -x0 : x0; if (a > pk) pk = a;
            accL[i] += y; accR[i] += y;
            fr += st; n += dir * (int32_t)(fr >> 16); fr &= 0xFFFF;
        }
    } else {
        for (int i = 0; i < 64; i++) {
            const int32_t x0 = p[n], x = x0 + ((p[n + dir] - x0) * (int32_t)(fr >> 1) >> 15);
            const int32_t a = x0 < 0 ? -x0 : x0; if (a > pk) pk = a;
            accL[i] += x * gl >> 15; accR[i] += x * gr >> 15;
            fr += st; n += dir * (int32_t)(fr >> 16); fr &= 0xFFFF;
        }
    }
    { const uint8_t a = (uint8_t)(pk >> 7); volatile uint8_t *pp = &tm_sl_peak[d->s][d->bin]; if (a > *pp) *pp = a; }
    { const int32_t mp = pk * d->ga >> 15; if (mp > sl_mpk[d->c]) sl_mpk[d->c] = mp; }
}   /* tape/sl_mix.S sl_macc8: centred slots */
static void sl_macc8_c(int32_t *accL, int32_t *accR, const sl_m8 *m)
{
    for (int i = 0; i < 64; i++) {
        int64_t a = 0; for (int k = 0; k < 8; k++) a += (int64_t)m->p[k][i] * m->g[k];
        const int32_t v = (int32_t)(a >> 15);
        if (m->add) { accL[i] += v; accR[i] += v; } else accL[i] = accR[i] = v;
    }
}
#ifndef OP1_HOST
extern void sl_mac4(int32_t *acc, const sl_m4 *m);
extern void sl_macc8(int32_t *accL, int32_t *accR, const sl_m8 *m);
#else
#define sl_mac4 sl_mac4_c
#define sl_macc8 sl_macc8_c
#endif
#ifdef OP1_HOST
int host_sl_fmx;
#endif
volatile int32_t tm_mac_check;                                   /* sl_mix.S self-check: mismatches + 1 (0: not run) */
static int tm_alloc(void)
{
    if (tm_v == tm_v_bss) { int16_t (*v)[64] = (int16_t (*)[64])MALLOC(sizeof tm_v_bss); if (v) { for (int t = 0; t < 4; t++) for (int i = 0; i < 64; i++) v[t][i] = 0; tm_v = v; } }
    if (sl_hot == &sl_hot_bss) { sl_hot_t *h = (sl_hot_t *)MALLOC(sizeof *h); if (h) sl_hot = h; }
#ifndef OP1_HOST
    if (!tm_mac_check) {                                         /* tape/sl_mix.S against its C version, once: 1 = equal */
        static int16_t a[4][64]; static int32_t x[64], y[64];
        for (int j = 0; j < 4; j++) for (int i = 0; i < 64; i++) a[j][i] = (int16_t)((i * 977 + j * 3331) * 40503u >> 16) | (i == 7 ? (int16_t)0x8000 : 0);
        for (int i = 0; i < 64; i++) x[i] = y[i] = i * 1000 - 30000;
        const sl_m4 m = { { a[0], a[1], a[2], a[3] }, 32767u | 1234u << 16, (uint32_t)(uint16_t)-20000 | 32767u << 16 };
        sl_mac4(x, &m); sl_mac4_c(y, &m);
        int bad = 0; for (int i = 0; i < 64; i++) bad += x[i] != y[i];
        {                                                        /* sl_macc8: both passes */
            static int32_t l1[64], r1[64], l2[64], r2[64];
            sl_m8 m8 = { { a[0], a[1], a[2], a[3], a[3], a[2], a[1], a[0] }, { 32767, 1234, -20000, 32767, 5, -32768, 100, 7 }, 0 };
            sl_macc8(l1, r1, &m8); sl_macc8_c(l2, r2, &m8); m8.add = 1; sl_macc8(l1, r1, &m8); sl_macc8_c(l2, r2, &m8);
            for (int i = 0; i < 64; i++) bad += (l1[i] != l2[i]) + (r1[i] != r2[i]);
        }
        tm_mac_check = bad + 1;
    }
#endif
    if (slot[0].data) return 1;
    int16_t *m = (int16_t *)MALLOC((TM_SLOTS + 1) * CHUNK * 2);
    if (!m) return 0;
    spare = m + TM_SLOTS * CHUNK;
    for (int i = 0; i < TM_SLOTS; i++) { TM_IRQ(2); slot[i].t = -1; slot[i].state = 0; if (i) slot[i].data = m + i * CHUNK; }
    __asm__ volatile ("" : : : "memory");                        /* slot[0].data is the ISR's "ready": set last (an audio block
                                                                  * in the loop would claim a slot still t = 0 from bss) */
    slot[0].data = m;
    return 1;
}

/* the slot of (t, chunk), requesting it if missing; the data only if loaded */
static const int16_t *tm_chunk(int t, int c, int want)
{
    if (c < 0 || c >= TM_CHUNKS) return 0;
    int s = map[t][c] - 1;
    if (s >= 0) { slot[s].used = blk; return slot[s].state == 2 ? slot[s].data : 0; }
    if (!want) return 0;
    int v = -1; uint32_t age = 0;                               /* LRU slot not used this block */
    for (int i = 0; i < TM_SLOTS; i++) {
        if (slot[i].t < 0) { v = i; break; }
        if (blk - slot[i].used > age && slot[i].used != blk) { age = blk - slot[i].used; v = i; }
    }
    if (v < 0) return 0;
    if (slot[v].t >= 0) map[slot[v].t][slot[v].chunk] = 0;
    slot[v].state = 1; slot[v].refresh = 0; slot[v].gen++;
    slot[v].t = (int16_t)t; slot[v].chunk = (int16_t)c; slot[v].used = blk;
    map[t][c] = (int8_t)(v + 1);
    return 0;
}

volatile uint32_t tm_miss_t[4];                                  /* misses per track (OP1R_TFDIAG line) */
static inline int16_t tm_sample(int t, int32_t i, const int16_t **cache, int32_t *cache_c)
{
    const int32_t c = i >> 15;
    if (c != *cache_c) { *cache_c = c; *cache = tm_chunk(t, c, 1); }
#ifndef OP1_HOST
    if (!*cache && tm_on == 2 && CLIP_IDX) {                     /* slot machine: a chunk still in R246's clip cache (a fresh
                                                                  * recording) is read there until ours is loaded (tm_service
                                                                  * waits behind the saves) */
        const uint8_t *e = CLIP_IDX[c];
        if (e && e[0] && *(const int16_t *)(e + 6) == c) *cache = (const int16_t *)(*(uint8_t *const *)(e + 0x10) + t * 0x10000);
    }
#endif
    if (!*cache) { tm_miss++; tm_miss_t[t & 3]++; return 0; }
    return (*cache)[i & (CHUNK - 1)];
}

static void tm_resync(int32_t pos)
{
    for (int t = 0; t < 4; t++) {
        int32_t h = pos;
        const volatile tm_track *tr = &tm_tr[t];
        if (tr->lon && tr->lout > tr->lin && (pos < tr->lin || pos >= tr->lout)) {
            const int32_t len = tr->lout - tr->lin;
            int32_t r = (pos - tr->lin) % len; if (r < 0) r += len;
            h = tr->lin + r;
        }
        head[t] = (int64_t)h << 32;
    }
}

#ifndef TM_XHI
#define TM_XHI   (5 * 60 * 44100)                                  /* transport kept below 5:00 (test builds: lower) */
#define TM_XSPAN (3 * 60 * 44100)
#endif
static int32_t xlast = -1;                                       /* transport position last block (jump detection) */
volatile int16_t tm_in_peak;                                     /* the recorded signal (the instrument bus) for the meter */
void tm_set_head(int t, int32_t pos)                             /* GUI: bar jumps (the tape end at most: no chunk after it) */
{
    head[t & 3] = (int64_t)(pos < 0 ? 0 : pos > TM_CHUNKS * CHUNK ? TM_CHUNKS * CHUNK : pos) << 32;
}

static int64_t rec_h0[4], rec_step[4], rec_lo[4], rec_hi[4];    /* this block's head path per track (for recording) */
/* one interpolated sample at h (Q32): 4-point Hermite (Catmull-Rom), coefficients ~2^18, x Q12 stays in 32 bits */
static inline int32_t tm_herm(int t, int64_t h, const int16_t **cache, int32_t *cache_c)
{
    const int32_t n = (int32_t)(h >> 32), f = (int32_t)((uint64_t)h >> 20 & 0xFFF);
    const int32_t sm = tm_sample(t, n - 1, cache, cache_c), s0 = tm_sample(t, n, cache, cache_c),
                  s1 = tm_sample(t, n + 1, cache, cache_c), s2 = tm_sample(t, n + 2, cache, cache_c);
    const int32_t c1 = (s1 - sm) >> 1, c2 = sm - (5 * s0 >> 1) + 2 * s1 - (s2 >> 1), c3 = ((s2 - sm) >> 1) + (3 * (s0 - s1) >> 1);
    int32_t s = (((((c3 * f >> 12) + c2) * f >> 12) + c1) * f >> 12) + s0;
    return s > 32767 ? 32767 : s < -32768 ? -32768 : s;
}
#ifdef TM_DBGRING
int16_t tm_dbg[32768]; uint32_t tm_dbg_n;
#endif
static int32_t fade_prev;                                        /* output gain of the last block (tm_block) */
#define TM_SEAM 512                                             /* seam jump threshold (sum of both sides, ~-36 dBFS) */
#define TM_XF 256                                                /* loop crossfade, samples (power of 2) */
#ifndef OP1_HOST
void tm_hloop(const int16_t *d, int32_t r, int32_t st, uint32_t gains, int32_t shift, int32_t *accL, int32_t *accR, int16_t *out)
    __attribute__((longcall));                                   /* in L1 (.l1tape): far from SDRAM */
void tm_hloop_near(const int16_t *d, int32_t r, int32_t st, uint32_t gains, int32_t shift, int32_t *accL, int32_t *accR,
                   int16_t *out) __asm__("_tm_hloop");           /* the same, from tm_fast (both in .l1tape) */
#endif
volatile uint32_t tm_chk_n, tm_chk_bad;                          /* OP1R_TMCHECK: blocks compared, mismatches */
typedef struct { int32_t lo, hi, lin, lout; int64_t rlo, rhi; int16_t c; int8_t s; uint8_t lon, ok; } tm_win;
static uint32_t tm_fast_n, tm_fast_hit;
#ifdef OP1_HOST
static int tm_nofast;
#endif
                                      /* host test: tm_fast calls */
static tm_win win[4];                                            /* per track: where tm_fast may run (set by the slow path) */
#ifdef OP1_HOST
static void tm_hloop_host(const int16_t *d, int32_t r, int32_t st, uint32_t gains, int32_t shift, int32_t *accL, int32_t *accR, int16_t *out)
{
    const int32_t gl = (int16_t)gains, gr = (int16_t)(gains >> 16);
    for (int i = 0; i < 64; i++) {
        const int32_t n = r >> 16, f = (r >> 4) & 0xFFF;
        const int32_t sm = d[n - 1], s0 = d[n], s1 = d[n + 1], s2 = d[n + 2];
        const int32_t c1 = (s1 - sm) >> 1, c2 = sm - (5 * s0 >> 1) + 2 * s1 - (s2 >> 1), c3 = ((s2 - sm) >> 1) + (3 * (s0 - s1) >> 1);
        int32_t v = (((((c3 * f >> 12) + c2) * f >> 12) + c1) * f >> 12) + s0;
        v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        accL[i] += v * gl >> shift; accR[i] += v * gr >> shift; out[i] = (int16_t)v;
        r += st;
    }
}
#define TM_HLOOP_NEAR tm_hloop_host
#define TM_FAST_SEC
#else
#define TM_HLOOP_NEAR tm_hloop_near
#define TM_FAST_SEC __attribute__((section(".l1tape")))
#endif
#ifndef OP1_HOST
int tm_fast(int t, int64_t step, int32_t *accL, int32_t *accR) __attribute__((longcall));   /* L1, called from SDRAM */
#endif
/* the per-block fast path (A + B, docs/82): a few 32-bit compares against the window, then the inner loop. In L1. */
TM_FAST_SEC int tm_fast(int t, int64_t step, int32_t *accL, int32_t *accR)
{
#ifdef OP1_HOST
    if (tm_nofast) return 0;                                     /* host test (tmfast): the old path only */
#endif
    tm_fast_n++;
    const tm_win *w = &win[t];
    if (!w->ok || ((blk + (uint32_t)t * 4) & 15) == 0) return 0;   /* housekeeping blocks: the slow path */
    const volatile tm_track *tr = &tm_tr[t];
    if (tr->lin != w->lin || tr->lout != w->lout || tr->lon != w->lon) return 0;
    const int64_t h = head[t], he = h + step * 64;
    const int32_t na = (int32_t)(h >> 32), nb = (int32_t)(he >> 32);
    if ((na < nb ? na : nb) < w->lo || (na < nb ? nb : na) >= w->hi) return 0;
    tm_slot *sl = &slot[w->s];
    if (sl->t != t || sl->chunk != w->c || sl->state != 2) return 0;
    sl->used = blk;
    if (MODE == 1) { rec_h0[t] = h; rec_step[t] = step; rec_lo[t] = w->rlo; rec_hi[t] = w->rhi; }
    const int16_t gl = tr->mute ? 0 : GAIN_L(t), gr = tr->mute ? 0 : GAIN_R(t);
    TM_HLOOP_NEAR(sl->data, (int32_t)((h - ((int64_t)(w->c * CHUNK) << 32)) >> 16), (int32_t)(step >> 16),
                  (uint16_t)gl | (uint32_t)(uint16_t)gr << 16, 15, accL, accR, tm_v[t]);
    head[t] = he;
    tm_fast_hit++;
    return 1;
}
static void tm_track_block(int t, int64_t step, int loops, int gain_shift, int32_t *accL, int32_t *accR)
{
    int64_t h = head[t];
    int64_t lo = 0, hi = (int64_t)(TM_CHUNKS * CHUNK) << 32;
    if (loops && tm_tr[t].lon && tm_tr[t].lout > tm_tr[t].lin) {
        lo = (int64_t)tm_tr[t].lin << 32; hi = (int64_t)tm_tr[t].lout << 32;
        if (h < lo || h >= hi) { int64_t r = (h - lo) % (hi - lo); if (r < 0) r += hi - lo; h = lo + r; }   /* loop just set: fold in */
    }
    rec_h0[t] = h; rec_step[t] = step; rec_lo[t] = lo; rec_hi[t] = hi;
    const int16_t gl = tm_tr[t].mute ? 0 : GAIN_L(t), gr = tm_tr[t].mute ? 0 : GAIN_R(t);   /* muted: silent, the head runs */
    const int16_t *cache = 0, *cache2 = 0; int32_t cache_c = -1, cache2_c = -1;
    int looped = hi - lo < ((int64_t)(TM_CHUNKS * CHUNK) << 32) && hi - lo > ((int64_t)(2 * TM_XF) << 32);
    /* housekeeping (seam test, prefetch) every 16 blocks per track (staggered), or at once when the loop changed:
     * it was most of the per-track cost (~3600 cycles a call, device) */
    static int32_t hk_lo[4], hk_hi[4]; static int8_t seam[4];
    const int32_t lo32 = (int32_t)(lo >> 32), hi32 = (int32_t)(hi >> 32);
    const int hk = ((blk + (uint32_t)t * 4) & 15) == 0 || lo32 != hk_lo[t] || hi32 != hk_hi[t];
    hk_lo[t] = lo32; hk_hi[t] = hi32;
    if (looped && !hk) looped = seam[t];
    else if (looped) {                                           /* as stock: only a seam that jumps is smoothed; test = each
                                                                  * side's linear extrapolation against the other's sample */
        const int32_t a = (int32_t)(lo >> 32), b = (int32_t)(hi >> 32) - 1;
        const int32_t sa = tm_sample(t, a, &cache2, &cache2_c), sa1 = tm_sample(t, a + 1, &cache2, &cache2_c),
                      sb = tm_sample(t, b, &cache2, &cache2_c), sb1 = tm_sample(t, b - 1, &cache2, &cache2_c);
        const int32_t j1 = sa - (2 * sb - sb1), j2 = sb - (2 * sa - sa1);
        looped = seam[t] = (int8_t)((j1 < 0 ? -j1 : j1) + (j2 < 0 ? -j2 : j2) > TM_SEAM);
    }
    {                                                            /* fast path (almost every block): the 4 Hermite points of all
                                                                  * 64 samples in one loaded chunk, no wrap, no crossfade zone */
        const int64_t h_end = h + 64 * step;
        const int64_t mn = step > 0 ? h : h_end, mx = step > 0 ? h_end : h;
        const int64_t xf = looped ? (int64_t)TM_XF << 32 : 0;
        const int32_t n0 = (int32_t)(mn >> 32) - 1, n1 = (int32_t)(mx >> 32) + 2, c = n0 >> 15;
        if (n0 >= 0 && c == n1 >> 15 && mn >= lo + xf && mx < hi - xf && c < TM_CHUNKS) {
            const int16_t *d = tm_chunk(t, c, 1);
            if (d) {
                const int32_t base = c * CHUNK;
                int32_t r = (int32_t)((h - ((int64_t)base << 32)) >> 16);   /* Q16 inside the chunk (< 2^31) */
                const int32_t st = (int32_t)(step >> 16);
#if !defined(OP1_HOST) && !defined(TM_CHECK)
                tm_hloop(d, r, st, (uint16_t)gl | (uint32_t)(uint16_t)gr << 16, gain_shift, accL, accR, tm_v[t]);   /* tm_loop.S */
#else
#ifdef TM_CHECK
                int32_t cl[64], cr[64]; int16_t cv[64];
                for (int i = 0; i < 64; i++) { cl[i] = accL[i]; cr[i] = accR[i]; }
                tm_hloop(d, r, st, (uint16_t)gl | (uint32_t)(uint16_t)gr << 16, gain_shift, cl, cr, cv);
#endif
                for (int i = 0; i < 64; i++) {
                    const int32_t n = r >> 16, f = (r >> 4) & 0xFFF;
                    const int32_t sm = d[n - 1], s0 = d[n], s1 = d[n + 1], s2 = d[n + 2];
                    const int32_t c1 = (s1 - sm) >> 1, c2 = sm - (5 * s0 >> 1) + 2 * s1 - (s2 >> 1), c3 = ((s2 - sm) >> 1) + (3 * (s0 - s1) >> 1);
                    int32_t v = (((((c3 * f >> 12) + c2) * f >> 12) + c1) * f >> 12) + s0;
                    v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
                    accL[i] += v * gl >> gain_shift; accR[i] += v * gr >> gain_shift;
                    tm_v[t][i] = (int16_t)v;
                    r += st;
                }
#ifdef TM_CHECK
                for (int i = 0; i < 64; i++) if (cl[i] != accL[i] || cr[i] != accR[i] || cv[i] != tm_v[t][i]) { tm_chk_bad++; break; }
                tm_chk_n++;
#endif
#endif
                h = h_end;
                goto done;
            }
        }
    }
    for (int i = 0; i < 64; i++) {
        if (h >= hi) h -= hi - lo; else if (h < lo) h += hi - lo;
        int32_t s = tm_herm(t, h, &cache, &cache_c);
        if (looped) {                                            /* near the wrap: crossfade into what follows it */
            const int64_t xf = (int64_t)TM_XF << 32;
            int64_t d = -1, other = 0;
            if (step > 0 && h > hi - xf) { d = h - (hi - xf); other = h - (hi - lo); }        /* forward: before loop in */
            else if (step < 0 && h < lo + xf) { d = lo + xf - h; other = h + (hi - lo); }     /* reverse: after loop out */
            if (d >= 0) {
                const int32_t a = (int32_t)(d >> 25) & 0x7FFF;   /* d / TM_XF in Q15 (TM_XF = 2^8: >> 32 - 15 + 8) */
                const int32_t o = tm_herm(t, other < 0 ? 0 : other, &cache2, &cache2_c);
                s += (o - s) * a >> 15;
            }
        }
        accL[i] += s * gl >> gain_shift; accR[i] += s * gr >> gain_shift;
        tm_v[t][i] = (int16_t)s;
        h += step;
        if (h >= hi) h -= hi - lo; else if (h < lo) h += hi - lo;   /* the stored head stays inside its loop */
    }
done:
    if (h < 0) h = 0;
    head[t] = h;
    {                                                            /* the fast window for the next blocks (tm_fast) */
        tm_win *w = &win[t];
        const int32_t n = (int32_t)(h >> 32), c = n >> 15;
        const int sl = loops && c >= 0 && c < TM_CHUNKS ? map[t][c] - 1 : -1;
        w->ok = 0;
        if (sl >= 0 && slot[sl].state == 2) {
            const int32_t xf32 = looped ? TM_XF : 0, lo32w = (int32_t)(lo >> 32) + xf32, hi32w = (int32_t)(hi >> 32) - xf32;
            w->lo = c * CHUNK + 1 > lo32w ? c * CHUNK + 1 : lo32w;
            w->hi = (c + 1) * CHUNK - 3 < hi32w ? (c + 1) * CHUNK - 3 : hi32w;
            w->s = (int8_t)sl; w->c = (int16_t)c;            /* (no data pointer: tm_service swaps it) */
            w->lin = tm_tr[t].lin; w->lout = tm_tr[t].lout; w->lon = tm_tr[t].lon;
            w->rlo = lo; w->rhi = hi;
            w->ok = w->hi > w->lo;
        }
    }
    if (!hk) return;
    const int32_t c = (int32_t)(h >> 32) >> 15, d = step < 0 ? -1 : 1;   /* prefetch ahead, and the loop start */
    /* ahead along the loop (wrapping), not past loop out: the chunks after loop in must be there at the wrap (device
     * loads take 10..15 ms; a miss plays 0 = the loop-end pop). No 64-bit modulo here (a software division per call) */
    const int32_t L0 = (int32_t)(lo >> 32), L1 = (int32_t)(hi >> 32), len = L1 - L0, hn = (int32_t)(h >> 32);
    if (len < TM_CHUNKS * CHUNK && len <= 3 * CHUNK) {           /* short loop: all of it */
        for (int k = L0 >> 15; k <= (L1 - 1) >> 15; k++) tm_chunk(t, k, 1);
    } else for (int k = 1; k <= 3; k++) {
        int32_t p = hn + k * d * CHUNK;
        if (len < TM_CHUNKS * CHUNK) { if (p >= L1) p -= len; else if (p < L0) p += len; }   /* k CHUNK <= len: one wrap */
        tm_chunk(t, p >> 15, 1);
    }
    (void)c;
    if (hi - lo < ((int64_t)(TM_CHUNKS * CHUNK) << 32)) tm_chunk(t, (int32_t)((d > 0 ? lo : hi - 1) >> 32) >> 15, 1);
    if (looped) tm_chunk(t, (int32_t)((d > 0 ? lo - ((int64_t)TM_XF << 32) : hi + ((int64_t)TM_XF << 32)) >> 32) >> 15, 1);   /* the crossfade's other side */
}

static int32_t sl_mpk[4]; static uint8_t sl_mpk_on;
static void sl_mix_list(int32_t *acc, const int16_t *const *src, const int32_t *g, int n)   /* groups of four */
{
    for (int k = 0; k < n; k += 4) {
        sl_m4 m; int32_t gg[4];
        for (int j = 0; j < 4; j++) { m.p[j] = k + j < n ? src[k + j] : sl_zero; gg[j] = k + j < n ? g[k + j] : 0; }
        m.g01 = (uint32_t)(uint16_t)gg[0] | (uint32_t)gg[1] << 16; m.g23 = (uint32_t)(uint16_t)gg[2] | (uint32_t)gg[3] << 16;
        sl_mac4(acc, &m);
    }
}              /* ELSE-MARIE's fast mix: the channel meters (tm_v not written) */
/* the block's output: L / R (the fade over the block), R246's mixer meters and op1r's per-track outs (tm_v) */
static void tm_out(const int32_t *accL, const int32_t *accR, int32_t f0, int32_t fade)
{
    for (int i = 0; i < 64; i++) {
        const int32_t g = f0 + ((fade - f0) * (i + 1) >> 6);
        const int32_t l = accL[i] * g >> 15, r = accR[i] * g >> 15;
        OUT_L[i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
#ifdef TM_DBGRING
        { extern int16_t tm_dbg[32768]; extern uint32_t tm_dbg_n; tm_dbg[tm_dbg_n++ & 32767] = OUT_L[i]; }
#endif
        OUT_R[i] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
#ifndef OP1_HOST
    /* FREE's tracks downstream (docs/82): R246's mixer meters (cleared per block by Core_render_block, then the tape
     * mix's block peaks, Core_mix_master adds the recording) and op1r's per-track outs (USB tracks, FX2 aux), pre-fader */
    {
        extern int16_t tr_out[4][64]; extern volatile uint8_t usb_tracks_live; extern int fr_tracks_needed(void);
        const int outs = usb_tracks_live || fr_tracks_needed();
        for (int t = 0; t < 4; t++) {
            const int16_t *src = tm_v[t]; int32_t pk = 0;
            if (sl_mpk_on) pk = sl_mpk[t];
            else for (int i = 0; i < 64; i++) { const int32_t a = src[i] < 0 ? -src[i] : src[i]; if (a > pk) pk = a; }
            const int muted = tm_tr[t].mute;
            ((volatile int16_t *)0xFF802990)[t] = (int16_t)(muted ? 0 : pk > 32767 ? 32767 : pk);
            if (outs) {
                int16_t *o = tr_out[t];
                if (muted) for (int i = 0; i < 64; i++) o[i] = 0;
                else for (int i = 0; i < 64; i++) o[i] = (int16_t)(src[i] * (f0 + ((fade - f0) * (i + 1) >> 6)) >> 15);
            }
        }
    }
#endif
}

/* ---- slot machine (docs/85, tm_on == 2): 12 slots of up to 2:00, 3 per tape track (slot s: track s / 3, from
 * (s % 3) x 2:00), each looping its recorded length at 1x into one of the 4 channels (the mixer's tracks: tm_v, tm_g)
 * whatever the transport does. Recording (R246's record state, started by screens/tfscreen.c): the slot's own head
 * from its start, replacing the audio there, up to 2:00. */
#include "slots.h"
volatile tm_slm tm_sl[SL_N];
volatile int8_t tm_sl_rec = -1;                                  /* the slot recording (-1 none) */
volatile int32_t tm_sl_recpos, tm_sl_rectgt = SL_MAX, tm_sl_bar = 88200, tm_sl_bpm;
volatile uint8_t tm_sl_recwait, tm_sl_od;
static int32_t sl_od_h0, sl_od_step;                             /* overdub: the slot's head at the block start, its rate (Q16, signed) */
static uint32_t sl_clock, sl_anchor;                             /* frames; the bar grid starts at sl_anchor */
volatile tm_sllfo tm_sl_lfo[SL_N][4];
volatile uint32_t tm_sl_tr[SL_N];
volatile uint8_t tm_sl_lfoon[SL_N];                              /* LFOs with an amount, a bit each (the GUI): none = skipped */
static int32_t sl_envstep[101];                                  /* envelope gain step per block for 0..100 (built once) */
volatile uint8_t tm_sl_peak[SL_N][SL_PKN];
#if defined(SL_PROF) || defined(OP1_HOST)                       /* OP1R_SLPROF=1 (docs/86): the slot engine's profile counters
                                                                  * (perf page, op1dev perf); the host tests use the path counts */
#define SLP(...) __VA_ARGS__
volatile uint8_t tm_sl_paths[3];                                 /* last block: slots on the fast / general / stretch path (sysmon) */
static uint8_t sl_np[3];
uint32_t sl_fast_blocks, sl_direct_blocks;                       /* (harness/slfast_test.c) */
volatile uint32_t tm_sl_pcyc[3];                                 /* cycles per block on each path (sysmon, averaged) */
static uint32_t sl_pc[3], sl_pcn;
static inline uint32_t sl_cyc(void) {
#ifndef OP1_HOST
    uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c)); return c;
#else
    return 0;
#endif
}
#else
#define SLP(...)
#endif
static volatile uint8_t *sl_rpk; static int32_t sl_rpk_base;     /* the recording slot's peaks (tm_record) */
volatile uint32_t tm_sl_rinc[64], tm_sl_dinc[SL_DIVS];
static int32_t sl_lfo_shape(int w, uint32_t ph, uint32_t cnt, int s)   /* Q15 (the quad LFO's shapes; sine: a parabola) */
{
    switch (w) {
    case 1: { const int32_t q = (int32_t)((ph + 0x40000000u) >> 16); return (q < 32768 ? q : 65535 - q) * 2 - 32767; }
    case 2: return (int32_t)ph >> 16;
    case 3: return ~((int32_t)ph >> 16);
    case 4: return ph < 0x80000000u ? 32767 : -32767;
    case 5: { uint32_t x = (cnt + (uint32_t)s * 0x9E37u) * 2654435761u; x ^= x >> 15; return (int16_t)(x >> 16); }
    default: { const int32_t q = (int32_t)ph >> 16, v = (q * (32768 - (q < 0 ? -q : q))) >> 13; return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }
    }
}
static const uint32_t sl_semi_q16[49];                           /* (below) */
static int32_t sl_lorenz(volatile tm_sllfo *L, uint32_t adv, int out_z)   /* lfo/quadlfo.c ql_lorenz (rho 28), Q15 */
{
    const int32_t one = 65536;
    if (!L->lz || L->lx > 100 * one || L->lx < -100 * one || L->ly > 100 * one || L->ly < -100 * one || L->lz > 100 * one || L->lz < 0) {
        L->lx = one; L->ly = one; L->lz = 20 * one;              /* start / runaway */
    }
    uint32_t dt = (uint32_t)(((adv >> 8) * 179u) >> 16);         /* one cycle ~ 0.7 time units */
    int n = (int)((dt + 654) / 655);                             /* Euler sub-steps <= 0.01 (per block, not per sample) */
    if (n > 16) { n = 16; dt = 16 * 655; }
    const int32_t h = n ? (int32_t)(dt / (uint32_t)n) : 0, rho = 28 * 256;
    int32_t x = L->lx, y = L->ly, z = L->lz;
    for (int k = 0; k < n; k++) {
        const int32_t xs = x >> 8, ys = y >> 8, zs = z >> 8;
        const int32_t dx = 10 * (y - x), dy = xs * (rho - zs) - y, dz = xs * ys - ((z >> 8) * 683);
        x += ((dx >> 8) * h) >> 8; y += ((dy >> 8) * h) >> 8; z += ((dz >> 8) * h) >> 8;
    }
    L->lx = x; L->ly = y; L->lz = z;
    const int32_t v = out_z ? (((z - 24 * one) >> 4) * 1638) >> 12 : ((x >> 4) * 1638) >> 12;
    return v > 32767 ? 32767 : v < -32767 ? -32767 : v;
}
typedef struct { const int16_t *p[2]; int32_t c[2]; uint8_t lru; } sl_rc;   /* two chunk pointers (a read and its neighbour) */
static inline int32_t sl_rd(int t, int32_t i, sl_rc *r)          /* a slot sample: a lookup only when the chunk changes */
{
    const int32_t c = i >> 15;
    int k = c == r->c[0] ? 0 : c == r->c[1] ? 1 : -1;
    if (k < 0) {
        k = r->lru; r->lru ^= 1; r->c[k] = c;
        const int16_t *cp = 0; int32_t cc = -1; (void)tm_sample(t, i, &cp, &cc); r->p[k] = cp;   /* (one miss count per chunk) */
    } else r->lru = (uint8_t)(k ^ 1);
    return r->p[k] ? r->p[k][i & (CHUNK - 1)] : 0;
}
static uint32_t sl_pitch_q16(int32_t q8)                          /* 2^(q8 / 256 / 12), Q16, -24 .. 24 semitones */
{
    if (q8 < -24 * 256) q8 = -24 * 256; else if (q8 > 24 * 256) q8 = 24 * 256;
    const int i = (q8 >> 8) + 24, f = q8 & 255;
    if (i >= 48) return sl_semi_q16[48];
    return sl_semi_q16[i] + (uint32_t)(((int32_t)(sl_semi_q16[i + 1] - sl_semi_q16[i]) * f) >> 8);
}
static void sl_fire(volatile tm_slm *q)                          /* start: from its start, the envelope from 0 */
{
    q->pend = 0;
    {                                                            /* restarting LFOs from 0 */
        volatile tm_sllfo *L = tm_sl_lfo[q - tm_sl];
        for (int k = 0; k < 4; k++) if (L[k].type) { L[k].ph = 0; L[k].cnt = 0; L[k].done = 0; L[k].lz = 0; }   /* (Lorenz: from its start) */
    }
    if (q->len <= 0) return;
    q->pos = q->rev ? q->len - 1 : 0; q->frac = 0; q->gph = 0; q->pdir = (int8_t)(q->rev ? -1 : 1);
    q->g = 0; q->stage = 0; q->gate = 1; q->play = 1;
}
static const uint32_t sl_semi_q16[49] = {                       /* 2^(n / 12), n = -24 .. 24, Q16 */
    16384, 17358, 18390, 19484, 20643, 21870, 23170, 24548, 26008, 27554, 29193, 30929,
    32768, 34716, 36781, 38968, 41285, 43740, 46341, 49097, 52016, 55109, 58386, 61858,
    65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715,
    131072, 138866, 147123, 155872, 165140, 174959, 185364, 196386, 208064, 220436, 233544, 247431, 262144 };
volatile uint8_t tm_sl_full;                                     /* the recording reached 2:00 (the GUI stops it) */
volatile int32_t tm_sl_queue = -1;                               /* a chunk to queue in R246's cache (record start) */
static inline int32_t sl_base(int s) { return (s % 3) * SL_MAX; }
SLP(volatile uint32_t tm_prof[12];)                               /* PROFILE (docs/86): start, pre, loop, post, mix, out, rec, blocks */
static void tm_slots_block(int32_t *accL, int32_t *accR)
{
    SLP(uint32_t pf = sl_cyc();)
#ifndef OP1_HOST
    if (tm_sl_queue >= 0) { QUEUE_CLIP_EARLY(tm_sl_queue); tm_sl_queue = -1; }
#endif
    int32_t (*const cm)[64] = sl_hot->cm, (*const chl)[64] = sl_hot->chl, (*const chr)[64] = sl_hot->chr;   /* per channel: centred
                                                                  * slots (mono), panned ones (stereo); heap 0 */
    uint8_t um = 0, us = 0;                                      /* channels written this block (bits) */
    /* fast mix (docs/86: an SDRAM store costs ~23 cycles on the device, a load 3): plain slots are summed sample by sample
     * in registers straight into accL / accR, with no per-slot or per-channel buffers; only without per-track outs */
#ifndef OP1_HOST
    extern volatile uint8_t usb_tracks_live; extern int fr_tracks_needed(void);
    const int fmx = !(usb_tracks_live || fr_tracks_needed());
#else
    extern int host_sl_fmx; const int fmx = host_sl_fmx;         /* harness/slfast_test.c switches it */
#endif
    const int16_t *fm_src[SL_N]; int32_t fm_gl[SL_N], fm_gr[SL_N]; int nfm = 0;
    const int16_t *fc_src[SL_N]; int32_t fc_g[SL_N]; int nfc = 0;
    sl_dr dr[SL_N]; int ndr = 0;                                 /* (b, c): reverse / resampled blocks, after the assembly mix */   /* (equal left / right gain: one multiply) */
    sl_mpk_on = (uint8_t)fmx; sl_mpk[0] = sl_mpk[1] = sl_mpk[2] = sl_mpk[3] = 0;
    SLP(if (++sl_pcn == 256) { for (int k = 0; k < 3; k++) { tm_sl_pcyc[k] = sl_pc[k] >> 8; sl_pc[k] = 0; } sl_pcn = 0; }
        tm_sl_paths[0] = sl_np[0]; tm_sl_paths[1] = sl_np[1]; tm_sl_paths[2] = sl_np[2]; sl_np[0] = sl_np[1] = sl_np[2] = 0;)
    const int rs = tm_sl_rec;
    {                                                            /* starts asked for: now, or (sync) at the bar in this block */
        int busy = rs >= 0 && !tm_sl_recwait, want = tm_sl_recwait && rs >= 0;
        for (int s = 0; s < SL_N; s++) { busy |= tm_sl[s].play; want |= tm_sl[s].pend | tm_sl[s].pstop; }
        if (want) {                                              /* per slot: unsynced now; synced at the bar (now if nothing runs) */
            const uint32_t bar = tm_sl_bar < 1024 ? 1024 : (uint32_t)tm_sl_bar;
            const int at_bar = !busy || (sl_clock - sl_anchor) % bar + 64 >= bar;
            if (!busy) sl_anchor = sl_clock;                     /* nothing running: this block is bar 1 */
            for (int s = 0; s < SL_N; s++) {
                volatile tm_slm *q = &tm_sl[s];
                if (q->pend == 1 || (q->pend == 2 && at_bar)) sl_fire(q);
                if (q->pstop && at_bar) { q->pstop = 0; q->gate = 0; }
            }
            if (tm_sl_recwait && at_bar) { tm_sl_recwait = 0; tm_sl_recpos = 0; }
        }
        sl_clock += 64;
    }
    SLP({ const uint32_t c = sl_cyc(); tm_prof[0] += c - pf; pf = c; })
    SLP(uint32_t pa = 0, pb = 0, pc = 0, pt;)                         /* PROFILE (fast slots): top .. envelope + LFO, setup, fast part */
    for (int s = 0; s < SL_N; s++) {
        volatile tm_slm *q = &tm_sl[s];
        SLP(pt = sl_cyc();)
        const int t = s / 3; const int32_t base = sl_base(s), len = q->len;
        if (len > 0 && ((s ^ blk) & 3) == 0) {                  /* its start stays cached (an instant start, and where a loop wraps
                                                                  * to): the first chunk, reversed the last; and the one 8192 samples
                                                                  * on if the start is near a chunk's end (slots 2/3 of a track) */
            const int32_t a0 = q->rev ? base + len - 1 : base, a1 = q->rev ? a0 - 8192 : a0 + 8192;
            tm_chunk(t, a0 >> 15, 1);
            if ((a1 >> 15) != (a0 >> 15) && a1 >= base && a1 < base + len) tm_chunk(t, a1 >> 15, 1);
        }   /* every slot's first chunk stays cached (instant
                                                                  * starts): touched every 4th block, staggered */
        if (!q->play || len <= 0 || (s == rs && !tm_sl_od)) { q->play = 0; continue; }
        /* the envelope, per block (one division per sounding slot), ramped over the block */
        const int32_t g0 = q->g; int32_t g = g0;
        if (!q->gate && q->stage < 3) q->stage = 3;
        {
            if (!sl_envstep[0]) for (int k = 0; k <= 100; k++) { const int32_t bl = k * k * 353 >> 10; sl_envstep[k] = bl > 0 ? 32768 / bl : 32768; }   /* ms = v^2 / 2 */
            const int st = q->stage, v = st == 0 ? q->a : st == 3 ? q->r : q->d;
            const int32_t step = sl_envstep[v > 100 ? 100 : v];
            const int32_t sus = q->s * 10486 >> 5;               /* x 32768 / 100 without a division */
            if (st == 0) { g += step; if (g >= 32768) { g = 32768; q->stage = 1; } }
            else if (st == 1) { g -= step; if (g <= sus) { g = sus; q->stage = 2; } }
            else if (st == 2) g = sus;
            else { g -= step; if (g <= 0) g = 0; }
        }
        q->g = g;
        int32_t m[4] = { 0, 0, 0, 0 };                           /* the LFOs (T4): level, pan, pitch, speed; Q15 */
        if (tm_sl_lfoon[s]) for (int k = 0; k < 4; k++) {
            volatile tm_sllfo *L = &tm_sl_lfo[s][k];
            if (!L->amt) { L->val = 0; continue; }
            uint32_t adv = 0;
            if (!L->done) {
                const uint32_t inc = L->sync ? tm_sl_dinc[L->rate < SL_DIVS ? L->rate : SL_DIVS - 1] : tm_sl_rinc[L->rate & 63];
                const uint32_t p = L->ph + inc; adv = inc;
                if (p < L->ph) { L->cnt++; if (L->type == 2) { L->done = 1; L->ph = 0xFFFFFFFFu; } else L->ph = p; }   /* one cycle: holds */
                else L->ph = p;
            }
            const int32_t sh = L->wave >= 6 ? sl_lorenz(L, adv, L->wave == 7) : sl_lfo_shape(L->wave, L->ph, L->cnt, s);
            const int32_t v = sh * L->amt * 1057 >> 15;          /* x amount / 31 */
            L->val = (int16_t)v; m[L->tgt & 3] += v;
        }
        SLP(uint32_t pt1 = sl_cyc();)
        int32_t lv = q->lvl * 10486 >> 5;
        if (m[0]) { const int32_t f = 32768 + m[0]; lv = lv * (f < 0 ? 0 : f > 65536 ? 65536 : f) >> 15; }
        const int32_t ga = g0 * lv >> 15, gb = g * lv >> 15, gd = (gb - ga) >> 6;
        int32_t pos = q->pos; if (pos >= len || pos < 0) pos = 0;
        uint32_t fr = q->frac; int dir = q->pdir ? q->pdir : 1;
        const uint32_t pstep = sl_pitch_q16(q->semi * 256 + (m[2] * 3 >> 5));   /* + LFO: +-12 semitones at full */
        uint32_t tr = 65536;                                     /* synced: the speed follows the tempo (now / recorded) */
        if (q->sync && q->bpm > 0 && tm_sl_tr[s]) tr = tm_sl_tr[s];   /* (the GUI's ratio) */
        if (m[3]) tr = (uint32_t)((uint64_t)tr * sl_pitch_q16(m[3] * 3 >> 5) >> 16);   /* LFO speed: +-12 semitones */
        const int stretch = q->pmode == 1 && len >= 2 * SL_G;     /* (always: an LFO crossing 0 would switch the algorithm) */
        const uint32_t step = stretch ? pstep : (uint32_t)((uint64_t)pstep * tr >> 16);   /* tape: pitch x tempo */
        if (step == 65536 && !stretch) fr = 0;                   /* back at 1x: whole samples again (the fast path; < 1 sample) */
        const int mode = q->mode;
        SLP(const uint32_t pt2 = sl_cyc();)
        if (fmx && stretch && ga == gb && s != rs) {             /* stretch: the block's reads = the position's run + a grain */
            const int32_t padv = (int32_t)((fr + 64u * tr) >> 16) + 1, span = padv + SL_G + 1;
            const int32_t lo = dir > 0 ? pos : pos - span, hi = dir > 0 ? pos + span : pos;
            const int32_t cnow = (base + pos) >> 15;
            if (lo >= 48 && hi < len - 48 && ((base + lo) >> 15) == cnow && ((base + hi) >> 15) == cnow) {
                const int16_t *cp = 0; { int32_t cc2 = -1; (void)tm_sample(t, base + pos, &cp, &cc2); }
                if (cp) {
                    const int pv = q->pan + (m[1] * 50 >> 15), pn = pv < -50 ? -50 : pv > 50 ? 50 : pv, c = q->ch & 3;
                    const int32_t pl = pn < 0 ? 32768 : (50 - pn) * 655, pr = pn > 0 ? 32768 : (50 + pn) * 655;
                    int32_t gl = ((ga >> 1) * pl >> 14) * GAIN_L(c) >> 15, gr = ((ga >> 1) * pr >> 14) * GAIN_R(c) >> 15;
                    gl = gl < 0 ? 0 : gl > 65535 ? 65535 : gl; gr = gr < 0 ? 0 : gr > 65535 ? 65535 : gr;
                    const uint32_t M = ((uint32_t)SL_G << 16) - 1; const int32_t dph = (int32_t)step - (int32_t)tr;
                    sl_dr *d = &dr[ndr++];
                    d->src = cp + ((base + pos) & (CHUNK - 1)); d->fr = fr; d->step = step; d->dir = dir; d->gl = gl; d->gr = gr;
                    d->s = (int8_t)s; d->c = (int8_t)c; d->bin = pos >> SL_PKS; d->ga = ga; d->str = 1; d->ph = q->gph; d->tr = tr; d->dph = dph;
                    SLP(sl_np[2]++; sl_direct_blocks++;)
                    q->gph = (q->gph + 64u * (uint32_t)dph) & M;   /* as stepping 64 samples */
                    const uint32_t fe = fr + 64u * tr;
                    pos += dir * (int32_t)(fe >> 16); q->frac = (uint16_t)(fe & 0xFFFF); q->pos = pos;
                    if (q->stage == 3 && g == 0) q->play = 0;
                    if (((s ^ blk) & 3) == 0) {                  /* ahead: the next chunk in the direction it plays */
                        const int32_t cn = (base + pos) >> 15;
                        if (dir > 0 && cn < ((base + len - 1) >> 15)) tm_chunk(t, cn + 1, 1);
                        if (dir < 0 && cn > (base >> 15)) tm_chunk(t, cn - 1, 1);
                    }
                    continue;
                }
            }
        }
        if (fmx && !stretch && ga == gb && s != rs) {            /* a block that stays inside one chunk, away from the slot's ends
                                                                  * (any mode, either direction, any rate): no per-sample checks */
            const uint32_t tot = fr + 63u * step;                /* the last sample's position (+ its interpolation neighbour) */
            const int32_t adv = (int32_t)(tot >> 16) + 1, lo = dir > 0 ? pos : pos - adv, hi = dir > 0 ? pos + adv : pos;
            const int32_t cnow = (base + pos) >> 15;
            if (lo >= 48 && hi < len - 48 && ((base + lo) >> 15) == cnow && ((base + hi) >> 15) == cnow) {
                const int16_t *cp = 0;                           /* every block: the touch keeps it in tm_chunk's LRU (a pointer kept
                                                                  * across blocks could be evicted and reused under us) */
                { int32_t cc2 = -1; (void)tm_sample(t, base + pos, &cp, &cc2); }
                if (cp) {
                    const int16_t *src = cp + ((base + pos) & (CHUNK - 1));
                    const int pv = q->pan + (m[1] * 50 >> 15), pn = pv < -50 ? -50 : pv > 50 ? 50 : pv, c = q->ch & 3;
                    const int32_t pl = pn < 0 ? 32768 : (50 - pn) * 655, pr = pn > 0 ? 32768 : (50 + pn) * 655;
                    int32_t gl = ((ga >> 1) * pl >> 14) * GAIN_L(c) >> 15, gr = ((ga >> 1) * pr >> 14) * GAIN_R(c) >> 15;
                    gl = gl < 0 ? 0 : gl > 65535 ? 65535 : gl; gr = gr < 0 ? 0 : gr > 65535 ? 65535 : gr;
                    if (step == 65536 && !fr && dir > 0 && gl <= 32767 && gr <= 32767) {   /* (a) 1x forward: the assembly mix */
                        if (gl == gr) { fc_src[nfc] = src; fc_g[nfc] = gl; nfc++; }
                        else { fm_src[nfm] = src; fm_gl[nfm] = gl; fm_gr[nfm] = gr; nfm++; }
                        int32_t pk = 0;                          /* peaks: three samples (waveform, channel meter) */
                        for (int k = 0; k < 64; k += 31) { const int32_t a = src[k] < 0 ? -src[k] : src[k]; if (a > pk) pk = a; }
                        { const uint8_t a = (uint8_t)(pk >> 7); volatile uint8_t *pp = &tm_sl_peak[s][pos >> SL_PKS]; if (a > *pp) *pp = a; }
                        { const int32_t mp = pk * ga >> 15; if (mp > sl_mpk[c]) sl_mpk[c] = mp; }
                        SLP(sl_np[0]++; sl_fast_blocks++;)
                    } else {                                     /* (b, c) reverse / any rate: the direct loop after the mix */
                        sl_dr *d = &dr[ndr++];
                        d->src = src; d->fr = fr; d->step = step; d->dir = dir; d->gl = gl; d->gr = gr; d->s = (int8_t)s; d->c = (int8_t)c;
                        d->bin = pos >> SL_PKS; d->ga = ga; d->str = 0;
                        SLP(sl_np[1]++; sl_direct_blocks++;)
                    }
                    const int32_t pe = pos + dir * (int32_t)((fr + 64u * step) >> 16);   /* as stepping 64 samples */
                    q->frac = (uint16_t)((fr + 64u * step) & 0xFFFF); pos = pe; q->pos = pos;
                    if (q->stage == 3 && g == 0) q->play = 0;
                    const int32_t off = (base + pos) & (CHUNK - 1), cn = (base + pos) >> 15;
                    if (((s ^ blk) & 3) == 0) {                  /* ahead, near the chunk's end in the direction it plays */
                        if (dir > 0 && cn < ((base + len - 1) >> 15)) tm_chunk(t, cn + 1, 1);   /* (early: NAND is slow, and slots
                                                                  * cross chunks together) */
                        if (dir < 0 && cn > (base >> 15)) tm_chunk(t, cn - 1, 1);
                        (void)off;
                    }
                    SLP({ const uint32_t e = sl_cyc(); pa += pt1 - pt; pb += pt2 - pt1; pc += e - pt2; })
                    continue;
                }
            }
        }

        const int16_t *cache = 0; int32_t cc = -1;
        sl_rc rc = { { 0, 0 }, { -1, -1 }, 0 };
        int32_t o[64];                                           /* this slot's block (mono, on the stack), then into its channel */
        volatile uint8_t *const pkv = tm_sl_peak[s];             /* its waveform (from what plays) */
        int32_t gg = ga;
        for (int i = 0; i < 64; i++) o[i] = 0;
        if (s == rs) { sl_od_h0 = pos; sl_od_step = dir * (int32_t)(stretch ? tr : step); }   /* overdub: record along the playback */
        SLP(const int path = stretch ? 2 : step == 65536 && !fr && dir > 0 && mode == 0 && len > 128 ? 0 : 1;)
        SLP(sl_np[path]++; const uint32_t pc0 = sl_cyc();)
        if (stretch) {       /* stretch: two grains (half a grain apart, triangle windows)
                                                                  * read at the pitch around a 1x position */
            uint32_t ph = q->gph; const int32_t dph = (int32_t)step - (int32_t)tr;   /* grains at the pitch, the position at the tempo */ const uint32_t M = ((uint32_t)SL_G << 16) - 1;
            for (int i = 0; i < 64; i++) {
                int32_t acc = 0;
                for (int k = 0; k < 2; k++) {
                    const uint32_t p = (ph + (k ? (uint32_t)SL_G << 15 : 0)) & M;
                    const int32_t off = (int32_t)(p >> 16), f = (int32_t)(p & 0xFFFF) >> 1;
                    int32_t a = pos + dir * off, b2 = a + dir;
                    if (a >= len) a -= len; else if (a < 0) a += len;   /* |off| < len / 2: one wrap */
                    if (b2 >= len) b2 -= len; else if (b2 < 0) b2 += len;
                    const int32_t x0 = sl_rd(t, base + a, &rc), x1 = sl_rd(t, base + b2, &rc);
                    const int32_t w = (off < SL_G / 2 ? off : SL_G - off) << 5;   /* Q15, the two sum to 1 */
                    const int32_t ed = a < len - 1 - a ? a : len - 1 - a;   /* the loop seam: ~1 ms fades either side */
                    const int32_t x = x0 + ((x1 - x0) * f >> 15);
                    acc += (ed < 48 ? x * ed * 683 >> 15 : x) * w >> 15;
                }
                { const uint8_t pa = (uint8_t)((acc < 0 ? -acc : acc) >> 7); volatile uint8_t *pk = &pkv[pos >> SL_PKS]; if (pa > *pk) *pk = pa; }
                o[i] += acc * gg >> 15; gg += gd;
                ph = (ph + (uint32_t)dph) & M;
                fr += tr; pos += dir * (int32_t)(fr >> 16); fr &= 0xFFFF;
                if (pos >= len || pos < 0) {
                    if (mode == 1) { q->play = 0; q->gate = 0; pos = 0; break; }
                    if (mode == 2) { if (pos >= len) { pos = len - 1; dir = -1; } else { pos = 0; dir = 1; } }
                    else pos = pos < 0 ? len - 1 : 0;
                }
            }
            q->gph = ph; q->frac = (uint16_t)fr;
        } else if (step == 65536 && !fr && dir > 0 && mode == 0 && len > 128) {   /* 1x forward loop (the common case): runs
                                                                  * straight from the chunk, no interpolation, one peak per run */
            int i = 0;
            while (i < 64) {
                int32_t n = 64 - i; const int32_t te = len - pos, off = (base + pos) & (CHUNK - 1), tc = CHUNK - off;
                if (n > te) n = te;
                if (n > tc) n = tc;
                (void)tm_sample(t, base + pos, &cache, &cc);
                const int16_t *src = cache ? cache + off : 0;
                if (!src) { gg += gd * n; }
                else {
                    int32_t pk = 0;
                    const int edge = pos < 48 || pos + n > len - 48;
                    for (int k = 0; k < n; k++) {
                        int32_t x = src[k];
                        const int32_t ax = x < 0 ? -x : x; if (ax > pk) pk = ax;
                        if (edge) { const int32_t p2 = pos + k, ed = p2 < len - 1 - p2 ? p2 : len - 1 - p2; if (ed < 48) x = x * ed * 683 >> 15; }
                        o[i + k] += x * gg >> 15; gg += gd;
                    }
                    const uint8_t a = (uint8_t)(pk >> 7); volatile uint8_t *pp = &pkv[pos >> SL_PKS]; if (a > *pp) *pp = a;
                }
                pos += n; if (pos >= len) pos = 0;
                i += n;
            }
        } else
        for (int i = 0; i < 64; i++) {
            int32_t nx = pos + dir;                              /* the neighbour for the interpolation */
            if (nx >= len) nx = mode == 2 ? len - 1 : 0; else if (nx < 0) nx = mode == 2 ? 0 : len - 1;
            const int32_t x0 = sl_rd(t, base + pos, &rc), x1 = sl_rd(t, base + nx, &rc);
            { const uint8_t a = (uint8_t)((x0 < 0 ? -x0 : x0) >> 7); volatile uint8_t *pk = &pkv[pos >> SL_PKS]; if (a > *pk) *pk = a; }
            int32_t x = x0 + ((x1 - x0) * (int32_t)(fr >> 1) >> 15);
            { const int32_t ed = pos < len - 1 - pos ? pos : len - 1 - pos; if (ed < 48) x = x * ed * 683 >> 15; }   /* the loop seam: ~1 ms fades (683 = 2^15 / 48) */
            o[i] += x * gg >> 15; gg += gd;
            fr += step; pos += dir * (int32_t)(fr >> 16); fr &= 0xFFFF;
            if (pos >= len || pos < 0) {
                if (mode == 1) { q->play = 0; q->gate = 0; pos = 0; break; }   /* once: the end */
                if (mode == 2) { if (pos >= len) { pos = 2 * len - 2 - pos; dir = -1; } else { pos = -pos; dir = 1; }   /* ping-pong */
                                 if (pos < 0 || pos >= len) pos = 0; }
                else { pos %= len; if (pos < 0) pos += len; }
            }
        }
        SLP(sl_pc[path] += sl_cyc() - pc0;)
        q->frac = (uint16_t)fr; q->pdir = (int8_t)dir;
        if (q->stage == 3 && g == 0) q->play = 0;                /* released */
        q->pos = pos;
        {                                                        /* into the slot's channel: centred mono, else panned stereo */
            const int pv = q->pan + (m[1] * 50 >> 15), pn = pv < -50 ? -50 : pv > 50 ? 50 : pv, c = q->ch & 3;
            if (!pn) {
                int32_t *M = cm[c];
                if (um & (1 << c)) for (int i = 0; i < 64; i++) M[i] += o[i]; else { for (int i = 0; i < 64; i++) M[i] = o[i]; um |= (uint8_t)(1 << c); }
            } else {
                const int32_t pl = pn < 0 ? 32768 : (50 - pn) * 655, pr = pn > 0 ? 32768 : (50 + pn) * 655;
                int32_t *L = chl[c], *R = chr[c];
                if (!(us & (1 << c))) { for (int i = 0; i < 64; i++) L[i] = R[i] = 0; us |= (uint8_t)(1 << c); }
                for (int i = 0; i < 64; i++) { L[i] += o[i] * pl >> 15; R[i] += o[i] * pr >> 15; }
            }
        }
        const int32_t c = (base + pos) >> 15, last = (base + len - 1) >> 15;
        if (c < last) tm_chunk(t, c + 1, 1);                     /* the chunks either side (reverse, ping-pong) */
        if (c > (base >> 15) && (dir < 0 || mode == 2 || stretch)) tm_chunk(t, c - 1, 1);   /* behind: only when it can go there */
    }
    SLP({ const uint32_t c = sl_cyc(); tm_prof[1] += c - pf; pf = c; } tm_prof[8] += pa; tm_prof[9] += pb; tm_prof[10] += pc;)
    for (int k = 0; k < nfc || k == 0; k += 8) {                 /* the fast mix (tape/sl_mix.S): centred slots, 8 per pass, both
                                                                  * sides at once; the first pass assigns (no zeroing) */
        sl_m8 m8; m8.add = k > 0;
        for (int j = 0; j < 8; j++) { m8.p[j] = k + j < nfc ? fc_src[k + j] : sl_zero; m8.g[j] = (int16_t)(k + j < nfc ? fc_g[k + j] : 0); }
        sl_macc8(accL, accR, &m8);
    }
    sl_mix_list(accL, fm_src, fm_gl, nfm); sl_mix_list(accR, fm_src, fm_gr, nfm);
    for (int k = 0; k < ndr; k++) sl_direct(accL, accR, &dr[k]);
    for (int c = 0; c < 4; c++) {
        const int16_t gl = GAIN_L(c), gr = GAIN_R(c);
        const int hm = um >> c & 1, hs = us >> c & 1;
        if (!hm && !hs) { if (!fmx) for (int i = 0; i < 64; i++) tm_v[c][i] = 0; continue; }   /* silent channel */
        int32_t mp = sl_mpk[c];
        for (int i = 0; i < 64; i++) {
            const int32_t mm = hm ? cm[c][i] : 0, l0 = mm + (hs ? chl[c][i] : 0), r0 = mm + (hs ? chr[c][i] : 0);
            const int32_t l = l0 > 32767 ? 32767 : l0 < -32768 ? -32768 : l0, r = r0 > 32767 ? 32767 : r0 < -32768 ? -32768 : r0;
            const int32_t v = (l + r) >> 1;
            if (fmx) { const int32_t a = v < 0 ? -v : v; if (a > mp) mp = a; } else tm_v[c][i] = (int16_t)v;
            accL[i] += l * gl >> 15; accR[i] += r * gr >> 15;
        }
        sl_mpk[c] = mp;
    }
    SLP({ const uint32_t c = sl_cyc(); tm_prof[4] += c - pf; })
    for (int t = 0; t < 4; t++) rec_step[t] = 0;
    sl_rpk = rs >= 0 && rs < SL_N ? tm_sl_peak[rs] : 0; sl_rpk_base = rs >= 0 ? sl_base(rs) : 0;
    if (rs >= 0 && rs < SL_N && MODE == 1 && !tm_sl_recwait) {                     /* recording: this block's head path on the slot's track */
        const int t = rs / 3; const int32_t base = sl_base(rs), rp = tm_sl_recpos;
        tm_sel = (uint8_t)t;
        if (tm_sl_od) {                                          /* overdub: along the slot's playback, wrapped in its length */
            const int32_t len = tm_sl[rs].len;
            rec_h0[t] = (int64_t)(base + sl_od_h0) << 32; rec_step[t] = (int64_t)sl_od_step << 16;
            rec_lo[t] = (int64_t)base << 32; rec_hi[t] = (int64_t)(base + len) << 32;
            if (!tm_sl[rs].play || len <= 0) rec_step[t] = 0;    /* released / emptied: nothing more */
        } else if (rp + 64 <= SL_MAX) {
            rec_h0[t] = (int64_t)(base + rp) << 32; rec_step[t] = (int64_t)1 << 32;
            rec_lo[t] = (int64_t)base << 32; rec_hi[t] = (int64_t)(base + SL_MAX) << 32;
            const int32_t tg = tm_sl_rectgt > SL_MAX ? SL_MAX : tm_sl_rectgt;
            tm_sl_recpos = rp + 64; tm_sl[rs].len = rp + 64 < tg ? rp + 64 : tg;
            if (rp + 64 >= tg) tm_sl_full = 1;                   /* the fixed length reached (the GUI stops it) */
        } else tm_sl_full = 1;
    }
}

volatile uint32_t tm_cyc;                                        /* cycles in tm_block + recording (sysmon page 5 "fr") */
static void tm_block_body(void);
void tm_block(void)                                              /* audio ISR, after tape_play_mix */
{
#ifndef OP1_HOST
    uint32_t c0; __asm__ volatile ("%0 = CYCLES;" : "=d"(c0));
    tm_block_body();
    uint32_t c1; __asm__ volatile ("%0 = CYCLES;" : "=d"(c1));
    tm_cyc += c1 - c0;
#else
    tm_block_body();
#endif
}
static void tm_block_body(void)
{
    blk++;
    {                                                            /* the meter: the bus R246 records (last block's) */
        int32_t pk = tm_in_peak * 31 >> 5;
        const volatile int16_t *b = BUS;
        for (int i = 0; i < 64; i++) { int32_t v = b[i] < 0 ? -b[i] : b[i]; if (v > pk) pk = v; }
        tm_in_peak = (int16_t)(pk > 32767 ? 32767 : pk);
    }
    tm_live = (uint8_t)(tm_on && slot[0].data);
    if (!tm_on || !slot[0].data) return;                         /* memory comes from tm_service (main loop) */
#ifndef OP1_HOST
    SEL_TRACK = tm_sel & 3;                                      /* R246's selected track = ours, every block: its clipboard worker
                                                                  * (drop) and take close read it later; R246 rewrites it */
#endif
    if (tm_on == 2) {                                            /* the slot machine: its own clock, any transport state */
        int32_t aL[64], aR[64];                                  /* (tm_slots_block assigns them) */
        tm_slots_block(aL, aR);
        SLP(const uint32_t c0 = sl_cyc();)
        tm_out(aL, aR, 0x7FFF, 0x7FFF);
        SLP(tm_prof[5] += sl_cyc() - c0; tm_prof[7]++;)
#ifndef OP1_HOST
        {                                                        /* R246's transport (it runs while a slot records) kept from
                                                                  * the tape end, as FREE: it would stop there, and the take */
            const int32_t m = MODE, p = TPOS >> 6;
            if ((m == 1 || m == 2) && p > TM_XHI) TPOS = (p - TM_XSPAN) << 6;
        }
#endif
        return;
    }
    const int32_t mode = MODE, pos = TPOS >> 6, v = TSPEED;
    if (mode == 1) rec_seen = 1;                                 /* recording (R246's machine) changes chunks we may hold */
    {                                                            /* the transport jumped (locate, marker): heads resync */
        int32_t d = xlast < 0 ? 0x7FFFFFFF : pos - (xlast + ((mode == 1 || mode == 2) ? v >> 21 : 0));
        if (d != 0x7FFFFFFF && LOOP_ON && LOOP_E > LOOP_S) {     /* the transport's own loop wrap is no jump */
            const int32_t L = LOOP_E - LOOP_S;
            d %= L; if (d > L / 2) d -= L; else if (d < -L / 2) d += L;
        }
        if (mode == 1 || mode == 2) tm_hold_heads = 0;
        if ((d > 1024 || d < -1024) && !tm_hold_heads) tm_resync(pos);   /* restored heads: kept until the first play */
        xlast = pos;
#ifndef OP1_HOST
        /* FREE runs the clock: R246's transport is kept away from the tape ends (it stops at 6:00, and FREE with it);
         * our heads have their own positions, so the transport is moved back by 3 min without a resync */
        if (tm_on && (mode == 1 || mode == 2 || v) && !LOOP_ON) {
            const int32_t span = TM_XSPAN;
            int32_t np = pos;
            if (v >= 0 && pos > TM_XHI) np = pos - span;
            else if (v < 0 && pos < 60 * 44100) np = pos + span;
            if (np != pos) { TPOS = np << 6; xlast = np; }
        }
#endif
    }
    {                                                            /* scrubbing: spin up / down to +-8x like stock winding */
        const int32_t target = tm_scrub * (8 << 16), dv = 0x2000;
        scrub_v = scrub_v < target ? (target - scrub_v < dv ? target : scrub_v + dv) : (scrub_v - target < dv ? target : scrub_v - dv);
    }
    const int playing = mode == 1 || mode == 2 || (mode == 0 && v != 0), sc = tm_scrub_track & 3;   /* stop: spin down as stock */
    if (!playing && !scrub_v) { fade_prev = 0; return; }         /* stopped: R246's (silent) output stays; next start ramps from 0 */
    if (playing && rec_seen && mode == 2) {                      /* first play after a recording: reload every chunk */
        rec_seen = 0;
        tm_refresh_all();
    }
    int32_t accL[64], accR[64];
    for (int i = 0; i < 64; i++) accL[i] = accR[i] = 0;
    for (int t = 0; t < 4; t++) {
        if (scrub_v && t == sc) tm_track_block(t, (int64_t)scrub_v << 16, 0, 16, accL, accR);   /* -6 dB, no loops */
        else if (tm_tr[t].stop) { rec_step[t] = 0; for (int i = 0; i < 64; i++) tm_v[t][i] = 0; }   /* stopped track: its head
                                                                  * stands, silent, no recording (tm_record: step 0) */
        else if (playing) {
            const int64_t step = ((int64_t)v * tm_tr[t].speed) >> 11;   /* (1x = 2^27) x Q16 -> Q32 */
            if (!tm_fast(t, step, accL, accR)) tm_track_block(t, step, 1, 15, accL, accR);
        }
        else for (int i = 0; i < 64; i++) tm_v[t][i] = 0;
    }
    int32_t fade = 0x7FFF;                                       /* starting / stopping: below 1/4x the volume follows the speed,
                                                                  * so neither the first sample nor the held near-DC of an almost
                                                                  * stopped tape pops (R246's W[0x01B4EF78] is no fade) */
    if (!(scrub_v && !playing)) { const int32_t av = v < 0 ? -v : v; fade = av >> 10 > 0x7FFF ? 0x7FFF : av >> 10; }
    const int32_t f0 = fade_prev; fade_prev = fade;
    tm_out(accL, accR, f0, fade);
}

/* ---- recording (FREE): the active track at its own head / speed / loop (docs/82) ----
 * In place of R246's tape_record_block (both CALLs -> hook.S l1_tramp_rec -> tm_rec_entry): the bus x the record level
 * onto the tape positions the head crossed this block (as played, tm_track_block), linear between the bus samples,
 * overdub sat16(old + new), into R246's clip cache entry of the chunk (dirty + track bit: tape_c::service saves it)
 * and our read slot. A chunk not in R246's cache is queued and that part skipped (as R246 does). The take: range
 * reset at record start (0x01A54C1C), widened per block (0x01A54BB4): R246 closes it (Clips) on stop, onto its
 * selected track (= ours: tfscreen.c sets it). Without an own loop the take ends at the tape end (either way), as
 * stock's (R246's transport stops there): a wrap to the other end would widen the range to the whole track. */
#if !defined(OP1_HOST) || defined(TM_REC_HOST)                   /* host: tmrec_test.c fakes R246's parts */
#ifndef OP1_HOST
#define REC_LEVEL  (*(volatile int16_t *)0x01B4EF6E)
#define QUEUE_CLIP ((void (*)(int))0x01A54CD4)
#define RANGE_ADD  ((void (*)(int32_t))0x01A54BB4)
#define RANGE_RST  ((void (*)(void))0x01A54C1C)
#define REC_STOCK  ((void (*)(void *))0xFFA06334)
#endif
volatile uint32_t tm_rec_written, tm_rec_missed;
static uint8_t rec_on, rec_end;                                  /* rec_end: the tape end reached (no own loop) */
/* our read copy gets the recorded samples one block late: the record track's own playback (4-point, reads n - 1 .. n + 2)
 * would otherwise hear the live input a few samples late (a comb: "resonance" at speeds != 1x); the next loop pass
 * hears them, R246's cache has them at once */
/* slot + gen, not a pointer: tm_service may swap the slot's buffer, or the ISR reassign the slot, between two blocks */
#define PEND_MAX 520
static uint16_t pend_o[PEND_MAX]; static int8_t pend_s[PEND_MAX]; static uint8_t pend_g[PEND_MAX]; static int16_t pend_v[PEND_MAX];
static int pend_n;
static int16_t rec_fade;
static void tm_record(const volatile int16_t *bus)
{
    const int t = tm_sel & 3;                                    /* ours: R246 rewrites 0xFF802964 in several places */
    for (int k = 0; k < pend_n; k++) {                           /* last block's samples into our copy (after this block's playback) */
        tm_slot *p = &slot[pend_s[k]];
        if (p->gen != pend_g[k] || p->state != 2) continue;      /* reassigned (or a reread queued) meanwhile */
        p->data[pend_o[k]] = pend_v[k];
        if (pend_s[k] == tm_rd) spare[pend_o[k]] = pend_v[k];   /* being reread: the new copy too (its read may be past it) */
    }
    pend_n = 0;
    if (MODE != 1) { rec_on = 0; return; }
    SEL_TRACK = t;                                               /* R246 closes the take onto this track */
    if (!rec_on) { rec_on = 1; rec_end = 0; rec_fade = 0; RANGE_RST(); }
    const int64_t step = rec_step[t], lo = rec_lo[t], hi = rec_hi[t];
    if (!step || rec_end) return;
    const int full = hi - lo >= (int64_t)(TM_CHUNKS * CHUNK) << 32;   /* no own loop */
    const int32_t lvl = REC_LEVEL < 0 ? 0 : REC_LEVEL;
    int32_t in[65];
    for (int i = 0; i < 64; i++) {                               /* level, fade-in over the first block */
        int32_t s = bus[i] * lvl >> 15;
        if (rec_fade < 0x7FFF) { s = s * rec_fade >> 15; rec_fade = (int16_t)(rec_fade + 512 > 0x7FFF ? 0x7FFF : rec_fade + 512); }
        in[i] = s;
    }
    in[64] = in[63];
    const int64_t ast = step < 0 ? -step : step;
    const uint32_t inv = (uint32_t)((((uint64_t)1 << 47) / (uint64_t)ast));   /* 2^15 / |step| in samples, Q32 -> frac Q15 */
    int64_t h = rec_h0[t];
    int32_t first = -1, last = -1;
    static int32_t box_sum; static int box_n;
    static int16_t recip[33];                                    /* 2^15 / k */
    if (!recip[1]) for (int k = 1; k <= 32; k++) recip[k] = (int16_t)(k == 1 ? 32767 : 32768 / k);
    const uint8_t *e = 0; int32_t ec = -1; int slot_s = -1;
    for (int i = 0; i < 64; i++) {
        const int64_t h1 = h + step;
        int32_t n = step > 0 ? (int32_t)((h + 0xFFFFFFFFLL) >> 32) : (int32_t)(h >> 32), nend = (int32_t)(h1 >> 32);
        int crossed = 0;
        for (int g = 0; g < 8; g++) {                            /* the integer positions crossed (at most 8: up to 4x + slack) */
            const int64_t nq = (int64_t)n * ((int64_t)1 << 32);   /* (n < 0 at 0:00 in reverse: no negative shift) */
            if (step > 0 ? nq >= h1 : nq <= h1) break;
            int32_t pn = n;
            crossed = 1;
            int64_t q = (int64_t)pn * ((int64_t)1 << 32);
            if (q >= hi || q < lo) {
                if (full) { rec_end = 1; break; }                /* the tape end */
                if (q >= hi) pn -= (int32_t)((hi - lo) >> 32); else pn += (int32_t)((hi - lo) >> 32);
            }
            int32_t v;
            if (ast < ((int64_t)1 << 32)) {                      /* slower than 1x: the mean of the input since the last
                                                                  * position (box filter: no aliasing from the compression) */
                v = box_n ? (box_sum + in[i]) * recip[box_n + 1 > 32 ? 32 : box_n + 1] >> 15 : in[i];
                box_sum = 0; box_n = 0;
            } else {
                const uint32_t fr = (uint32_t)(((uint64_t)(step > 0 ? nq - h : h - nq) >> 17) * inv >> 15) & 0x7FFF;
                v = in[i] + ((in[i + 1] - in[i]) * (int32_t)fr >> 15);
            }
            const int32_t c = pn >> 15;
            if (c != ec) {                                       /* R246's clip cache entry of this chunk, our slot */
                ec = c; e = CLIP_IDX && c >= 0 && c < TM_CHUNKS ? CLIP_IDX[c] : 0;   /* (a loop out from before the clamp) */
                if (!(e && e[0] && *(const int16_t *)(e + 6) == c)) { e = 0; if (c >= 0 && c < TM_CHUNKS) QUEUE_CLIP(c); }
                const int s = c >= 0 && c < TM_CHUNKS ? map[t][c] - 1 : -1;
                slot_s = s >= 0 && slot[s].state == 2 ? s : -1;
            }
            if (e) {
                int16_t *d = (int16_t *)(*(uint8_t *const *)(e + 0x10) + t * 0x10000) + (pn & (CHUNK - 1));
                int32_t o = tm_sl_rec >= 0 && !tm_sl_od ? v : *d + v;   /* a slot (docs/85): replaced (its overdub: mixed), else overdub */
                o = o > 32767 ? 32767 : o < -32768 ? -32768 : o;
                *d = (int16_t)o;
                if (sl_rpk && tm_on == 2) {                      /* a slot's take: its waveform */
                    const int32_t b = (pn - sl_rpk_base) >> SL_PKS; const uint8_t a = (uint8_t)((o < 0 ? -o : o) >> 7);
                    if ((uint32_t)b < SL_PKN && a > sl_rpk[b]) sl_rpk[b] = a;
                }
                if (slot_s >= 0 && pend_n < PEND_MAX) {
                    pend_o[pend_n] = (uint16_t)(pn & (CHUNK - 1)); pend_s[pend_n] = (int8_t)slot_s; pend_g[pend_n] = slot[slot_s].gen;
                    pend_v[pend_n++] = (int16_t)o;
                }
                ((volatile uint8_t *)e)[2] = 1; *(volatile uint16_t *)(e + 4) |= (uint16_t)(1u << t);
                if (first < 0) first = pn;
                last = pn;
                tm_rec_written++;
            } else tm_rec_missed++;
            n += step > 0 ? 1 : -1;
        }
        if (rec_end) break;
        if (!crossed && ast < ((int64_t)1 << 32)) { box_sum += in[i]; if (box_n < 31) box_n++; }   /* waits for the next position */
        (void)nend;
        h = h1;
        if ((h >= hi || h < lo) && full) { rec_end = 1; break; }
        if (h >= hi) h -= hi - lo; else if (h < lo) h += hi - lo;
    }
    if (first >= 0) { RANGE_ADD(first); RANGE_ADD(last); }
    for (int k = 0; k < 2 && !rec_end; k++) {                    /* ahead (not past the tape ends) */
        const int32_t c = ((int32_t)(h >> 32) >> 15) + (k ? (step < 0 ? -1 : 1) : 0);
        if (c >= 0 && c < TM_CHUNKS) QUEUE_CLIP(c);
    }
}
#ifndef OP1_HOST
static void tm_rec_body(void *bus);
#endif
void tm_rec_entry(void *bus)                                     /* audio ISR, in place of tape_record_block (hook.S) */
{
#ifndef OP1_HOST
    uint32_t c0; __asm__ volatile ("%0 = CYCLES;" : "=d"(c0));
    tm_rec_body(bus);
    uint32_t c1; __asm__ volatile ("%0 = CYCLES;" : "=d"(c1));
    tm_cyc += c1 - c0; SLP(tm_prof[6] += c1 - c0;)
}
static void tm_rec_body(void *bus)
{
#endif
    if (!tm_on || !slot[0].data) { REC_STOCK(bus); return; }
    tm_record((const volatile int16_t *)bus);
}
#endif

int32_t tm_head_pos(int t) { return (int32_t)(head[t] >> 32); }   /* GUI: head in samples */
int tm_span(int t, int32_t *lo, int32_t *hi)                     /* GUI: the track's own loop (1 if on) */
{
    if (!(tm_tr[t].lon && tm_tr[t].lout > tm_tr[t].lin)) return 0;
    *lo = tm_tr[t].lin; *hi = tm_tr[t].lout;
    return 1;
}

volatile uint8_t tm_reload_after_clip;                           /* lift / drop on our screen: reload once the clipboard is done */
#ifndef OP1_HOST
#define CLIPS      ((const volatile int32_t *)0x01AA8D70)        /* 512 x { track, start, stop } (db2::Clips) */
#define CLIP_USED  ((const volatile uint8_t *)0x01AAA570)
/* as R246's player: only clips sound. Zero what no clip of track t covers in chunk c (an erase only clears the
 * clip list; the old audio stays in tape_c.raw and would click here) */
static void tm_mask_clips(int t, int c, int16_t *d)
{
    const int32_t a = c * CHUNK, b = a + CHUNK;
    int32_t x0 = 0, x1 = 0;                                      /* the take being recorded on t counts as a clip */
    if (*(volatile uint8_t *)0xFF8029EC && t == (tm_sel & 3)) { x0 = *(volatile int32_t *)0xFF8029E4; x1 = *(volatile int32_t *)0xFF8029E8 + 1; }
    int32_t cur = a;
    while (cur < b) {
        int32_t ext = cur, next = b;                             /* ext: covered up to; next: the next clip start */
        if (x0 <= cur && x1 > ext) ext = x1; else if (x0 > cur && x0 < next && x1 > x0) next = x0;
        for (int i = 0; i < 512; i++) {
            if (!CLIP_USED[i] || CLIPS[i * 3] != t) continue;
            const int32_t s0 = CLIPS[i * 3 + 1], s1 = CLIPS[i * 3 + 2];
            if (s0 <= cur && s1 > ext) ext = s1;
            else if (s0 > cur && s0 < next) next = s0;
        }
        if (ext > cur) { cur = ext; continue; }                  /* inside a clip: skip it */
        for (int32_t k = cur; k < next; k++) d[k - a] = 0;       /* a gap up to the next clip */
        cur = next;
    }
}
/* the clip list changed (erase, delete, lift, record): reload what we hold */
static uint32_t clips_hash(void)
{
    uint32_t h = 0;
    for (int i = 0; i < 512; i++) if (CLIP_USED[i]) h = h * 31 + (uint32_t)(CLIPS[i * 3] + CLIPS[i * 3 + 1] * 7 + CLIPS[i * 3 + 2] * 13);
    return h;
}
void tm_mask_cache(int c, uint8_t *data)                         /* safewrite.c: R246's clip cache entry just read from the file
                                                                  * (4 tracks x 64 KB): recordings must not mix into old audio */
{
    if (tm_on) for (int t = 0; t < 4; t++) tm_mask_clips(t, c, (int16_t *)(data + t * 0x10000));
}
#endif
int tm_service(void)                                             /* main loop (tape_service): one chunk per call */
{
#ifndef OP1_HOST
    {
        static uint32_t last; const uint32_t h = MODE == 1 ? last : clips_hash();   /* recording: the reload follows it */
        if (h != last) { last = h; tm_refresh_all(); }
    }
#endif
#ifndef OP1_HOST
    {                                                            /* the clipboard queue empty for 0.5 s (the worker may still
                                                                  * run the job it popped): reload our chunks */
        static uint32_t idle_t; uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c));
        if (!tm_reload_after_clip || !((int (*)(void))0x0191C750)()) idle_t = c;
        else if (c - idle_t > 200000000u) {
            tm_reload_after_clip = 0;
            tm_refresh_all();
        }
    }
#endif
    if (!tm_on) return 0;
    if (!tm_alloc()) return 0;
    for (int k = 0; k < 2 * TM_SLOTS; k++) {                     /* wanted (silent) chunks first, then the rereads */
        tm_slot *s = &slot[k % TM_SLOTS];
        if (k < TM_SLOTS ? s->state != 1 : !(s->state == 2 && s->refresh)) continue;
        const uint8_t gen = s->gen; const int t = s->t, c = s->chunk;
        int16_t *const d = spare;
        tm_rd = (int8_t)(k % TM_SLOTS);
#ifdef OP1_HOST
        host_tm_read(t, c, d);
#else
        const uint8_t *e = CLIP_IDX ? CLIP_IDX[c] : 0;           /* R246's clip cache holds it (fresh recordings) */
        if (e && e[0] && *(const int16_t *)(e + 6) == c) {
            const int16_t *src = (const int16_t *)(*(uint8_t *const *)(e + 0x10) + t * 0x10000);
            for (int k = 0; k < CHUNK; k++) d[k] = src[k];
        } else {
            Y_LSEEK(TAPE_FD, TRACK_BASE[t] + c * 0x10000, 0);
            Y_READ(TAPE_FD, d, 0x10000);
        }
#endif
#ifndef OP1_HOST
        tm_mask_clips(t, c, d);
#endif
        const uint32_t m = tm_irq_off();                         /* the ISR reassigns slots (tm_chunk): compare and publish as one */
        if (s->gen == gen) { TM_IRQ(1); spare = s->data; s->data = d; s->state = 2; s->refresh = 0; }   /* reassigned meanwhile:
                                                                  * stays wanted, d stays the spare */
        tm_rd = -1;
        tm_irq_on(m);
        tm_loads++;
        return 1;
    }
    return 0;
}
