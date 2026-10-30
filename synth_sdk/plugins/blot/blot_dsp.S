/* BLOT's oscillator pass on the Blackfin: pass 1 of blot's bl_loop (blot.c / blot_dsp.S), unchanged, bit for bit the C
 * bl_osc in blot.c (which host builds and -DBLOT_C_REF use). The ladder (pass 2 of blot's loop) is C in blot.c.
 *
 * void bl_osc(bl_blk *b)    b: 0 p, 20 inc, 24 out, 32 wave, 44 rinv, 48 sh, 84 w
 * per sample p += inc[n]; out[n] = osc >> 1, osc = saw(p) (- saw(p + 2^31) wave >> 15 when wave != 0).
 *   saw(q) = clip((q ^ 2^31) >>> 16 + (e e >> 15) - (t t >> 15), +-32767), e = 32768 - frac(q), t = 32768 - frac(-q),
 *   frac(x) = min(min(x >> sh, 65535) rinv >> 14, 32767) (unsigned products: FU, the integer product).
 *   Fast path (w != 0): while q is >= w from a wrap (q + w >= 2 w unsigned; for square also q + 2^31: the test on
 *   (q + w) mod 2^31) both polyBLEP terms are 0 and the clip idle (blot.c): saw(q) = (q ^ 2^31) >>> 16, so osc >> 1 =
 *   (q ^ 2^31) >>> 17 for saw; saw(q + 2^31) = q >>> 16 (never -32768 there), s1 wave >> 15 as one truncating
 *   fractional product. Near a wrap the loop jumps out to the full saw(s) and back to its last instruction (the store).
 *   Fast loops: R0 p, R3 w, R4 2 w, R5 wave. Near a wrap and in the full loops: R7 {rinv : -sh}, R6 32767.
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

	.align 4
	.global _bl_osc
	.type _bl_osc, STT_FUNC
_bl_osc:
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
	JUMP .Lend;
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
	JUMP .Lend;

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
	JUMP .Lend;
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

.Lend:
	[P5] = R0;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _bl_osc, .-_bl_osc
#endif
