/* Host test for bgsave.c: slot saves are collected while knobs turn, queued once after the quiet period or on a
 * flush, one job per (kind, slot). */
#include <assert.h>
#include <stdio.h>
#include "../bgsave.c"

int main(void)
{
    for (int i = 0; i < 10; i++) { bgs_host_now += 100000000u; bgs_queue_slot(0, 2); bgs_service(); }   /* 10 detents */
    assert(bgs_host_njobs == 0);
    bgs_host_now += BGS_QUIET - 1; bgs_service(); assert(bgs_host_njobs == 0);
    bgs_host_now += 1; bgs_service(); assert(bgs_host_njobs == 1 && bgs_host_jobs[0] == 2);   /* one job, synth 2 */
    bgs_service(); assert(bgs_host_njobs == 1);
    bgs_queue_slot(1, 7); bgs_queue_slot(0, 0); bgs_queue_slot(1, 7); bgs_flush();          /* key press: at once */
    assert(bgs_host_njobs == 3 && bgs_host_jobs[1] == 0 && bgs_host_jobs[2] == 15);
    bgs_host_now = 0xFFFFFF00u; bgs_queue_slot(0, 1); bgs_host_now += BGS_QUIET; bgs_service();   /* counter wraps */
    assert(bgs_host_njobs == 4 && bgs_host_jobs[3] == 1);
    bgs_queue_slot(0, 9); assert(bgs_host_njobs == 5 && bgs_host_jobs[4] == 9);              /* out of range: as stock */
    bgs_host_now = 5u * BGS_GC_GAP;                                                            /* idle GC: max 1 per gap */
    for (int i = 0; i < 250; i++) { bgs_idle_gc(0); bgs_host_now += BGS_GC_GAP / 25; }      /* 250 idle passes / 1 s */
    assert(bgs_host_gc_calls == 10);
    printf("ok\n");
    return 0;
}
