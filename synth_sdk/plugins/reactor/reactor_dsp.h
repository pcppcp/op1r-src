/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* REACTOR's sample loop: the state and constants one block needs (rx_run), the C reference (rx_block_c, the host
 * build and the Blackfin fallback) and the hand-scheduled kernel (reactor_kernel.S rx_block, the same arithmetic bit
 * for bit). rx_run's layout is the kernel's: change both together (the offsets are in reactor_kernel.S's header).
 * Rate (2026-11-03): the loop runs at R246's 88.2 kHz voice rate (128 samples a call). Every per-sample pole is the
 * Mac model's (44.1 kHz) one taken in two steps: 2^-10 -> 2^-11, 31785 -> 32273 (the square root of 0.970), the
 * coolant and tone coefficients 1 - sqrt(1 - a). The DC pole keeps fraction bits (a pole of 2^-11 on whole counts
 * would stall 8x further from the signal).
 * The runaway (2026-11-04): a rod at half rod 1's pitch phase-modulates it while critical, and a pitch sag (dinc) is
 * added to every pitch step. To pay for it (the 35 % synth slot) the overdrive's bias and its second DC estimate went,
 * the first DC estimate keeps its whole counts in the high half, and the loop has no click branch any more: rx_render
 * splits the call at the Geiger click events and starts each click between the pieces. */
#ifndef REACTOR_DSP_H
#define REACTOR_DSP_H
#include <stdint.h>

#define RX_SIN_BITS 11                       /* the loop's sine table: 2048 entries, index = phase >> 21 */
#define RX_RING 9                            /* constant words the kernel reads per sample, in its order */
#define RX_NO_CLICK 0x7FFFFFFE

typedef struct {
    uint32_t p1;                             /*  0 rod 1's phase (rods 2..4 run at 2, 3 and 5 times it) */
    int32_t  fb;                             /*  4 coolant, Q14 (stays in 16 bits) */
    int32_t  dce;                            /*  8 DC estimate (one-pole, 2^-11), whole counts in the high half */
    int32_t  cnt;                            /* 12 samples this piece (1..128) */
    uint32_t rng;                            /* 16 multiplicative LCG, x 1664525 (odd: period 2^30) */
    int32_t  click;                          /* 20 Geiger click envelope, Q15 */
    int32_t  next;                           /* 24 samples to the next click */
    uint32_t p0;                             /* 28 the runaway rod's phase: half of rod 1's (2026-11-04) */
    int32_t  dinc;                           /* 32 the pitch sag, added to every pitch step (0 = in tune) */
    uint32_t ring[RX_RING];                  /* 36 the kernel's constants (rx_ring) */
    int16_t  nln[16];                        /* 72 256 * -ln((i + 0.5) / 16): exponential click intervals */
    int32_t  lp;                             /* 104 the tone lowpass after the overdrive */
    int32_t  mean4;                          /* 108 mean click interval >> 4, 0 = no clicks */
    const int16_t *tab;                      /* 112 rx_sin */
    /* the C reference's view of the same constants */
    int16_t  g4, g3, g2, g1, m4, m3, m2, nz, a, drive, og, tone, ms8;
} rx_run;

#define RX_MUL(a, b) ((int32_t)(int16_t)(a) * (int16_t)(b))           /* one 16 x 16 multiply on Blackfin */
static inline int32_t rx_sat16(int32_t x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : x; }

/* the ring in the order rx_block loads it */
static inline void rx_ring(rx_run *r)
{
    r->ring[0] = 1664525u;                                                   /* LCG multiplier */
    r->ring[1] = (uint32_t)(uint16_t)r->ms8 << 16 | (uint16_t)r->g4;
    r->ring[2] = (uint32_t)(uint16_t)r->m4 << 16 | (uint16_t)r->g3;
    r->ring[3] = (uint32_t)(uint16_t)r->m3 << 16 | (uint16_t)r->g2;
    r->ring[4] = (uint32_t)(uint16_t)r->m2 << 16 | (uint16_t)r->g1;
    r->ring[5] = (uint32_t)(uint16_t)r->a << 16 | (uint16_t)r->nz;
    r->ring[6] = (uint32_t)(uint16_t)r->og << 16 | (uint16_t)r->drive;
    r->ring[7] = 0x8001u << 16 | 32273u;                                     /* -32767 for the clip; click decay (tau 0.75 ms) */
    r->ring[8] = (uint32_t)(uint16_t)r->tone << 16;
}

static inline void rx_block_c(rx_run *r, int16_t *out, const int32_t *inc)
{
    const int16_t *tab = r->tab;
    uint32_t p1 = r->p1, p0 = r->p0, rng = r->rng;
    int32_t fb = r->fb, dce = r->dce, lp = r->lp, click = r->click;
    for (int n = 0; n < r->cnt; n++) {
        const uint32_t in = (uint32_t)inc[n] + (uint32_t)r->dinc;            /* the pitch step, sagging while critical */
        p1 += in;
        p0 += in >> 1;                                                      /* the runaway rod: half of rod 1's pitch */
        rng *= 1664525u;
        /* its phase modulation of rod 1, in whole 2^16 phase steps: (s0 ms8) >> 15 = (s0 ms) >> 12 */
        const int32_t o0 = RX_MUL(tab[p0 >> 21], r->ms8) >> 15;
        const int32_t s4 = tab[(p1 * 5u + ((uint32_t)RX_MUL(fb, r->g4) << 4)) >> 21];
        const int32_t s3 = tab[(p1 * 3u + ((uint32_t)(RX_MUL(s4, r->m4) + RX_MUL(fb, r->g3)) << 4)) >> 21];
        const int32_t s2 = tab[(p1 * 2u + ((uint32_t)(RX_MUL(s3, r->m3) + RX_MUL(fb, r->g2)) << 4)) >> 21];
        const int32_t y  = tab[(p1 + ((uint32_t)(RX_MUL(s2, r->m2) + RX_MUL(fb, r->g1)) << 4) + ((uint32_t)o0 << 16)) >> 21];
        const int32_t rh = (int32_t)rng >> 16;
        const int32_t yn = (y >> 1) + (RX_MUL(rh, r->nz) >> 15);            /* radiation contaminates the coolant */
        fb += RX_MUL((yn - fb) >> 1, r->a) >> 14;
        /* DC out before HEAT's drive (feedback FM leans to one side; clipped off-centre it would lose its body) */
        const int32_t e = rx_sat16(y - (dce >> 16));                        /* one pole, 2^-11 (6.9 Hz at 88.2 kHz) */
        dce += e * 32;
        const int32_t yd = e >> 1;
        int32_t x = rx_sat16(RX_MUL(yd, r->drive) >> 10);                  /* y * drive (Q11; yd is half of y) */
        if (x < -32767) x = -32767;
        const int32_t x3 = RX_MUL(RX_MUL(x, x) >> 15, x) >> 15;             /* cubic soft clip 1.5 x - 0.5 x^3 */
        int32_t o = x + ((x - x3 - 1) >> 1);                                /* = (3 x - x3 - 1) >> 1: in 16 bits */
        o = rx_sat16(o + (RX_MUL(rh, click) >> 15));                        /* Geiger click */
        click = RX_MUL(click, 32273) >> 15;                                  /* tau 66 samples = 0.75 ms */
        /* tone: darker as HEAT rises. Its step is saturated: lp can step a count past o, and a full-scale o at Nyquist
           then took (o - lp) >> 1 past 16 bits, the multiply saw the wrong sign and the state ran away (2026-11-04) */
        lp += RX_MUL(rx_sat16((o - lp) >> 1), r->tone) >> 14;
        out[n] = (int16_t)rx_sat16(RX_MUL(rx_sat16(lp), r->og) >> 14);       /* half scale, as the other engines */
    }
    r->p1 = p1; r->p0 = p0; r->rng = rng; r->fb = fb; r->dce = dce; r->lp = lp; r->click = click;
}

#ifdef __bfin__
void rx_block(rx_run *r, int16_t *out, const int32_t *inc);
#endif

/* one call's 128 samples, in pieces between the Geiger click events: a click starts after the sample on which
   'next' runs out, from the LCG's state after that sample (as the loop's own branch did before 2026-11-04) */
static inline void rx_render(rx_run *r, int16_t *out, const int32_t *inc, void (*blk)(rx_run *, int16_t *, const int32_t *))
{
    for (int n0 = 0; n0 < 128; ) {
        const int32_t left = 128 - n0;
        r->cnt = r->next < left ? r->next : left;
        blk(r, out + n0, inc + n0);
        n0 += r->cnt;
        if ((r->next -= r->cnt) <= 0) {
            { const int32_t u = (int32_t)(r->rng >> 19); r->click = 9900 + u + (u >> 1) - (u >> 3); }   /* sqrt(2) x the model's 7000, + 1.375 u: white noise at 88.2 kHz is 3 dB thinner in the band */
            if (r->mean4) { r->next = (r->mean4 * r->nln[r->rng >> 28]) >> 4; if (r->next < 1) r->next = 1; }
            else r->next = RX_NO_CLICK;
        }
    }
}
#endif
