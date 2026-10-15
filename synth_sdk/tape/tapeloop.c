/* Polymetric tape loops: every track loops over its own length. Global setting, toggled by pressing the blue encoder
 * on the tape screen.
 * While it is on and the tape loop is on, every track whose clips end before the loop out point loops on its own
 * over [in, in + L), L = (end of its last clip - in) rounded to whole 4/4 bars (at least one). The tracks run freely:
 * they keep their phase across the global loop wrap, so a 3-bar track drifts against a 4-bar loop. A track whose
 * clips reach the out point (or that is empty) plays as usual. A locate (jump) restarts all tracks in phase.
 *
 * How: the L1 tape tick (0xFFA067A8) gets each block's samples for all four tracks from one chunk-cache entry: the
 * lookup 0x01A54D5C returns the entry of the chunk (32768 samples) holding the position, track t at data + t * 64 KB.
 * The lookup's entry is hooked (hook.S _tl_lookup_entry): after the stock lookup (prefetch / pinning unchanged) it
 * returns a private entry, taken once from the cache's LRU end, whose track windows around the requested position are
 * copied from each track's own position. Only the window the tick can read this block is filled (speed dependent).
 * The track's loop seam is crossfaded with the material after its loop end (TL_XF samples) once it has wrapped.
 * Plain playback only (tape state 10): any other state (recording, punch, FF / REW) gets the stock entry, so the
 * private entry is never written or flushed to the tape file. */
#include "../op1_synth.h"
#ifndef OP1_HOST
#pragma GCC optimize ("Os")                  /* code space: little of this runs per sample */
#endif

typedef struct te_entry {                   /* tape_c chunk cache entry (24 x, 0x01A54168 init) */
    uint8_t loaded, invalid, dirty, pad;    /* +0 data read, +1 read cancelled, +2 has unsaved samples */
    uint16_t dirty_tracks;                  /* +4 */
    int16_t idx;                            /* +6 chunk (-1: none) */
    int16_t slot, pad2;                     /* +8 entry number */
    int32_t base;                           /* +0xC idx << 15 */
    int16_t *data;                          /* +0x10 4 tracks x 32768 samples */
    struct te_entry *next, *prev;           /* +0x14 towards the LRU end, +0x18 towards the MRU head */
} te_entry_t;

#define TL_SH      15
#define TL_CHUNK   (1 << TL_SH)
#define TL_NCHUNK  485
#define TL_XF      256                      /* loop seam crossfade, samples */
#define TL_WRAP    2048                     /* loop wrap: at most this much travel across it between lookups */
#define TL_JUMP    4096                     /* a bigger step between lookups is a locate */
#define TL_WMAX    720                      /* window half width cap (the tick reads <= 600 samples per block) */
#define TL_SPL     297                      /* stock loop wrap splice (0x129 samples each side of the wrap) */
#define TL_PRE     8192                     /* rewrite the splice buffers within this of the wrap */

#ifdef OP1_HOST
extern int32_t host_tape_state, host_tape_speed, host_tape_pos, host_loop_in, host_loop_out, host_loop_on, host_tl_beat;
extern te_entry_t **host_chunk_tab;
extern const int32_t (*host_clips)[3]; extern const uint8_t *host_clips_has;
te_entry_t *host_lookup(int32_t pos, int32_t *dir);
void host_request(int idx);
te_entry_t *host_pop_lru(void);
extern te_entry_t *host_tail;
extern int16_t *host_splice[4];
#define SPLICE(t)   host_splice[t]
#define TAPE_STATE  host_tape_state
#define TAPE_SPEED  host_tape_speed
#define TAPE_POS    host_tape_pos
#define LOOP_IN     host_loop_in
#define LOOP_OUT    host_loop_out
#define LOOP_ON     host_loop_on
#define LOOP_MOVED  0
#define LOOP_MOVED_IN 0
#define CHUNK_TAB   host_chunk_tab
#define CACHE_TAIL  host_tail
#define CLIP(i)     host_clips[i]
#define CLIP_HAS(i) host_clips_has[i]
#define BEAT_LEN()  host_tl_beat
#define ORIG(p, d)  host_lookup(p, d)
#define REQUEST(i)  host_request(i)
#define POP_LRU()   host_pop_lru()
#define IRQ_OFF(s)  ((s) = 0)
#define IRQ_ON(s)   ((void)(s))
#else
#define TAPE_STATE  (*(volatile int32_t *)0x018C17D4)
#define TAPE_SPEED  (*(volatile int32_t *)0xFF802970)     /* 1x = 64 << 21, < 0: reverse */
#define TAPE_POS    (*(volatile int32_t *)0xFF802974)     /* 1/64 samples */
#define LOOP_IN     (*(volatile int32_t *)0x01B4EF34)     /* samples */
#define LOOP_OUT    (*(volatile int32_t *)0x01B4EF38)
#define LOOP_ON     (*(volatile uint8_t *)0xFF8029C5)
#define LOOP_MOVED  (*(volatile uint8_t *)0xFF8029C7)     /* Shift + left / right while looping: window moved */
#define LOOP_MOVED_IN (*(volatile int32_t *)0xFF8029D4)   /* .. its in point (same length) */
#define CHUNK_TAB   (*(te_entry_t ***)0x01B4EF48)          /* chunk -> cached entry or 0 */
#define CACHE_TAIL  (*(te_entry_t **)0x01B4EF40)           /* LRU end (head: 0x01B4EF3C) */
#define CLIP(i)     ((const int32_t *)0x01AA8D70 + 3 * (i))  /* db2::Clips record {track, start, stop} */
#define SPLICE(t)   (((int16_t **)0x01B4EF7C)[t])           /* loop_buffer[t]: 297 before out, 297 from in */
#define CLIP_HAS(i) (((const uint8_t *)0x01AAA570)[i])
#define BEAT_LEN()  (*(volatile int32_t *)(0x01AADABC + 0x1C) / 0x4000)   /* metronome beat (tape BPM), samples */
extern te_entry_t *tl_lookup_orig(int32_t pos, int32_t *dir);           /* hook.S: the stock lookup */
#define ORIG(p, d)  tl_lookup_orig(p, d)
#define REQUEST(i)  (((void (*)(int))0x01A54CD4)(i))        /* cache / queue a chunk, make it most recent */
#define POP_LRU()   (((te_entry_t *(*)(void))0x01A53E40)())  /* unlink the LRU end entry */
#define IRQ_OFF(s)  __asm__ volatile ("cli %0;" : "=d"(s))
#define IRQ_ON(s)   __asm__ volatile ("sti %0;" : : "d"(s))
#endif

volatile uint8_t tl_on;                     /* the setting */
volatile int32_t tl_len[4];                 /* main loop -> audio: loop length (samples), 0 = plays normally */
volatile uint8_t tl_bars[4];                /* the same in bars (screen) */
volatile int32_t tl_pos[4] = { -1, -1, -1, -1 };   /* audio -> screen: each looping track's position, -1 = none */
volatile uint8_t tl_sync_req;               /* restart all tracks in phase (toggle on) */
static int32_t tl_use[4];                   /* audio: the lengths in use */
static int32_t tl_shift[4];                 /* audio: track phase - global phase, 0..len-1 */
static uint8_t tl_cycled[4];                /* wrapped since the last sync: seam crossfade on */
static int32_t tl_prev = -1;                /* last looked-up position */
static te_entry_t *tl_ent;                  /* the private entry */
static uint8_t tl_spl_n;                    /* lookups since the last splice rewrite */

static inline int32_t tl_mod(int32_t a, int32_t m) { a %= m; return a < 0 ? a + m : a; }
static void tl_region(int32_t *in, int32_t *out)        /* the loop as the tape plays it */
{
    int32_t a = LOOP_IN, b = LOOP_OUT;
    if (LOOP_MOVED) { b = LOOP_MOVED_IN + (b - a); a = LOOP_MOVED_IN; }
    *in = a; *out = b;
}

static void tl_sync(void)
{
    for (int t = 0; t < 4; t++) { tl_shift[t] = 0; tl_cycled[t] = 0; }
}
static void tl_shift_all(int32_t d)        /* global loop wrap: the tracks keep running */
{
    for (int t = 0; t < 4; t++)
        if (tl_use[t]) { tl_shift[t] = tl_mod(tl_shift[t] + d, tl_use[t]); tl_cycled[t] = 1; }
}

/* follow the global position: loop wraps shift the tracks, locates resync them (audio thread) */
static void tl_track(int32_t pos, int32_t in, int32_t out)
{
    int32_t region = out - in;
    for (int t = 0; t < 4; t++) {
        int32_t L = tl_len[t];
        if (L <= 0 || L >= region) L = 0;
        if (L != tl_use[t]) { tl_use[t] = L; tl_shift[t] = L ? tl_mod(tl_shift[t], L) : 0; }
    }
    if (tl_sync_req) { tl_sync_req = 0; tl_sync(); tl_prev = pos; return; }
    if (tl_prev >= 0) {
        int32_t d = pos - tl_prev;
        int32_t fw = (out - tl_prev) + (pos - in), rv = (tl_prev - in) + (out - pos);   /* travel if it wrapped */
        if (d < 0 && fw >= 0 && fw <= TL_WRAP) tl_shift_all(region);
        else if (d > 0 && rv >= 0 && rv <= TL_WRAP) tl_shift_all(-region);
        else if (d > TL_JUMP || d < -TL_JUMP) tl_sync();
    }
    tl_prev = pos;
}

/* samples of track t at tape position m: pointer and how many follow in the same chunk (0 pointer: not cached) */
static const int16_t *tl_src(int t, int32_t m, int32_t *avail)
{
    *avail = TL_CHUNK - (m & (TL_CHUNK - 1));
    if (m < 0 || (m >> TL_SH) >= TL_NCHUNK) return 0;
    te_entry_t *e = CHUNK_TAB[m >> TL_SH];
    if (!e || e == tl_ent || !e->loaded || e->idx != (m >> TL_SH)) return 0;
    return e->data + t * TL_CHUNK + (m & (TL_CHUNK - 1));
}
static __attribute__((noinline)) int16_t tl_at(int t, int32_t m)
{
    int32_t n;
    const int16_t *s = tl_src(t, m, &n);
    return s ? *s : 0;
}

/* window [lo, hi) of track t into dst: from its own loop if L, else from the tape as is */
static void tl_fill(int t, int16_t *dst, int32_t lo, int32_t hi, int32_t in)
{
    int32_t L = tl_use[t];
    for (int32_t p = lo; p < hi; ) {
        int32_t run = hi - p, q = 0, m = p, n;
        if (L) {
            int32_t raw = p - in + tl_shift[t];
            if (raw >= L) tl_cycled[t] = 1;
            q = tl_mod(raw, L); m = in + q;
            if (run > L - q) run = L - q;
        }
        const int16_t *s = tl_src(t, m, &n);
        if (run > n) run = n;
        if (s) for (int32_t i = 0; i < run; i++) dst[i] = s[i];
        else for (int32_t i = 0; i < run; i++) dst[i] = 0;
        if (L && q < TL_XF && tl_cycled[t]) {              /* seam: fade in from the tail after the loop end */
            int32_t k = run < TL_XF - q ? run : TL_XF - q;
            for (int32_t i = 0; i < k; i++) {
                int32_t g = ((q + i) << 15) / TL_XF;
                dst[i] = (int16_t)((dst[i] * g + tl_at(t, in + L + q + i) * (32768 - g)) >> 15);
            }
        }
        dst += run; p += run;
    }
}

/* The tick plays the stock splice buffers instead of the chunks around the loop wrap (tracks in its mask 0x018C17F0;
 * built by 0x01A54284 from the raw tape). For looping tracks they are rewritten with the track's own audio across
 * the wrap: before it with the shift at out, after it with the shift at in (one region more when going forward). */
static int32_t tl_shift_out(int t, int32_t region, int fwd) { return fwd ? tl_shift[t] : tl_shift[t] - region; }
static void tl_splice(int32_t in, int32_t out, int fwd)
{
    int32_t region = out - in;
    for (int t = 0; t < 4; t++) {
        int32_t L = tl_use[t];
        int16_t *b = SPLICE(t);
        if (!L || !b) continue;
        int32_t so = tl_shift_out(t, region, fwd), si = so + region;
        for (int i = 0; i < TL_SPL; i++) {
            b[i] = tl_at(t, in + tl_mod(region - TL_SPL + i + so, L));
            b[TL_SPL + i] = tl_at(t, in + tl_mod(i + si, L));
        }
    }
}

/* the hooked lookup (L1 tape tick, audio thread): R0 position (samples), R1 &direction */
static __attribute__((noinline)) void tl_req(int32_t m) { REQUEST(m >> TL_SH); }
te_entry_t *tl_lookup(int32_t pos, int32_t *dir)
{
    te_entry_t *e = ORIG(pos, dir);
    int32_t in, out;
    tl_region(&in, &out);
    int32_t region = out - in;
    int any = 0;
    if (tl_on && LOOP_ON && tl_ent && TAPE_STATE == 10 && region >= 2 * TL_XF) {
        tl_track(pos, in, out);
        if (pos >= in && pos < out && (pos >> TL_SH) < TL_NCHUNK)
            for (int t = 0; t < 4; t++) any |= tl_use[t] != 0;
    }
    if (!any) {
        for (int t = 0; t < 4; t++) tl_pos[t] = -1;
        return e;
    }
    int fwd = TAPE_SPEED >= 0;
    int32_t sp = fwd ? TAPE_SPEED : -TAPE_SPEED;
    int32_t w = (sp >> 21) + 48;                           /* samples per block (64 at 1x) + filter taps + margin */
    if (w > TL_WMAX) w = TL_WMAX;
    int32_t near = fwd ? out - pos : pos - in;             /* to the loop wrap, in the direction of travel */
    for (int t = 0; t < 4; t++) {                          /* keep each track's chunks (and its tail's) cached */
        int32_t L = tl_use[t], m = -1;
        if (L) {
            m = in + tl_mod(pos - in + tl_shift[t], L);
            tl_req(m); tl_req(m + TL_CHUNK); tl_req(in + L);
            if (near < 3 * TL_CHUNK) {                     /* and the splice's positions, in time */
                int32_t so = tl_shift_out(t, region, fwd);
                tl_req(in + tl_mod(region - TL_SPL + so, L));
                tl_req(in + tl_mod(so + region, L));
            }
        }
        tl_pos[t] = m;
    }
    if (near < TL_PRE && near > TL_SPL + w && !(tl_spl_n++ & 15)) tl_splice(in, out, fwd);
    int32_t base = pos & ~(TL_CHUNK - 1);
    int32_t lo = pos - w < base ? base : pos - w, hi = pos + w > base + TL_CHUNK ? base + TL_CHUNK : pos + w;
    te_entry_t *s = tl_ent;
    s->idx = (int16_t)(pos >> TL_SH); s->base = base;
    s->loaded = 1; s->invalid = 0; s->dirty = 0; s->dirty_tracks = 0;
    for (int t = 0; t < 4; t++) tl_fill(t, s->data + t * TL_CHUNK + (lo - base), lo, hi, in);
    return s;
}

/* main loop: take the private entry once (the LRU end, when clean), loop lengths from the clips */
static void tl_take_entry(void)
{
    uint32_t irq;
    IRQ_OFF(irq);
    te_entry_t *e = CACHE_TAIL;
    if (e && !e->dirty && !e->dirty_tracks && e->prev) {
        e = POP_LRU();
        if (e->idx >= 0 && e->idx < TL_NCHUNK && CHUNK_TAB[e->idx] == e) CHUNK_TAB[e->idx] = 0;
        e->idx = -1; e->loaded = 0;
        tl_ent = e;
    }
    IRQ_ON(irq);
}

void tl_lengths(void)
{
    int32_t in, out;
    tl_region(&in, &out);
    int32_t region = out - in;
    int32_t bar = 4 * BEAT_LEN();
    if (bar <= 0) bar = 4 * 22050;                         /* 120 BPM */
    int32_t end[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 512; i++) {
        if (!CLIP_HAS(i)) continue;
        const int32_t *k = CLIP(i);
        int t = k[0];
        int32_t a = k[1] > in ? k[1] : in, b = k[2] < out ? k[2] : out;
        if (t < 0 || t > 3 || b <= a) continue;
        if (b > end[t]) end[t] = b;
    }
    for (int t = 0; t < 4; t++) {
        int32_t raw = end[t] - in, L = 0;
        if (raw > 0 && region > 0) {
            int32_t bars = (raw + bar / 2) / bar;
            if (bars < 1) bars = 1;
            L = bars * bar;
            if (L >= region) L = 0;                        /* reaches the out point: plays as usual */
        }
        tl_len[t] = L;
        tl_bars[t] = (uint8_t)(L ? L / bar : 0);
    }
}

static uint8_t tl_tick;
void tl_service(void)
{
    if (!tl_on) return;
    if (!tl_ent) tl_take_entry();
    if (!(tl_tick++ & 15)) tl_lengths();
}

void tl_toggle(void)
{
    tl_on ^= 1;
    if (tl_on) { tl_sync_req = 1; tl_tick = 0; }
}

#ifndef OP1_HOST
/* ---- screen: indicator + each looping track's playhead, drawn after the stock tape screen and the markers ---- */
extern int32_t tm_zoom_q10(void);
#define TL_CENTRE_X 163973                                  /* as markers.c */
static const uint8_t tl_col_on[4] = { 0x00, 0xED, 0x95, 0xFF }, tl_col_head[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static void tl_line(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
void tl_draw(void)
{
    if (!tl_on) return;
    void *r = op1_raster();
    for (int t = 0; t < 4; t++) {                          /* indicator: a row per track, 3 px per bar of its loop */
        int32_t y = OP1_Q10(3 + 3 * t), n = tl_bars[t] > 8 ? 8 : tl_bars[t];
        tl_line(r, OP1_Q10(204), y, OP1_Q10(204 + (n ? 3 * n : 1)), y, 0x500, n ? tl_col_on : tl_col_head);
    }
    int32_t c = TAPE_POS >> 6, z = tm_zoom_q10();
    int32_t k = (int32_t)(((int64_t)z << 16) / 44100);
    for (int t = 0; t < 4; t++) {
        int32_t p = tl_pos[t];
        if (p < 0) continue;
        int64_t x = TL_CENTRE_X + (((int64_t)(p - c) * k) >> 16);
        if (x < OP1_Q10(3) || x > OP1_Q10(317)) continue;
        int32_t y = OP1_Q10(124) + (OP1_Q10(22) * (2 * t + 1)) / 8;
        tl_line(r, (int32_t)x, y - OP1_Q10(2), (int32_t)x, y + OP1_Q10(2), 0x500, tl_col_head);
    }
}
#endif
