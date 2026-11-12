/* perc_kit _kit.wav outputs through perc's decoder (host only, from plugins/percsyn):
 *   cc -O2 -DOP1_HOST -I. -I../.. tests/perc_kitwav_test.c perc_xfer.c -lm -o perc_kitwav_test && ./perc_kitwav_test *_kit.wav */
#include "perc_xfer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
static perc_xfer_t x;
int main(int argc, char **argv)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    int bad = 0;
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb"); fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 44, SEEK_SET);
        unsigned n = (unsigned)(len - 44) / 2; int16_t *s = malloc(n * 2); fread(s, 2, n, f); fclose(f);
        unsigned cut = n > 529152 ? 529152 : n;
        perc_xfer_begin(&x); perc_xfer_push(&x, s, cut); free(s);
        int both = 0; for (int i = 0; i < 16; i++) both += x.have[i] == 3;
        if (both != 16 || n > 529152) { bad++; printf("FAIL %s: %d both-sided, %.2f s\n", argv[a], both, n / 44100.0); }
    }
    printf("%d kit wavs, %d bad\n", argc - 1, bad);
    return bad != 0;
}
