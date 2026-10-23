/* Shift + Drop on the tape screen: drop an all-tracks lift as ONE track. A clipboard taken from all four tracks
 * (Shift + Lift with loop points, clipboard command 3) holds one buffer per track, and the stock Drop puts every
 * track back on its own track. Here the four buffers are summed into one and the clipboard becomes a single-track
 * tape lift (the same state the sample editor leaves); the dispatcher then lets the stock Drop paste it on the
 * selected track at the playhead.
 *
 * All-tracks clipboard: table 0x02040000 = the four buffer pointers (int16 mono, n samples each, 0x1E47F4 bytes
 * apart, at most 992250 samples = 22.5 s); 0x01AA7100 = -1 (source channel), 0x01AA7104 = n, 0x01AA7108 = 1 (tape
 * audio), 0x01AA710C = -1 (track). Single-track form: channel 0 (data in buffer 0), track 0. A single-track
 * paste of channel -1 would hit the stock range assert, which is why stock Drop tests the track field first. */
#include <stdint.h>

#define MD_MAX 992250                               /* samples per track of an all-tracks lift (22.5 s) */

static int16_t md_sat(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* d = saturated sum of the four tracks (d may be t[0]) */
void tape_mix4(int16_t *d, const int16_t *const t[4], int32_t n)
{
    for (int32_t i = 0; i < n; i++) d[i] = md_sat((int32_t)t[0][i] + t[1][i] + t[2][i] + t[3][i]);
}

#ifndef OP1_HOST
#define MD_CH          (*(volatile int32_t *)0x01AA7100)
#define MD_N           (*(volatile int32_t *)0x01AA7104)
#define MD_KIND        (*(volatile int32_t *)0x01AA7108)
#define MD_TRACK       (*(volatile int32_t *)0x01AA710C)
#define MD_Q_HEAD      (*(volatile int32_t *)0x01AA7330)   /* clipboard queue idle: head == tail */
#define MD_Q_TAIL      (*(volatile int32_t *)0x01AA7334)
#define MD_TAB         ((const int32_t *)0x02040000)
#define MD_STRIDE      0x1E47F4                           /* bytes between the track buffers */

/* 1 = the clipboard held an all-tracks lift and is now the sum as a single-track lift; 0 = nothing changed */
int tape_mix_clip(void)
{
    int32_t n = MD_N;
    if (MD_TRACK != -1 || MD_CH != -1 || MD_KIND != 1 || n <= 0 || n > MD_MAX || MD_Q_HEAD != MD_Q_TAIL) return 0;
    const int16_t *t[4];
    for (int i = 0; i < 4; i++) {
        t[i] = (const int16_t *)MD_TAB[i];
        if ((int32_t)t[i] != MD_TAB[0] + i * MD_STRIDE) return 0;   /* not the layout of an all-tracks lift */
    }
    tape_mix4((int16_t *)t[0], t, n);
    MD_CH = 0;
    MD_TRACK = 0;
    return 1;
}
#endif
