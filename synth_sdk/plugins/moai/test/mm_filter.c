/* Host test tool: the ladder of moai.c alone, int16 mono on stdout (output y4 scaled x4 = 16 bit).
 *   mm_filter noise <hz> <emphasis 0..1.1> <n> <amp>   white noise (+-amp of F), linear small-signal response
 *   mm_filter ring  <hz> <emphasis 0..1.1> <n> <amp>   one impulse (amp), then silence: ringing / self-oscillation
 *   mm_filter step  <hz> <emphasis 0..1.1> <n> <amp>   constant input amp (DC gain)
 * emphasis 1.0 = feedback gain 4 (self-oscillation threshold). Everything goes through the real lad_step / tables (-DOP1_HOST, sanitizers). */
#include "../../../op1_plugin.h"
#include "../moai.c"
#include "../moai_ui.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    if (argc < 6) return 2;
    const char *mode = argv[1]; double hz = atof(argv[2]), emp = atof(argv[3]); long n = atol(argv[4]); int amp = atoi(argv[5]);
    int32_t pos = (int32_t)lrint(log2(hz / 20.0) * 1024);
    if (pos > MAX_POS) pos = MAX_POS;
    int32_t gg = lerp_tab(MM_G, pos);
    int32_t k = (int32_t)lrint(4096.0 * 4 * emp);              /* emphasis 1.0 = feedback 4 = self-oscillation */
    lad_coef_t lc; lad_coef(&lc, gg, k);
    fprintf(stderr, "pos %d gg %d k %.3f inv %d\n", pos, gg, k / 4096.0, lc.invk & 0xFFFF);
    lad_t l = { 0, 0, 0, 0, { 0, 0, 0, 0 } };
    uint32_t rng = 12345; int32_t smax = 0;
    for (long i = 0; i < n; i++) {
        int32_t x = 0;
        if (!strcmp(mode, "noise")) { rng = rng * 1664525u + 1013904223u; x = (int32_t)(((rng >> 16) & 0xFFFF)) - 32768; x = x * amp / 32768; }
        else if (!strcmp(mode, "ring")) x = i == 0 ? amp : 0;
        else x = amp;
        int32_t y = lad_step(&l, &lc, x * 65536, l.t, (lc.ggk >> 16) != 0, 0) >> 16;            /* Q16 in / out */
        int32_t m[4] = { l.s1, l.s2, l.s3, l.s4 };
        for (int j = 0; j < 4; j++) { int32_t a = m[j] < 0 ? -m[j] : m[j]; if (a > smax) smax = a; }
        int16_t o = op1_sat16(y * 32768 / F); fwrite(&o, 2, 1, stdout);
    }
    fprintf(stderr, "max |state| %d\n", smax);
    return 0;
}
