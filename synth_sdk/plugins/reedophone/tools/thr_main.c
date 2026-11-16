#define main render_main
#include "render_notes.c"
#undef main
#include "../reedophone.c"
int main(void)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    int16_t k[8] = { 26214, 16384, 16384, 8192 };
    int ms[] = { 50, 53, 60, 65, 72, 80, 89 };
    for (int j = 0; j < 7; j++) {
        uint32_t inc = (uint32_t)lrint(4294967296.0 * 440.0 * pow(2, (ms[j] - 69) / 12.0) / 44100);
        rv_voice_t v = {0}; setup(&v, &st[0], inc, k);
        printf("midi %d: gth %.4f zeta %.4f lpc %d gq %d tau %.3f midi_q8 %.2f\n", ms[j], st[0].gth / 8192.0, st[0].zeta / 32768.0,
               st[0].lpc, st[0].gq15, st[0].tau / 65536.0, st[0].midi_q8 / 256.0);
    }
    return 0;
}
