/* Delayed patch saves (firmware audit, docs/58). Stock: every knob detent on a synth / drum sound queues a
 * SaveSlotJob (0x019411B8(is_drum, slot) -> BgFileWorker -> `UPDATE synth_slots / drum_slots SET patch=...`):
 * emulator, 10 detents = ~15 op1.db transactions and ~1200 NAND pages (2.4 MB, 19 block erases).
 * Here the queue call only notes (kind, slot); the jobs are queued once nothing new came for BGS_QUIET, or at once on
 * a key press (mode change, disk mode, ...). The job saves the patch as it is then, so nothing is lost but the
 * intermediate states.
 * shortcut: switching the OP-1 off within BGS_QUIET after the last knob turn loses that change; hook the power-off
 * path if it shows up in practice. */
#include "op1_synth.h"

#define BGS_QUIET  600000000u                               /* cycles: 1.5 s at 400 MHz */

static volatile uint16_t bgs_pending;                       /* bit kind * 8 + slot */
static volatile uint32_t bgs_last;

#ifdef OP1_HOST
uint32_t bgs_host_now; int bgs_host_jobs[16], bgs_host_njobs;
#define NOW()           bgs_host_now
#define QUEUE(kind, s)  (bgs_host_jobs[bgs_host_njobs++ & 15] = (kind) * 8 + (s))
static uint32_t irq_off(void) { return 0; }
static void irq_on(uint32_t m) { (void)m; }
uint32_t bgs_host_gc_calls;
#define BG_GC(path)     (bgs_host_gc_calls++)
#else
#define BG_GC(path)     ((void (*)(int))0x01A9AC48)(path)  /* yaffs_do_background_gc */
extern void bgs_queue_stock(int is_drum, int slot);         /* hook.S: the stock queue call (LINK + 0x019411BC) */
#define NOW()           op1_cycles()
#define QUEUE(kind, s)  bgs_queue_stock((kind), (s))
static uint32_t irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m)); return m; }
static void irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m)); }
#endif

void bgs_queue_slot(int is_drum, int slot)                  /* replaces 0x019411B8 (hook.S) */
{
    if (slot < 0 || slot > 7) { QUEUE(is_drum & 0xFF, slot); return; }   /* unknown: stock behaviour */
    uint32_t m = irq_off();
    bgs_pending |= (uint16_t)(1u << ((is_drum & 1) * 8 + slot));
    bgs_last = NOW();
    irq_on(m);
}

void bgs_flush(void)                                        /* queue the waiting saves now */
{
    if (!bgs_pending) return;
    uint32_t m = irq_off();
    uint16_t p = bgs_pending;
    bgs_pending = 0;
    irq_on(m);
    for (int b = 0; b < 16; b++) if (p >> b & 1) QUEUE(b >> 3, b & 7);
}

void bgs_service(void)                                      /* main loop: after a quiet period */
{
    if (bgs_pending && NOW() - bgs_last >= BGS_QUIET) bgs_flush();
}

/* Idle yaffs background GC (docs/58). Stock: every main-loop pass with no work calls yaffs_do_background_gc
 * (~250/s); each call rescans every block (find_oldest_dirty_seq caches nothing when nothing is dirty) = 2.8 % CPU.
 * Replaces that CALL at 0x019F0C46: at most every BGS_GC_GAP. Writes still run their own GC when they need space. */
#define BGS_GC_GAP 40000000u                                /* cycles: 100 ms at 400 MHz */
static uint32_t bgs_gc_last;

void bgs_idle_gc(int path)
{
    uint32_t now = NOW();
    if (now - bgs_gc_last < BGS_GC_GAP) return;
    bgs_gc_last = now;
    BG_GC(path);
}
