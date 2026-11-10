/* Host test tool: oscillator 1 alone (the real wave_at / wave_of), int16 mono on stdout.
 *   mm_osc <midi> <wave id 0..6: W_TRI .. W_NARROW> <seconds> <blep 1|0>   blep 0 = naive (no edge correction), for comparison */
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
    if (argc < 5) return 2;
    double midi = atof(argv[1]); int wk = atoi(argv[2]); long n = (long)(atof(argv[3]) * 44100); int blep = atoi(argv[4]);
    uint32_t inc = (uint32_t)lrint(4294967296.0 * 440.0 * pow(2, (midi - 69) / 12.0) / 44100);
    uint32_t r = 0x40000000u / ((inc >> 8) | 1), p = 0;
    wave_t w; wave_of(&w, wk);
    for (long i = 0; i < n; i++) {
        p += inc;
        int32_t s;
        if (blep) s = wave_at(&w, p, inc, r);
        else s = wave_raw(&w, p) >> 1;              /* naive: the same shapes without the edge correction */
        int16_t o = op1_sat16(s); fwrite(&o, 2, 1, stdout);
    }
    return 0;
}
