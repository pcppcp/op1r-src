/* QUAD host test: rows, names, silence, bounded output and tails, SIZE, a per-config cost estimate, and WAVs.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 * Build (from ext/kennel):
 *   cc -O2 -Wall -DOP1_HOST -I synth_sdk -o OUT/quad_test synth_sdk/plugins/rackverb/quad_test.c \
 *      synth_sdk/plugins/rackverb/rv_quad.c -lm
 *   OUT/quad_test [WAVDIR]     (WAVDIR: write source-only host audio there)
 * The audio is source-only host audio at QD_RATE (44,100 Hz): not native, not R246's FX chain.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plugins/rackverb/rv_quad.h"
#define QD_TEST_META
#include "plugins/rackverb/rv_quad_presets.h"

static qd_state S;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 1;
static int16_t noise(void) { rng = rng * 1664525u + 1013904223u; return (int16_t)(rng >> 16); }
static void start(int i) { memset(&S, 0, sizeof S); qd_select(&S, i); }

/* blocks of input from f (NULL: silence); returns the peak; *last = the last block whose peak was > thr */
static int run(int blocks, int16_t (*f)(int), int32_t size, int at, int thr, int *last, double *energy, double *tw)
{
 int16_t in[QD_N], l[QD_N], r[QD_N]; int pk = 0;
 for (int b = 0; b < blocks; b++) {
  for (int n = 0; n < QD_N; n++) in[n] = f ? f(at + b * QD_N + n) : 0;
  qd_render(&S, in, l, r, size);
  int bp = 0;
  for (int n = 0; n < QD_N; n++) {
   int a = abs(l[n]), c = abs(r[n]); if (a > bp) bp = a; if (c > bp) bp = c;
   double v = (double)l[n] * l[n] + (double)r[n] * r[n];
   if (energy) *energy += v;
   if (tw) *tw += v * b;
  }
  if (bp > pk) pk = bp;
  if (last && bp > thr) *last = b;
 }
 return pk;
}
static int16_t f_noise(int t) { (void)t; return noise(); }
#define BLK(sec) ((int)((sec) * QD_RATE / QD_N + 0.5))

/* ---- test signals at QD_RATE */
static double drum(int t)          /* 120 bpm: kick 1 and 3, snare 2 and 4, hats on eighths, 4 bars */
{
 const int beat = QD_RATE / 2, e = beat / 2;
 double v = 0, tb = (t % beat) / (double)QD_RATE, te = (t % e) / (double)QD_RATE;
 int b = (t / beat) % 4;
 if (t >= 8 * beat * 2) return 0;
 if (b == 0 || b == 2) v += 0.9 * sin(2 * M_PI * (50 * tb + 60 * (1 - exp(-tb * 30)) / 30)) * exp(-tb * 9);
 else { rng = rng * 1664525u + 1013904223u; v += (0.55 * ((int32_t)rng >> 16) / 32768.0 + 0.35 * sin(2 * M_PI * 190 * tb)) * exp(-tb * 22); }
 rng = rng * 1664525u + 1013904223u;
 v += 0.15 * ((int32_t)rng >> 16) / 32768.0 * exp(-te * 120);
 return v;
}
static double chord(int t)         /* C maj7 band-limited saws, 2.5 s with a soft attack, then silence */
{
 static const double f[4] = { 130.81, 164.81, 196.0, 246.94 };
 double s = t / (double)QD_RATE, v = 0;
 if (s > 2.5) return 0;
 for (int k = 0; k < 4; k++) for (int h = 1; h * f[k] < 8000; h++) v += sin(2 * M_PI * h * f[k] * s) / h;
 double env = (1 - exp(-s * 40)) * (s > 2.3 ? (2.5 - s) / 0.2 : 1);
 return 0.12 * v * env;
}
static void wav(const char *path, const int16_t *l, const int16_t *r, int n)
{
 FILE *f = fopen(path, "wb"); if (!f) { printf("cannot write %s\n", path); return; }
 uint32_t frames = (uint32_t)n, data = frames * 4, v;
 fwrite("RIFF", 1, 4, f); v = 36 + data; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
 v = 16; fwrite(&v, 4, 1, f); uint16_t h[2] = { 1, 2 }; fwrite(h, 2, 2, f);
 v = 44100; fwrite(&v, 4, 1, f); v = 44100 * 4; fwrite(&v, 4, 1, f); h[0] = 4; h[1] = 16; fwrite(h, 2, 2, f);
 fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
 for (int i = 0; i < n; i++) {
  int16_t o[2] = { l[i], r[i] };
  fwrite(o, 2, 2, f);
 }
 fclose(f);
}
static void render_file(const char *dir, int i, const char *tag, double (*src)(int), double secs, FILE *idx)
{
 int n = BLK(secs) * QD_N;
 int16_t *l = malloc(n * 2), *r = malloc(n * 2), *x = malloc(n * 2), *ml = malloc(n * 2), *mr = malloc(n * 2);
 rng = 7; for (int t = 0; t < n; t++) { double v = src(t) * 32767; x[t] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
 start(i);
 for (int b = 0; b < n / QD_N; b++) qd_render(&S, x + b * QD_N, l + b * QD_N, r + b * QD_N, 32767);
 for (int t = 0; t < n; t++) {        /* a 50 % dry / wet mix, as the wrapper would at MIX 50 % */
  int a = (x[t] + l[t]) >> 1, c = (x[t] + r[t]) >> 1;
  ml[t] = (int16_t)a; mr[t] = (int16_t)c;
 }
 char name[64], path[512]; int k = 0;
 for (const char *p = qd_name(i); *p && k < 60; p++) name[k++] = (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ? (char)(*p + ((*p >= 'A' && *p <= 'Z') ? 32 : 0)) : '-';
 name[k] = 0;
 snprintf(path, sizeof path, "%s/%02d-%s-%s.wav", dir, i, name, tag);
 wav(path, ml, mr, n);
 fprintf(idx, "<li><span>%02d %s (config %d), %s</span><audio controls preload=\"none\" src=\"%02d-%s-%s.wav\"></audio></li>\n",
         i, qd_name(i), qd_config(i), tag, i, name, tag);
 free(l); free(r); free(x); free(ml); free(mr);
}

/* ---- cost: bcore packets per qd_render call (64 samples at 44.1 kHz), measured 2026-11-04 with
   quad_kern_check.py --cost (CYCLES around qd_render, noise in, SIZE at max and swinging; worst call of the worst
   preset per config). Re-measure after a change. */
static const struct { int cfg, packets, preset; } measured[5] = {
 { 1, 28052, 29 }, { 2, 24296, 40 }, { 3, 8301, 53 }, { 4, 12402, 60 }, { 5, 17893, 70 } };

int main(int argc, char **argv)
{
 printf("sizeof(qd_state) = %u (max %u), preset row %u bytes, %u bytes of rows\n", (unsigned)sizeof(qd_state),
        QD_STATE_MAX, (unsigned)sizeof(qd_preset), (unsigned)sizeof qd_presets);
 CHECK(sizeof(qd_state) <= QD_STATE_MAX, "state too large");
 /* names and configs */
 static const int first[6] = { 0, 0, 30, 42, 54, 66 }, last[6] = { 0, 29, 41, 53, 65, 89 };
 for (int i = 0; i < QD_PRESETS; i++) {
  const char *n = qd_name(i); int len = (int)strlen(n);
  CHECK(len > 0 && len <= 18, "%d name length", i);
  for (const char *c = n; *c; c++)
   CHECK((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr(" .-/%+#:", *c), "%d name char '%c'", i, *c);
  for (int j = 0; j < i; j++) CHECK(strcmp(n, qd_name(j)), "%d and %d same name %s", i, j, n);
  int c = qd_config(i);
  CHECK(c >= 1 && c <= 5 && i >= first[c] && i <= last[c], "%d config %d outside its block", i, c);
 }
 /* silence, noise tails, SIZE */
 int over = 0, nosize = 0;
 printf("tails (noise 2 s, then silence; last block above -70 dBFS):\n");
 for (int i = 0; i < QD_PRESETS; i++) {
  int lb = -1;
  start(i);
  int pk = run(BLK(2), NULL, 32767, 0, 0, &lb, NULL, NULL);
  CHECK(pk == 0, "%d %s silence in gives %d out", i, qd_name(i), pk);
  start(i); rng = 99;
  int pkn = run(BLK(2), f_noise, 32767, 0, 1 << 30, NULL, NULL, NULL);
  double rt = qd_meta_rt[i] / 1000.0, regen = qd_meta_regen[i] / 1000.0;
  double bound = rt * 1.5 + 4.0, full = bound + regen;
  lb = -1;
  int pkt = run(BLK(full + 1.0), NULL, 32767, 0, 10, &lb, NULL, NULL);
  double tail = (lb + 1) * (double)QD_N / QD_RATE;
  CHECK(pkn <= 32768 && pkt <= pkn + 1000, "%d %s tail peak %d above the noise peak %d", i, qd_name(i), pkt, pkn);
  CHECK(tail <= full, "%d %s tail %.2f s > %.2f s", i, qd_name(i), tail, full);
  if (tail > bound) { over++; printf("  %2d %-18s tail %.2f s > %.2f s: long regen delay (%.1f s regen)\n", i, qd_name(i), tail, bound, regen); }
  if (i % 10 == 0 || tail > 0.8 * bound)
   printf("  %2d %-18s peak %5d, tail %.2f s (bound %.2f, rt %.1f)\n", i, qd_name(i), pkn, tail, bound, rt);
  /* SIZE 50 % vs 100 %: the tail's energy-weighted mean time after a 0.5 s burst (a delay's or the reverse
     cluster's energy hardly depends on SIZE; its timing does) */
  const qd_preset *p = &qd_presets[i];
  double e[2] = { 0, 0 }, tw[2] = { 0, 0 };
  for (int k = 0; k < 2; k++) {
   start(i); rng = 5;
   run(BLK(0.5), f_noise, k ? 32767 : 0, 0, 1 << 30, NULL, NULL, NULL);
   run(BLK(3.0), NULL, k ? 32767 : 0, 0, 1 << 30, NULL, &e[k], &tw[k]);
  }
  const double m0 = tw[0] / (e[0] + 1), m1 = tw[1] / (e[1] + 1);
  double rel = fabs(m1 - m0) / (m1 + m0 + 1e-9);
  if (p->rtype || p->dtype) CHECK(rel > 0.05, "%d %s SIZE changes the tail by only %.3f", i, qd_name(i), rel);
  else nosize++;
 }
 printf("%d presets need their delay regen time beyond rt*1.5+4 s; %d presets have no reverb or delay (SIZE has no effect)\n",
        over, nosize);
 printf("bcore packets per call (measured, see the table above main), worst preset per config:\n");
 for (int c = 0; c < 5; c++) printf("  config %d: %d (%s)\n", measured[c].cfg, measured[c].packets, qd_name(measured[c].preset));
 if (argc > 1) {
  static const int pick[12] = { 0, 5, 7, 14, 19, 21, 31, 45, 56, 77, 84, 87 };
  char path[512]; snprintf(path, sizeof path, "%s/index.html", argv[1]);
  FILE *idx = fopen(path, "w");
  if (!idx) { printf("cannot write %s\n", path); return 1; }
  fprintf(idx, "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
   "<title>QUAD host audio</title><style>:root{--bg:#fafaf8;--fg:#1b1b1b;--mut:#666}"
   "@media (prefers-color-scheme:dark){:root{--bg:#161616;--fg:#e8e8e4;--mut:#9a9a9a}}"
   "body{background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif;margin:0 auto;max-width:760px;padding:16px}"
   "p{color:var(--mut)}ul{list-style:none;padding:0}li{margin:10px 0;display:flex;flex-wrap:wrap;gap:8px;align-items:center}"
   "li span{min-width:300px}audio{width:100%%;max-width:380px}</style></head><body>"
   "<h1>rackverb QUAD: 12 presets</h1><p>Source-only host audio, not native, not R246's FX chain. rv_quad.c on the host at 44,100 Hz "
   "(its C references, the same samples as the Blackfin kernels); 50 %% dry / wet; SIZE 100 %%. A synthesized drum loop and a C maj7 chord.</p><ul>\n");
  for (int k = 0; k < 12; k++) {
   render_file(argv[1], pick[k], "drums", drum, 9.0, idx);
   render_file(argv[1], pick[k], "chord", chord, 7.0, idx);
  }
  fprintf(idx, "</ul></body></html>\n");
  fclose(idx);
  printf("wrote 24 WAVs and index.html to %s\n", argv[1]);
 }
 printf(fails ? "%d FAILED\n" : "all OK\n", fails);
 return fails != 0;
}
