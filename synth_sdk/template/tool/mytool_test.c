/* Host test of mytool: python3 pluginkit.py check <this folder> builds it with the plugin's sources (ASan + UBSan) and
 * runs it; any FAIL line, sanitizer report or a nonzero exit fails the check. Add a check for each thing your tool
 * promises. With the argument "render" it also prints overlay frames (JSON) for the preview PNG. */
#include "harness/plugin_test.h"
extern const op1_tool_t TOOL_mytool;

int main(int argc, char **argv)
{
    pt_init();
    int render = argc > 1 && !strcmp(argv[1], "render");

    /* without Shift: nothing; the key is never swallowed */
    CHECK(TOOL_mytool.key(0x01, 1) == 0 && TOOL_mytool.key(0x01, 0) == 0, "metronome swallowed");
    TOOL_mytool.service();
    CHECK(pt_overlay == 0, "overlay without Shift");
    /* Shift + metronome: the next main-loop pass shows the overlay */
    host_shift = 1;
    CHECK(TOOL_mytool.key(0x01, 1) == 0, "Shift + metronome swallowed");
    TOOL_mytool.key(0x01, 0);
    TOOL_mytool.service();
    CHECK(pt_overlay == &TOOL_mytool, "no overlay after Shift + metronome");
    for (int i = 0; i < 4; i++) { TOOL_mytool.key(0x01, 1); TOOL_mytool.key(0x01, 0); }
    TOOL_mytool.service();
    /* the overlay draws inside the screen for a while, then ends */
    int frames = 0;
    pt_frame_start();
    while (TOOL_mytool.draw(host_raster())) {
        CHECK(pt_nlines > 0 && pt_lines_on_screen(), "frame %d: no lines, or lines off the screen", frames);
        if (render && frames == 0) pt_frame("5 presses");
        pt_frame_start();
        if (++frames > 1000) break;
    }
    CHECK(frames > 10 && frames <= 1000, "overlay shown for %d frames", frames);
    /* other keys: never taken */
    for (int code = 0; code < 0x40; code++) CHECK(TOOL_mytool.key(code, 1) == 0 && TOOL_mytool.key(code, 0) == 0, "key %#x swallowed", code);
    return pt_done();
}
