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

static struct { const uint8_t *clip; uint32_t t; } ts_saved[TS_N];
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

static uint32_t *ts_slot(const uint8_t *c)
{
    int free_i = 0;
    for (int i = 0; i < TS_N; i++) {
        if (ts_saved[i].clip == c) return &ts_saved[i].t;
        if (!ts_saved[i].clip) free_i = i;
    }
    ts_saved[free_i].clip = c;                                     /* 24 cache entries: a free slot always exists */
    ts_saved[free_i].t = NOW() - TS_HOLD;
    return &ts_saved[free_i].t;
}

int tape_service(int arg)
{
    int pos = 0;
    for (uint8_t *c = CLIP_HEAD; c; c = CLIP_NEXT(c), pos++) {
        int idx = *(volatile int16_t *)(c + 6);
        if (c[2]) {                                                /* dirty */
            uint32_t *t = ts_slot(c), now = NOW();
            if (!arg && TAPE_MODE == 1 && c[0] && pos < TS_DEEP && now - *t < TS_HOLD) continue;
            *t = now;
            c[2] = 0;
            volatile uint16_t *mask = (volatile uint16_t *)(c + 4);
            for (int k = 0; k < 4; k++) {
                if (!(*mask >> k & 1)) continue;
                *mask &= (uint16_t)~(1u << k);
                IO_T0();
                Y_LSEEK(TAPE_FD, TRACK_BASE[k] + idx * 0x10000, 0);
                tape_safe_write(TAPE_FD, CLIP_DATA(c) + k * 0x10000, 0x10000);
                IO_ADD(1);
            }
            return 1;
        }
        if (arg) continue;                                          /* flush: every dirty clip (stock stopped at the first clean one) */
        if (idx >= 0 && !c[0]) {                                    /* not loaded: load all 4 tracks */
            c[1] = 0;
            for (int k = 0; k < 4; k++) {
                IO_T0();
                Y_LSEEK(TAPE_FD, idx * 0x10000 + TRACK_BASE[k], 0);
                Y_READ(TAPE_FD, CLIP_DATA(c) + k * 0x10000, 0x10000);
                IO_ADD(0);
            }
            if (!c[1]) c[0] = *(volatile int16_t *)(c + 6) == idx;  /* invalidated meanwhile: stays unloaded */
            return 1;
        }
    }
    return 0;
}
