/* perc against libpo32's float reference renderer (host only).
 *   cc -O2 -DOP1_HOST -I.. -I../../.. -I$LIBPO32/core/include -I$LIBPO32/core/src perc_ref.c ../perc_dsp.c \
 *      $LIBPO32/core/src/po32_synth.c -lm -o perc_ref && ./perc_ref [outdir] [random-count]
 * For each factory sound and for random patches: the RMS envelope (10 ms windows) and octave-band spectrum of perc
 * and of po32_synth_render at 44.1 kHz, velocity 127 and 64. Writes perc_<name>.wav / ref_<name>.wav for the
 * factory sounds. Not a bit comparison: perc is an independent integer implementation (noise differs). */
#include "perc_dsp.h"
#include "perc_tables.h"
#include "po32_synth.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_voice_env[128]; int16_t host_knobs[8][8]; int host_shift; int host_slot;

#define FS 44100
#define LEN (FS * 3)
static float ref[LEN], mine[LEN];

static void wav(const char *path, const float *x, int n)
{
    FILE *f = fopen(path, "wb"); if (!f) return;
    int bytes = n * 2; int32_t hdr[11] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00010001, FS, FS * 2, 0x00100002, 0x61746164, bytes };
    fwrite(hdr, 4, 11, f);
    for (int i = 0; i < n; i++) { float v = x[i] * 32767; int16_t s = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); fwrite(&s, 2, 1, f); }
    fclose(f);
}
static int render_perc(const uint16_t *p, int vel, float *out)
{
    static perc_voice_t v; perc_macro_t m; memset(&m, 0, sizeof m);
    perc_prepare(&v.p, p, &m, vel); perc_trigger(&v);
    int16_t b[64]; int n = 0;
    while (n < LEN) { int alive = perc_render(&v, b, 64, 0); for (int i = 0; i < 64 && n < LEN; i++) out[n++] = b[i] / 32768.0f; if (!alive) break; }
    for (; n < LEN; n++) out[n] = 0;
    return 0;
}
static void render_ref(const uint16_t *p, int vel, float *out)
{
    po32_synth_t s; po32_synth_init(&s, FS);
    po32_patch_params_t pp; float *f = (float *)&pp;
    for (int i = 0; i < 21; i++) f[i] = p[i] / 65535.0f;
    size_t len = 0;
    memset(out, 0, sizeof(float) * LEN);
    po32_synth_render(&s, &pp, vel, (float)LEN / FS, out, LEN, &len);
}
/* RMS envelope in dB per 10 ms (441 samples) */
static int env_db(const float *x, double *e)
{
    int w = 0;
    for (int i = 0; i + 441 <= LEN; i += 441, w++) {
        double a = 0; for (int j = 0; j < 441; j++) a += (double)x[i + j] * x[i + j];
        e[w] = 10 * log10(a / 441 + 1e-12);
    }
    return w;
}
/* octave band energies (dB) of the first 0.5 s with a direct DFT on a log-spaced set of frequencies */
static void bands(const float *x, double *b)
{
    static const double fc[9] = { 40, 80, 160, 320, 640, 1280, 2560, 5120, 10240 };
    int n = FS / 2;
    for (int k = 0; k < 9; k++) {
        double acc = 0;
        for (int s = 0; s < 6; s++) {                    /* 6 frequencies inside each octave */
            double f = fc[k] * pow(2, s / 6.0), re = 0, im = 0, w = 2 * M_PI * f / FS;
            for (int i = 0; i < n; i++) { re += x[i] * cos(w * i); im += x[i] * sin(w * i); }
            acc += re * re + im * im;
        }
        b[k] = 10 * log10(acc + 1e-9);
    }
}
typedef struct { double env_max, env_mean, band_max, peak_ref, peak_mine; } cmp_t;
static cmp_t compare(void)
{
    static double er[400], em[400]; double br[9], bm[9]; cmp_t c = { 0, 0, 0, 0, 0 };
    int w = env_db(ref, er); env_db(mine, em);
    int cnt = 0;
    for (int i = 0; i < w; i++) if (er[i] > -50) { double d = fabs(er[i] - em[i]); if (d > c.env_max) c.env_max = d; c.env_mean += d; cnt++; }
    c.env_mean = cnt ? c.env_mean / cnt : 0;
    bands(ref, br); bands(mine, bm);
    double top = -1e9; for (int k = 0; k < 9; k++) if (br[k] > top) top = br[k];
    for (int k = 0; k < 9; k++) if (br[k] > top - 30) { double d = fabs(br[k] - bm[k]); if (d > c.band_max) c.band_max = d; }
    for (int i = 0; i < LEN; i++) { if (fabs(ref[i]) > c.peak_ref) c.peak_ref = fabs(ref[i]); if (fabs(mine[i]) > c.peak_mine) c.peak_mine = fabs(mine[i]); }
    return c;
}
int main(int argc, char **argv)
{
    if (argc == 23 && !strcmp(argv[1], "dbg")) {           /* dbg p0 .. p20: both envelopes side by side */
        for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
        uint16_t p[21]; for (int i = 0; i < 21; i++) p[i] = (uint16_t)atoi(argv[2 + i]);
        render_ref(p, 127, ref); render_perc(p, 127, mine);
        static double er[400], em[400]; int w = env_db(ref, er); env_db(mine, em);
        for (int i = 0; i < w; i += 5) printf("%4d ms  ref %7.1f  perc %7.1f\n", i * 10, er[i], em[i]);
        double br[9], bm[9]; bands(ref, br); bands(mine, bm);
        for (int k = 0; k < 9; k++) printf("band %d  ref %6.1f  perc %6.1f\n", k, br[k], bm[k]);
        wav("dbg_ref.wav", ref, LEN); wav("dbg_perc.wav", mine, LEN);
        return 0;
    }
    const char *dir = argc > 1 ? argv[1] : ".";
    int nrand = argc > 2 ? atoi(argv[2]) : 200;
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    printf("%-9s %3s  %8s %8s %8s  %7s %7s\n", "sound", "vel", "env max", "env avg", "band max", "pk ref", "pk perc");
    for (int s = 0; s < PERC_FACTORY_COUNT; s++)
        for (int vi = 0; vi < 2; vi++) {
            int vel = vi ? 64 : 127;
            render_ref(PERC_FACTORY[s], vel, ref); render_perc(PERC_FACTORY[s], vel, mine);
            cmp_t c = compare();
            printf("%-9s %3d  %6.2fdB %6.2fdB %6.2fdB  %7.3f %7.3f\n", PERC_FACTORY_NAME[s], vel, c.env_max, c.env_mean, c.band_max, c.peak_ref, c.peak_mine);
            if (!vi) { char p[512]; snprintf(p, sizeof p, "%s/ref_%s.wav", dir, PERC_FACTORY_NAME[s]); wav(p, ref, FS * 2);
                       snprintf(p, sizeof p, "%s/perc_%s.wav", dir, PERC_FACTORY_NAME[s]); wav(p, mine, FS * 2); }
        }
    /* random patches: distribution of the differences */
    srand(7); int over_env = 0, over_band = 0; int by_mode[3][2] = {{0}}; double worst = 0; uint16_t wp[21]; int worst_i = -1;
    static double emeans[10000];
    for (int r = 0; r < nrand; r++) {
        uint16_t p[21]; for (int i = 0; i < 21; i++) p[i] = (uint16_t)(rand() & 0xffff);
        p[P_LEVEL] = (uint16_t)(40000 + rand() % 20000);
        render_ref(p, 127, ref); render_perc(p, 127, mine);
        cmp_t c = compare(); emeans[r] = c.env_mean;
        if (c.env_mean > 3) { over_env++; perc_prep_t o; perc_macro_t m; memset(&m, 0, sizeof m); perc_prepare(&o, p, &m, 127);
            printf("  #%d env %.1f band %.1f: wave %d mod %d nf %d nenv %d dist %d eq %d | Q %u lvl %u eqg %u mix %u pk %.2f/%.2f\n", r, c.env_mean, c.band_max, o.wave, o.mod_mode, o.nf_mode, o.nenv_mode, o.dist_mode, o.use_eq, p[P_NFILQ], p[P_LEVEL], p[P_EQGAIN], p[P_MIX], c.peak_ref, c.peak_mine);
            printf("    patch:"); for (int i = 0; i < 21; i++) printf(" %u", p[i]); printf("\n"); }
        { perc_prep_t o; perc_macro_t mm; memset(&mm, 0, sizeof mm); perc_prepare(&o, p, &mm, 127); by_mode[o.mod_mode][0]++; if (c.band_max > 6) by_mode[o.mod_mode][1]++; }
        if (c.band_max > 6) over_band++;
        if (c.env_mean > worst) { worst = c.env_mean; memcpy(wp, p, sizeof wp); worst_i = r; }
    }
    printf("band diff > 6 dB by pitch mod mode: decay %d/%d, sine %d/%d, noise %d/%d\n", by_mode[0][1], by_mode[0][0], by_mode[1][1], by_mode[1][0], by_mode[2][1], by_mode[2][0]);
    printf("random %d patches: mean envelope diff > 3 dB: %d, band diff > 6 dB: %d, worst mean env %.1f dB (#%d)\n", nrand, over_env, over_band, worst, worst_i);
    if (worst_i >= 0) { printf("worst patch:"); for (int i = 0; i < 21; i++) printf(" %u", wp[i]); printf("\n"); }
    return 0;
}
