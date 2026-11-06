/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
 *
 * SISTER: a cathedral reverb whose tail sings (CHOIR: drifting vowel formants), rings in tune (PIPES: root,
 * fifth and octave combs) and swells backwards (VESPER: reversed windows of the tail).
 *
 * Block flow (64 samples in at SI_FS, wet path at SI_FI = SI_FS / 2, 32 samples):
 *   mono sum -> half-band decimator -> pre-delay (+ early taps) -> 4 input allpasses ->
 *   4-line FDN (Hadamard scattering, slowly modulated reads, damping, an allpass in each line) ->
 *   tail L/R -> body + early + CHOIR (two 3-band formant resonators) + PIPES (three combs) = forward wet ->
 *   reverse ring -> crossfaded reversed windows -> forward / reverse blend -> brightness tilt -> DC remover ->
 *   block limiter -> half-band interpolator -> dry/wet mix at SI_FS.
 * Integers only, no division per sample, no libc. The per-sample loops that cost most are Blackfin kernels
 * (sister_asm.S; C references with the same samples in sister_kern.c). Per-block work (parameters, delays,
 * coefficients, level followers) is C. State and workspace live in the position's FX scratch.
 * Textbook references (no code copied): J. O. Smith, Physical Audio Signal Processing (FDN reverberation, comb
 * and allpass filters, the constant-peak-gain resonator); Peterson and Barney 1952 (vowel formants).
 */
#include "sister_dsp.h"
#include "sister_kern.h"
#include <stddef.h>

_Static_assert(SI_LINE_MOD > 12, "tank modulation room");
_Static_assert(2 * (SI_W_MAX + SI_F_MAX) + SI_DELTA_MAX + 32 < SI_REV, "reverse ring too short");
_Static_assert(SI_PRE_MAX + 1361 + SI_N < SI_PRE, "pre-delay ring too short");
_Static_assert(SI_PRE % SI_N == 0, "the pre-delay write never wraps within a block");
_Static_assert(sizeof(si_state) % 4 == 0, "state size");
_Static_assert(offsetof(si_state, wm) % 4 == 0 && offsetof(si_ws, db) % 4 == 0, "decimator taps must be word-aligned");

#define M(a, b) ((int32_t)(int16_t)(a) * (int16_t)(b))
#define M15(a, b) (M(a, b) >> 15)
/* signal-path products and halvings round half up: a decaying signal reaches 0 instead of hanging at -1 */
#define M15R(a, b) ((M(a, b) + 16384) >> 15)
#define M14R(a, b) ((M(a, b) + 8192) >> 14)
#define HALF(v) (((v) + 1) >> 1)
static inline int32_t sat(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static inline int32_t clamp15(int32_t v) { return v > 32767 ? 32767 : v < 0 ? 0 : v; }
static inline int32_t pack(int32_t lo, int32_t hi) { return (int32_t)((uint32_t)(uint16_t)hi << 16 | (uint16_t)lo); }

static inline int32_t lerp_tab(const int32_t *row0, unsigned stride, unsigned col, int32_t v)
{
 /* v: 0..32767 -> row v >> 8 of 129, linear between rows */
 unsigned i = (unsigned)v >> 8; int32_t f = v & 255;
 const int32_t a = row0[i * stride + col], b = row0[(i + (i < 128)) * stride + col];
 return a + (((b - a) * f) >> 8);
}
#define SPACE(c, v) lerp_tab(&si_space[0][0], 9, (c), (v))
#define VOICE(c, v) lerp_tab(&si_voice[0][0], 8, (c), (v))
#define SWELL(c, v) lerp_tab(&si_swell[0][0], 4, (c), (v))
#define MIX(c, v) lerp_tab(&si_mix[0][0], 3, (c), (v))

#ifdef SI_PROF
static inline uint32_t cyc(void) { uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c)); return c; }
#define PROF(i) do { const uint32_t t_ = cyc(); s->prof[i] += t_ - prof_t; prof_t = t_; } while (0)
#else
#define PROF(i) do { } while (0)
#endif

static inline int32_t tri(uint32_t ph) { return (int32_t)(((ph >> 15) ^ (0u - (ph >> 31))) & 65535) - 32768; }
static inline int32_t sine(const int16_t *tab, uint32_t ph)
{
 unsigned i = ph >> 24; int32_t f = (ph >> 16) & 255;
 return tab[i] + (((tab[i + 1] - tab[i]) * f) >> 8);
}
/* 2^(-e), e in Q32 of [0, 1) -> Q15 */
static inline int32_t exp2neg(const int16_t *tab, uint32_t e)
{
 unsigned i = e >> 24; int32_t f = (e >> 16) & 255;
 return tab[i] + (((tab[i + 1] - tab[i]) * f) >> 8);
}
/* toward zero: a recursion's rounding must not hold a DC */
static inline int32_t rz14(int32_t v) { return (v + ((v >> 31) & 16383)) >> 14; }

/* 32 samples of a power-of-two ring from index start on, unwrapped */
static void ring_read(const int16_t *ring, uint32_t mask, uint32_t start, int16_t *dst)
{
 start &= mask;
 unsigned n1 = mask + 1 - start;
 if (n1 > SI_N) n1 = SI_N;
 const int16_t *p = ring + start;
 for (unsigned i = 0; i < n1; i++) dst[i] = p[i];
 for (unsigned i = n1; i < SI_N; i++) dst[i] = ring[i - n1];
}
/* n samples of the reverse ring backwards from index start (dst[j] = ring[start - j]) */
static void rev_read(const int16_t *ring, int32_t start, int16_t *dst, unsigned n)
{
 if (start < 0) start += SI_REV;
 unsigned n1 = (unsigned)start + 1;
 if (n1 > n) n1 = n;
 const int16_t *p = ring + start;
 for (unsigned j = 0; j < n1; j++) dst[j] = p[-(int)j];
 p = ring + SI_REV - 1;
 for (unsigned j = n1; j < n; j++) dst[j] = p[-(int)(j - n1)];
}

static const uint32_t si_rate[4] = { 0x00076000u, 0x000A3000u, 0x000C8000u, 0x000FE000u };   /* tank LFOs, ~0.31..0.67 Hz */
static const si_tp si_tp_rom = { si_fco, si_vowel, si_root, si_rate, si_hb, si_hb2, si_quarter, si_exp2, si_sq1mg, si_sine,
                                 si_line_len, si_lap_len, si_diff_len };

static int32_t line_base(const uint16_t *len, unsigned i)
{
 int32_t b = 0;
 for (unsigned j = 0; j < i; j++) b += len[j] + SI_LINE_MOD + 2;
 return b;
}

void si_init(si_state *s)
{
 /* runs in the audio callback: clear only the scalar state and workspace here (32-bit stores); the rings
    are cleared SI_CLEAR_WORDS per block in si_process while the wet path stays silent. */
 uint32_t *p = (uint32_t *)(void *)s, n = (uint32_t)((char *)s->pre - (char *)s) / 4;
#ifdef SI_PROF
 p += 12; n -= 12;                /* keep the profile across re-selects */
#endif
 for (uint32_t i = 0; i < n; i++) p[i] = 0;
 s->first = 1; s->lim = 16384;
 s->parm.key[0] = -1;
 for (unsigned i = 0; i < 4; i++) s->lfo[i] = 0x9E3779B9u * (i + 1u);
 s->vlfo[1] = 0x55555555u;
}

/* slow level follower for a resonator bank: per block peaks of its input (the tail) and of its output (with the
   current gain applied); the gain (Q12, 1/16 .. 4) moves 1/8 of the way per block to where out matches in. It sits
   in the bank's input coefficient, so the per-sample code doesn't see it. */
static void follow(si_follow_t *f, int32_t pin, int32_t pout)
{
 if (!f->agc) { f->agc = 4096; f->in = f->out = 0; }
 f->in += (pin * 16 - f->in) >> 5;
 f->out += (pout * 16 - f->out) >> 5;
 int32_t t = f->agc;                                              /* too quiet to measure: hold */
 if (f->out > 1024 && f->in > 256) {
  t = (((f->in >> 4) * (f->agc >> 4)) / (f->out >> 4)) << 4;     /* once per block */
  if (t > 16384) t = 16384;
  if (t < 256) t = 256;
 }
 f->agc += (t - f->agc) >> 3;
}

/* the tank: four lines (each: a slowly modulated read folded into the damping, then its allpass), then the
   Hadamard scattering writes them back with their decay gains and the diffused input */
static void tank(si_state *s, const int32_t gains[4], int32_t damp, int32_t depth)
{
 const si_tp *t = s->t;
 int16_t *ring[4];
 int32_t R[4];
 const uint32_t aph = s->ap_h;
 si_line_t ln;
 for (unsigned i = 0; i < 4; i++) {
  int16_t *rg = ring[i] = s->line + line_base(t->line_len, i);
  const int32_t Ri = R[i] = t->line_len[i] + SI_LINE_MOD + 2;
  const uint32_t ph = s->lfo[i] + t->rate[i];
  s->lfo[i] = ph;
  /* the delay moves less than 0.1 sample per block: one delay and fraction per block */
  const int32_t dq = (int32_t)t->line_len[i] * 65536 + ((tri(ph) * depth) >> 7);   /* depth Q8 -> Q16 */
  const int32_t di = dq >> 16, fr = (dq & 0xFFFF) >> 1;
  int32_t idx = s->line_h[i] - di;
  if (idx < 0) idx += Ri;
  ln.ring = rg; ln.ring_bytes = (uint32_t)Ri * 2; ln.rd = rg + idx;
  ln.b = rg[idx ? idx - 1 : Ri - 1];
  ln.ap = s->lap[i]; ln.ap_wr = s->lap[i] + (aph & (SI_AP - 1)); ln.ap_rd = s->lap[i] + ((aph - t->lap_len[i]) & (SI_AP - 1));
  ln.out = s->w->o[i];
  ln.kzka = pack(clamp15(32768 - damp), M15(32767 - fr, damp));
  ln.kbg = pack(M15(fr, damp), 14746);                                              /* allpass 0.45 */
  ln.z = s->damp[i];
  si_k_line(&ln);
  s->damp[i] = ln.z;
 }
 si_scat_t sc;
 sc.o = s->w->o[0]; sc.in = s->w->x; sc.tl = s->w->tl; sc.tr = s->w->tr;
 for (unsigned i = 0; i < 4; i++) {
  sc.base[i] = ring[i]; sc.bytes[i] = (uint32_t)R[i] * 2; sc.w[i] = ring[i] + s->line_h[i];
  int32_t h = s->line_h[i] + SI_N;
  if (h >= R[i]) h -= R[i];
  s->line_h[i] = h;
 }
 sc.g01 = pack(gains[0], gains[1]); sc.g23 = pack(gains[2], gains[3]);   /* Q15 g read as Q14 2g: the 1/2 Hadamard */
 si_k_scatter(&sc);
 /* the tail leaves through a DC remover (per block, ~4 Hz): the tank's rounding holds a DC of a few LSB that the
    resonators would ring on; and +-1 LSB is the long loops' rounding limit cycle, not tail: it stays inside */
 int16_t *tl = s->w->tl, *tr = s->w->tr;
 int32_t sl = 0, sr = 0;
 for (unsigned n = 0; n < SI_N; n++) { sl += tl[n]; sr += tr[n]; }
 s->tdc[0] += (sl - s->tdc[0]) >> 4; s->tdc[1] += (sr - s->tdc[1]) >> 4;   /* 32 x the mean */
 const int32_t ml = (s->tdc[0] + 16) >> 5, mr = (s->tdc[1] + 16) >> 5;
 for (unsigned n = 0; n < SI_N; n++) {
  const int32_t yl = tl[n] - ml, yr = tr[n] - mr;
  tl[n] = (int16_t)((uint32_t)(yl + 1) <= 2u ? 0 : sat(yl));
  tr[n] = (int16_t)((uint32_t)(yr + 1) <= 2u ? 0 : sat(yr));
 }
}

/* CHOIR: two 3-band formant resonator banks (left on tl, right on tr), each vowel drifting */
static void choir(si_state *s, int32_t vrate, int32_t vdepth, int32_t qinv, int32_t g0, int32_t g1)
{
 const int32_t agc = s->choir_lv.agc ? s->choir_lv.agc : 4096;
 int32_t pin = 0, pout = 0;
 for (unsigned v = 0; v < 2; v++) {
  s->vlfo[v] += v ? vrate + (vrate >> 3) : vrate;
  int32_t p = (sine(s->t->sine, s->vlfo[v]) * vdepth) >> 15;          /* Q12 vowel position around aah */
  if (p < 0) p += 3 * 4096;
  s->vowel[v] = p;
  const int16_t *in = v ? s->w->tr : s->w->tl;
  int16_t *sum = s->w->rsum[v], *dx = s->w->dx;
  int32_t x1 = s->res_x[v][0], x2 = s->res_x[v][1], pk = 0;
  for (unsigned n = 0; n < SI_N; n++) {
   const int32_t a = in[n], x = HALF(a);
   dx[n] = (int16_t)(x - x2); x2 = x1; x1 = x;
   sum[n] = 0;
   const int32_t m = a < 0 ? -a : a;
   if (m > pk) pk = m;
  }
  s->res_x[v][0] = x1; s->res_x[v][1] = x2;
  if (pk > pin) pin = pk;
  /* the quiet gate: no input, no resonator state (it would only hold a rounding limit cycle) */
  if (pk <= 4) { s->res[v][0] = s->res[v][1] = s->res[v][2] = 0; continue; }
  const unsigned i = (unsigned)p >> 12, j = i == 2 ? 0 : i + 1;
  const int32_t fr = p & 4095;
  si_res_t rb;
  rb.dx = dx; rb.sum = sum; rb.unused = 0;
  for (unsigned k = 0; k < 3; k++) {
   const int32_t (*vw)[3][2] = s->t->vowel, (*fco)[2] = s->t->fco;
   int32_t lg = vw[i][k][0] + (((vw[j][k][0] - vw[i][k][0]) * fr) >> 12);
   const int32_t gk = vw[i][k][1] + (((vw[j][k][1] - vw[i][k][1]) * fr) >> 12);
   if (lg < 0) lg = 0;
   unsigned li = (unsigned)lg >> 8; int32_t lf = lg & 255;
   if (li >= SI_FLOG_N - 1) { li = SI_FLOG_N - 2; lf = 255; }
   const int32_t c2 = fco[li][0] + (((fco[li + 1][0] - fco[li][0]) * lf) >> 8);
   const int32_t ek = fco[li][1] + (((fco[li + 1][1] - fco[li][1]) * lf) >> 8);
   const int32_t q = k == 2 ? qinv + (qinv >> 1) : qinv;
   const int32_t rad = exp2neg(s->t->exp2, (uint32_t)((ek >> 8) * q) << 2);   /* pole radius exp(-pi BW / FI), Q15 */
   const int32_t a1 = (rad * c2) >> 15;                           /* r cos(theta), Q15 (c2 = 2 cos in Q14) */
   const int32_t a2 = (rad * rad) >> 15;
   const int32_t b0 = clamp15((((((32768 - a2) >> 1) * gk) >> 15) * agc) >> 12);   /* level follower inside */
   rb.b0a1 = pack(b0, a1); rb.a2 = a2; rb.y = s->res[v][k];
   si_k_res(&rb);
   s->res[v][k] = rb.y;
  }
  for (unsigned n = 0; n < SI_N; n++) { const int32_t a = sum[n] < 0 ? -sum[n] : sum[n]; if (a > pout) pout = a; }
 }
 /* level follower: a source whose partials sit on the formants would ring up much louder than one that misses
    them; keep the choir near the tail's level */
 follow(&s->choir_lv, pin, pout * 3);
 int32_t g = g0 << 5;
 const int32_t gs = g1 - g0;
 int32_t *fl = s->w->fl, *fr = s->w->fr;
 const int16_t *sl = s->w->rsum[0], *sr = s->w->rsum[1];
 for (unsigned n = 0; n < SI_N; n++) { g += gs; const int32_t gg = g >> 5; fl[n] += M14R(sl[n], gg); fr[n] += M14R(sr[n], gg); }
}

/* PIPES: three tuned combs (root, fifth, octave) on the mono tail, panned into fl / fr */
static void pipes(si_state *s, int32_t klog, int32_t bright, int32_t g0, int32_t g1)
{
 /* target delays from the key: root table per semitone, cents interpolated (per block) */
 int32_t c = s->key_cents + 2400;
 if (c < 0) c = 0;
 if (c > 4800) c = 4800;
 int32_t semi = c / 100, cents = c - semi * 100;
 if (semi >= 48) { semi = 47; cents = 100; }
 const si_tp *tb = s->t;
 const int32_t root = tb->root[semi] + ((tb->root[semi + 1] - tb->root[semi]) * cents) / 100;
 const int32_t target[3] = { root, (root * 2) / 3, root >> 1 };
 int16_t *tm = s->w->rl, *ys[3] = { s->w->o[0], s->w->o[1], s->w->o[2] };   /* free workspace at this point */
 int32_t pin = 0;
 for (unsigned n = 0; n < SI_N; n++) {
  const int32_t v = HALF(s->w->tl[n] + s->w->tr[n]), m = v < 0 ? -v : v;
  tm[n] = (int16_t)v;
  if (m > pin) pin = m;
 }
 const int32_t agc = s->pipe_lv.agc ? s->pipe_lv.agc : 4096;
 si_comb_t cb;
 for (unsigned i = 0; i < 3; i++) {
  int32_t d = s->comb_d[i] ? s->comb_d[i] : target[i];
  const int32_t t = target[i], diff = t - d, adiff = diff < 0 ? -diff : diff;
  int32_t step = adiff >> 4;                                 /* glide to a new key, at most half a sample per block */
  if (step < 1 << 12) step = 1 << 12;
  if (step > 1 << 15) step = 1 << 15;
  d = adiff <= step ? t : diff > 0 ? d + step : d - step;
  s->comb_d[i] = d;
  const uint32_t e = (uint32_t)((d >> 12) * klog);          /* log2 loss over one loop, Q32 */
  const int32_t gain = exp2neg(tb->exp2, e);
  const unsigned ei = e >> 24; const int32_t ef = (e >> 16) & 255;
  const int32_t norm = tb->sq1mg[ei] + (((tb->sq1mg[ei + 1] - tb->sq1mg[ei]) * ef) >> 8);   /* sqrt(1 - g): noise in = noise out */
  const int32_t di = d >> 16, fr = (d & 0xFFFF) >> 1;
  int16_t *rg = s->comb[i];
  const uint32_t w = s->comb_h, rd = (w - (uint32_t)di) & (SI_COMB - 1);
  cb.ring = rg; cb.ring_bytes = SI_COMB * 2; cb.rd = rg + rd; cb.wr = rg + w;
  cb.b = rg[(rd - 1) & (SI_COMB - 1)];
  cb.in = tm; cb.y = ys[i];
  cb.kzka = pack(clamp15(32768 - bright), M15(32767 - fr, bright));
  cb.kbg = pack(M15(fr, bright), gain);
  cb.inq = clamp15((norm * agc) >> 12);                       /* level follower inside */
  cb.z = s->comb_z[i];
  si_k_comb(&cb);
  s->comb_z[i] = cb.z;
 }
 /* level follower: in-key material rings up to ~20 dB louder than noise does; keep the pipes near the tail's
    level, so every key and every source sits at a similar loudness */
 const int16_t *y0 = ys[0], *y1 = ys[1], *y2 = ys[2];
 int32_t g = g0 << 5, pout = 0;
 const int32_t gs = g1 - g0;
 int32_t *fl = s->w->fl, *fr = s->w->fr;
 for (unsigned n = 0; n < SI_N; n++) {
  g += gs;
  const int32_t gg = g >> 5, a = y0[n], b = y1[n], cc = y2[n];
  const int32_t pl = sat(HALF(a + b + HALF(cc))), pr = sat(HALF(a + HALF(b) + cc));
  fl[n] += M14R(pl, gg);
  fr[n] += M14R(pr, gg);
  const int32_t m = pl < 0 ? -pl : pl;
  if (m > pout) pout = m;
 }
 follow(&s->pipe_lv, pin, pout + (pout >> 1));
 s->comb_h = (s->comb_h + SI_N) & (SI_COMB - 1);
}

/* VESPER's window switch (the current window fades out over its tail as the next one starts at wh) */
static void win_next(si_state *s, int32_t wh, int32_t win, int32_t fade, int32_t delta)
{
 s->prev_anchor = s->anchor; s->prev_win = s->win;
 s->prev_fade = s->fade; s->prev_inv_fade = s->inv_fade; s->prev_delta = s->delta;
 s->anchor = wh ? wh - 1 : SI_REV - 1;       /* the newest sample written so far */
 s->win = win; s->fade = fade; s->delta = delta;
 s->inv_fade = (128 << 16) / fade;           /* once per window */
}

/* a window edge (fade in or out) while VESPER is heard: the crossfaded readers, sample by sample (out of line) */
static __attribute__((noinline)) void vesper_xfade(si_state *s, int32_t win, int32_t fade, int32_t delta)
{
 int16_t *ring = s->rev, *rl = s->w->rl, *rr = s->w->rr;
 int32_t tau = s->tau, ptau = s->prev_tau;
 int32_t ic = s->anchor - tau, ip = s->prev_anchor - ptau;
 if (ic < 0) ic += SI_REV;
 if (ip < 0) ip += SI_REV;
 int32_t dl = s->delta, pdl = s->prev_delta;
 int32_t wh = s->rev_h - SI_N;                /* write position of sample n */
 if (wh < 0) wh += SI_REV;
 for (unsigned n = 0; n < SI_N; n++) {
  int32_t gc = 32767;
  if (tau < s->fade) gc = s->t->quarter[(tau * s->inv_fade) >> 16];
  int32_t icr = ic - dl;
  if (icr < 0) icr += SI_REV;
  int32_t al = M15R(ring[ic], gc), ar = M15R(ring[icr], gc);
  if (ptau < s->prev_win + s->prev_fade) {
   const int32_t t = ptau - s->prev_win;
   const int32_t gp = t < 0 ? 32767 : s->t->quarter[128 - ((t * s->prev_inv_fade) >> 16)];
   int32_t ipr = ip - pdl;
   if (ipr < 0) ipr += SI_REV;
   al += M15R(ring[ip], gp); ar += M15R(ring[ipr], gp);
  }
  rl[n] = (int16_t)sat(al + (al >> 1)); rr[n] = (int16_t)sat(ar + (ar >> 1));
  if (--ic < 0) ic += SI_REV;
  if (--ip < 0) ip += SI_REV;
  ptau++;
  if (++wh == SI_REV) wh = 0;
  if (++tau >= s->win) {                     /* next window */
   win_next(s, wh, win, fade, delta);
   ptau = tau; ip = ic; ic = s->anchor; tau = 0; pdl = s->prev_delta; dl = s->delta;
  }
 }
 if (ptau > 0x40000000) ptau = 0x40000000;
 s->tau = tau; s->prev_tau = ptau;
}

/* VESPER: write the forward wet (mono), read it back in reversed windows (crossfaded) into rl / rr. Not read
   (SWELL silent): rl / rr are left as they are (the wet sum weighs them by 0) and only the window clock moves. */
static void vesper(si_state *s, int read, int32_t win, int32_t fade, int32_t delta)
{
 int16_t *ring = s->rev, *rl = s->w->rl, *rr = s->w->rr;
 {
  int32_t h = s->rev_h;
  unsigned n1 = (unsigned)(SI_REV - h);
  if (n1 > SI_N) n1 = SI_N;
  int16_t *p = ring + h;
  const int32_t *fl = s->w->fl, *fr = s->w->fr;
  for (unsigned n = 0; n < n1; n++) p[n] = (int16_t)sat(HALF(fl[n] + fr[n]));
  for (unsigned n = n1; n < SI_N; n++) ring[n - n1] = (int16_t)sat(HALF(fl[n] + fr[n]));
  h += SI_N;
  if (h >= SI_REV) h -= SI_REV;
  s->rev_h = h;
 }
 int32_t tau = s->tau, ptau = s->prev_tau;
 const int fast = tau >= s->fade && tau + SI_N < s->win && ptau >= s->prev_win + s->prev_fade;
 if (read && !fast) { vesper_xfade(s, win, fade, delta); return; }
 if (fast) {                                  /* the common block: one reader at full gain, no window edge */
  if (read) {
   int32_t ic = s->anchor - tau;
   if (ic < 0) ic += SI_REV;
   rev_read(ring, ic, rl, SI_N); rev_read(ring, ic - s->delta, rr, SI_N);
   for (unsigned n = 0; n < SI_N; n++) {        /* +3.5 dB: the ring is mono, the tail it holds was wide */
    rl[n] = (int16_t)sat(rl[n] + (rl[n] >> 1)); rr[n] = (int16_t)sat(rr[n] + (rr[n] >> 1));
   }
  }
  s->tau = tau + SI_N;
  s->prev_tau = ptau > 0x40000000 ? 0x40000000 : ptau + SI_N;
  return;
 }
 /* not read, at a window edge: vesper_xfade's clock without its readers. The window switches after the k-th
    sample of the block (at most once: a window is far longer than a block), at write position rev_h - SI_N + k. */
 if (tau + SI_N >= s->win) {
  const int32_t k = s->win - tau < 1 ? 1 : s->win - tau;
  int32_t wh = s->rev_h - SI_N + k;
  if (wh < 0) wh += SI_REV;
  if (wh >= SI_REV) wh -= SI_REV;
  win_next(s, wh, win, fade, delta);
  ptau = tau + SI_N;                          /* prev_tau restarts at the old window's end (tau + k) and runs on */
  tau = SI_N - k;                             /* the new window's clock */
 } else { ptau += SI_N; tau += SI_N; }
 if (ptau > 0x40000000) ptau = 0x40000000;
 s->tau = tau; s->prev_tau = ptau;
}

static void smooth(si_state *s, const int32_t q[4])
{
 static const unsigned shift[4] = { 4, 4, 3, 3 };
 for (unsigned k = 0; k < 4; k++) {
  const int32_t t = clamp15(q[k]) * 16;
  if (s->first) s->sm[k] = t;
  else {
   const int32_t d = t - s->sm[k];
   if (d < 16 * 8 && d > -16 * 8) s->sm[k] = t;
   else s->sm[k] += d >> shift[k];
  }
 }
}

/* dry / wet at SI_FS: gains ramp over the block from the last block's (wg, 1 - dry) to this one's */
static void mix_out(si_state *s, int16_t *l, int16_t *r, const int16_t *wl, const int16_t *wr, int32_t dg1, int32_t wg1)
{
 /* Q14 table gains -> Q15 (dry as 1 - dry, so dry 1.0 is exact) */
 const int32_t n0 = clamp15(32768 - (s->g_dry << 1)), w0 = clamp15(s->g_wet << 1);
 const int32_t n1 = clamp15(32768 - (dg1 << 1)), w1 = clamp15(wg1 << 1);
 si_mix_t m;
 m.l = l; m.r = r; m.wl = wl; m.wr = wr;
 m.g = pack(w0, n0);
 m.step = pack((w1 - w0) >> 6, (n1 - n0) >> 6);
 if (r) si_k_mix(&m); else si_k_mixm(&m);
 s->g_dry = dg1; s->g_wet = wg1;
}

/* the block parameters for the smoothed macros k (only when they moved: out of line) */
static __attribute__((noinline)) void params(si_par *P, const int32_t k[4])
{
 for (unsigned i = 0; i < 4; i++) P->key[i] = k[i];
 const int32_t sp = k[0] >> 4, vo = k[1] >> 4, sw = k[2] >> 4, mx = k[3] >> 4;
 for (unsigned i = 0; i < 4; i++) P->gains[i] = SPACE(i, sp);
 P->damp = SPACE(4, sp); P->pre_t = SPACE(5, sp); P->win = SPACE(7, sp); P->depth = SPACE(8, sp);
#ifdef SI_DBG_PART      /* level analysis: 1 body only, 2 resonators only (no early taps in either) */
 P->er_g = 0;
 P->g_choir = SI_DBG_PART == 1 ? 0 : VOICE(0, vo); P->g_pipes = SI_DBG_PART == 1 ? 0 : VOICE(1, vo);
 P->g_body = SI_DBG_PART == 2 ? 0 : VOICE(2, vo);
#else
 P->er_g = SPACE(6, sp);
 P->g_choir = VOICE(0, vo); P->g_pipes = VOICE(1, vo); P->g_body = VOICE(2, vo);
#endif
 P->vdepth = VOICE(3, vo); P->vrate = VOICE(4, vo); P->qinv = VOICE(5, vo); P->klog = VOICE(6, vo); P->bright = VOICE(7, vo);
 P->g_fwd = SWELL(0, sw); P->g_rev = SWELL(1, sw); P->fade = SWELL(2, sw); P->delta = SWELL(3, sw);
 P->dg = MIX(0, mx); P->wg = MIX(1, mx); P->g_tilt = MIX(2, mx);
}

/* the partition is new to us: copy the tables in (out of line: rare) */
static __attribute__((noinline)) void l1_fill(si_l1tab *T)
{
 for (unsigned i = 0; i < SI_FLOG_N; i++) { T->fco[i][0] = si_fco[i][0]; T->fco[i][1] = si_fco[i][1]; }
 for (unsigned i = 0; i < 3; i++) for (unsigned k = 0; k < 3; k++) { T->vowel[i][k][0] = si_vowel[i][k][0]; T->vowel[i][k][1] = si_vowel[i][k][1]; }
 for (unsigned i = 0; i < 49; i++) T->root[i] = si_root[i];
 for (unsigned i = 0; i < 4; i++) {
  T->rate[i] = si_rate[i]; T->line_len[i] = si_line_len[i]; T->lap_len[i] = si_lap_len[i]; T->diff_len[i] = si_diff_len[i];
 }
 for (unsigned i = 0; i < 6; i++) { T->hb[i] = si_hb[i]; T->hb2[i] = si_hb2[i]; }
 for (unsigned i = 0; i < 129; i++) T->quarter[i] = si_quarter[i];
 for (unsigned i = 0; i < 257; i++) { T->exp2[i] = si_exp2[i]; T->sine[i] = si_sine[i]; T->sq1mg[i] = si_sq1mg[i]; }
 T->tp = (si_tp){ (const int32_t (*)[2])T->fco, (const int32_t (*)[3][2])T->vowel, T->root, T->rate, T->hb, T->hb2, T->quarter,
                  T->exp2, T->sq1mg, T->sine, T->line_len, T->lap_len, T->diff_len };
}

/* a block of the lazy ring clear after init: the wet path stays silent, the dry follows the mix law (out of line) */
static __attribute__((noinline)) void clear_block(si_state *s, int16_t *l, int16_t *r, uint32_t ring_words, int32_t dg, int32_t wg)
{
 uint32_t *p = (uint32_t *)(void *)s->pre + s->clear_done, n = ring_words - s->clear_done;
 if (n > SI_CLEAR_WORDS) n = SI_CLEAR_WORDS;
 for (uint32_t i = 0; i < n; i++) p[i] = 0;
 s->clear_done += n;
 for (unsigned i = 0; i < 64; i++) s->w->out[0][i] = s->w->out[1][i] = 0;
 mix_out(s, l, r, s->w->out[0], s->w->out[1], dg, wg);
 s->peak = 0;
}

/* the first wet block: the ramps start at the parameters (out of line) */
static __attribute__((noinline)) void seed(si_state *s, const si_par *P)
{
 s->seeded = 1;
 s->g_body = P->g_body; s->g_choir = P->g_choir; s->g_pipes = P->g_pipes; s->g_fwd = P->g_fwd; s->g_rev = P->g_rev;
 s->g_er = P->er_g; s->g_tilt = P->g_tilt; s->pre_d = P->pre_t << 4;
 s->win = P->win; s->fade = P->fade; s->delta = s->prev_delta = P->delta; s->inv_fade = (128 << 16) / P->fade;
 s->prev_tau = 0x40000000;
}

void si_process(si_state *s, int16_t *l, int16_t *r, const int32_t q[4], int32_t key_cents) { si_process_l1(s, l, r, q, key_cents, 0, 0, 0); }
void si_process_l1(si_state *s, int16_t *l, int16_t *r, const int32_t q[4], int32_t key_cents, void *pool, uint32_t pool_bytes, int fresh)
{
 if (pool) {                            /* the L1 pool: workspace at 0, the parameters and the tables behind it */
  uint8_t *p = pool;
  si_l1tab *T = pool_bytes >= SI_L1_BYTES ? (si_l1tab *)(void *)(p + SI_L1_TAB) : 0;
  s->w = (si_ws *)pool; s->par = (si_par *)(void *)(p + SI_L1_PAR); s->t = T ? &T->tp : &si_tp_rom;
  if (fresh) { s->par->key[0] = -1; if (T) l1_fill(T); }
 } else { s->w = &s->wm; s->par = &s->parm; s->t = &si_tp_rom; }
#ifdef SI_PROF
 uint32_t prof_t = cyc();
#endif
 smooth(s, q);
 const si_par *P = s->par;
 if (P->key[0] != s->sm[0] || P->key[1] != s->sm[1] || P->key[2] != s->sm[2] || P->key[3] != s->sm[3]) params(s->par, s->sm);
 const int32_t dg = P->dg, wg = P->wg;
 if (s->first) { s->g_dry = dg; s->g_wet = wg; }
 s->first = 0;
 s->key_cents = key_cents;

 /* lazy ring clear after init: the wet path stays silent, the dry follows the mix law */
 const uint32_t ring_words = (uint32_t)(sizeof(si_state) - (uint32_t)((char *)s->pre - (char *)s)) / 4;
 if (s->clear_done < ring_words) { clear_block(s, l, r, ring_words, dg, wg); return; }

 /* ---- per-block parameters ---- */
 const int32_t damp = P->damp, pre_t = P->pre_t, win = P->win, depth = P->depth, er_g = P->er_g;
 const int32_t g_choir = P->g_choir, g_pipes = P->g_pipes, g_body = P->g_body;
 const int32_t vdepth = P->vdepth, vrate = P->vrate, qinv = P->qinv, klog = P->klog, bright = P->bright;
 const int32_t g_fwd = P->g_fwd, g_rev = P->g_rev, fade = P->fade, delta = P->delta;
 const int32_t g_tilt = P->g_tilt;
 if (!s->seeded) seed(s, P);

 PROF(0);
 /* ---- input: decimate, pre-delay, early taps, diffusion ---- */
 {
  int16_t *b = s->w->db + 1;
  for (unsigned i = 0; i < 18; i++) b[i] = s->dh[i];
  if (r) for (unsigned i = 0; i < 64; i++) b[18 + i] = (int16_t)((l[i] + r[i]) >> 1);
  else for (unsigned i = 0; i < 64; i++) b[18 + i] = l[i];
  si_k_decim(b + 10, s->w->x, s->t->hb);
  for (unsigned i = 0; i < 18; i++) s->dh[i] = b[64 + i];
 }
 {
  const int32_t pt = pre_t << 4;
  int32_t d = s->pre_d;
  if (d < pt) d += d + 8 < pt ? 8 : pt - d; else if (d > pt) d -= d - 8 > pt ? 8 : d - pt;   /* 0.5 sample / block */
  s->pre_d = d;
  const uint32_t h = s->pre_h, dly = (uint32_t)(d >> 4);
  int16_t *pre = s->pre, *x = s->w->x, *erl = s->w->erl, *err = s->w->err, *t0 = s->w->o[0], *t1 = s->w->o[1], *t2 = s->w->o[2], *t3 = s->w->o[3];
  {                                   /* pre_h moves in steps of SI_N from 0 in a ring of SI_PRE: a block never wraps */
   int16_t *p = pre + h;
   for (unsigned n = 0; n < SI_N; n++) p[n] = x[n];
  }
  const uint32_t b = h - dly;
  ring_read(pre, SI_PRE - 1, b, x);
  /* early taps: two per side, crossed, unequal (the room's first walls); fixed times, not moved by the pre-delay
     slew (a one-sample step in a direct tap would click on drums) */
  ring_read(pre, SI_PRE - 1, h - si_er_l[0], t0); ring_read(pre, SI_PRE - 1, h - si_er_r[1], t1);
  ring_read(pre, SI_PRE - 1, h - si_er_r[0], t2); ring_read(pre, SI_PRE - 1, h - si_er_l[1], t3);
  for (unsigned n = 0; n < SI_N; n++) {
   erl[n] = (int16_t)(HALF(t0[n]) - ((t1[n] + 2) >> 2));
   err[n] = (int16_t)(HALF(t2[n]) - ((t3[n] + 2) >> 2));
  }
  s->pre_h = (h + SI_N) & (SI_PRE - 1);
  for (unsigned i = 0; i < 4; i++)
   si_k_ap(s->diff[i], s->ap_h & (SI_AP - 1), (s->ap_h - s->t->diff_len[i]) & (SI_AP - 1), i < 2 ? 20480 : 16384, s->w->x);
 }

 PROF(1);
 /* ---- tank ---- */
 tank(s, P->gains, damp, depth);
 s->ap_h = (s->ap_h + SI_N) & (SI_AP - 1);

 PROF(2);
 /* ---- forward wet: body + early taps + CHOIR + PIPES ---- */
 {
  int32_t b = s->g_body << 5, e = s->g_er << 5;
  const int32_t bs = g_body - s->g_body, es = er_g - s->g_er;
  int32_t *fl = s->w->fl, *fr = s->w->fr;
  const int16_t *tl = s->w->tl, *tr = s->w->tr, *el = s->w->erl, *er = s->w->err;
  for (unsigned n = 0; n < SI_N; n++) {
   b += bs; e += es;
   const int32_t bb = b >> 5, ee = e >> 5;
   fl[n] = M15R(tl[n], bb) + M15R(el[n], ee);
   fr[n] = M15R(tr[n], bb) + M15R(er[n], ee);
  }
  s->g_body = g_body; s->g_er = er_g;
 }
 PROF(3);
 if (g_choir || s->g_choir) choir(s, vrate, vdepth, qinv, s->g_choir, g_choir);
 else { s->res[0][0] = s->res[0][1] = s->res[0][2] = s->res[1][0] = s->res[1][1] = s->res[1][2] = 0; s->choir_lv.agc = 0; }
 s->g_choir = g_choir;
 PROF(4);
 if (g_pipes || s->g_pipes) pipes(s, klog, bright, s->g_pipes, g_pipes);
 else { s->comb_d[0] = s->comb_d[1] = s->comb_d[2] = 0; s->pipe_lv.agc = 0; }
 s->g_pipes = g_pipes;

 PROF(5);
 /* ---- VESPER and the wet sum ---- */
 const int rev = g_rev || s->g_rev;
 vesper(s, rev, win, fade, delta);
 PROF(6);
 int16_t (*ub)[10 + SI_N] = s->w->ub, *wl = ub[0] + 10, *wr = ub[1] + 10;
 for (unsigned c = 0; c < 2; c++) for (unsigned i = 0; i < 10; i++) ub[c][i] = s->uh[c][i];
 {
  int32_t *aw = s->w->fl, *bw = s->w->fr;     /* the wet sum overwrites fl / fr (the reverse ring has them) */
  const int16_t *rl = s->w->rl, *rr = s->w->rr;
  /* forward / reverse blend, ramped (settled: the step is 0; VESPER silent: the reverse gain is 0 on both ends,
     M15R(x, 0) = 0 for whatever rl / rr hold), +6 dB (the tank leaves at half scale), and the DC remover's sums */
  int32_t fa = s->g_fwd << 5, ra = s->g_rev << 5, sa = 0, sb = 0;
  const int32_t fs = g_fwd - s->g_fwd, rs = g_rev - s->g_rev;
  for (unsigned n = 0; n < SI_N; n++) {
   fa += fs; ra += rs;
   const int32_t f = fa >> 5, rv = ra >> 5;
   const int32_t a = 2 * (M15R(sat(aw[n]), f) + M15R(rl[n], rv)), b = 2 * (M15R(sat(bw[n]), f) + M15R(rr[n], rv));
   aw[n] = a; bw[n] = b; sa += a; sb += b;
  }
  s->g_fwd = g_fwd; s->g_rev = g_rev;
  /* brightness tilt at high MIX (a gentle high shelf over a ~1.9 kHz one-pole) */
  if (g_tilt || s->g_tilt) {
   int32_t tz0 = s->tilt_z[0], tz1 = s->tilt_z[1], ta = s->g_tilt << 5;
   const int32_t ts = g_tilt - s->g_tilt;
   sa = sb = 0;
   for (unsigned n = 0; n < SI_N; n++) {
    ta += ts;
    const int32_t t = ta >> 5, a = sat(aw[n]), b = sat(bw[n]);
    tz0 += rz14(M((a - tz0) >> 1, 10000));
    tz1 += rz14(M((b - tz1) >> 1, 10000));
    aw[n] = a + M14R((a - tz0) >> 1, t); bw[n] = b + M14R((b - tz1) >> 1, t);
    sa += aw[n]; sb += bw[n];
   }
   s->tilt_z[0] = tz0; s->tilt_z[1] = tz1;
  } else s->tilt_z[0] = s->tilt_z[1] = 0;
  s->g_tilt = g_tilt;
  /* DC remover (per block, ~4 Hz) and the block peak */
  int32_t peak = 0;
  s->dc[0] += (sa - s->dc[0]) >> 4; s->dc[1] += (sb - s->dc[1]) >> 4;
  const int32_t ma = (s->dc[0] + 16) >> 5, mb = (s->dc[1] + 16) >> 5;
  for (unsigned n = 0; n < SI_N; n++) {
   const int32_t a = aw[n] - ma, b = bw[n] - mb;
   aw[n] = a; bw[n] = b;
   const int32_t pa = a < 0 ? -a : a, pb = b < 0 ? -b : b;
   if (pa > peak) peak = pa;
   if (pb > peak) peak = pb;
  }
  /* a tail at the last bits: the tank's rounding can hold +-1..2 LSB (x2 here); let it end in exact silence */
  if (peak <= 4) { for (unsigned n = 0; n < SI_N; n++) aw[n] = bw[n] = 0; peak = 0; }
  /* block limiter (Q14): ramp down within the block to what this block's peak needs, release slowly */
  const int32_t lim0 = s->lim;
  int32_t lim1 = lim0 + ((16384 - lim0) >> 6);
  if (peak > 28000) {
   const int32_t need = (28000 << 14) / peak;   /* once per block */
   if (need < lim1) lim1 = need;
  }
  {                                            /* at rest (16384 both ends) M14R(v, 16384) = v */
   int32_t la = lim0 << 5;
   const int32_t ls = lim1 - lim0;
   for (unsigned n = 0; n < SI_N; n++) {
    la += ls;
    wl[n] = (int16_t)M14R(sat(aw[n]), la >> 5); wr[n] = (int16_t)M14R(sat(bw[n]), la >> 5);
   }
  }
  s->lim = lim1;
  s->peak = peak > 32767 ? 32767 : peak;
  s->wet_level += (s->peak - s->wet_level) >> 3;
 }

 PROF(7);
 /* ---- back to SI_FS and mix ---- */
 if (r) {
  si_k_interp(ub[0] + 5, s->w->out[0], s->t->hb2);
  si_k_interp(ub[1] + 5, s->w->out[1], s->t->hb2);
 } else {
  for (unsigned n = 0; n < SI_N; n++) wl[n] = (int16_t)HALF(wl[n] + wr[n]);
  si_k_interp(ub[0] + 5, s->w->out[0], s->t->hb2);
 }
 for (unsigned c = 0; c < 2; c++) for (unsigned i = 0; i < 10; i++) s->uh[c][i] = ub[c][SI_N + i];
 mix_out(s, l, r, s->w->out[0], s->w->out[1], dg, wg);
 PROF(8);
}
