/* Host test tool: render one moai note to raw int16 mono 44.1 kHz on stdout.
 *   mm_dump <midi> <k0> <k1> <k2> <k3> <w5> <w6> <w7> <seconds> <release_at_s> [velocity 1..127] [bend_cents_per_s] [id:value,...]
 * k0..k3 = home knobs (setup, cutoff, emphasis, contour), w5..w7 = detune, drive, drift; the last argument sets
 * session parameters (P[], see moai.h) after the first block, as the views would
 * Built by test/run.sh with the real plugin source (-DOP1_HOST) and sanitizers. */
#include "../../../op1_plugin.h"
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
#include "../moai.c"
#include "../moai_ui.c"
int main(int argc, char **argv)
{
    if (argc < 11) { fprintf(stderr, "usage\n"); return 2; }
    double midi = atof(argv[1]);
    int16_t k[8] = { (int16_t)atoi(argv[2]), (int16_t)atoi(argv[3]), (int16_t)atoi(argv[4]), (int16_t)atoi(argv[5]),
                     0x5301, (int16_t)atoi(argv[6]), (int16_t)atoi(argv[7]), (int16_t)atoi(argv[8]) };
    double secs = atof(argv[9]), rel = atof(argv[10]);
    int vel = argc > 11 ? atoi(argv[11]) : 127;
    double bend = argc > 12 ? atof(argv[12]) : 0;
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    host_voice_vel[0] = (int16_t)(vel << 8);
    int blocks = (int)(secs * 44100 / OP1_BLOCK), relb = (int)(rel * 44100 / OP1_BLOCK);
    int16_t out[OP1_BLOCK];
    for (int i = 0; i < 8; i++) host_knobs[host_slot][i] = k[i];       /* the live words (else k reads as LFO offsets) */
    ENGINE_moai.note_on(host_voice_mem, k);
    const char *ov = argc > 13 ? argv[13] : 0;
    for (int b = 0; b < blocks; b++) {
        if (b == 1 && ov) for (const char *t = ov; *t; ) {          /* session overrides (after the first derivation) */
            int id = atoi(t); while (*t && *t != ':') t++; if (!*t) break;
            int val = atoi(++t); while (*t && *t != ',') t++; if (*t) t++;
            if (id >= 0 && id < P_N) P[id] = (int16_t)pclamp(id, val);
        }
        if (b == relb) ENGINE_moai.note_off(host_voice_mem);
        for (int i = 0; i < OP1_BLOCK; i++) {
            double m = midi + bend / 100.0 * ((b * OP1_BLOCK + i) / 44100.0) ;
            host_pitch_inc[i] = (int32_t)lrint(4294967296.0 * 440.0 * pow(2, (m - 69) / 12.0) / 44100);
        }
        ENGINE_moai.render(host_voice_mem, out, k);
        fwrite(out, 2, OP1_BLOCK, stdout);
    }
    return 0;
}
