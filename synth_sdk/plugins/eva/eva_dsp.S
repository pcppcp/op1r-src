/* EVA's block passes, hand-scheduled for the Blackfin: the same arithmetic as eva.c's C loops (kept there for host
 * builds), bit for bit, with each pass's state in registers and its loads and stores in parallel slots. In render
 * order: eva_saw / eva_pul (the source below 5.5 kHz), eva_noise (breath), eva_pad, eva_vowel (per band), eva_ens,
 * eva_noise (hiss), eva_rhp / eva_rlp / eva_rout (or eva_rsat, then eva_rcrush or the beeping voice's C tail); plus
 * eva_clear (note_on) and eva_inv (the polyBLEP's 1 / dt). Each is described above its code.
 *
 * void eva_ens(int32_t *pb, const int32_t *k)
 *   the ensemble: two interpolated taps of the doubled delay line, then the level and the carrier, in place on pb
 *   (64 samples, 8 sub-blocks of 8). k = { dl, wi, da, db, (256 - fb) << 16 | (256 - fa), fb << 16 | fa,
 *   then per sub-block cho, ag (8 pairs) and one spare pair (read after the last sub-block, not used) }.
 *   The line is a circle of 384 samples (768 bytes): the write through I0, the taps through I2 / I3, all three
 *   circular over it (B = dl, L = 768); the taps start da, db samples (167 .. 376) behind dl[wi] (ra[-1], rb[-1]:
 *   I2 = I3 = &dl[wi], then I2 -= 2 da, I3 -= 2 db, wrapping on the circle). The write w = clamp(y, -32768, 32767) is y << 16 saturated (its high half);
 *   it goes to dl[wi]. A tap a + ((r - a) * f >> 8) is
 *   (a * (256 - f) + r * f) >> 8 (a * 256 is a multiple of 256), both taps in one dual multiply-accumulate.
 *
 * Products of 32-bit values use the 32-bit multiply (it wraps as the C's int32 product does); 16 x 16 products only
 * where both operands are int16 by construction. No packet stores a register another of its ops writes, or loads a
 * register another of its ops reads; no shift beside a P-register store (anomaly 05000074). L0 .. L3 are 0 again on
 * return; eva.c restores the caller's L0..L3 after the block. M1 is not touched; eva_ens saves and restores M2 / M3.
 * The per-block kernels go in .text.ev_hot (with eva.c's voice-call code, one run of lines), the rare ones (eva_rsat,
 * eva_rcrush, eva_clear, eva_inv) in .text.ev_rare after them.
 * Host builds (screen and golden harnesses) assemble it empty: eva.c keeps the C loops there.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__

/* ---- eva_ens: 13 packets per sample ---- */
/* R4 = 256 - f (b | a), R5 = f (b | a), R6 cho, R7 ag; R3 y, R2 the write then the sample, R0 (a | b) and R1
 * (ra[-1] | rb[-1]) loaded a sample ahead; P4 reads pb a sample ahead, P5 stores it a sample behind (the first store
 * writes pb[-1] back unchanged), P3 k's pairs */
	.section .text.ev_hot, "ax", @progbits
	.align 4
	.global _eva_ens
	.type _eva_ens, STT_FUNC
_eva_ens:
	[--SP] = (R7:4, P5:3);
	[--SP] = M2;
	[--SP] = M3;
	P4 = R0;
	P5 = R1;
	R2 = [P5];                                       /* dl */
	R3 = [P5 + 4];                                   /* wi */
	R3 = R3 << 1;
	B0 = R2;
	B2 = R2;
	B3 = R2;
	R0 = R2 + R3;
	I0 = R0;
	R1 = 768;                                        /* the circle, bytes */
	L0 = R1;
	L2 = R1;
	L3 = R1;
	I2 = R0;
	I3 = R0;
	R0 = [P5 + 8];
	R0 = R0 << 1;
	M2 = R0;
	R0 = [P5 + 12];
	R0 = R0 << 1;
	M3 = R0;
	I2 -= M2;                                        /* &ra[-1], circular */
	I3 -= M3;                                        /* &rb[-1] */
	R4 = [P5 + 16];
	R5 = [P5 + 20];
	P3 = P5;
	P3 += 24;
	R6 = [P3++];
	R7 = [P3++];
	P5 = P4;
	P5 += -4;
	R2 = [P5];                                       /* pb[-1], stored back unchanged by the first lagging store */
	R3 = [P4++];                                     /* y */
	R1.L = W[I2++];                                  /* ra[-1] */
	R1.H = W[I3++];                                  /* rb[-1] */
	R0.L = W[I2];                                    /* a */
	R0.H = W[I3];                                    /* b */
	P1 = 8;
	P2 = 8;
	LSETUP (.Lens_ob, .Lens_oe) LC1 = P1;
.Lens_ob:
	LSETUP (.Lens_ib, .Lens_ie) LC0 = P2;
.Lens_ib:
	A1 = R0.H * R4.H, A0 = R0.L * R4.L (IS) || [P5++] = R2 || NOP;
	R1 = (A1 += R1.H * R5.H), R0 = (A0 += R1.L * R5.L) (IS);
	R2 = R3 << 16 (S);                               /* the write: clamp(y, -32768, 32767) in the high half */
	R0 = R0 >>> 8 || W[I0++] = R2.H || NOP;          /* ta */
	R1 = R1 >>> 8;                                   /* tb */
	R2 = R0 + R1 (NS);
	R2 = R2 >>> 1 || R1.L = W[I2++] || NOP;
	R2 = R2 - R3 (NS) || R1.H = W[I3++] || NOP;
	R2 *= R6;
	R2 = R2 >>> 15 || R0.L = W[I2] || NOP;
	R2 = R2 + R3 (NS) || R0.H = W[I3] || NOP;
	R2 *= R7;
.Lens_ie:
	R2 = R2 >>> 15 || R3 = [P4++] || NOP;
	R6 = [P3++];
.Lens_oe:
	R7 = [P3++];
	[P5] = R2;
	R0 = 0;
	L0 = R0;
	L2 = R0;
	L3 = R0;
	M3 = [SP++];
	M2 = [SP++];
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _eva_ens, .-_eva_ens

/* ---- the radio: eva_rhp, eva_rlp, then eva_rout (sclip, gain, out), or eva_rsat (sclip in place) and eva_rcrush ----
 * Each pass reads pb a sample ahead (pb[64] is read, not used) and the in-place ones store a sample behind (the first
 * store writes pb[-1] back unchanged). */

/* void eva_rhp(int32_t *pb, int32_t *st, const int32_t *hp): in place, y = clamp(pb, +-65535), the two high-pass
 * one-poles, pb = y - hs1 - hs2 (the low-pass's input); st = { hs1, hs2 }; hp: 4 sub-blocks of 16 and a spare word.
 * 12 packets per sample. R0 y, R1 d (and the lagging store), R2 hs1, R3 hs2, R4 hp, R5 65535, R6 -65535, R7 next */
	.align 4
	.global _eva_rhp
	.type _eva_rhp, STT_FUNC
_eva_rhp:
	[--SP] = (R7:4, P5:3);
	P0 = R1;
	P2 = R2;
	I0 = R0;
	R0 += -4;
	I1 = R0;
	P1 = R0;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	R1 = [P1];
	R7 = [I0++];
	R2 = [P0];
	R3 = [P0 + 4];
	R4 = [P2++];
	R5 = 65535 (Z);
	R6 = -R5;
	P4 = 4;
	P5 = 16;
	LSETUP (.Lrhp_ob, .Lrhp_oe) LC1 = P4;
.Lrhp_ob:
	LSETUP (.Lrhp_ib, .Lrhp_ie) LC0 = P5;
.Lrhp_ib:
	R0 = MIN(R7, R5);
	R0 = MAX(R0, R6) || R7 = [I0++] || [I1++] = R1;
	R1 = R0 - R2 (NS);
	R1 *= R4;
	R1 >>>= 14;
	R2 = R2 + R1;
	R0 = R0 - R2;
	R1 = R0 - R3;
	R1 *= R4;
	R1 >>>= 14;
	R3 = R3 + R1;
.Lrhp_ie:
	R1 = R0 - R3;
.Lrhp_oe:
	R4 = [P2++];
	[I1++] = R1;
	[P0] = R2;
	[P0 + 4] = R3;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _eva_rhp, .-_eva_rhp

/* void eva_rlp(int32_t *pb, int32_t *st, const int32_t *k): in place, the two low-pass one-poles and the drive,
 * pb = (l2 * sat) >> 12; st = { l1, l2 }; k = { rlp, sat } for 4 sub-blocks of 16 and a spare pair.
 * 11 packets per sample. R1 d, R2 l1, R3 l2, R4 rlp, R5 sat, R6 the result (stored a sample behind), R7 next */
	.align 4
	.global _eva_rlp
	.type _eva_rlp, STT_FUNC
_eva_rlp:
	[--SP] = (R7:4, P5:3);
	P0 = R1;
	P2 = R2;
	I0 = R0;
	R0 += -4;
	I1 = R0;
	P1 = R0;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	R6 = [P1];
	R7 = [I0++];
	R2 = [P0];
	R3 = [P0 + 4];
	R4 = [P2++];
	R5 = [P2++];
	P4 = 4;
	P5 = 16;
	LSETUP (.Lrlp_ob, .Lrlp_oe) LC1 = P4;
.Lrlp_ob:
	LSETUP (.Lrlp_ib, .Lrlp_ie) LC0 = P5;
.Lrlp_ib:
	R1 = R7 - R2 (NS);
	R1 *= R4;
	R1 = R1 >>> 15 || R7 = [I0++] || [I1++] = R6;
	R2 = R2 + R1;
	R1 = R2 - R3;
	R1 *= R4;
	R1 >>>= 15;
	R3 = R3 + R1;
	R6 = R3;
	R6 *= R5;
.Lrlp_ie:
	R6 >>>= 12;
	R4 = [P2++];
.Lrlp_oe:
	R5 = [P2++];
	[I1++] = R6;
	[P0] = R2;
	[P0 + 4] = R3;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _eva_rlp, .-_eva_rlp

/* sclip of \y (clamped to +-49151: R3 / R4; R5 4855), with R1 / R2 as scratch: 8 packets after the clamps */
.macro SCLIP y
	\y = MIN(\y, R3);
	\y = MAX(\y, R4);
	R1 = \y >>> 1;                                   /* h, |h| <= 24575 */
	R2 = R1.L * R1.L (IS);
	R2 = R2 >>> 14;                                  /* x2 >> 1 */
	R2 *= R1;
	R2 >>>= 13;                                      /* x3 */
	R2 *= R5;
	R2 >>>= 15;
	\y = \y - R2;
.endm

/* void eva_rsat(int32_t *pb): in place, pb = sclip(pb). 11 packets per sample. R0 y, R6 the result (stored a
 * sample behind), R7 next */
	.section .text.ev_rare, "ax", @progbits
	.align 4
	.global _eva_rsat
	.type _eva_rsat, STT_FUNC
_eva_rsat:
	[--SP] = (R7:4, P5:3);
	I0 = R0;
	R0 += -4;
	I1 = R0;
	P1 = R0;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	R6 = [P1];
	R7 = [I0++];
	R3 = 49151 (Z);
	R4 = -R3;
	R5 = 4855 (Z);
	P5 = 64;
	LSETUP (.Lrsat_b, .Lrsat_e) LC0 = P5;
.Lrsat_b:
	R0 = R7;
	R0 = MIN(R0, R3) || R7 = [I0++] || [I1++] = R6;
	R0 = MAX(R0, R4);
	R1 = R0 >>> 1;
	R2 = R1.L * R1.L (IS);
	R2 = R2 >>> 14;
	R2 *= R1;
	R2 >>>= 13;
	R2 *= R5;
	R2 >>>= 15;
.Lrsat_e:
	R6 = R0 - R2;
	[I1++] = R6;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _eva_rsat, .-_eva_rsat

/* one output pair from \y (last in \l): \y = \y * gain >> 15; R1 = (last + y) >> 1; the caller stores R1.L, then
 * \y.L (two 16-bit stores: nothing here assumes out is 4-aligned) */
.macro OUTPAIR y, l
	\y *= R6;
	\y >>>= 15;
	R1 = \l + \y (NS);
	R1 = R1 >>> 1;
.endm

/* void eva_rout(const int32_t *pb, int16_t *out, int32_t *last, int32_t gain): sclip, the gain and the 2x
 * interpolation into out (128 samples); *last updated. Two samples per pass of the loop, the sample alternating
 * between R0 and R7 (the other one is last). 16 packets per sample */
	.section .text.ev_hot, "ax", @progbits
	.align 4
	.global _eva_rout
	.type _eva_rout, STT_FUNC
_eva_rout:
	[--SP] = (R7:4, P5:3);
	I0 = R0;
	I1 = R1;
	P0 = R2;
	R6 = [SP + 40];                                  /* gain: the 4th argument, on the stack (12 + 28 pushed) */
	R0 = 0;
	L0 = R0;
	L1 = R0;
	R7 = [P0];
	R0 = [I0++];
	R3 = 49151 (Z);
	R4 = -R3;
	R5 = 4855 (Z);
	P5 = 32;
	LSETUP (.Lrout_b, .Lrout_e) LC0 = P5;
.Lrout_b:
	SCLIP R0
	OUTPAIR R0, R7
	MNOP || R7 = [I0++] || W[I1++] = R1.L;
	W[I1++] = R0.L;
	SCLIP R7
	OUTPAIR R7, R0
	MNOP || R0 = [I0++] || W[I1++] = R1.L;
.Lrout_e:
	W[I1++] = R7.L;
	[P0] = R7;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _eva_rout, .-_eva_rout

/* void eva_rcrush(const int32_t *pb, int16_t *out, int32_t *st, const int32_t *k): after eva_rsat, the crush's
 * sample-and-hold mixed in, the gain and the 2x interpolation into out; st = { last, cnt, held } (updated);
 * k = { hold_n, mask (~(step - 1)), gain, cr for 4 sub-blocks of 16, a spare word }.
 * R2 held, R3 mask, R4 cr, R6 gain, P1 cnt, P2 hold_n, P0 0; the sample alternates between R0 and R7 */
.macro CRUSH y
	P1 += 1;
	CC = P2 <= P1;                                   /* ++cnt >= hold_n */
	IF CC P1 = P0;
	R1 = \y & R3;
	IF CC R2 = R1;
	R1 = R2 - \y (NS);
	R1 *= R4;
	R1 >>>= 15;
	\y = \y + R1;
.endm

	.section .text.ev_rare, "ax", @progbits
	.align 4
	.global _eva_rcrush
	.type _eva_rcrush, STT_FUNC
_eva_rcrush:
	[--SP] = (R7:4, P5:3);
	I0 = R0;
	I1 = R1;
	P5 = R2;
	P3 = [SP + 40];                                  /* k: the 4th argument, on the stack (12 + 28 pushed) */
	R0 = 0;
	L0 = R0;
	L1 = R0;
	P0 = 0;
	R7 = [P5];
	P1 = [P5 + 4];
	R2 = [P5 + 8];
	P2 = [P3++];
	R3 = [P3++];
	R6 = [P3++];
	R4 = [P3++];
	R0 = [I0++];
	P4 = 4;
	LSETUP (.Lcr_ob, .Lcr_oe) LC1 = P4;
.Lcr_ob:
	P4 = 8;
	LSETUP (.Lcr_ib, .Lcr_ie) LC0 = P4;
.Lcr_ib:
	CRUSH R0
	OUTPAIR R0, R7
	MNOP || R7 = [I0++] || W[I1++] = R1.L;
	W[I1++] = R0.L;
	CRUSH R7
	OUTPAIR R7, R0
	MNOP || R0 = [I0++] || W[I1++] = R1.L;
.Lcr_ie:
	W[I1++] = R7.L;
.Lcr_oe:
	R4 = [P3++];
	[P5] = R7;
	[P5 + 4] = P1;
	[P5 + 8] = R2;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _eva_rcrush, .-_eva_rcrush

/* ---- the source below 5.5 kHz (eva.c's first loop): eva_saw, then eva_pul ----
 * BLEP \ph -> \s: eva.c's blep_saw(ph, dt, inv8) with dt / inv8 at [P5 + \dto] / [P5 + \io\()]; \t \x \k scratch. The
 * products are 32-bit multiplies (as the C's int32 products). */
.macro BLEP ph, s, t, x, k, dto, io
	\t = \ph >> 17;
	\s = \ph;
	BITTGL (\s, 31);
	\s = \s >>> 16;                                  /* (ph >> 16) - 32768 */
	\k = [P5 + \dto\()];
	CC = \t < \k;
	IF CC JUMP 1f;
	\x = 32768 (Z);
	\x = \x - \k;
	CC = \x < \t;                                    /* t > 32768 - dt */
	IF !CC JUMP 3f;
	\x = 32768 (Z);
	\t = \t - \x;
	\k = [P5 + \io\()];
	\t *= \k;
	\t >>>= 7;
	\x = \t;
	\x *= \t;
	\x >>>= 15;
	\x = \x + \t;
	\x = \x + \t;
	\k = 32768 (Z);
	\x = \x + \k;
	\s = \s - \x;
	JUMP 3f;
1:
	\k = [P5 + \io\()];
	\t *= \k;
	\t >>>= 7;
	\x = \t;
	\x *= \t;
	\x >>>= 15;
	\k = \t + \t;
	\k = \k - \x;
	\x = 32768 (Z);
	\k = \k - \x;
	\s = \s - \k;
3:
.endm

/* void eva_saw(int32_t *y, uint32_t *ph, const int32_t *k): y[i] = saw * 19000 for the 64 samples, ph += i1 before
 * each; k = { i1, (2 i1) ^ 0x80000000, dt1, inv1 }. The phase is held XOR 0x80000000 (its signed high half is the
 * plain ramp (ph >> 16) - 32768; near the wrap, ph + i1 < 2 i1, is a signed compare of it + i1 with k[1]); near the
 * wrap the polyBLEP runs out of line. 5 packets per sample away from the wrap.
 * R0 phase, R2 i1, R4 k[1], R6.L 19000, R7 the sample; R1 R3 R5 the polyBLEP's scratch */
	.section .text.ev_hot, "ax", @progbits
	.align 4
	.global _eva_saw
	.type _eva_saw, STT_FUNC
_eva_saw:
	[--SP] = (R7:4, P5:3);
	I1 = R0;
	P4 = R1;
	P5 = R2;
	R0 = 0;
	L1 = R0;
	R0 = [P4];
	BITTGL (R0, 31);
	R2 = [P5];
	R4 = [P5 + 4];
	R6 = 19000 (X);
	R0 = R0 + R2 (NS);
	P1 = 64;
	LSETUP (.Lsaw_b, .Lsaw_e) LC0 = P1;
.Lsaw_b:
	R7 = R0 + R2 (NS);
	CC = R7 < R4;
	IF CC JUMP .Lsaw_edge;
	R7 = R0.H * R6.L (IS);
.Lsaw_e:
	R0 = R0 + R2 (NS) || [I1++] = R7 || NOP;
	R0 = R0 - R2 (NS);
	BITTGL (R0, 31);
	[P4] = R0;
	(R7:4, P5:3) = [SP++];
	RTS;
.Lsaw_edge:
	R7 = R0;
	BITTGL (R7, 31);                                 /* ph */
	BLEP R7, R5, R3, R1, R7, 8, 12
	R7 = 19000 (X);
	R5 *= R7;
	R7 = R5;
	JUMP .Lsaw_e;
	.size _eva_saw, .-_eva_saw

/* void eva_pul(int32_t *x, uint32_t *ph, const int32_t *k): in place on x (eva_saw's saw * 19000; x[64] is read, not
 * used): x = (saw * 19000 + pul * 12000) >> 15 with the soft pulse pul, ph += i2 before each sample; k = { i2, 2 i2,
 * 4 i2, dt2, inv2 }. Away from both edges, (ph << 1) + 2 i2 >= 4 i2 (unsigned; W below is (ph + i2) << 1), pul is
 * +-16384 and x = (x >> 15) + (ph < 2^31 ? -6000 : 6000), as s - (s ^ 6000) with s = ph >>> 31. 9 packets per
 * sample there; near an edge the polyBLEP runs out of line.
 * R0 the saw part (loaded a sample ahead), R1 ph, R2 2 i2, R3 i2, R4 4 i2, R5 W, R6 6000, R7 the sample */
	.align 4
	.global _eva_pul
	.type _eva_pul, STT_FUNC
_eva_pul:
	[--SP] = (R7:4, P5:3);
	I0 = R0;
	I1 = R0;
	P4 = R1;
	P5 = R2;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	R1 = [P4];
	R3 = [P5];
	R2 = [P5 + 4];
	R4 = [P5 + 8];
	R6 = 6000 (X);
	R5 = R1 + R3 (NS);
	R5 = R5 << 1;
	R1 = R1 + R3 (NS);
	R0 = [I0++];
	P1 = 64;
	LSETUP (.Lpul_b, .Lpul_e) LC0 = P1;
.Lpul_b:
	R5 = R5 + R2 (NS);
	CC = R5 < R4 (IU);
	IF CC JUMP .Lpul_edge;
	R0 = R0 >>> 15;
	R7 = R1 >>> 31;
	R0 = R0 + R7;
	R7 = R7 ^ R6;
	R7 = R0 - R7;
.Lpul_e:
	R1 = R1 + R3 (NS) || R0 = [I0++] || [I1++] = R7;
	R1 = R1 - R3 (NS);
	[P4] = R1;
	(R7:4, P5:3) = [SP++];
	RTS;
.Lpul_edge:
	[--SP] = (R7:2);
	R7 = R1 + R3;
	CC = R7 < R2 (IU);                               /* ph + i2 < 2 i2: the up edge */
	IF !CC JUMP .Lpul_down;
	BLEP R1, R5, R3, R4, R6, 12, 16                  /* blep_saw(ph) */
	R7 = R1 >>> 16;                                  /* ((ph + 2^31) >> 16) - 32768 */
	R7 = R5 - R7;
	JUMP .Lpul_mix;
.Lpul_down:
	R2 = R1;
	BITTGL (R2, 31);
	BLEP R2, R5, R3, R4, R6, 12, 16                  /* blep_saw(ph + 2^31) */
	R7 = R1;
	BITTGL (R7, 31);
	R7 = R7 >>> 16;                                  /* (ph >> 16) - 32768 */
	R7 = R7 - R5;
.Lpul_mix:
	R7 >>>= 1;                                       /* pul */
	R6 = 12000 (X);
	R7 *= R6;
	R0 = R7 + R0;
	R0 >>>= 15;
	(R7:2) = [SP++];
	R7 = R0;
	JUMP .Lpul_e;
	.size _eva_pul, .-_eva_pul

/* ---- eva_vowel: one formant band, 17 packets per sample ----
 * void eva_vowel(int32_t *pb, const int32_t *x, int32_t *k): pb += the band (in place; the store runs a sample
 * behind, the first writes pb[-1] back unchanged); k = { lo, bd (in / out), then f, q, g for 4 sub-blocks of 16 and a
 * spare triple }: lo += f * bd >> 14; bd += f * ((x >> 2) - lo - (q * bd >> 14)) >> 14; pb += bd * g >> 13.
 * R0 lo, R1 bd, R2 f, R3 q, R4 g, R5 x >> 2, R6 / R7 scratch (R7 also pb); copies as R6 = R1 << 0 (parallel slots) */
	.align 4
	.global _eva_vowel
	.type _eva_vowel, STT_FUNC
_eva_vowel:
	[--SP] = (R7:4, P5:3);
	I1 = R0;
	R0 += -4;
	I2 = R0;
	P1 = R0;
	I0 = R1;
	P5 = R2;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	L2 = R0;
	R7 = [P1];                                       /* pb[-1] */
	R0 = [P5];
	R1 = [P5 + 4];
	P2 = P5;
	P2 += 8;
	R2 = [P2++];
	R3 = [P2++];
	R4 = [P2++];
	P3 = 4;
	P4 = 16;
	LSETUP (.Lvw_ob, .Lvw_oe) LC1 = P3;
.Lvw_ob:
	LSETUP (.Lvw_ib, .Lvw_ie) LC0 = P4;
.Lvw_ib:
	R6 = R1 << 0 || R5 = [I0++] || [I2++] = R7;
	R6 *= R2;
	R6 = R6 >>> 14;
	R0 = R0 + R6 (NS);                               /* lo */
	R5 = R5 >>> 2;
	R6 = R1 << 0;
	R6 *= R3;
	R6 = R6 >>> 14;
	R7 = R5 - R0 (NS);
	R7 = R7 - R6 (NS);
	R7 *= R2;
	R7 = R7 >>> 14;
	R1 = R1 + R7 (NS);                               /* bd */
	R6 = R1 << 0 || R7 = [I1++] || NOP;
	R6 *= R4;
	R6 = R6 >>> 13;
.Lvw_ie:
	R7 = R7 + R6 (NS);
	R2 = [P2++];
	R3 = [P2++];
.Lvw_oe:
	R4 = [P2++];
	[I2++] = R7;
	[P5] = R0;
	[P5 + 4] = R1;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _eva_vowel, .-_eva_vowel

/* ---- eva_noise: the breath (on the source) and the hiss (on the voice), 5 packets per sample ----
 * void eva_noise(int32_t *buf, uint32_t *noise, const int32_t *k): in place, buf += ((int32_t)lcg(noise) >> 16) * lvl
 * >> 15, the LCG stepped once per sample; k = { sub-blocks, samples per sub-block, lvl per sub-block, a spare word }
 * (lvl 0 .. 32767: the LCG's high half and lvl are int16, one 16 x 16 product). The store runs a sample behind (the
 * first one writes buf[-1], a spare word).
 * R0 the LCG, R1 1664525, R2 1013904223, R3 lvl, R5 the sample (loaded), R6 the result, R7 the product */
	.align 4
	.global _eva_noise
	.type _eva_noise, STT_FUNC
_eva_noise:
	[--SP] = (R7:4, P5:3);
	I0 = R0;
	R0 += -4;
	I1 = R0;
	P4 = R1;
	P5 = R2;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	R0 = [P4];
	R1.H = 25;
	R1.L = 26125;                                    /* 1664525 */
	R2.H = 15470;
	R2.L = 62303;                                    /* 1013904223 */
	P3 = [P5++];
	P2 = [P5++];
	R3 = [P5++];
	LSETUP (.Lnz_ob, .Lnz_oe) LC1 = P3;
.Lnz_ob:
	LSETUP (.Lnz_ib, .Lnz_ie) LC0 = P2;
.Lnz_ib:
	R0 *= R1;
	R0 = R0 + R2 (NS);
	R7 = R0.H * R3.L (IS) || R5 = [I0++] || [I1++] = R6;
	R7 = R7 >>> 15;
.Lnz_ie:
	R6 = R5 + R7 (NS);
.Lnz_oe:
	R3 = [P5++];
	[I1++] = R6;
	[P4] = R0;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _eva_noise, .-_eva_noise

/* ---- eva_pad: the pad's two low-pass one-poles, 8 packets per sample ----
 * void eva_pad(const int32_t *x, int32_t *pb, int32_t *k): pb = a2 after a1 += (x - a1) * lpa >> 15,
 * a2 += (a1 - a2) * lpa >> 15; k = { a1, a2 (in / out), lpa for 8 sub-blocks of 8, a spare word }. x is read a sample
 * ahead (x[64] is read, not used), pb stored a sample behind (the first store writes pb[-1], a spare word).
 * R1 d, R2 a1, R3 a2, R4 lpa, R7 x */
	.align 4
	.global _eva_pad
	.type _eva_pad, STT_FUNC
_eva_pad:
	[--SP] = (R7:4, P5:3);
	I0 = R0;
	R1 += -4;
	I1 = R1;
	P5 = R2;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	R2 = [P5];
	R3 = [P5 + 4];
	P2 = P5;
	P2 += 8;
	R4 = [P2++];
	R7 = [I0++];
	P3 = 8;
	LSETUP (.Lpad_ob, .Lpad_oe) LC1 = P3;
.Lpad_ob:
	P4 = 8;
	LSETUP (.Lpad_ib, .Lpad_ie) LC0 = P4;
.Lpad_ib:
	R1 = R7 - R2 (NS) || [I1++] = R3 || NOP;
	R1 *= R4;
	R1 = R1 >>> 15 || R7 = [I0++] || NOP;
	R2 = R2 + R1 (NS);
	R1 = R2 - R3 (NS);
	R1 *= R4;
	R1 = R1 >>> 15;
.Lpad_ie:
	R3 = R3 + R1 (NS);
.Lpad_oe:
	R4 = [P2++];
	[I1++] = R3;
	[P5] = R2;
	[P5 + 4] = R3;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _eva_pad, .-_eva_pad

/* ---- eva_clear: void eva_clear(int16_t *dl): dl[0 .. 384) = 0 (note_on's delay-line clear), a word per packet;
 * dl is 4-aligned (hot_t) ---- */
	.section .text.ev_rare, "ax", @progbits
	.align 4
	.global _eva_clear
	.type _eva_clear, STT_FUNC
_eva_clear:
	P0 = R0;
	R0 = 0;
	P1 = 192;
	LSETUP (.Lclr_b, .Lclr_b) LC0 = P1;
.Lclr_b:
	[P0++] = R0;
	RTS;
	.size _eva_clear, .-_eva_clear

/* ---- int32_t eva_inv(int32_t d): floor(2^22 / d) for 1 <= d < 2^15 (eva.c's ((1 << 30) / dt0) >> 8, the polyBLEP's
 * 1 / dt when the pitch moves): libgcc's restoring division (__udivsi3, as SID's sid_udiv) started b = 23 - k bits
 * from the end, k = floor(log2 d) (SIGNBITS d = 30 - k), where 2^22 >> b = 2^(k - 1) < d already: 5 b packets in
 * the loop (b = 9 .. 23) instead of about 190 for the library call. Only R0..R3, P0, P1 and LC0 ---- */
	.align 4
	.global _eva_inv
	.type _eva_inv, STT_FUNC
_eva_inv:
	R1 = R0;                                         /* d */
	R2.L = SIGNBITS R0;
	R2 = R2.L (Z);
	R2 += -7;                                        /* b */
	R0 = 1;
	R0 <<= 22;                                       /* n */
	P0 = R2;
	R3 = -R2;
	R3 = LSHIFT R0 BY R3.L;                          /* the partial remainder: n >> b */
	R2 += -32;
	R2 = -R2;                                        /* 32 - b */
	P1 = R2;
	R0 = LSHIFT R0 BY R2.L;                          /* n's low b bits at the top */
	LSETUP (.Linv_b, .Linv_e) LC0 = P0;
.Linv_b:
	R0 = ROT R0 BY 1;                                /* CC = the next bit of n; in: the last quotient bit, inverted */
	R3 = ROT R3 BY 1;
	R2 = R3 - R1;
	CC = R3 < R1 (IU);
.Linv_e:
	IF !CC R3 = R2;
	R0 = ROT R0 BY 1;
	R0 = ~R0;
	R2 = P1;
	R0 = LSHIFT R0 BY R2.L;
	R2 = -R2;
	R0 = LSHIFT R0 BY R2.L;                          /* the b quotient bits */
	RTS;
	.size _eva_inv, .-_eva_inv

#endif
