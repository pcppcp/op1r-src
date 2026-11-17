/* Host test for tape/punch.c: forward / reverse / varispeed travel, loop wraps both ways, starting between markers,
 * one in/out per arming, cancel, not playing, a key while the punch-in is made. Tape state 10 = playing, 5 = recording while playing. */
#include "../op1_synth.h"
#include <assert.h>
#include <stdio.h>
int32_t host_tape_state, host_tape_pos, host_loop_in, host_loop_out, host_loop_on;
static int clips_left = 1, kh_toggle;                  /* kh_toggle: a key toggles punch mode inside the clips check */
void pu_toggle(void);
int kh_clips(void) { if (kh_toggle) { kh_toggle = 0; pu_toggle(); } return clips_left; }
#define host_clips_left kh_clips()
static int set_modes;
void host_set_mode(void) { set_modes++; }
int32_t tm_pos[32]; int tm_n;
#include "../tape/punch.c"

static int rec_from = -1, rec_to = -1;                 /* tape positions (samples) where recording began / ended */
static int32_t pos_s(void) { return host_tape_pos >> 6; }
static void step(int32_t samples)                       /* one audio block of travel + a main-loop pass */
{
    int32_t p = pos_s() + samples;
    if (host_loop_on && samples > 0 && p > host_loop_out) p = host_loop_in + (p - host_loop_out);
    if (host_loop_on && samples < 0 && p < host_loop_in) p = host_loop_out - (host_loop_in - p);
    if (p < 0) p = 0;
    host_tape_pos = p << 6;
    int was = host_tape_state;
    pu_block(); pu_service();
    if (was == 10 && host_tape_state == 5) rec_from = pos_s();
    if (was == 5 && host_tape_state == 10) rec_to = pos_s();
}
static void setup(int32_t start, int loop)
{
    tm_n = 3; tm_pos[0] = 10000; tm_pos[1] = 20000; tm_pos[2] = 30000;
    host_tape_pos = start << 6; host_tape_state = 10; host_loop_on = loop; host_loop_in = 5000; host_loop_out = 40000;
    pu_mode = PU_OFF; pu_req = 0; pu_prev = -1; rec_from = rec_to = -1; set_modes = 0;
    pu_toggle(); assert(pu_mode == PU_ARM);
}
static void run(int32_t per_block, int blocks) { for (int i = 0; i < blocks; i++) step(per_block); }

int main(void)
{
    /* forward from between markers: the current region is skipped, in at 20000, out at 30000, then off */
    setup(15000, 0); run(64, 400);
    assert(rec_from >= 20000 && rec_from < 20064 && rec_to >= 30000 && rec_to < 30064 && pu_mode == PU_OFF);
    assert(host_tape_state == 10);
    /* reverse from 25000: in at 20000, out at 10000 */
    setup(25000, 0); run(-64, 400);
    assert(rec_from <= 20000 && rec_from > 20000 - 64 && rec_to <= 10000 && rec_to > 10000 - 64 && pu_mode == PU_OFF);
    /* varispeed 2.5x forward */
    setup(9000, 0); run(160, 200);
    assert(rec_from >= 10000 && rec_from < 10160 && rec_to >= 20000 && rec_to < 20160);
    /* loop wrap forward: from 35000 (after the last marker) through the wrap: in at 10000, out at 20000 */
    setup(35000, 1); run(64, 400);
    assert(rec_from >= 10000 && rec_from < 10064 && rec_to >= 20000 && rec_to < 20064);
    /* loop wrap forward with a marker right after the loop in: crossed inside the wrap block */
    tm_n = 3; setup(39990, 1); tm_pos[0] = 5010; run(64, 300);
    assert(rec_from >= 5000 && rec_from < 5100 && rec_to >= 20000 && rec_to < 20064);
    /* loop wrap in reverse: from 7000 down through the loop in, continues from 40000: in at 30000, out at 20000 */
    setup(7000, 1); run(-64, 400);
    assert(rec_from <= 30000 && rec_from > 30000 - 64 && rec_to <= 20000 && rec_to > 20000 - 64);
    /* a locate (jump without the loop): no crossing */
    setup(15000, 0); host_tape_pos = 35000 << 6; step(0); step(64); assert(pu_mode == PU_ARM && rec_from < 0);
    /* cancel (PLAY / STOP) and toggle off before the in marker */
    /* armed + playing: PLAY / STOP are stock, punch mode stays */
    setup(15000, 0); run(64, 10); assert(!pu_key(0x22) && !pu_key(0x23) && pu_mode == PU_ARM);
    /* armed + stopped: STOP disarms */
    setup(15000, 0); host_tape_state = 0; assert(!pu_key(0x23) && pu_mode == PU_OFF);
    /* punched in: PLAY 1 = punch mode off, still recording (no punch out at the next marker); PLAY 2 = punch out */
    setup(15000, 0); run(64, 100); assert(pu_mode == PU_REC && host_tape_state == 5);
    assert(pu_key(0x22) && pu_mode == PU_OFF && host_tape_state == 5);
    run(64, 300); assert(host_tape_state == 5);                     /* past the out marker: still recording */
    assert(pu_key(0x22) && host_tape_state == 10);
    assert(!pu_key(0x22));                                          /* then PLAY is stock again */
    /* punched in: STOP punches out first, then the stock STOP */
    setup(15000, 0); run(64, 100); assert(!pu_key(0x23) && host_tape_state == 10 && pu_mode == PU_OFF);
    /* after PLAY 1: STOP punches out too */
    setup(15000, 0); run(64, 100); pu_key(0x22); assert(!pu_key(0x23) && host_tape_state == 10);
    setup(15000, 0); pu_toggle(); run(64, 400); assert(rec_from < 0);
    /* toggling during a punch stops the recording */
    setup(15000, 0); run(64, 100); assert(host_tape_state == 5); pu_toggle(); assert(host_tape_state == 10 && pu_mode == PU_OFF);
    /* not playing (stopped / armed) at the in marker: no recording, punch mode off */
    setup(15000, 0); host_tape_state = 0; run(64, 100); assert(host_tape_state == 0 && pu_mode == PU_OFF);
    /* out of clips: no recording */
    setup(15000, 0); clips_left = 0; run(64, 100); assert(host_tape_state == 10 && pu_mode == PU_OFF); clips_left = 1;
    /* punch mode turned off (key thread) while the main loop makes the punch-in: no recording */
    setup(15000, 0); kh_toggle = 1; run(64, 400);
    assert(!kh_toggle && pu_mode == PU_OFF && host_tape_state == 10 && rec_from < 0);
    printf("ok\n");
    return 0;
}
