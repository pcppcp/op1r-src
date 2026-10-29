/* BLOT's sample loop on the Blackfin: blot.c's bl_loop (the C, kept for host builds and -DBLOT_C_REF), bit for bit.
 * Every product is one 16 x 16 MAC (the C's operands are 16-bit by construction: see blot.c); no 32-bit multiply.
 *
 * void bl_loop(bl_blk *b)    b: 0 p, 4 y[4], 20 inc, 24 out, 28 k, 32 wave, 36 cd, 40 gain, 44 rinv, 48 sh, 52 g[16],
 *                               84 w
 * pass 1: per sample p += inc[n]; out[n] = osc >> 1, osc = saw(p) (- saw(p + 2^31) wave >> 15 when wave != 0).
 *   saw(q) = clip((q ^ 2^31) >>> 16 + (e e >> 15) - (t t >> 15), +-32767), e = 32768 - frac(q), t = 32768 - frac(-q),
 *   frac(x) = min(min(x >> sh, 65535) rinv >> 14, 32767) (unsigned products: FU, the integer product).
 *   Fast path (w != 0): while q is >= w from a wrap (q + w >= 2 w unsigned; for square also q + 2^31: the test on
 *   (q + w) mod 2^31) both polyBLEP terms are 0 and the clip idle (blot.c): saw(q) = (q ^ 2^31) >>> 16, so osc >> 1 =
 *   (q ^ 2^31) >>> 17 for saw; saw(q + 2^31) = q >>> 16 (never -32768 there), s1 wave >> 15 as one truncating
 *   fractional product. Near a wrap the loop jumps out to the full saw(s) and back to its last instruction (the store).
 *   Fast loops: R0 p, R3 w, R4 2 w, R5 wave. Near a wrap and in the full loops: R7 {rinv : -sh}, R6 32767.
 * pass 2, in place: u = soft(out[n] - (k y3 >>> 12)); y_i += sat16((x g - y_i g) >>> 14), four stages, each one
 *   fractional MAC chain on 2 g with a truncated 16-bit result (A0 = 2 x (2g), A0 -= 2 y (2g), its high half = the
 *   step, saturated); o = clip(o + (o >>> 1), 32767), o = soft(y3 cd >>> 12) (soft peaks at +-21846); out[n] = o gain
 *   >> 15 (truncating fractional, 16 x 16). soft(x) = x - (((x x >> 15) x >> 15) 10923 >> 15) on x clipped to
 *   +-32767: three truncating fractional products (no operand is -32768). Stages packed R6 = {y1 : y0}, R5 =
 *   {y3 : y2} (16-bit: each stays between its input and its state); R4 = {k : 2 g}, R3 = {gain : cd}, R7 =
 *   {-32767 : 10923} (the clips: saturating << 16, then MAX with R7: the value in the high half); g per 8 samples.
 * Callee-saved R4..R7, P3..P5 are saved; no circular addressing (L0..L3 stay 0), no M registers. The fast loops leave
 * the hardware loop and come back to its last instruction near a wrap (the loop runs on, LC0 untouched).
 * Host builds assemble it empty.
 * Copyright (C) 2026 pcppcp. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

/* d = saw(q) (d != q), temps t1 t2 t3; R7 = {rinv : -sh}, R6 = 32767 */
.macro SAW d, q, t1, t2, t3
	\t3 = 65535 (Z);
	\t1 = LSHIFT \q BY R7.L;            /* q >> sh */
	CC = \t3 < \t1 (IU);
	IF CC \t1 = \t3;                    /* <= 65535 */
	\t1 = \t1\().L * R7.H (FU);         /* x rinv */
	\t1 >>= 14;
	\t1 = MIN(\t1, R6);                 /* frac(q) */
	\t2 = -\q;                          /* 2^32 - q */
	\t2 = LSHIFT \t2 BY R7.L;
	CC = \t3 < \t2 (IU);
	IF CC \t2 = \t3;
	\t2 = \t2\().L * R7.H (FU);
	\t2 >>= 14;
	\t2 = MIN(\t2, R6);                 /* frac(-q) */
	\t3 = 32768 (Z);
	\t1 = \t3 - \t1;                    /* e */
	\t2 = \t3 - \t2;                    /* t */
	\t1 = \t1\().L * \t1\().L (FU);
	\t1 >>= 15;
	\t2 = \t2\().L * \t2\().L (FU);
	\t2 >>= 15;
	\t1 = \t1 - \t2;
	\d = \q;
	BITTGL(\d, 31);
	\d >>>= 16;                         /* (q >> 16) - 32768 */
	\d = \d + \t1;
	\d = MIN(\d, R6);
	\t3 = -R6;
	\d = MAX(\d, \t3);                  /* +-32767 */
.endm

/* R0 = soft(R0) in R0.L (R0.H undefined), temp R1; R7 = {-32767 : 10923}. The clip: a saturating << 16 puts
 * sat16(x) in R0.H (low half 0), the MAX with R7 (= -32767 << 16 | 10923) lifts -32768 to -32767 */
.macro SOFT
	R0 = R0 << 16 (S);
	R0 = MAX(R0, R7);                   /* x in R0.H, +-32767 */
	R1.L = R0.H * R0.H (T);             /* x x >> 15 */
	R1.L = R1.L * R0.H (T);             /* .. x >> 15 */
	R1.L = R1.L * R7.L (T);             /* .. 10923 >> 15 */
	R0.L = R0.H - R1.L (NS);
.endm

/* y += sat16((x g - y g) >> 14): x, y register halves; R4.L = 2 g; temp R2 */
.macro STAGE x, y
	A0 = \x * R4.L;
	R2.L = (A0 -= \y * R4.L) (T);
	\y = \y + R2.L (NS);
.endm

	.align 4
	.global _bl_loop
	.type _bl_loop, STT_FUNC
_bl_loop:
	[--SP] = (R7:4, P5:3);
	P5 = R0;

	/* ---- pass 1: the oscillator into out[] ---- */
	R0 = [P5];                          /* p */
	P3 = [P5 + 20];                     /* inc */
	P4 = [P5 + 24];                     /* out */
	R7 = [P5 + 48];
	R7 = -R7;                           /* -sh */
	R1 = [P5 + 44];
	R7 = PACK(R1.L, R7.L);              /* {rinv : -sh} */
	R6 = 32767 (X);
	R5 = [P5 + 32];                     /* wave */
	R3 = [P5 + 84];                     /* w */
	R4 = R3 << 1;                       /* 2 w */
	P0 = 128;
	CC = R5 == 0;
	IF !CC JUMP .Lsq;
	CC = R3 == 0;
	IF CC JUMP .Lsaw_full;

	/* saw, fast */
	LSETUP (.Lsf_b, .Lsf_e) LC0 = P0;
.Lsf_b:
	R1 = [P3++];
	R0 = R0 + R1;                       /* p */
	R1 = R0 + R3;
	CC = R1 < R4 (IU);
	IF CC JUMP .Lsf_near;
	R1 = R0;
	BITTGL(R1, 31);
	R1 >>>= 17;                         /* osc >> 1 */
.Lsf_e:
	W[P4++] = R1;
	JUMP .Lpass2;
.Lsf_near:
	SAW R1, R0, R2, R3, R4
	R1 >>>= 1;
	R3 = [P5 + 84];
	R4 = R3 << 1;
	JUMP .Lsf_e;

	/* saw, full (high notes) */
.Lsaw_full:
	LSETUP (.Lsu_b, .Lsu_e) LC0 = P0;
.Lsu_b:
	R1 = [P3++];
	R0 = R0 + R1;
	SAW R1, R0, R2, R3, R4
	R1 >>>= 1;
.Lsu_e:
	W[P4++] = R1;
	JUMP .Lpass2;

.Lsq:
	CC = R3 == 0;
	IF CC JUMP .Lsq_full;

	/* square, fast */
	LSETUP (.Lqf_b, .Lqf_e) LC0 = P0;
.Lqf_b:
	R1 = [P3++];
	R0 = R0 + R1;
	R1 = R0 + R3;
	BITCLR(R1, 31);                     /* (q + w) mod 2^31: near either wrap */
	CC = R1 < R4 (IU);
	IF CC JUMP .Lqf_near;
	R1 = R0;
	BITTGL(R1, 31);
	R1 >>>= 16;                         /* saw(p) */
	R2 = R0 >>> 16;                     /* saw(p + 2^31) */
	R2.L = R2.L * R5.L (T);             /* x wave >> 15 */
	R2 = R2.L (X);
	R1 = R1 - R2;
	R1 >>>= 1;
.Lqf_e:
	W[P4++] = R1;
	JUMP .Lpass2;
.Lqf_near:
	SAW R1, R0, R2, R3, R4              /* saw(p) */
	W[P4] = R1;
	R5 = R0;
	BITTGL(R5, 31);
	SAW R1, R5, R2, R3, R4              /* saw(p + 2^31) */
	R5 = [P5 + 32];                     /* wave */
	R1 = R1.L * R5.L (IS);
	R1 >>>= 15;
	R2 = W[P4] (X);
	R1 = R2 - R1;
	R1 >>>= 1;
	R3 = [P5 + 84];
	R4 = R3 << 1;
	JUMP .Lqf_e;

	/* square, full (high notes) */
.Lsq_full:
	LSETUP (.Lqu_b, .Lqu_e) LC0 = P0;
.Lqu_b:
	R1 = [P3++];
	R0 = R0 + R1;
	SAW R1, R0, R2, R3, R4
	W[P4] = R1;
	R5 = R0;
	BITTGL(R5, 31);
	SAW R1, R5, R2, R3, R4
	R5 = [P5 + 32];
	R1 = R1.L * R5.L (IS);
	R1 >>>= 15;
	R2 = W[P4] (X);
	R1 = R2 - R1;
	R1 >>>= 1;
.Lqu_e:
	W[P4++] = R1;

.Lpass2:
	[P5] = R0;

	/* ---- pass 2: the ladder and the output stage, in place ---- */
	R0 = [P5 + 4];
	R1 = [P5 + 8];
	R6 = PACK(R1.L, R0.L);              /* {y1 : y0} */
	R0 = [P5 + 12];
	R1 = [P5 + 16];
	R5 = PACK(R1.L, R0.L);              /* {y3 : y2} */
	R0 = [P5 + 28];
	R4 = PACK(R0.L, R4.L);              /* {k : (2 g)} */
	R0 = [P5 + 40];
	R1 = [P5 + 36];
	R3 = PACK(R0.L, R1.L);              /* {gain : cd} */
	R7.H = 0x8001;
	R7.L = 10923;                       /* {-32767 : 10923} */
	P4 = [P5 + 24];
	P2 = P5;
	P2 += 52;                           /* g[] */
	P1 = 16;
	P0 = 8;
	LSETUP (.Lo_b, .Lo_e) LC1 = P1;
.Lo_b:
	R1 = W[P2++] (X);
	R1 <<= 1;
	R4 = PACK(R4.H, R1.L);              /* 2 g of these 8 samples */
	LSETUP (.Li_b, .Li_e) LC0 = P0;
.Li_b:
	R0 = W[P4] (X);                     /* osc >> 1 */
	R1 = R5.H * R4.H (IS);
	R1 >>>= 12;                         /* k y3 >> 12 */
	R0 = R0 - R1;
	SOFT                                /* u */
	STAGE R0.L, R6.L
	STAGE R6.L, R6.H
	STAGE R6.H, R5.L
	STAGE R5.L, R5.H
	R0 = R5.H * R3.L (IS);
	R0 >>>= 12;                         /* y3 cd >> 12 */
	SOFT
	R0 = R0.L (X);
	R1 = R0 >>> 1;
	R0 = R0 + R1;
	R0 = R0 << 16 (S);                  /* soft() peaks at +-21846: +-32769, clipped to +-32767 in R0.H */
	R0 = MAX(R0, R7);
	R0.L = R0.H * R3.H (T);             /* x gain >> 15 */
.Li_e:
	W[P4++] = R0;
.Lo_e:
	NOP;

	R0 = R6.L (X);
	[P5 + 4] = R0;
	R0 = R6 >>> 16;
	[P5 + 8] = R0;
	R0 = R5.L (X);
	[P5 + 12] = R0;
	R0 = R5 >>> 16;
	[P5 + 16] = R0;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _bl_loop, .-_bl_loop
#endif
