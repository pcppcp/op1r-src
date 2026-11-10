/* rackverb unit 4, QUAD: EQ, pitch (chorus/flange/detune/phaser), leslie, delay and reverb blocks in five fixed
 * chains, in the style of a late-80s 16-bit quad rack processor (our own design, rv_quad.h).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 *
 * Textbook references (no code copied): R. Bristow-Johnson, Audio EQ Cookbook (biquads, coefficients made by
 * gen_quad_presets.py); J. Dattorro, "Effect Design Part 1: Reverberator and Other Filters" (1997) (the figure-eight
 * tank, its output taps); Schroeder allpass diffusers; the two-tap crossfaded delay-ramp pitch shifter; the
 * first-order allpass phaser.
 *
 * Every stage runs at QD_RATE (44,100 Hz), QD_N = 64 samples a call, block-major: every ring delay is longer than a
 * block (gen_quad_presets.py checks), so each stage reads spans written in earlier calls and then writes its block.
 * A span that wraps its ring is copied to a small buffer first (span()), so the kernels walk plain pointers.
 * Integers only; no division, no libc, no table built at run time. The heavy loops are the qk_* kernels: Blackfin
 * assembly in rv_quad_kern.S, and C references here (qk_*_c), the same samples bit for bit (quad_kern_check.py runs
 * both on bcore). The host build and -DQD_REF use the C references.
 * Stack: the FX render can run on R246's 1,520-byte L1 kernel stack, so every row-sized buffer and kernel argument
 * block lives in a work area (qd_work, rv_quad.h): the caller's (rackverb.c: the L1 pool's partition, else its home)
 * through qd_render_w, or s->home through qd_render. bfin-elf-gcc -fstack-usage, 2026-11-06: qd_render_w 152 bytes
 * (the reverb, delay and pitch stages inlined), shift_side 16, dly_taps 16; the kernels push at most 28 (R7:4, P5:3).
 * The deepest chain, qd_render_w > shift_side or dly_taps > qk_tap, is 184 bytes.
 * Ring stores are preceded by a TOUCH of their lines (R246's data cache does not allocate on a write miss), except
 * the input allpasses', whose slots were just read.
 *
 * Cost, bcore packets per qd_render call (64 samples), worst preset per config, SIZE still or swinging
 * (quad_kern_check.py --cost, 2026-11-06): config 1 27,910 (29 AMBIENT WASH), 2 24,160 (40 LESLIE PING HALL),
 * 3 8,324 (53 RADIO SLAP), 4 12,404 (60 PHASER PING), 5 17,763 (70 WOOD ROOM). Packets are the smaller part: with
 * the calibrated cache / SDRAM model the worst block is about 2.7x its packets (quad_cpu.py, the README's Cost). The C references alone: config 1
 * 45,727, 2 40,627, 3 14,399, 4 15,065, 5 34,251 (SIZE max).
 */
#include "op1_plugin.h"
#include "rv_quad.h"
#include "rv_quad_presets.h"

#define TOUCH(p) ((void)*(volatile const int16_t *)(p))
#define N QD_N
#define PM (QD_PIT_LEN - 1)
_Static_assert((QD_PIT_LEN & PM) == 0 && QD_PIT_LEN % N == 0, "the pitch ring: a power of two, whole blocks");
_Static_assert(QD_DLY_LEN % N == 0 && QD_PRE_LEN % N == 0, "a block never wraps the delay or pre-delay ring");
_Static_assert(N == 64, "per-sample steps are (end - start) >> 6");
_Static_assert(__builtin_offsetof(qd_state, dl) % 32 == 0 && __builtin_offsetof(qd_state, dr) % 32 == 0 &&
               __builtin_offsetof(qd_state, pre) % 32 == 0 && __builtin_offsetof(qd_state, pit) % 32 == 0,
               "rings on 32-byte lines (the wrapper places the state on a 32-byte boundary)");

#define M(a, b) ((int32_t)(int16_t)(a) * (int16_t)(b))         /* operands in int16 range */
#define M15(a, b) (M(a, b) >> 15)
#define NI __attribute__((noinline))
/* 16-bit saturating ops: one instruction each on Blackfin (gcc's built-ins), the same results on the host */
#ifdef __bfin__
static inline int32_t sat(int32_t x) { return __builtin_bfin_shl_fr1x32(x, 16) >> 16; }
static inline int32_t adds(int32_t a, int32_t b) { return __builtin_bfin_add_fr1x16((int16_t)a, (int16_t)b); }
static inline int32_t subs(int32_t a, int32_t b) { return __builtin_bfin_sub_fr1x16((int16_t)a, (int16_t)b); }
static inline int32_t mulf(int32_t a, int32_t b) { return __builtin_bfin_mult_fr1x16((int16_t)a, (int16_t)b); }
#else
static inline int32_t sat(int32_t v) { v = v < -32768 ? -32768 : v; return v > 32767 ? 32767 : v; }
static inline int32_t adds(int32_t a, int32_t b) { return sat((int16_t)a + (int16_t)b); }
static inline int32_t subs(int32_t a, int32_t b) { return sat((int16_t)a - (int16_t)b); }
static inline int32_t mulf(int32_t a, int32_t b) { int32_t v = M(a, b); return v == 0x40000000 ? 32767 : v >> 15; }
#endif
/* Q15 product rounded toward zero (feedback gains): a loop at silence decays to exact zero */
static inline int32_t mz(int32_t a, int32_t b) { int32_t v = M(a, b); return (v + (int32_t)((uint32_t)(v >> 31) >> 17)) >> 15; }
/* one-pole step toward v: z' = v + (z - v) * k (k Q15) */
static inline int32_t pole(int32_t z, int32_t v, int32_t k) { return v + (M((z - v) >> 1, k) >> 14); }
/* (a (256 - f) + b f) / 256, f 0..255: a at the whole delay, b one sample older */
static inline int32_t lerp(int32_t a, int32_t b, int32_t f) { return (M(a, 256 - f) + M(b, f)) >> 8; }
static inline int32_t sine(uint32_t p)
{
 uint32_t i = p >> 24; int32_t a = qd_sin[i];
 return a + (M(qd_sin[i + 1] - a, (p >> 9) & 32767) >> 15);
}
typedef qd_ring_t ring_t;
/* cnt contiguous samples from slot at (any value: wrapped into the ring), in place or copied to tmp */
static const int16_t *span(const ring_t *g, int32_t at, int16_t *tmp, uint32_t cnt)
{
 while (at < 0) at += (int32_t)g->len;
 while ((uint32_t)at >= g->len) at -= (int32_t)g->len;
 const uint32_t a = (uint32_t)at;
 if (a + cnt <= g->len) return g->r + a;
 const uint32_t k = g->len - a;
 for (uint32_t i = 0; i < k; i++) tmp[i] = g->r[a + i];
 for (uint32_t i = k; i < cnt; i++) tmp[i] = g->r[i - k];
 return tmp;
}
static NI void copy(int16_t *o, const int16_t *x, uint32_t cnt) { for (uint32_t n = 0; n < cnt; n++) o[n] = x[n]; }
static void touch_block(const int16_t *w) { for (uint32_t i = 0; i < N; i += 16) TOUCH(w + i); }

/* ---- kernels: the C references (rv_quad_kern.S has the same loops for the Blackfin). N samples each. */
#if defined(__bfin__) && !defined(QD_REF)
#define QK(f) f
#else
#define QK(f) f##_c
#endif
#ifdef QD_EXPORT_C
#define QKC                          /* quad_kern_guest.c calls the references too */
#else
#define QKC static NI __attribute__((unused))
#endif
/* x in place: DF1 biquad, Q13 coefficients (b0 b1 b2 a1 a2), st = x1 x2 y1 y2 and the carried truncation error */
QKC void qk_biquad_c(int32_t *st, const int16_t *c, int16_t *x)
{
 int32_t x1 = st[0], x2 = st[1], y1 = st[2], y2 = st[3], e = st[4];
 const int32_t b0 = c[0], b1 = c[1], b2 = c[2], a1 = c[3], a2 = c[4];
 for (uint32_t n = 0; n < N; n++) {
  int32_t x0 = x[n], acc = M(b0, x0) + M(b1, x1) + M(b2, x2) - M(a1, y1) - M(a2, y2) + e;
  int32_t v = sat(acc >> 13);
  e = acc & 8191; x2 = x1; x1 = x0; y2 = y1; y1 = v; x[n] = (int16_t)v;
 }
 st[0] = x1; st[1] = x2; st[2] = y1; st[3] = y2; st[4] = e;
}
/* out[n] = lerp(p[n + 1], p[n], f) */
QKC void qk_lerp_c(const int16_t *p, int16_t *out, int32_t f)
{
 for (uint32_t n = 0; n < N; n++) out[n] = (int16_t)lerp(p[n + 1], p[n], f);
}
/* a moving tap: out[n] = lerp(base[n - D], base[n - D - 1], f), D = d >> 16, f = (d >> 8) & 255, d += sd */
QKC void qk_tap_c(const qk_tap_t *t)
{
 const int16_t *b = t->base; int32_t d = t->d; const int32_t sd = t->sd;
 for (uint32_t n = 0; n < N; n++) { const int16_t *a = b + n - (d >> 16); t->out[n] = (int16_t)lerp(a[0], a[-1], (d >> 8) & 255); d += sd; }
}
/* o = (o (32767 - tri(q)) + b tri(q)) >> 15, q += inc; tri: 0 .. 32767 .. 0 over q's cycle */
static inline int32_t tri(uint32_t p) { int32_t t = (int32_t)p; return (t ^ (t >> 31)) >> 16; }
QKC void qk_xfade_c(const qk_xf_t *t)
{
 uint32_t q = t->q;
 for (uint32_t n = 0; n < N; n++) { int32_t g = tri(q); t->o[n] = (int16_t)((M(t->o[n], 32767 - g) + M(t->b[n], g)) >> 15); q += t->inc; }
}
/* input allpass in place, m samples, the ring slot read and written: v = x - g d, y = d + g v */
QKC void qk_apslot_c(const qk_aps_t *t)
{
 int16_t *p = t->p, *x = t->x; const int32_t g = t->g;
 for (int32_t k = 0; k < t->m; k++) {
  int32_t d = p[k], v = subs(x[k], mulf(d, g));
  p[k] = (int16_t)v; x[k] = (int16_t)adds(d, mulf(v, g));
 }
}
/* tank allpass: in = x + mz(fb, dec), d = lerp(p[n + 1], p[n], f), v = in + g d, y = d - g v */
QKC void qk_aplerp_c(const qk_apl_t *t)
{
 const int16_t *p = t->p; const int32_t f = t->f, g = t->g, dec = t->dec;
 for (uint32_t n = 0; n < N; n++) {
  int32_t in = adds(t->x[n], mz(t->fb[n], dec)), d = lerp(p[n + 1], p[n], f), w = adds(in, mulf(d, g));
  t->v[n] = (int16_t)w; t->y[n] = (int16_t)subs(d, mulf(w, g));
 }
}
/* the tank's damping: z = pole(z, lerp(p[n + 1], p[n], f), dk), u = mz(z, dec); returns z */
QKC int32_t qk_damp_c(const qk_dmp_t *t)
{
 const int16_t *p = t->p; const int32_t f = t->f, dk = t->dk, dec = t->dec; int32_t z = t->z;
 for (uint32_t n = 0; n < N; n++) { z = pole(z, lerp(p[n + 1], p[n], f), dk); t->u[n] = (int16_t)mz(z, dec); }
 return z;
}
/* a delay's write: z = pole(z, v, k), w = x + mz(z, fb); returns z */
QKC int32_t qk_dlyw_c(const qk_dw_t *t)
{
 const int32_t k = t->k, fb = t->fb; int32_t z = t->z;
 for (uint32_t n = 0; n < N; n++) { z = pole(z, t->v[n], k); t->w[n] = (int16_t)adds(t->x[n], mz(z, fb)); }
 return z;
}
/* a += v (neg: a -= v) */
QKC void qk_acc_c(int32_t *a, const int16_t *v, int32_t neg)
{
 if (neg) for (uint32_t n = 0; n < N; n++) a[n] -= v[n];
 else for (uint32_t n = 0; n < N; n++) a[n] += v[n];
}
/* a += (g v) >> sh */
QKC void qk_mac_c(const qk_mac_t *t)
{
 const int32_t g = t->g, sh = t->sh;
 for (uint32_t n = 0; n < N; n++) t->a[n] += M(g, t->v[n]) >> sh;
}
/* o = sat(o + mulf(g, x)) */
QKC void qk_gadd_c(int16_t *o, const int16_t *x, int32_t g)
{
 for (uint32_t n = 0; n < N; n++) o[n] = (int16_t)adds(o[n], mulf(g, x[n]));
}
/* the reverb's output: o = sat(o + mulf(mulf(sat((a sc) >> sh), gain), lev)), gain += gs; returns the last gain */
QKC int32_t qk_rvout_c(const qk_ro_t *t)
{
 int32_t g = t->g;
 for (uint32_t n = 0; n < N; n++) { t->o[n] = (int16_t)adds(t->o[n], mulf(mulf(sat((t->a[n] * t->sc) >> t->sh), g), t->lev)); g += t->gs; }
 return g;
}
/* a = (add ? a : 0) + (sum of g_k p_k) >> sh over 4 (or 7) streams; the sum fits 32 bits (the callers' gains) */
static inline int32_t gk(const qk_sum_t *t, uint32_t k) { return (int16_t)(t->g[k >> 1] >> (16 * (k & 1))); }
static inline void qk_sum_c(const qk_sum_t *t, uint32_t cnt)
{
 for (uint32_t n = 0; n < N; n++) {
  int32_t v = 0;
  for (uint32_t k = 0; k < cnt; k++) v += M(gk(t, k), t->p[k][n]);
  t->a[n] = (t->add ? t->a[n] : 0) + (v >> t->sh);
 }
}
QKC void qk_sum4_c(const qk_sum_t *t) { qk_sum_c(t, 4); }
QKC void qk_sum7_c(const qk_sum_t *t) { qk_sum_t u = *t; u.sh = 13; u.add = 0; qk_sum_c(&u, 7); }   /* sh 13, add 0 */
/* the pitch block's output: o = adds(mulf(dry, x), mulf(wet, o)), wet_dry = wet << 16 | dry */
QKC void qk_pmix_c(int16_t *o, const int16_t *x, int32_t wd)
{
 const int32_t dry = (int16_t)wd, wet = wd >> 16;
 for (uint32_t n = 0; n < N; n++) o[n] = (int16_t)adds(mulf(dry, x[n]), mulf(wet, o[n]));
}
/* the reverb input: w = adds((al + ar) >> 1, mulf(dl, (yl + yr) >> 1)); returns the peak of w ^ (w >> 31) */
QKC int32_t qk_rvin_c(const qk_in_t *t)
{
 int32_t pk = 0;
 for (uint32_t n = 0; n < N; n++) {
  int32_t x = adds((t->al[n] + t->ar[n]) >> 1, mulf(t->dl, (t->yl[n] + t->yr[n]) >> 1));
  t->w[n] = (int16_t)x; x ^= x >> 31; pk = x > pk ? x : pk;     /* |x|, -1 for a negative x: 0 .. 32767 */
 }
 return pk;
}
/* the bandwidth one-pole: z = pole(z, q, k), x = z; returns z */
QKC int32_t qk_bw_c(const qk_bw_t *t)
{
 int32_t z = t->z;
 for (uint32_t n = 0; n < N; n++) { z = pole(z, t->q[n], t->k); t->x[n] = (int16_t)z; }
 return z;
}

/* ---- EQ */
static void eq(const qd_preset *p, qd_state *s, qd_work *wk, const int16_t *in)
{
 copy(wk->al, in, N);
 for (uint32_t k = 0; k < p->neq; k++) QK(qk_biquad)(s->eqs[k], p->eq[k], wk->al);
}

/* ---- PITCH. The pitch ring is stored twice (slot i and i + QD_PIT_LEN); a block is written at h (a multiple of
   N), so base = pit + h + QD_PIT_LEN gives the sample D before block sample n at base[n - D] for 0 <= D < QD_PIT_LEN
   without a mask. */
static NI void pit_write(int16_t *r, uint32_t h, const int16_t *x)
{
 int16_t *a = r + h, *b = a + QD_PIT_LEN;
 touch_block(a); touch_block(b);
 for (uint32_t n = 0; n < N; n++) { int16_t v = x[n]; a[n] = v; b[n] = v; }
}
static void tap(const int16_t *base, int32_t d, int32_t sd, int16_t *out)
{
 const qk_tap_t t = { base, d, sd, out };
 QK(qk_tap)(&t);
}
/* a modulated delay (Q8 centre and depth) at phase ph and ph1 (the call's end): Q16 start and per-sample step */
static inline void mdel(int32_t c, int32_t dp, uint32_t ph, uint32_t ph1, int32_t *d, int32_t *sd)
{
 const int32_t a = (c + ((dp * sine(ph)) >> 15)) * 256, b = (c + ((dp * sine(ph1)) >> 15)) * 256;
 *d = a; *sd = (b - a) >> 6;
}
/* flange L: the tap fed back, so tap and write alternate per sample (its delay can be shorter than a block) */
static NI void flange_l(int16_t *r, uint32_t h, int32_t d, int32_t sd, int32_t fb, int16_t *x)
{
 int16_t *a = r + h, *b = a + QD_PIT_LEN;
 touch_block(a); touch_block(b);
 for (uint32_t n = 0; n < N; n++) {
  const int16_t *q = b + n - (d >> 16);
  int32_t t = lerp(q[0], q[-1], (d >> 8) & 255);
  int16_t w = (int16_t)adds(x[n], mz(t, fb)); a[n] = w; b[n] = w;
  x[n] = (int16_t)t; d += sd;
 }
}
/* chorus / flange: an L and an R tap (wet into al, ar) */
static void p_mod(qd_state *s, qd_work *wk, const qd_preset *p)
{
 int16_t *r = s->pit; const uint32_t h = s->hp;
 const uint32_t ph = s->ph[0], ph1 = ph + p->prate * N, sp = (uint32_t)p->pspread << 16;
 int32_t dl, sl, dr, sr;
 mdel(p->pcen, p->pdep, ph, ph1, &dl, &sl); mdel(p->pcen, p->pdep, ph + sp, ph1 + sp, &dr, &sr);
 if (p->pfb) flange_l(r, h, dl, sl, p->pfb, wk->al);
 else { pit_write(r, h, wk->al); tap(r + h + QD_PIT_LEN, dl, sl, wk->al); }
 tap(r + h + QD_PIT_LEN, dr, sr, wk->ar);                 /* reads the finished block */
 s->hp = (h + N) & PM; s->ph[0] = ph1;
}
/* detune: per side two taps on a delay ramp half a window apart, crossfaded by triangles (L up, R down). A tap's
   delay is linear in the ramp, so it steps by a constant per sample (Q16); at the ramp's wrap (its gain ~0) the
   call's last samples run on past the window end by under a sample and the jump lands on the next call. */
static uint32_t shift_side(const int16_t *base, uint32_t q, uint32_t inc, int32_t c, int32_t win, int16_t *o, int16_t *b)
{
 const int32_t st = ((int32_t)inc * win) >> 16;              /* |inc| * win < 2^31: the generator */
 const uint32_t q2 = q + 0x80000000u;
 tap(base, c * 256 + (int32_t)((q >> 16) * (uint32_t)win), st, o);
 tap(base, c * 256 + (int32_t)((q2 >> 16) * (uint32_t)win), st, b);
 const qk_xf_t t = { o, b, q2, inc };
 QK(qk_xfade)(&t);
 return q + inc * N;
}
static void p_detune(qd_state *s, qd_work *wk, const qd_preset *p)
{
 int16_t *r = s->pit; const uint32_t h = s->hp;
 pit_write(r, h, wk->al);
 s->ramp[0] = shift_side(r + h + QD_PIT_LEN, s->ramp[0], p->prate, p->pcen, p->pdep, wk->al, wk->b);
 s->ramp[1] = shift_side(r + h + QD_PIT_LEN, s->ramp[1], p->prate2, p->pcen, p->pdep, wk->ar, wk->b);
 s->hp = (h + N) & PM;
}
/* phaser: six first-order allpasses y = x1 + a (x - y1), a swept per block; the wet into al and ar */
static NI void phaser_run(int16_t *st, int32_t a, int32_t fb, int32_t v0, int16_t *al, int16_t *ar)
{
 for (uint32_t n = 0; n < N; n++) {
  int32_t v = adds(al[n], mz(v0, fb));
  for (uint32_t k = 0; k < 12; k += 2) {
   int32_t y = sat(st[k] + (M(a, (v - st[k + 1]) >> 1) >> 14));
   st[k] = (int16_t)v; st[k + 1] = (int16_t)y; v = y;
  }
  v0 = v; al[n] = ar[n] = (int16_t)v;
 }
}
static void p_phaser(qd_state *s, qd_work *wk, const qd_preset *p)
{
 int32_t u = (sine(s->ph[0]) + 32767) >> 1; u = M15(u, u);
 const int32_t a = p->pa0 + M15(p->pa1 - p->pa0, u);
 s->ph[0] += p->prate * N;
 int16_t st[12];
 for (uint32_t k = 0; k < 6; k++) { st[2 * k] = (int16_t)s->aps[k][0]; st[2 * k + 1] = (int16_t)s->aps[k][1]; }
 phaser_run(st, a, p->pfb, s->pfbv, wk->al, wk->ar);
 s->pfbv = wk->al[N - 1];
 for (uint32_t k = 0; k < 6; k++) { s->aps[k][0] = st[2 * k]; s->aps[k][1] = st[2 * k + 1]; }
}
/* leslie: one-pole crossover; the horn through a Doppler tap per mic plus AM, the drum AM only */
static inline int32_t amg(int32_t depth, int32_t sn) { return 32767 - (M(depth, (32767 + sn) >> 1) >> 15); }
static NI int32_t xover(int32_t lp, int32_t xk, int16_t *x, int16_t *lo)
{
 for (uint32_t n = 0; n < N; n++) { int32_t v = x[n]; lp = pole(lp, v, xk); lo[n] = (int16_t)lp; x[n] = (int16_t)subs(v, lp); }
 return lp;
}
static NI void am_mix(int16_t *o, const int16_t *lo, int32_t g, int32_t sg, int32_t k, int32_t sk)
{
 for (uint32_t n = 0; n < N; n++) { o[n] = (int16_t)adds(mulf(o[n], g), mulf(lo[n], k)); g += sg; k += sk; }
}
static void p_leslie(qd_state *s, qd_work *wk, const qd_preset *p)
{
 int16_t *r = s->pit, *lo = wk->yl; const uint32_t h = s->hp;
 const uint32_t ph = s->ph[0], pd = s->ph[1], ph1 = ph + p->prate * N, pd1 = pd + p->prate2 * N;
 const uint32_t sp = (uint32_t)p->pspread << 16;
 const int32_t ha = p->pa0, da = p->pa1;
 /* crossover: the drum (low) into yl, the horn into the ring (its taps are >= 1 sample back) */
 s->xlp = xover(s->xlp, 32767 - p->px, wk->al, lo);
 pit_write(r, h, wk->al);
 for (uint32_t m = 0; m < 2; m++) {
  const uint32_t o = m ? sp : 0;
  const int32_t a0 = sine(ph + o), a1 = sine(ph1 + o), b0 = sine(pd + o), b1 = sine(pd1 + o);
  const int32_t g = amg(ha, a0), k = amg(da, b0);
  int32_t d, sd;
  mdel(p->pcen, p->pdep, ph + o, ph1 + o, &d, &sd);
  int16_t *out = m ? wk->ar : wk->al;
  tap(r + h + QD_PIT_LEN, d, sd, out);
  am_mix(out, lo, g, (amg(ha, a1) - g) >> 6, k, (amg(da, b1) - k) >> 6);
 }
 s->hp = (h + N) & PM; s->ph[0] = ph1; s->ph[1] = pd1;
}

/* ---- DELAY: mono, stereo, ping-pong; one-pole lowpass in the loop; times follow SIZE sample by sample.
   The taps are read first (every delay is longer than a block), then the block is written at h. */
static void dly_taps(int16_t *r, uint32_t h, int32_t q, int32_t st, int16_t *out, int16_t *tmp)
{
 const int32_t q1 = q + st * N;
 const int32_t dmax = (q > q1 ? q : q1) >> 8, dmin = (q < q1 ? q : q1) >> 8;   /* |q1 - q| <= 24 samples: tmp N + 32 */
 const ring_t g = { r, QD_DLY_LEN, 0 };
 const int16_t *p = span(&g, (int32_t)h - dmax - 1, tmp, (uint32_t)(N + 1 + dmax - dmin));
 if (st) tap(p + dmax + 1, q * 256, st * 256, out);
 else QK(qk_lerp)(p, out, q & 255);   /* SIZE still: the same samples, a fixed tap */
}
static int32_t dly_write(int16_t *w, const int16_t *x, const int16_t *v, int32_t z, int32_t k, int32_t fb)
{
 touch_block(w);
 const qk_dw_t t = { w, x, v, z, 32767 - k, fb };
 return QK(qk_dlyw)(&t);
}
static NI void mono(int16_t *m, const int16_t *a, const int16_t *b) { for (uint32_t n = 0; n < N; n++) m[n] = (int16_t)((a[n] + b[n]) >> 1); }
static void delay(qd_state *s, qd_work *wk, const qd_preset *p)
{
 int16_t *al = wk->al, *ar = wk->ar, *yl = wk->yl, *yr = wk->yr; const uint32_t h = s->hd;
 const int32_t ql = (p->dtl * s->szp) >> 7, qr = (p->dtr * s->szp) >> 7;     /* Q8 samples */
 const int32_t sl = (((p->dtl * s->sz) >> 7) - ql) >> 6, sr = (((p->dtr * s->sz) >> 7) - qr) >> 6;
 int16_t *m = wk->m;
 dly_taps(s->dl, h, ql, sl, yl, wk->tmp);
 if (p->dtype != QD_D_STEREO) mono(m, al, ar);
 if (p->dtype == QD_D_MONO) {
  s->dlp[0] = dly_write(s->dl + h, m, yl, s->dlp[0], p->ddamp, p->dfb);
  copy(yr, yl, N);
 } else {
  dly_taps(s->dr, h, qr, sr, yr, wk->tmp);
  if (p->dtype == QD_D_STEREO) {
   s->dlp[0] = dly_write(s->dl + h, al, yl, s->dlp[0], p->ddamp, p->dfb);
   s->dlp[1] = dly_write(s->dr + h, ar, yr, s->dlp[1], p->ddamp, p->dfbr);
  } else {                       /* ping-pong: in + R's filtered feedback into L, L's tap into R */
   s->dlp[1] = dly_write(s->dl + h, m, yr, s->dlp[1], p->ddamp, p->dfb);
   touch_block(s->dr + h); copy(s->dr + h, yl, N);
  }
 }
 s->hd = h + N == QD_DLY_LEN ? 0 : h + N;
}

/* ---- REVERB */
/* Dattorro's output taps: element and position (Q15 of its delay); 7 left then 7 right, signs + + - + - - - */
static const uint8_t tap_e[14] = { 9, 9, 10, 11, 5, 6, 7, 5, 5, 6, 7, 9, 10, 11 };
static const uint16_t tap_f[14] = { 2067, 23110, 23601, 20678, 14644, 3404, 9390,
                                    2598, 26690, 22355, 23545, 16404, 4133, 1257 };
/* early taps from the pre-delay ring: position (Q15 of rert), gain; 4 left then 4 right */
static const uint16_t er_f[8] = { 4260, 12124, 19988, 31785, 6881, 14746, 25231, 28836 };
static const int16_t er_g[8] = { 26214, -19661, 16384, -11469, 24576, -18022, 14746, -9830 };

/* N samples into the ring at its head; the head moves on */
static void put(ring_t *g, const int16_t *x)
{
 int16_t *r = g->r; uint32_t h = g->h, k = g->len - h;
 if (k >= N) { touch_block(r + h); copy(r + h, x, N); h += N; if (h == g->len) h = 0; }
 else { TOUCH(r + h); TOUCH(r + g->len - 1); touch_block(r); copy(r + h, x, k); copy(r, x + k, N - k); h = N - k; }
 g->h = h;
}
/* a block written straight into the ring when it does not wrap there, else into tmp and copied by wend */
static int16_t *wbeg(const ring_t *g, int16_t *tmp)
{
 if (g->h + N > g->len) return tmp;
 int16_t *w = g->r + g->h; touch_block(w); return w;
}
static void wend(ring_t *g, const int16_t *w, const int16_t *tmp)
{
 if (w == tmp) put(g, tmp);
 else { g->h += N; if (g->h == g->len) g->h = 0; }
}
/* the N + 1 samples ending at delay q (Q8): p[n + 1] is the sample q >> 8 before block sample n */
static const int16_t *at_delay(const ring_t *g, int32_t q, int16_t *tmp) { return span(g, (int32_t)g->h - (q >> 8) - 1, tmp, N + 1); }
/* input allpass, ring length == delay: read the slot, write it back */
static void ap_fixed(ring_t *g, int16_t *x, int32_t gn, qk_aps_t *t)
{
 const uint32_t h = g->h, k = g->len - h;
 t->p = g->r + h; t->x = x; t->g = gn;
 if (k > N) { t->m = N; QK(qk_apslot)(t); g->h = h + N; }
 else {
  t->m = (int32_t)k; QK(qk_apslot)(t);
  if (k < N) { t->p = g->r; t->x = x + k; t->m = (int32_t)(N - k); QK(qk_apslot)(t); }
  g->h = N - k;
 }
}
static void reverb(qd_state *s, qd_work *wk, const qd_preset *p, int16_t *outl, int16_t *outr)
{
 const uint32_t t = p->rtype;
 const uint16_t *L = qd_rvl[t];
 const uint32_t hp = s->hpre;
 const int32_t z1 = s->sz;
 ring_t *g = wk->g;
 const uint32_t ne = t == QD_R_REVERSE ? 5 : QD_RV_ELEMS;
 for (uint32_t e = 0; e < ne; e++) {     /* a head out of its ring (a state not yet cleared) restarts at 0 */
  g[e].r = s->rv + qd_rvw[t][e]; g[e].len = qd_rvr[t][e]; g[e].h = s->rh[e] < g[e].len ? s->rh[e] : 0;
 }
 /* the input into the pre-delay ring (a block never wraps: hp is a multiple of N) */
 touch_block(s->pre + hp);                /* without a delay the taps are al, ar at gain 0: the same samples */
 qk_in_t *ti = &wk->in;
 ti->w = s->pre + hp; ti->al = wk->al; ti->ar = wk->ar;
 ti->yl = p->dtype ? wk->yl : wk->al; ti->yr = p->dtype ? wk->yr : wk->ar; ti->dl = p->dtype ? p->dlev : 0;
 const int32_t pk = QK(qk_rvin)(ti);
 const ring_t pr = { s->pre, QD_PRE_LEN, hp };
 s->hpre = hp + N == QD_PRE_LEN ? 0 : hp + N;
 int32_t g0 = s->gg, g1 = 32767;
 if (p->gate) {
  if (pk > p->gthr) { g1 = 32767; s->ghold = p->ghold; }
  else if (s->ghold > 0) { s->ghold--; g1 = g0; }
  else g1 = mz(g0, p->grel);
  s->gg = g1;
 } else g0 = 32767;
 /* pre-delay (SIZE only), held for the call; the bandwidth filter; the input allpasses */
 const int32_t P = (p->rpre * z1) >> 15;
 int16_t *tmp = wk->tmp, *x = wk->x;
 int32_t *al = wk->al32, *ar = wk->ar32, sc, sh;
 { qk_bw_t *tb = &wk->bw; tb->q = span(&pr, (int32_t)hp - P, tmp, N); tb->x = x; tb->z = s->rbwz; tb->k = 32767 - p->rbw;
   s->rbwz = QK(qk_bw)(tb); }
 ap_fixed(&g[0], x, p->rdif1, &wk->aps); ap_fixed(&g[1], x, p->rdif1, &wk->aps);
 ap_fixed(&g[2], x, p->rdif2, &wk->aps); ap_fixed(&g[3], x, p->rdif2, &wk->aps);
 if (t == QD_R_REVERSE) {
  /* the diffused input on one long line, read by 12 taps per side whose gains rise with the delay */
  const uint32_t T = (uint32_t)((((L[4] < p->rrev ? L[4] : p->rrev) * p->rsize >> 15) * z1) >> 15);
  put(&g[4], x);
  int16_t (*tb)[N] = wk->t.tb;             /* 12 taps a side, gains rising with the delay: three sums of four */
  qk_sum_t *m = &wk->sum;
  for (uint32_t side = 0; side < 2; side++)
   for (uint32_t j = 0; j < 3; j++) {
    m->a = side ? ar : al; m->sh = 14; m->add = j > 0; m->g[2] = m->g[3] = 0;
    for (uint32_t i = 0; i < 4; i++) {
     const uint32_t k = 4 * j + i, o = side ? 1911u : 819u;
     m->p[i] = span(&g[4], (int32_t)g[4].h - N - (int32_t)((k * 2731u + o) * T >> 15), tb[i], N);
    }
    m->g[0] = (int32_t)((uint32_t)(qd_revg[4 * j + 1] >> 1) << 16 | (uint16_t)(qd_revg[4 * j] >> 1));
    m->g[1] = (int32_t)((uint32_t)(qd_revg[4 * j + 3] >> 1) << 16 | (uint16_t)(qd_revg[4 * j + 2] >> 1));
    QK(qk_sum4)(m);
   }
  sc = 1; sh = 0;
 } else {
  /* tank delays for this call (Q8 samples, preset scale x SIZE) */
  int32_t *q = wk->q;
  for (uint32_t e = 4; e < QD_RV_ELEMS; e++) q[e] = (((L[e] * p->rsize) >> 15) * z1) >> 7;
  const uint32_t pm = s->ph[2];
  s->ph[2] = pm + ((uint32_t)p->rmrate << 8) * N;
  const int32_t ma = M15(p->rmod, sine(pm)), mb = M15(p->rmod, sine(pm + 0x40000000u));
  const int32_t dec = p->rdec, dd1 = p->rddif1, dd2 = p->rddif2, dk = 32767 - p->rdamp;
  int16_t *fa = wk->fa, *fb = wk->fb, *u = wk->u;
  QK(qk_lerp)(at_delay(&g[7], q[7], tmp), fa, q[7] & 255);
  QK(qk_lerp)(at_delay(&g[11], q[11], tmp), fb, q[11] & 255);
  /* half A: modulated allpass, delay, damping, decay, allpass, delay; then half B. The delay line's output is read
     (and damped) before the allpass writes the line; the cross feedback was copied above. Each allpass reads its
     own ring ahead of its writes (the ring is at least 2 samples longer than the delay). */
  for (uint32_t h = 0; h < 2; h++) {
   const uint32_t o = 4 * h;
   ring_t *g4 = &g[4 + o], *g5 = &g[5 + o], *g6 = &g[6 + o], *g7 = &g[7 + o];
   int16_t *w4 = wk->t.w[0], *w5 = wk->t.w[1], *w6 = wk->t.w[2], *w7 = wk->t.w[3];
   const int32_t d5 = q[5 + o], d4 = q[4 + o] + (h ? mb : ma), d6 = q[6 + o] & ~255;
   qk_dmp_t *dm = &wk->dmp;
   dm->p = at_delay(g5, d5, tmp); dm->f = d5 & 255; dm->u = u; dm->z = s->rdz[h]; dm->dk = dk; dm->dec = dec;
   s->rdz[h] = QK(qk_damp)(dm);
   qk_apl_t *a = &wk->apl;
   int16_t *v4 = wbeg(g4, w4), *v5 = wbeg(g5, w5);
   a->p = at_delay(g4, d4, tmp); a->f = d4 & 255; a->x = x; a->fb = h ? fa : fb; a->dec = dec; a->v = v4; a->y = v5; a->g = dd1;
   QK(qk_aplerp)(a);
   wend(g4, v4, w4); wend(g5, v5, w5);
   int16_t *v6 = wbeg(g6, w6), *v7 = wbeg(g7, w7);
   a->p = at_delay(g6, d6, tmp); a->f = 0; a->x = u; a->fb = u; a->dec = 0; a->v = v6; a->y = v7; a->g = -dd2;
   QK(qk_aplerp)(a);
   wend(g6, v6, w6); wend(g7, v7, w7);
  }
  /* output taps, read after this call's writes (a tap's delay is >= 1 and <= its element's): seven a side at
     gain +-1 (8192 >> 13), then the early taps (room, chamber, hall) from the pre-delay ring, four a side */
  int16_t (*tb)[N] = wk->t.tb;
  qk_sum_t *m = &wk->sum;
  for (uint32_t side = 0; side < 2; side++) {
   m->g[0] = (int32_t)(8192u << 16 | 8192u); m->g[1] = (int32_t)(8192u << 16 | (uint16_t)-8192);
   m->g[2] = (int32_t)((uint32_t)(uint16_t)-8192 << 16 | (uint16_t)-8192); m->g[3] = (uint16_t)-8192;
   m->a = side ? ar : al; m->sh = 13; m->add = 0; m->p[7] = 0;
   for (uint32_t j = 0; j < 7; j++) {
    const uint32_t k = 7 * side + j; const ring_t *r = &g[tap_e[k]];
    m->p[j] = span(r, (int32_t)r->h - N - (((q[tap_e[k]] >> 8) * tap_f[k]) >> 15), tb[j], N);
   }
   QK(qk_sum7)(m);
   if (p->rer) {
    const int32_t ert = (p->rert * z1) >> 15;
    qk_sum_t *e = m;
    e->a = side ? ar : al; e->sh = 14; e->add = 1; e->g[2] = e->g[3] = 0;
    for (uint32_t j = 0; j < 4; j++) {
     const uint32_t k = 4 * side + j;
     e->p[j] = span(&pr, (int32_t)hp - P - ((ert * er_f[k]) >> 15), tb[j], N);
    }
    const int32_t g0e = M15(er_g[4 * side], p->rer) >> 1, g1e = M15(er_g[4 * side + 1], p->rer) >> 1;
    const int32_t g2e = M15(er_g[4 * side + 2], p->rer) >> 1, g3e = M15(er_g[4 * side + 3], p->rer) >> 1;
    e->g[0] = (int32_t)((uint32_t)(uint16_t)g1e << 16 | (uint16_t)g0e);
    e->g[1] = (int32_t)((uint32_t)(uint16_t)g3e << 16 | (uint16_t)g2e);
    QK(qk_sum4)(e);
   }
  }
  sc = 5; sh = 3;
 }
 /* gate, rlev, added into the output */
 const int32_t gs = (g1 - g0) >> 6;
 qk_ro_t *ro = &wk->ro;
 ro->o = outl; ro->a = al; ro->sc = sc; ro->sh = sh; ro->g = g0; ro->gs = gs; ro->lev = p->rlev;
 QK(qk_rvout)(ro);
 ro->o = outr; ro->a = ar; ro->g = g0;
 QK(qk_rvout)(ro);
 for (uint32_t e = 0; e < ne; e++) s->rh[e] = (uint16_t)g[e].h;
}

void qd_select(qd_state *s, int preset)
{
 if (preset < 0) preset = 0;
 if (preset >= QD_PRESETS) preset = QD_PRESETS - 1;
 s->p = &qd_presets[preset];
 s->sz = -1;
 s->ph[1] = 0x55555555u; s->ph[2] = 0x2aaaaaaau; s->ramp[1] = 0x80000000u;
}

void qd_render_w(qd_state *s, qd_work *wk, const int16_t *in, int16_t *outl, int16_t *outr, int32_t size_q15)
{
 const qd_preset *p = s->p;
 if (!p) { for (uint32_t n = 0; n < N; n++) outl[n] = outr[n] = 0; return; }
 if (size_q15 < 0) size_q15 = 0;
 if (size_q15 > 32767) size_q15 = 32767;
 const int32_t tg = 16384 + (size_q15 >> 1);
 /* keep every index in range whatever the state holds (rings stay safe while a clear is under way) */
 s->hp &= PM & ~(uint32_t)(N - 1);
 s->hd -= s->hd % N; if (s->hd >= QD_DLY_LEN) s->hd = 0;
 s->hpre -= s->hpre % N; if (s->hpre >= QD_PRE_LEN) s->hpre = 0;
 if (s->gg < 0 || s->gg > 32767) s->gg = 0;
 if (s->sz < 16384 || s->sz > 32767) s->sz = tg;
 s->szp = s->sz;
 { int32_t d = (tg - s->sz) >> 4; if (d > 24) d = 24; if (d < -24) d = -24;
   if (d == 0 && tg != s->sz) d = tg > s->sz ? 1 : -1;
   s->sz += d; }
 int16_t *al = wk->al, *ar = wk->ar;
 eq(p, s, wk, in);
 const uint32_t pt = p->ptype;
 if (pt == QD_P_NONE) copy(ar, al, N);
 else if (pt == QD_P_LESLIE) p_leslie(s, wk, p);
 else {
  int16_t *x = wk->ql;                       /* the dry input, for the mix (ql is free) */
  copy(x, al, N);
  if (pt == QD_P_DETUNE) p_detune(s, wk, p); else if (pt == QD_P_PHASER) p_phaser(s, wk, p); else p_mod(s, wk, p);
  const int32_t wd = (int32_t)((uint32_t)(uint16_t)p->pwet << 16 | (uint16_t)p->pdry);
  QK(qk_pmix)(al, x, wd);
  QK(qk_pmix)(ar, x, pt == QD_P_PHASER ? (int32_t)((uint32_t)(uint16_t)-p->pwet << 16 | (uint16_t)p->pdry) : wd);
 }
 if (p->dtype) delay(s, wk, p);
 /* out = dir * pre + dlev * delay taps + rlev * reverb */
 for (uint32_t n = 0; n < N; n++) outl[n] = outr[n] = 0;
 if (p->dir) { QK(qk_gadd)(outl, al, p->dir); QK(qk_gadd)(outr, ar, p->dir); }
 if (p->dtype) { QK(qk_gadd)(outl, wk->yl, p->dlev); QK(qk_gadd)(outr, wk->yr, p->dlev); }
 if (p->rtype) reverb(s, wk, p, outl, outr);
}

void qd_render(qd_state *s, const int16_t *in, int16_t *outl, int16_t *outr, int32_t size_q15) { qd_render_w(s, &s->home, in, outl, outr, size_q15); }

const char *qd_name(int preset) { return preset >= 0 && preset < QD_PRESETS ? qd_names[preset] : ""; }
int qd_config(int preset) { return preset >= 0 && preset < QD_PRESETS ? qd_presets[preset].cfg : 0; }
