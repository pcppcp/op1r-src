/* Host test of seq/plk.c (docs/87): recording turns over a clip, playback (linear, looped), overdub per lane, synced
 * vs free start, bases back at stop, CUT, a lane silent under another engine, length.
 *   cc -DOP1_HOST -fsanitize=address,undefined -I.. plk_test.c ../seq/plk.c */
#include "../seq/plk.h"
#include <stdio.h>
#include <string.h>

extern int16_t plk_host_words[2][2][8];
extern int32_t plk_host_type[2][2];
extern uint16_t plk_host_mask[2][2];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define E(w) plk_host_words[0][0][w]                     /* synth mode, engine words */
#define F(w) plk_host_words[0][1][w]                     /* fx words */

static void ticks(int n) { for (int i = 0; i < n; i++) plk_tick(0); }

int main(void)
{
    plk_host_mask[0][0] = 0x2F;                          /* engine words 0..3 and 5 */
    plk_host_mask[0][1] = 0x0F;
    plk_host_type[0][0] = 7 << 16 | 40; plk_host_type[0][1] = 3 << 16;
    CHECK(plk_lanes(0) == (0x1F | 0x0F << 7), "lanes %x", plk_lanes(0));
    plk_clip[0].len_i = 0; plk_clip[0].sync = 1;         /* 1/4 bar = 96 ticks, synced */
    E(0) = 1000; E(1) = 2000; F(2) = 50;

    /* synced: REC arms, the recording starts at the first step */
    plk_rec(0);
    CHECK(plk_run[0].state == PLK_ARMED, "armed");
    plk_start(0);
    CHECK(plk_run[0].state == PLK_REC && plk_run[0].pos == 0, "recording from the first step");
    ticks(10); E(0) = 3000;                              /* a turn at tick 10 */
    ticks(40); E(0) = 2000;                              /* and at 50 */
    ticks(46);                                           /* the pass ends at 96 */
    CHECK(plk_run[0].state == PLK_IDLE && plk_clip[0].lane[0].n == 5, "pass over: %d events (start, held, turn, held, turn)", plk_clip[0].lane[0].n);
    CHECK(plk_clip[0].lane[1].n == 0 && plk_clip[0].lane[9].n == 0, "untouched lanes empty");

    /* playback: the recorded steps, looped (tick-rate steps, not ramped) */
    ticks(5); CHECK(E(0) == 1000, "t 5: start value %d", E(0));
    ticks(10); CHECK(E(0) == 3000, "t 15: after the first turn %d", E(0));
    ticks(40); CHECK(E(0) == 2000, "t 55: after the second %d", E(0));
    CHECK(plk_clip[0].lane[0].n == 5, "own playback not recorded (%d events)", plk_clip[0].lane[0].n);

    /* linear between sparse points */
    plk_lane_t *ln = &plk_clip[0].lane[1];
    ln->n = 2; ln->type = plk_host_type[0][0]; ln->start = 0;
    ln->ev[0].t = 0; ln->ev[0].v = 0; ln->ev[1].t = 80; ln->ev[1].v = 8000;
    plk_start(0); ticks(41);
    CHECK(E(1) >= 3900 && E(1) <= 4100, "ramp halfway: %d", E(1));

    /* overdub: only the turned lane (FX 2) is replaced, engine word 0 keeps its curve */
    plk_rec(0); plk_start(0);
    ticks(20); F(2) = 90; ticks(76);
    CHECK(plk_clip[0].lane[9].n == 3 && plk_clip[0].lane[0].n == 5, "overdub per lane: fx2 %d, e0 %d", plk_clip[0].lane[9].n, plk_clip[0].lane[0].n);
    ticks(10); CHECK(F(2) == 50, "fx2 plays its start before the turn: %d", F(2));
    ticks(15); CHECK(F(2) == 90, "fx2 after the turn: %d", F(2));

    /* stop: the bases come back */
    E(0) = 4321;                                         /* (a turn while it plays) */
    plk_stop(0);
    CHECK(E(1) == 2000 && F(2) == 90, "bases back at stop (the knob positions): e1 %d fx2 %d", E(1), F(2));

    /* another engine on the slot: its lanes stay silent */
    plk_host_type[0][0] = 9 << 16; E(0) = 111;
    plk_start(0); ticks(30);
    CHECK(E(0) == 111, "other engine: lane silent %d", E(0));
    plk_stop(0); plk_host_type[0][0] = 7 << 16 | 40;

    /* free: starts at REC, not at the first step */
    plk_clip[0].sync = 0; plk_start(0); ticks(30);
    plk_rec(0);
    CHECK(plk_run[0].state == PLK_REC && plk_run[0].pos == 0, "free: recording at once");
    plk_rec(0); CHECK(plk_run[0].state == PLK_IDLE, "REC again ends it");
    plk_stop(0);

    /* the first tick after a stop starts it (no reset call) */
    plk_clip[0].sync = 1; plk_start(0); ticks(30); plk_rec(0);
    CHECK(plk_run[0].state == PLK_ARMED, "synced while running: armed");
    ticks(66);
    CHECK(plk_run[0].state == PLK_REC && plk_run[0].pos == 0, "synced: recording from the clip start");
    plk_stop(0);

    /* CUT, length */
    plk_cut(0, 9); CHECK(plk_clip[0].lane[9].n == 0 && plk_clip[0].lane[0].n == 5, "cut one lane");
    plk_cut(0, -1); CHECK(plk_clip[0].lane[0].n == 0, "cut all");
    plk_set_len(0, 6); CHECK(plk_len_ticks[plk_clip[0].len_i] == 6144, "16 bars");
    plk_set_len(0, 99); CHECK(plk_clip[0].len_i == 6, "clamped");

    /* 16 bars of continuous turning: events spaced, the whole pass recorded */
    plk_cut(0, -1); plk_set_len(0, 6); plk_clip[0].sync = 0; plk_start(0); plk_rec(0);
    for (int i = 0; i < 6144; i++) { E(2) = (int16_t)(i * 5); plk_tick(0); }
    CHECK(plk_run[0].state == PLK_IDLE, "16 bars: pass over");
    { const plk_lane_t *l2 = &plk_clip[0].lane[2];
      CHECK(l2->n > 100 && l2->n <= 256 && l2->ev[l2->n - 1].t > 6000, "16 bars: %d events, last at %d", l2->n, l2->ev[l2->n - 1].t); }
    ticks(3000); CHECK(E(2) > 14000 && E(2) < 16000, "16 bars: played back at the middle %d", E(2));
    /* STOP halts and restores, PLAY restarts */
    plk_halt(1); { int16_t v = E(2); int32_t p = plk_run[0].pos; ticks(100); CHECK(E(2) == v && plk_run[0].pos == p, "halted"); }
    plk_halt(0); ticks(10); CHECK(plk_run[0].pos == 10, "PLAY: from the start");
    plk_stop(0); plk_set_len(0, 0);

    /* persistence: a lane and the header round trip; malformed rows are refused */
    { static char buf[16 + 8 * PLK_EV + 1];
      plk_lane_t *a = &plk_clip[1].lane[3];
      a->type = 0x00070028; a->start = -5; a->n = 3;
      a->ev[0].t = 0; a->ev[0].v = -5; a->ev[1].t = 40; a->ev[1].v = 32767; a->ev[2].t = 95; a->ev[2].v = -32768;
      plk_clip[1].len_i = 5; plk_clip[1].sync = 0;
      int n = plk_serialize(16 + 3, buf), nh = plk_serialize(16 + 15, buf + 2000);
      plk_lane_t keep = *a; a->n = 0; plk_clip[1].len_i = 0; plk_clip[1].sync = 1;
      CHECK(plk_parse(16 + 3, buf, n) && plk_parse(16 + 15, buf + 2000, nh), "parse");
      CHECK(a->n == 3 && a->type == keep.type && a->start == -5 && a->ev[1].t == 40 && a->ev[2].v == -32768, "lane round trip");
      CHECK(plk_clip[1].len_i == 5 && plk_clip[1].sync == 0, "header round trip");
      CHECK(!plk_parse(16 + 3, buf, n - 1) && a->n == 3, "short row refused, lane kept");
      buf[16 + 8] = 'z'; CHECK(!plk_parse(16 + 3, buf, n), "bad hex refused");
      CHECK(!plk_parse(16 + 15, "0900", 4), "length out of range refused"); }

    /* track lock: the clip restarts with the chosen track only */
    plk_cut(0, -1); plk_set_len(0, 3); plk_clip[0].sync = 1; plk_clip[0].trk = 2; plk_set_tracks(0, 6);
    plk_halt(0); plk_start(0); ticks(50);
    plk_track_restart(0, 0); CHECK(plk_run[0].pos == 50, "other track: no restart");
    plk_track_restart(0, 1); CHECK(plk_run[0].pos == 0, "track 2: back to 0");
    plk_rec(0); ticks(30); plk_track_restart(0, 1);
    CHECK(plk_run[0].state == PLK_REC && plk_run[0].pos == 0, "armed: recording starts with the track");
    ticks(48); plk_track_restart(0, 1); CHECK(plk_run[0].state == PLK_IDLE, "recording ends when the track restarts");
    plk_clip[0].trk = 0; ticks(10); plk_track_restart(0, 1); CHECK(plk_run[0].pos == 10, "off: runs free");
    { char b[8]; int n = plk_serialize(15, b); plk_clip[0].trk = 3; n = plk_serialize(15, b);
      plk_clip[0].trk = 0; CHECK(n == 6 && plk_parse(15, b, n) && plk_clip[0].trk == 3, "track lock saved");
      CHECK(plk_parse(15, "0301", 4) && plk_clip[0].trk == 0, "old 4-char row: off"); }
    plk_stop(0);

    /* start lock: only the track's start restarts the clip, its loops do not */
    plk_set_len(0, 4); plk_clip[0].trk = PLK_START + 1; plk_halt(0); plk_start(0); ticks(200);
    plk_track_restart(0, 0 | PLK_FIRST); CHECK(plk_run[0].pos == 0, "start: restarts");
    ticks(384); plk_track_restart(0, 0); CHECK(plk_run[0].pos == 384, "loop: no restart, the clip goes on (%d)", (int)plk_run[0].pos);
    plk_track_restart(0, 1 | PLK_FIRST); CHECK(plk_run[0].pos == 384, "another track's start: none");
    { char b[8]; int n = plk_serialize(15, b); plk_clip[0].trk = 0;
      CHECK(plk_parse(15, b, n) && plk_clip[0].trk == PLK_START + 1, "start lock saved"); }
    plk_stop(0);

    if (fails) { printf("plk: %d failed\n", fails); return 1; }
    printf("plk: ok\n");
    return 0;
}
