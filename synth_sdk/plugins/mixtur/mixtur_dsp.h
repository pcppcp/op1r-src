/* Mixtur's two sample passes: the blocks they work on, the C loops (host builds and -DMIXTUR_C_REF) and
 * the Blackfin kernels in mixtur_dsp.S, which compute the same samples bit for bit.
 * Copyright (C) 2026 ultralazr, Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifndef MIXTUR_DSP_H
#define MIXTUR_DSP_H
#include <stdint.h>

#define MX_BLOCK 128

/* pass 1 (master phase, dividers, polyBLEP -> the mixture). Pairs hold divider 1 (3) in the high half and divider 0
 * (2) in the low half. c < n (or c = 0 for n = 0); gq = g << 7, the gain at sample 0 (mixtur.c's first step taken);
 * bh = (c D) >> 17; fade: the gains step by dg at samples 16, 32, .. 112. The kernel updates p, c, gq and bh. */
typedef struct {
    uint32_t p;                 /*  0 master phase */
    uint32_t c01, c23;          /*  4 divider wrap counters */
    uint32_t n01, n23;          /* 12 undertones (0 = off) */
    uint32_t gq01, gq23;        /* 20 gains, Q7 x 256 (0 .. 32640) */
    uint32_t dg01, dg23;        /* 28 gain step per 16 samples (int16 halves) */
    uint32_t bh01, bh23;        /* 36 (c D) >> 17 */
    uint32_t r01, r23;          /* 44 DIV_R[n] */
    uint32_t rinv;              /* 52 polyBLEP reciprocal 2^29 / (inc0 >> sh) */
    int32_t  nsh;               /* 56 -sh */
    uint32_t gsum;              /* 60 kernel: sum of the gains */
    uint32_t d[4];              /* 64 DIV_D[n] */
    const int32_t *inc;         /* 80 the block's phase increments */
    uint32_t fade;              /* 84 */
    uint32_t one2;              /* 88 0x00010001 */
    uint32_t look;              /* 92 kernel: p + inc of an event sample */
} mx_p1_t;

/* pass 2 (two formant SVFs, makeup x velocity, soft knee). f = f2 << 16 | f1, qg = gain << 16 | q (Q13 / Q12) */
typedef struct {
    uint32_t f, qg;             /*  0 */
    int32_t  f2l, cm, kn, zero, ka;   /*  8 constants: F2_LEVEL, KNEE + KNEE_T, KNEE, 0, KNEE_A */
    int32_t  lp1, lp2, bp1, bp2;      /* 28 states */
} mx_p2_t;

#define MX_F2_LEVEL 22938                           /* second formant 0.7 (vowel F2 is weaker) */
#define MX_KNEE     22938                           /* soft knee: linear below 0.7 FS */
#define MX_KNEE_T   19658                           /* ... reaches 32767 with zero slope 0.6 FS later */
#define MX_KNEE_A   27312                           /* 32768^2 / (2 KNEE_T) / 32768 in Q15 */

static inline uint32_t mx_lo(uint32_t w) { return w & 0xFFFFu; }
static inline uint32_t mx_hi(uint32_t w) { return w >> 16; }

/* 2^29 / d for 16384 < d < 32768 (the quotient is under 32768) with the Blackfin divide primitive, DIVS + 15 DIVQ
 * (the 32 / 16 integer division of the programming reference), instead of libgcc's 32-step loop */
static inline uint32_t recip29(uint32_t d)
{
#if defined(__bfin__) && !defined(MIXTUR_C_REF)
    int32_t q = 1 << 30;                                                 /* the dividend 2^29, shifted left once */
    __asm__("DIVS(%0, %1);\n\t"
            "DIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\t"
            "DIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\t"
            "DIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\tDIVQ(%0, %1);\n\t"
            "%0 = %0.L (Z);" : "+d"(q) : "d"(d) : "ASTAT");
    return (uint32_t)q;
#else
    return (1u << 29) / d;
#endif
}

/* the C loops (mixtur.c's, unchanged arithmetic) */
/* 16 x 16 -> 32 bit multiplies of register halves: one MAC op on the Blackfin. Plain C makes GCC hoist the int16
 * casts out of the loop and fall back to the multi-cycle 32-bit "Rx *= Ry", so the Blackfin build spells them out. */
#ifdef __bfin__
#define MX_MAC(name, T, ha, hb, mode) static inline T name(T a, T b) \
    { T r;__asm__("%0 = %1." #ha " * %2." #hb " (" #mode ");" : "=d"(r) : "d"(a), "d"(b)); return r; }
MX_MAC(mx_ll, int32_t, L, L, IS)  MX_MAC(mx_lh, int32_t, L, H, IS)  MX_MAC(mx_ull, uint32_t, L, L, FU)
#else
static inline int32_t mx_ll(int32_t a, int32_t b) { return (int16_t)a * (int16_t)b; }
static inline int32_t mx_lh(int32_t a, int32_t b) { return (int16_t)a * (int16_t)((uint32_t)b >> 16); }
static inline uint32_t mx_ull(uint32_t a, uint32_t b) { return (uint16_t)a * (uint32_t)(uint16_t)b; }
#endif
static inline void mx_pass1_c(mx_p1_t *s, int16_t *xbuf)
{
    uint32_t c[4] = { mx_lo(s->c01), mx_hi(s->c01), mx_lo(s->c23), mx_hi(s->c23) };
    uint32_t n[4] = { mx_lo(s->n01), mx_hi(s->n01), mx_lo(s->n23), mx_hi(s->n23) };
    int32_t gq[4] = { (int32_t)mx_lo(s->gq01), (int32_t)mx_hi(s->gq01), (int32_t)mx_lo(s->gq23), (int32_t)mx_hi(s->gq23) };
    int32_t dg[4] = { (int16_t)s->dg01, (int16_t)(s->dg01 >> 16), (int16_t)s->dg23, (int16_t)(s->dg23 >> 16) };
    uint32_t R[4] = { mx_lo(s->r01), mx_hi(s->r01), mx_lo(s->r23), mx_hi(s->r23) }, base[4];
    for (int k = 0; k < 4; k++) base[k] = c[k] * s->d[k];
    const int32_t *inc = s->inc;
    uint32_t p = s->p, A = 0, B = 0, off = 0, rinv = s->rinv;
    int sh = -s->nsh;
    int32_t gm = s->fade ? 15 : -1;
    for (int i = 0; i < MX_BLOCK; i++) {
        uint32_t di = (uint32_t)inc[i], pn = p + di;
        int32_t blep = 0, upd = (i & gm) == 0;
        if (upd) {                                                       /* gain step every 16 samples while fading */
            if (i) for (int k = 0; k < 4; k++) gq[k] += dg[k];         /* (sample 0's step is in gq) */
            off = (uint32_t)(gq[0] + gq[1] + gq[2] + gq[3]) << 14;      /* saw offset: sum gq * 16384 */
            B = 0;
            for (int k = 0; k < 4; k++) B += ((uint32_t)gq[k] * R[k]) >> 17;
        }
        if (pn < p) {                                                    /* master wrap: dividers count */
            int32_t hg = 0;
            for (int k = 0; k < 4; k++) {
                if (++c[k] >= n[k]) { c[k] = 0; hg += gq[k]; }          /* this divider's saw resets */
                base[k] = c[k] * s->d[k];
            }
            uint32_t d = ((pn >> sh) * rinv) >> 14; if (d > 32767) d = 32767;
            int32_t e = 32768 - (int32_t)d, sq = (e * e) >> 15;         /* (1 - d)^2: sample after the step */
            blep += ((hg >> 2) * sq) >> 14;
            upd = 1;
        }
        if (upd) { A = 0; for (int k = 0; k < 4; k++) A += (uint32_t)gq[k] * (base[k] >> 17); }
        p = pn;
        if (p + di < p) {                                                /* lookahead: wrap before the next sample */
            int32_t hg = 0;
            for (int k = 0; k < 4; k++) if (c[k] + 1 >= n[k]) hg += gq[k];
            uint32_t d = (((p + di) >> sh) * rinv) >> 14; if (d > 32767) d = 32767;
            int32_t sq = ((int32_t)d * (int32_t)d) >> 15;                /* d^2: sample before the step */
            blep -= ((hg >> 2) * sq) >> 14;
        }
        uint32_t ph = p >> 16;
        xbuf[i] = (int16_t)((((int32_t)(A + mx_ull(ph, B) - off) >> 15) + blep) >> 2);   /* |x| <= 25088 */
    }
    s->p = p;
    s->c01 = c[1] << 16 | c[0]; s->c23 = c[3] << 16 | c[2];
}

static inline void mx_pass2_c(mx_p2_t *s, const int16_t *xbuf, int16_t *out)
{
    int32_t fq = (int32_t)(s->qg << 16 | (s->f & 0xFFFFu)), f2g = (int32_t)((s->qg & 0xFFFF0000u) | (s->f >> 16));
    int32_t lp1 = s->lp1, bp1 = s->bp1, lp2 = s->lp2, bp2 = s->bp2;
    for (int i = 0; i < MX_BLOCK; i++) {
        int32_t in = mx_lh(xbuf[i], fq) >> 12, hp, b;                   /* unity-peak band-pass input: x * q */
        b = bp1 >> 2;                                                    /* Chamberlin SVF, Q13 coefficients */
        lp1 += mx_ll(b, fq) >> 11;
        hp = in - lp1 - (mx_lh(b, fq) >> 11);
        bp1 += mx_ll(hp >> 3, fq) >> 10;
        b = bp2 >> 2;
        lp2 += mx_ll(b, f2g) >> 11;
        hp = in - lp2 - (mx_lh(b, fq) >> 11);
        bp2 += mx_ll(hp >> 3, f2g) >> 10;
        int32_t y = (bp1 >> 2) + (mx_ll(bp2 >> 2, MX_F2_LEVEL) >> 15);   /* <= 47600 */
        y = mx_lh(y >> 1, f2g) >> 8;
        if (y > 49152) y = 49152; else if (y < -49152) y = -49152;
        int32_t ay = y < 0 ? -y : y;                                     /* soft knee: linear up to 0.7, then */
        if (ay > MX_KNEE) {                                              /* t - t^2 / (2 T) up to full scale */
            int32_t t = ay - MX_KNEE; if (t > MX_KNEE_T) t = MX_KNEE_T;
            ay = MX_KNEE + t - ((((t * t) >> 15) * MX_KNEE_A) >> 15);
            y = y < 0 ? -ay : ay;
        }
        out[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
    s->lp1 = lp1; s->bp1 = bp1; s->lp2 = lp2; s->bp2 = bp2;
}

#if defined(__bfin__) && !defined(MIXTUR_C_REF)
void mx_pass1(mx_p1_t *s, int16_t *xbuf);           /* mixtur_dsp.S */
void mx_pass2(mx_p2_t *s, const int16_t *xbuf, int16_t *out);
#endif
#endif
