/* Tape clip save through a bounce buffer (docs/42). tape_c::service 0x01A54710 saves a dirty clip with
 * yaffs_write(fd, live clip data, 64 KB) while the audio interrupt may still record into it. YAFFS writes full chunks
 * straight from the caller's buffer and, on the first chunk of a fresh block, compares the read-back with that buffer;
 * a change in between fails the compare and retires a good block (shown as "bad" in FILE). Here every piece is copied
 * first, so the flash always matches what YAFFS compares. A copy taken mid-recording is fine: recording marks the
 * clip dirty again and it is saved again. Replaces the CALL yaffs_write at 0x01A54842. */
#include <stdint.h>

#define SW_PIECE 4096                            /* whole YAFFS chunks (2 KB pages), aligned like the 64 KB write */

#ifdef OP1_HOST
extern int host_yaffs_write(int fd, const void *buf, unsigned n);
extern int host_yaffs_lseek(int fd, int off, int whence);
extern int host_yaffs_read(int fd, void *buf, unsigned n);
extern uint8_t *host_clip_head; extern int host_tape_fd, host_track_base[4], host_tape_mode; extern uint32_t host_now;
#define Y_WRITE host_yaffs_write
#define Y_LSEEK host_yaffs_lseek
#define Y_READ  host_yaffs_read
#define CLIP_HEAD   host_clip_head
#define TAPE_FD     host_tape_fd
#define TRACK_BASE  host_track_base
#define TAPE_MODE   host_tape_mode
#define NOW()       host_now
extern uint8_t *host_clip_data(const uint8_t *c), *host_clip_next(const uint8_t *c);   /* 64-bit host pointers */
#define CLIP_DATA(c) host_clip_data(c)
#define CLIP_NEXT(c) host_clip_next(c)
#else
#define Y_WRITE ((int (*)(int, const void *, unsigned))0x01A99F84)
#define Y_LSEEK ((int (*)(int, int, int))0x01A9A0E4)
#define Y_READ  ((int (*)(int, void *, unsigned))0x01A99C90)
#define CLIP_HEAD   (*(uint8_t *volatile *)0x01B4EF3C)            /* clip cache, MRU list (24 entries) */
#define TAPE_FD     (*(volatile int *)0x01B4EF44)
#define TRACK_BASE  ((volatile int *)0x018C1810)                  /* file offset of each track */
#define TAPE_MODE   (*(volatile int32_t *)0xFF802960)             /* 1 = recording (docs/29) */
#define CLIP_DATA(c) (*(uint8_t **)((c) + 0x10))
#define CLIP_NEXT(c) (*(uint8_t **)((c) + 0x14))
static inline uint32_t NOW(void) { uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c)); return c; }
#endif
#ifdef OP1_HOST
static uint32_t irq_off(void) { return 0; }
static void irq_on(uint32_t m) { (void)m; }
int host_incremental;                                              /* host test: the device's 4 KB incremental saves */
#define INCREMENTAL host_incremental
#else
static uint32_t irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m) : : "memory"); return m; }
static void irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m) : "memory"); }
#define INCREMENTAL 1
#endif

static uint32_t sw_buf[SW_PIECE / 4];

int tape_safe_write(int fd, const void *src, unsigned n)
{
    const uint32_t *s = (const uint32_t *)src;       /* clip data: int16 pairs */
    unsigned done = 0;
    while (done < n) {
        unsigned k = n - done < SW_PIECE ? n - done : SW_PIECE;
        if (((uintptr_t)src & 3) == 0)
            for (unsigned i = 0; i < (k + 3) / 4; i++) sw_buf[i] = s[(done >> 2) + i];
        else                                         /* (not seen: misaligned loads would fault on Blackfin) */
            for (unsigned i = 0; i < k; i++) ((uint8_t *)sw_buf)[i] = ((const uint8_t *)src)[done + i];
        int r = Y_WRITE(fd, sw_buf, k);
        if (r < 0) return done ? (int)done : r;
        done += (unsigned)r;
        if ((unsigned)r < k) break;
    }
    return (int)done;
}

/* Replaces tape_c::service 0x01A54710 (main loop CALL 0x019F0A24 via ci_service, shutdown flush CALL 0x019F0BE2).
 * Stock: recording re-dirties the clip under the playhead every audio block, so the main loop rewrites that whole
 * clip (4 x 64 KB tracks' dirty ones) on every pass: ~80 clip saves/s, ~12500 NAND pages in 5 s of recording, and
 * the main loop (keys, clipboard, sequencer services, prefetch loads) is stuck in flash writes (docs/58).
 * Here, while recording, a clip saved less than TS_HOLD ago is skipped (the walk goes on to other dirty clips /
 * prefetch loads); once recording stops it is saved on the next pass. Clips deep in the MRU list are never held:
 * eviction (0x01A53E40) takes the tail and drops its dirty flag without saving.
 * shortcut: a power loss while recording loses up to TS_HOLD of audio (stock: one main-loop pass); the normal
 * power-off flush (arg != 0) saves every dirty clip. */
#define TS_HOLD  400000000u                                        /* cycles: 1 s at 400 MHz */
#define TS_DEEP  12                                                /* MRU positions that may be held (of 24) */
#define TS_N     24
#define TS_FLUSH_TRIES 3                                           /* shutdown flush: tries per clip, then given up */

/* Failed clip saves since boot (resource monitor, page 4). A failed save (YAFFS full, I/O error, short write) keeps
 * the clip dirty with the failed tracks' bits: it is tried again after TS_HOLD, so a full flash is not hammered on
 * every main-loop pass. Before, the dirty flag and track bits were cleared first and the write results ignored: the
 * take kept playing from the clip cache and silently went back to the old audio once the clip was evicted or the
 * OP-1 restarted. (op1r-src PR #45 by tencur; here also for the incremental saves.) */
volatile uint32_t tape_save_failures;

static struct { const uint8_t *clip; uint32_t t; uint8_t fails; } ts_saved[TS_N];
#ifdef TAPE_IO
/* OP1R_TAPEIO=1 (docs/82): time of each 64 KB track read / write (lseek + call), us: reads n, sum, max, writes n,
 * sum, max (resource monitor page 9) */
volatile uint32_t ts_io[6];
static inline void ts_io_add(int w, uint32_t c0) { uint32_t us = (NOW() - c0) / 400; volatile uint32_t *s = ts_io + 3 * w; s[0]++; s[1] += us; if (us > s[2]) s[2] = us; }
#define IO_T0() uint32_t io_c0 = NOW()
#define IO_ADD(w) ts_io_add(w, io_c0)
#else
#define IO_T0()
#define IO_ADD(w)
#endif

static int ts_find(const uint8_t *c)
{
    int free_i = 0;
    for (int i = 0; i < TS_N; i++) {
        if (ts_saved[i].clip == c) return i;
        if (!ts_saved[i].clip) free_i = i;
    }
    ts_saved[free_i].clip = c;                                     /* 24 cache entries: a free slot always exists */
    ts_saved[free_i].t = NOW() - TS_HOLD;
    ts_saved[free_i].fails = 0;
    return free_i;
}

/* n bytes of track k of clip idx at offset off: 1 = all of it written (seek ok, no short write) */
static int save_part(int k, int idx, const uint8_t *data, int off, int n)
{
    IO_T0();
    int ok = Y_LSEEK(TAPE_FD, TRACK_BASE[k] + idx * 0x10000 + off, 0) >= 0 &&
             tape_safe_write(TAPE_FD, data + k * 0x10000 + off, (unsigned)n) == n;
    IO_ADD(1);
    return ok;
}

/* end of a clip's save: failed tracks dirty again (only if the entry still holds that clip and buffer), back off */
static void save_done(uint8_t *c, int s, int idx, const uint8_t *data, uint16_t failed)
{
    if (!failed) { ts_saved[s].fails = 0; return; }
    tape_save_failures++;
    if (ts_saved[s].fails < 255) ts_saved[s].fails++;
    const uint32_t m = irq_off();
    if (*(volatile int16_t *)(c + 6) == idx && CLIP_DATA(c) == data) { *(volatile uint16_t *)(c + 4) |= failed; c[2] = 1; }
    irq_on(m);
}

static void load_clip(uint8_t *c, int idx)
{
    c[1] = 0;
    for (int k = 0; k < 4; k++) {
        IO_T0();
        Y_LSEEK(TAPE_FD, idx * 0x10000 + TRACK_BASE[k], 0);
        Y_READ(TAPE_FD, CLIP_DATA(c) + k * 0x10000, 0x10000);
        IO_ADD(0);
    }
#ifndef OP1_HOST
    { extern void tm_mask_cache(int, uint8_t *); tm_mask_cache(idx, CLIP_DATA(c)); }   /* only clips' audio (tm.c) */
#endif
    if (!c[1]) c[0] = *(volatile int16_t *)(c + 6) == idx;      /* invalidated meanwhile: stays unloaded */
}

int tape_service(int arg)
{
#if defined(WDOG) && !defined(OP1_HOST)
    if (arg) { extern void wd_off(void); wd_off(); }        /* power-off flush: the hang detector off (wdog.c) */
#endif
#ifndef OP1_HOST
    if (!arg) { extern void tfs_poll(void); tfs_poll(); }     /* free tape machine screen: setting, scrubbing (every pass) */
#endif
    /* incremental save (main loop): one 4 KB piece per pass instead of a whole 64 KB track (38 .. 136 ms on the device,
     * docs/82): keys / UI stay responsive while recording; clip loads go first (both caches). The dirty flag is cleared at the start
     * and eviction (0x01A53E40) drops a dirty flag unsaved, so the rest is written in one go when the clip is deep in
     * the MRU list (next to be evicted), when its entry was reassigned (unloaded: the buffer still holds this clip
     * until load_clip reads the new one, and only tape_service loads), and in the power-off flush (arg != 0). A buffer
     * reused already cannot be saved: counted as a failed save. A failed piece skips the rest of its track; at the end
     * the failed tracks are dirty again (save_done). */
    static uint8_t *sv_c; static const uint8_t *sv_data; static int sv_idx, sv_k, sv_off, sv_s; static uint16_t sv_mask, sv_failed;
    if (sv_c) {
        const int moved = *(volatile int16_t *)(sv_c + 6) != sv_idx;
        if (CLIP_DATA(sv_c) != sv_data || (moved && sv_c[0])) { tape_save_failures++; sv_c = 0; }
        else {
            int p = 0, deep = 0;                                    /* deep: ours, or a backlog behind it */
            for (const uint8_t *c = CLIP_HEAD; c; c = CLIP_NEXT(c), p++) if (p >= TS_DEEP && (c == sv_c || c[2])) deep = 1;
            const int whole = arg || moved || deep;
            if (!whole)                                             /* a clip to load (not ours: that buffer is being saved) */
                for (uint8_t *c = CLIP_HEAD; c; c = CLIP_NEXT(c)) {
                    const int idx = *(volatile int16_t *)(c + 6);
                    if (c != sv_c && idx >= 0 && !c[0] && CLIP_DATA(c) != sv_data) { load_clip(c, idx); return 1; }
                }
#ifndef OP1_HOST
            if (!whole) { extern int tm_service(void); if (tm_service()) return 1; }   /* and the free tape machine's */
#endif
            do {
                const int n = whole ? 0x10000 - sv_off : SW_PIECE;
                if (save_part(sv_k, sv_idx, sv_data, sv_off, n)) sv_off += n;
                else { sv_failed |= (uint16_t)(1u << sv_k); sv_off = 0x10000; }   /* the rest of this track: later */
                if (sv_off >= 0x10000) {
                    sv_off = 0; sv_mask &= (uint16_t)~(1u << sv_k);
                    while (sv_k < 4 && !(sv_mask >> sv_k & 1)) sv_k++;
                    if (sv_k >= 4) { save_done(sv_c, sv_s, sv_idx, sv_data, sv_failed); sv_c = 0; }
                }
            } while (whole && sv_c);
            return 1;
        }
    }
    uint8_t *c = 0; int pos = 0, cpos = 0, s = 0;
    for (uint8_t *e = CLIP_HEAD; e; e = CLIP_NEXT(e), pos++) {
        if (e[2]) {                                                /* dirty */
            const int i = ts_find(e);
            if (!arg && NOW() - ts_saved[i].t < TS_HOLD &&
                (ts_saved[i].fails || (TAPE_MODE == 1 && e[0] && pos < TS_DEEP))) continue;   /* held / backing off */
            if (arg && ts_saved[i].fails >= TS_FLUSH_TRIES) continue;   /* power-off: given up on this clip */
            if (!c || pos >= TS_DEEP) { c = e; cpos = pos; s = i; }   /* the first one, but the deepest first: a
                                                                  * backlog is saved before eviction takes it */
            continue;
        }
        if (arg || c) continue;                                     /* flush: every dirty clip (stock stopped at the first clean one);
                                                                     * loads after a clip to save wait (as stock) */
        const int idx = *(volatile int16_t *)(e + 6);
        if (idx >= 0 && !e[0]) { load_clip(e, idx); return 1; }    /* not loaded: load all 4 tracks */
    }
    if (c) {
        const int idx = *(volatile int16_t *)(c + 6);
        ts_saved[s].t = NOW();
        volatile uint16_t *mask = (volatile uint16_t *)(c + 4);
        const uint32_t m = irq_off();                              /* snapshot: the audio interrupt sets bits while recording */
        const uint16_t bits = (uint16_t)(*mask & 0xF);
        uint8_t *const data = CLIP_DATA(c);
        *mask &= (uint16_t)~0xFu; c[2] = 0;
        irq_on(m);
        if (!bits) return 1;                                       /* (dirty without a track bit: nothing to write) */
        if (!arg && INCREMENTAL && cpos < TS_DEEP) {               /* start an incremental save (above) */
            sv_mask = bits; sv_failed = 0; sv_c = c; sv_data = data; sv_idx = idx; sv_off = 0; sv_k = 0; sv_s = s;
            while (!(sv_mask >> sv_k & 1)) sv_k++;
            return 1;
        }
        uint16_t failed = 0;                                       /* whole tracks (flush, deep clips; host default) */
        for (int k = 0; k < 4; k++)
            if (bits >> k & 1 && !save_part(k, idx, data, 0, 0x10000)) failed |= (uint16_t)(1u << k);
        save_done(c, s, idx, data, failed);
        return 1;
    }
#ifndef OP1_HOST
    if (!arg) { extern int tm_service(void); return tm_service(); }   /* the free tape machine's chunks (tape/tm.c) */
#endif
    return 0;
}
