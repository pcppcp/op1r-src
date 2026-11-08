/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* Host test and renderer for juni chorus (float only in this test, never in the plugin).
 *   cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -DOP1_HOST -I<synth_sdk> \
 *      juni_chorus_test.c juni_chorus.c -lm -o juni_chorus_test
 *   ./juni_chorus_test check                      checks only (exit 1 on failure)
 *   ./juni_chorus_test render <dir>               WAVs (44.1 kHz stereo, R246's FX rate): dry, I, II, I+II
 *   ./juni_chorus_test measure [dir]              depth figures as JSON (see measure() below); short WAVs into dir
 * Files per source (chord, pad): <src>_dry.wav, <src>_I.wav, _II.wav, _I+II.wav, and <src>_mono_I.wav ... through
 * the mono render at position 0. */
#include "op1_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const op1_fx_stereo_t FX_juni_chorus;
volatile uint8_t op1_fx_page[3][4];
#define FS 44100                        /* R246 calls an FX with 64 samples at 44.1 kHz */
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* band-limited saw sum: harmonics up to 9.5 kHz, rolloff exponent e */
static float partials(float f, float ph, float e)
{
    float s = 0;
    for (int n = 1; n * f < 9500; n++) s += sinf((float)n * ph) / powf((float)n, e);
    return s;
}

/* chord (plain saws, plucked) or pad (3 detuned voices per note, panned wide, slow swell); two chords in turn */
static void source(int pad, int secs, float *l, float *r)
{
    static const float c1[] = { 130.81f, 155.56f, 196.00f, 233.08f, 261.63f };
    static const float c2[] = { 103.83f, 155.56f, 207.65f, 261.63f, 311.13f };
    const int total = secs * FS;
    memset(l, 0, sizeof(float) * (size_t)total); memset(r, 0, sizeof(float) * (size_t)total);
    for (int seg = 0; seg < 2; seg++) {
        const float *notes = seg ? c2 : c1;
        const int a = seg * total / 2, z = (seg + 1) * total / 2;
        for (int k = 0; k < 5; k++) {
            const int nv = pad ? 3 : 1;
            for (int v = 0; v < nv; v++) {
                const float det = pad ? powf(2, ((float)v - 1) * 9 / 1200.0f) : 1.0f;
                const float f = notes[k] * det;
                float ph = (float)(k * 7 + v * 3);
                const float pan = pad ? ((float)v - 1) * 0.6f : 0;
                const float gl = 0.5f - 0.5f * pan, gr = 0.5f + 0.5f * pan;
                for (int i = a; i < z; i++) {
                    const float t = (float)(i - a) / FS, d = (float)(z - i) / FS;
                    float env = pad ? fminf(t / 1.0f, 1) * fminf(d / 0.8f, 1) : fminf(t / 0.01f, 1) * fminf(d / 0.05f, 1);
                    if (!pad) env *= expf(-t * 0.6f);
                    ph += 6.2831853f * f / FS;
                    if (ph > 6.2831853f * 1000) ph -= 6.2831853f * 1000;
                    const float x = partials(f, ph, pad ? 1.6f : 1.0f) * env * (pad ? 0.11f : 0.09f);
                    l[i] += x * gl * 2; r[i] += x * gr * 2;
                }
            }
        }
    }
}

static void wav(const char *path, const int16_t *lr, int frames)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    uint32_t d = (uint32_t)frames * 4, v;
    uint16_t h;
    fwrite("RIFF", 1, 4, f); v = 36 + d; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f); h = 1; fwrite(&h, 2, 1, f); h = 2; fwrite(&h, 2, 1, f);
    v = FS; fwrite(&v, 4, 1, f); v = FS * 4; fwrite(&v, 4, 1, f); h = 4; fwrite(&h, 2, 1, f); h = 16; fwrite(&h, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&d, 4, 1, f); fwrite(lr, 4, (size_t)frames, f); fclose(f);
}

static int16_t kn[8];
static void run(int stereo, const int16_t *in_l, const int16_t *in_r, int16_t *out_l, int16_t *out_r, int frames, int pos)
{
    FX_juni_chorus.mono.init(pos);
    for (int b = 0; b + OP1_FX_BLOCK <= frames; b += OP1_FX_BLOCK) {
        memcpy(out_l + b, in_l + b, 2 * OP1_FX_BLOCK); memcpy(out_r + b, in_r + b, 2 * OP1_FX_BLOCK);
        if (stereo) FX_juni_chorus.render_stereo(pos, out_l + b, out_r + b, kn);
        else { FX_juni_chorus.mono.render(pos, out_l + b, kn); memcpy(out_r + b, out_l + b, 2 * OP1_FX_BLOCK); }
    }
    for (int i = frames / OP1_FX_BLOCK * OP1_FX_BLOCK; i < frames; i++) out_l[i] = out_r[i] = 0;   /* a partial block is not processed */
}

static int peak(const int16_t *x, int n) { int p = 0; for (int i = 0; i < n; i++) if (abs(x[i]) > p) p = abs(x[i]); return p; }

static float fl[8 * FS], fr[8 * FS];
static int16_t il[8 * FS], ir[8 * FS], ol[8 * FS], orr[8 * FS], lr[2 * 8 * FS];

/* ---- measure: how much the chorus moves, per position and knob setting (host, source-only) ----
 * Two steady sources whose partials all sit on a 22.05 Hz grid (an exact 2000-sample period): a 441 Hz sine and a
 * JUNI-like saw chord (band-limited saws on grid harmonics 6, 7, 9, 12: 132.3, 154.35, 198.45, 264.6 Hz) and its lowest
 * note alone (saw132: partials 132 Hz apart, so its pitch figure is the clean one; the chord's partials sit one 22 Hz
 * bin apart in places, where the vibrato's sidebands leak into the neighbour and read high). A sliding
 * 2000-sample DFT (45 ms) follows each partial's complex amplitude c(t) every 100 samples, from 1 s to 7 s. Per
 * channel, weighted by each partial's power (the chord's partials up to 1.5 kHz only: above it the vibrato's sidebands
 * reach the next partial one 22 Hz bin away):
 *   out cents   RMS of the output's instantaneous pitch deviation (the partial's phase slope), dry + wet together
 *   wet cents   the same for the wet signal alone (out - dry); the tap's own vibrato
 *   wet dB      the wet's power against the dry's
 *   mod dB      the time-varying power of the output (|c - mean c|^2), against the dry's: everything the chorus
 *               moves (pitch, comb sweep, beating); the dry and any static colour don't count
 *   flutter dB  RMS of the output partials' level in dB around their mean (the comb sweep as heard level change)
 * The 45 ms window smooths I+II's 9 Hz a little (about 0.75 x). */
#define MW 2000
static double mtw_c[MW], mtw_s[MW];
typedef struct { double out_c, wet_c, wet_db, mod_db, flut_db, lvl_db; } meas_t;

static void measure_ch(const int16_t *dry, const int16_t *out, int a, int z, const int *bins, int nb, meas_t *m)
{
    double sw = 0, so = 0, sc = 0, swc = 0, swe = 0, sd = 0, smod = 0, sfl = 0, sfw = 0, slv = 0;
    enum { H = 100, L = MW / H };                                    /* 20 hops = one window */
    const int nf = (z - a) / H;
    static double cr[2][(8 * FS) / H + 2], ci[2][(8 * FS) / H + 2];
    for (int b = 0; b < nb; b++) {
        const int k = bins[b];
        double dr = 0, di = 0;
        for (int n = a - MW; n < a; n++) { const int q = (int)(((long)k * n) % MW); dr += dry[n] * mtw_c[q]; di -= dry[n] * mtw_s[q]; }
        const double pd = dr * dr + di * di;
        if (pd < 1e-9) continue;
        for (int s = 0; s < 2; s++) {                                 /* 0: out, 1: wet = out - dry */
            double r = 0, i = 0;
            for (int n = a - MW; n < a; n++) {
                const double y = s ? out[n] - dry[n] : out[n];
                const int q = (int)(((long)k * n) % MW); r += y * mtw_c[q]; i -= y * mtw_s[q];
            }
            int f = 0;
            for (int n = a; n < a + nf * H; n++) {
                if ((n - a) % H == 0) { cr[s][f] = r; ci[s][f] = i; f++; }
                const double y = s ? out[n] - dry[n] : out[n], yo = s ? out[n - MW] - dry[n - MW] : out[n - MW];
                const int q = (int)(((long)k * n) % MW);
                r += (y - yo) * mtw_c[q]; i -= (y - yo) * mtw_s[q];
            }
        }
        const double fk = (double)k * FS / MW;
        double mr = 0, mi = 0, pw = 0, pww = 0, ldb = 0;
        for (int f = 0; f < nf; f++) { mr += cr[0][f]; mi += ci[0][f]; pw += cr[0][f] * cr[0][f] + ci[0][f] * ci[0][f];
                                       pww += cr[1][f] * cr[1][f] + ci[1][f] * ci[1][f]; }
        mr /= nf; mi /= nf; pw /= nf; pww /= nf;
        double var = 0;
        for (int f = 0; f < nf; f++) { const double xr = cr[0][f] - mr, xi = ci[0][f] - mi; var += xr * xr + xi * xi;
                                       ldb += 10 * log10((cr[0][f] * cr[0][f] + ci[0][f] * ci[0][f]) / pw + 1e-30); }
        var /= nf; ldb /= nf;
        for (int f = 0; f < nf; f++) {
            const double l = 10 * log10((cr[0][f] * cr[0][f] + ci[0][f] * ci[0][f]) / pw + 1e-30) - ldb;
            sfl += pd * l * l; sfw += pd;
        }
        for (int s = 0; s < 2; s++) {
            /* the phase slope over one window (2000 samples), unwrapped hop by hop: a neighbour partial's leakage
             * turns a whole number of times in 2000 samples, so it drops out of the difference */
            static double ph[(8 * FS) / H + 2];
            ph[0] = 0;
            for (int f = 1; f < nf; f++) {
                const double xr = cr[s][f] * cr[s][f - 1] + ci[s][f] * ci[s][f - 1], xi = ci[s][f] * cr[s][f - 1] - cr[s][f] * ci[s][f - 1];
                ph[f] = ph[f - 1] + atan2(xi, xr);
            }
            for (int f = L; f < nf; f++) {
                const double df = (ph[f] - ph[f - L]) / (2 * M_PI) * FS / MW;
                const int fm = f - L / 2;
                const double c = 1200 * log2(1 + df / fk), w = cr[s][fm] * cr[s][fm] + ci[s][fm] * ci[s][fm];
                if (s) { swc += w * c * c; swe += w; } else { sc += w * c * c; so += w; }
            }
        }
        sd += pd; smod += var; sw += pww; slv += pw;
    }
    m->out_c = sqrt(sc / so); m->wet_c = sqrt(swc / swe);
    m->wet_db = 10 * log10(sw / sd); m->mod_db = 10 * log10(smod / sd); m->flut_db = sqrt(sfl / sfw); m->lvl_db = 10 * log10(slv / sd);
}

static void measure(const char *dir)
{
    const int secs = 8, N = secs * FS, a = FS, z = 7 * FS;
    for (int i = 0; i < MW; i++) { mtw_c[i] = cos(2 * M_PI * i / MW); mtw_s[i] = sin(2 * M_PI * i / MW); }
    static const char *src[3] = { "sine441", "sawchord", "saw132" };
    static const struct { const char *name; int16_t k[4]; } set[] = {
        { "old-defaults_II", { 16384, 24000, 6000, 21000 } },
        { "modeI_old-depth-mix", { 4608, 24000, 6000, 21000 } },
        { "new-defaults_I", { 4608, 27852, 6000, 24575 } },
        { "new-defaults_I_mix100", { 4608, 27852, 6000, 32767 } },
        { "modeII_new-depth-mix", { 16384, 27852, 6000, 24575 } },
        { "modeI+II_new-depth-mix", { 32767, 27852, 6000, 24575 } } };
    static const int grid[4] = { 6, 7, 9, 12 };
    int bins[512], nb = 0;
    printf("[\n");
    int firstrow = 1;
    for (int s = 0; s < 3; s++) {
        nb = 0;
        if (s == 0) bins[nb++] = 20;                                   /* 441 Hz */
        else if (s == 1) { for (int k = 1; k <= 68; k++) for (int g = 0; g < 4; g++) if (k % grid[g] == 0) { bins[nb++] = k; break; } }
        else for (int k = 6; k <= 68; k += 6) bins[nb++] = k;
        for (int i = 0; i < N; i++) {
            double x = 0;
            if (s == 0) x = 9000 * sin(2 * M_PI * 20 * i / MW);
            else for (int g = 0; g < (s == 1 ? 4 : 1); g++)
                for (int h = 1; h * grid[g] < 431; h++) x += 0.25 * sin(2 * M_PI * h * grid[g] * i / MW + g * 1.3 + h * 0.7) / h;
            fl[i] = (float)x;
        }
        if (s) { float pk = 0; for (int i = 0; i < N; i++) pk = fmaxf(pk, fabsf(fl[i])); for (int i = 0; i < N; i++) fl[i] *= 12000 / pk; }
        for (int i = 0; i < N; i++) il[i] = ir[i] = (int16_t)lrintf(fl[i]);
        if (dir) {
            for (int i = 0; i < 6 * FS; i++) { lr[2 * i] = il[i + FS]; lr[2 * i + 1] = ir[i + FS]; }
            char p[512]; snprintf(p, sizeof p, "%s/%s_dry.wav", dir, src[s]); wav(p, lr, 6 * FS);
        }
        for (unsigned c = 0; c < sizeof set / sizeof set[0]; c++) for (int st = 0; st < 2; st++) {
            memcpy(kn, set[c].k, sizeof set[c].k);
            run(st, il, ir, ol, orr, N, st ? 2 : 0);
            meas_t ml, mr;
            measure_ch(il, ol, a, z, bins, nb, &ml);
            if (st) measure_ch(ir, orr, a, z, bins, nb, &mr); else mr = ml;
            printf("%s{\"source\":\"%s\",\"setting\":\"%s\",\"knobs\":[%d,%d,%d,%d],\"pos\":\"%s\","
                   "\"out_cents\":%.2f,\"wet_cents\":%.2f,\"wet_db\":%.1f,\"mod_db\":%.1f,\"flutter_db\":%.2f,\"level_db\":%.2f,"
                   "\"R_out_cents\":%.2f,\"R_mod_db\":%.1f}\n", firstrow ? "" : ",", src[s], set[c].name, set[c].k[0], set[c].k[1],
                   set[c].k[2], set[c].k[3], st ? "stereo" : "mono", ml.out_c, ml.wet_c, ml.wet_db, ml.mod_db, ml.flut_db, ml.lvl_db,
                   mr.out_c, mr.mod_db);
            firstrow = 0;
            if (dir && (c == 0 || c == 2)) {
                for (int i = 0; i < 6 * FS; i++) { lr[2 * i] = ol[i + FS]; lr[2 * i + 1] = orr[i + FS]; }
                char p[512]; snprintf(p, sizeof p, "%s/%s_%s_%s.wav", dir, src[s], set[c].name, st ? "stereo" : "mono");
                wav(p, lr, 6 * FS);
            }
        }
    }
    printf("]\n");
}

int main(int argc, char **argv)
{
    const int secs = 8, N = secs * FS;
    kn[4] = 0x5301;
    if (argc > 1 && !strcmp(argv[1], "measure")) { measure(argc > 2 ? argv[2] : 0); return 0; }
    if (argc > 2 && !strcmp(argv[1], "render")) {
        static const char *src[2] = { "chord", "pad" };
        static const char *mn[4] = { "dry", "I", "II", "I+II" };
        static const int16_t blue[4] = { 0, 4608, 16384, 29000 };
        for (int s = 0; s < 2; s++) {
            source(s, secs, fl, fr);
            float pk = 0;
            for (int i = 0; i < N; i++) { pk = fmaxf(pk, fabsf(fl[i])); pk = fmaxf(pk, fabsf(fr[i])); }
            const float g = 12000 / pk;                                  /* source peak 12000 (-8.7 dBFS) */
            for (int i = 0; i < N; i++) { il[i] = (int16_t)lrintf(fl[i] * g); ir[i] = (int16_t)lrintf(fr[i] * g); }
            for (int m = 0; m < 4; m++) for (int mono = 0; mono < 2; mono++) {
                kn[0] = blue[m]; kn[1] = 27852; kn[2] = 6000; kn[3] = m ? 24575 : 0;   /* the defaults' DEPTH, TONE, MIX */
                run(!mono, il, ir, ol, orr, N, 2 - mono * 2);
                for (int i = 0; i < N; i++) { lr[2 * i] = ol[i]; lr[2 * i + 1] = orr[i]; }
                char p[512]; snprintf(p, sizeof p, "%s/%s_%s%s.wav", argv[2], src[s], mono ? "mono_" : "", mn[m]);
                if (m || !mono) wav(p, lr, N);
                if (m || !mono) printf("%-50s peak L %5d R %5d (input %d)\n", p, peak(ol, N), peak(orr, N), peak(il, N));
            }
        }
        return 0;
    }

    /* 1. knob grid at full scale: loud saw in, both renders, all positions: sanitizers, worst output peak */
    for (int i = 0; i < FS; i++) { il[i] = (int16_t)(((i * 3) & 63) * 1024 - 32768 + 1024); ir[i] = (int16_t)-il[i]; }
    const int g[3] = { 0, 16384, 32767 };
    int worst = 0;
    for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) for (int c = 0; c < 3; c++) for (int d = 0; d < 3; d++)
        for (int st = 0; st < 2; st++) {
            kn[0] = (int16_t)g[a]; kn[1] = (int16_t)g[b]; kn[2] = (int16_t)g[c]; kn[3] = (int16_t)g[d];
            run(st, il, ir, ol, orr, FS, 2 - st * 2);
            int p = peak(ol, FS); if (peak(orr, FS) > p) p = peak(orr, FS); if (p > worst) worst = p;
        }
    printf("full-scale saw through all 3^4 knob settings: no UB; worst output peak %d\n", worst);

    /* 2. silence in, silence out, in every position (no dirty state, no DC) */
    memset(il, 0, sizeof il); memset(ir, 0, sizeof ir);
    for (int pos = 0; pos < 3; pos++) {
        kn[0] = 16384; kn[1] = 32767; kn[2] = 0; kn[3] = 32767;
        run(pos == 2, il, ir, ol, orr, FS, pos);
        CHECK(peak(ol, FS) == 0 && peak(orr, FS) == 0, "silence in gives sound out at pos %d (%d)", pos, peak(ol, FS));
    }
    /* a loud burst then silence: the tail must end (delay <= 5.3 ms + the filters) and settle to at most 1 LSB */
    for (int i = 0; i < 640; i++) il[i] = ir[i] = (int16_t)((i & 31) * 2000 - 31000);
    run(1, il, ir, ol, orr, FS, 2);
    int tail = 0; for (int i = 3200; i < FS; i++) tail = abs(ol[i]) > tail ? abs(ol[i]) : tail;
    CHECK(tail <= 1, "tail after a burst: %d", tail);

    /* 3. mix 0 is the dry signal exactly; at mix 100 % the stereo sides differ */
    for (int i = 0; i < FS; i++) il[i] = ir[i] = (int16_t)lrintf(9000 * sinf(6.2831853f * 440 * i / FS));
    kn[0] = 3000; kn[1] = 20000; kn[2] = 8000; kn[3] = 0;
    run(1, il, ir, ol, orr, FS, 2);
    int diff0 = 0; for (int i = 0; i < FS; i++) if (i < FS / 64 * 64) diff0 += abs(ol[i] - il[i]) + abs(orr[i] - ir[i]);
    CHECK(diff0 == 0, "mix 0 is not dry (%d)", diff0);
    kn[3] = 32767;
    run(1, il, ir, ol, orr, FS, 2);
    long dl = 0; for (int i = 2000; i < FS; i++) dl += abs(ol[i] - orr[i]);
    CHECK(dl > 0, "stereo L and R identical");
    printf("mix 0 == dry; at mix 100 %% L-R mean abs difference %ld per sample (1 s of 440 Hz, 9000)\n", dl / (FS - 2000));
    const int pl = peak(ol, FS);
    printf("sine 9000 at mix 100 %%: peak %d (%.2f x)\n", pl, pl / 9000.0);

    /* 4. clicks: sweep MODE I -> II -> I+II and back over 6 s on a 1 kHz sine; the largest sample-to-sample step may
     * not exceed 1.2 x the larger of the steps with the knob parked in I and in II (+50) */
    for (int i = 0; i < 6 * FS; i++) il[i] = ir[i] = (int16_t)lrintf(10000 * sinf(6.2831853f * 1000 * i / FS));
    int maxd[3] = { 0, 0, 0 };
    for (int pass = 0; pass < 3; pass++) {
        FX_juni_chorus.mono.init(2);
        for (int b = 0; b + 64 <= 6 * FS; b += 64) {
            memcpy(ol + b, il + b, 128); memcpy(orr + b, ir + b, 128);
            const int blk = b / 64, tot = 6 * FS / 64;
            int mode = pass == 0 ? 0 : pass == 1 ? 16384 : (blk < tot / 2 ? blk * 2 * 32767 / tot : (tot - blk) * 2 * 32767 / tot);
            kn[0] = (int16_t)mode; kn[1] = 32767; kn[2] = 3000; kn[3] = 32767;
            FX_juni_chorus.render_stereo(2, ol + b, orr + b, kn);
        }
        for (int i = 1; i < 6 * FS; i++) { int d = abs(ol[i] - ol[i - 1]); if (d > maxd[pass]) maxd[pass] = d; }
    }
    printf("max step: mode I %d, II %d, sweep %d\n", maxd[0], maxd[1], maxd[2]);

    /* 5. mono is the stereo left side: one tap and the dry, bit for bit (so the stereo click check covers it too),
     * through a MODE sweep with the other knobs moving */
    for (int i = 0; i < 4 * FS; i++) il[i] = ir[i] = (int16_t)(lrintf(9000 * sinf(6.2831853f * 441 * i / FS)) + ((i * 7) & 255) - 128);
    {
        static int16_t ml[4 * FS];
        long diff = 0;
        FX_juni_chorus.mono.init(0); FX_juni_chorus.mono.init(2);
        for (int b = 0; b + 64 <= 4 * FS; b += 64) {
            const int blk = b / 64;
            kn[0] = (int16_t)(blk * 97 % 32768); kn[1] = (int16_t)(blk * 53 % 32768); kn[2] = (int16_t)(blk * 31 % 32768); kn[3] = (int16_t)(blk * 71 % 32768);
            memcpy(ml + b, il + b, 128); memcpy(ol + b, il + b, 128); memcpy(orr + b, ir + b, 128);
            FX_juni_chorus.mono.render(0, ml + b, kn);
            FX_juni_chorus.render_stereo(2, ol + b, orr + b, kn);
            for (int i = b; i < b + 64; i++) diff += ml[i] != ol[i];
        }
        CHECK(diff == 0, "mono differs from the stereo left side in %ld samples", diff);
        printf("mono == stereo left side over a 4 s knob sweep\n");
    }
    CHECK(maxd[2] <= (maxd[0] > maxd[1] ? maxd[0] : maxd[1]) * 12 / 10 + 50, "mode sweep clicks");
    printf(fails ? "FAILED\n" : "juni_chorus host checks passed\n");
    return fails != 0;
}
