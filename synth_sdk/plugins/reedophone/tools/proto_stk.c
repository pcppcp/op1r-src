/* Float prototype of the "chalumeau" model: the classic waveguide clarinet (Smith 1986, Cook's STK Clarinet).
 *
 *   proto_stk out.wav midi vel dur_s [key=value ...]
 *
 * A memoryless reed: the pressure difference across the reed, dp = -(bore return) - breath, is reflected with a
 * coefficient from a clipped linear "reed table" r = clamp(offset + slope * dp, -1, 1); the wave sent into the
 * bore is breath + r * dp. The bore is one delay line (round trip = half a period) with a low-pass reflection.
 * Output = the bore wave through a radiation high-pass and a body EQ (as in proto.c).
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
    {"b_f", 0.9}, {"b_p", 0.5}, {"vcurve", 1.0}, {"bslope", 0.0},   /* breath at vel 127 / 0, curve, per octave */
    {"offset", 0.7}, {"slope", -0.3},                               /* reed table */
    {"fc", 3000}, {"loss", 0.05}, {"loss_k", 0.5},                  /* bore reflection low-pass and loss */
    {"att", 0.02}, {"att_p", 0.06}, {"rel", 0.05},
    {"noise", 0.02},
    {"rad_hp", 300}, {"rad_mix", 1.0},
    {"eq_f", 0}, {"eq_g", 0}, {"eq_q", 1}, {"eq2_f", 0}, {"eq2_g", 0}, {"eq2_q", 1},
    {"olp", 0}, {"olp_q", 0.707},
    {"tune", 0}, {"off", 0.0},
};
#define NP (sizeof P / sizeof P[0])
static double g(const char *k) { for (unsigned i = 0; i < NP; i++) if (!strcmp(P[i].k, k)) return P[i].v; fprintf(stderr, "no param %s\n", k); exit(2); }

#define DL 16384
typedef struct { double b[DL]; int w; } line_t;
static void lpush(line_t *l, double x) { l->w = (l->w + 1) & (DL - 1); l->b[l->w] = x; }
static double lread(const line_t *l, double d)
{
    int i = (int)d; double f = d - i;
    double xm1 = l->b[(l->w - i + 1) & (DL - 1)], x0 = l->b[(l->w - i) & (DL - 1)];
    double x1 = l->b[(l->w - i - 1) & (DL - 1)], x2 = l->b[(l->w - i - 2) & (DL - 1)];
    double c0 = -f * (f - 1) * (f - 2) / 6, c1 = (f + 1) * (f - 1) * (f - 2) / 2, c2 = -(f + 1) * f * (f - 2) / 2, c3 = (f + 1) * f * (f - 1) / 6;
    return c0 * xm1 + c1 * x0 + c2 * x1 + c3 * x2;
}
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
    if (argc < 5) { fprintf(stderr, "usage: proto_stk out.wav midi vel dur [k=v...]\n"); return 1; }
    const char *outp = argv[1]; double midi = atof(argv[2]), vel = atof(argv[3]), dur = atof(argv[4]);
    for (int i = 5; i < argc; i++) {
        char *e = strchr(argv[i], '='); if (!e) continue; *e = 0; int ok = 0;
        for (unsigned j = 0; j < NP; j++) if (!strcmp(P[j].k, argv[i])) { P[j].v = atof(e + 1); ok = 1; }
        if (!ok) { fprintf(stderr, "unknown param %s\n", argv[i]); return 2; }
    }
    int N = (int)(dur * SR); float *out = calloc(N, sizeof(float));
    double f0 = 440 * pow(2, (midi - 69) / 12.0) * pow(2, g("tune") / 1200), vn = vel / 127.0, oct = (midi - 70) / 12.0;
    double bt = g("b_p") + (g("b_f") - g("b_p")) * pow(vn, g("vcurve")) + g("bslope") * oct;
    double a = exp(-2 * M_PI * g("fc") / SR), lpz = 0, loss = 1 - g("loss") * pow(f0 / 261.6, g("loss_k"));
    double w0 = 2 * M_PI * f0 / SR, tau = atan2(a * sin(w0), 1 - a * cos(w0)) / w0;
    double D = 0.5 * SR / f0 - tau - 1; if (D < 2) D = 2;
    static line_t bore;
    double off = g("offset"), sl = g("slope"), noise = g("noise");
    double attt = g("att_p") + (g("att") - g("att_p")) * vn, att = exp(-1 / (attt * SR)), rel = exp(-1 / (g("rel") * SR));
    int noff = (int)((dur - (g("off") > 0 ? g("off") : 0.3)) * SR);
    bq_t eq1 = {0}, eq2 = {0}, olp = {0}; eq1.b0 = eq2.b0 = olp.b0 = 1;
    if (g("eq_f") > 0) bq_peak(&eq1, g("eq_f"), g("eq_g"), g("eq_q"));
    if (g("eq2_f") > 0) bq_peak(&eq2, g("eq2_f"), g("eq2_g"), g("eq2_q"));
    if (g("olp") > 0) bq_lp(&olp, g("olp"), g("olp_q"));
    double hpa = exp(-2 * M_PI * g("rad_hp") / SR), hx = 0, hy = 0, dcx = 0, dcy = 0, br = 0, nz = 0, radmix = g("rad_mix");
    unsigned seed = 12345;
    for (int n = 0; n < N; n++) {
        double tgt = n < noff ? bt : 0;
        br = tgt + (br - tgt) * (tgt > br ? att : rel);
        seed = seed * 1664525u + 1013904223u; nz = 0.7 * nz + 0.3 * ((int)(seed >> 8) / 8388608.0 - 1.0);
        double breath = br * (1 + noise * nz);
        double ret = lread(&bore, D);                    /* wave returning from the bell */
        lpz = (1 - a) * ret + a * lpz;
        double dp = -loss * lpz - breath;
        double r = off + sl * dp; if (r > 1) r = 1; if (r < -1) r = -1;
        double sent = breath + r * dp;
        lpush(&bore, sent);
        double tx = ret;                                 /* what reaches the bell */
        hy = hpa * (hy + tx - hx); hx = tx;
        double y = radmix * hy + (1 - radmix) * tx;
        dcy = y - dcx + 0.9995 * dcy; dcx = y;
        out[n] = (float)bq(&olp, bq(&eq2, bq(&eq1, dcy)));
    }
    FILE *f = fopen(outp, "wb"); if (!f) return 3;
    unsigned dsz = N * 4, v32; unsigned short v16;
    fwrite("RIFF", 1, 4, f); v32 = 36 + dsz; fwrite(&v32, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v32 = 16; fwrite(&v32, 4, 1, f); v16 = 3; fwrite(&v16, 2, 1, f); v16 = 1; fwrite(&v16, 2, 1, f);
    v32 = 44100; fwrite(&v32, 4, 1, f); v32 = 44100 * 4; fwrite(&v32, 4, 1, f); v16 = 4; fwrite(&v16, 2, 1, f);
    v16 = 32; fwrite(&v16, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&dsz, 4, 1, f); fwrite(out, 4, N, f); fclose(f);
    return 0;
}
