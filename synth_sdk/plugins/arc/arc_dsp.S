/* ARC's sample loops hand-scheduled for the Blackfin: the same arithmetic as arc.c's loops (and its host build), bit
 * for bit, in fewer instruction packets. Constants come from a small ring read through a circular I-register in the
 * parallel slots, so the state stays in registers.
 *
 * void arc_shape(int16_t *out, ArcShape *s)
 *   pass 2 (carbon, DC blocker, tilt, gain) over the block's 128 samples, in place; s (arc.c ArcShape): [0] dc, [1] lp
 *   (updated), [2] ga, [3] cb | g << 16, [4..11] the ring: -8192, 32767, 16383, -16383, btq, 32767, -32767, dg.
 *   30 packets per sample instead of about 47.
 *
 * void arc_lamp1(ArcRun *k, int16_t *out), void arc_lamp2(ArcRun *k, int16_t *out)
 *   pass 1 (arc.c LAMP and the noise / output lines) for the block: one lamp (coupling 0), 28 packets per sample
 *   instead of about 62, or both lamps, 80 packets per sample pair instead of about 180. k (arc.c ArcRun): [0] x1,
 *   [1] v1, [2] x2, [3] v2, [4] r (updated), [5..9] scratch, [10] samples (one lamp) or pairs per portrait chunk,
 *   [11] chunks, [12..14] the portrait ring (arc_trace) as I3 / B3 / L3: the next point, the start, its bytes, or an
 *   8-byte dummy for an untraced voice. The chunks run in an outer hardware loop (the traced voice's 64 chunks cost
 *   no calls); after each, the kernel writes arc_trace's point (xs, lienard(xs, v1), xt, lienard(xt, v2) with xt, v2
 *   = xs, v1 for one lamp) and arc_render advances arc_tpos. Rings: +60 the portrait's (7 words), +88 one lamp's
 *   (6), +112 both lamps' (19), filled per block by arc_render (lists at the kernels). x1, v1, r live in R0..R2; x2,
 *   v2 and a few pair values go through k in parallel slots.
 *
 * Why it is exact: every C product is M16 / MH, a 16 x 16 integer MAC on the operands' halves (.L for an (int16_t)
 * cast, .H for >> 16), and the shifts, clamps (MIN / MAX), wrapping adds and the one saturating shift are the C's own,
 * one instruction each. gq = (int16_t)(ga >> 16) is ga's high half; (int16_t)(SHL3(o gq) >> 16) is the high half
 * of the saturated shift.
 * The step's a = (MH(1 - x^2, K) + 1024) >> 11 is rebuilt as (2^29 + 2048 he + 1024 - s he) >> 11 = 2^18 + a with an
 * unsigned MAC: s = xs^2 >> 13 reaches 32768 (xs = -16384), which a signed 16-bit operand cannot hold; he >= 0, and
 * the 2^29 offset (a multiple of 2^11, larger than 32768 he) keeps the accumulator positive. a >= A_MIN is then
 * MAX(2^18 + a, 2^18 + A_MIN), and the next product only reads a's low half, the same as a's. The output terms use
 * the fractional identities of wavetable_dsp.S: ((d >> 1) mix) >> 14 is the truncated fractional product of
 * (d with bit 0 cleared) and mix; (r hiss) >> 15 likewise (hiss = h2k >> 2 <= 8191); in arc_lamp2, (q |xs|) >> 13 is
 * the truncated fractional product of 4 q (|q| <= 8191) and |xs| (<= 16384; arc_lamp1 shifts the integer product).
 * None of them can hit 0x8000 x 0x8000. The two output samples of a pair are summed in 16-bit halves: only the low 16
 * bits of the C's 32-bit sum are stored. LIEN (the portrait): F = x - x^3 / 3 stays within +-24577 and epsd <= 28671,
 * so epsd F is a 16 x 16 product; the other products are the C's 32-bit multiplies.
 * Memory: arc_shape reads and writes exactly out[0..127] (the last sample runs after the loop, without a read-ahead);
 * the lamp kernels write exactly the block through I1, and every other access is k's own words or a circular ring.
 *
 * Host builds assemble it empty: arc.c keeps the C loops there, and a Blackfin build with -DARC_C_REF uses them too.
 * Arithmetic: Copyright (C) 2026 ultralazr (arc.c). Kernels: Copyright (C) 2026 Hundekraft.
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

/* one sample of pass 2: y in R0; \ld loads the next y (or NOP for the last sample), \last = the loop end label */
.macro SHAPE ld, last
	R1 = R0.L * R0.L (IS) || R2 = [I0++] || NOP;           /* -8192 */
	R1 = R1 >>> 12;
	R1 = R1 + R2 (NS) || R4 = [I0++] || NOP;               /* 32767 */
	R1 = MIN(R1, R4);                                       /* q */
	R1 = R1.L * R3.L (IS);
	R1 = R1 >>> 16;
	R0 = R0 + R1;                                           /* y += q cb >> 16 */
	R1 = R0 << 8;
	R1 = R1 - R6;
	R1 = R1 >>> 10;
	R6 = R6 + R1;                                           /* dc */
	R1 = R6 >>> 8 || R2 = [I0++] || NOP;                   /* 16383 */
	R0 = R0 - R1 (NS) || R4 = [I0++] || NOP;               /* -16383 */
	R0 = MIN(R0, R2);
	R0 = MAX(R0, R4);                                       /* y */
	R1 = R7 >>> 8;
	R2 = R0 - R1;
	R2 = R2.L * R3.H (IS);
	R2 = R2 >>> 7;
	R7 = R7 + R2;                                           /* lp */
	R1 = R7 >>> 8 || R4 = [I0++] || NOP;                   /* l; btq */
	R2 = R0 - R1;
	R2 = R2.L * R4.L (IS) || R0 = [I0++] || NOP;           /* 32767 */
	R2 = R2 >>> 12 || R4 = [I0++] || NOP;                  /* -32767 */
	R2 = R2 + R1;
	R2 = MIN(R2, R0);
	R2 = MAX(R2, R4) || \ld || NOP;                        /* o; the next y */
	R2 = R2.L * R5.H (IS) || R1 = [I0++] || NOP;           /* dg */
	R2 = R2 << 3 (S);
\last: R5 = R5 + R1 (NS) || W[I1++] = R2.H || NOP;            /* ga += dg; out */
.endm

	.global _arc_shape
	.type _arc_shape, STT_FUNC
_arc_shape:
	[--SP] = (R7:4);
	[--SP] = L0;
	[--SP] = L1;
	P1 = R1;                                                /* s */
	P0 = R0;                                                /* y in */
	I1 = R0;                                                /* out */
	R1 = 0;
	L1 = R1;
	R6 = [P1];                                            /* dc */
	R1 = P1;
	R2 = 16;
	R1 = R1 + R2;
	I0 = R1;
	B0 = R1;
	R1 = 32;
	L0 = R1;                                                /* the ring: 8 words */
	R7 = [P1 + 4];                                          /* lp */
	R5 = [P1 + 8];                                          /* ga */
	R3 = [P1 + 12];                                         /* cb | g << 16 */
	P2 = 127 (X);
	R0 = W[P0++] (X);                                       /* y */
	LSETUP(.Lsh_s, .Lsh_e) LC0 = P2;
.Lsh_s:
	SHAPE "R0 = W[P0++] (X)", .Lsh_e
	SHAPE NOP, .Lsh_x                                       /* out[127]: no read past the block */
	[P1] = R6;
	[P1 + 4] = R7;
	L1 = [SP++];
	L0 = [SP++];
	(R7:4) = [SP++];
	RTS;
	.size _arc_shape, .-_arc_shape

/* the rest of one main-lamp step (arc.c LAMP with x1 = R0, v1 = R1, r = R2): \xs = x1 >> 17; A1 = the coupling term
 * (Q27 before >> sh), or none with \first = 1; I0 at the step's five ring words CLK, E, AMINK, G, H; \nz = the
 * noise half of r; \st = a store for the first packet's free slot; \last = the loop's end label (on the last
 * packet). Uses R4, R6, R7. 16 packets. */
.macro STEP xs, nz, first, st, last
	R4 = \xs\().L * \xs\().L (IS) || R7 = [I0++] || \st;     /* CLK */
	R4 = R4 >>> 13;                                         /* s = xs^2 >> 13, 0 .. 32768 */
	A0 = R7 || R7 = [I0++] || NOP;                          /* E: he << 16 */
	R4 = (A0 -= R4.L * R7.H) (IU) || R7 = [I0++] || NOP;   /* 2^29 + 2048 he + 1024 - s he; AMINK */
	R4 = R4 >> 11;                                          /* 2^18 + a */
	R4 = MAX(R4, R7) || R7 = [I0++] || NOP;                /* a >= A_MIN; G: sg | -h << 16 */
	R4 = R1.H * R4.L;                                       /* 2 (v >> 16) a */
.if \first
	A1 = \xs\().L * R7.H (IS);                              /* -xs h */
.else
	A1 += \xs\().L * R7.H (IS);                             /* cpl - xs h */
.endif
	R1 = R1 + R4 (S) || R6 = [I0++] || NOP;                /* v; H: -sh | h << 16 */
	A1 = ASHIFT A1 BY R6.L;                                 /* >> sh */
	R7 = (A1 += \nz * R7.L) (IS);                           /* + nz sg */
	R1 = R1 + R7 (S);                                       /* v */
	R7 = R1.H * R6.H (IS);
	R7 = ASHIFT R7 BY R6.L;
	R7 = R7 << 3 (S);
\last: R0 = R0 + R7 (S);                                  /* x */
.endm

/* prologue of both lamp kernels: k (R0) in P5, out (R1) in I1, the ring at k + \ring (\len bytes) in I0, x1 / v1 / r
 * in R0 / R1 / R2, k's steps per chunk, chunks and portrait buffer in P0 / P1 / P2 */
.macro LAMP_ENTER ring, len
	[--SP] = (R7:4, P5:5);
	[--SP] = L0;
	[--SP] = L1;
	[--SP] = L2;
	[--SP] = L3;
	P5 = R0;
	R4 = R0;
	I1 = R1;
	R3 = 0;
	L1 = R3;
	R3 = \ring;
	R3 = R3 + R0;
	I0 = R3;
	B0 = R3;
	R3 = \len (X);
	L0 = R3;
	R0 = [P5];
	R1 = [P5 + 4];
	R2 = [P5 + 16];
	P0 = [P5 + 40];                                         /* steps per chunk */
	P1 = [P5 + 44];                                         /* chunks */
	R3 = [P5 + 48];
	I3 = R3;                                                /* the portrait ring at its next point */
	R3 = [P5 + 52];
	B3 = R3;
	R3 = [P5 + 56];
	L3 = R3;
	R3 = 60;
	R3 = R3 + R4;
	I2 = R3;
	B2 = R3;
	R3 = 28;
	L2 = R3;                                                /* ring3: the portrait's 7 words */
.endm
.macro LAMP_LEAVE
	[P5] = R0;
	[P5 + 4] = R1;
	[P5 + 16] = R2;
	L3 = [SP++];
	L2 = [SP++];
	L1 = [SP++];
	L0 = [SP++];
	(R7:4, P5:5) = [SP++];
	RTS;
.endm

/* the portrait ordinate of one lamp, arc.c lienard(\x, \v, epsd, vmul), into R4 (I2 at ring3: 12288, -12288, 10923,
 * epsd, vmul, 32767, -32767). Uses R3, R6, R7; \v must not be one of them. 18 packets. */
.macro LIEN x, v
	R6 = [I2++];
	R3 = MIN(\x, R6) || R6 = [I2++] || NOP;
	R3 = MAX(R3, R6) || R7 = [I2++] || NOP;                /* xs clipped to +-3 */
	R4 = R3.L * R3.L (IS);
	R4 = R4 >>> 12;
	R4 *= R3;
	R4 = R4 >>> 12;                                         /* x^3 */
	R4 *= R7;
	R4 = R4 >>> 15 || R7 = [I2++] || NOP;                  /* epsd */
	R4 = R3 - R4 (NS);                                      /* F = x - x^3 / 3, |F| <= 24576 */
	R4 = R4.L * R7.L (IS) || R7 = [I2++] || NOP;           /* epsd F; vmul */
	R4 = R4 >>> 12;
	R6 = \v >>> 15;
	R4 = R6 - R4 (NS);
	R4 *= R7;
	R4 = R4 >>> 12 || R6 = [I2++] || NOP;
	R4 = MIN(R4, R6) || R6 = [I2++] || NOP;
	R4 = MAX(R4, R6);
.endm

/* void arc_lamp1(ArcRun *k, int16_t *out): one lamp, k->m chunks of k->n samples; ring1 (k + 88): O, CLK, E, AMINK,
 * G, H */
	.global _arc_lamp1
	.type _arc_lamp1, STT_FUNC
_arc_lamp1:
	LAMP_ENTER 88, 24
	LSETUP(.Ll1_cs, .Ll1_ce) LC1 = P1;
.Ll1_cs:
	LSETUP(.Ll1_s, .Ll1_e) LC0 = P0;
.Ll1_s:
	R3 = R2 << 13 || R6 = [I0++] || NOP;                   /* O: hiss | mix << 16 */
	R2 = R2 ^ R3;
	R3 = R2 >> 17;
	R2 = R2 ^ R3;
	R3 = R2 << 5;
	R2 = R2 ^ R3;                                           /* r: xorshift32 */
	R5 = R0 >>> 17;                                         /* xs */
	R4.H = R2.L * R6.L (T);                                 /* (r.L hiss) >> 15 */
	R3 = ABS R5;
	R4 = R4.H * R3.L (IS);
	R4 = R4 >>> 13;
	R3 = R4 + R5 (NS);                                      /* out: xs + the hiss riding on |x| */
	STEP R5, R2.H, 1, "W[I1++] = R3.L", .Ll1_e
	LIEN R5, R1                                             /* the chunk's portrait point: xs, lienard(xs, v1) twice */
	R3 = PACK(R4.L, R5.L);
	[I3++] = R3;
.Ll1_ce: [I3++] = R3;
	LAMP_LEAVE
	.size _arc_lamp1, .-_arc_lamp1

/* void arc_lamp2(ArcRun *k, int16_t *out): both lamps, k->m chunks of k->n sample pairs; ring2 (k + 112): K2, CLK2, E2,
 * AMINK, G2, H2, M, then CLK1, E1, AMINK, G1, H1, O, MASK, CLK1, E1, AMINK, G1, H1. k: x2 at +8, v2 at +12, xt at +24,
 * v1 >> 17 at +28, the coupling at +32, xp / XP at +36. */
	.global _arc_lamp2
	.type _arc_lamp2, STT_FUNC
_arc_lamp2:
	LAMP_ENTER 112, 76
	LSETUP(.Ll2_cs, .Ll2_ce) LC1 = P1;
.Ll2_cs:
	LSETUP(.Ll2_s, .Ll2_e) LC0 = P0;                        /* pairs */
.Ll2_s:
	R3 = R2 << 13 || R4 = [P5 + 8] || NOP;                 /* x2 */
	R2 = R2 ^ R3;
	R3 = R2 >> 17 || R5 = [P5 + 12] || NOP;                /* v2 */
	R2 = R2 ^ R3;
	R3 = R2 << 5 || R7 = [I0++] || NOP;                    /* K2: -hk2 */
	R2 = R2 ^ R3;                                           /* r */
	/* the 2nd lamp: x2 = R4, v2 = R5, xp = R3 */
	R6 = R1 >>> 17;
	R3 = R5 >>> 17 || [P5 + 28] = R6 || NOP;
	R3 = R3 - R6 (NS);                                      /* dv = (v2 >> 17) - (v1 >> 17) */
	A1 = R3.L * R7.L (IS) || R7 = [I0++] || NOP;           /* cpl = -dv hk2; CLK2 */
	R3 = R4 >>> 17;                                         /* xp */
	R6 = R3.L * R3.L (IS);
	R6 = R6 >>> 13;
	A0 = R7 || R7 = [I0++] || NOP;                          /* E2 */
	R6 = (A0 -= R6.L * R7.H) (IU) || R7 = [I0++] || NOP;   /* AMINK */
	R6 = R6 >> 11;
	R6 = MAX(R6, R7) || R7 = [I0++] || NOP;                /* G2 */
	R6 = R5.H * R6.L;
	A1 += R3.L * R7.H (IS);
	R5 = R5 + R6 (S) || R6 = [I0++] || NOP;                /* H2 */
	A1 = ASHIFT A1 BY R6.L || [P5 + 36] = R3 || NOP;
	R3 = R2 >> 8;                                           /* nz = r >> 8 (its low half) */
	R7 = (A1 += R3.L * R7.L) (IS);
	R5 = R5 + R7 (S);                                       /* v2 */
	R7 = R5.H * R6.H (IS);
	R7 = ASHIFT R7 BY R6.L || [P5 + 12] = R5 || NOP;
	R7 = R7 << 3 (S);
	R4 = R4 + R7 (S) || R3 = [P5 + 36] || NOP;             /* x2; xp */
	R7 = R4 >>> 17 || [P5 + 8] = R4 || R6 = [I0++];        /* xt; M: hk1 */
	R3 = R3 + R7 (NS) || [P5 + 24] = R7 || NOP;
	R3 = R3 >>> 1 || R4 = [P5 + 28] || NOP;                /* xp = (xp + xt) >> 1; v1 >> 17 */
	R3 = PACK(R7.L, R3.L);                                  /* XP = xp | xt << 16 */
	R7 = R5 >>> 17 || [P5 + 36] = R3 || NOP;
	R7 = R7 - R4 (NS);
	R5 = (A1 = R7.L * R6.L) (IS);                           /* the main lamp's coupling, held for the pair */
	/* the main lamp, two steps */
	R3 = R0 >>> 17 || [P5 + 32] = R5 || NOP;               /* xs1 */
	STEP R3, R2.H, 0, NOP, .Ll2_x
	R5 = R0 >>> 17;                                         /* xs2 */
	R3 = PACK(R5.L, R3.L) || R4 = [P5 + 36] || R6 = [I0++];    /* XS = xs1 | xs2 << 16; XP; O */
	R4 = R4 -|- R3 || R7 = [I0++] || NOP;                  /* XP - XS; MASK */
	R4 = R4 & R7;                                           /* 2 ((XP - XS) >> 1) per half */
	R4.H = R4.H * R6.H, R4.L = R4.L * R6.H (T);            /* (((XP - XS) >> 1) mix) >> 14 */
	R7.H = R2.H * R6.L, R7.L = R2.L * R6.L (T);            /* q = (r hiss) >> 15 per half */
	R6 = ABS R3 (V);
	R7 = R7 << 2 (V);
	R7.H = R7.H * R6.H, R7.L = R7.L * R6.L (T);            /* (q |xs|) >> 13 */
	R3 = R3 +|+ R4;
	R6 = R3 +|+ R7 || R4 = [P5 + 32] || NOP;               /* out[0] | out[1] << 16; the coupling */
	A1 = R4 || W[I1++] = R6.L || NOP;
	STEP R5, R2.L, 0, "W[I1++] = R6.H", .Ll2_e
	LIEN R5, R1                                             /* the chunk's portrait point: xs, lienard(xs, v1), */
	R3 = PACK(R4.L, R5.L);                                  /* xt, lienard(xt, v2) */
	[I3++] = R3;
	[P5 + 4] = R1;
	R5 = [P5 + 24];
	R1 = [P5 + 12];
	LIEN R5, R1
	R3 = PACK(R4.L, R5.L);
	[I3++] = R3;
.Ll2_ce: R1 = [P5 + 4];
	LAMP_LEAVE
	.size _arc_lamp2, .-_arc_lamp2

#endif
