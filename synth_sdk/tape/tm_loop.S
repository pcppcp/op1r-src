/* Free tape machine: the fast-path inner loop of tm_track_block (tape/tm.c, docs/82), bit-exact with its C version
 * (OP1R_TMCHECK=1 compares both per block). 64 samples, 4-point Hermite (Catmull-Rom) from one chunk, mixed into
 * the accumulators:
 *   n = r >> 16, f = (r & 0xFFFF) >> 4
 *   c1 = (s1 - sm) >> 1, c2 = sm - (5 s0 >> 1) + 2 s1 - (s2 >> 1), c3 = ((s2 - sm) >> 1) + (3 (s0 - s1) >> 1)
 *   v = sat16((((c3 f >> 12) + c2) f >> 12 + c1) f >> 12 + s0); accL += v gl >> shift, accR += v gr >> shift; r += st
 * void tm_hloop(const int16_t *d, int32_t r, int32_t st, uint32_t gains (gl low, gr high), int32_t shift,
 *               int32_t *accL, int32_t *accR, int16_t *out (v per sample, pre-fader: meters, track outs)) */
	.text
	.align 4
	.global _tm_hloop
	.type _tm_hloop, STT_FUNC
_tm_hloop:
	[--SP] = (R7:4, P5:3);                      /* 7 words: the stack args move up by 28 */
	P0 = R0;                                    /* d */
	R7 = R1;                                    /* r (Q16) */
	M0 = R2;                                    /* st */
	R0 = [SP + 40]; I2 = R0;                    /* gains */
	R0 = [SP + 44]; I3 = R0;                    /* shift */
	P1 = [SP + 48];                             /* accL */
	P2 = [SP + 52];                             /* accR */
	R0 = [SP + 56]; I0 = R0;                    /* out */
	P4 = 64;
	LSETUP (1f, 2f) LC0 = P4;
1:	R0 = R7 >>> 16;
	P3 = R0;
	R6 = R7.L (Z);
	P3 = P0 + (P3 << 1);
	R6 >>= 4;                                   /* f, Q12 */
	R1 = W[P3 + -2] (X);                        /* sm */
	R2 = W[P3] (X);                             /* s0 */
	R3 = W[P3 + 2] (X);                         /* s1 */
	R4 = W[P3 + 4] (X);                         /* s2 */
	R0 = R2 - R3;                               /* c3 = ((s2 - sm) >> 1) + (3 (s0 - s1) >> 1) */
	R5 = R0 + R0;
	R0 = R0 + R5;
	R0 >>>= 1;
	R5 = R4 - R1;
	R5 >>>= 1;
	R5 = R5 + R0;
	R0 = R2 << 2;                               /* c2 = sm - (5 s0 >> 1) + 2 s1 - (s2 >> 1) */
	R0 = R0 + R2;
	R0 >>>= 1;
	R0 = R1 - R0;
	R0 = R0 + R3;
	R0 = R0 + R3;
	R4 >>>= 1;
	R0 = R0 - R4;
	R4 = R3 - R1;                               /* c1 = (s1 - sm) >> 1 */
	R4 >>>= 1;
	R5 *= R6;                                   /* Horner */
	R5 >>>= 12;
	R5 = R5 + R0;
	R5 *= R6;
	R5 >>>= 12;
	R5 = R5 + R4;
	R5 *= R6;
	R5 >>>= 12;
	R5 = R5 + R2;
	R5 = R5 << 16 (S);                          /* sat16, v in the high half */
	W[I0++] = R5.H;
	R1 = I2;
	R3 = R5.H * R1.L (IS);                      /* v gl */
	R5 = R5.H * R1.H (IS);                      /* v gr */
	R1 = I3;
	R3 >>>= R1;
	R5 >>>= R1;
	R0 = [P1];
	R0 = R0 + R3;
	[P1++] = R0;
	R0 = [P2];
	R0 = R0 + R5;
	[P2++] = R0;
	R0 = M0;
2:	R7 = R7 + R0;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _tm_hloop, .-_tm_hloop
