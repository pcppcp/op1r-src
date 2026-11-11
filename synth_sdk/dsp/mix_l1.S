/* Instrument bus into master L / R (docs/81 stage 3a), in place of R246's two loops in Core_mix_master
 * (0xFFA00E58..0xFFA00FDC, 388 bytes): one pass per sample instead of two (per sample the same operations in the
 * same order, so bit-exact with the model dsp/mix.c). Entered inside R246's frame (after LINK / saves / SP -= 16):
 * R0 = the argument; R4-R7, P3-P5 are restored by R246's epilogue. Leaves what the rest of the function reads:
 * [SP + 0x2C] = R0, [SP + 0x38] = &flag 0xFF80103C, [FP - 8] = &peak 0xFF801084, W[FP + 0x10] = vl b,
 * [FP - 4] = &m 0x01B4EF6E and P3 = 0xFF800BB4 (both read by the master monitor 0xFFA013A0); the slot meter
 * (flag) follows at 0xFFA00FDC, R246's. */
	.section .l1mix,"ax"
	.global _l1_mix
	.align 2
_l1_mix:
	[SP + 0x2C] = R0;
	P0.L = 0x103C; P0.H = 0xFF80; [SP + 0x38] = P0;
	P1.L = 0x1084; P1.H = 0xFF80; [FP - 8] = P1;
	P2.L = 0x2964; P2.H = 0xFF80; P2 = [P2];		/* slot index */
	P1.L = 0x1928; P1.H = 0xFF80; P1 = P1 + (P2 << 1);
	R6 = W[P1] (X);				/* vol */
	R7 = W[P1 + 8] (X);			/* pan */
	R3 = W[P1 + -0x74] (X);			/* lev (0xFF8018B4) */
	R0.H = 0x4000;
	R1.H = R0.H - R7.L (S);
	R2.H = R7.L + R0.H (S);
	R4.H = R6.L * R3.L;			/* vl */
	R1.H = R1.H << 1 (S);
	R2.H = R2.H << 1 (S);
	R1.H = R4.H * R1.H;			/* vl a */
	R2.H = R4.H * R2.H;			/* vl b */
	R7 = R2 >> 16;
	W[FP + 0x10] = R7;
	P2.L = 0xEF6E; P2.H = 0x01B4; R6 = W[P2] (X);	/* m */
	R0.L = R6.L * R1.H;			/* gL */
	R0.H = R6.L * R2.H;			/* gR */
	P2 = [FP - 8]; P2 += -4;		/* ramps: r2 0xFF801080, r1 +2, peak +4 */
	R5 = W[P2] (X);
	R4 = W[P2 + 2] (X);
	R1 = -0x80 (X);				/* s1 */
	R2 = -0x80 (X);				/* s2 */
	P0 = [SP + 0x38]; R3 = B[P0] (X);	/* flag */
	CC = R3 == 0;
	R3 = 0x80 (X);
	R7 = 0;
	IF CC JUMP 1f;
	CC = R5 < 0; R2 = R7; IF CC R2 = R3;	/* flag: s2 = r2 < 0 ? 0x80 : 0 */
	JUMP.S 2f;
1:	CC = R4 < 0; R1 = R7; IF CC R1 = R3;	/* no flag: s1 = r1 < 0 ? 0x80 : 0 */
2:	R5 = PACK (R5.L, R4.L);			/* H r2, L r1 */
	R4 = PACK (R2.L, R1.L);			/* H s2, L s1 */
	R3 = 0;					/* peak */
	I0.L = 0x0BB4; I0.H = 0xFF80;		/* instrument bus */
	P0.L = 0x0D34; P0.H = 0xFF80;		/* master L */
	P1.L = 0x0DB4; P1.H = 0xFF80;		/* master R */
	P4 = 64;
	LSETUP (3f, 4f) LC0 = P4;
3:	R7.L = W[I0++];
	R1.L = R7.L * R0.L;			/* x gL */
	R1.H = R7.L * R0.H;			/* x gR */
	R7.L = R7.L * R6.L;			/* t = x m */
	R2.L = R7.L * R5.H;			/* t r2 */
	R1.L = R1.L * R5.L;			/* (x gL) r1 */
	R1.H = R1.H * R5.L;			/* (x gR) r1 */
	R7 = R7.L (X);
	R7 = ABS R7 (V);
	R3 = MAX (R3, R7) (V);			/* peak (low half) */
	R7 = W[P0] (X);
	R7.L = R7.L + R1.L (S);
	R7.L = R7.L + R2.L (S);
	W[P0++] = R7;
	R7 = W[P1] (X);
	R7.L = R7.L + R1.H (S);
	R7.L = R7.L + R2.L (S);
	W[P1++] = R7;
4:	R5 = R5 +|+ R4 (S);			/* ramps */
	R7 = R5 >> 16;
	W[P2] = R7;
	W[P2 + 2] = R5;
	W[P2 + 4] = R3;
	P3.L = 0x0BB4; P3.H = 0xFF80;		/* R246's loops leave these for the master monitor (0xFFA013A0): */
	P0.L = 0xEF6E; P0.H = 0x01B4;		/* P3 = the bus (its output), [FP - 4] = &m */
	[FP - 4] = P0;
	JUMP.L _mix_ret;
