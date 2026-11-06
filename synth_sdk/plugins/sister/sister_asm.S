/* SISTER's per-sample kernels in Blackfin assembly. Results equal sister_kern.c sample for sample (the
 * semantics are in sister_kern.h); check.py's exactness test compares both on bcore.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
 *
 * Calling convention (bfin-elf-gcc): arguments in R0..R2, then the stack (12 bytes reserved by the caller);
 * R4..R7 / P3..P5 are saved here; L0..L3 are left at 0 (the compiler assumes linear addressing). */
#if defined(__bfin__) && !defined(SI_NO_ASM)
	.text

/* ---- void si_k_line(si_line_t *p) ---- */
#ifndef SI_C_LINE
	.align 4
	.global _si_k_line
	.type _si_k_line, STT_FUNC
_si_k_line:
	[--SP] = (R7:4, P5:3);
	P0 = R0;
	R0 = [P0 + 0]; B0 = R0;
	R0 = [P0 + 4]; L0 = R0;
	R0 = [P0 + 8]; I0 = R0;
	R0 = [P0 + 12]; B1 = R0; B2 = R0;
	R0 = 1024 (X); L1 = R0; L2 = R0;
	R0 = [P0 + 16]; I1 = R0;
	R0 = [P0 + 20]; I2 = R0;
	P1 = [P0 + 24];
	R4 = [P0 + 28];
	R3 = [P0 + 32];
	R7 = [P0 + 36];
	R6 = [P0 + 40];
	P3 = 16;
	LSETUP (.Lline_s, .Lline_e) LC0 = P3;
.Lline_s:
	/* sample A: a -> R0, b in R7 */
	A0 = R6.L * R4.L || R0.L = W[I0++] || R1.L = W[I1++];
	A0 += R0.L * R4.H;
	R6.L = (A0 += R7.L * R3.L) (T);
	R2.L = R1.L * R3.H (T);
	R2.L = R6.L - R2.L (S);
	R5.L = R2.L * R3.H (T) || W[I2++] = R2.L || NOP;
	R5.L = R1.L + R5.L (S);
	/* sample B: a -> R7, b in R0 */
	A0 = R6.L * R4.L || W[P1++] = R5 || R7.L = W[I0++];
	R1.L = W[I1++];
	A0 += R7.L * R4.H;
	R6.L = (A0 += R0.L * R3.L) (T);
	R2.L = R1.L * R3.H (T);
	R2.L = R6.L - R2.L (S);
	R5.L = R2.L * R3.H (T) || W[I2++] = R2.L || NOP;
	R5.L = R1.L + R5.L (S);
.Lline_e:
	W[P1++] = R5;
	R6 = R6.L (X);
	[P0 + 40] = R6;
	R7 = R7.L (X);
	[P0 + 36] = R7;
	R0 = 0;
	L0 = R0; L1 = R0; L2 = R0;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _si_k_line, .-_si_k_line
#endif

/* ---- void si_k_res(si_res_t *p) ---- */
#ifndef SI_C_RES
	.align 4
	.global _si_k_res
	.type _si_k_res, STT_FUNC
_si_k_res:
	[--SP] = (R7:4, P5:3);
	P0 = R0;
	P1 = [P0 + 0];
	P2 = [P0 + 4];
	R4 = [P0 + 8];
	R5 = [P0 + 12];
	R6 = [P0 + 16];
	R7 = 0xFFFF (Z);
	P4 = P2;
	P3 = 32;
	R0 = W[P1++] (X);
	LSETUP (.Lres_s, .Lres_e) LC0 = P3;
.Lres_s:
	A0 = R0.L * R4.L || R1 = W[P2++] (X) || NOP;
	A0 += R6.H * R4.H || R0 = W[P1++] (X) || NOP;
	A0 += R6.H * R4.H;
	R2 = (A0 -= R6.L * R5.L);
	R3 = R2 >>> 31;
	R3 = R3 & R7;
	R2 = R2 + R3;
	R2 >>>= 16;
	R6 = PACK(R2.L, R6.H);
	R1.L = R1.L + R2.L (S);
.Lres_e:
	W[P4++] = R1;
	[P0 + 16] = R6;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _si_k_res, .-_si_k_res
#endif

/* ---- void si_k_comb(si_comb_t *p) ---- */
#ifndef SI_C_COMB
	.align 4
	.global _si_k_comb
	.type _si_k_comb, STT_FUNC
_si_k_comb:
	[--SP] = (R7:4, P5:3);
	P0 = R0;
	R0 = [P0 + 0]; B0 = R0; B1 = R0;
	R0 = [P0 + 4]; L0 = R0; L1 = R0;
	R0 = [P0 + 8]; I0 = R0;
	R0 = [P0 + 12]; I1 = R0;
	P1 = [P0 + 16];
	P2 = [P0 + 20];
	R4 = [P0 + 24];
	R5 = [P0 + 28];
	R6 = [P0 + 32];
	R7 = [P0 + 36];
	R2 = [P0 + 40];
	R3 = 0;
	R3.L = 0x8000;                /* the damping one-pole's half-up bias */
	P3 = 16;
	LSETUP (.Lcomb_s, .Lcomb_e) LC0 = P3;
.Lcomb_s:
	/* sample A: a -> R0, b in R7 (R7 is the toward-zero temp once z is made) */
	A0 = R3 || R0.L = W[I0++] || NOP;
	A0 += R2.L * R4.L || R1 = W[P1++] (X) || NOP;
	A0 += R0.L * R4.H;
	R2.L = (A0 += R7.L * R5.L) (T);
	A1 = R1.L * R6.L;
	R1 = (A1 += R2.L * R5.H);
	R7 = R1 >>> 31;
	R7 >>= 16;
	R1 = R1 + R7;
	R1 >>>= 16;
	/* sample B: a -> R7, b in R0 (then R0 is the temp) */
	A0 = R3 || W[I1++] = R1.L || R7.L = W[I0++];
	W[P2++] = R1;
	A0 += R2.L * R4.L || R1 = W[P1++] (X) || NOP;
	A0 += R7.L * R4.H;
	R2.L = (A0 += R0.L * R5.L) (T);
	A1 = R1.L * R6.L;
	R1 = (A1 += R2.L * R5.H);
	R0 = R1 >>> 31;
	R0 >>= 16;
	R1 = R1 + R0;
	R1 >>>= 16;
	MNOP || W[I1++] = R1.L || NOP;
.Lcomb_e:
	W[P2++] = R1;
	R2 = R2.L (X);
	[P0 + 40] = R2;
	R7 = R7.L (X);
	[P0 + 36] = R7;
	R0 = 0;
	L0 = R0; L1 = R0;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _si_k_comb, .-_si_k_comb
#endif

/* ---- void si_k_mix(si_mix_t *p): stereo, 64 samples ---- */
#ifndef SI_C_MIX
	.align 4
	.global _si_k_mix
	.type _si_k_mix, STT_FUNC
_si_k_mix:
	[--SP] = (R7:4, P5:3);
	P0 = R0;
	R0 = [P0 + 0]; I0 = R0; P4 = R0;
	R0 = [P0 + 4]; I1 = R0; P5 = R0;
	R0 = [P0 + 8]; I2 = R0;
	R0 = [P0 + 12]; I3 = R0;
	R5 = [P0 + 16];
	R6 = [P0 + 20];
	P3 = 64;
	LSETUP (.Lmix_s, .Lmix_e) LC0 = P3;
.Lmix_s:
	R5 = R5 +|+ R6 || R0.L = W[I0++] || R0.H = W[I1++];
	MNOP || R3.L = W[I2++] || R3.H = W[I3++];
	A0 = R3.L * R5.L, A1 = R3.H * R5.L;
	R2.L = (A0 -= R0.L * R5.H), R2.H = (A1 -= R0.H * R5.H) (T);
	R2 = R2 +|+ R0 (S);
	R1 = R2 >>> 16 || W[P4++] = R2 || NOP;
.Lmix_e:
	W[P5++] = R1;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _si_k_mix, .-_si_k_mix
#endif

/* ---- void si_k_mixm(si_mix_t *p): mono ---- */
#ifndef SI_C_MIXM
	.align 4
	.global _si_k_mixm
	.type _si_k_mixm, STT_FUNC
_si_k_mixm:
	[--SP] = (R7:4, P5:3);
	P0 = R0;
	R0 = [P0 + 0]; I0 = R0; P4 = R0;
	R0 = [P0 + 8]; I2 = R0;
	R5 = [P0 + 16];
	R6 = [P0 + 20];
	P3 = 64;
	LSETUP (.Lmixm_s, .Lmixm_e) LC0 = P3;
.Lmixm_s:
	R5 = R5 +|+ R6 || R0.L = W[I0++] || R3.L = W[I2++];
	A0 = R3.L * R5.L;
	R2.L = (A0 -= R0.L * R5.H) (T);
	R2.L = R2.L + R0.L (S);
.Lmixm_e:
	W[P4++] = R2;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _si_k_mixm, .-_si_k_mixm
#endif

/* ---- void si_k_decim(const int16_t *c, int16_t *x, const int16_t *h) ----
 * c[-9 .. 10] are 10 aligned words: the left half read forward from c - 9, the right half backward from
 * c + 9; pairs share a coefficient. c[0] is the high half of the word at c - 1. */
#ifndef SI_C_DECIM
	.align 4
	.global _si_k_decim
	.type _si_k_decim, STT_FUNC
_si_k_decim:
	[--SP] = (R7:4, P5:3);
	P1 = R1;
	P2 = R2;
	R3 = 18;
	R0 = R0 - R3;
	I0 = R0;                      /* c - 9 */
	R3 = 36;
	R0 = R0 + R3;
	I1 = R0;                      /* c + 9 (word c[9], c[10]) */
	R4 = [P2 + 0];                /* h0 | h1 */
	R5 = [P2 + 4];                /* h2 | h3 */
	R6 = W[P2 + 8] (X);           /* h4 */
	R6.H = 0x4000;                /* centre 1/2 */
	M0 = -16;
	M1 = 24;
	P3 = 32;
	LSETUP (.Ldec_s, .Ldec_e) LC0 = P3;
.Ldec_s:
	MNOP || R0 = [I0++] || R1 = [I1--];
	A0 = R0.L * R6.L || R2 = [I0++] || R3 = [I1--];
	A0 += R1.L * R6.L || R0 = [I0++] || R1 = [I1--];
	A0 += R2.L * R5.H;
	A0 += R3.L * R5.H || R2 = [I0++] || R3 = [I1--];
	A0 += R0.L * R5.L;
	A0 += R1.L * R5.L || R0 = [I0++] || R1 = [I1--];
	A0 += R2.L * R4.H;
	A0 += R3.L * R4.H;
	A0 += R0.L * R4.L;
	A0 += R1.L * R4.L || I0 += M0 || NOP;
	R7.L = (A0 += R0.H * R6.H) (T) || I1 += M1 || NOP;
.Ldec_e:
	W[P1++] = R7;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _si_k_decim, .-_si_k_decim
#endif

/* ---- void si_k_interp(const int16_t *c, int16_t *out, const int16_t *h2) ---- */
#ifndef SI_C_INTERP
	.align 4
	.global _si_k_interp
	.type _si_k_interp, STT_FUNC
_si_k_interp:
	[--SP] = (R7:4, P5:3);
	P1 = R1;
	P2 = R2;
	R3 = 8;
	R1 = R0 - R3;
	I0 = R1;                      /* c - 4 */
	R3 = 10;
	R1 = R0 + R3;
	I1 = R1;                      /* c + 5 */
	R4 = [P2 + 0];
	R5 = [P2 + 4];
	R6 = W[P2 + 8] (X);
	M0 = -8;
	M1 = 12;
	P3 = 32;
	LSETUP (.Lint_s, .Lint_e) LC0 = P3;
.Lint_s:
	MNOP || R0.L = W[I0++] || R1.L = W[I1--];
	A0 = R0.L * R6.L || R2.L = W[I0++] || R3.L = W[I1--];
	A0 += R1.L * R6.L || R0.L = W[I0++] || R1.L = W[I1--];
	A0 += R2.L * R5.H;
	A0 += R3.L * R5.H || R2.L = W[I0++] || R3.L = W[I1--];
	A0 += R0.L * R5.L;
	A0 += R1.L * R5.L || R0.L = W[I0++] || R1.L = W[I1--];
	A0 += R2.L * R4.H;
	A0 += R3.L * R4.H || W[P1++] = R0 || NOP;
	A0 += R0.L * R4.L || I0 += M0 || NOP;
	R7.L = (A0 += R1.L * R4.L) (T) || I1 += M1 || NOP;
.Lint_e:
	W[P1++] = R7;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _si_k_interp, .-_si_k_interp
#endif

/* ---- void si_k_ap(int16_t *ring, uint32_t wr, uint32_t rd, int32_t g, int16_t *x) ---- */
#ifndef SI_C_AP
	.align 4
	.global _si_k_ap
	.type _si_k_ap, STT_FUNC
_si_k_ap:
	[--SP] = (R7:4, P5:3);
	B0 = R0; B1 = R0;
	R3 = 1024 (X);
	L0 = R3; L1 = R3;
	R1 = R1 << 1;
	R1 = R0 + R1;
	I0 = R1;                      /* write */
	R2 = R2 << 1;
	R2 = R0 + R2;
	I1 = R2;                      /* read */
	R7 = [SP + 40];               /* g */
	P1 = [SP + 44];               /* x */
	P2 = P1;
	P3 = 32;
	LSETUP (.Lap_s, .Lap_e) LC0 = P3;
.Lap_s:
	MNOP || R0.L = W[I1++] || R1 = W[P1++] (X);
	R2.L = R0.L * R7.L (T);
	R2.L = R1.L - R2.L (S);
	R3.L = R2.L * R7.L (T) || W[I0++] = R2.L || NOP;
	R3.L = R0.L + R3.L (S);
.Lap_e:
	W[P2++] = R3;
	R0 = 0;
	L0 = R0; L1 = R0;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _si_k_ap, .-_si_k_ap
#endif
#ifndef SI_C_SCAT
/* ---- void si_k_scatter(si_scat_t *p) ---- */
	.align 4
	.global _si_k_scatter
	.type _si_k_scatter, STT_FUNC
_si_k_scatter:
	[--SP] = (R7:4, P5:3);
	P0 = R0;
	R0 = [P0 + 32]; B0 = R0;
	R0 = [P0 + 36]; B1 = R0;
	R0 = [P0 + 40]; B2 = R0;
	R0 = [P0 + 44]; B3 = R0;
	R0 = [P0 + 48]; L0 = R0;
	R0 = [P0 + 52]; L1 = R0;
	R0 = [P0 + 56]; L2 = R0;
	R0 = [P0 + 60]; L3 = R0;
	R0 = [P0 + 16]; I0 = R0;
	R0 = [P0 + 20]; I1 = R0;
	R0 = [P0 + 24]; I2 = R0;
	R0 = [P0 + 28]; I3 = R0;
	P1 = [P0 + 0];
	P2 = [P0 + 4];
	P3 = [P0 + 8];
	P4 = [P0 + 12];
	P5 = 32;
	LSETUP (.Lsc_s, .Lsc_e) LC0 = P5;
.Lsc_s:
	R0 = W[P1] (X);
	R1 = W[P1 + 64] (X);
	R2 = W[P1 + 128] (X);
	R3 = W[P1 + 192] (X);
	P1 += 2;
	R4 = R0 + R2, R5 = R0 - R2;
	R6 = R1 + R3, R7 = R1 - R3;
	R0 = R4 >>> 1;
	R1 = R6 >>> 1;
	W[P3++] = R0;
	W[P4++] = R1;
	R0 = R4 + R6, R1 = R4 - R6;
	R2 = R5 + R7, R3 = R5 - R7;
	R0 >>>= 2;
	R1 >>>= 2;
	R2 >>>= 2;
	R3 >>>= 2;
	R4 = [P0 + 64];
	R5 = [P0 + 68];
	R0 = R0.L * R4.L (IS);
	R1 = R1.L * R4.H (IS);
	R2 = R2.L * R5.L (IS);
	R3 = R3.L * R5.H (IS);
	R6 = R0 >>> 31;
	R7 = R1 >>> 31;
	R6 >>= 18;
	R7 >>= 18;
	R0 = R0 + R6;
	R1 = R1 + R7;
	R6 = R2 >>> 31;
	R7 = R3 >>> 31;
	R6 >>= 18;
	R7 >>= 18;
	R2 = R2 + R6;
	R3 = R3 + R7;
	R0 >>>= 14;
	R1 >>>= 14;
	R2 >>>= 14;
	R3 >>>= 14;
	R6 = W[P2++] (X);
	R6 += 2;
	R6 >>>= 2;
	R0 = R0 + R6;
	R1 = R1 - R6;
	R2 = R2 + R6;
	R3 = R3 - R6;
	R0 = R0 << 16 (S);
	R1 = R1 << 16 (S) || W[I0++] = R0.H || NOP;
	R2 = R2 << 16 (S) || W[I1++] = R1.H || NOP;
	R3 = R3 << 16 (S) || W[I2++] = R2.H || NOP;
.Lsc_e:
	W[I3++] = R3.H;
	R0 = 0;
	L0 = R0; L1 = R0; L2 = R0; L3 = R0;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _si_k_scatter, .-_si_k_scatter
#endif
#endif
