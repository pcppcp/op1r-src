/* perc under ASan / UBSan with random patches, knob offsets at and past their limits, velocities and keys (host only).
 *   cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DOP1_HOST -I.. -I../../.. perc_fuzz.c \
 *      ../perc_dsp.c -lm -o perc_fuzz && ./perc_fuzz [count]
 * Checks that every hit renders without undefined behaviour, keeps the output in range and ends. */
#include "perc_dsp.h"
#include "perc_tables.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_voice_env[128]; int16_t host_knobs[8][8]; int host_shift; int host_slot;
static int32_t pick(int32_t lo, int32_t hi) { int r = rand() % 8; if (r == 0) return lo; if (r == 1) return hi; return lo + (int32_t)((double)rand() / RAND_MAX * (hi - lo)); }
int main(int argc, char **argv)
{
    int count = argc > 1 ? atoi(argv[1]) : 2000;
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    srand(11);
    static perc_voice_t v; int16_t out[64]; long long longest = 0; int unended = 0;
    for (int n = 0; n < count; n++) {
        uint16_t p[21];
        for (int i = 0; i < 21; i++) { int r = rand() % 6; p[i] = (uint16_t)(r == 0 ? 0 : r == 1 ? 65535 : rand() & 0xffff); }
        if (n < PERC_FACTORY_COUNT * 20) for (int i = 0; i < 21; i++) p[i] = PERC_FACTORY[n % PERC_FACTORY_COUNT][i];
        perc_macro_t m = { pick(-131072, 131072), pick(-217706, 217706), pick(-65536, 65536), pick(-196608, 196608),
                           pick(0, 65534), pick(-786432, 786432) };
        perc_prepare(&v.p, p, &m, pick(0, 128)); perc_trigger(&v);
        int32_t key = pick(-6 * 65536, 4 * 65536);
        long long s = 0; int alive = 1;
        while (alive && s < 44100LL * 400) { alive = perc_render(&v, out, 64, key); s += 64; }
        if (alive && unended++ < 6) printf("  stuck: osc_env %d nenv %d e1 %d e2 %d osc_g %d noise_g %d atk %u natk %u lin_end %u nmode %d eq %d dist %d level %d dcy_c %d ndcy_c %d\n", v.osc_env, v.nenv, v.e1, v.e2, v.p.osc_g, v.p.noise_g, v.p.osc_atk_n, v.p.n_atk_n, v.p.lin_end_n, v.p.nenv_mode, v.p.use_eq, v.p.dist_mode, v.p.level, v.p.osc_dcy_c, v.p.n_dcy_c);
        if (s > longest) longest = s;
    }
    printf("%d hits rendered (factory sounds with random knobs, then random patches): longest %.1f s, %d not ended after 400 s\n",
           count, longest / 44100.0, unended);
    return unended != 0;
}
