/* Float prototype of the reedophone clarinet model (host only, for exploring and fitting).
 *
 *   proto out.wav midi vel dur_s [key=value ...]
 *
 * Model (dimensionless, after Guillemain, Kergomard & Voinier 2005, "Real-time synthesis of clarinet-like
 * instruments using digital impedance models"):
 *   reed   : x'' / wr^2 + qr x' / wr + x = p      (explicit 2nd-order scheme, x(n) from p(n-1))
 *   flow   : u = zeta (1 - gamma + x)+ sgn(gamma - p) sqrt|gamma - p|     (solved in closed form with p = u + 2 p-)
 *   bore   : waveguide, mouthpiece -> [register vent at fraction a] -> open end with reflection -LP(fc)
 *   output : the wave leaving the open end (1 + R) e, through a radiation high-pass and a body EQ
 * Register: below `brk` the bore plays its 1st mode (round trip P/2); from `brk` up the bore is 3x longer
 * (round trip 3P/2) and a vent at 1/3 of its length damps the modes that are not harmonics of the note.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 44100.0
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
typedef struct { const char *k; double v; } kv_t;
static kv_t P[] = {
    {"gamma_f", 0.62}, {"gamma_p", 0.42},   /* mouth pressure at vel 127 / at vel 0 */
    {"zeta", 0.35}, {"zeta_hi", 0.30},      /* embouchure (reed opening) low / high register */
    {"fr", 2400}, {"qr", 0.35},             /* reed resonance and damping */
    {"fc", 1500}, {"lpq", 0.0}, {"fck", 0},   /* fck: cutoff floor as a multiple of f0 (0 = none) */
    {"att2", 0},                            /* 2nd breath smoothing stage (s), 0 = off: S-shaped onset */             /* end reflection low-pass cutoff (tone-hole lattice) ; 0 = 1-pole */
    {"loss", 0.004}, {"loss_k", 0.5},       /* extra loop loss: g = 1 - loss*(f/261)^loss_k */
    {"brk", 69.5},                          /* first midi note played in the 2nd register */
    {"vent_a", 0.25}, {"vent_y", 0.6},     /* vent position (fraction of bore) and shunt admittance */
    {"vent_lp", 0},                         /* vent admittance low-pass (Hz), 0 = flat */
    {"att", 0.025}, {"att_p", 0.08}, {"rel", 0.05},   /* breath attack at vel 127 / vel 0, release (s) */
    {"vcurve", 1.0},                        /* velocity curve exponent for gamma */
    {"grel", 0}, {"d_f", 0.25}, {"d_p", 0.03}, {"thr_hi", 0.0},  /* grel=1: gamma = threshold + d(vel) (+thr_hi in reg 2) */
    {"gslope", 0.0},                        /* gamma change per octave above midi 70 */
    {"zslope", 0.0},                        /* zeta change per octave above midi 70 */
    {"olp", 0}, {"olp_q", 0.707},           /* output 2nd-order low-pass, 0 = off */
    {"noise", 0.0},                         /* breath noise added to gamma */
    {"tongue", 0.0}, {"tongue_p", -1},      /* tongue: reed held shut for this long at the onset (s), vel 127 / vel 0 (-1 = same) */
    {"rad_hp", 300}, {"rad_mix", 1.0},      /* radiation: 1-pole HP corner; mix of HP vs internal pressure */
    {"out_src", 0},                         /* 0 = transmitted end wave, 1 = mouthpiece pressure */
    {"eq_f", 0}, {"eq_g", 0}, {"eq_q", 1},  /* one peaking EQ (Hz, dB, Q), 0 = off */
    {"eq2_f", 0}, {"eq2_g", 0}, {"eq2_q", 1},
    {"tune", 0},                            /* cents */
    {"off", 0.0},                           /* note-off time (s) before end, 0 = release at dur-0.3 */
    {"interp", 1},                          /* 1 = 4-point Lagrange delay read */
};
#define NP (sizeof P / sizeof P[0])
static double g(const char *k) { for (unsigned i = 0; i < NP; i++) if (!strcmp(P[i].k, k)) return P[i].v; fprintf(stderr, "no param %s\n", k); exit(2); }

#define DL 16384
typedef struct { double b[DL]; int w; } line_t;
static void lpush(line_t *l, double x) { l->w = (l->w + 1) & (DL - 1); l->b[l->w] = x; }
/* value written d samples ago (d >= 1 real) */
static double lread(const line_t *l, double d, int lag)
{
    int i = (int)d; double f = d - i;
    if (!lag) { double a = l->b[(l->w - i) & (DL - 1)], b = l->b[(l->w - i - 1) & (DL - 1)]; return a + (b - a) * f; }
    /* 4-point Lagrange around i, i+1 */
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
/* phase delay (samples) of a biquad / one-pole at w */
static double bq_pd(const bq_t *f, double w)
{
    double complex_re_n = f->b0 + f->b1 * cos(w) + f->b2 * cos(2 * w), im_n = -f->b1 * sin(w) - f->b2 * sin(2 * w);
    double re_d = 1 + f->a1 * cos(w) + f->a2 * cos(2 * w), im_d = -f->a1 * sin(w) - f->a2 * sin(2 * w);
    double ph = atan2(im_n, complex_re_n) - atan2(im_d, re_d);
    return -ph / w;
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: proto out.wav midi vel dur [k=v...]\n"); return 1; }
    const char *outp = argv[1]; double midi = atof(argv[2]), vel = atof(argv[3]), dur = atof(argv[4]);
    for (int i = 5; i < argc; i++) {
        char *e = strchr(argv[i], '='); if (!e) continue; *e = 0; int ok = 0;
        for (unsigned j = 0; j < NP; j++) if (!strcmp(P[j].k, argv[i])) { P[j].v = atof(e + 1); ok = 1; }
        if (!ok) { fprintf(stderr, "unknown param %s\n", argv[i]); return 2; }
    }
    int N = (int)(dur * SR); float *out = calloc(N, sizeof(float));
    double f0 = 440 * pow(2, (midi - 69) / 12.0) * pow(2, g("tune") / 1200);
    int reg2 = midi >= g("brk");
    double vn = vel / 127.0;
    double oct = (midi - 70) / 12.0;
    double gamma_t = g("gamma_p") + (g("gamma_f") - g("gamma_p")) * pow(vn, g("vcurve")) + g("gslope") * oct;
    double zeta = (reg2 ? g("zeta_hi") : g("zeta")) + g("zslope") * oct;
    if (zeta < 0.02) zeta = 0.02;
    /* reed scheme */
    double wr = 2 * M_PI * g("fr"), T = 1 / SR, A = 1 / (wr * wr * T * T), B = g("qr") / (2 * wr * T);
    double rb1 = 1 / (A + B), ra1 = (2 * A - 1) / (A + B), ra2 = -(A - B) / (A + B);
    /* end reflection low-pass + loss */
    bq_t lp = {0}; double fc = g("fc");
    if (fc < g("fck") * f0) fc = g("fck") * f0;
    if (g("lpq") > 0) bq_lp(&lp, fc, g("lpq"));
    else { double a = exp(-2 * M_PI * fc / SR); lp.b0 = 1 - a; lp.a1 = -a; }
    double loss = 1 - g("loss") * pow(f0 / 261.6, g("loss_k"));
    double gth = 0;
    {
        double w = 2 * M_PI * f0 / SR, re = lp.b0 + lp.b1 * cos(w) + lp.b2 * cos(2 * w), im = -lp.b1 * sin(w) - lp.b2 * sin(2 * w);
        double rd = 1 + lp.a1 * cos(w) + lp.a2 * cos(2 * w), id = -lp.a1 * sin(w) - lp.a2 * sin(2 * w);
        double R = loss * sqrt((re * re + im * im) / (rd * rd + id * id));
        double Zpk = (1 + R) / (1 - R), c = 1 / (zeta * Zpk), sq = (c + sqrt(c * c + 3)) / 3;
        gth = sq * sq;
        if (g("grel") > 0) gamma_t = gth + (reg2 ? g("thr_hi") : 0) + g("d_p") + (g("d_f") - g("d_p")) * pow(vn, g("vcurve"));
    }
    if (getenv("PROTO_VERBOSE")) fprintf(stderr, "gth %.4f gamma %.4f zeta %.3f\n", gth, gamma_t, zeta);
    /* vent */
    double va = g("vent_a"), vy = g("vent_y"), vc0 = 1, va1 = 0, vs = 0;
    if (g("vent_lp") > 0) { va1 = exp(-2 * M_PI * g("vent_lp") / SR); vc0 = 1 - va1; }
    /* loop length: round trip = P/2 (reg 1) or 3P/2 (reg 2), minus the low-pass phase delay at the note */
    double w0 = 2 * M_PI * f0 / SR;
    double rt = (reg2 ? 1.5 : 0.5) * SR / f0 - bq_pd(&lp, w0);
    if (rt < 4) rt = 4;
    /* the loop as coded adds 1 sample per backward line read before its push */
    /* the vent sits at the first pressure node of the sounding mode: a quarter wavelength (P/4) from the reed,
     * vent_a = fraction of the period (0.25 ideal) */
    double La = reg2 ? va * SR / f0 - 1 : 0, Lb = reg2 ? (rt - 2) - La - (La + 1) + 0 : (rt - 1) / 2;   /* one-way lengths */
    if (reg2) Lb = (rt - 2) / 2 - La;
    if (La < 1.5) La = 1.5;
    if (Lb < 1.5) Lb = 1.5;
    static line_t af, ab, bf, bb;                                 /* seg a fwd/back, seg b fwd/back */
    double x1 = 0, x2 = 0, pprev = 0, gam = 0;
    bq_t eq1 = {0}, eq2 = {0}; eq1.b0 = eq2.b0 = 1;
    if (g("eq_f") > 0) bq_peak(&eq1, g("eq_f"), g("eq_g"), g("eq_q"));
    if (g("eq2_f") > 0) bq_peak(&eq2, g("eq2_f"), g("eq2_g"), g("eq2_q"));
    bq_t olp = {0}; olp.b0 = 1; if (g("olp") > 0) bq_lp(&olp, g("olp"), g("olp_q"));
    double hpa = exp(-2 * M_PI * g("rad_hp") / SR), hx = 0, hy = 0, dcx = 0, dcy = 0;
    double attt = g("att_p") + (g("att") - g("att_p")) * vn;
    double a2c = g("att2") > 0 ? exp(-1 / (g("att2") * SR)) : 0, g1 = 0;
    double att = exp(-1 / (attt * SR)), rel = exp(-1 / (g("rel") * SR));
    int noff = (int)((dur - (g("off") > 0 ? g("off") : 0.3)) * SR);
    double tp = g("tongue_p") < 0 ? g("tongue") : g("tongue_p");
    int ntongue = (int)((tp + (g("tongue") - tp) * vn) * SR);
    unsigned seed = 12345; double nz = 0;
    int lag = (int)g("interp"); double noise = g("noise"), radmix = g("rad_mix"); int osrc = (int)g("out_src");
    for (int n = 0; n < N; n++) {
        double tgt = n < noff ? gamma_t : 0;
        if (a2c > 0) { g1 = tgt + (g1 - tgt) * (tgt > g1 ? a2c : rel); gam = g1 + (gam - g1) * (g1 > gam ? att : rel); }
        else gam = tgt + (gam - tgt) * (tgt > gam ? att : rel);
        seed = seed * 1664525u + 1013904223u; nz = 0.7 * nz + 0.3 * ((int)(seed >> 8) / 8388608.0 - 1.0);
        double gm = gam * (1 + noise * nz);
        /* incoming wave at the mouthpiece */
        double pmin = reg2 ? lread(&ab, La, lag) : lread(&bb, Lb, lag);
        /* reed */
        double x = rb1 * pprev + ra1 * x1 + ra2 * x2; x2 = x1; x1 = x;
        double Ar = zeta * (1 - gm + x); if (Ar < 0 || n < ntongue) Ar = 0;
        double V = gm - 2 * pmin;
        double u = (V >= 0 ? 1 : -1) * Ar * (sqrt(Ar * Ar + 4 * fabs(V)) - Ar) / 2;
        double p = 2 * pmin + u; pprev = p;
        double pplus = p - pmin;
        double e;                                                 /* forward wave arriving at the open end */
        if (reg2) {
            lpush(&af, pplus);
            double a_in = lread(&af, La, lag);                    /* fwd wave reaching the vent */
            double b_in = lread(&bb, Lb, lag);                    /* back wave reaching the vent */
            /* shunt junction: P = (2(a+b) - q)/2 with leak flow q = vy * LP(P) (inertive vent: leaks lows) */
            double pj = (2 * (a_in + b_in) - vy * vs) / (2 + vy * vc0);
            double q = vc0 * pj + vs; vs = va1 * q;
            lpush(&ab, pj - a_in);                                /* back to the mouthpiece */
            lpush(&bf, pj - b_in);                                /* on to the open end */
            e = lread(&bf, Lb, lag);
        } else {
            lpush(&af, pplus);
            e = lread(&af, Lb, lag);
        }
        double r = -loss * bq(&lp, e);
        lpush(&bb, r);
        double tx = osrc ? p : (e + r);                           /* transmitted wave (pressure leaving the end) */
        /* radiation: 1-pole high-pass mixed with the plain signal */
        hy = hpa * (hy + tx - hx); hx = tx;
        double y = radmix * hy + (1 - radmix) * tx;
        dcy = y - dcx + 0.9995 * dcy; dcx = y;                    /* DC blocker */
        y = bq(&olp, bq(&eq2, bq(&eq1, dcy)));
        out[n] = (float)y;
    }
    /* write float wav */
    FILE *f = fopen(outp, "wb"); if (!f) return 3;
    unsigned dsz = N * 4, v32; unsigned short v16;
    fwrite("RIFF", 1, 4, f); v32 = 36 + dsz; fwrite(&v32, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v32 = 16; fwrite(&v32, 4, 1, f); v16 = 3; fwrite(&v16, 2, 1, f); v16 = 1; fwrite(&v16, 2, 1, f);
    v32 = 44100; fwrite(&v32, 4, 1, f); v32 = 44100 * 4; fwrite(&v32, 4, 1, f); v16 = 4; fwrite(&v16, 2, 1, f);
    v16 = 32; fwrite(&v16, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&dsz, 4, 1, f); fwrite(out, 4, N, f); fclose(f);
    return 0;
}
