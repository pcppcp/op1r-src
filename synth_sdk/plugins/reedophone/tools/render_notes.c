/* Host renderer for the comparison tools: one note of a synth plugin -> float WAV, like the OP-1 plays it.
 *
 *   render_notes out.wav midi vel dur k0 k1 k2 k3 [attack_ms release_ms sustain(0..1) raw]
 *
 * Built with -DOP1_HOST -DENGINE=<symbol> and the plugin sources (clar_eval.py does this). The stock envelope is
 * simulated as attack (linear) / sustain / release (linear) scaled like the device's (peak = velocity x ~0.5, 16256
 * at velocity 127), handed to the engine in OP1_VOICE_ENV and then multiplied into the output, as the firmware does.
 * raw = 1 skips that multiply (engine output only). Note off at dur - 0.3 s. */
#include "op1_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6]; int16_t host_voice_env[128]; int16_t host_accel[3] = { 0, 0, 16384 };
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
uint8_t *host_node_style_rgba(void *s, const char *id) { static uint8_t c[4]; (void)s; (void)id; return c; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
void *host_raster(void) { return 0; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c) { (void)r; (void)a; (void)b; (void)w; (void)c; }
void host_raster_dim(void *r) { (void)r; }
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t s, const uint8_t *c) { (void)x; (void)y; (void)s; (void)c; return o; }
void host_text_set(void *t, const char *s) { (void)t; (void)s; }
void host_text_draw(void *t, void *r, const int32_t *m) { (void)t; (void)r; (void)m; }
void *read_fx_param_safe(int slot, int pos) { (void)slot; (void)pos; return 0; }

extern const op1_engine_t ENGINE;

int main(int argc, char **argv)
{
    if (argc < 9) { fprintf(stderr, "usage: render_notes out.wav midi vel dur k0 k1 k2 k3 [att_ms rel_ms sus raw]\n"); return 1; }
    const char *path = argv[1]; double midi = atof(argv[2]); int vel = atoi(argv[3]); double dur = atof(argv[4]);
    int16_t k[8] = { 0, 0, 0, 0, 0x5301, 0, 0, 0 };
    for (int i = 0; i < 4; i++) k[i] = (int16_t)atoi(argv[5 + i]);
    double att_ms = argc > 9 ? atof(argv[9]) : 5, rel_ms = argc > 10 ? atof(argv[10]) : 150, sus = argc > 11 ? atof(argv[11]) : 1.0;
    int raw = argc > 12 ? atoi(argv[12]) : 0;
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    int32_t inc = (int32_t)lrint(4294967296.0 * 440.0 * pow(2, (midi - 69) / 12.0) / 44100);
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = inc;
    host_voice_vel[0] = (int16_t)(vel << 8);
    int nb = (int)(dur * 44100 / 128), off = (int)((dur - 0.3) * 44100 / 128);
    float *o = malloc(sizeof(float) * nb * 128); int16_t out[128];
    double peak = vel * 16256.0 / 127, e = 0, att_step = peak / (att_ms * 44.1 + 1), rel_step = peak * sus / (rel_ms * 44.1 + 1);
    ENGINE.note_on(host_voice_mem, k);
    for (int b = 0; b < nb; b++) {
        if (b == off && ENGINE.note_off) ENGINE.note_off(host_voice_mem);
        int16_t ev[128];
        for (int i = 0; i < 128; i++) {
            int n = b * 128 + i;
            if (n < off * 128) { e += att_step; if (e > peak) e = peak; if (n > att_ms * 44.1 && e > peak * sus) e = peak * sus; }
            else { e -= rel_step; if (e < 0) e = 0; }
            ev[i] = (int16_t)e; host_voice_env[i] = ev[i];
        }
        ENGINE.render(host_voice_mem, out, k);
        for (int i = 0; i < 128; i++) o[b * 128 + i] = raw ? out[i] / 32768.0f : (float)(out[i] * (double)ev[i] / 32768.0 / 32768.0);
    }
    FILE *f = fopen(path, "wb"); if (!f) return 3;
    unsigned N = (unsigned)nb * 128, dsz = N * 4, v32; unsigned short v16;
    fwrite("RIFF", 1, 4, f); v32 = 36 + dsz; fwrite(&v32, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v32 = 16; fwrite(&v32, 4, 1, f); v16 = 3; fwrite(&v16, 2, 1, f); v16 = 1; fwrite(&v16, 2, 1, f);
    v32 = 44100; fwrite(&v32, 4, 1, f); v32 = 44100 * 4; fwrite(&v32, 4, 1, f); v16 = 4; fwrite(&v16, 2, 1, f);
    v16 = 32; fwrite(&v16, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&dsz, 4, 1, f); fwrite(o, 4, N, f); fclose(f);
    return 0;
}
