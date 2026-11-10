/* QUAD's inner loops for the Blackfin: the same arithmetic as rv_quad.c's qk_*_c references, bit for bit
 * (quad_kern_check.py compares them on bcore), in fewer packets. 64 samples a call (QD_N) unless noted. The
 * argument blocks are rv_quad.h's qk_*_t (4-byte fields at the offsets given there).
 *
 * Common pieces:
 *   lerp(a, b, f) = (a (256 - f) + b f) >> 8 as one accumulator sum, (256 - f | f) packed in one register;
 *   mulf(a, b)    = a.L * b.L (T), a fractional product truncated (the host's (a b) >> 15, -1 x -1 saturating);
 *   adds / subs   = 16-bit saturating add / subtract (S);
 *   mz(a, b)      = (a b + bias) >>> 15, bias = 32767 for a negative product (rounding toward zero);
 *   pole(z, v, k) = v + ((((z - v) >>> 1) k) >>> 14).
 * Uses R0..R7, P0..P5, I0..I2 (L0..L2 stay 0), A0, LC0.
 *
 *   void qk_biquad(int32_t *st, const int16_t *c, int16_t *x)   DF1 in place: the five products in A0, + carried
 *        error, e = the low 13 bits, y = sat(acc >>> 13); x1 x2 / y1 y2 packed in halves. 15 packets per sample.
 *   void qk_lerp(const int16_t *p, int16_t *out, int32_t f)     out[n] = lerp(p[n + 1], p[n], f). 4 packets / sample.
 *   void qk_tap(const qk_tap_t *t)        a moving tap from base: D = d >>> 16, f = (d >> 8) & 255. 14 / sample.
 *   void qk_xfade(const qk_xf_t *t)       o = (o (32767 - tri) + b tri) >>> 15, tri from q. 11 / sample.
 *   void qk_apslot(const qk_aps_t *t)     input allpass in place over m samples. 8 / sample.
 *   void qk_aplerp(const qk_apl_t *t)     tank allpass with the cross feedback. 19 / sample.
 *   int32_t qk_damp(const qk_dmp_t *t)    lerp, pole, mz; returns z. 16 / sample.
 *   int32_t qk_dlyw(const qk_dw_t *t)     pole, mz, adds; returns z. 13 / sample.
 *   void qk_acc(int32_t *a, const int16_t *v, int32_t neg)      a +/-= v. 4 / sample.
 *   void qk_mac(const qk_mac_t *t)        a += (g v) >>> sh. 6 / sample.
 *   void qk_gadd(int16_t *o, const int16_t *x, int32_t g)       o = adds(o, mulf(g, x)). 5 / sample.
 *   int32_t qk_rvout(const qk_ro_t *t)    o = adds(o, mulf(mulf(sat((a sc) >>> sh), g), lev)), g += gs. 11 / sample.
 * Host builds assemble it empty (the C references run there).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

/* mz(Rs.L, Rk.L) into Rd (Rt scratch) */
.macro MZ Rd, Rs, Rk, Rt
	\Rd = \Rs\().L * \Rk\().L (IS);
	\Rt = \Rd >>> 31;
	\Rt = \Rt >> 17;
	\Rd = \Rd + \Rt;
	\Rd >>>= 15;
.endm

	.align 4
	.global _qk_biquad
	.type _qk_biquad, STT_FUNC
_qk_biquad:
	[--SP] = (R7:4, P5:3);
	P5 = R0;                             /* st */
	P1 = R1;                             /* c */
	I0 = R2;
	P0 = R2;
	L0 = 0;
	R0 = W[P1 + 0] (X);
	R1 = W[P1 + 2] (X);
	R5 = PACK(R1.L, R0.L);               /* b1 | b0 */
	R0 = W[P1 + 4] (X);
	R1 = W[P1 + 6] (X);
	R1 = -R1;
	R6 = PACK(R1.L, R0.L);               /* -a1 | b2 */
	R0 = W[P1 + 8] (X);
	R7 = -R0;                            /* -a2 */
	R0 = [P5 + 0];
	R1 = [P5 + 4];
	R1 = PACK(R1.L, R0.L);               /* x2 | x1 */
	R0 = [P5 + 8];
	R2 = [P5 + 12];
	R2 = PACK(R2.L, R0.L);               /* y2 | y1 */
	R3 = [P5 + 16];                      /* e */
	P2 = 64;
	R0.L = W[I0++];
	LSETUP (.Lbq_b, .Lbq_e) LC0 = P2;
.Lbq_b:
	A0 = R0.L * R5.L (IS);
	A0 += R1.L * R5.H (IS);
	A0 += R1.H * R6.L (IS);
	A0 += R2.L * R6.H (IS);
	R4 = (A0 += R2.H * R7.L) (IS);
	R4 = R4 + R3;
	R3 = R4 << 19;
	R3 = R3 >> 19;                       /* e: the low 13 bits */
	R4 >>>= 13;
	R4 = R4 << 16 (S);
	R1 = PACK(R1.L, R0.L) || R0.L = W[I0++];   /* x2 = x1, x1 = x0; the next x0 (one past the end: a read) */
	R4 = R4 >>> 16;
	R2 = PACK(R2.L, R4.L);
.Lbq_e:
	W[P0++] = R4;
	R0 = R1.L (X);
	[P5 + 0] = R0;
	R0 = R1 >>> 16;
	[P5 + 4] = R0;
	R0 = R2.L (X);
	[P5 + 8] = R0;
	R0 = R2 >>> 16;
	[P5 + 12] = R0;
	[P5 + 16] = R3;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _qk_biquad, .-_qk_biquad

	.align 4
	.global _qk_lerp
	.type _qk_lerp, STT_FUNC
_qk_lerp:
	[--SP] = (R7:4);
	I0 = R0;
	L0 = 0;
	P0 = R1;
	R7 = 256 (X);
	R7 = R7 - R2;
	R5 = PACK(R2.L, R7.L);               /* f | 256 - f */
	R1.L = W[I0++];                      /* p[0] */
	R0.L = W[I0++];                      /* p[1] */
	P2 = 32;
	LSETUP (.Llp_b, .Llp_e) LC0 = P2;
.Llp_b:                                  /* two samples: the newer / older sample alternate between R0 and R1 */
	A0 = R0.L * R5.L (IS);
	R2 = (A0 += R1.L * R5.H) (IS) || R1.L = W[I0++];
	R2 >>>= 8;
	W[P0++] = R2;
	A0 = R1.L * R5.L (IS);
	R2 = (A0 += R0.L * R5.H) (IS) || R0.L = W[I0++];
	R2 >>>= 8;
.Llp_e:
	W[P0++] = R2;
	(R7:4) = [SP++];
	RTS;
	.size _qk_lerp, .-_qk_lerp

	.align 4
	.global _qk_tap
	.type _qk_tap, STT_FUNC
_qk_tap:
	[--SP] = (R7:4);
	P2 = R0;
	R0 = [P2 + 0];
	I1 = R0;                             /* base, + 2 bytes a sample */
	R6 = [P2 + 4];                       /* d */
	R7 = [P2 + 8];                       /* sd */
	R0 = [P2 + 12];
	I2 = R0;                             /* out */
	L0 = 0;
	L1 = 0;
	L2 = 0;
	R5.L = 0x0808;                       /* EXTRACT pattern: bits 8 .. 15 */
	R5.H = 256;
	P2 = 64;
	LSETUP (.Ltp_b, .Ltp_e) LC0 = P2;
.Ltp_b:
	R0 = R6 >>> 15;
	BITCLR (R0, 0);                      /* 2 D bytes */
	M0 = R0;
	I0 = I1;
	I0 -= M0;
	R3 = EXTRACT(R6, R5.L) (Z) || R1.L = W[I0--];   /* f; a[0] */
	R4.L = R5.H - R3.L (NS) || R2.L = W[I0];        /* 256 - f; a[-1] */
	A1 = R1.L * R4.L (IS);
	R1 = (A1 += R2.L * R3.L) (IS);
	R1 >>>= 8;
	R6 = R6 + R7 (NS) || W[I2++] = R1.L;
.Ltp_e:
	I1 += 2;
	(R7:4) = [SP++];
	RTS;
	.size _qk_tap, .-_qk_tap

	.align 4
	.global _qk_xfade
	.type _qk_xfade, STT_FUNC
_qk_xfade:
	[--SP] = (R7:4);
	P2 = R0;
	P0 = [P2 + 0];                       /* o */
	P1 = [P2 + 4];                       /* b */
	R6 = [P2 + 8];                       /* q */
	R7 = [P2 + 12];                      /* inc */
	R5 = 32767 (X);
	P2 = 64;
	LSETUP (.Lxf_b, .Lxf_e) LC0 = P2;
.Lxf_b:
	R0 = R6 >>> 31;
	R0 = R0 ^ R6;
	R0 >>= 16;                           /* tri */
	R1 = R5 - R0;
	R2 = W[P0] (X);
	R3 = W[P1++] (X);
	A0 = R2.L * R1.L (IS);
	R2 = (A0 += R3.L * R0.L) (IS);
	R2 >>>= 15;
	R6 = R6 + R7;
.Lxf_e:
	W[P0++] = R2;
	(R7:4) = [SP++];
	RTS;
	.size _qk_xfade, .-_qk_xfade

	.align 4
	.global _qk_apslot
	.type _qk_apslot, STT_FUNC
_qk_apslot:
	[--SP] = (R7:4);
	P2 = R0;
	R0 = [P2 + 0];
	I0 = R0;                             /* p, read one ahead */
	I1 = R0;                             /* p, written */
	R0 = [P2 + 4];
	I2 = R0;                             /* x, read */
	I3 = R0;                             /* x, written */
	L0 = 0;
	L1 = 0;
	L2 = 0;
	L3 = 0;
	R7 = [P2 + 12];                      /* g */
	P2 = [P2 + 8];                       /* m >= 1 */
	R0.L = W[I0++];                      /* d */
	LSETUP (.Las_b, .Las_e) LC0 = P2;
.Las_b:
	R2.L = R0.L * R7.L (T) || R1.L = W[I2++];
	R1.L = R1.L - R2.L (S);              /* v */
	R2.L = R1.L * R7.L (T) || W[I1++] = R1.L;
	R3.L = R0.L + R2.L (S) || R0.L = W[I0++];   /* y; the next d (one past the run's end: a read) */
.Las_e:
	W[I3++] = R3.L;
	(R7:4) = [SP++];
	RTS;
	.size _qk_apslot, .-_qk_apslot

	.align 4
	.global _qk_aplerp
	.type _qk_aplerp, STT_FUNC
_qk_aplerp:                              /* dec >= 0: mz's bias is the sign of fb (A0's fractional sum) */
	[--SP] = (R7:4, P5:3);
	P5 = R0;
	R0 = [P5 + 0];
	I0 = R0;                             /* p */
	R0 = [P5 + 8];
	I1 = R0;                             /* x */
	R0 = [P5 + 12];
	I2 = R0;                             /* fb */
	R0 = [P5 + 20];
	I3 = R0;                             /* v */
	L0 = 0;
	L1 = 0;
	L2 = 0;
	L3 = 0;
	R2 = [P5 + 4];                       /* f */
	R7 = 256 (X);
	R7 = R7 - R2;
	R5 = PACK(R2.L, R7.L);               /* f | 256 - f */
	R6 = [P5 + 16];                      /* dec */
	P1 = [P5 + 24];                      /* y */
	R7 = [P5 + 28];                      /* g */
	R7.H = -32767;
	R1.L = W[I0++];                      /* p[0] */
	R0.L = W[I0++];                      /* p[1] */
	P2 = 32;
	LSETUP (.Lal_b, .Lal_e) LC0 = P2;
.Lal_b:                                  /* two samples: the newer / older p alternate between R0 and R1 */
	A1 = R0.L * R5.L (IS) || R2.L = W[I2++] || R4.L = W[I1++];
	R3 = (A1 += R1.L * R5.H) (IS) || R1.L = W[I0++];
	R3 >>>= 8;                           /* d */
	A0 = R2.L * R6.L;
	R2 = R2 >>> 15 (V);
	R2.L = (A0 += R2.L * R7.H) (T);      /* mz(fb, dec) */
	R4.L = R4.L + R2.L (S);              /* in */
	R2.L = R3.L * R7.L (T);
	R4.L = R4.L + R2.L (S);              /* w */
	R2.L = R4.L * R7.L (T) || W[I3++] = R4.L;
	R3.L = R3.L - R2.L (S);
	W[P1++] = R3;
	A1 = R1.L * R5.L (IS) || R2.L = W[I2++] || R4.L = W[I1++];
	R3 = (A1 += R0.L * R5.H) (IS) || R0.L = W[I0++];
	R3 >>>= 8;
	A0 = R2.L * R6.L;
	R2 = R2 >>> 15 (V);
	R2.L = (A0 += R2.L * R7.H) (T);
	R4.L = R4.L + R2.L (S);
	R2.L = R3.L * R7.L (T);
	R4.L = R4.L + R2.L (S);
	R2.L = R4.L * R7.L (T) || W[I3++] = R4.L;
	R3.L = R3.L - R2.L (S);
.Lal_e:
	W[P1++] = R3;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _qk_aplerp, .-_qk_aplerp

	.align 4
	.global _qk_damp
	.type _qk_damp, STT_FUNC
_qk_damp:                                /* dec >= 0 */
	[--SP] = (R7:4);
	P2 = R0;
	R0 = [P2 + 0];
	I0 = R0;                             /* p */
	L0 = 0;
	R2 = [P2 + 4];                       /* f */
	R7 = 256 (X);
	R7 = R7 - R2;
	R5 = PACK(R2.L, R7.L);
	P0 = [P2 + 8];                       /* u */
	R4 = [P2 + 12];                      /* z */
	R6 = [P2 + 16];                      /* dk */
	R7 = [P2 + 20];                      /* dec */
	R7.H = -32767;
	R1.L = W[I0++];
	R0.L = W[I0++];
	P2 = 32;
	LSETUP (.Ldm_b, .Ldm_e) LC0 = P2;
.Ldm_b:
	A1 = R0.L * R5.L (IS);
	R3 = (A1 += R1.L * R5.H) (IS) || R1.L = W[I0++];
	R3 >>>= 8;                           /* v */
	R2 = R4 - R3;
	R2 >>>= 1;
	R2 = R2.L * R6.L (IS);
	R2 >>>= 14;
	R4 = R3 + R2;                        /* z */
	A0 = R4.L * R7.L;
	R2 = R4 >>> 15 (V);
	R2.L = (A0 += R2.L * R7.H) (T);
	W[P0++] = R2;
	A1 = R1.L * R5.L (IS);
	R3 = (A1 += R0.L * R5.H) (IS) || R0.L = W[I0++];
	R3 >>>= 8;
	R2 = R4 - R3;
	R2 >>>= 1;
	R2 = R2.L * R6.L (IS);
	R2 >>>= 14;
	R4 = R3 + R2;
	A0 = R4.L * R7.L;
	R2 = R4 >>> 15 (V);
	R2.L = (A0 += R2.L * R7.H) (T);
.Ldm_e:
	W[P0++] = R2;
	R0 = R4;
	(R7:4) = [SP++];
	RTS;
	.size _qk_damp, .-_qk_damp

	.align 4
	.global _qk_dlyw
	.type _qk_dlyw, STT_FUNC
_qk_dlyw:                                /* fb >= 0 */
	[--SP] = (R7:4, P5:3);
	P5 = R0;
	P0 = [P5 + 0];                       /* w */
	R0 = [P5 + 4];
	I1 = R0;                             /* x */
	L1 = 0;
	P3 = [P5 + 8];                       /* v */
	R4 = [P5 + 12];                      /* z */
	R6 = [P5 + 16];                      /* k */
	R7 = [P5 + 20];                      /* fb */
	R7.H = -32767;
	P2 = 64;
	LSETUP (.Ldw_b, .Ldw_e) LC0 = P2;
.Ldw_b:
	R3 = W[P3++] (X);                    /* v */
	R2 = R4 - R3;
	R2 >>>= 1;
	R2 = R2.L * R6.L (IS);
	R2 >>>= 14;
	R4 = R3 + R2;                        /* z */
	A0 = R4.L * R7.L;
	R2 = R4 >>> 15 (V);
	R2.L = (A0 += R2.L * R7.H) (T) || R0.L = W[I1++];
	R0.L = R0.L + R2.L (S);
.Ldw_e:
	W[P0++] = R0;
	R0 = R4;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _qk_dlyw, .-_qk_dlyw

	.align 4
	.global _qk_acc
	.type _qk_acc, STT_FUNC
_qk_acc:
	P0 = R0;
	P2 = R0;
	P1 = R1;
	CC = R2 == 0;
	R2 = 64;
	LC0 = R2;
	IF !CC JUMP .Lac_neg;
	LSETUP (.Lac_b, .Lac_e) LC0;
.Lac_b:
	R0 = [P0++];
	R1 = W[P1++] (X);
	R0 = R0 + R1;
.Lac_e:
	[P2++] = R0;
	RTS;
.Lac_neg:
	LSETUP (.Lan_b, .Lan_e) LC0;
.Lan_b:
	R0 = [P0++];
	R1 = W[P1++] (X);
	R0 = R0 - R1;
.Lan_e:
	[P2++] = R0;
	RTS;
	.size _qk_acc, .-_qk_acc

	.align 4
	.global _qk_mac
	.type _qk_mac, STT_FUNC
_qk_mac:
	[--SP] = (R7:4);
	P2 = R0;
	P0 = [P2 + 0];                       /* a */
	P1 = [P2 + 4];                       /* v */
	R7 = [P2 + 8];                       /* g */
	R6 = [P2 + 12];                      /* sh */
	P2 = 64;
	LSETUP (.Lmc_b, .Lmc_e) LC0 = P2;
.Lmc_b:
	R1 = W[P1++] (X);
	R1 = R1.L * R7.L (IS);
	R1 >>>= R6;
	R0 = [P0];
	R0 = R0 + R1;
.Lmc_e:
	[P0++] = R0;
	(R7:4) = [SP++];
	RTS;
	.size _qk_mac, .-_qk_mac

	.align 4
	.global _qk_gadd
	.type _qk_gadd, STT_FUNC
_qk_gadd:
	P0 = R0;
	P1 = R1;
	P2 = 64;
	LSETUP (.Lga_b, .Lga_e) LC0 = P2;
.Lga_b:
	R0 = W[P0] (X);
	R1 = W[P1++] (X);
	R1.L = R1.L * R2.L (T);
	R0.L = R0.L + R1.L (S);
.Lga_e:
	W[P0++] = R0;
	RTS;
	.size _qk_gadd, .-_qk_gadd

	.align 4
	.global _qk_rvout
	.type _qk_rvout, STT_FUNC
_qk_rvout:
	[--SP] = (R7:4, P5:3);
	P5 = R0;
	P0 = [P5 + 0];                       /* o */
	P1 = [P5 + 4];                       /* a */
	R5 = [P5 + 8];                       /* sc */
	R6 = [P5 + 12];                      /* sh */
	R4 = [P5 + 16];                      /* g */
	R3 = [P5 + 20];                      /* gs */
	R7 = [P5 + 24];                      /* lev */
	P2 = 64;
	LSETUP (.Lro_b, .Lro_e) LC0 = P2;
.Lro_b:
	R0 = [P1++];
	R0 *= R5;
	R0 >>>= R6;
	R0 = R0 << 16 (S);
	R0 = R0 >>> 16;
	R0.L = R0.L * R4.L (T);
	R0.L = R0.L * R7.L (T);
	R1 = W[P0] (X);
	R1.L = R1.L + R0.L (S);
	R4 = R4 + R3;
.Lro_e:
	W[P0++] = R1;
	R0 = R4;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _qk_rvout, .-_qk_rvout
	.align 4
	.global _qk_sum4
	.type _qk_sum4, STT_FUNC
_qk_sum4:
	[--SP] = (R7:4, P5:3);
	P5 = R0;
	R0 = [P5 + 0];
	I0 = R0;
	R0 = [P5 + 4];
	I1 = R0;
	R0 = [P5 + 8];
	I2 = R0;
	R0 = [P5 + 12];
	I3 = R0;
	L0 = 0;
	L1 = 0;
	L2 = 0;
	L3 = 0;
	R4 = [P5 + 32];                      /* g1 | g0 */
	R5 = [P5 + 36];                      /* g3 | g2 */
	P0 = [P5 + 48];
	P1 = P0;
	R6 = [P5 + 52];                      /* sh */
	R7 = [P5 + 56];                      /* add */
	P2 = 64;
	CC = R7 == 0;
	IF CC JUMP .Ls4_set;
	LSETUP (.Ls4a_b, .Ls4a_e) LC0 = P2;
.Ls4a_b:
	MNOP || R0.L = W[I0++] || R1.L = W[I1++];
	A0 = R0.L * R4.L (IS) || R2.L = W[I2++] || R3.L = W[I3++];
	A0 += R1.L * R4.H (IS) || R1 = [P0++];
	A0 += R2.L * R5.L (IS);
	R0 = (A0 += R3.L * R5.H) (IS);
	R0 >>>= R6;
	R0 = R0 + R1;
.Ls4a_e:
	[P1++] = R0;
	(R7:4, P5:3) = [SP++];
	RTS;
.Ls4_set:
	LSETUP (.Ls4s_b, .Ls4s_e) LC0 = P2;
.Ls4s_b:
	MNOP || R0.L = W[I0++] || R1.L = W[I1++];
	A0 = R0.L * R4.L (IS) || R2.L = W[I2++] || R3.L = W[I3++];
	A0 += R1.L * R4.H (IS);
	A0 += R2.L * R5.L (IS);
	R0 = (A0 += R3.L * R5.H) (IS);
	R0 >>>= R6;
.Ls4s_e:
	[P1++] = R0;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _qk_sum4, .-_qk_sum4

	.align 4
	.global _qk_sum7
	.type _qk_sum7, STT_FUNC
_qk_sum7:                                /* sh 13, add 0 (the tank's taps; qk_sum7_c the same) */
	[--SP] = (R7:4, P5:3);
	P5 = R0;
	R0 = [P5 + 0];
	I0 = R0;
	R0 = [P5 + 4];
	I1 = R0;
	R0 = [P5 + 8];
	I2 = R0;
	R0 = [P5 + 12];
	I3 = R0;
	L0 = 0;
	L1 = 0;
	L2 = 0;
	L3 = 0;
	P2 = [P5 + 16];
	P3 = [P5 + 20];
	P4 = [P5 + 24];
	R4 = [P5 + 32];
	R5 = [P5 + 36];
	R6 = [P5 + 40];
	R7 = [P5 + 44];
	P0 = [P5 + 48];
	P1 = 64;
	LSETUP (.Ls7_b, .Ls7_e) LC0 = P1;
.Ls7_b:
	MNOP || R0.L = W[I0++] || R1.L = W[I1++];
	A0 = R0.L * R4.L (IS) || R2.L = W[I2++] || R3.L = W[I3++];
	A0 += R1.L * R4.H (IS) || R0 = W[P2++] (X);
	A0 += R2.L * R5.L (IS) || R1 = W[P3++] (X);
	A0 += R3.L * R5.H (IS) || R2 = W[P4++] (X);
	A0 += R0.L * R6.L (IS);
	A0 += R1.L * R6.H (IS);
	R0 = (A0 += R2.L * R7.L) (IS);
	R0 >>>= 13;
.Ls7_e:
	[P0++] = R0;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _qk_sum7, .-_qk_sum7

	.align 4
	.global _qk_pmix
	.type _qk_pmix, STT_FUNC
_qk_pmix:
	P0 = R0;
	I0 = R0;
	L0 = 0;
	P1 = R1;
	P2 = 64;
	LSETUP (.Lpm_b, .Lpm_e) LC0 = P2;
.Lpm_b:
	R0.L = W[I0++];
	R1 = W[P1++] (X);
	R0.L = R0.L * R2.H (T);
	R1.L = R1.L * R2.L (T);
	R0.L = R0.L + R1.L (S);
.Lpm_e:
	W[P0++] = R0;
	RTS;
	.size _qk_pmix, .-_qk_pmix

	.align 4
	.global _qk_rvin
	.type _qk_rvin, STT_FUNC
_qk_rvin:
	[--SP] = (R7:4, P5:3);
	P5 = R0;
	P0 = [P5 + 0];
	P1 = [P5 + 4];
	P2 = [P5 + 8];
	P3 = [P5 + 12];
	P4 = [P5 + 16];
	R7 = [P5 + 20];                      /* dl */
	R6 = 0;
	P5 = 64;
	LSETUP (.Lri_b, .Lri_e) LC0 = P5;
.Lri_b:
	R0 = W[P1++] (X);
	R1 = W[P2++] (X);
	R0 = R0 + R1;
	R0 >>>= 1;
	R1 = W[P3++] (X);
	R2 = W[P4++] (X);
	R1 = R1 + R2;
	R1 >>>= 1;
	R1.L = R1.L * R7.L (T);
	R0.L = R0.L + R1.L (S);
	W[P0++] = R0;
	R0 = R0.L (X);
	R1 = R0 >>> 31;
	R0 = R0 ^ R1;
.Lri_e:
	R6 = MAX(R6, R0);
	R0 = R6;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _qk_rvin, .-_qk_rvin

	.align 4
	.global _qk_bw
	.type _qk_bw, STT_FUNC
_qk_bw:
	[--SP] = (R7:4);
	P2 = R0;
	P1 = [P2 + 0];                       /* q */
	P0 = [P2 + 4];                       /* x */
	R4 = [P2 + 8];                       /* z */
	R7 = [P2 + 12];                      /* k */
	P2 = 64;
	LSETUP (.Lbw_b, .Lbw_e) LC0 = P2;
.Lbw_b:
	R0 = W[P1++] (X);
	R2 = R4 - R0;
	R2 >>>= 1;
	R2 = R2.L * R7.L (IS);
	R2 >>>= 14;
	R4 = R0 + R2;
.Lbw_e:
	W[P0++] = R4;
	R0 = R4;
	(R7:4) = [SP++];
	RTS;
	.size _qk_bw, .-_qk_bw
#endif
