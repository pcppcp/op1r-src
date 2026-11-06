/* Host test: tape FX page 2 (mix, heads, sync) from impulse responses (feedback 0, age 0, wow 0).
 * Unset words (0, sounds before page 2) must sound as before: dry 1 + echo 0.7 at the time knob's delay. */
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

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
#define N 70000
static int16_t out[N];

/* impulse (amp 16000) at sample 64 through the tape with words w5..w7; time knob k2 */
static void impulse(int16_t k2, int16_t mix, int16_t heads, int16_t sync)   /* words: 5 heads, 6 mix, 7 sync step */
{
    int16_t k[8] = { 0, 0, k2, 0, OP1_ENGINE_MAGIC + 1, heads, mix, sync };
    tape_init(0);
    for (int b = 0; b < N / OP1_FX_BLOCK; b++) {
        int16_t buf[OP1_FX_BLOCK];
        for (int i = 0; i < OP1_FX_BLOCK; i++) buf[i] = b * OP1_FX_BLOCK + i == 64 ? 16000 : 0;
        tape_render(0, buf, k);
        for (int i = 0; i < OP1_FX_BLOCK; i++) out[b * OP1_FX_BLOCK + i] = buf[i];
    }
}
/* echo energy around sample at (+-40, after the age low-pass: sum of |x|) */
static double echo(int at) { double e = 0; for (int i = at - 40; i < at + 40; i++) if (i > 100 && i < N) e += abs(out[i]); return e; }

int main(void)
{
    static int32_t scratch[44115]; host_fx_scratch[0] = scratch;
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    int16_t k2 = 12000;
    int D = DMIN + (int)(((uint32_t)k2 * (DMAX - DMIN)) >> 15) + 64;   /* echo position */

    impulse(k2, 0, 0, 0);                                          /* unset = before page 2 */
    double e_old = echo(D);
    CHECK(out[64] == 16000, "unset: dry full (%d)", out[64]);
    CHECK(e_old > 1000 && echo(D / 2 + 32) < e_old * 0.02, "unset: one echo at the delay (%.0f)", e_old);
    impulse(k2, 16384, 0, 0);
    CHECK(fabs(echo(D) - e_old) < e_old * 0.02 && out[64] == 16000, "mix 50 %% = the old sound");
    impulse(k2, 1, 0, 0);
    CHECK(out[64] == 16000 && echo(D) < e_old * 0.01, "mix 0 %%: dry only (echo %.0f)", echo(D));
    impulse(k2, 32767, 0, 0);
    CHECK(abs(out[64]) < 50 && fabs(echo(D) - e_old) < e_old * 0.03, "mix 100 %%: wet only (dry %d, echo %.0f)", out[64], echo(D));

    impulse(k2, 16384, 15000, 0);                                  /* 3 heads: 1/3, 2/3, 1, 0.6 each */
    int d0 = D - 64;
    double a = echo(64 + d0 / 3), b = echo(64 + 2 * d0 / 3), c = echo(D);
    CHECK(a > e_old * 0.5 && b > e_old * 0.5 && c > e_old * 0.5 && c < e_old * 0.7, "3 heads: echoes at 1/3, 2/3, 1 (%.0f %.0f %.0f of %.0f)", a, b, c, e_old);
    impulse(k2, 16384, 7000, 0);                                   /* 2 heads: 1/2, 1 */
    CHECK(echo(64 + d0 / 2) > e_old * 0.6 && echo(64 + d0 / 3) < e_old * 0.05, "2 heads: echo at 1/2");

    op1_beat_t *bc = host_beat(); bc->len = 22050 << 14; bc->next = 0; bc->step = 0x100000;   /* 120 BPM */
    impulse(k2, 16384, 0, 7);                                      /* sync step 7 = 1/4 = 22050 samples */
    int s4 = 64 + 22050;
    CHECK(echo(s4) > e_old * 0.8 && echo(D) < e_old * 0.05, "sync 1/4 at 120 BPM: echo at 22050 samples (%.0f)", echo(s4));
    impulse(k2, 16384, 0, 9);                                      /* 9 = 1/2 = 44100 */
    CHECK(echo(64 + 44100) > e_old * 0.8, "sync 1/2 at 120 BPM: echo at 44100 samples");
    bc->len = 0;
    impulse(k2, 16384, 0, 7);
    CHECK(echo(D) > e_old * 0.8, "sync without a clock: free time");

    printf(fails ? "%d failed\n" : "tape page 2: all ok\n", fails);
    return fails != 0;
}
