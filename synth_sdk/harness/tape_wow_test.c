/* Host test: measure the pitch modulation that the wow knob puts on the echo.
 * Feeds a 1 kHz sine, isolates the wet signal (out - in, feedback 0, age 0), measures the period of every
 * cycle from zero crossings and reports the peak pitch deviation in % and cents per wow setting.
 *   cc -DOP1_HOST -I.. tape_wow_test.c -lm -o build/tape_wow_test && build/tape_wow_test */
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

#define SECS 6
static float wet[44100 * SECS];

int main(void)
{
    static int32_t scratch[44115]; host_fx_scratch[0] = scratch;
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    const int16_t wows[] = { 0, 8192, 16384, 24576, 32767 };
    for (int w = 0; w < 5; w++) {
        int16_t k[8] = { 0, 0, 12000, wows[w], OP1_ENGINE_MAGIC + 1 };     /* age 0, fb 0 */
        tape_init(0);
        int16_t buf[OP1_FX_BLOCK]; double ph = 0; int n = 0;
        for (int b = 0; b < 44100 * (SECS + 1) / OP1_FX_BLOCK; b++) {
            int16_t in[OP1_FX_BLOCK];
            for (int i = 0; i < OP1_FX_BLOCK; i++) { in[i] = buf[i] = (int16_t)(12000 * sin(ph)); ph += 2 * M_PI * 1000 / 44100; }
            tape_render(0, buf, k);
            if (b * OP1_FX_BLOCK >= 44100)                                  /* skip 1 s settle */
                for (int i = 0; i < OP1_FX_BLOCK && n < 44100 * SECS; i++) wet[n++] = buf[i] - in[i];
        }
        double last = -1, pmin = 1e9, pmax = 0;
        for (int i = 1; i < n; i++)
            if (wet[i - 1] < 0 && wet[i] >= 0) {
                double z = i - 1 + wet[i - 1] / (wet[i - 1] - wet[i]);
                if (last >= 0) { double p = z - last; if (p < pmin) pmin = p; if (p > pmax) pmax = p; }
                last = z;
            }
        double dev = (pmax - pmin) / (pmax + pmin) * 100;                  /* +- % */
        printf("wow knob %5d: pitch +-%.3f %%  (+-%.1f cents)\n", wows[w], dev, 1200 * log2(1 + dev / 100));
    }
    return 0;
}
