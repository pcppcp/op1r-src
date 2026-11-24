/* Host test: the tape FX feedback loop. Plucked saws for 2 s, then 10 s of silence; over the last second: below 100 %
 * feedback the echoes have died out (age's drive inside the loop used to multiply the feedback: at full age 33 % went
 * round above 1 and never stopped), and at 110 % (self-oscillation) the loop holds no DC.
 *   cc -DOP1_HOST -I.. tape_fb_test.c -lm -o build/tape_fb_test && build/tape_fb_test */
#include "../plugins/tape/tape.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4];
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
void *host_raster(void) { return 0; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c) { (void)r; (void)a; (void)b; (void)w; (void)c; }
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t z, const uint8_t *c) { (void)x; (void)y; (void)z; (void)c; return o; }
void host_text_set(void *t, const char *s) { (void)t; (void)s; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
uint8_t *host_node_style_rgba(void *scr, const char *id) { static uint8_t c[4]; (void)scr; (void)id; return c; }
void host_text_draw(void *t, void *r, const int32_t *m) { (void)t; (void)r; (void)m; }

int main(void)
{
    static int32_t scratch[44115]; host_fx_scratch[0] = scratch;
    static const struct { const char *l; int16_t k[4]; int dies; } c[] = {
        { "age 0, fb 50 %", { 0, 14895, 12000, 0 }, 1 }, { "age max, fb 33 %", { 32767, 9830, 12000, 0 }, 1 },
        { "age max, fb 50 %", { 32767, 14895, 12000, 0 }, 1 }, { "age max, fb 90 %", { 32767, 26810, 12000, 0 }, 0 },
        { "age 50 %, fb 110 %", { 16384, 32767, 16384, 16384 }, 0 }, { "all max (fb 110 %)", { 32767, 32767, 32767, 32767 }, 0 } };
    int fails = 0;
    for (unsigned j = 0; j < sizeof c / sizeof c[0]; j++) {
        int16_t k[8] = { c[j].k[0], c[j].k[1], c[j].k[2], c[j].k[3], OP1_ENGINE_MAGIC + 1, 0, 0, 0 };
        for (int i = 0; i < 44115; i++) scratch[i] = 0;
        tape_init(0);
        int16_t buf[OP1_FX_BLOCK]; uint32_t ph = 0; int32_t env = 0;
        const int nb = 44100 * 12 / OP1_FX_BLOCK, last = 44100 / OP1_FX_BLOCK;
        double sum = 0; int peak = 0;
        for (int b = 0; b < nb; b++) {
            if (b * OP1_FX_BLOCK < 2 * 44100) { if (b % 276 == 0) { env = 20000; ph = 0; } } else env = 0;
            for (int i = 0; i < OP1_FX_BLOCK; i++) { ph += 13000000u; buf[i] = (int16_t)((((int32_t)ph >> 16) * env) >> 15); env -= env >> 11; }
            tape_render(0, buf, k);
            if (b >= nb - last) for (int i = 0; i < OP1_FX_BLOCK; i++) { sum += buf[i]; if (abs(buf[i]) > peak) peak = abs(buf[i]); }
        }
        double mean = sum / (last * OP1_FX_BLOCK);
        int ok = c[j].dies ? peak == 0 : fabs(mean) < 64;
        printf("%-20s after 10 s of silence: mean %7.1f, peak %5d%s\n", c[j].l, mean, peak, ok ? "" : c[j].dies ? "  FAIL: still sounding" : "  FAIL: DC");
        fails += !ok;
    }
    printf(fails ? "%d FAILED\n" : "OK: tape feedback\n", fails);
    return fails != 0;
}
