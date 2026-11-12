/* Host test for tape/safewrite.c under a recording that moves through R246's 24-entry clip cache: no recorded audio
 * may be lost to eviction (0x01A53E40 takes the MRU tail and drops its dirty flag unsaved). Model: queueing a chunk
 * not in the cache takes the tail entry; a recording writes only into a loaded entry (else that audio is missed, as
 * R246 / tm_record do). Time: one main-loop pass = base + its I/O (64 KB write 80 ms, 64 KB read 8 ms).
 *   cc -DOP1_HOST tapeevict_test.c ../tape/safewrite.c */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define NE 24
#define NCH 485
static uint8_t ent[NE][0x20]; static uint8_t *cdata[NE]; static int order[NE];   /* order[0] = MRU */
static uint8_t *file[4];                                   /* tape_c.raw per track: NCH x 64 KB */
uint8_t *host_clip_head; int host_tape_fd = 7, host_track_base[4] = {0, 0x2000000, 0x4000000, 0x6000000}, host_tape_mode;
uint32_t host_now;
extern int host_incremental; int tape_service(int);
static int seek; static double io_ms;
int host_yaffs_lseek(int fd, int off, int whence) { (void)fd; (void)whence; seek = off; return off; }
int host_yaffs_write(int fd, const void *buf, unsigned n)
{
    (void)fd; memcpy(file[seek / 0x2000000] + seek % 0x2000000, buf, n); seek += (int)n; io_ms += 80.0 * n / 65536; return (int)n;
}
int host_yaffs_read(int fd, void *buf, unsigned n)
{
    (void)fd; memcpy(buf, file[seek / 0x2000000] + seek % 0x2000000, n); seek += (int)n; io_ms += 8.0 * n / 65536; return (int)n;
}
static int pos_of(const uint8_t *c) { int e = (int)((c - ent[0]) / 0x20); for (int i = 0; i < NE; i++) if (order[i] == e) return i; return -1; }
uint8_t *host_clip_data(const uint8_t *c) { return cdata[(c - ent[0]) / 0x20]; }
uint8_t *host_clip_next(const uint8_t *c) { int p = pos_of(c) + 1; return p < NE ? ent[order[p]] : NULL; }
static void to_front(int p) { int e = order[p]; memmove(order + 1, order, (size_t)p * sizeof(int)); order[0] = e; host_clip_head = ent[order[0]]; }
static int find(int idx) { for (int p = 0; p < NE; p++) if (*(int16_t *)(ent[order[p]] + 6) == idx) return p; return -1; }
static void queue(int idx)                                  /* R246 QUEUE_CLIP */
{
    if (idx < 0 || idx >= NCH) return;
    int p = find(idx);
    if (p >= 0) { to_front(p); return; }
    uint8_t *e = ent[order[NE - 1]];
    e[0] = 0; e[1] = 1; e[2] = 0; *(uint16_t *)(e + 4) = 0; *(int16_t *)(e + 6) = (int16_t)idx;   /* dirty dropped */
    to_front(NE - 1);
}
/* records `secs` at `speed` onto track 0 with `base` ms per main-loop pass; samples lost, passes longer than 50 ms */
static long run(int incr, double speed, double base, double secs, long *slow)
{
    host_incremental = incr;
    for (int t = 0; t < 4; t++) { free(file[t]); file[t] = calloc(NCH, 65536); }
    for (int i = 0; i < NE; i++) {
        free(cdata[i]); cdata[i] = calloc(4, 65536); order[i] = i;
        memset(ent[i], 0, 0x20); *(int16_t *)(ent[i] + 6) = (int16_t)(400 + i); ent[i][0] = 1;
    }
    host_clip_head = ent[order[0]];
    double now_ms = 0, pos = 0; const double total = secs * 44100 * speed;
    long recorded = 0; int last_c = -1; *slow = 0;
    host_tape_mode = 1;
    while (pos < total) {
        io_ms = 0;
        tape_service(0);
        const double dt = base + io_ms;
        if (dt > 50) ++*slow;
        now_ms += dt; host_now = (uint32_t)(uint64_t)(now_ms * 400000.0);   /* CYCLES at 400 MHz, wraps */
        for (double a = 0; a < dt * 44.1 * speed && pos < total; a += 64 * speed) {   /* the audio blocks meanwhile */
            int s1 = (int)(pos + 64 * speed); if (s1 > total) s1 = (int)total;
            for (int s = (int)pos; s < s1; s++) {
                const int c = s >> 15;
                if (c != last_c) { last_c = c; queue(c); queue(c + 1); }
                const int p = find(c); uint8_t *e = p >= 0 ? ent[order[p]] : 0;
                if (e && e[0]) { ((int16_t *)cdata[order[p]])[s & 0x7FFF] = 0x5A5A; e[2] = 1; *(uint16_t *)(e + 4) |= 1; recorded++; }
            }
            pos += 64 * speed;
        }
    }
    host_tape_mode = 2;                                      /* stop: saved as normal, then the power-off flush */
    for (int i = 0; i < 2000 && tape_service(0); i++) {}
    while (tape_service(1)) {}
    long infile = 0;
    for (int s = 0; s < (int)total; s++) infile += ((int16_t *)file[0])[s] == 0x5A5A;
    return recorded - infile;
}
int main(void)
{
    static const double sc[][2] = { { 1, 10 }, { 1, 25 }, { 1, 40 }, { 2, 10 }, { 2, 25 }, { 2, 40 }, { 4, 10 }, { 4, 25 } };
    setvbuf(stdout, NULL, _IONBF, 0);
    for (unsigned i = 0; i < sizeof sc / sizeof sc[0]; i++) {
        long slow, slow0;
        const long lost = run(1, sc[i][0], sc[i][1], 60, &slow), lost0 = run(0, sc[i][0], sc[i][1], 60, &slow0);
        printf("tapeevict: %.0fx, %2.0f ms passes: lost %ld samples (whole tracks: %ld), passes > 50 ms %ld (whole tracks: %ld)\n",
               sc[i][0], sc[i][1], lost, lost0, slow, slow0);
        assert(lost == 0 && lost0 == 0);
        assert(slow <= slow0 && (sc[i][0] > 1 || sc[i][1] > 10 || !slow));   /* fewer long passes than whole tracks;
                                                                  * 1x, fast passes: 4 KB pieces only (keys stay responsive) */
    }
    printf("tapeevict_test: ok\n");
    return 0;
}
