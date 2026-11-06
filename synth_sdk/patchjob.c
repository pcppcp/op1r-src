/* Live patch JSON vs. background patch jobs, and two R246 sampler-preset bugs (docs/66; after Hundekraft kennel 3.1.4,
 * kennel/kn_drum_synth.c and kennel/kn_sampler_guard.c, GPL-3.0-or-later + OP-1 permission).
 *
 * 1. R246's BgFileWorker runs jobs that serialize a live slot patch with jsoncpp: the slot save (0x01940574, job
 *    vtable 0x0190F4AC), two other patch jobs (0x0193F578 / 0x019407DC, vtables 0x0190F2F4 / 0x0190F3A4) and the type
 *    jobs (sp_type_job, dp_type_job). Higher-priority threads (OP_Main, Gfx, KeyHandler) can preempt such a job inside
 *    the writer; if they change that patch's JSON meanwhile, the writer walks freed nodes and the OP-1 crashes. The
 *    jobs are counted (pj_busy, vtable run slots: make_firmware.py SLOTS); our JSON writers on other threads wait for
 *    the count to drop (op1_patch_wait: VDK Sleep, bounded) - never from inside a job (the count would not drop).
 * 2. More sampler presets than R246's limit (42) assert at boot (patch.cpp:629, every boot): the loader's over-limit
 *    branch (0x01A05984) continues with the next row instead (hook.S _pj_sampler_over; the rows stay in the DB).
 * 3. Entering disk mode opens each sampler preset's .raw to test it (CALL yaffs_open at 0x0199DD7A) and never closes
 *    it: past ~94 presets yaffs runs out of handles and R246 stops ("missing source file"). The probe closes it. */
#include <stdint.h>

volatile uint32_t pj_busy;                                     /* patch jobs running (BgFileWorker only changes it) */
volatile uint32_t pj_skipped;                                  /* sampler presets skipped at boot (over the limit) */

#ifndef OP1_HOST
#define JOB(addr, job)  ((void (*)(void *))(addr))(job)
#define VDK_SLEEP(t)    ((void (*)(unsigned))0xFFA0BD2C)(t)    /* VDK::Sleep(ticks): threads only */
#define YAFFS_OPEN      ((int (*)(const char *, int, int))0x01A997F4)
#define YAFFS_CLOSE     ((int (*)(int))0x01A99908)
#else
extern void pj_host_job(uint32_t addr, void *job);
extern void pj_host_sleep(unsigned t);
#define JOB(addr, job)  pj_host_job(addr, job)
#define VDK_SLEEP(t)    pj_host_sleep(t)
#endif

void pj_begin(void) { pj_busy++; }
void pj_end(void) { if (pj_busy) pj_busy--; }
static void run(uint32_t addr, void *job) { pj_begin(); JOB(addr, job); pj_end(); }
void pj_slot_save_job(void *job) { run(0x01940574u, job); }
void pj_patch_job_a(void *job) { run(0x0193F578u, job); }
void pj_patch_job_b(void *job) { run(0x019407DCu, job); }

/* before changing a live patch's JSON from a thread above the BgFileWorker: wait while a patch job runs (at most
 * ~2 s, then go on as before). 1 = had to wait. Never call it from inside a patch job. */
int op1_patch_wait(void)
{
    int waited = 0;
    for (int i = 0; i < 200 && pj_busy; i++) { VDK_SLEEP(1); waited = 1; }
    return waited;
}

#ifndef OP1_HOST
int pj_probe_open(const char *path, int flags, int mode)        /* disk-mode export: the same open, closed again */
{
    int fd = YAFFS_OPEN(path, flags, mode);
    if (fd >= 0) YAFFS_CLOSE(fd);
    return fd;
}
#endif
