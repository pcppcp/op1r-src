/* Host test of myfx: python3 pluginkit.py check <this folder> builds it with the plugin's sources (ASan + UBSan) and
 * runs it; any FAIL line, sanitizer report or a nonzero exit fails the check. Add a check for each thing your effect
 * promises. With the argument "render" it also prints screen frames (JSON) for the preview PNG. */
#include "harness/plugin_test.h"
extern const op1_fx_t FX_myfx;
extern const op1_screen_t SCREEN_myfx;

int main(int argc, char **argv)
{
    pt_init();
    int16_t k[8] = { 8000, 16000, 4000, 32767 };              /* plugin.json "defaults" */

    /* every position (0 synth, 1 drum, 2 master): sound passes, silence stays silent */
    for (int pos = 0; pos < 3; pos++) {
        FX_myfx.init(pos);
        CHECK(pt_fx_run(&FX_myfx, pos, k, 300, 16000) > 1000, "position %d: no sound through the FX", pos);
        CHECK(pt_fx_run(&FX_myfx, pos, k, 300, 0) == 0, "position %d: output from silence", pos);
    }
    /* mix 0 passes the input unchanged */
    int16_t dry[8] = { 8000, 16000, 4000, 0 }, buf[OP1_FX_BLOCK], in[OP1_FX_BLOCK];
    FX_myfx.init(0);
    for (int i = 0; i < OP1_FX_BLOCK; i++) in[i] = buf[i] = (int16_t)(i * 500 - 16000);
    FX_myfx.render(0, buf, dry);
    CHECK(!memcmp(buf, in, sizeof buf), "mix 0 changes the input");
    /* every knob at both ends, full-scale input: the sanitizers catch overflows */
    for (int m = 0; m < 16; m++) {
        int16_t e[8] = { 0 };
        for (int i = 0; i < 4; i++) e[i] = (int16_t)(m >> i & 1 ? 32767 : 0);
        FX_myfx.init(2);
        pt_fx_run(&FX_myfx, 2, e, 50, 32767);
    }

    /* screen: draws inside the 320 x 160 frame at any knob position */
    SCREEN_myfx.init(0);
    static const int16_t states[3][4] = { { 8000, 16000, 4000, 32767 }, { 0, 0, 0, 0 }, { 32767, 32767, 32767, 32767 } };
    static const char *labels[3] = { "defaults", "all knobs min", "all knobs max" };
    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < 4; i++) host_fx_knobs[0][i] = states[s][i];
        pt_fx_run(&FX_myfx, 0, host_fx_knobs[0], 40, 16000);
        SCREEN_myfx.update(0, 16);
        CHECK(pt_nlines > 0 && pt_lines_on_screen(), "screen (%s): no lines, or lines off the screen", labels[s]);
        if (argc > 1 && !strcmp(argv[1], "render")) pt_frame(labels[s]);
    }
    return pt_done();
}
