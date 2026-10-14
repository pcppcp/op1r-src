/* Host test for tape/miditrack.c: recording (held / latched REC, overdub, held notes at the end), playback forward /
 * reverse / across the loop wrap / after a locate, stop releases, the recursion guard, scissors / lift / drop and the
 * saved text round trip. Tape state 10 = playing. */
#include "../op1_synth.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int32_t host_tape_state, host_tape_pos, host_tape_speed = 1 << 21, host_loop_in, host_loop_out, host_loop_on;
int host_mode, host_fx_type, host_fx_lo[4], host_fx_hi[4];
int16_t host_synth_words[8], host_fx_words[4];
int32_t host_sel_track; uint8_t host_track_muted[4];

typedef struct { int on, midi, vel, param, pos; } call_t;
static call_t calls[256];
static int ncalls;
void mt_vc_down(const int32_t *rec, int param);
void mt_vc_up(const int32_t *rec);
/* the firmware VoiceCtl entry runs our hook first (hook.S), also for the track's own notes */
void host_vc_down(int32_t *rec, int param)
{
    mt_vc_down(rec, param);
    if (ncalls < 256) calls[ncalls++] = (call_t){ 1, rec[0], rec[1], param, host_tape_pos >> 6 };
}
void host_vc_up(int32_t *rec)
{
    mt_vc_up(rec);
    if (ncalls < 256) calls[ncalls++] = (call_t){ 0, rec[0], 0, 0, host_tape_pos >> 6 };
}
void *host_raster(void) { return 0; }
int32_t tm_zoom_q10(void) { return 40 << 10; }
void host_mt_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint8_t *c) { (void)x0; (void)y0; (void)x1; (void)y1; (void)c; }
#include "../tape/miditrack.c"

static int32_t pos_s(void) { return host_tape_pos >> 6; }
static void step(int32_t samples)                            /* one audio block of travel */
{
    int32_t p = pos_s() + samples;
    if (host_loop_on && samples > 0 && p > host_loop_out) p = host_loop_in + (p - host_loop_out);
    if (host_loop_on && samples < 0 && p < host_loop_in) p = host_loop_out - (host_loop_in - p);
    if (p < 0) p = 0;
    host_tape_pos = p << 6;
    mt_block();
}
static void run_to(int32_t target, int32_t per_block)       /* travel until the position passes target */
{
    for (int i = 0; i < 100000; i++) {
        if (per_block > 0 ? pos_s() >= target : pos_s() <= target) return;
        step(per_block);
    }
    assert(0);
}
static void key(int midi, int vel, int down)                  /* a keyboard note through VoiceCtl */
{
    int32_t r[4] = { midi, vel, 0, 0 }; ((uint8_t *)r)[8] = 1;
    if (down) mt_vc_down(r, 0); else mt_vc_up(r);
}
static void play_from(int32_t p, int32_t speed)
{
    host_tape_state = 0; mt_block();
    host_tape_pos = p << 6; host_tape_speed = speed < 0 ? -(1 << 21) : 1 << 21;
    mt_block(); mt_block();                                   /* (located: stands still there) */
    host_tape_state = 10; ncalls = 0; mt_block();
}
static void stop(void) { host_tape_state = 0; mt_block(); mt_block(); }   /* (stands still for a block) */
static void reset(void)
{
    stop(); mt_clear(); mt_sel = 0; mt_rec_held = mt_rec_latch = 0; mt_cb_len = -1; ncalls = 0;
    host_loop_on = 0; mt_dirty = 0;
    memset(mt_sounding, 0, sizeof mt_sounding);
    host_mode = 0; host_fx_type = 0; memset(host_synth_words, 0, sizeof host_synth_words); memset(host_fx_words, 0, sizeof host_fx_words);
    memset(host_fx_lo, 0, sizeof host_fx_lo); memset(host_fx_hi, 0, sizeof host_fx_hi);
}
static int n_auto(int t) { int n = 0; for (int i = 0; i < mt_n; i++) n += (mt_ev[i].flags & MT_AUTO) && mt_ev[i].midi == t; return n; }
/* knob automation: record knob 0 (synth) 1000 -> 5000 at ~2000 -> 9000 at ~3000, REC held 1000..4000 */
static void record_knob(void)
{
    reset(); mt_sel = 1; host_synth_words[0] = 1000;
    play_from(1000, 1); mt_key(0x21, 1, 0, 1); step(64);
    run_to(2000, 64); host_synth_words[0] = 5000;
    run_to(3000, 64); host_synth_words[0] = 9000;
    run_to(4000, 64); mt_key(0x21, 0, 0, 1); step(64); stop();
}
static void test_automation(void)
{
    record_knob();
    assert(n_auto(0) == 3 && mt_n == 3 && mt_nclips == 1);
    for (int i = 0; i < mt_n; i++) assert(!(mt_ev[i].flags & MT_FRESH));
    assert(mt_val(&mt_ev[0]) == 1000 && mt_val(&mt_ev[1]) == 5000 && mt_val(&mt_ev[2]) == 9000);
    /* playback writes the values at their positions */
    host_synth_words[0] = 0; play_from(500, 1);
    run_to(1200, 64); assert(host_synth_words[0] == 1000);
    run_to(2200, 64); assert(host_synth_words[0] == 5000);
    run_to(3200, 64); assert(host_synth_words[0] == 9000);
    stop();
    /* REC latched before PLAY, playback later started from the same position: the start value is played */
    {
        mt_clear(); host_synth_words[0] = 1000;
        mt_key(0x21, 1, 1, 1); mt_key(0x21, 0, 1, 1);                 /* Shift + REC while stopped */
        play_from(800, 1); run_to(1500, 64); host_synth_words[0] = 2000; run_to(1700, 64); stop();
        host_synth_words[0] = 0; play_from(800, 1); assert(host_synth_words[0] == 1000); stop();
        record_knob(); host_synth_words[0] = 0;
    }
    /* spin-up: the tape moves 4300 samples before its state says playing; what lies there still plays */
    stop(); host_tape_pos = 900 << 6; mt_block(); mt_block(); host_synth_words[0] = 0;
    for (int i = 0; i < 25; i++) { host_tape_pos += 64 << 6; mt_block(); }        /* state 0, moving to 2500 */
    assert(host_synth_words[0] == 0);
    host_tape_state = 10; step(64); assert(host_synth_words[0] == 5000); stop();   /* 1000 and 5000 passed */
    /* taken over: a hand turn after 2200 keeps its value past the next event; STOP gives it back */
    play_from(500, 1); run_to(2200, 64); host_synth_words[0] = 7777;
    run_to(3500, 64); assert(host_synth_words[0] == 7777);
    stop(); play_from(2500, 1); run_to(3200, 64); assert(host_synth_words[0] == 9000); stop();
    /* reverse: crossing the 9000 event backwards restores the value before it */
    play_from(3500, -1); run_to(2500, -64); assert(host_synth_words[0] == 5000); stop();
    /* drum mode: synth knob automation does not apply */
    host_mode = 1; host_synth_words[0] = 1; play_from(500, 1); run_to(3500, 64); assert(host_synth_words[0] == 1); stop();
    host_mode = 0;
    /* another engine selected mid-play: the takeover is forgotten, automation goes on with the new engine */
    play_from(500, 1); run_to(1200, 64); host_synth_words[0] = 4444; step(64);
    run_to(2200, 64); assert(host_synth_words[0] == 4444);
    host_synth_words[4] = 0x5301; step(64);                         /* (selector change: new context) */
    run_to(3200, 64); assert(host_synth_words[0] == 9000);
    stop(); host_synth_words[4] = 0;
    /* re-recording knob 0 over 1500..2600, turned at 2500: its older values from 2500 to 2600 are replaced */
    host_synth_words[0] = 9000; play_from(1500, 1); mt_key(0x21, 1, 0, 1); step(64);
    run_to(2500, 64); host_synth_words[0] = 20000;
    run_to(2600, 64); mt_key(0x21, 0, 0, 1); step(64); stop();
    assert(n_auto(0) == 4);                                         /* 1000@1000, 5000@2000 (before the touch), 20000, 9000@3000 */
    host_synth_words[0] = 0; play_from(500, 1);
    run_to(2200, 64); assert(host_synth_words[0] == 5000);
    run_to(2700, 64); assert(host_synth_words[0] == 20000);
    run_to(3200, 64); assert(host_synth_words[0] == 9000);
    stop();
    /* touched in a loop take after the wrap: the whole take is replaced */
    record_knob();
    host_loop_on = 1; host_loop_in = 1500; host_loop_out = 3500;
    play_from(1600, 1); mt_key(0x21, 1, 0, 1); step(64);
    run_to(3400, 64); for (int i = 0; i < 10; i++) step(64);         /* wrapped to ~1600 */
    host_synth_words[0] = 3333; step(64); run_to(1800, 64);
    mt_key(0x21, 0, 0, 1); step(64); stop(); host_loop_on = 0;
    for (int i = 0; i < mt_n; i++) if (mt_ev[i].pos >= 1500 && mt_ev[i].pos <= 3500) assert(mt_val(&mt_ev[i]) == 3333);
    /* thinning: a knob changing every block -> about one value per MT_THIN blocks (+ start and end) */
    reset(); mt_sel = 1; play_from(1000, 1); mt_key(0x21, 1, 0, 1); step(64);
    for (int i = 0; i < 140; i++) { host_synth_words[1] = (int16_t)(i * 100); step(64); }
    mt_key(0x21, 0, 0, 1); step(64); stop();
    assert(n_auto(1) >= 140 / MT_THIN && n_auto(1) <= 140 / MT_THIN + 3);
    /* FX knob: kept as 0..32767 of the type's range, written into the current FX type's range */
    reset(); mt_sel = 1; host_fx_lo[2] = 64; host_fx_hi[2] = 14528; host_fx_type = 5; host_fx_words[2] = 64;
    play_from(1000, 1); mt_key(0x21, 1, 0, 1); step(64);
    run_to(2000, 64); host_fx_words[2] = 14528;                       /* 0 % -> 100 % */
    run_to(3000, 64); mt_key(0x21, 0, 0, 1); step(64); stop();
    assert(n_auto(10) == 2);
    host_fx_type = 3; host_fx_lo[2] = 0; host_fx_hi[2] = 24576;      /* another FX: same 0..100 % in its range */
    host_fx_words[2] = 100; play_from(500, 1); run_to(1500, 64); assert(host_fx_words[2] == 0);
    run_to(2500, 64); assert(host_fx_words[2] == 24576); stop();
    /* saved and loaded again: same automation */
    static char notes[MT_ROW * 14 + 1];
    int nn = mt_fmt_notes(notes, 0), n = mt_n;
    mt_clear(); mt_parse_notes(notes, nn);
    assert(mt_n == n && n_auto(10) == 2 && mt_val(&mt_ev[1]) == 32767);
}
static int count(int on, int midi) { int n = 0; for (int i = 0; i < ncalls; i++) n += calls[i].on == on && calls[i].midi == midi; return n; }

/* record C4 (60) at ~11000..12000 and E4 (64) at ~13000.. held to the end, REC held from 10000 to 15000 */
static void record_basic(void)
{
    reset();
    assert(!mt_key(0x09, 1, 1, 1) && !mt_sel);               /* Shift + T4: stock (mute of track 4) */
    assert(!mt_key(0x09, 0, 1, 1));
    assert(!mt_key(0x04, 1, 0, 1));                           /* TAPE held + T1 */
    assert(mt_key(0x06, 1, 0, 1) && mt_sel);
    assert(!mt_key(0x04, 0, 0, 1));
    assert(mt_key(0x06, 0, 0, 1));                            /* its release (TAPE already up) is swallowed too */
    play_from(10000, 1);
    assert(mt_key(0x21, 1, 0, 1)); step(64);                  /* REC held */
    assert(mt_recording);
    run_to(11000, 64); key(60, 100, 1);
    run_to(12000, 64); key(60, 0, 0);
    run_to(13000, 64); key(64, 90, 1);
    run_to(15000, 64);
    assert(mt_key(0x21, 0, 0, 1)); step(64);                  /* REC released: recording ends, E4 closed */
    assert(!mt_recording && mt_dirty);
}

int main(void)
{
    mt_service();                                             /* load (host: nothing) */
    assert(mt_loaded);

    /* not selected: REC is stock, nothing recorded */
    reset(); play_from(1000, 1); assert(!mt_key(0x21, 1, 0, 1)); run_to(2000, 64); key(60, 100, 1); assert(mt_n == 0);
    assert(!mt_key(0x21, 0, 0, 1));

    record_basic();
    assert(mt_n == 4 && mt_nclips == 1);
    assert(mt_clip[0].start <= 10064 && mt_clip[0].stop >= 15000);
    assert(mt_ev[0].midi == 60 && (mt_ev[0].flags & MT_ON) && mt_ev[0].vel == 100 && mt_ev[0].pos >= 11000 && mt_ev[0].pos < 11064);
    assert(mt_ev[1].midi == 60 && !(mt_ev[1].flags & MT_ON));
    assert(mt_ev[3].midi == 64 && !(mt_ev[3].flags & MT_ON) && mt_ev[3].pos >= 15000);
    assert(ncalls == 0);                                      /* recorded notes are not played back in the same pass */

    /* forward playback from 9000: on 60, off 60, on 64, off 64, in order and at their positions */
    play_from(9000, 1); run_to(16000, 64);
    assert(ncalls == 4 && calls[0].on && calls[0].midi == 60 && calls[0].vel == 100 && calls[1].midi == 60 && !calls[1].on);
    assert(calls[0].pos >= mt_ev[0].pos && calls[0].pos < mt_ev[0].pos + 64);
    assert(calls[2].on && calls[2].midi == 64 && calls[2].vel == 90 && !calls[3].on);
    stop();

    /* the guard: playing did not record anything */
    assert(mt_n == 4);

    /* starting exactly on a note plays it */
    play_from(mt_ev[0].pos, 1); assert(count(1, 60) == 1); stop();

    /* reverse from 16000: E4 starts at its end (velocity of its note-on), C4 starts at 12000 and ends at 11000 */
    play_from(16000, -1); run_to(9000, -64);
    assert(ncalls == 4 && calls[0].on && calls[0].midi == 64 && calls[0].vel == 90);
    assert(calls[2].on && calls[2].midi == 60 && calls[2].vel == 100 && calls[2].pos <= mt_ev[1].pos && !calls[3].on && calls[3].pos <= mt_ev[0].pos);
    stop();

    /* stop while a note sounds: released once; then nothing on the next stop */
    play_from(11500, 1); run_to(11600, 64);                   /* inside C4 but started after its on: silent */
    assert(ncalls == 0);
    play_from(10900, 1); run_to(11500, 64); assert(count(1, 60) == 1 && count(0, 60) == 0);
    stop(); assert(count(0, 60) == 1); stop(); assert(count(0, 60) == 1);

    /* loop 10500..11500: C4 on at 11000 each pass, released at every wrap */
    play_from(10600, 1); host_loop_on = 1; host_loop_in = 10500; host_loop_out = 11500;
    for (int i = 0; i < 3 * 1000 / 64 + 3; i++) step(64);
    assert(count(1, 60) == 3 && count(0, 60) >= 2);
    stop(); host_loop_on = 0;
    for (int n = 0; n < 4; n++) assert(!mt_sounding[n]);

    /* a locate (jump without loop) releases and skips what lies between */
    play_from(10900, 1); run_to(11200, 64); assert(count(1, 60) == 1);
    host_tape_pos = 14000 << 6; step(0);
    assert(count(0, 60) == 1 && count(1, 64) == 0 && count(0, 64) == 0);
    run_to(15500, 64); assert(count(0, 64) == 0);                         /* E4 off without its on: not sent */
    stop();

    /* overdub on a second pass with latched REC (Shift + REC); STOP ends it and clears the latch */
    play_from(10000, 1);
    assert(mt_key(0x21, 1, 1, 1) && mt_key(0x21, 0, 1, 1));
    step(64); assert(mt_recording);
    run_to(14000, 64); key(67, 80, 1); run_to(14500, 64); key(67, 0, 0);
    run_to(17000, 64);
    assert(!mt_key(0x23, 1, 0, 1)); stop();                   /* STOP is stock too */
    assert(!mt_recording && mt_n == 6 && mt_nclips == 1 && mt_clip[0].stop >= 17000 && !mt_rec_latch);
    play_from(13900, 1); run_to(14600, 64); assert(count(1, 67) == 1 && count(0, 67) == 1); stop();

    /* T1..T4 select a stock track again (and pass on) */
    assert(!mt_key(0x07, 1, 0, 1) && !mt_sel);
    assert(!mt_key(0x0C, 1, 0, 1));                           /* scissors: stock while an audio track is selected */
    mt_sel = 1;

    /* scissors at 12500: two clips; lift the second (12501..) -> clipboard; drop it at 30000 */
    host_tape_pos = 12500 << 6; assert(mt_key(0x0C, 1, 0, 1) && mt_key(0x0C, 0, 0, 1));
    assert(mt_nclips == 2 && mt_clip[0].stop == 12500 && mt_clip[1].start == 12501);
    host_tape_pos = 14000 << 6; assert(mt_key(0x0A, 1, 0, 1)); mt_key(0x0A, 0, 0, 1);
    assert(mt_nclips == 1 && mt_n == 2 && mt_cb_n == 4);       /* C4 stays, E4 and G4 lifted */
    host_tape_pos = 30000 << 6; assert(mt_key(0x0B, 1, 0, 1)); mt_key(0x0B, 0, 0, 1);
    assert(mt_nclips == 2 && mt_n == 6 && mt_clip[1].start == 30000);
    for (int i = 1; i < mt_n; i++) assert(mt_ev[i - 1].pos <= mt_ev[i].pos);
    play_from(29000, 1); run_to(36000, 64); assert(count(1, 64) == 1 && count(1, 67) == 1 && count(0, 64) == 1); stop();

    /* saved text round trip */
    static char clips[2000], notes[MT_ROW * 14 + 1];
    mt_fmt_clips(clips); int nn = mt_fmt_notes(notes, 0);
    mt_ev_t ev[8]; mt_clip_t cl[4]; int n = mt_n, nc = mt_nclips;
    memcpy(ev, mt_ev, sizeof ev); memcpy(cl, mt_clip, sizeof cl);
    mt_clear(); mt_parse_clips(clips, (int)strlen(clips)); mt_parse_notes(notes, nn);
    assert(mt_n == n && mt_nclips == nc && !memcmp(cl, mt_clip, nc * sizeof cl[0]));
    for (int i = 0; i < n; i++)
        assert(ev[i].pos == mt_ev[i].pos && ev[i].midi == mt_ev[i].midi && ev[i].vel == mt_ev[i].vel &&
               ev[i].flags == mt_ev[i].flags && ev[i].param == mt_ev[i].param);
    mt_clear(); mt_parse_notes("zz", 2); mt_parse_notes("0000103c64ff0", 13); mt_parse_clips("5,", 2);
    assert(mt_n == 0 && mt_nclips == 0);

    test_automation();

    /* monitoring: with M selected the stock selection moves off a muted audio track (keyboard / MIDI audible) */
    reset(); host_sel_track = 0; host_track_muted[0] = 1;
    mt_key(0x04, 1, 0, 1); mt_key(0x06, 1, 0, 1); mt_key(0x06, 0, 0, 1); mt_key(0x04, 0, 0, 1);
    assert(mt_sel && host_sel_track == 1);
    assert(!mt_key(0x07, 1, 1, 1) && mt_sel); mt_key(0x07, 0, 1, 1);   /* Shift + T2 (stock mute of track 2): M stays */
    host_track_muted[1] = 1; mt_service(); assert(host_sel_track == 2);
    memset(host_track_muted, 1, 4); host_sel_track = 2; mt_service(); assert(host_sel_track == 2);   /* all muted */
    memset(host_track_muted, 0, 4); mt_sel = 0; host_track_muted[0] = 1; host_sel_track = 0;
    mt_service(); assert(host_sel_track == 0);                         /* audio track selected: stock */
    host_track_muted[0] = 0;
    /* TAPE + T1: selects M; again while M is selected: mute / unmute */
    mt_sel = 0; mt_mute = 0;
    mt_key(0x04, 1, 0, 1); mt_key(0x06, 1, 0, 1); mt_key(0x06, 0, 0, 1); mt_key(0x04, 0, 0, 1); assert(mt_sel && !mt_mute);
    mt_key(0x04, 1, 0, 1); mt_key(0x06, 1, 0, 1); mt_key(0x06, 0, 0, 1); mt_key(0x04, 0, 0, 1); assert(mt_sel && mt_mute);
    mt_key(0x04, 1, 0, 1); mt_key(0x06, 1, 0, 1); mt_key(0x06, 0, 0, 1); mt_key(0x04, 0, 0, 1); assert(mt_sel && !mt_mute);

    /* drum mode: notes outside the 24 drum keys fold in by octaves (and are released folded the same way) */
    reset(); mt_sel = 1; play_from(1000, 1); mt_key(0x21, 1, 0, 1); step(64);
    key(40, 100, 1); run_to(1500, 64); key(40, 0, 0); key(90, 100, 1); run_to(2000, 64); key(90, 0, 0);
    run_to(3000, 64); mt_key(0x21, 0, 0, 1); step(64); stop();
    host_mode = 1; play_from(900, 1); run_to(3000, 64);
    assert(count(1, 64) == 1 && count(0, 64) == 1 && count(1, 66) == 1 && count(0, 66) == 1 && count(1, 40) == 0); stop();
    host_mode = 0;
    /* a note cut by lift (its off lifted away) is released when the playhead leaves the clip */
    reset(); mt_sel = 1; play_from(1000, 1); mt_key(0x21, 1, 0, 1); step(64);
    key(60, 100, 1); run_to(5000, 64); key(60, 0, 0); run_to(6000, 64); mt_key(0x21, 0, 0, 1); step(64); stop();
    host_tape_pos = 3000 << 6; mt_key(0x0C, 1, 0, 1); mt_key(0x0C, 0, 0, 1);   /* scissors at 3000 */
    host_tape_pos = 4000 << 6; mt_key(0x0A, 1, 0, 1); mt_key(0x0A, 0, 0, 1);   /* lift the second half */
    assert(mt_nclips == 1 && mt_n == 1);
    play_from(900, 1); run_to(3100, 64); assert(count(1, 60) == 1 && count(0, 60) == 1); stop();
    /* load repair: notes outside any clip get a clip again */
    mt_clear();
    { mt_ev_t e1 = { 20000, 60, 100, MT_ON, 0 }, e2 = { 30000, 60, 0, 0, 0 }, e3 = { 500000, 62, 100, MT_ON, 0 };
      mt_insert(&e1); mt_insert(&e2); mt_insert(&e3); }
    mt_heal();
    assert(mt_nclips == 2 && mt_clip[0].start == 20000 && mt_clip[0].stop >= 30000 && mt_clip[1].start == 500000);

    /* plays while an audio track records (state 4) and while starting (9); muted: nothing, unmuted again */
    record_basic();
    play_from(10500, 1); host_tape_state = 4; run_to(12500, 64); assert(count(1, 60) == 1 && count(0, 60) == 1); stop();
    mt_key(0x04, 1, 0, 1); assert(mt_key(0x06, 1, 0, 1) && mt_mute); mt_key(0x06, 0, 0, 1); mt_key(0x04, 0, 0, 1);
    play_from(10500, 1); run_to(16000, 64); assert(ncalls == 0); stop();
    mt_key(0x04, 1, 0, 1); mt_key(0x06, 1, 0, 1); mt_key(0x06, 0, 0, 1); mt_key(0x04, 0, 0, 1); assert(!mt_mute);
    play_from(10500, 1); run_to(16000, 64); assert(count(1, 60) == 1 && count(1, 64) == 1); stop();
    /* muting while a note sounds releases it */
    play_from(10900, 1); run_to(11500, 64); assert(count(1, 60) == 1);
    mt_key(0x04, 1, 0, 1); mt_key(0x06, 1, 0, 1); assert(count(0, 60) == 1); mt_key(0x06, 0, 0, 1); mt_key(0x04, 0, 0, 1);
    mt_key(0x04, 1, 0, 1); mt_key(0x06, 1, 0, 1); mt_key(0x06, 0, 0, 1); mt_key(0x04, 0, 0, 1);
    stop();

    /* recording from the very tape start (position 0): the clip is kept, a note in the first block too */
    reset(); mt_sel = 1; host_tape_pos = 0; mt_block(); mt_block();
    mt_key(0x21, 1, 0, 1); host_tape_state = 10; mt_block();
    assert(mt_recording); key(60, 100, 1);
    run_to(3000, 64); key(60, 0, 0); run_to(5000, 64);
    mt_key(0x21, 0, 0, 1); step(64); stop();
    assert(mt_nclips == 1 && mt_clip[0].start == 0 && mt_n == 2 && mt_ev[0].pos == 0);
    play_from(0, 1); run_to(4000, 64); assert(count(1, 60) == 1 && count(0, 60) == 1); stop();

    /* full list: inserts stop, nothing breaks */
    reset(); mt_sel = 1; play_from(100, 1); mt_key(0x21, 1, 0, 1); step(64);
    for (int i = 0; i < MT_MAX + 10; i++) { key(60 + (i & 7), 100, 1); key(60 + (i & 7), 0, 0); if (!(i & 15)) step(64); }
    assert(mt_n == MT_MAX);
    mt_key(0x21, 0, 0, 1); step(64); stop();

    mt_draw();
    /* red theme: forced while armed / recording, never otherwise */
    {
        static uint8_t scr[0x100];
        reset(); mt_tape_pre(scr); assert(scr[0x93] == 0);
        mt_sel = 1; mt_key(0x21, 1, 0, 1); scr[0x98] = 7; mt_tape_pre(scr);
        assert(scr[0x93] == 1 && scr[0x98] == 0); mt_key(0x21, 0, 0, 1);
        scr[0x93] = 0; mt_tape_pre(scr); assert(scr[0x93] == 0);
    }
    puts("miditrack_test: ok");
    return 0;
}
