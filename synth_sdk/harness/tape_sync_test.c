/* Host test: the tape screen must move a mark from the record to the play head in exactly the DSP's
 * delay time, whatever the screen frame rate.
 *   cc -DOP1_HOST -I.. tape_sync_test.c -lm -o build/tape_sync_test && build/tape_sync_test */
#include "../plugins/tape/tape.c"
#include <math.h>
#include <stdio.h>

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
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    int fail = 0;
    const int fps[] = { 20, 30, 60, 90 };
    const int16_t times[] = { 0, 8000, 20000, 32767 };
    for (int t = 0; t < 4; t++) for (int f = 0; f < 4; f++) {
        int16_t k[8] = { 12000, 14000, times[t], 6000, OP1_ENGINE_MAGIC + 1 };
        for (int i = 0; i < 8; i++) host_fx_knobs[0][i] = k[i];
        tape_init(0); tape_screen_init(0);
        int16_t buf[OP1_FX_BLOCK] = { 0 };
        for (int i = 0; i < 2000; i++) tape_render(0, buf, k);            /* settle glide */
        tape_screen_update(0, 0);
        uint32_t p0 = tpos, s0 = samples[0];
        int blocks_per_frame = 44100 / OP1_FX_BLOCK / fps[f];
        for (int fr = 0; fr < 3 * fps[f]; fr++) {                          /* 3 s */
            for (int b = 0; b < blocks_per_frame; b++) tape_render(0, buf, k);
            tape_screen_update(0, 0);
        }
        double secs = (samples[0] - s0) / 44100.0, px = (tpos - p0) / 1024.0;
        double delay = (st[0].d / 1024.0) / 44100.0;
        double travel = (TP_PLAY_X - TP_REC_X) / (px / secs);              /* rec->play time on screen */
        double err = fabs(travel - delay) / delay * 100;
        printf("time knob %5d  %2d fps: delay %.3f s, screen rec->play %.3f s  (%.2f %%)\n",
               times[t], fps[f], delay, travel, err);
        if (err > 1.0) fail = 1;
    }
    puts(fail ? "FAIL" : "OK: screen tape travel matches the DSP delay");
    return fail;
}
