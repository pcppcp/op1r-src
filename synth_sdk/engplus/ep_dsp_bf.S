/* engine+ stages (docs/90) in Blackfin assembly: the same results as ep_dsp.c's C loops (EP_CHECK builds compare
 * them per block), without the compiler's register shuffles and stalls. Per-block parameters come from the C code.
 *   void ep_drive_bf(int16_t *buf, int n, int G)
 *   void ep_filter_bf(int32_t y[4], int16_t *buf, int n, int32_t ak, int32_t g4)   ak = a16 | k4 << 16
 *   void ep_crush_bf(ep_voice *v, int16_t *buf, int n, int32_t mask, int32_t hold)
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
	.text

/* drive: u = rnd(sat(x * G << 4)), y = rnd(u32 + (sat(u32 - u * rnd(u * u)) >>> 1)), u32 = u << 16; two samples
 * per pass (both multipliers, buf 4-aligned, n even): R1 / R6 hold {u1 : u0} / {u1^2 : u0^2} */
	.global _ep_drive_bf
	.align 4
_ep_drive_bf:
	[--SP] = (R7:4);
	P0 = R0;
	P1 = R1;
	P1 = P1 >> 1;
	R3.L = 0;
	R3.H = -1;			/* 0xFFFF0000 */
	LSETUP(.Ld0, .Ld1) LC0 = P1;
.Ld0:	R0 = [P0];
	R5 = R0.H * R2.L, R4 = R0.L * R2.L;
	R4 = R4 << 4 (S);
	R5 = R5 << 4 (S);
	R1.L = R4 (RND);
	R1.H = R5 (RND);
	R5 = R1.H * R1.H, R4 = R1.L * R1.L;
	R7 = R1 & R3;			/* u1 << 16 */
	R6.L = R4 (RND);
	R6.H = R5 (RND);
	R5 = R1.H * R6.H, R4 = R1.L * R6.L;
	R6 = R1 << 16;			/* u0 << 16 */
	R4 = R6 - R4 (S);
	R5 = R7 - R5 (S);
	R4 >>>= 1;
	R5 >>>= 1;
	R4 = R6 + R4 (S);
	R5 = R7 + R5 (S);
	R0.L = R4 (RND);
	R0.H = R5 (RND);
.Ld1:	[P0++] = R0;
	(R7:4) = [SP++];
	RTS;
	.size _ep_drive_bf, .-_ep_drive_bf

/* ladder: in = sat(x << 15 - sat(k4 * rnd(y4) << 2)); y_i = sat(y_i + a16 * rnd(sat(prev - y_i))), i = 1..4;
 * out = rnd(sat(rnd(y4) * g4 << 3)) */
	.global _ep_filter_bf
	.align 4
_ep_filter_bf:
	[--SP] = (R7:4);
	R3 = [SP + 28];			/* ak (12 + 16) */
	P1 = R2;
	R2 = [SP + 32];			/* g4 */
	P2 = R0;
	P0 = R1;
	R4 = [P2];
	R5 = [P2 + 4];
	R6 = [P2 + 8];
	R7 = [P2 + 12];
	LSETUP(.Lf0, .Lf1) LC0 = P1;
.Lf0:	R1.L = R7 (RND);
	R0 = W[P0] (X);
	A0 = R4;
	R1 = R3.H * R1.L;
	R0 <<= 15;
	A1 = R5;
	R1 = R1 << 2 (S);
	R0 = R0 - R1 (S);
	R1 = R0 - R4 (S);
	R1.L = R1 (RND);
	R4 = (A0 += R3.L * R1.L);	/* y1 = sat(y1 + a * rnd(in - y1)) */
	A0 = R6;
	R1 = R4 - R5 (S);
	R1.L = R1 (RND);
	R5 = (A1 += R3.L * R1.L);
	A1 = R7;
	R1 = R5 - R6 (S);
	R1.L = R1 (RND);
	R6 = (A0 += R3.L * R1.L);
	R1 = R6 - R7 (S);
	R1.L = R1 (RND);
	R7 = (A1 += R3.L * R1.L);
	R0.L = R7 (RND);
	R0 = R0.L * R2.L;
	R0 = R0 << 3 (S);
	R0.L = R0 (RND);
.Lf1:	W[P0++] = R0;
	[P2] = R4;
	[P2 + 4] = R5;
	[P2 + 8] = R6;
	[P2 + 12] = R7;
	(R7:4) = [SP++];
	RTS;
	.size _ep_filter_bf, .-_ep_filter_bf

/* crush: when the count is 0, hold = x & mask and count = hold; count -= 1; out = hold. ep_voice: hold int16 at 16,
 * count uint8 at 18 */
	.global _ep_crush_bf
	.align 4
_ep_crush_bf:
	[--SP] = (R7:4);
	R4 = [SP + 28];			/* mask */
	R5 = [SP + 32];			/* hold */
	P2 = R0;
	P0 = R1;
	P1 = R2;
	R3 = W[P2 + 16] (X);
	R1 = B[P2 + 18] (Z);
	LSETUP(.Lc0, .Lc1) LC0 = P1;
.Lc0:	R0 = W[P0] (X);
	CC = R1 == 0;
	R0 = R0 & R4;
	IF CC R3 = R0;
	IF CC R1 = R5;
	R1 += -1;
.Lc1:	W[P0++] = R3;
	W[P2 + 16] = R3;
	B[P2 + 18] = R1;
	(R7:4) = [SP++];
	RTS;
	.size _ep_crush_bf, .-_ep_crush_bf
