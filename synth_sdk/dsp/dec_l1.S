/* The instrument bus decimator (docs/81 stage 4), in place of R246's synth_bus_decimate (L1 0xFFA005D4, 176 bytes):
 * the same results as dsp/dec.c's model (checked), two outputs per pass with both multipliers. With v_k =
 * A[(i + 1 + k) & 31] (k = 0..31, A[i] = x[2i] already written) and v_32 = x[2i + 2] (A[i + 1] is written only after
 * the pass: y[i] still reads its old value as v_0):
 *   y[i] acc A0 = B[i + 16] << 15, += h[k] v_k (k = 0..31); y[i+1] acc A1 = B[i + 17] << 15, += h[k-1] v_k (k = 1..32),
 * both in R246's order (W32, saturating per MAC; the extra h[32] = 0 / h[-1] = 0 products change nothing).
 * Registers as R246's: only M0 of the M registers (its caller keeps a pointer in M1), L1-L3 0 at the end.
 * Pairs hp[k] = (h[k], h[k-1]) are built on the stack every block (h: R246's table 0x0106A6F0, filled at its init). */
	.section .l1dec,"ax"
	.global _l1_dec
	.align 2
_l1_dec:
	[--SP] = (R7:4, P5:3);
	[--SP] = RETS;				/* the helper call below */
	[--SP] = L0;				/* R246's caller may leave L0 set: I0 must be linear here */
	call _l1_dec_hp;			/* in the free end of .l1mix: hp, and P3 x, P4 y, P5 / R7 gain, step */
	I1.L = 0x123C; I1.H = 0xFF80; B1 = I1; L1 = 64;	/* A */
	I3.L = 0x127C; I3.H = 0xFF80; B3 = I3; L3 = 64;	/* B, written */
	I2 = I3; B2 = I3; L2 = 64;
	M0 = 32; I2 += M0;			/* B, read from index 16 */
	P2 = 32;
	LSETUP (3f, 4f) LC1 = P2;
3:	R4 = [P3++];				/* x[2i], x[2i+1] */
	R5 = [P3++];				/* x[2i+2], x[2i+3] */
	W[I3++] = R4.H;
	W[I3++] = R5.H;
	W[I1++] = R4.L;				/* A[i] */
	R0.L = W[I2++];
	R0 = R0.L (X);
	R0 <<= 15;
	A0 = R0;
	R0.L = W[I2++];
	R0 = R0.L (X);
	R0 <<= 15;
	A1 = R0;
	P0 = SP;
	R1 = [P0++];
	R0.L = W[I1++];
	P2 = 32;
	LSETUP (5f, 5f) LC0 = P2;
5:	A1 += R0.L * R1.H, A0 += R0.L * R1.L (W32) || R1 = [P0++] || R0.L = W[I1++];
	A1 += R5.L * R1.H, A0 += R5.L * R1.L (W32);
	I1 -= 2;				/* the pass read one past: back to A[i + 1] */
	W[I1++] = R5.L;				/* A[i + 1] */
	R2 = A0;
	R6.L = R2.H * R7.L;
	R7.L = R7.L + R7.H (S);
	R1 = A1;
	R6.H = R1.H * R7.L;
	R7.L = R7.L + R7.H (S);
4:	[P4++] = R6;
	W[P5] = R7;
	L1 = 0; L2 = 0; L3 = 0;
	P0 = 132;
	SP = SP + P0;
	L0 = [SP++];
	RETS = [SP++];
	(R7:4, P5:3) = [SP++];
	RTS;

/* hp[k] = (h[k], h[k-1]), k = 0..32, at SP (the caller's 132-byte area; CALL keeps RETS in a register) */
	.section .l1mix.1,"ax"
	.global _l1_dec_hp
	.align 2
_l1_dec_hp:
	L0 = 0;
	P0 = -132;				/* the caller's hp area (CALL keeps RETS in a register) */
	SP = SP + P0;
	P0.L = 0xA6F0; P0.H = 0x0106;		/* h */
	P1 = SP;
	R1 = 0;					/* h[k-1] */
	P2 = 32;
	LSETUP (1f, 2f) LC0 = P2;
1:	R0 = W[P0++] (Z);
	R2 = R1 << 16;
	R2 = R2 | R0;
	[P1++] = R2;
2:	R1 = R0;
	R1 <<= 16;
	[P1] = R1;				/* hp[32] = (0, h[31]) */
	P3.L = 0x0BB4; P3.H = 0xFF80;		/* x */
	P4.L = 0x1E50; P4.H = 0xFF80;		/* y */
	P5.L = 0x107C; P5.H = 0xFF80;		/* gain, step */
	R7 = [P5];
	RTS;
