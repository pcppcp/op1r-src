/* Host test of FREE recording (tape/tm.c tm_record) at the tape ends: R246's clip cache faked as every chunk loaded
 * (exactly TM_CHUNKS entries: ASan catches an index past it), its take range (RANGE_ADD: closed into a clip on stop)
 * and clip queue recorded. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
uint8_t host_tm_xport[0x80]; int32_t host_tm_loop[2]; uint8_t host_tm_loopon;
int16_t host_tm_out[2][64];
void *host_tm_malloc(unsigned n) { return malloc(n); }
int host_tm_read(int t, int c, int16_t *dst) { (void)t; (void)c; memset(dst, 0, 0x10000); return 1; }
static int16_t rec_level = 0x7FFF; static int32_t sel_track;
static int32_t take_lo, take_hi; static int q_lo, q_hi;
static uint8_t **clip_idx;
static void range_add(int32_t p) { if (p < take_lo) take_lo = p; if (p > take_hi) take_hi = p; }
static void range_rst(void) { take_lo = 0x7FFFFFFF; take_hi = -1; }
static void queue_clip(int c) { if (c < q_lo) q_lo = c; if (c > q_hi) q_hi = c; }
static void rec_stock(void *b) { (void)b; abort(); }
#define TM_REC_HOST
#define REC_LEVEL  rec_level
#define QUEUE_CLIP queue_clip
#define RANGE_ADD  range_add
#define RANGE_RST  range_rst
#define REC_STOCK  rec_stock
#define CLIP_IDX   clip_idx
#define SEL_TRACK  sel_track
#include "../tape/tm.c"

#define SET32(o, v) (*(int32_t *)(host_tm_xport + (o)) = (v))
#define END (TM_CHUNKS * CHUNK)
static int16_t bus[64];
static int16_t *trk(int c) { return (int16_t *)*(uint8_t **)(clip_idx[c] + 0x10); }   /* track 1 of chunk c */
static int written(int32_t a, int32_t b) { int n = 0; for (int32_t p = a; p < b; p++) n += trk(p >> 15)[p & (CHUNK - 1)] != 0; return n; }
static void clear(void) { for (int c = 0; c < TM_CHUNKS; c++) memset(trk(c), 0, 0x10000); }
/* record n blocks on track 1 from head h0 at speed sp (Q16), own loop [lin, lout) if lout */
static void rec(int32_t h0, int32_t sp, int32_t lin, int32_t lout, int n)
{
    clear(); q_lo = 0x7FFFFFFF; q_hi = -1;
    tm_tr[0].speed = sp; tm_tr[0].lin = lin; tm_tr[0].lout = lout; tm_tr[0].lon = lout > lin;
    SET32(0x00, 0); tm_record(bus);                     /* (stopped: the next recording starts a new take) */
    tm_set_head(0, h0);
    SET32(0x00, 1); SET32(0x14, 100 << 6); xlast = 100; tm_hold_heads = 1;   /* no resync (as after a bar jump) */
    for (int b = 0; b < n; b++) { tm_block(); tm_rec_entry(bus); while (tm_service()) {} }
}
int main(void)
{
    clip_idx = calloc(TM_CHUNKS, sizeof *clip_idx);
    for (int c = 0; c < TM_CHUNKS; c++) {
        uint8_t *e = calloc(1, 0x20); e[0] = 1; *(int16_t *)(e + 6) = (int16_t)c;
        *(uint8_t **)(e + 0x10) = calloc(4, 0x10000); clip_idx[c] = e;
    }
    for (int i = 0; i < 64; i++) bus[i] = 1000;
    tm_on = 1; tm_service();
    SET32(0x08, 0x08000000);                            /* 1x */
    /* 1: no own loop, 0.2 s before the end, 0.43 s recorded: the take ends at the end (before: the head wrapped to
     * 0:00 and the range became the whole track, [0, 6:00]) */
    rec(END - 9000, 0x10000, 0, 0, 300);
    assert(take_lo >= END - 9000 && take_hi == END - 1 && written(END - 9000, END) > 8900 && written(0, 20000) == 0);
    assert(q_lo >= 0 && q_hi < TM_CHUNKS);
    /* 2: reverse through 0:00, the same */
    rec(2000, -0x10000, 0, 0, 100);
    assert(take_lo == 0 && take_hi <= 2000 && written(0, 2001) > 1900 && written(END - 20000, END) == 0 && q_lo >= 0);
    /* 3: an own loop wraps as before (the take: the loop, to a block: first / last position of each block) */
    rec(1000000 + 15000, 0x10000, 1000000, 1020000, 400);
    assert(take_lo < 1000064 && take_hi >= 1020000 - 64 && written(1000000, 1020000) > 19990 && written(990000, 1000000) == 0);
    /* 4: a loop out past the last chunk (saved by older builds up to R246's 0xF2BFA0, or a clip loop): nothing indexed
     * or queued past the chunk table */
    rec(END - 2000, 0x10000, END - 88200, 0xF2BFA0, 100);
    assert(q_hi < TM_CHUNKS && written(END - 2000, END) > 1900);
    /* 5: heads stay on the tape (Shift + > past the last bar, saved heads, marker jumps) */
    tm_set_head(1, 0xF2BFA0); assert(tm_head_pos(1) == END);
    tm_set_head(1, -5); assert(tm_head_pos(1) == 0);
    printf("tmrec_test: ok\n");
    return 0;
}
