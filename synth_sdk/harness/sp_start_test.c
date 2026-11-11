/* sampler+ velocity / random start (sp_plus_velocity) with a shared preset's region: start after end must not move
 * the voice outside the region (the device crossfade then read before the buffer).
 * cc -O1 -I.. sp_start_test.c ../samplerplus/sp_plus.c ../samplerplus/sp_voice.c */
#include <assert.h>
#include <stdio.h>
#include "../samplerplus/sp_plus.h"

static int32_t start_pos(const int16_t *k, int vel_start, int rand_start, int vel)
{
    sp_extra x; sp_pre c; sp_extra_default(&x);
    x.vel_start = (uint8_t)vel_start; x.rand_start = (uint8_t)rand_start; x.xfade = 127; x.sustain = 127;
    sp_plus_prepare(&x, &c);
    sp_voice v; sp_xvoice s = { 0 };
    sp_plus_note_on(&v, &s, k, &x, &c);
    sp_plus_velocity(&v, &s, k, &x, vel);
    return v.pos;
}

int main(void)
{
    /* the fuzzer's minimal input: start word 13876 > end word 256, crossfade, velocity start 1, velocity 0 */
    int16_t k[8] = { 13876, 13616, 14130, 256, 0, 15360, 0, 16000 };
    int32_t a = sp_bound(k[0], 0), b = sp_bound(k[3], 0);
    assert(start_pos(k, 1, 0, 0) == a);
    for (int i = 0; i < 200; i++) assert(start_pos(k, 127, 127, i & 127) == a);
    k[4] = 20000;                                                  /* reverse: from the end word */
    for (int i = 0; i < 200; i++) assert(start_pos(k, 127, 127, i & 127) == b);
    /* a normal region: the offset still applies, capped at the region */
    int16_t n[8] = { 1000, 1000, 2000, 1010, 0, 0, 0, 16000 };
    int32_t na = sp_bound(n[0], 0), room = (sp_bound(n[3], 0) - na) >> 12;
    assert(start_pos(n, 1, 0, 0) == na + (64 << 12));
    assert(start_pos(n, 127, 0, 0) == na + (room << 12));
    printf("sp_start_test: ok\n");
    return 0;
}
