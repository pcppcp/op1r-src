/* QUAD's Blackfin kernels (rv_quad_kern.S) against rv_quad.c's C references on bcore (quad_kern_check.py):
 * 1. every kernel on random and full-scale buffers and parameters, outputs and returns compared;
 * 2. the whole effect: qd_render (the kernels) against qdr_render (rv_quad.c built with -DQD_REF, the C loops) on
 *    the same input, every output sample and the whole state compared after every call: every preset with noise
 *    then silence and SIZE moving, presets switched every few calls on a dirty state, and full-scale alternation;
 * 3. the cost: CYCLES around qd_render per preset (bcore with --cycles counts packets; 0 elsewhere).
 * Prints "KERN ok ..." (and the cost lines) or the first difference.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include <stdint.h>
#include "rv_quad.h"
void qdr_select(qd_state *s, int preset);
void qdr_render(qd_state *s, const int16_t *in, int16_t *outl, int16_t *outr, int32_t size_q15);
void qk_biquad_c(int32_t *st, const int16_t *c, int16_t *x);
void qk_lerp_c(const int16_t *p, int16_t *out, int32_t f);
void qk_tap_c(const qk_tap_t *t);
void qk_xfade_c(const qk_xf_t *t);
void qk_apslot_c(const qk_aps_t *t);
void qk_aplerp_c(const qk_apl_t *t);
int32_t qk_damp_c(const qk_dmp_t *t);
int32_t qk_dlyw_c(const qk_dw_t *t);
void qk_acc_c(int32_t *a, const int16_t *v, int32_t neg);
void qk_mac_c(const qk_mac_t *t);
void qk_gadd_c(int16_t *o, const int16_t *x, int32_t g);
int32_t qk_rvout_c(const qk_ro_t *t);
void qk_sum4_c(const qk_sum_t *t);
void qk_sum7_c(const qk_sum_t *t);
void qk_pmix_c(int16_t *o, const int16_t *x, int32_t wd);
int32_t qk_rvin_c(const qk_in_t *t);
int32_t qk_bw_c(const qk_bw_t *t);
void t2_write(const char *, int);
uint32_t t2_r246_calls[2];
static qd_state A __attribute__((aligned(32))), B __attribute__((aligned(32)));
static char line[1024];
static int n, bad;
static uint32_t checks;
static void put(const char *p) { while (*p) line[n++] = *p++; }
static void num(uint32_t v) { char b[11]; int i = 10; b[i] = 0; do b[--i] = (char)('0' + v % 10); while (v /= 10); put(b + i); }
static int same(const void *a, const void *b, unsigned len)
{
 const uint8_t *x = a, *y = b;
 for (unsigned i = 0; i < len; i++) if (x[i] != y[i]) return 0;
 return 1;
}
static void fail(const char *what, uint32_t i) { if (!bad) { bad = 1; put("KERN differ "); put(what); put(" at "); num(i); put("\n"); } }
static uint32_t rng = 4242;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng; }
static int16_t r16(void) { uint32_t r = rnd(); uint32_t k = r & 15; return k == 0 ? 32767 : k == 1 ? -32768 : k == 2 ? -32767 : (int16_t)(r >> 16); }
static int16_t rq(int32_t lo, int32_t hi) { return (int16_t)(lo + (int32_t)((rnd() >> 8) % (uint32_t)(hi - lo + 1))); }
static void fill(int16_t *b, int cnt) { for (int i = 0; i < cnt; i++) b[i] = r16(); }

static int16_t P1[256], P2[256], X1[64], X2[64], Y1[64], Y2[64], V1[64], V2[64];
static int32_t A1[64], A2[64];
static void kernels(void)
{
 for (uint32_t it = 0; it < 400 && !bad; it++) {
  /* biquad: coefficients as the generator keeps them (sum |c| < 7.3 in Q13) */
  { int32_t s1[5], s2[5]; int16_t c[5];
    c[0] = rq(-12000, 12000); c[1] = rq(-16000, 16000); c[2] = rq(-12000, 12000); c[3] = rq(-16300, 16300); c[4] = rq(-8100, 8100);
    for (int k = 0; k < 4; k++) s1[k] = s2[k] = r16();
    s1[4] = s2[4] = (int32_t)(rnd() & 8191);
    fill(X1, 64); for (int k = 0; k < 64; k++) X2[k] = X1[k];
    qk_biquad(s1, c, X1); qk_biquad_c(s2, c, X2);
    if (!same(X1, X2, sizeof X1) || !same(s1, s2, sizeof s1)) fail("biquad", it); }
  /* lerp */
  { fill(P1, 66); int32_t f = rq(0, 255);
    qk_lerp(P1, X1, f); qk_lerp_c(P1, X2, f);
    if (!same(X1, X2, sizeof X1)) fail("lerp", it); }
  /* tap: d moving across a 200-sample span */
  { fill(P1, 256); int32_t d = (int32_t)(rq(1, 120)) << 16 | (int32_t)(rnd() & 65535), sd = (int32_t)(rnd() % 131073) - 65536;
    if ((d >> 16) + ((sd * 64) >> 16) < 1) sd = 0;
    qk_tap_t t = { P1 + 128, d, sd, X1 }, u = { P1 + 128, d, sd, X2 };
    qk_tap(&t); qk_tap_c(&u);
    if (!same(X1, X2, sizeof X1)) fail("tap", it); }
  /* xfade */
  { fill(X1, 64); for (int k = 0; k < 64; k++) X2[k] = X1[k]; fill(Y1, 64);
    qk_xf_t t = { X1, Y1, rnd(), rnd() >> (rnd() & 15) }, u = t; u.o = X2;
    qk_xfade(&t); qk_xfade_c(&u);
    if (!same(X1, X2, sizeof X1)) fail("xfade", it); }
  /* apslot */
  { fill(P1, 64); for (int k = 0; k < 64; k++) P2[k] = P1[k]; fill(X1, 64); for (int k = 0; k < 64; k++) X2[k] = X1[k];
    int32_t m = rq(1, 64), g = r16();
    qk_aps_t t = { P1, X1, m, g }, u = { P2, X2, m, g };
    qk_apslot(&t); qk_apslot_c(&u);
    if (!same(X1, X2, sizeof X1) || !same(P1, P2, 128)) fail("apslot", it); }
  /* aplerp */
  { fill(P1, 66); fill(X1, 64); fill(Y1, 64);
    int32_t f = (it & 3) ? rq(0, 255) : 0, dec = (it & 7) ? rq(0, 32767) : 0, g = r16();   /* dec >= 0 (the kernel's contract) */
    qk_apl_t t = { P1, f, X1, Y1, dec, V1, X2, g }, u = { P1, f, X1, Y1, dec, V2, Y2, g };
    qk_aplerp(&t); qk_aplerp_c(&u);
    if (!same(V1, V2, sizeof V1) || !same(X2, Y2, sizeof Y2)) fail("aplerp", it); }
  /* damp */
  { fill(P1, 66); int32_t f = rq(0, 255), z = r16(), dk = rq(0, 32767), dec = rq(0, 32767);
    qk_dmp_t t = { P1, f, X1, z, dk, dec }, u = { P1, f, X2, z, dk, dec };
    int32_t a = qk_damp(&t), b = qk_damp_c(&u);
    if (a != b || !same(X1, X2, sizeof X1)) fail("damp", it); }
  /* dlyw */
  { fill(Y1, 64); fill(V1, 64); int32_t z = r16(), k = rq(0, 32767), fb = rq(0, 32767);
    qk_dw_t t = { X1, Y1, V1, z, k, fb }, u = { X2, Y1, V1, z, k, fb };
    int32_t a = qk_dlyw(&t), b = qk_dlyw_c(&u);
    if (a != b || !same(X1, X2, sizeof X1)) fail("dlyw", it); }
  /* acc, mac */
  { for (int k = 0; k < 64; k++) A1[k] = A2[k] = (int32_t)rnd() >> 8;
    fill(V1, 64); int32_t neg = (int32_t)(rnd() & 1);
    qk_acc(A1, V1, neg); qk_acc_c(A2, V1, neg);
    qk_mac_t t = { A1, V1, r16(), rq(0, 15) }, u = t; u.a = A2;
    qk_mac(&t); qk_mac_c(&u);
    if (!same(A1, A2, sizeof A1)) fail("acc/mac", it); }
  /* gadd */
  { fill(X1, 64); for (int k = 0; k < 64; k++) X2[k] = X1[k]; fill(Y1, 64); int32_t g = r16();
    qk_gadd(X1, Y1, g); qk_gadd_c(X2, Y1, g);
    if (!same(X1, X2, sizeof X1)) fail("gadd", it); }
  /* rvout */
  { fill(X1, 64); for (int k = 0; k < 64; k++) X2[k] = X1[k];
    for (int k = 0; k < 64; k++) A1[k] = (int32_t)rnd() >> (12 + (rnd() & 3));
    int32_t g = rq(0, 32767), gs = (rq(0, 32767) - g) >> 6;
    qk_ro_t t = { X1, A1, rq(1, 13), rq(0, 12), g, gs, r16() }, u = t; u.o = X2;
    int32_t a = qk_rvout(&t), b = qk_rvout_c(&u);
    if (a != b || !same(X1, X2, sizeof X1)) fail("rvout", it); }
  /* sum4 (with and without add), sum7: gains as the callers keep them */
  { static int16_t S[8][64]; for (int k = 0; k < 8; k++) fill(S[k], 64);
    for (int k = 0; k < 64; k++) A1[k] = A2[k] = (int32_t)rnd() >> 4;
    qk_sum_t t = { { S[0], S[1], S[2], S[3], S[4], S[5], S[6], S[7] }, { 0 }, A1, rq(10, 15), (int32_t)(rnd() & 1) }, u;
    for (int k = 0; k < 2; k++) t.g[k] = (int32_t)((uint32_t)(uint16_t)rq(-13107, 13107) << 16 | (uint16_t)rq(-13107, 13107));
    u = t; u.a = A2;
    qk_sum4(&t); qk_sum4_c(&u);
    if (!same(A1, A2, sizeof A1)) fail("sum4", it);
    for (int k = 0; k < 4; k++) t.g[k] = (int32_t)((uint32_t)(uint16_t)rq(-8192, 8192) << 16 | (uint16_t)rq(-8192, 8192));
    u = t; u.a = A2;
    qk_sum7(&t); qk_sum7_c(&u);
    if (!same(A1, A2, sizeof A1)) fail("sum7", it); }
  /* pmix, rvin, bw */
  { fill(X1, 64); for (int k = 0; k < 64; k++) X2[k] = X1[k]; fill(Y1, 64);
    int32_t wd = (int32_t)((uint32_t)(uint16_t)r16() << 16 | (uint16_t)r16());
    qk_pmix(X1, Y1, wd); qk_pmix_c(X2, Y1, wd);
    if (!same(X1, X2, sizeof X1)) fail("pmix", it);
    fill(P1, 64); fill(P2, 64); fill(Y1, 64); fill(Y2, 64);
    qk_in_t a = { X1, P1, P2, Y1, Y2, r16() }, b = a; b.w = X2;
    int32_t ra = qk_rvin(&a), rb = qk_rvin_c(&b);
    if (ra != rb || !same(X1, X2, sizeof X1)) fail("rvin", it);
    qk_bw_t c = { P1, X1, r16(), rq(0, 32767) }, d = c; d.x = X2;
    ra = qk_bw(&c); rb = qk_bw_c(&d);
    if (ra != rb || !same(X1, X2, sizeof X1)) fail("bw", it); }
  checks++;
 }
}
static void clear(qd_state *s) { uint32_t *w = (uint32_t *)(void *)s; for (unsigned i = 0; i < sizeof *s / 4; i++) w[i] = 0; }
static uint32_t calls;
static void call(const int16_t *in, int32_t size, uint32_t what)
{
 int16_t l1[64], r1[64], l2[64], r2[64];
 qd_render(&A, in, l1, r1, size);
 qdr_render(&B, in, l2, r2, size);
 calls++;
 /* the persistent states differ only in p (each build points into its own preset table) */
 const qd_preset *pa = A.p, *pb = B.p; A.p = B.p = 0;
 if (!same(l1, l2, sizeof l1) || !same(r1, r2, sizeof r1)) fail(what == 1 ? "render (presets)" : what == 2 ? "render (switching)" : "render (full scale)", calls);
 else if (!same(&A, &B, __builtin_offsetof(qd_state, home))) fail("state", calls);   /* the work area holds pointers into each state */
 A.p = pa; B.p = pb;
}
static void whole(void)
{
 int16_t in[64];
 for (int p = 0; p < QD_PRESETS && !bad; p++) {          /* every preset: noise, silence, SIZE moving */
  clear(&A); clear(&B); qd_select(&A, p); qdr_select(&B, p);
  for (uint32_t c = 0; c < 260 && !bad; c++) {
   for (int k = 0; k < 64; k++) in[k] = c < 120 ? (int16_t)(rnd() >> 16) : 0;
   call(in, (int32_t)((c * 977u) & 32767), 1);
  }
 }
 for (uint32_t c = 0; c < 1500 && !bad; c++) {           /* presets switched on a dirty state, SIZE jumping */
  if (c % 7 == 0) { int p = (int)(rnd() % QD_PRESETS); qd_select(&A, p); qdr_select(&B, p); }
  for (int k = 0; k < 64; k++) in[k] = (int16_t)(rnd() >> 16);
  call(in, (int32_t)(rnd() & 32767), 2);
 }
 for (int p = 0; p < QD_PRESETS && !bad; p += 3) {       /* full-scale alternation: the saturating paths */
  clear(&A); clear(&B); qd_select(&A, p); qdr_select(&B, p);
  for (uint32_t c = 0; c < 60 && !bad; c++) {
   for (int k = 0; k < 64; k++) in[k] = (k & 1) ? 32767 : -32768;
   call(in, 32767, 3);
  }
 }
}
static void cost(void)
{
 static const uint8_t first[6] = { 0, 0, 30, 42, 54, 66 }, last[6] = { 0, 29, 41, 53, 65, 89 };
 uint32_t worst[6] = { 0 }, wp[6] = { 0 }, c0, c1;
 int16_t in[64], l[64], r[64];
 __asm__ volatile("%0 = CYCLES;" : "=d"(c0));
 for (volatile int k = 0; k < 10; k++) {}
 __asm__ volatile("%0 = CYCLES;" : "=d"(c1));
 if (c1 == c0) return;                                   /* no cycle counter in this emulator */
 for (int p = 0; p < QD_PRESETS; p++) {
  clear(&A); qd_select(&A, p);
  for (uint32_t c = 0; c < 240; c++) {             /* SIZE max, then SIZE swinging (every delay moving) */
   for (int k = 0; k < 64; k++) in[k] = (int16_t)(rnd() >> 16);
   const int32_t size = c < 120 ? 32767 : (c & 16 ? 32767 : 0);
   __asm__ volatile("%0 = CYCLES;" : "=d"(c0));
   qd_render(&A, in, l, r, size);
   __asm__ volatile("%0 = CYCLES;" : "=d"(c1));
   int cf = qd_config(p);
   if ((c % 120) >= 10 && c1 - c0 > worst[cf]) { worst[cf] = c1 - c0; wp[cf] = (uint32_t)p; }
  }
 }
 for (int cf = 1; cf <= 5; cf++) { put("COST config "); num((uint32_t)cf); put(" "); num(worst[cf]); put(" packets (preset "); num(wp[cf]); put(")\n"); }
 (void)first; (void)last;
}
int main(void)
{
 kernels();
 whole();
 if (!bad) { put("KERN ok kernels="); num(checks); put(" calls="); num(calls); put("\n"); }
 cost();
 t2_write(line, n);
 return 0;
}
