/* REACTOR's sample loop for the Blackfin: reactor_dsp.h's rx_block_c, bit for bit.
 *
 * void rx_block(rx_run *r, int16_t *out, const int32_t *inc)    r->cnt samples (1..128), the controls held. reactor.c
 *   splits a call's 128 samples at the Geiger click events and starts each click itself, so the loop has no branch.
 *   rx_run offsets: 0 p1, 4 fb, 8 dce, 12 cnt, 16 rng, 20 click, 24 next, 28 p0, 32 dinc, 36 ring[9], 72 nln[16],
 *   104 lp, 108 mean4, 112 tab
 *   ring (rx_ring): LCG multiplier, ms8|g4, m4|g3, m3|g2, m2|g1, a|nz, og|drive, 0x8001|32273, tone|0 (high|low halves)
 *
 * Registers in the loop: R7 p1, R6 fb, R5 rng, R4 click (low half; the high half its sample), P5 tab, I1 out, I2 &lp,
 * P4 r, I0 inc, I3 / B3 / L3 the ring (circular, 36 bytes). R0..R3, P0, P1, A0 scratch. R4.H also carries the runaway
 * rod's phase offset from its multiply to rod 1 (the click's sample takes it after that). p0 and dce live in r,
 * loaded and stored in parallel slots.
 * Why it is exact: every product is a 16 x 16 (IS) one on values the C keeps in 16 bits; the C's
 * rx_sat16(p >> 10) is the high half of (p << 6) (S) and rx_sat16(p >> 14) that of (p << 2) (S); (a b) >> 15 for
 * 16-bit a, b (not both -32768) is the (T) fractional product; the clip's lower bound is a vector MAX with 0x8001 (x in
 * the high half); y - dce and o + click are 16-bit saturating subtracts and adds, x - x3 - 1 (x + ~x3), its half and
 * e >> 1 16-bit ones (all of them fit 16 bits); lp is kept in 32 bits as the C keeps it, and the C's
 * rx_sat16((o - lp) >> 1) is the high half of ((o - lp) << 15) (S).
 * Host builds assemble it empty: reactor.c runs rx_block_c there.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text
	.align 4
	.global _rx_block;
	.type _rx_block, STT_FUNC;
_rx_block:
	[--SP] = (R7:4, P5:3);
	[--SP] = L3;
	P4 = R0;
	I1 = R1;
	I0 = R2;
	R7 = [P4 + 0];
	R6 = [P4 + 4];
	R5 = [P4 + 16];
	R4 = [P4 + 20];
	P5 = [P4 + 112];
	R0 = P4;
	R0 += 36;
	I3 = R0;
	B3 = R0;
	R0 = 36;
	L3 = R0;
	R0 = P4;
	R0 += 52;
	R0 += 52;                                               /* &lp (an add takes -64..63) */
	I2 = R0;
	P0 = [P4 + 12];                                         /* cnt */
	LSETUP(.Lrx_s, .Lrx_e) LC0 = P0;
.Lrx_s:
	MNOP || R0 = [P4 + 32] || R1 = [I0++];                  /* dinc; inc */
	R1 = R1 + R0 (NS) || R0 = [P4 + 28] || R2 = [I3++];     /* in = inc + dinc; p0; LCG multiplier */
	R7 = R7 + R1 (NS) || R3 = [I3++] || NOP;                /* p1; ms8 | g4 */
	R1 = R1 >> 1;
	R0 = R0 + R1 (NS);                                      /* p0 += in >> 1: the runaway rod */
	R1 = R0 >> 21 || [P4 + 28] = R0 || NOP;
	P1 = R1;
	P1 = P5 + (P1 << 1);
	R5 *= R2;                                               /* rng */
	R0 = R6.L * R3.L (IS) || R1 = W[P1] (X) || R2 = [I3++]; /* fb g4; s0; m4 | g3 */
	R4.H = R1.L * R3.H (T);                                 /* o0 = (s0 ms8) >> 15 */
	R1 = R7 << 2;
	R0 <<= 4;
	R1 = R1 + R7;
	R0 = R0 + R1;                                           /* p1 * 5 + offset */
	R0 >>= 21;
	P0 = R0;
	P0 = P5 + (P0 << 1);
	R1 = R7 << 1 || R3 = W[P0] (X) || NOP;                  /* p1 * 2; s4 */
	A0 = R3.L * R2.H (IS);
	R0 = (A0 += R6.L * R2.L) (IS) || R3 = [I3++] || NOP;    /* m3 | g2 */
	R0 <<= 4;
	R2 = R1 + R7;
	R0 = R0 + R2;                                           /* p1 * 3 + offset */
	R0 >>= 21;
	P0 = R0;
	P0 = P5 + (P0 << 1);
	R2 = W[P0] (X);                                         /* s3 */
	A0 = R2.L * R3.H (IS);
	R0 = (A0 += R6.L * R3.L) (IS) || R2 = [I3++] || NOP;    /* m2 | g1 */
	R0 <<= 4;
	R0 = R0 + R1;                                           /* p1 * 2 + offset */
	R0 >>= 21;
	P0 = R0;
	P0 = P5 + (P0 << 1);
	R3 = W[P0] (X);                                         /* s2 */
	A0 = R3.L * R2.H (IS);
	R0 = (A0 += R6.L * R2.L) (IS) || R1 = [I3++] || NOP;    /* a | nz */
	R0 <<= 4;
	R0 = R0 + R7;                                           /* p1 + offset */
	R0.H = R0.H + R4.H (NS);                                /* + o0 << 16 */
	R0 >>= 21;
	P0 = R0;
	P0 = P5 + (P0 << 1);
	R2 = R5.H * R1.L (IS) || R3 = W[P0] (X) || NOP;         /* noise; y */
	/* coolant */
	R2 >>>= 15;
	R0 = R3 >>> 1;
	R0 = R0 + R2;                                           /* yn */
	R0 = R0 - R6;
	R0 >>>= 1;
	R0 = R0.L * R1.H (IS) || R1 = [P4 + 8] || NOP;          /* dce */
	R0 >>>= 14;
	R6 = R6 + R0;                                           /* fb */
	/* DC: dce keeps its whole counts in the high half (16 fraction bits), one pole 2^-11 */
	R0.L = R3.L - R1.H (S);                                 /* e = sat16(y - dce) */
	R2 = R0 << 16;
	R2 >>>= 11;
	R1 = R1 + R2 (NS) || R2 = [I3++] || NOP;                /* dce += e << 5; og | drive */
	R0.L = R0.L >>> 1 || [P4 + 8] = R1 || NOP;              /* yd = e >> 1; dce back */
	/* drive, the overdrive's clip, the click and the tone */
	R0 = R0.L * R2.L (IS) || R1 = [I3++] || NOP;            /* 0x8001 | 32273 */
	R4.H = R5.H * R4.L (T);                                 /* the click's sample (from the click before its decay) */
	R4.L = R4.L * R1.L (T);                                 /* click decay */
	R0 = R0 << 6 (S);
	R0 = MAX(R0, R1) (V) || R3 = [I3++] || NOP;             /* x in R0.H; tone | 0 */
	R1.L = R0.H * R0.H (T);
	R1.L = R1.L * R0.H (T);                                 /* x3 */
	R1 = ~R1;
	R1.L = R1.L + R0.H (NS);
	R1.L = R1.L >>> 1;
	R0.L = R0.H + R1.L (NS);                                /* o = x + ((x + ~x3) >> 1) */
	R0.L = R0.L + R4.H (S) || R1 = [I2] || NOP;             /* + the click; lp */
	R0 = R0.L (X);
	R0 = R0 - R1 (NS);
	R0 = R0 << 15 (S);                                      /* sat16((o - lp) >> 1) in the high half */
	R0 = R0.H * R3.H (IS) || R2 = [P4 + 60] || NOP;         /* og | drive */
	R0 >>>= 14;
	R1 = R0 + R1;                                           /* lp */
	R0 = R1 << 16 (S) || [I2] = R1 || NOP;                  /* lp back */
	R0 = R0.H * R2.H (IS);
	R0 = R0 << 2 (S);                                       /* out in R0.H */
.Lrx_e:
	W[I1++] = R0.H;
	R4 = R4.L (Z);
	[P4 + 0] = R7;
	[P4 + 4] = R6;
	[P4 + 16] = R5;
	[P4 + 20] = R4;
	L3 = [SP++];
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _rx_block, .-_rx_block
#endif
