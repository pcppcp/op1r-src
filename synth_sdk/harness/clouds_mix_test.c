/* Host test (op1r-src PR #40, dry end only): Clouds with MIX fully dry passes the input at the level of FX off (it was
 * -9 dB: the original's equal-power dry gain 0.707 and the halving before the soft limiter); MIX fully wet keeps its
 * level (the wet side is unchanged). 440 Hz sine, dense grains, RMS after 500 blocks relative to the input. */
#define CLOUDS_MAX_GRAINS 24
#define CLOUDS_LINEAR_READ
#define CLOUDS_SKIP_IDLE
#include "../plugins/clouds/clouds_dsp.c"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static double run(int amp, int dw)
{
    static clouds_t c; static uint8_t mem[CLOUDS_MEM_BYTES];
    clouds_init(&c, mem, CLOUDS_GRANULAR);
    clouds_params_t p = { 16384, 16384, 28000, 16384, dw, 0, 0, 0, 0, 0 };
    int16_t in[64], out[64]; double si = 0, so = 0; long ph = 0;
    for (int b = 0; b < 1000; b++) {
        for (int i = 0; i < 64; i++, ph++) in[i] = (int16_t)(amp * sin(ph * 2 * M_PI * 440 / 44100));
        clouds_process(&c, &p, in, out, 64);
        if (b >= 500) for (int i = 0; i < 64; i++) { si += (double)in[i] * in[i]; so += (double)out[i] * out[i]; }
    }
    return 10 * log10(so / si);
}
int main(void)
{
    for (int a = 0; a < 2; a++) {
        const int amp = a ? 8000 : 1000;
        const double d0 = run(amp, 0), d1 = run(amp, 32767);
        printf("amp %d: mix 0 %+.2f dB, mix 1 %+.2f dB\n", amp, d0, d1);
        assert(fabs(d0) < 0.2);                                  /* dry end: unity (was -9 dB) */
        assert(d1 > -3.0 && d1 < -1.0);                          /* wet end: as before (-1.8 / -1.9 dB here) */
    }
    assert(run(30000, 0) < 0.1);                                 /* full scale, dry: the limiter, no overshoot */
    printf("clouds_mix_test: ok\n");
    return 0;
}
