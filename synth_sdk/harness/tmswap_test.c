/* Host test of the free tape machine's chunk cache (tape/tm.c) against the audio interrupt (tm_block / tm_chunk run
 * from the ISR, tm_service from the main loop). The interrupt is modelled: TM_IRQ(k) marks where one may arrive; with
 * tm_irq_off it is held until tm_irq_on (as the device's cli / sti).
 * 1: a reread (clip edit, first play after a recording) reads the raw file and masks after: an audio block inside the
 *    read must keep playing the old (masked) audio, never the raw file (it did: 10-15 ms of erased audio a track).
 * 2: a slot published while the ISR reassigns it (LRU) must not be marked loaded for the new chunk with the old audio.
 * 3: an audio block during tm_alloc must not claim a slot the init loop then frees (map -> a free slot). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
uint8_t host_tm_xport[0x80]; int32_t host_tm_loop[2]; uint8_t host_tm_loopon;
int16_t host_tm_out[2][64];
void *host_tm_malloc(unsigned n) { return calloc(1, n); }

#define TM_IRQ_MODEL
static int irq_dis, irq_pend, arm, arm_skip;
static void isr(int k);
static void tm_irq_at(int k) { if (arm != k || arm_skip-- > 0) return; arm = 0; if (irq_dis) irq_pend = k; else isr(k); }
#define TM_IRQ(k) tm_irq_at(k)
static uint32_t tm_irq_off(void) { irq_dis = 1; return 0; }
static void tm_irq_on(uint32_t m) { (void)m; irq_dis = 0; if (irq_pend) { const int k = irq_pend; irq_pend = 0; isr(k); } }

static int16_t raw = 12000, masked;                  /* tape_c.raw: an erased take; after tm_mask_clips: masked */
static int preempt, peak, ids;                       /* ids: every sample of a chunk = 1000 * (t + 1) + c */
static int16_t id(int t, int c) { return (int16_t)(1000 * (t + 1) + c); }
static void isr_block(void);
int host_tm_read(int t, int c, int16_t *dst)
{
    if (ids) { for (int k = 0; k < 0x8000; k++) dst[k] = id(t, c); return 1; }
    for (int k = 0; k < 0x8000; k++) dst[k] = raw;   /* Y_READ */
    if (preempt) isr_block();                        /* an audio block before the mask */
    for (int k = 0; k < 0x8000; k++) dst[k] = masked;   /* tm_mask_clips */
    return 1;
}
#include "../tape/tm.c"

#define SET32(o, v) (*(int32_t *)(host_tm_xport + (o)) = (v))
static int32_t pos = 100000;
static int victim = -1;
static void isr_block(void)
{
    SET32(0x14, pos << 6); pos += 64; tm_block();
    for (int i = 0; i < 64; i++) { const int a = abs(host_tm_out[0][i]); if (a > peak) peak = a; }
}
static void isr(int k)
{
    if (k == 1) {                                    /* one block: the head moved on, the LRU slot is the one published */
        blk++;
        for (int i = 0; i < TM_SLOTS; i++) if (i != victim) slot[i].used = blk;
        tm_chunk(0, 400, 1);
    } else isr_block();                              /* k 2: playing while FREE is switched on */
}
static void consistent(void)                         /* map and slots agree; a loaded slot holds its chunk */
{
    for (int t = 0; t < 4; t++) for (int c = 0; c < TM_CHUNKS; c++) if (map[t][c]) {
        const tm_slot *s = &slot[map[t][c] - 1];
        assert(s->t == t && s->chunk == c && s->state);
    }
    if (ids) for (int i = 0; i < TM_SLOTS; i++) if (slot[i].state == 2) assert(slot[i].data[0] == id(slot[i].t, slot[i].chunk));
}

int main(void)
{
    tm_on = 1; SET32(0x00, 2); SET32(0x08, 0x08000000);
    for (int k = 0; k < 8; k++) tm_g[k] = 0;
    tm_g[0] = tm_g[4] = 0x7FFF;
    /* 3: tm_alloc with an audio block in its init loop (the 2nd slot) */
    arm = 2; arm_skip = 1;
    tm_service();
    assert(!arm);
    for (int b = 0; b < 4; b++) { isr_block(); consistent(); while (tm_service()) {} }
    consistent();
    /* 1: rereads with audio blocks inside; several rounds (the fast path must not keep a swapped-out buffer) */
    for (int b = 0; b < 20; b++) { isr_block(); while (tm_service()) {} }
    for (int r = 1; r <= 4; r++) {
        const int16_t old = masked;
        masked = (int16_t)(r * 1000);
        const uint32_t hit = tm_fast_hit;
        tm_refresh_all();
        peak = 0; preempt = 1;
        int n = 0; while (tm_service()) n++;
        preempt = 0;
        printf("tmswap_test: reread round %d: %d rereads, peak inside them %d (audio %d -> %d, raw %d)\n", r, n, peak, old, masked, raw);
        fflush(stdout);
        assert(n > 0 && tm_fast_hit > hit);
        assert(peak <= ((old > masked ? old : masked) * 0x7FFF >> 15) + 1);   /* old or new (masked) audio, never the raw file */
        isr_block();
        for (int i = 0; i < 64; i++) assert(abs(host_tm_out[0][i] - (masked * 0x7FFF >> 15)) <= 1);   /* then the new */
    }
    consistent();
    /* 2: every slot loaded (no free one: the LRU picks), then chunk 300 wanted and published while the ISR reassigns
     * its slot to chunk 400 */
    ids = 1; SET32(0x00, 0); SET32(0x08, 0);
    for (int c = 0; c < TM_SLOTS; c++) { blk++; tm_chunk(1, 200 + c, 1); }
    while (tm_service()) {}
    consistent();
    blk += 10;
    tm_chunk(0, 300, 1);
    victim = map[0][300] - 1;
    assert(victim >= 0 && slot[victim].state == 1);
    arm = 1;
    tm_service();
    assert(!arm && map[0][400] - 1 == victim);
    consistent();                                    /* chunk 400's slot is not "loaded" with chunk 300's audio */
    while (tm_service()) {}
    assert(slot[victim].state == 2 && slot[victim].chunk == 400);
    consistent();
    printf("tmswap_test: ok\n");
    return 0;
}
