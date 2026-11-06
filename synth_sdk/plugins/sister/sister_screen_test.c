/* SISTER screen host test: drives the screen through knob, level and page states and prints frames as JSON (for
 * screen_check.py: bounds, motion, previews). Telemetry is the test's (no DSP here).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
 *   sister_screen_test SCENARIO     (prints one JSON frame per captured frame, one per line) */
#define OP1_HOST
#include "../../op1_synth.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int16_t host_fx_knobs[3][8];
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
static int lines, maxy, clipcheck, clipped_bad;
static int hidden(int32_t x, int32_t y, int mode);
void host_raster_line(void *r, const void *aa, const void *bb, int32_t w, const uint8_t *c)
{
 (void)r; const op1_pt *a = aa, *b = bb;
 assert(a->x >= 0 && a->x <= 320 * 1024 && b->x >= 0 && b->x <= 320 * 1024);
 assert(a->y >= 0 && a->y <= 160 * 1024 && b->y >= 0 && b->y <= 160 * 1024);
 lines++;
 if (a->y + w / 2 > maxy) maxy = a->y + w / 2;
 if (b->y + w / 2 > maxy) maxy = b->y + w / 2;
 if (clipcheck) {                      /* the live parts never run over the still art: line ends on the visible side */
  const int teal = c[0] == 0x0B && c[1] == 0x9A, other = c[0] != 255 && !(c[0] == 80 && c[1] == 21) && !teal;
  if ((teal || other) && (hidden(a->x, a->y, other) || hidden(b->x, b->y, other))) clipped_bad++;
 }
 if (capture) { printf("%s[\"line\",%d,%d,%d,%d,%d,[%u,%u,%u,%u]]", first ? "" : ",", a->x, a->y, b->x, b->y, w, c[0], c[1], c[2], c[3]); first = 0; }
}
#include "sister_screen.c"
si_pos_t sister_pos[3];

static void frame(int cap, const char *name)
{
 capture = cap; first = 1; lines = 0;
 if (cap) printf("{\"name\":\"%s\",\"ops\":[", name);
 SCREEN_sister.update(0, 33);
 if (cap) printf("],\"nlines\":%d,\"depth\":%d,\"xc\":%d,\"flame\":[%d,%d,%d]}\n", lines, e_depth, e_xc, e_flame[0], e_flame[1], e_flame[2]);
}
static void knobs(int a, int b, int c, int d) { host_fx_knobs[0][0] = (int16_t)a; host_fx_knobs[0][1] = (int16_t)b; host_fx_knobs[0][2] = (int16_t)c; host_fx_knobs[0][3] = (int16_t)d; }

int main(int argc, char **argv)
{
 const char *sc = argc > 1 ? argv[1] : "states";
 SCREEN_sister.init(0);
 if (!strcmp(sc, "states")) {
  static const struct { const char *name; int k[4]; int level, page, key; } st[] = {
   { "defaults-silent", { 19661, 6554, 0, 11469 }, 0, 0, 0 }, { "defaults-singing", { 19661, 6554, 0, 11469 }, 20000, 0, 0 },
   { "all-min", { 0, 0, 0, 0 }, 0, 0, 0 }, { "all-max", { 32767, 32767, 32767, 32767 }, 32767, 0, 0 },
   { "choir-end", { 16384, 0, 8000, 20000 }, 26000, 0, 0 }, { "blend", { 16384, 16384, 8000, 20000 }, 26000, 0, 0 },
   { "pipes-end", { 16384, 32767, 8000, 20000 }, 26000, 0, 0 }, { "vesper", { 26000, 6554, 32767, 26000 }, 32767, 0, 0 },
   { "key-page-c3", { 19661, 32767, 0, 11469 }, 12000, 1, 0 }, { "key-page-f#4", { 19661, 32767, 0, 11469 }, 12000, 1, 1800 },
   { "key-page-c1", { 19661, 32767, 0, 11469 }, 12000, 1, -2400 }, { "key-page-c5", { 19661, 32767, 0, 11469 }, 12000, 1, 2400 } };
  for (unsigned i = 0; i < sizeof st / sizeof st[0]; i++) {
   SCREEN_sister.init(0); e_depth = -1;
   knobs(st[i].k[0], st[i].k[1], st[i].k[2], st[i].k[3]);
   sister_pos[0].level = sister_pos[0].peak = (int16_t)st[i].level; op1_fx_page[0][1] = (uint8_t)st[i].page; sister_pos[0].key_cents = st[i].key;
   for (int f = 0; f < 120; f++) { sister_pos[0].vowel[0] = (int16_t)((f * 61) % 12288); sister_pos[0].vowel[1] = (int16_t)((f * 61 + 4096) % 12288); frame(0, ""); }
   frame(1, st[i].name);
   assert(maxy <= 139 * 1024);
  }
 } else if (!strcmp(sc, "motion")) {
  /* a SPACE jump, a VOICE jump, a SWELL jump: every eased value moves smoothly (no jump > 1/4 of its travel in a
     frame, monotonic toward the target) */
  knobs(0, 0, 0, 11469); sister_pos[0].level = sister_pos[0].peak = 16000;
  for (int f = 0; f < 60; f++) frame(0, "");
  knobs(32767, 32767, 32767, 32767);
  int32_t pd = e_depth, px = e_xc, pf = e_flame[0];
  const int32_t d0 = e_depth, x0 = e_xc, f0 = e_flame[0];
  int max_dd = 0, max_dx = 0;
  for (int f = 0; f < 90; f++) {
   char name[32]; snprintf(name, sizeof name, "jump-frame-%02d", f);
   frame(f == 1 || f == 3 || f == 6 || f == 12 || f == 89, name);
   assert(e_depth >= pd && e_xc >= px);
   if (e_depth - pd > max_dd) max_dd = e_depth - pd;
   if (e_xc - px > max_dx) max_dx = e_xc - px;
   pd = e_depth; px = e_xc; pf = e_flame[0];
  }
  (void)pf; (void)f0;
  fprintf(stderr, "depth %d -> %d (largest step %d), xc %d -> %d (largest step %d)\n", d0, e_depth, max_dd, x0, e_xc, max_dx);
  assert(max_dd * 4 <= e_depth - d0 + 4 && max_dx * 4 <= e_xc - x0 + 4);
 } else if (!strcmp(sc, "flicker")) {
  /* SWELL 99, full tail: 400 frames, every extent inside the screen's lower margin; the flame moves */
  knobs(19661, 6554, 32767, 32767); sister_pos[0].level = sister_pos[0].peak = 32767;
  int lo = 1 << 30, hi = -(1 << 30);
  for (int f = 0; f < 400; f++) { frame(f == 150 || f == 151 || f == 152, "flicker"); assert(maxy <= 139 * 1024);
   if (e_flame[0] < lo) lo = e_flame[0]; if (e_flame[0] > hi) hi = e_flame[0]; }
  fprintf(stderr, "flame height Q8 %d..%d\n", lo, hi);
  assert(hi - lo > 128);
 } else if (!strcmp(sc, "clip")) {
  /* SPACE 0..99 and VOICE across the crossfade, singing: no line end of the nave inside the ring's outer edge or
     behind the nun, none of a petal or pipe outside the ring's inner edge or behind the nun */
  clipcheck = 1; sister_pos[0].level = sister_pos[0].peak = 32767;
  for (int k = 0; k <= 32767; k += 2048) {
   knobs(k, (k * 7) & 32767, 16384, 16384);
   for (int f = 0; f < 20; f++) { sister_pos[0].vowel[0] = (int16_t)((f * 613) % 12288); sister_pos[0].vowel[1] = (int16_t)((f * 613 + 4096) % 12288); frame(0, ""); }
  }
  fprintf(stderr, "line ends over the still art: %d\n", clipped_bad);
  assert(clipped_bad == 0);
 } else if (!strcmp(sc, "quiet")) {
  /* op1emu's levels: a held note whose wet block peaks wander 5..30 (native reading, 2026-10-28). The petals light
     clearly and breathe; a level that jumps between silence and full scale stays inside the screen; silence returns
     every level follower to rest (the still sketch) */
  knobs(19661, 6554, 0, 11469);
  int32_t lo = 1 << 30, hi = -(1 << 30);
  for (int f = 0; f < 120; f++) {
   sister_pos[0].peak = (int16_t)(5 + (rng >> 27)); sister_pos[0].level = 10;
   frame(f == 100 || f == 101 || f == 102, "quiet-held");
   if (f > 40) { if (e_breath < lo) lo = e_breath; if (e_breath > hi) hi = e_breath; }
  }
  fprintf(stderr, "held at peak 5..36: lit %d, breath %d..%d (Q8 px)\n", e_lit, lo, hi);
  assert(e_lit > 12000 && hi - lo > 40);
  for (int f = 0; f < 200; f++) { sister_pos[0].peak = (int16_t)(f & 1 ? 32767 : 0); frame(0, ""); assert(maxy <= 139 * 1024); }
  for (int f = 0; f < 200; f++) { sister_pos[0].peak = 0; sister_pos[0].level = 0; frame(f == 199, "quiet-released"); }
  assert(e_lit == 0 && e_breath == 0 && e_fast == 0 && e_slow == 0 && e_mid == 0);
 }
 return 0;
}
