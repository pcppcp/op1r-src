/* Host test tool: n voices played together (every voice renders once per block, as in the firmware's audio callback),
 * for the CPU guard (osc 3 off from OSC3_OFF_AT voices). Voice 0 plays C3, the others other notes.
 *   mm_poly <voices> <seconds> <setup word>  ->  voice 0's output (int16) on stdout,
 *   stderr: the guard flag per 0.1 s */
#include "../../../op1_plugin.h"
#include "../moai.c"
#include "../moai_ui.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24] __attribute__((aligned(4)));
int16_t host_voice_vel[6]; int16_t host_accel[3]; int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
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
    if (argc < 4) return 2;
    int nv = atoi(argv[1]); double secs = atof(argv[2]);
    int16_t k[8] = { (int16_t)atoi(argv[3]), 32767, 0, 0, 0x5301, 0, 0, 0 };   /* setup, cutoff open */
    static const int notes[6] = { 48, 52, 55, 59, 62, 64 };
    int16_t out[OP1_BLOCK];
    for (int i = 0; i < 8; i++) host_knobs[host_slot][i] = k[i];       /* the live words (else k reads as LFO offsets) */
    for (int v = 0; v < nv; v++) { host_voice_vel[v] = 100 << 8; ENGINE_moai.note_on(host_voice_mem + v * 0x24, k); }
    int blocks = (int)(secs * 44100 / OP1_BLOCK);
    for (int b = 0; b < blocks; b++) {
        for (int v = 0; v < nv; v++) {
            for (int i = 0; i < OP1_BLOCK; i++) host_pitch_inc[i] = (int32_t)lrint(4294967296.0 * 440.0 * pow(2, (notes[v] - 69) / 12.0) / 44100);
            ENGINE_moai.render(host_voice_mem + v * 0x24, out, k);
            if (v == 0) fwrite(out, 2, OP1_BLOCK, stdout);
        }
        if (b % 34 == 0) fprintf(stderr, "%d", mm_osc3_off);
    }
    fprintf(stderr, "\n");
    return 0;
}
