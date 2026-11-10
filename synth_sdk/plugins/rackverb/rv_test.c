/* rackverb host test: every VERB I / VERB II / FEX program builds and fits, runs stable on full-scale noise, falls
 * silent, and the reverbs' decay is near their name. Source-only (the machine alone at its own rate, no OP-1 chain).
 * Build: cc -O2 -Wall -DOP1_HOST -I../.. -o rv_test rv_test.c rv_mv.c && ./rv_test [wavdir]
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rv_programs.h"

static rv_mv_t m;
static uint32_t rng = 1;
static int16_t noise(void) { rng = rng * 1664525u + 1013904223u; return (int16_t)((int32_t)rng >> 20); }   /* +-2048: the ADC's range */

static void reset(const rv_row_t *row, int32_t fs2)
{
    memset(&m, 0, sizeof m);
    if (rv_mv_build(&m.prog, row, fs2) < 0) { printf("FAIL build %s\n", row->name); exit(1); }
    rv_mv_size(&m.prog, 32767);
}

/* the energy envelope of the wet after a noise burst: 10 ms windows, in dB */
static int run(const rv_row_t *row, int32_t fs2, double *db, int nwin, int *peak)
{
    reset(row, fs2);
    int fs = fs2 / 2, win = fs / 100, burst = fs / 10;
    int16_t in[64], l[64], r[64];
    *peak = 0;
    for (int w = 0; w < nwin; w++) {
        double e = 0;
        for (int s = 0; s < win; s += 32) {
            int n = win - s < 32 ? win - s : 32;
            for (int i = 0; i < n; i++) in[i] = (w * win + s + i) < burst ? (int16_t)(noise() * 2) : 0;
            rv_mv_run(&m, in, l, r, n);
            rv_mv_lfo(&m.prog, n);
            for (int i = 0; i < n; i++) {
                int a = abs(l[i]) > abs(r[i]) ? abs(l[i]) : abs(r[i]);
                if (a > *peak) *peak = a;
                e += (double)l[i] * l[i] + (double)r[i] * r[i];
            }
        }
        db[w] = 10 * log10(e / (2.0 * win) / (4096.0 * 4096.0) + 1e-20);
    }
    return 0;
}

int main(void)
{
    struct { const char *u; const rv_row_t *rows; int n, first; int32_t fs2; } units[3] = {
        { "VERB I", rv_verb1, 63, 1, 46875 }, { "VERB II", rv_verb2, 100, 0, 62500 }, { "FEX", rv_fex, 63, 1, 46875 } };
    static double db[3000];
    int fails = 0, maxused = 0, maxring = 0;
    for (int u = 0; u < 3; u++)
        for (int i = 0; i < units[u].n; i++) {
            const rv_row_t *row = &units[u].rows[i];
            int peak, nwin = 2500;                   /* 25 s */
            run(row, units[u].fs2, db, nwin, &peak);
            if (m.prog.used > maxused) maxused = m.prog.used;
            if (m.prog.ring_used > maxring) maxring = m.prog.ring_used;
            int pk = 0;
            for (int w = 0; w < nwin; w++) if (db[w] > db[pk]) pk = w;
            int t5 = -1, t35 = -1, quiet = -1;
            for (int w = pk; w < nwin; w++) {
                if (t5 < 0 && db[w] < db[pk] - 5) t5 = w;
                if (t35 < 0 && db[w] < db[pk] - 35) t35 = w;
                if (quiet < 0 && db[w] < -80) { int ok = 1; for (int j = w; j < w + 50 && j < nwin; j++) if (db[j] >= -80) ok = 0; if (ok) quiet = w; }
            }
            double rt = (t5 >= 0 && t35 >= 0) ? (t35 - t5) * 0.01 * 60 / 30 : -1;
            char note[64] = "";
            int bad = 0;
            if (row->shape != RV_DEFEAT && db[pk] < -40) { bad = 1; strcpy(note, "too quiet"); }
            if (row->shape == RV_REV) {
                double want = row->t / 100.0;
                if (rt < 0 || rt < want * 0.5 || rt > want * 1.8) { bad = 1; snprintf(note, sizeof note, "RT %.2f s, wants %.2f", rt, want); }
            }
            if (row->shape != RV_DEFEAT && quiet < 0 && !(row->shape == RV_REV && row->t > 1000)) { bad = 1; strcpy(note, "never quiet"); }
            printf("%-7s %02d %-24s steps %3d ring %5d peak %5d level %6.1f dB RT %6.2f s quiet %5.2f s %s%s\n",
                   units[u].u, i + units[u].first, row->name, m.prog.used, m.prog.ring_used, peak, db[pk], rt,
                   quiet * 0.01, bad ? "FAIL " : "", note);
            fails += bad;
        }
    /* the branch-free kernel against the reference, bit for bit, on every program (with its LFOs moving) */
    static rv_mv_t a, b;
    for (int u = 0; u < 3; u++)
        for (int i = 0; i < units[u].n; i++) {
            memset(&a, 0, sizeof a);
            rv_mv_build(&a.prog, &units[u].rows[i], units[u].fs2);
            rv_mv_size(&a.prog, 20000);
            b = a;
            for (int c = 0; c < 3000; c++) {
                int16_t in[40], l1[40], r1[40], l2[40], r2[40];
                for (int k = 0; k < 40; k++) in[k] = c < 1500 ? (int16_t)(noise() * 2) : 0;
                rv_mv_run(&a, in, l1, r1, 35); rv_mv_lfo(&a.prog, 35);
                rv_mv_run_ref(&b, in, l2, r2, 35); rv_mv_lfo(&b.prog, 35);
                if (memcmp(l1, l2, 70) || memcmp(r1, r2, 70) || memcmp(a.ring, b.ring, sizeof a.ring)) {
                    printf("FAIL kernel differs: %s %s call %d\n", units[u].u, units[u].rows[i].name, c); fails++; break;
                }
            }
        }
    printf("max steps %d of 125, max ring %d of %d; %d fail\n", maxused, maxring, RV_RING, fails);
    return fails ? 1 : 0;
}
