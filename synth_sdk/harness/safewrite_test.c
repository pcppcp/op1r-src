/* Host test for tape/safewrite.c: same bytes, pieces of whole chunks, never the caller's buffer.
 *   cc -DOP1_HOST safewrite_test.c ../tape/safewrite.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
int tape_safe_write(int fd, const void *src, unsigned n);
int tape_service(int arg);
static uint8_t out[65536]; static unsigned pos, calls; static const void *caller;
static int seek_off, reads, writes_at[64], nwrites;
int host_yaffs_write(int fd, const void *buf, unsigned n)
{
    assert(fd == 7 && buf != caller && n <= 4096 && n % 2048 == 0);
    if (caller) { memcpy(out + pos, buf, n); pos += n; }
    else { if (seek_off % 65536 == 0 && nwrites < 64) writes_at[nwrites++] = seek_off; seek_off += n; }
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
int main(void)
{
    static uint32_t clip[16384];
    for (int i = 0; i < 16384; i++) clip[i] = (uint32_t)i * 2654435761u;
    caller = clip;
    assert(tape_safe_write(7, clip, 65536) == 65536 && pos == 65536 && calls == 16);
    assert(!memcmp(out, clip, 65536));
    service_test();
    printf("safewrite_test: ok\n");
    return 0;
}
