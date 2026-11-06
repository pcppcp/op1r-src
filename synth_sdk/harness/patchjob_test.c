/* Host test of patchjob.c (docs/66): the patch-job count around a job, op1_patch_wait sleeps while one runs, gives up
 * after its bound, never sleeps when idle. */
#include <stdint.h>
#include <stdio.h>
extern volatile uint32_t pj_busy;
void pj_slot_save_job(void *), pj_begin(void), pj_end(void);
int op1_patch_wait(void);
static int sleeps, busy_in_job, finish_after = -1;
void pj_host_job(uint32_t addr, void *job) { (void)addr; (void)job; busy_in_job = (int)pj_busy; }
void pj_host_sleep(unsigned t) { (void)t; sleeps++; if (finish_after >= 0 && sleeps >= finish_after) pj_busy = 0; }
int main(void)
{
    int fails = 0;
#define CHECK(c, m) do { if (!(c)) { fails++; printf("FAIL: %s\n", m); } } while (0)
    pj_slot_save_job(0);
    CHECK(busy_in_job == 1 && pj_busy == 0, "a job counts while it runs");
    sleeps = 0; CHECK(op1_patch_wait() == 0 && sleeps == 0, "idle: no wait");
    pj_begin(); finish_after = 3; sleeps = 0;
    CHECK(op1_patch_wait() == 1 && sleeps == 3, "busy: sleeps until the job ends");
    pj_begin(); finish_after = -1; sleeps = 0;
    CHECK(op1_patch_wait() == 1 && sleeps == 200, "a stuck job: gives up after its bound");
    pj_end(); pj_end(); CHECK(pj_busy == 0, "end never goes below 0");
    printf(fails ? "%d failed\n" : "patchjob: all ok\n", fails);
    return fails != 0;
}
