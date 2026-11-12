/* Host test of mysynth: python3 pluginkit.py check <this folder> builds it with the plugin's sources (ASan + UBSan) and
 * runs it; any FAIL line, sanitizer report or a nonzero exit fails the check. Add a check for each thing your engine
 * promises. With the argument "render" it also prints screen frames (JSON) for the preview PNG. */
#include "harness/plugin_test.h"
extern const op1_engine_t ENGINE_mysynth;
extern const op1_screen_t SCREEN_mysynth;

int main(int argc, char **argv)
{
    pt_init();
    int16_t k[8] = { 4000, 12000, 6000, 24000 };              /* plugin.json "defaults" */

    /* sound: a note at the defaults is heard; level 0 is silent */
    CHECK(pt_synth_note(&ENGINE_mysynth, 60, 127, k, 300) > 1000, "no sound at the default knobs");
    int16_t quiet[8] = { 4000, 12000, 6000, 0 };
    CHECK(pt_synth_note(&ENGINE_mysynth, 60, 127, quiet, 100) == 0, "level 0 is not silent");
    /* every knob at both ends, low and high notes, soft and hard: the sanitizers catch overflows */
    for (int m = 0; m < 16; m++)
        for (int n = 24; n <= 108; n += 42) {
            int16_t e[8] = { 0 };
            for (int i = 0; i < 4; i++) e[i] = (int16_t)(m >> i & 1 ? 32767 : 0);
            pt_synth_note(&ENGINE_mysynth, n, n & 1 ? 1 : 127, e, 50);
        }

    /* screen: draws inside the 320 x 160 frame at any knob position */
    SCREEN_mysynth.init(0);
    static const int16_t states[3][4] = { { 4000, 12000, 6000, 24000 }, { 0, 0, 0, 0 }, { 32767, 32767, 32767, 32767 } };
    static const char *labels[3] = { "defaults", "all knobs min", "all knobs max" };
    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < 4; i++) host_knobs[0][i] = states[s][i];
        SCREEN_mysynth.update(0, 16);
        CHECK(pt_nlines > 0 && pt_lines_on_screen(), "screen (%s): no lines, or lines off the screen", labels[s]);
        if (argc > 1 && !strcmp(argv[1], "render")) pt_frame(labels[s]);
    }
    return pt_done();
}
