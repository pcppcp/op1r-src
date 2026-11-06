/* l1pool.c's lease logic (kennel's API, docs/67): claims, the idle rule, state copies in and back, no takeover outside
 * the callback or under a thread's note_on, size checks. */
#define L1POOL_IMPL
#include "../l1pool.c"
#include <assert.h>
#include <stdio.h>

static void block(void) { l1_begin(); }                 /* a callback's start .. l1_end() */

int main(void)
{
    static uint32_t ha[64], hb[64];
    for (int i = 0; i < 64; i++) { ha[i] = 100 + i; hb[i] = 200 + i; }
    int fr;
    /* outside the callback: nobody takes a free partition */
    assert(!op1_l1_claim_state(OP1_L1_SYNTH, ha, 256, 1024, &fr) && !fr);
    /* A takes it: its state copied in, fresh */
    block();
    uint32_t *a = op1_l1_claim_state(OP1_L1_SYNTH, ha, 256, 1024, &fr);
    assert(a && fr && a[0] == 100 && a[63] == 163);
    a[0] = 999;                                             /* A works in L1 */
    assert(op1_l1_claim_state(OP1_L1_SYNTH, ha, 256, 1024, &fr) == a && !fr);
    /* B in the same block: A still holds it */
    assert(!op1_l1_claim_state(OP1_L1_SYNTH, hb, 256, 1024, &fr) && !fr);
    l1_end();
    /* A keeps it across blocks, and outside the callback (holder) */
    block(); assert(op1_l1_claim_state(OP1_L1_SYNTH, ha, 256, 1024, &fr) == a && !fr); l1_end();
    assert(op1_l1_claim_state(OP1_L1_SYNTH, ha, 256, 1024, &fr) == a);
    /* A idle for L1_IDLE blocks: B takes over, A's state goes home first */
    block(); l1_end(); block(); l1_end();
    block();
    uint32_t t = l1_thread_enter(); assert(t == 0);          /* inside the callback: no bracket */
    uint32_t *b = op1_l1_claim_state(OP1_L1_SYNTH, hb, 256, 1024, &fr);
    assert(b == a && fr && b[0] == 200 && ha[0] == 999 && ha[63] == 163);
    l1_end();
    /* A back while B renders: 0, A works on its home */
    block(); assert(op1_l1_claim_state(OP1_L1_SYNTH, hb, 256, 1024, &fr)); assert(!op1_l1_claim_state(OP1_L1_SYNTH, ha, 256, 1024, &fr)); l1_end();
    /* a thread's note_on in progress: no takeover even when the holder is idle */
    block(); l1_end(); block(); l1_end();
    t = l1_thread_enter(); assert(t == 1);
    block(); assert(!op1_l1_claim_state(OP1_L1_SYNTH, ha, 256, 1024, &fr)); l1_end();
    l1_thread_leave(t);
    block(); assert(op1_l1_claim_state(OP1_L1_SYNTH, ha, 256, 1024, &fr) && fr); l1_end();
    assert(hb[0] == 200);                                   /* B's state went home */
    /* sizes: over the partition, misaligned state, no room for FX / MASTER */
    block();
    assert(!op1_l1_claim(OP1_L1_DRUM, &fr, 513, &fr));
    assert(op1_l1_claim(OP1_L1_DRUM, &fr, 512, &fr) && fr);
    assert(!op1_l1_claim(OP1_L1_FX, &fr, 32, &fr) && !op1_l1_claim(OP1_L1_MASTER, &fr, 32, &fr));
    assert(!op1_l1_claim_state(OP1_L1_SYNTH, ha, 6, 1024, &fr));
    assert(!op1_l1_claim(OP1_L1_SYNTH, ha, 8193, &fr));
    /* the holder changes its state size: home and in again */
    uint32_t *c = op1_l1_claim_state(OP1_L1_SYNTH, ha, 128, 1024, &fr);
    assert(c && fr);
    l1_end();
    puts("l1pool ok");
    return 0;
}
