/* Host test of the free tape machine (tape/tm.c, docs/82): synthetic tape (track t, sample n = (t + 1) * 4000 +
 * (n & 1023)), R246's transport faked; checks 1x playback against the data, 2x, and a track looping half the global
 * loop on its own. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
uint8_t host_tm_xport[0x80]; int32_t host_tm_loop[2]; uint8_t host_tm_loopon;
int16_t host_tm_out[2][64];
void *host_tm_malloc(unsigned n) { return malloc(n); }
static int16_t val(int t, int32_t n) { return (int16_t)((t + 1) * 4000 + (n & 1023)); }
int host_tm_read(int t, int c, int16_t *dst) { for (int k = 0; k < 0x8000; k++) dst[k] = val(t, c * 0x8000 + k); return 1; }
#include "../tape/tm.c"

#define SET32(o, v) (*(int32_t *)(host_tm_xport + (o)) = (v))
#define GL(t) (((int16_t *)(host_tm_xport + 0x38))[t])
#define GR(t) (((int16_t *)(host_tm_xport + 0x40))[t])

static void run(int32_t start, int blocks, int pos_step)          /* pos_step: transport samples per block */
{
    for (int b = 0; b < blocks; b++) {
        SET32(0x14, (start + b * pos_step) << 6);
        tm_block();
        while (tm_service()) {}
    }
}

int main(void)
{
    tm_on = 1;
    SET32(0x00, 2); SET32(0x08, 0x08000000);
    memset(host_tm_xport + 0x38, 0, 16); GL(0) = 0x7FFF; GR(0) = 0x7FFF;
    tm_service();                                                 /* allocates */
    run(100000, 8, 64);                                           /* 1x, track 1: warms the cache */
    SET32(0x14, (100000 + 8 * 64) << 6); tm_block();
    /* the head is at 100000 + 9 * 64 - 64 after resync + 9 blocks; compare against the data around it */
    int32_t h = (int32_t)(head[0] >> 32) - 64;
    for (int i = 0; i < 64; i++) { int e = val(0, h + i) * 0x7FFF >> 15; assert(abs(host_tm_out[0][i] - e) <= 1); }
    /* 2x: track 2 alone */
    GL(0) = GR(0) = 0; GL(1) = GR(1) = 0x7FFF; tm_tr[1].speed = 0x20000;
    run(200000, 8, 64);                                           /* a locate: resync */
    SET32(0x14, (200000 + 8 * 64) << 6); tm_block();
    h = (int32_t)(head[1] >> 32) - 128;
    for (int i = 0; i < 64; i++) { int e = val(1, h + 2 * i) * 0x7FFF >> 15; assert(abs(host_tm_out[0][i] - e) <= 1); }
    /* loop: global [0, 8192), track 3 loops half of it from offset 1/4: stays in [2048, 6144) */
    GL(1) = GR(1) = 0; GL(2) = GR(2) = 0x7FFF; tm_tr[1].speed = 0x10000;
    host_tm_loop[0] = 0; host_tm_loop[1] = 8192; host_tm_loopon = 1; tm_tr[2].loop = 0x8000; tm_tr[2].off = 0x4000;
    for (int b = 0; b < 300; b++) {                               /* the transport loops too (wraps are no locate) */
        SET32(0x14, ((b * 64) % 8192) << 6);
        tm_block(); while (tm_service()) {}
        const int32_t n = (int32_t)(head[2] >> 32);
        assert(n >= 2048 && n < 2048 + 4097);
    }
    /* a recording (mode 1) in between: the next play reloads what we hold (stale chunks) */
    host_tm_loopon = 0; tm_tr[2].loop = 0xFFFF; tm_tr[2].off = 0;
    run(300000, 4, 64);
    const uint32_t before = tm_loads;
    SET32(0x00, 1); tm_block(); SET32(0x00, 2);
    run(300000 + 5 * 64, 2, 64);
    assert(tm_loads > before);
    printf("tm ok (loads %u, misses %u)\n", tm_loads, tm_miss);
    return 0;
}
