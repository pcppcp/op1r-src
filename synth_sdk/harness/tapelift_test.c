/* Host test: tape/safewrite.c's saves vs R246's clipboard jobs (lift 0x01A5485C, drop 0x01A549B8). A job reads /
 * writes the file directly, then 0x01A540C0 marks the clean cache entries of its range unloaded + invalidated (same
 * index) so they are read again. An incremental save clears the clip's track bits at its start, so its clip counts
 * as clean; before the fix it went on writing the old buffer over the job's result (ISSUES Z1).
 * The main loop is modelled as stock runs it: tape_service, then audio_clipboard::service (the job). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define NE 24
#define TSZ 0x800000                                     /* bytes per track in the fake file: clips 0..127 */
static _Alignas(8) uint8_t ent[NE][0x20]; static uint8_t *cdata[NE];
static uint8_t *file;
uint8_t *host_clip_head; int host_tape_fd = 7, host_track_base[4] = {0, TSZ, 2 * TSZ, 3 * TSZ}, host_tape_mode;
uint32_t host_now; extern int host_incremental, host_clipq_busy; extern volatile uint32_t tape_save_failures;
int tape_service(int);
static int seek, writes, redirty;
int host_yaffs_lseek(int fd, int off, int w) { assert(fd == 7 && w == 0 && off >= 0 && off < 4 * TSZ); seek = off; return off; }
int host_yaffs_write(int fd, const void *b, unsigned n)
{
    assert(fd == 7 && seek + (int)n <= 4 * TSZ);
    memcpy(file + seek, b, n); seek += (int)n; writes++;
    if (redirty) { ent[0][2] = 1; *(uint16_t *)(ent[0] + 4) |= 1; assert(writes < 1000); }   /* recording into clip 100
                                                                  * meanwhile (the save must end anyway) */
    return (int)n;
}
int host_yaffs_read(int fd, void *b, unsigned n) { assert(fd == 7 && seek + (int)n <= 4 * TSZ); memcpy(b, file + seek, n); seek += (int)n; return (int)n; }
uint8_t *host_clip_data(const uint8_t *c) { return cdata[(c - ent[0]) / 0x20]; }
uint8_t *host_clip_next(const uint8_t *c) { int e = (int)((c - ent[0]) / 0x20) + 1; return e < NE ? ent[e] : NULL; }

static int16_t *fsamp(int t, int clip) { return (int16_t *)(file + t * TSZ + clip * 65536); }
static int16_t *csamp(int e, int t) { return (int16_t *)(cdata[e] + t * 65536); }
static long count(const int16_t *p, int16_t v) { long n = 0; for (int s = 0; s < 32768; s++) n += p[s] == v; return n; }

static void setup(void)
{
    memset(file, 0, 4 * TSZ);
    for (int i = 0; i < NE; i++) {
        memset(ent[i], 0, 0x20); memset(cdata[i], 0, 4 * 65536);
        *(int16_t *)(ent[i] + 6) = (int16_t)(100 + i); ent[i][0] = 1;
    }
    host_clip_head = ent[0]; host_tape_mode = 2; host_clipq_busy = 0; redirty = 0;
    host_now += 800000000u;                              /* 2 s: no save holds from the case before */
}
static void record(int e, int t, int16_t v) { for (int s = 0; s < 32768; s++) csamp(e, t)[s] = v; ent[e][2] = 1; *(uint16_t *)(ent[e] + 4) |= (uint16_t)(1u << t); }

/* the job: 0 = lift track t, clips c0..c1 (file -> clipboard, zeros -> file), 1 = drop (clipboard -> file) */
static int16_t clipboard[4][32768];
static int job = -1, job_t, job_c0, job_c1;
static void queue(int kind, int t, int c0, int c1) { job = kind; job_t = t; job_c0 = c0; job_c1 = c1; host_clipq_busy = 1; }
static void run_job(void)
{
    for (int c = job_c0; c <= job_c1; c++) {
        if (job == 0) { memcpy(clipboard[c - job_c0], fsamp(job_t, c), 65536); memset(fsamp(job_t, c), 0, 65536); }
        else memcpy(fsamp(job_t, c), clipboard[c - job_c0], 65536);
    }
    for (int i = 0; i < NE; i++) {                       /* 0x01A540C0: clean entries of the range */
        const int idx = *(int16_t *)(ent[i] + 6);
        if (*(uint16_t *)(ent[i] + 4) == 0 && idx >= job_c0 && idx <= job_c1) { ent[i][0] = 0; ent[i][1] = 1; }
    }
    job = -1; host_clipq_busy = 0;
}
static int pass(void) { const int r = tape_service(0); if (job >= 0) run_job(); return r; }
static void settle(void) { for (int i = 0; i < 300; i++) pass(); }           /* the save, the job, the reload */

int main(void)
{
    host_incremental = 1;
    file = calloc(4, TSZ);
    for (int i = 0; i < NE; i++) cdata[i] = calloc(4, 65536);
    int fails = 0;

    /* 1. lift track 1 of clip 100 0 .. 30 passes after the recording stopped (0: not saved yet, 4 / 15: mid-save) */
    for (int k = 0; k <= 30; k++) {
        setup();
        record(0, 1, 0x5A5A);
        for (int p = 0; p < k; p++) pass();
        queue(0, 1, 100, 100);
        settle();
        const long cb = count(clipboard[0], 0x5A5A), back = count(fsamp(1, 100), 0x5A5A), cache = count(csamp(0, 1), 0x5A5A);
        if (cb != 32768 || back || cache) { printf("FAIL lift after %d passes: clipboard %ld/32768, file %ld, cache %ld (want 0, 0)\n", k, cb, back, cache); fails++; }
    }

    /* 2. a backlog: clip 100 half saved, clip 101 dirty behind it; lift both */
    setup();
    record(0, 0, 0x1111); record(1, 0, 0x2222);
    for (int p = 0; p < 6; p++) pass();
    queue(0, 0, 100, 101);
    settle();
    if (count(clipboard[0], 0x1111) != 32768 || count(clipboard[1], 0x2222) != 32768 ||
        count(fsamp(0, 100), 0x1111) || count(fsamp(0, 101), 0x2222)) { printf("FAIL backlog lift\n"); fails++; }

    /* 3. two tracks recorded, track 0 lifted mid-save: track 2 still reaches the file */
    setup();
    record(0, 0, 0x1111); record(0, 2, 0x3333);
    for (int p = 0; p < 3; p++) pass();
    queue(0, 0, 100, 100);
    settle();
    if (count(clipboard[0], 0x1111) != 32768 || count(fsamp(0, 100), 0x1111) || count(fsamp(2, 100), 0x3333) != 32768 ||
        count(csamp(0, 2), 0x3333) != 32768) { printf("FAIL other track of a lifted clip\n"); fails++; }

    /* 4. drop into a clip being saved: the dropped audio stays */
    setup();
    record(0, 0, 0x1111);
    for (int p = 0; p < 5; p++) pass();
    for (int s = 0; s < 32768; s++) clipboard[0][s] = 0x4444;
    queue(1, 0, 100, 100);
    settle();
    if (count(fsamp(0, 100), 0x4444) != 32768 || count(csamp(0, 0), 0x4444) != 32768) { printf("FAIL drop mid-save: file %ld cache %ld\n", count(fsamp(0, 100), 0x4444), count(csamp(0, 0), 0x4444)); fails++; }

    /* 5. a job that got in after tape_service's check (queued in the gap): the old buffer is not written over it */
    setup();
    record(0, 0, 0x1111);
    for (int p = 0; p < 4; p++) pass();
    const uint32_t f0 = tape_save_failures;
    job = 0; job_t = 0; job_c0 = job_c1 = 100; run_job();
    settle();
    if (count(fsamp(0, 100), 0x1111) || count(csamp(0, 0), 0x1111) || tape_save_failures != f0 + 1) {
        printf("FAIL job in the gap: file %ld cache %ld failures +%u\n", count(fsamp(0, 100), 0x1111), count(csamp(0, 0), 0x1111), tape_save_failures - f0); fails++;
    }

    /* 6. a job while recording into the clip being saved: the whole-clip saves end (each dirty clip once) */
    setup();
    host_tape_mode = 1;
    record(0, 0, 0x1111); record(1, 0, 0x2222);
    pass(); pass();
    redirty = 1; writes = 0;
    queue(0, 0, 101, 101);
    pass();
    if (writes > 3 * 16 || count(clipboard[0], 0x2222) != 32768) { printf("FAIL recording: %d writes, clipboard %ld\n", writes, count(clipboard[0], 0x2222)); fails++; }

    for (int i = 0; i < NE; i++) free(cdata[i]);
    free(file);
    if (fails) return 1;
    printf("tapelift: ok\n");
    return 0;
}
