/* Float prototype of the "licorice" model: FM clarinet (after Chowning / the classic DX clarinet patches).
 *
 *   proto_fm out.wav midi vel dur_s [key=value ...]
 *
 * Carrier A at f, phase-modulated by a sine at 2f: sidebands f +- 2kf = odd harmonics only (chalumeau).
 * Carrier B at f, modulated at f: all harmonics; mixed in for the even harmonics of the upper register.
 * Both indices follow the amplitude envelope raised to idx_k, so the tone brightens as the note builds up
 * (as the references do: upper harmonics arrive later). Output through a body EQ like the other prototypes.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define SR 44100.0
typedef struct { const char *k; double v; } kv_t;
static kv_t P[] = {
    {"i1_f", 1.5}, {"i1_p", 0.4}, {"i1_hi", 0.6},          /* odd-harmonic index at vel 127 / 0; factor in the clarion */
    {"i2_f", 0.5}, {"i2_p", 0.1}, {"i2_hi", 1.5},          /* all-harmonic index; factor in the clarion */
    {"mixb", 0.2}, {"mixb_hi", 0.6},                      /* level of carrier B (low / high register) */
    {"brk", 69.5}, {"islope", 0.0},                       /* index change per octave above 70 */
    {"vcurve", 1.0}, {"idx_k", 1.5},
    {"fb", 0.0},                                          /* modulator feedback */
    {"att", 0.03}, {"att_p", 0.1}, {"rel", 0.05}, {"noise", 0.002},
    {"eq_f", 0}, {"eq_g", 0}, {"eq_q", 1}, {"eq2_f", 0}, {"eq2_g", 0}, {"eq2_q", 1},
    {"olp", 0}, {"olp_q", 0.707}, {"tune", 0}, {"off", 0.0},
};
#define NP (sizeof P / sizeof P[0])
static double g(const char *k) { for (unsigned i = 0; i < NP; i++) if (!strcmp(P[i].k, k)) return P[i].v; fprintf(stderr, "no param %s\n", k); exit(2); }
typedef struct { double b0, b1, b2, a1, a2, z1, z2; } bq_t;
static double bq(bq_t *f, double x) { double y = f->b0 * x + f->z1; f->z1 = f->b1 * x - f->a1 * y + f->z2; f->z2 = f->b2 * x - f->a2 * y; return y; }
static void bq_peak(bq_t *f, double fc, double db, double q)
{
    double A = pow(10, db / 40), w = 2 * M_PI * fc / SR, al = sin(w) / (2 * q), c = cos(w), a0 = 1 + al / A;
    f->b0 = (1 + al * A) / a0; f->b1 = -2 * c / a0; f->b2 = (1 - al * A) / a0; f->a1 = -2 * c / a0; f->a2 = (1 - al / A) / a0;
}
static void bq_lp(bq_t *f, double fc, double q)
{
    double w = 2 * M_PI * fc / SR, al = sin(w) / (2 * q), c = cos(w), a0 = 1 + al;
    f->b0 = (1 - c) / 2 / a0; f->b1 = (1 - c) / a0; f->b2 = f->b0; f->a1 = -2 * c / a0; f->a2 = (1 - al) / a0;
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: proto_fm out.wav midi vel dur [k=v...]\n"); return 1; }
    const char *outp = argv[1]; double midi = atof(argv[2]), vel = atof(argv[3]), dur = atof(argv[4]);
    for (int i = 5; i < argc; i++) {
        char *e = strchr(argv[i], '='); if (!e) continue; *e = 0; int ok = 0;
        for (unsigned j = 0; j < NP; j++) if (!strcmp(P[j].k, argv[i])) { P[j].v = atof(e + 1); ok = 1; }
        if (!ok) { fprintf(stderr, "unknown param %s\n", argv[i]); return 2; }
    }
    int N = (int)(dur * SR); float *out = calloc(N, sizeof(float));
    double f0 = 440 * pow(2, (midi - 69) / 12.0) * pow(2, g("tune") / 1200), vn = vel / 127.0, oct = (midi - 70) / 12.0;
    int hi = midi >= g("brk");
    double dyn = pow(vn, g("vcurve")), isc = exp(g("islope") * oct);
    double i1 = (g("i1_p") + (g("i1_f") - g("i1_p")) * dyn) * isc * (hi ? g("i1_hi") : 1);
    double i2 = (g("i2_p") + (g("i2_f") - g("i2_p")) * dyn) * isc * (hi ? g("i2_hi") : 1);
    double mb = hi ? g("mixb_hi") : g("mixb"), ik = g("idx_k"), fb = g("fb"), noise = g("noise");
    double attt = g("att_p") + (g("att") - g("att_p")) * vn, att = exp(-1 / (attt * SR)), rel = exp(-1 / (g("rel") * SR));
    int noff = (int)((dur - (g("off") > 0 ? g("off") : 0.3)) * SR);
    bq_t eq1 = {0}, eq2 = {0}, olp = {0}; eq1.b0 = eq2.b0 = olp.b0 = 1;
    if (g("eq_f") > 0) bq_peak(&eq1, g("eq_f"), g("eq_g"), g("eq_q"));
    if (g("eq2_f") > 0) bq_peak(&eq2, g("eq2_f"), g("eq2_g"), g("eq2_q"));
    if (g("olp") > 0) bq_lp(&olp, g("olp"), g("olp_q"));
    double ph = 0, w = 2 * M_PI * f0 / SR, env = 0, m1 = 0, m2 = 0, nz = 0; unsigned seed = 999;
    for (int n = 0; n < N; n++) {
        double tgt = n < noff ? 1 : 0;
        env = tgt + (env - tgt) * (tgt > env ? att : rel);
        double ie = pow(env, ik);
        seed = seed * 1664525u + 1013904223u; nz = 0.8 * nz + 0.2 * ((int)(seed >> 8) / 8388608.0 - 1.0);
        m1 = sin(2 * ph + fb * m1); m2 = sin(ph + fb * m2);
        double a = sin(ph + i1 * ie * m1), b = sin(ph + i2 * ie * m2);
        double y = env * ((1 - mb) * a + mb * b) + noise * env * nz;
        ph += w; if (ph > 2 * M_PI) ph -= 2 * M_PI;
        out[n] = (float)bq(&olp, bq(&eq2, bq(&eq1, y)));
    }
    FILE *f = fopen(outp, "wb"); if (!f) return 3;
    unsigned dsz = N * 4, v32; unsigned short v16;
    fwrite("RIFF", 1, 4, f); v32 = 36 + dsz; fwrite(&v32, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v32 = 16; fwrite(&v32, 4, 1, f); v16 = 3; fwrite(&v16, 2, 1, f); v16 = 1; fwrite(&v16, 2, 1, f);
    v32 = 44100; fwrite(&v32, 4, 1, f); v32 = 44100 * 4; fwrite(&v32, 4, 1, f); v16 = 4; fwrite(&v16, 2, 1, f);
    v16 = 32; fwrite(&v16, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&dsz, 4, 1, f); fwrite(out, 4, N, f); fclose(f);
    return 0;
}
