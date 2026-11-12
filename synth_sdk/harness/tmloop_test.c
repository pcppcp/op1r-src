/* Host test (docs/82): a long per-track loop under device-like load (one chunk load per 24 blocks, the other tracks
 * streaming): after warm-up the looping track must not miss (the prefetch follows the loop; was the loop-end pop). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
uint8_t host_tm_xport[0x80]; int32_t host_tm_loop[2]; uint8_t host_tm_loopon;
int16_t host_tm_out[2][64];
void *host_tm_malloc(unsigned n) { return malloc(n); }
static int16_t val(int t, int32_t n) { (void)t; return (int16_t)(8000 * sin(n * 2 * M_PI / 97.3)); }
int host_tm_read(int t, int c, int16_t *dst) { for (int k = 0; k < 0x8000; k++) dst[k] = val(t, c * 0x8000 + k); return 1; }
#include "../tape/tm.c"
#define SET32(o, v) (*(int32_t *)(host_tm_xport + (o)) = (v))
int main(void)                                   /* device-like: a load needs 8 blocks, other tracks stream */
{
    tm_on = 1; SET32(0x00, 2); SET32(0x08, 0x08000000);
    for (int k = 0; k < 8; k++) tm_g[k] = 0x7FFF;
    tm_service();
    long pos = 100000;
    for (int b = 0; b < 60000; b++) {
        if (b == 50) { tm_tr[0].lin = 98304 + 32700; tm_tr[0].lout = 98304 + 20 * 32768 + 500; tm_tr[0].lon = 1; }
        SET32(0x14, (int32_t)(pos << 6)); pos += 64;
        if (b == 2000) tm_miss_t[0] = 0;
        tm_block(); if (b % 24 == 0) tm_service();
    }
    printf("track 0 misses %u (others %u)\n", tm_miss_t[0], tm_miss_t[1] + tm_miss_t[2] + tm_miss_t[3]);
    return tm_miss_t[0] > 0;
}
