/* The master EQ's FIR (docs/81), as R246's libdsp fir_fr16 computes it for one 64-sample block: our own code, the same
 * results (checked per block against R246's master_eq, dsp/eq.c). Delay line d (k samples, circular), newest slot
 * e = p + 1; per sample: x into slot e, y = sum h[j] d[e + j] (fractional MACs, 40 bits), rounded to 16 bits by the
 * accumulator extraction; e moves one slot back; at the end p = e - 1.
 * Two samples per pass (both multipliers): with D[m] = d[e + m] (x[i] already in D[0], D[k-1] still the oldest value,
 * which x[i+1] overwrites only after the pass): y[i] = sum h[m] D[m] (A0), y[i+1] = h[0] x[i+1] + sum h[m+1] D[m]
 * (A1); each D[m] is loaded once and multiplied with the pair hp[m] = (h[m], h[m+1]) (hp[k-1].H = 0).
 *   void eq_fir(fir_st *s, const int16_t *x, int16_t *y, const int32_t *hp)    s = { h, d, p, k }; x, y 4-aligned
 * Assembled twice: into .text (check builds) and into the L1 copy (dsp/fir_l1.S).
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef FIR_SECTION
#define FIR_SECTION .text
#define FIR_NAME _eq_fir
#define MID_NAME _eq_mid
#define OP_NAME _eq_onepole
#define MIX_NAME _eq_mix
#endif
	.section FIR_SECTION,"ax"
	.global FIR_NAME
	.align 4
FIR_NAME:
	P0 = [SP + 12];			/* hp (4th argument) */
	[--SP] = R4;
	[--SP] = P3;
	P2 = R0;			/* state */
	P3 = R1;			/* x */
	P1 = R2;			/* y */
	R0 = [P2 + 4];			/* d */
	R1 = [P2 + 8];			/* p */
	R3 = [P2 + 12];			/* k */
	I3 = P2;
	B0 = R0;
	I0 = R1;
	R1 = R3 + R3;
	L0 = R1;			/* d: k int16, circular */
	I2 = P0;			/* hp */
	R4 = [P0];			/* R4.L = h[0] */
	P2 = R3;			/* taps */
	I0 += 2;			/* e = p + 1 */
	P0 = 32;			/* pairs */
1:	R3 = [P3++];			/* R3.L = x[i], R3.H = x[i+1] */
	W[I0] = R3.L;			/* x[i] into slot e */
	I1 = I2;
	A1 = R4.L * R3.H;		/* h[0] x[i+1] */
	A0 = 0;
	R0.L = W[I0++];
	R1 = [I1++];
	LSETUP (2f, 2f) LC0 = P2;
2:	A1 += R1.H * R0.L, A0 += R1.L * R0.L || R0.L = W[I0++] || R1 = [I1++];
	R2.H = A1, R2.L = A0;
	[P1++] = R2;			/* y[i], y[i+1] */
	I0 -= 4;			/* k + 1 slots on: e + 1; back to e - 1 */
	W[I0] = R3.H;			/* x[i+1] into slot e - 1 */
	I0 -= 2;			/* the next e: e - 2 */
	P0 += -1;
	CC = P0 == 0;
	IF !CC JUMP 1b;
	I0 -= 2;			/* p = e - 1 */
	R0 = I0;
	P2 = I3;
	[P2 + 8] = R0;
	L0 = 0;
	P3 = [SP++];
	R4 = [SP++];
	RTS;

/* mid band: MID[i] = sat16(sat16(dly[di] - LO[i]) - HI[i]), then dly[di] = x[i], di = (di + 1) mod 25
 *   void eq_mid(const int16_t *x, int16_t *dly, int32_t *di)       LO / HI / MID: 0x01AABB28 / BBA8 / BC28 */
	.global MID_NAME
	.align 4
MID_NAME:
	P0 = R0;			/* x */
	B1 = R1;
	I0 = R2;			/* &di (kept) */
	P2 = R2;
	R2 = [P2];
	R2 = R2 + R2;
	R1 = R1 + R2;
	I1 = R1;
	R2 = 50;
	L1 = R2;			/* dly: 25 int16, circular */
	P1.L = 0xBB28; P1.H = 0x01AA;	/* LO */
	I2 = P1;
	P1.L = 0xBBA8; P1.H = 0x01AA;	/* HI */
	I3 = P1;
	P1.L = 0xBC28; P1.H = 0x01AA;	/* MID */
	P2 = 64;
	LSETUP (3f, 4f) LC0 = P2;
3:	R0.L = W[I1];
	R1.L = W[I2++];
	R0.L = R0.L - R1.L (S);
	R1.L = W[I3++];
	R0.L = R0.L - R1.L (S);
	W[P1++] = R0;
	R2 = W[P0++] (X);
4:	W[I1++] = R2.L;
	R0 = I1;
	R1 = B1;
	R0 = R0 - R1;
	R0 >>= 1;
	P2 = I0;
	[P2] = R0;
	L1 = 0;
	RTS;

/* one-pole in place, 32-bit saturating (W32): A = s + c b[0]; per i: A -= c y[i-1].H (y[-1] = s), y[i] = A, b[i] = y.H,
 * A += c b[i+1] (not after the last); returns the new state y[63]
 *   int32_t eq_onepole(int16_t *b, int32_t c, int32_t s)    (c in the low half) */
	.global OP_NAME
	.align 4
OP_NAME:
	I0 = R0;			/* b */
	R3 = R2;			/* y[-1] = s */
	A0 = R2;
	R0.L = W[I0];
	A0 += R1.L * R0.L (W32);
	P1 = 63;
	LSETUP (5f, 6f) LC0 = P1;
5:	A0 -= R1.L * R3.H (W32);
	R3 = A0.W;
	W[I0++] = R3.H;
	R0.L = W[I0];
6:	A0 += R1.L * R0.L (W32);
	A0 -= R1.L * R3.H (W32);
	R0 = A0.W;
	W[I0] = R0.H;
	RTS;

/* mix: x[i] = (sat(sat(lo a + mid b) + hi c) << 1).H with each product 2 band g << 1 (saturating), the meters
 * [0xFF8017D4 / D8 / DC] = max of each band's term
 *   void eq_mix(int16_t *x, int32_t ab, int32_t c)    a = ab.L, b = ab.H, c = c.L (the squared gains) */
	.global MIX_NAME
	.align 4
MIX_NAME:
	[--SP] = (R7:4);
	I0 = R0;			/* x (out) */
	P1.L = 0xBB28; P1.H = 0x01AA;	/* LO */
	I1 = P1;
	P1.L = 0xBC28; P1.H = 0x01AA;	/* MID */
	I2 = P1;
	P1.L = 0xBBA8; P1.H = 0x01AA;	/* HI */
	I3 = P1;
	P1.L = 0x17D4; P1.H = 0xFF80;	/* meters */
	R4 = [P1];
	R5 = [P1 + 4];
	R6 = [P1 + 8];
	P2 = 64;
	LSETUP (7f, 8f) LC0 = P2;
7:	R0.L = W[I1++];
	R0 = R0.L * R1.L;
	R0 = R0 << 1 (S);
	R4 = MAX (R4, R0);
	R3.L = W[I2++];
	R3 = R3.L * R1.H;
	R3 = R3 << 1 (S);
	R5 = MAX (R5, R3);
	R0 = R0 + R3 (S);
	R3.L = W[I3++];
	R3 = R3.L * R2.L;
	R3 = R3 << 1 (S);
	R6 = MAX (R6, R3);
	R0 = R3 + R0 (S);
	R0 = R0 << 1 (S);
8:	W[I0++] = R0.H;
	[P1] = R4;
	[P1 + 4] = R5;
	[P1 + 8] = R6;
	(R7:4) = [SP++];
	RTS;
