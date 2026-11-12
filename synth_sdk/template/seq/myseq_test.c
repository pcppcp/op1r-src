/* Host test of myseq: python3 pluginkit.py check <this folder> builds it with the plugin's sources (ASan + UBSan) and
 * runs it; any FAIL line, sanitizer report or a nonzero exit fails the check. Add a check for each thing your sequencer
 * promises. With the argument "render" it also prints screen frames (JSON) for the preview PNG. */
#include "harness/plugin_test.h"
void myseq_key(int m, int note, int key), myseq_turn(int m, int knob, int delta), myseq_tick(int m);
void myseq_all_off(int m), myseq_screen_draw(void *r, int m);

static void ticks(int m, int n) { for (int i = 0; i < n; i++) myseq_tick(m); }

int main(int argc, char **argv)
{
    pt_init();
    int render = argc > 1 && !strcmp(argv[1], "render");
    if (render) { pt_frame_start(); myseq_screen_draw(host_raster(), 0); pt_frame("stopped"); }

    /* no note yet: starting plays nothing */
    myseq_turn(0, 3, 1024);
    ticks(0, 96);
    CHECK(pt_seq_on[0] == 0, "notes without a key");
    /* a key on the page: one note per 1/16 (default rate), at the default octave, 50 % gate */
    myseq_key(0, 60, 7);
    ticks(0, 96 * 4);
    CHECK(pt_seq_on[0] == 16, "4 beats at 1/16: %d notes, not 16", pt_seq_on[0]);
    CHECK(pt_seq_last[0] == 60, "pitch %d, not 60", pt_seq_last[0]);
    CHECK(pt_seq_held[0] <= 1, "%d notes held at once", pt_seq_held[0]);
    if (render) { pt_frame_start(); myseq_screen_draw(host_raster(), 0); pt_frame("running, 1/16"); }
    /* octave +1, rate 1/8 */
    myseq_turn(0, 2, 1024); myseq_turn(0, 0, -1024);
    int before = pt_seq_on[0];
    ticks(0, 96 * 2);
    CHECK(pt_seq_on[0] - before == 4, "2 beats at 1/8: %d notes, not 4", pt_seq_on[0] - before);
    CHECK(pt_seq_last[0] == 72, "octave +1: pitch %d, not 72", pt_seq_last[0]);
    if (render) { pt_frame_start(); myseq_screen_draw(host_raster(), 0); pt_frame("running, 1/8, octave +1"); }
    /* the drum mode is independent: nothing played there */
    CHECK(pt_seq_on[1] == 0, "the synth sequencer played in drum mode");
    /* stop: the held note ends, nothing more */
    myseq_turn(0, 3, -1024);
    before = pt_seq_on[0];
    ticks(0, 96);
    CHECK(pt_seq_held[0] == 0 && pt_seq_on[0] == before, "still playing after stop");
    /* gate 100 %, all rates, octaves at the ends, extreme pitches: notes stay in 0..127, one at a time */
    myseq_key(1, 125, 23); myseq_turn(1, 1, 10 * 1024); myseq_turn(1, 2, 5 * 1024); myseq_turn(1, 3, 1024);
    for (int r = 0; r < 6; r++) { myseq_turn(1, 0, r < 3 ? 1024 : -1024); ticks(1, 200); }
    CHECK(pt_seq_held[1] <= 1, "drums: %d notes held at once", pt_seq_held[1]);
    CHECK(pt_seq_on[1] == 0, "pitch out of range played (125 + 24)");
    myseq_all_off(1);
    CHECK(pt_seq_held[1] == 0, "all off left a note on");
    CHECK(pt_lines_on_screen(), "screen: lines off the screen");
    return pt_done();
}
