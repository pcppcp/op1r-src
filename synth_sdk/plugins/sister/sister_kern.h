/* SISTER's per-sample kernels: Blackfin assembly (sister_asm.S) and C references of the same integer results
 * (sister_kern.c: host builds, and Blackfin builds with -DSI_NO_ASM for the exactness check).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
 *
 * Arithmetic is the Blackfin's: a fractional product p(a, b) = 2ab (Q31; never both -32768 here), summed in a
 * 40-bit accumulator and taken back as sat16(acc >> 16) (the "(T)" truncating form); "(S)" adds saturate to 16
 * bits. Rounding is never the hardware's RND_MOD-dependent default (bcore and the unit could differ on ties):
 * products round down, or toward zero where noted, so asm and C give the same samples. Struct offsets are fixed: the assembly reads them by number. */
#ifndef SISTER_KERN_H
#define SISTER_KERN_H
#include <stdint.h>
#include <stddef.h>

/* one tank line for one block: interpolated read (fixed delay and fraction for the block) folded into the
   damping one-pole, then the line's allpass; 32 outputs. Products round down (the tank's DC this leaves is removed
   where the tail leaves the tank). */
typedef struct {
 int16_t *ring;        /*  0: line ring base */
 uint32_t ring_bytes;  /*  4: its length in bytes */
 int16_t *rd;          /*  8: x[n - D] for n = 0 */
 int16_t *ap;          /* 12: allpass ring (512 samples) */
 int16_t *ap_rd;       /* 16 */
 int16_t *ap_wr;       /* 20 */
 int16_t *out;         /* 24: 32 outputs */
 int32_t kzka;         /* 28: low 1 - damp, high (1 - frac) damp (Q15) */
 int32_t kbg;          /* 32: low frac damp, high allpass gain */
 int32_t b;            /* 36: in: x[n - D - 1] for n = 0 */
 int32_t z;            /* 40: damping state, in/out */
} si_line_t;

/* one formant band over one channel block: y = b0 dx + 2 a1 y1 - a2 y2 rounded toward zero (32-bit saturated
   accumulator), sum[n] += y (saturating) */
typedef struct {
 const int16_t *dx;    /*  0: x[n] - x[n-2] */
 int16_t *sum;         /*  4: in/out */
 int32_t b0a1;         /*  8: low b0 (Q15), high a1 = r cos(theta) (Q15, used twice) */
 int32_t a2;           /* 12: low a2 (Q15) */
 int32_t y;            /* 16: high y1, low y2, in/out */
 int32_t unused;       /* 20 */
} si_res_t;

/* one PIPES comb for one block: z = damped interpolated x[n - D] (rounded half up via an accumulator bias);
   o = in inq + z g rounded toward zero (a ring ends in silence); ring and y get o */
typedef struct {
 int16_t *ring;        /*  0: comb ring base */
 uint32_t ring_bytes;  /*  4 */
 int16_t *rd;          /*  8: x[n - D] for n = 0 */
 int16_t *wr;          /* 12: write position for n = 0 */
 const int16_t *in;    /* 16 */
 int16_t *y;           /* 20 */
 int32_t kzka;         /* 24: low 1 - bright, high (1 - frac) bright */
 int32_t kbg;          /* 28: low frac bright, high loop gain g */
 int32_t inq;          /* 32: low input gain */
 int32_t b;            /* 36: x[n - D - 1] for n = 0 */
 int32_t z;            /* 40: in/out */
} si_comb_t;

/* dry/wet mix at the output rate: out = sat16(dry + ((wet wg - dry ndg) >> 15)), gains ramp per sample
   (packed: low wg, high ndg = 1 - dry gain; step added per sample with 16-bit lanes) */
typedef struct {
 int16_t *l;           /*  0: in place */
 int16_t *r;           /*  4: in place (stereo only) */
 const int16_t *wl;    /*  8: 64 wet samples */
 const int16_t *wr;    /* 12: (stereo only) */
 int32_t g;            /* 16: gains at sample -1 */
 int32_t step;         /* 20: per-sample step, 16-bit lanes */
} si_mix_t;

/* the tank's scattering for one block: lines o[0..3][32] (a, b, c, d) -> each line's ring gets
   sat16(rz14(y_k g_k) +- x) with y = (a+b+c+d)/4, (a-b+c-d)/4, (a+b-c-d)/4, (a-b-c+d)/4 (sums >> 2), g Q14 (2 x the
   decay gain: the orthonormal 1/2 Hadamard), x = (in + 2) >> 2 with signs + - + -; the raw tail is
   tl = (a + c) >> 1, tr = (b + d) >> 1 */
typedef struct {
 const int16_t *o;     /*  0: o[4][32] contiguous */
 const int16_t *in;    /*  4 */
 int16_t *tl;          /*  8 */
 int16_t *tr;          /* 12 */
 int16_t *w[4];        /* 16: write positions */
 int16_t *base[4];     /* 32: ring bases */
 uint32_t bytes[4];    /* 48: ring lengths in bytes */
 int32_t g01, g23;     /* 64: low g0 high g1, low g2 high g3 */
} si_scat_t;

#define SI_KERN_OFFSETS_OK \
 (offsetof(si_line_t, z) == 40 && offsetof(si_res_t, unused) == 20 && offsetof(si_comb_t, z) == 40 && \
  offsetof(si_mix_t, step) == 20 && offsetof(si_scat_t, g23) == 68 && sizeof(void *) == 4)

void si_k_line(si_line_t *p);
void si_k_res(si_res_t *p);
void si_k_scatter(si_scat_t *p);
void si_k_comb(si_comb_t *p);
void si_k_mix(si_mix_t *p);      /* stereo, 64 samples */
void si_k_mixm(si_mix_t *p);     /* mono: l and wl only */
/* 64 -> 32 half-band: c points at the centre of output 0 (b + 10), c - 9 4-byte aligned; 5 odd coefficients (Q15,
   4-byte aligned, as h2 below) */
void si_k_decim(const int16_t *c, int16_t *x, const int16_t *h);
/* 32 -> 64 half-band: c points at u + 5; out: 64 samples; h2: 2 x the coefficients */
void si_k_interp(const int16_t *c, int16_t *out, const int16_t *h2);
/* allpass over 32 samples in place: d = ring[rd]; v = x - p(d, g); ring[wr] = v; x = d + p(v, g)
   (ring of 512, indices advance and wrap) */
void si_k_ap(int16_t *ring, uint32_t wr, uint32_t rd, int32_t g, int16_t *x);
#endif
