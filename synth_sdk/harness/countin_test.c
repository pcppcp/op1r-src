/* Host test for tape/countin.c with a model of the stock metronome render (phase -0x100000 per block, wrap by the
 * beat length, click when on) and of the PLAY key (armed state 6 -> recording 7). */
#include "../op1_synth.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
uint8_t host_metro[0x40];
int32_t host_tape_state, host_tape_run, host_tape_speed, host_tape_pos;
static int clicks, plays, block;
static int click_half[64], click_block[64];
void pu_block(void) {}
void pu_service(void) {}
void mt_block(void) {}
void mt_service(void) {}
#include "../tape/countin.c"
void host_metro_render(void)
{
    int32_t step = M_TAPE ? (host_tape_speed >> 21) * 0x4000 : 0x100000;   /* as the stock render */
    if (M_PHASE - step < 0 && M_ON) { click_half[clicks] = M_HALF; click_block[clicks] = block; clicks++; M_ENV = M_START; }
    M_PHASE -= step;
    if (M_PHASE < 0) M_PHASE += *(int32_t *)(MB + 0x1C);
}
int host_tape_service(int a) { return a; }
static int restart_at = -1;                                         /* PLAY resets the phase a few blocks later */
static int moving;                                                  /* tape runs at 1x: pos and tape-synced phase move */
void host_dispatch_key(int code, int pressed) { if (code == 0x22 && pressed && host_tape_state == 6) { host_tape_state = 7; plays++; restart_at = block + 3; moving = 1; } }

static int32_t beat_len(int bpm) { return (int32_t)((int64_t)44100 * 60 / bpm / 64 * 0x100000); }   /* blocks per beat */
static void reset(int bpm, int on, int level)
{
    memset(host_metro, 0, sizeof host_metro); *(int32_t *)(MB + 0x1C) = beat_len(bpm);
    M_PHASE = beat_len(bpm) / 3; M_ON = on; M_LEVEL = (int16_t)level; M_HALF = 40; M_START = 0x7FFF;
    clicks = plays = block = 0; moving = 0; host_tape_state = 0; ci_on = ci_play_req = 0; acc_left = 0; bar_beat = 0; host_tape_run = 0; since_wrap = 0x7FFFFFFF;
    host_tape_pos = 0; bar_anchor = 0; host_tape_speed = 0;
}
static void run(int blocks)
{
    for (int i = 0; i < blocks; i++, block++) {
        if (block == restart_at) M_PHASE = 0;                      /* stock re-sync, not seen as a restart by us */
        host_tape_speed = moving ? 64 << 21 : 0;                   /* (speed >> 21) * 0x4000 = 0x100000 per block */
        ci_metro_render(); ci_service(0);
        if (moving) host_tape_pos += 64 * 64;
    }
}

int main(void)
{
    int bpms[] = { 40, 120, 220 }, ns[] = { 4, 8, 16 };
    for (int tape = 0; tape < 2; tape++)
    for (int b = 0; b < 3; b++) for (int k = 0; k < 3; k++) {
        reset(bpms[b], 0, 0); restart_at = -1; ci_beats = (uint8_t)ns[k];
        M_TAPE = (uint8_t)tape; host_tape_speed = 0;               /* tape-synced metronome, tape stopped (hardware) */
        host_tape_state = 3;                                       /* REC held */
        assert(ci_start() && host_tape_state == 6 && M_LEVEL == CI_LEVEL_MIN);
        int bl = (int)(44100L * 60 / bpms[b] / 64) + 1;
        run(bl * (ns[k] + 2));
        int pre = 0; for (int i = 0; i < clicks; i++) pre += click_block[i] < restart_at - 3;
        if (pre != ns[k]) fprintf(stderr, "bpm %d n %d: clicks %d plays %d\n", bpms[b], ns[k], pre, plays);
        assert(pre == ns[k]);                                      /* N clicks before the start */                                   /* N clicks, then recording */
        assert(plays == 1 && host_tape_state == 7);
        assert(M_ON == 0 && M_LEVEL == 0);                         /* stock settings back */
        for (int i = 1; i < pre; i++) assert(click_block[i] - click_block[i - 1] >= bl - 2 && click_block[i] - click_block[i - 1] <= bl + 1);
        for (int i = 0; i < pre; i++) assert(click_half[i] == (i % 4 == 0 ? 20 : 40));   /* beat 1 of each bar */
    }
    /* cancel: no recording, settings restored */
    reset(120, 1, 0x2000); restart_at = -1; host_tape_state = 3; assert(ci_start()); run(100); ci_cancel(); host_tape_state = 0; run(2000);
    assert(plays == 0 && M_ON == 1 && M_LEVEL == 0x2000);
    /* not armed by a held REC: nothing */
    reset(120, 0, 0x2000); host_tape_state = 0; assert(!ci_start()); host_tape_state = 2; assert(!ci_start());
    /* normal metronome: accent every 4th click, pitch restored */
    reset(120, 1, 0x2000); restart_at = -1; run(8000);
    int acc = 0; for (int i = 0; i < clicks; i++) acc += click_half[i] == 20;
    assert(clicks > 8 && acc >= clicks / 4 - 1 && acc <= clicks / 4 + 1 && M_HALF == 40);
    /* after the count with a tape-synced metronome on (hardware): beat 1 at the start, then 3 plain, then accent */
    reset(120, 1, 0x2000); M_TAPE = 1; host_tape_pos = 777 * 64; ci_beats = 4; host_tape_state = 3; restart_at = -1; assert(ci_start());
    run(60 * 80);
    int first = -1;
    for (int i = 0; i < clicks; i++) if (click_block[i] >= restart_at - 3) { first = i; break; }
    assert(first >= 0);
    /* from the start on: one click (the re-sync, accented; the count's start beat is silent), 3 plain, accent */
    assert(click_block[first] >= restart_at && click_half[first] == 20);
    int k = first + 1;                                              /* first beat after the start */
    assert(click_half[k] == 40 && click_half[k + 1] == 40 && click_half[k + 2] == 40 && click_half[k + 3] == 20);
    /* PLAY from stop, tape-synced metronome: accent on the tape's bar grid (anchor 0), whatever was counted before */
    reset(120, 1, 0x2000); M_TAPE = 1; restart_at = -1; bar_beat = 2;   /* stale counter */
    M_PHASE = 1; moving = 1; run(344 * 9);                          /* phase 1 at pos 0: a beat at pos 0 */
    for (int i = 0; i < clicks; i++) {
        int32_t idx = (int32_t)(((int64_t)click_block[i] * 64 + 344 * 32) / (344 * 64)); /* ~ beat index */
        assert(click_half[i] == ((idx & 3) == 0 ? 20 : 40));
    }
    printf("ok (%d metronome clicks, %d accented)\n", clicks, acc);
    return 0;
}
