/* Host test tool (white box): internal contour level and cutoff position of voice 0 after every block, as text.
 *   mm_env <midi> <cutoff> <amount> <attack> <decay> <sustain> <release> <seconds> <release_at_s> [velocity] [contour knob]
 * the contour values (0..32767) are set as the contour view would (after the first block), or with a contour knob
 * value (11th argument >= 0) the values derived from it. Prints: t_ms env(0..1) pos_oct(octaves above 20 Hz) */
#include "../../../op1_plugin.h"
#include "../moai.c"
#include "../moai_ui.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6]; int16_t host_accel[3];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
void *host_raster(void) { return 0; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c) { (void)r; (void)a; (void)b; (void)w; (void)c; }
void *read_fx_param_safe(int slot, int pos) { (void)slot; (void)pos; return 0; }
int main(int argc, char **argv)
{
    if (argc < 10) return 2;
    double midi = atof(argv[1]);
    int macro = argc > 11 ? atoi(argv[11]) : -1;
    int16_t k[8] = { 512, (int16_t)atoi(argv[2]), 0, (int16_t)(macro >= 0 ? macro : 0), 0x5301, 0, 0, 0 };
    int16_t ov[5] = { (int16_t)atoi(argv[3]), (int16_t)atoi(argv[4]), (int16_t)atoi(argv[5]), (int16_t)atoi(argv[6]), (int16_t)atoi(argv[7]) };
    double secs = atof(argv[8]), rel = atof(argv[9]); int vel = argc > 10 ? atoi(argv[10]) : 127;
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = (int32_t)lrint(4294967296.0 * 440.0 * pow(2, (midi - 69) / 12.0) / 44100);
    host_voice_vel[0] = (int16_t)(vel << 8);
    int16_t out[OP1_BLOCK];
    ENGINE_moai.render(host_voice_mem, out, k);           /* derive the knob's values first, then the overrides */
    if (macro < 0) for (int i = 0; i < 5; i++) P[P_AMOUNT + i] = ov[i];
    for (int i = 0; i < 8; i++) host_knobs[host_slot][i] = k[i];       /* the live words (else k reads as LFO offsets) */
    ENGINE_moai.note_on(host_voice_mem, k);
    for (int b = 0; b < (int)(secs * 44100 / OP1_BLOCK); b++) {
        if (b == (int)(rel * 44100 / OP1_BLOCK)) ENGINE_moai.note_off(host_voice_mem);
        ENGINE_moai.render(host_voice_mem, out, k);
        printf("%.2f %.5f %.4f\n", (b + 1) * OP1_BLOCK * 1000.0 / 44100, V[0].env / 4194304.0, V[0].pos / 1024.0);
    }
    return 0;
}
