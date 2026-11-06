/* C references of SISTER's kernels (sister_kern.h): the host build, and -DSI_NO_ASM on the Blackfin.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
 * 64-bit accumulators model the 40-bit hardware accumulator; this file is not the production Blackfin code. */
#include "sister_kern.h"
_Static_assert(SI_KERN_OFFSETS_OK || sizeof(void *) != 4, "kernel struct offsets");

static inline int32_t s16(int64_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int32_t)v; }
static inline int64_t p2(int32_t a, int32_t b) { return 2 * (int64_t)(int16_t)a * (int16_t)b; }   /* fractional */
static inline int32_t ext(int64_t acc) { return s16(acc >> 16); }                                  /* (T) */
static inline int32_t lo(int32_t v) { return (int16_t)v; }
static inline int32_t hi(int32_t v) { return (int16_t)(v >> 16); }

#if !defined(__bfin__) || defined(SI_NO_ASM) || defined(SI_C_LINE)
void si_k_line(si_line_t *p)
{
 int16_t *ring = p->ring;
 const int32_t len = (int32_t)(p->ring_bytes / 2);
 int32_t ri = (int32_t)(p->rd - ring), ar = (int32_t)(p->ap_rd - p->ap), aw = (int32_t)(p->ap_wr - p->ap);
 const int32_t kz = lo(p->kzka), ka = hi(p->kzka), kb = lo(p->kbg), g = hi(p->kbg);
 int32_t b = lo(p->b), z = lo(p->z);
 for (unsigned n = 0; n < 32; n++) {
  const int32_t a = ring[ri];
  if (++ri == len) ri = 0;
  z = ext(p2(z, kz) + p2(a, ka) + p2(b, kb));
  b = a;
  const int32_t d = p->ap[ar];
  ar = (ar + 1) & 511;
  const int32_t w = s16(z - ext(p2(d, g)));
  p->ap[aw] = (int16_t)w;
  aw = (aw + 1) & 511;
  p->out[n] = (int16_t)s16(d + ext(p2(w, g)));
 }
 p->z = z; p->b = b;
}
#endif

#if !defined(__bfin__) || defined(SI_NO_ASM) || defined(SI_C_RES)
void si_k_res(si_res_t *p)
{
 const int32_t b0 = lo(p->b0a1), a1 = hi(p->b0a1), a2 = lo(p->a2);
 int32_t y1 = hi(p->y), y2 = lo(p->y);
 for (unsigned n = 0; n < 32; n++) {
  int64_t acc = p2(p->dx[n], b0) + p2(y1, a1) + p2(y1, a1) - p2(y2, a2);
  const int32_t v = acc > 0x7FFFFFFF ? 0x7FFFFFFF : acc < -0x7FFFFFFF - 1 ? -0x7FFFFFFF - 1 : (int32_t)acc;
  const int32_t y = (v + ((v >> 31) & 0xFFFF)) >> 16;   /* toward zero: no limit cycle */
  y2 = y1; y1 = y;
  p->sum[n] = (int16_t)s16(p->sum[n] + y);
 }
 p->y = (int32_t)((uint32_t)(uint16_t)y1 << 16 | (uint16_t)y2);
}
#endif

#if !defined(__bfin__) || defined(SI_NO_ASM) || defined(SI_C_COMB)
void si_k_comb(si_comb_t *p)
{
 int16_t *ring = p->ring;
 const int32_t len = (int32_t)(p->ring_bytes / 2);
 int32_t ri = (int32_t)(p->rd - ring), wi = (int32_t)(p->wr - ring);
 const int32_t kz = lo(p->kzka), ka = hi(p->kzka), kb = lo(p->kbg), g = hi(p->kbg), q = lo(p->inq);
 int32_t b = lo(p->b), z = lo(p->z);
 for (unsigned n = 0; n < 32; n++) {
  const int32_t a = ring[ri];
  if (++ri == len) ri = 0;
  z = ext(0x8000 + p2(z, kz) + p2(a, ka) + p2(b, kb));       /* half up: with 1 - bright < 1/2 no deadband */
  b = a;
  const int64_t acc = p2(p->in[n], q) + p2(z, g);
  const int32_t v = acc > 0x7FFFFFFF ? 0x7FFFFFFF : acc < -0x7FFFFFFF - 1 ? -0x7FFFFFFF - 1 : (int32_t)acc;
  const int32_t o = (v + ((v >> 31) & 0xFFFF)) >> 16;   /* toward zero: a ring ends in silence */
  ring[wi] = (int16_t)o;
  if (++wi == len) wi = 0;
  p->y[n] = (int16_t)o;
 }
 p->z = z; p->b = b;
}
#endif

#if !defined(__bfin__) || defined(SI_NO_ASM) || defined(SI_C_MIX)
void si_k_mix(si_mix_t *p)
{
 int32_t wg = lo(p->g), ng = hi(p->g);
 const int32_t sw = lo(p->step), sn = hi(p->step);
 for (unsigned n = 0; n < 64; n++) {
  wg = lo(wg + sw); ng = lo(ng + sn);
  p->l[n] = (int16_t)s16(p->l[n] + ext(p2(p->wl[n], wg) - p2(p->l[n], ng)));
  p->r[n] = (int16_t)s16(p->r[n] + ext(p2(p->wr[n], wg) - p2(p->r[n], ng)));
 }
}
#endif

#if !defined(__bfin__) || defined(SI_NO_ASM) || defined(SI_C_MIXM)
void si_k_mixm(si_mix_t *p)
{
 int32_t wg = lo(p->g), ng = hi(p->g);
 const int32_t sw = lo(p->step), sn = hi(p->step);
 for (unsigned n = 0; n < 64; n++) {
  wg = lo(wg + sw); ng = lo(ng + sn);
  p->l[n] = (int16_t)s16(p->l[n] + ext(p2(p->wl[n], wg) - p2(p->l[n], ng)));
 }
}
#endif

#if !defined(__bfin__) || defined(SI_NO_ASM) || defined(SI_C_DECIM)
void si_k_decim(const int16_t *c, int16_t *x, const int16_t *h)
{
 for (unsigned k = 0; k < 32; k++, c += 2) {
  int64_t acc = p2(c[0], 16384);
  for (int j = 0; j < 5; j++) acc += p2(c[-(2 * j + 1)], h[j]) + p2(c[2 * j + 1], h[j]);
  x[k] = (int16_t)ext(acc);
 }
}
#endif

#if !defined(__bfin__) || defined(SI_NO_ASM) || defined(SI_C_INTERP)
void si_k_interp(const int16_t *c, int16_t *out, const int16_t *h2)
{
 for (unsigned k = 0; k < 32; k++, c++) {
  out[2 * k] = c[0];
  int64_t acc = 0;
  for (int j = 0; j < 5; j++) acc += p2(c[-j], h2[j]) + p2(c[1 + j], h2[j]);
  out[2 * k + 1] = (int16_t)ext(acc);
 }
}
#endif

#if !defined(__bfin__) || defined(SI_NO_ASM) || defined(SI_C_AP)
void si_k_ap(int16_t *ring, uint32_t wr, uint32_t rd, int32_t g, int16_t *x)
{
 for (unsigned n = 0; n < 32; n++) {
  const int32_t d = ring[rd];
  rd = (rd + 1) & 511;
  const int32_t v = s16(x[n] - ext(p2(d, g)));
  ring[wr] = (int16_t)v;
  wr = (wr + 1) & 511;
  x[n] = (int16_t)s16(d + ext(p2(v, g)));
 }
}
#endif

#if !defined(__bfin__) || defined(SI_NO_ASM) || defined(SI_C_SCAT)
void si_k_scatter(si_scat_t *p)
{
 int32_t wi[4];
 for (unsigned k = 0; k < 4; k++) wi[k] = (int32_t)(p->w[k] - p->base[k]);
 const int32_t g[4] = { lo(p->g01), hi(p->g01), lo(p->g23), hi(p->g23) };
 for (unsigned n = 0; n < 32; n++) {
  const int32_t a = p->o[n], b = p->o[32 + n], c = p->o[64 + n], d = p->o[96 + n];
  const int32_t s0 = a + c, d0 = a - c, s1 = b + d, d1 = b - d;
  p->tl[n] = (int16_t)(s0 >> 1); p->tr[n] = (int16_t)(s1 >> 1);
  const int32_t y[4] = { (s0 + s1) >> 2, (s0 - s1) >> 2, (d0 + d1) >> 2, (d0 - d1) >> 2 };
  const int32_t x = (p->in[n] + 2) >> 2;
  for (unsigned k = 0; k < 4; k++) {
   const int32_t q = y[k] * g[k], r = (q + ((q >> 31) & 0x3FFF)) >> 14;
   p->base[k][wi[k]] = (int16_t)s16(k & 1 ? r - x : r + x);
   if (++wi[k] == (int32_t)(p->bytes[k] / 2)) wi[k] = 0;
  }
 }
}
#endif
