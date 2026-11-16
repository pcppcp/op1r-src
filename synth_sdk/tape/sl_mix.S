/* ELSE-MARIE's fast mix (tape/tm.c, docs/86): four slots' 64 samples, each times its 16-bit gain (Q15), summed with
 * the MAC and added once per sample into a cached accumulator (an SDRAM store to a cold line costs ~23 cycles on the
 * device; the accumulator lines stay cached across the passes):
 *   acc[i] += (p0[i] g0 + p1[i] g1 + p2[i] g2 + p3[i] g3) >> 15        (40-bit A0, arithmetic shift)
 * void sl_mac4(int32_t *acc, const sl_m4 *m)   m: const int16_t *p[4]; uint32_t g01 (g0 low, g1 high), g23
 * A missing slot: a zero block with gain 0. Same result as sl_mac4_c (tape/tm.c, the host build). */
	.text
	.align 4
	.global _sl_mac4
	.type _sl_mac4, STT_FUNC
_sl_mac4:
	[--SP] = (R7:4);
	P0 = R0;                                    /* acc */
	P1 = R1;                                    /* m */
	R0 = [P1 + 0]; I0 = R0;
	R0 = [P1 + 4]; I1 = R0;
	R0 = [P1 + 8]; I2 = R0;
	R0 = [P1 + 12]; I3 = R0;
	R6 = [P1 + 16];                             /* g0 | g1 << 16 */
	R7 = [P1 + 20];                             /* g2 | g3 << 16 */
	P2 = 64;
	LSETUP (1f, 2f) LC0 = P2;
1:	R0.L = W[I0++];
	R0.H = W[I1++];
	R1.L = W[I2++];
	R1.H = W[I3++];
	A0 = R0.L * R6.L (IS);
	A0 += R0.H * R6.H (IS);
	A0 += R1.L * R7.L (IS);
	A0 += R1.H * R7.H (IS);
	A0 = A0 >>> 15;
	R2 = A0.W;
	R3 = [P0];
	R3 = R3 + R2;
2:	[P0++] = R3;
	(R7:4) = [SP++];
	RTS;
	.size _sl_mac4, .-_sl_mac4

/* eight centred slots (left gain = right gain), one pass: v = (sum p_k[i] g_k) >> 15; add: accL[i] += v, accR[i] += v,
 * else accL[i] = accR[i] = v (the first pass: no zeroing, no copy). Same result as sl_macc8_c (tape/tm.c).
 * void sl_macc8(int32_t *accL, int32_t *accR, const sl_m8 *m)   m: const int16_t *p[8]; int16_t g[8]; int32_t add */
	.align 4
	.global _sl_macc8
	.type _sl_macc8, STT_FUNC
_sl_macc8:
	[--SP] = (R7:4, P5:3);
	P0 = R0;                                    /* accL */
	P1 = R1;                                    /* accR */
	P2 = R2;                                    /* m */
	R0 = [P2 + 0]; I0 = R0;
	R0 = [P2 + 4]; I1 = R0;
	R0 = [P2 + 8]; I2 = R0;
	R0 = [P2 + 12]; I3 = R0;
	P3 = [P2 + 20];
	P4 = [P2 + 24];
	P5 = [P2 + 28];
	R4 = [P2 + 32];                             /* g0 | g1 << 16 */
	R5 = [P2 + 36];
	R6 = [P2 + 40];
	R7 = [P2 + 44];
	R0 = [P2 + 48];                             /* add */
	P2 = [P2 + 16];                             /* (last: p4) */
	CC = R0 == 0;
	R0 = 64;
	LC0 = R0;
	IF CC JUMP 5f;
	LSETUP (1f, 2f) LC0;
1:	R0.L = W[I0++];
	R0.H = W[I1++];
	R1.L = W[I2++];
	R1.H = W[I3++];
	A0 = R0.L * R4.L (IS);
	A0 += R0.H * R4.H (IS);
	A0 += R1.L * R5.L (IS);
	A0 += R1.H * R5.H (IS);
	R2 = W[P2++] (X);                           /* (half-register loads need an I register: full loads here) */
	A0 += R2.L * R6.L (IS);
	R2 = W[P3++] (X);
	A0 += R2.L * R6.H (IS);
	R3 = W[P4++] (X);
	A0 += R3.L * R7.L (IS);
	R3 = W[P5++] (X);
	A0 += R3.L * R7.H (IS);
	A0 = A0 >>> 15;
	R0 = A0.W;
	R1 = [P0];
	R1 = R1 + R0;
	[P0++] = R1;
	R1 = [P1];
	R1 = R1 + R0;
2:	[P1++] = R1;
	JUMP 6f;
5:	LSETUP (3f, 4f) LC0;
3:	R0.L = W[I0++];
	R0.H = W[I1++];
	R1.L = W[I2++];
	R1.H = W[I3++];
	A0 = R0.L * R4.L (IS);
	A0 += R0.H * R4.H (IS);
	A0 += R1.L * R5.L (IS);
	A0 += R1.H * R5.H (IS);
	R2 = W[P2++] (X);                           /* (half-register loads need an I register: full loads here) */
	A0 += R2.L * R6.L (IS);
	R2 = W[P3++] (X);
	A0 += R2.L * R6.H (IS);
	R3 = W[P4++] (X);
	A0 += R3.L * R7.L (IS);
	R3 = W[P5++] (X);
	A0 += R3.L * R7.H (IS);
	A0 = A0 >>> 15;
	R0 = A0.W;
	[P0++] = R0;
4:	[P1++] = R0;
6:	(R7:4, P5:3) = [SP++];
	RTS;
	.size _sl_macc8, .-_sl_macc8
