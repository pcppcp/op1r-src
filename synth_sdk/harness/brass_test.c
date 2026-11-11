/* Host test of plugins/brassophone/brass.c (the C loop; brass_dsp.S computes the same, bit for bit): the low-pass state
 * stays within int16 on low notes (the kernel's output multiply reads its low half), and a voice object left by
 * another engine (no note_on) is brought into range.
 *   cc -O1 -DOP1_HOST -fsanitize=address,undefined -I.. brass_test.c -lm -o build/brass_test */
#include "../op1_plugin.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6]; int16_t host_voice_env[128];
int16_t host_knobs[8][8]; int host_slot;
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
void *host_raster(void) { return 0; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c) { (void)r; (void)a; (void)b; (void)w; (void)c; }
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
#include "../plugins/brassophone/brass.c"

int main(void)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    for (int i = 0; i < 128; i++) host_voice_env[i] = 32767;
    host_voice_vel[0] = 127 << 8;
    void *v0 = OP1_VOICE(0);
    brass_voice_t *v = v0;
    int16_t out[128], k[8] = { 32767, 0, 0, 0, 0, 0, 0, 0 };   /* blue up, no sweep, no vibrato: a plain sine */
    long blocks = 0;
    for (double hz = 20; hz < 100; hz *= 1.05) {                   /* low notes: many samples near the trough */
        for (int i = 0; i < 128; i++) host_pitch_inc[i] = (int32_t)lrint(hz / OP1_VOICE_RATE * 4294967296.0);
        brass_note_on(v0, k);
        for (int b = 0; b < 2000; b++, blocks++) {
            brass_render(v0, out, k);
            assert(v->lp == (int16_t)v->lp);
        }
    }
    /* another engine's state in the voice object (engine switch, no note_on) */
    memset(v0, 0x7F, OP1_VOICE_SIZE);
    for (int b = 0; b < 4; b++) {
        brass_render(v0, out, k);
        assert(v->lp == (int16_t)v->lp && v->interp_start >= 0 && v->interp_start <= 32767);
        assert(v->prev_block_env >= 0 && v->prev_block_env <= 32767);
    }
    memset(v0, 0x80, OP1_VOICE_SIZE);
    brass_render(v0, out, k);
    assert(v->lp == (int16_t)v->lp && v->interp_start >= 0 && v->interp_start <= 32767);
    printf("brass_test: ok (%ld low-note blocks)\n", blocks);
    return 0;
}
