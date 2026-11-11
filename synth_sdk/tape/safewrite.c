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
#else
static uint32_t irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m) : : "memory"); return m; }
static void irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m) : "memory"); }
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

/* failed clip saves since boot (resource monitor, page 4). A failed save (YAFFS full, I/O error, short write) keeps
 * the clip dirty: it is tried again after TS_HOLD, so a full flash is not hammered on every main-loop pass. Before,
 * the dirty flag and track bits were cleared first and the write results ignored: the take kept playing from the
 * clip cache and silently went back to the old audio once the clip was evicted or the OP-1 restarted. */
volatile uint32_t tape_save_failures;

static struct { const uint8_t *clip; uint32_t t; uint8_t fails; } ts_saved[TS_N];

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

/* one track of a clip, 64 KB at its place in the tape file: 1 = all of it written */
static int save_track(int k, int idx, const uint8_t *data)
{
    const int off = TRACK_BASE[k] + idx * 0x10000;
    if (Y_LSEEK(TAPE_FD, off, 0) < 0) return 0;
    return tape_safe_write(TAPE_FD, data + k * 0x10000, 0x10000) == 0x10000;
}

int tape_service(int arg)
{
    int pos = 0;
    for (uint8_t *c = CLIP_HEAD; c; c = CLIP_NEXT(c), pos++) {
        int idx = *(volatile int16_t *)(c + 6);
        if (c[2]) {                                                /* dirty */
            const int s = ts_find(c);
            const uint32_t now = NOW();
            if (!arg && now - ts_saved[s].t < TS_HOLD &&
                (ts_saved[s].fails || (TAPE_MODE == 1 && c[0] && pos < TS_DEEP))) continue;   /* held / backing off */
            if (arg && ts_saved[s].fails >= TS_FLUSH_TRIES) continue;   /* power-off: given up on this clip */
            ts_saved[s].t = now;
            volatile uint16_t *mask = (volatile uint16_t *)(c + 4);
            uint32_t m = irq_off();                                /* the audio interrupt sets bits while recording */
            const uint16_t bits = *mask;
            uint8_t *const data = CLIP_DATA(c);
            *mask = 0; c[2] = 0;
            irq_on(m);
            uint16_t failed = 0;
            for (int k = 0; k < 4; k++)                            /* the snapshot: an entry changed meanwhile can't skip tracks */
                if (bits >> k & 1 && !save_track(k, idx, data)) failed |= (uint16_t)(1u << k);
            if (!failed) { ts_saved[s].fails = 0; return 1; }
            tape_save_failures++;
            if (ts_saved[s].fails < 255) ts_saved[s].fails++;
            m = irq_off();
            if (*(volatile int16_t *)(c + 6) == idx && CLIP_DATA(c) == data) { *mask |= failed; c[2] = 1; }   /* again later */
            irq_on(m);
            return 1;
        }
        if (arg) continue;                                          /* flush: every dirty clip (stock stopped at the first clean one) */
        if (idx >= 0 && !c[0]) {                                    /* not loaded: load all 4 tracks */
            c[1] = 0;
            for (int k = 0; k < 4; k++) {
                Y_LSEEK(TAPE_FD, idx * 0x10000 + TRACK_BASE[k], 0);
                Y_READ(TAPE_FD, CLIP_DATA(c) + k * 0x10000, 0x10000);
            }
            if (!c[1]) c[0] = *(volatile int16_t *)(c + 6) == idx;  /* invalidated meanwhile: stays unloaded */
            return 1;
        }
    }
    return 0;
}
