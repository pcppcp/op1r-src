/* The master EQ's FIR (docs/81), as R246's libdsp fir_fr16 computes it for one 64-sample block: our own code, the same
 * results (checked per block against R246's master_eq, dsp/eq.c). Per sample: the new x goes into the newest slot
 * e = p + 1 of the circular delay line d (k samples), y = sum h[j] d[e + j] (fractional MACs into the 40-bit A0, one
 * per cycle), rounded to 16 bits by the accumulator extraction (RND_MOD, saturating); then e moves one slot back;
 * at the end p = e - 1. Assembled twice: into .text (check builds) and into the L1 copy (dsp/fir_l1.S).
 *   void eq_fir(fir_st *s, const int16_t *x, int16_t *y)    s = { h, d, p, k }
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef FIR_SECTION
#define FIR_SECTION .text
#define FIR_NAME _eq_fir
#endif
	.section FIR_SECTION,"ax"
	.global FIR_NAME
	.align 4
FIR_NAME:
	P2 = R0;			/* state */
	P0 = R1;			/* x */
	P1 = R2;			/* y */
	R0 = [P2];			/* h */
	R1 = [P2 + 4];			/* d */
	R2 = [P2 + 8];			/* p */
	R3 = [P2 + 12];			/* k */
	I3 = P2;
	B0 = R1;
	I0 = R2;
	R1 = R3 + R3;
	L0 = R1;			/* d: k int16, circular */
	I2 = R0;
	P2 = R3;			/* taps */
	I0 += 2;			/* e = p + 1 */
	R3 = 64;
1:	R0 = W[P0++] (X);
	W[I0] = R0.L;			/* x into the newest slot */
	I1 = I2;
	A0 = 0;
	R0.L = W[I0++];
	R1.L = W[I1++];
	LSETUP (2f, 2f) LC0 = P2;
2:	A0 += R0.L * R1.L || R0.L = W[I0++] || R1.L = W[I1++];
	R0.L = A0;
	W[P1++] = R0;
	I0 -= 4;			/* k + 1 slots on: e + 1; the next e is e - 1 */
	R3 += -1;
	CC = R3 == 0;
	IF !CC JUMP 1b;
	I0 -= 2;			/* p = e - 1 */
	R0 = I0;
	P2 = I3;
	[P2 + 8] = R0;
	L0 = 0;
	RTS;
