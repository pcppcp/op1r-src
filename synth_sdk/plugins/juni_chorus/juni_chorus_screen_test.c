/* juni chorus screen host test: runs the real DSP (juni_chorus.c, 9 blocks = 26 ms per frame) under the screen and
 * prints frames as JSON (for screen_check.py: bounds, previews) and checks the motion: MIX 0 still, the sway and
 * ripple follow DEPTH / MODE / MIX / the input, TONE moves only the moon's colour and halo.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 *   juni_chorus_screen_test SCENARIO     (states | motion | strip; one JSON frame per line on stdout) */
#ifndef OP1_HOST
#define OP1_HOST
#endif
#include "../../op1_synth.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int16_t host_sine[8192];
int16_t host_fx_knobs[3][8];
int16_t host_fx_meter[3];
void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4];
static int capture, first;
typedef struct { int x, y, size; uint8_t c[4]; char s[32]; } txt;
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t z, const uint8_t *c) { txt *t = o; t->x = x; t->y = y; t->size = z; memcpy(t->c, c, 4); t->s[0] = 0; return o; }
void host_text_set(void *o, const char *s) { snprintf(((txt *)o)->s, 32, "%s", s); }
void host_text_draw(void *o, void *r, const int32_t *m) { (void)r; (void)m; if (!capture) return; txt *t = o;
 printf("%s[\"text\",%d,%d,%d,0,[%u,%u,%u,%u],\"%s\"]", first ? "" : ",", t->x, t->y, t->size, t->c[0], t->c[1], t->c[2], t->c[3], t->s); first = 0; }
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return (void *)1; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
uint8_t *host_node_rgba(void *s, const char *id, int f) { static uint8_t c[4]; (void)s; (void)id; (void)f; return c; }
void host_screen_draw(void *s, int d) { (void)s; (void)d; }
void *host_raster(void) { return 0; }
#define MAXL 2048
static int32_t L[MAXL][6]; static uint8_t LC[MAXL][4];
static int lines, maxy;
void host_raster_line(void *r, const void *aa, const void *bb, int32_t w, const uint8_t *c)
{
 (void)r; const op1_pt *a = aa, *b = bb;
 assert(a->x >= 0 && a->x <= 320 * 1024 && b->x >= 0 && b->x <= 320 * 1024);
 assert(a->y >= 0 && a->y <= 160 * 1024 && b->y >= 0 && b->y <= 160 * 1024);
 if (a->y + w / 2 > maxy) maxy = a->y + w / 2;
 if (b->y + w / 2 > maxy) maxy = b->y + w / 2;
 if (lines < MAXL) { L[lines][0] = a->x; L[lines][1] = a->y; L[lines][2] = b->x; L[lines][3] = b->y; L[lines][4] = w; memcpy(LC[lines], c, 4); }
 lines++;
 if (capture) { printf("%s[\"line\",%d,%d,%d,%d,%d,[%u,%u,%u,%u]]", first ? "" : ",", a->x, a->y, b->x, b->y, w, c[0], c[1], c[2], c[3]); first = 0; }
}
#include "juni_chorus_screen.c"
extern const op1_fx_stereo_t FX_juni_chorus;

static uint32_t t_samples, osc;
static int loud = 12000;                 /* the test tone's amplitude */
/* one frame: 9 blocks of a held chord-ish tone (or silence), then the screen */
static void frame(int cap, const char *name)
{
 for (int b = 0; b < 9; b++) {
  int16_t buf[OP1_FX_BLOCK];
  for (int n = 0; n < OP1_FX_BLOCK; n++, t_samples++) {
   osc += 0x05000000u;
   buf[n] = (int16_t)((loud * host_sine[osc >> 19]) >> 15);
  }
  FX_juni_chorus.mono.render(0, buf, host_fx_knobs[0]);
 }
 capture = cap; first = 1; lines = 0; maxy = 0;
 if (cap) printf("{\"name\":\"%s\",\"ops\":[", name);
 SCREEN_juni_chorus.update(0, 26);
 if (cap) printf("],\"nlines\":%d}\n", lines);
 capture = 0;
}
static void knobs(int a, int b, int c, int d)
{
 host_fx_knobs[0][0] = (int16_t)a; host_fx_knobs[0][1] = (int16_t)b; host_fx_knobs[0][2] = (int16_t)c; host_fx_knobs[0][3] = (int16_t)d;
}
static void fresh(int a, int b, int c, int d)
{
 knobs(a, b, c, d);
 FX_juni_chorus.mono.init(0);
 SCREEN_juni_chorus.init(0);
}
/* the tree tops' x (top trunk sample of tree 0 and 1, Q10) and a reflection dash's x, after the last frame */
static int32_t snap[MAXL][4];
static int nsnap;
static void keep(void) { nsnap = lines < MAXL ? lines : MAXL; for (int i = 0; i < nsnap; i++) memcpy(snap[i], L[i], 16); }
static int geometry_differs(void)
{
 if (lines != nsnap) return 1;
 for (int i = 0; i < nsnap; i++) if (memcmp(snap[i], L[i], 16)) return 1;
 return 0;
}
/* lean of tree 0 now (its last trunk point's sway, Q8 px): from the screen's own state */
static int32_t lean0(void) { return lean[0]; }

int main(int argc, char **argv)
{
 for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
 const char *sc = argc > 1 ? argv[1] : "states";
 if (!strcmp(sc, "states")) {
  static const struct { const char *name; int k[4]; } st[] = {
   { "defaults", { 4608, 27852, 6000, 24575 } }, { "mode-1", { 0, 27852, 6000, 24575 } },
   { "mode-2", { 16384, 27852, 6000, 24575 } }, { "mode-1+2", { 32767, 27852, 6000, 24575 } },
   { "depth-min", { 16384, 0, 6000, 24575 } }, { "depth-max", { 16384, 32767, 6000, 24575 } },
   { "tone-min", { 16384, 27852, 0, 24575 } }, { "tone-max", { 16384, 27852, 32767, 24575 } },
   { "mix-0", { 16384, 27852, 6000, 0 } }, { "mix-max", { 16384, 32767, 6000, 32767 } },
   { "all-min", { 0, 0, 0, 0 } }, { "all-max", { 32767, 32767, 32767, 32767 } } };
  for (unsigned i = 0; i < sizeof st / sizeof st[0]; i++) {
   fresh(st[i].k[0], st[i].k[1], st[i].k[2], st[i].k[3]);
   for (int f = 0; f < 47; f++) frame(0, "");
   frame(1, st[i].name);
   assert(maxy <= 139 * 1024);
  }
  /* every phase of the sway at the largest lean stays inside the margin */
  fresh(16384, 32767, 6000, 32767);
  for (int f = 0; f < 200; f++) { frame(0, ""); assert(maxy <= 139 * 1024); }
 } else if (!strcmp(sc, "motion")) {
  /* MIX 0: still (after the easing settles) even with sound and the LFO running */
  fresh(16384, 32767, 6000, 0);
  for (int f = 0; f < 80; f++) frame(0, "");
  keep();
  for (int f = 0; f < 60; f++) { frame(0, ""); assert(!geometry_differs()); }
  /* MIX 99, DEPTH 99, mode II: the tops lean both ways by several pixels */
  fresh(16384, 32767, 6000, 32767);
  int32_t lo = 1 << 30, hi = -(1 << 30);
  for (int f = 0; f < 120; f++) { frame(0, ""); if (lean0() < lo) lo = lean0(); if (lean0() > hi) hi = lean0(); }
  fprintf(stderr, "lean at DEPTH 99, MIX 99 (mode II): %.2f .. %.2f px\n", lo / 256.0, hi / 256.0);
  assert(hi > 5 * 256 && lo < -5 * 256);
  /* DEPTH 0: smaller (the plugin's 20 % floor) */
  fresh(16384, 0, 6000, 32767);
  int32_t lo2 = 1 << 30, hi2 = -(1 << 30);
  for (int f = 0; f < 120; f++) { frame(0, ""); if (lean0() < lo2) lo2 = lean0(); if (lean0() > hi2) hi2 = lean0(); }
  fprintf(stderr, "lean at DEPTH 0: %.2f .. %.2f px\n", lo2 / 256.0, hi2 / 256.0);
  assert(hi2 - lo2 < (hi - lo) / 3 && hi2 - lo2 > 256);
  /* the rate follows MODE, at half the chorus LFO: sign changes of tree 0's lean over 10 s */
  int flips[3];
  static const int modes[3] = { 0, 16384, 32767 };
  for (int m = 0; m < 3; m++) {
   fresh(modes[m], 32767, 6000, 32767);
   for (int f = 0; f < 40; f++) frame(0, "");
   int n = 0, prev = lean0() >= 0;
   for (int f = 0; f < 385; f++) { frame(0, ""); int s = lean0() >= 0; n += s != prev; prev = s; }
   flips[m] = n;
  }
  fprintf(stderr, "sway direction changes in 10 s: I %d, II %d, I+II %d\n", flips[0], flips[1], flips[2]);
  assert(flips[0] < flips[1] && flips[1] < flips[2]);
  /* the ripple: louder input, bigger ripple; silence still leaves the LFO's gentle one */
  fresh(16384, 24000, 6000, 32767);
  for (int f = 0; f < 80; f++) frame(0, "");
  const int32_t rl = e_ripple;
  loud = 0;
  for (int f = 0; f < 120; f++) frame(0, "");
  const int32_t rq = e_ripple;
  loud = 80;                                /* a held note as the FX input sees it in op1emu (peak near 75) */
  for (int f = 0; f < 120; f++) frame(0, "");
  const int32_t rh = e_ripple;
  loud = 12000;
  fprintf(stderr, "ripple: %.2f px loud, %.2f px at a quiet held note (peak 80), %.2f px in silence\n", rl / 256.0, rh / 256.0, rq / 256.0);
  assert(rl > 2 * 256 && rq < rl / 2 && rq > 0 && rh > rq * 2 && rh < rl);
  /* TONE: the moon's colour and the halo change, the trees and water don't */
  fresh(16384, 24000, 0, 0);
  for (int f = 0; f < 80; f++) frame(0, "");
  keep();
  knobs(16384, 24000, 32767, 0);
  for (int f = 0; f < 80; f++) frame(0, "");
  int diff = 0, moved = 0;
  const int nm = __builtin_popcount(JC_MOON_SEEN);     /* the ring's lines first, then the halo's 11 x 2 */
  for (int i = 0; i < nsnap && i < lines; i++) {
   if (memcmp(snap[i], L[i], 16)) diff++;
   if (i >= nm && i < nm + 22 && memcmp(snap[i], L[i], 16)) moved++;
  }
  fprintf(stderr, "TONE 0 -> 99: %d lines moved, %d of them the halo\n", diff, moved);
  assert(diff == moved && moved > 0);
 } else if (!strcmp(sc, "strip")) {
  /* six frames 4 apart (about 0.1 s) while sound plays, defaults with MIX and DEPTH up: sway and ripple */
  fresh(16384, 32767, 6000, 32767);
  for (int f = 0; f < 60; f++) frame(0, "");
  for (int i = 0; i < 8; i++) {
   char name[16];
   snprintf(name, sizeof name, "strip-%d", i);
   for (int f = 0; f < 3; f++) frame(0, "");
   frame(1, name);
  }
 }
 return 0;
}
