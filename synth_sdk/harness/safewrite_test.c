/* Host test for tape/safewrite.c: same bytes, pieces of whole chunks, never the caller's buffer.
 *   cc -DOP1_HOST safewrite_test.c ../tape/safewrite.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
int tape_safe_write(int fd, const void *src, unsigned n);
int tape_service(int arg);
extern volatile uint32_t tape_save_failures;
static uint8_t out[65536]; static unsigned pos, calls; static const void *caller;
static int seek_off, reads, writes_at[64], nwrites;
static int tbytes[4][128];                                /* bytes written per track / clip index (incremental test) */
static int fail_track = -1, fail_all, short_once;       /* write errors: one track's area, every write, one short write */
int host_yaffs_write(int fd, const void *buf, unsigned n)
{
    assert(fd == 7 && buf != caller && n <= 4096 && n % 2048 == 0);
    if (!caller && (fail_all || (seek_off >> 24) == fail_track)) return -1;   /* e.g. YAFFS full / I/O error */
    if (!caller && short_once) { short_once = 0; tbytes[seek_off >> 24][(seek_off >> 16) & 127] += 2048; seek_off += 2048; calls++; return 2048; }
    if (caller) { memcpy(out + pos, buf, n); pos += n; }
    else { if (seek_off % 65536 == 0 && nwrites < 64) writes_at[nwrites++] = seek_off; tbytes[seek_off >> 24][(seek_off >> 16) & 127] += (int)n; seek_off += n; }
    calls++;
    return (int)n;
}
int host_yaffs_lseek(int fd, int off, int whence) { assert(fd == 7 && whence == 0); seek_off = off; return off; }
int host_yaffs_read(int fd, void *buf, unsigned n) { assert(fd == 7 && n == 65536); memset(buf, 0x11, n); reads++; return (int)n; }
uint8_t *host_clip_head; int host_tape_fd = 7, host_track_base[4] = {0, 0x1000000, 0x2000000, 0x3000000}, host_tape_mode;
uint32_t host_now;

/* clip entry as in tape_c: +0 loaded, +1 invalidated, +2 dirty, +4 track mask, +6 clip index, +0x10 data, +0x14 next */
static _Alignas(8) uint8_t ent[24][0x20]; static uint8_t *cdata[24]; static int nent;
uint8_t *host_clip_data(const uint8_t *c) { return cdata[(c - ent[0]) / 0x20]; }
uint8_t *host_clip_next(const uint8_t *c) { int i = (int)((c - ent[0]) / 0x20) + 1; return i < nent ? ent[i] : NULL; }
static void list(int n)
{
    for (int i = 0; i < n; i++) {
        memset(ent[i], 0, 0x20); ent[i][0] = 1; *(int16_t *)(ent[i] + 6) = (int16_t)(100 + i);
        if (!cdata[i]) cdata[i] = calloc(4, 65536);
        nent = n;
    }
    host_clip_head = ent[0];
}
static void dirty(int i, int track) { ent[i][2] = 1; *(uint16_t *)(ent[i] + 4) |= (uint16_t)(1u << track); }
static void service_test(void)
{
    caller = NULL;
    list(24);
    host_tape_mode = 1; host_now = 1000;
    dirty(0, 1);                                         /* recording into clip 0, track 2 */
    nwrites = 0;
    assert(tape_service(0) == 1 && nwrites == 1 && writes_at[0] == 0x1000000 + 100 * 65536);
    assert(ent[0][2] == 0 && *(uint16_t *)(ent[0] + 4) == 0);
    for (int b = 0; b < 500; b++) {                     /* 0.5 s of audio blocks: re-dirtied, held */
        dirty(0, 1); host_now += 400000;
        assert(tape_service(0) == 0);
    }
    assert(nwrites == 1 && ent[0][2] == 1);
    dirty(1, 1);                                         /* previous clip: saved although clip 0 is held */
    assert(tape_service(0) == 1 && nwrites == 2 && writes_at[1] == 0x1000000 + 101 * 65536 && ent[1][2] == 0);
    host_now += 600000000;                               /* hold over: saved again */
    assert(tape_service(0) == 1 && nwrites == 3);
    dirty(0, 1); assert(tape_service(0) == 0);           /* held again */
    host_tape_mode = 2;                                  /* recording stopped: saved at once */
    assert(tape_service(0) == 1 && nwrites == 4 && ent[0][2] == 0);
    host_tape_mode = 1;
    dirty(20, 3); host_now += 1;                         /* deep in the MRU list: never held (eviction) */
    assert(tape_service(0) == 1 && tape_service(0) == 0);
    dirty(20, 3); assert(tape_service(0) == 1);
    dirty(0, 1); dirty(5, 0);                            /* shutdown flush: every dirty clip, even held ones */
    int n = 0; while (tape_service(1)) n++;
    assert(n == 2 && !ent[0][2] && !ent[5][2]);
    ent[3][0] = 0; reads = 0;                           /* not loaded: load 4 tracks, loaded again */
    assert(tape_service(0) == 1 && reads == 4 && ent[3][0] == 1 && cdata[3][3 * 65536] == 0x11);
    ent[4][0] = 0; ent[4][1] = 0; *(int16_t *)(ent[4] + 6) = -1;   /* unused entry: left alone */
    assert(tape_service(0) == 0);
}
/* a failed save keeps the clip dirty and is tried again (before: dirty cleared first, results ignored -> take lost) */
static int track_writes(int clip, int k)                 /* 64 KB writes of clip index `clip`, track k, in writes_at */
{
    int n = 0;
    for (int i = 0; i < nwrites; i++) n += writes_at[i] == host_track_base[k] + clip * 65536;
    return n;
}
static void failure_test(void)
{
    caller = NULL;
    list(24);
    host_tape_mode = 2; host_now = 5000000;              /* tape stopped: a dirty clip is saved at once */
    /* 1: every write fails: still dirty, counted; retried only after the hold, then saved */
    dirty(3, 1); nwrites = 0; fail_all = 1; uint32_t f0 = tape_save_failures;
    assert(tape_service(0) == 1);
    assert(ent[3][2] == 1 && *(uint16_t *)(ent[3] + 4) == 2 && tape_save_failures == f0 + 1);
    fail_all = 0;
    assert(tape_service(0) == 0 && nwrites == 0);        /* backing off: the flash is not hammered */
    host_now += 400000001u;
    assert(tape_service(0) == 1 && track_writes(103, 1) == 1 && ent[3][2] == 0 && *(uint16_t *)(ent[3] + 4) == 0);
    assert(tape_service(0) == 0);
    /* 2: a short write is a failed save */
    dirty(4, 0); nwrites = 0; short_once = 1;
    assert(tape_service(0) == 1 && ent[4][2] == 1 && *(uint16_t *)(ent[4] + 4) == 1);
    host_now += 400000001u;
    assert(tape_service(0) == 1 && ent[4][2] == 0 && track_writes(104, 0) == 1);
    /* 3: all four tracks dirty, track 2 fails: 1, 3 and 4 are written now, only track 2 stays dirty */
    for (int k = 0; k < 4; k++) dirty(6, k);
    nwrites = 0; fail_track = 1;
    assert(tape_service(0) == 1);
    assert(track_writes(106, 0) == 1 && track_writes(106, 2) == 1 && track_writes(106, 3) == 1 && track_writes(106, 1) == 0);
    assert(ent[6][2] == 1 && *(uint16_t *)(ent[6] + 4) == 2);
    fail_track = -1; host_now += 400000001u; nwrites = 0;
    assert(tape_service(0) == 1 && nwrites == 1 && track_writes(106, 1) == 1 && ent[6][2] == 0);
    /* 4: power-off flush with a flash that keeps failing ends (a few tries per clip), the others still saved */
    dirty(7, 0); dirty(8, 2); fail_track = 0; nwrites = 0;
    int n = 0; while (tape_service(1)) assert(++n < 50);
    assert(n == 4 && track_writes(108, 2) == 1 && ent[8][2] == 0 && ent[7][2] == 1);   /* 3 tries of clip 7 + clip 8 */
    fail_track = -1;
    host_now += 400000001u;                              /* the flash recovers before the power goes: saved */
    assert(tape_service(0) == 1 && track_writes(107, 0) == 1 && ent[7][2] == 0);
}
/* the device's incremental saves (4 KB a main-loop pass): the same guarantees as whole-track saves */
extern int host_incremental;
static int run0(void) { int n = 0; while (tape_service(0)) assert(++n < 400); return n; }   /* until idle */
static void incremental_test(void)
{
    caller = NULL; host_incremental = 1;
    list(24); host_tape_mode = 2; host_now = 3000000000u;
    memset(tbytes, 0, sizeof tbytes); uint32_t f0 = tape_save_failures;
    /* all fine: 2 tracks in 32 pieces (+ the start), clean afterwards */
    dirty(3, 0); dirty(3, 2);
    assert(run0() == 33 && tbytes[0][103] == 65536 && tbytes[2][103] == 65536 && !ent[3][2] && !*(uint16_t *)(ent[3] + 4));
    /* 4 tracks, track 2 fails: 1, 3, 4 complete, track 2 stays dirty and counted; backing off, then saved */
    memset(tbytes, 0, sizeof tbytes);
    for (int k = 0; k < 4; k++) dirty(5, k);
    fail_track = 1; run0();
    assert(tbytes[0][105] == 65536 && tbytes[1][105] == 0 && tbytes[2][105] == 65536 && tbytes[3][105] == 65536);
    assert(ent[5][2] == 1 && *(uint16_t *)(ent[5] + 4) == 2 && tape_save_failures == f0 + 1);
    fail_track = -1;
    assert(run0() == 0);                                 /* backing off */
    host_now += 400000001u;
    assert(run0() == 17 && tbytes[1][105] == 65536 && !ent[5][2]);
    /* a short write in the middle of a track: that track again later, complete */
    memset(tbytes, 0, sizeof tbytes);
    dirty(6, 3); assert(tape_service(0) == 1);            /* start */
    for (int i = 0; i < 5; i++) assert(tape_service(0) == 1);
    short_once = 1; run0();
    assert(ent[6][2] == 1 && *(uint16_t *)(ent[6] + 4) == 8 && tape_save_failures == f0 + 2);
    host_now += 400000001u; memset(tbytes, 0, sizeof tbytes);
    assert(run0() == 17 && tbytes[3][106] == 65536 && !ent[6][2]);
    /* the entry reassigned mid-save (eviction: another clip index, unloaded, dirty flag dropped): the old clip's buffer
     * is not reused before tape_service loads the new clip, so the rest is written from it at once, to the old index
     * (before: the rest was dropped, nothing re-dirtied it -> the take went back to the old audio after a reboot) */
    memset(tbytes, 0, sizeof tbytes);
    dirty(7, 0); assert(tape_service(0) == 1 && tape_service(0) == 1);
    *(int16_t *)(ent[7] + 6) = 200; ent[7][0] = 0; ent[7][1] = 1;
    reads = 0;
    assert(tape_service(0) == 1 && tbytes[0][107] == 65536 && tbytes[0][200 & 127] == 0 && reads == 0);
    assert(tape_service(0) == 1 && reads == 4 && ent[7][0] == 1);   /* then the new clip is loaded */
    assert(run0() == 0 && tape_save_failures == f0 + 2);
    /* reassigned and already loaded, or another buffer: cannot be written any more, counted as a failed save */
    memset(tbytes, 0, sizeof tbytes);
    dirty(10, 0); assert(tape_service(0) == 1 && tape_service(0) == 1);
    *(int16_t *)(ent[10] + 6) = 201;                     /* (loaded: ent[10][0] still 1) */
    run0(); assert(tbytes[0][201 & 127] == 0 && tbytes[0][110] == 4096 && tape_save_failures == f0 + 3);
    *(int16_t *)(ent[10] + 6) = 110;
    dirty(10, 0); assert(tape_service(0) == 1 && tape_service(0) == 1);
    uint8_t *keep = cdata[10]; cdata[10] = calloc(4, 65536);
    run0(); assert(tape_save_failures == f0 + 4); free(cdata[10]); cdata[10] = keep;
    /* recording sets a bit during a save: kept (the snapshot cleared only what it saves) */
    dirty(8, 0); assert(tape_service(0) == 1);            /* started: bits taken */
    dirty(8, 1);                                         /* the audio interrupt, mid-save */
    host_now += 400000001u; run0();
    assert(!ent[8][2] && tbytes[1][108] == 65536);
    /* power-off flush during an incremental save that fails: the flush ends, the clip stays dirty */
    dirty(9, 0); assert(tape_service(0) == 1); assert(tape_service(0) == 1);
    fail_all = 1; int n = 0; while (tape_service(1)) assert(++n < 50);
    assert(ent[9][2] == 1 && (*(uint16_t *)(ent[9] + 4) & 1));
    fail_all = 0; host_incremental = 0;
}
/* eviction (0x01A53E40) takes the MRU tail and drops its dirty flag unsaved: clips near the tail are saved whole and
 * first; clip loads are not held up by a save (recording misses audio into an unloaded chunk) */
static void evict_test(void)
{
    caller = NULL; host_incremental = 1;
    list(24); host_tape_mode = 2; host_now = 100000000u;
    memset(tbytes, 0, sizeof tbytes); uint32_t f0 = tape_save_failures;
    dirty(15, 1);                                        /* deep: one pass */
    assert(tape_service(0) == 1 && tbytes[1][115] == 65536 && !ent[15][2]);
    dirty(2, 0); dirty(20, 0); dirty(13, 3);             /* the deepest first, whole; then 13; then 2 in pieces */
    assert(tape_service(0) == 1 && tbytes[0][120] == 65536 && tbytes[0][102] == 0 && tbytes[3][113] == 0);
    assert(tape_service(0) == 1 && tbytes[3][113] == 65536 && tbytes[0][102] == 0);
    assert(run0() == 17 && tbytes[0][102] == 65536);
    /* a backlog behind an incremental save: the rest of it at once, then the backlog */
    memset(tbytes, 0, sizeof tbytes);
    dirty(3, 0); dirty(3, 1); assert(tape_service(0) == 1 && tape_service(0) == 1 && tbytes[0][103] == 4096);
    dirty(22, 2);
    assert(tape_service(0) == 1 && tbytes[0][103] == 65536 && tbytes[1][103] == 65536 && !ent[3][2]);
    assert(tape_service(0) == 1 && tbytes[2][122] == 65536 && run0() == 0);
    /* a clip to load during a save: loaded first, the save goes on */
    memset(tbytes, 0, sizeof tbytes);
    dirty(4, 0); assert(tape_service(0) == 1 && tape_service(0) == 1);
    ent[5][0] = 0; reads = 0;
    assert(tape_service(0) == 1 && reads == 4 && ent[5][0] == 1 && tbytes[0][104] == 4096);
    assert(run0() == 15 && tbytes[0][104] == 65536 && tape_save_failures == f0);
    host_incremental = 0;
}
int main(void)
{
    static uint32_t clip[16384];
    for (int i = 0; i < 16384; i++) clip[i] = (uint32_t)i * 2654435761u;
    caller = clip;
    assert(tape_safe_write(7, clip, 65536) == 65536 && pos == 65536 && calls == 16);
    assert(!memcmp(out, clip, 65536));
    service_test();
    failure_test();
    incremental_test();
    evict_test();
    printf("safewrite_test: ok\n");
    return 0;
}
