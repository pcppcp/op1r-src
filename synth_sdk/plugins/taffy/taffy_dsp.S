/* TAFFY's sample loop hand-scheduled for the Blackfin: the same arithmetic as taffy.c's sample() and render loop
 * (and its host build), bit for bit, in fewer instruction packets.
 *
 * uint32_t tf_eval_<a>[_<b>][_r](int16_t *y, const uint32_t *pitch, uint32_t ph, const int32_t *k,
 *                                const uint32_t *pairs)
 *   y[i] = sample(p, x) for the block's 128 samples, x = (ph += pitch[i]) >> 17; returns the last ph. <a> is the
 *   setup's shape sa, <b> its sb (left out when frac = 0, which only k0 = 0 gives: sa = 0, y = shape a), _r with the
 *   resonance (k3 > 0; with k3 = 0 the C's mix leaves y as it is). y is stored one sample behind (the first store
 *   lands on y[-1], so the buffer has a spare halfword in front).
 *   k (taffy.c tf_consts), 16 words: shape a's slot [0..3], shape b's [4..7] (offset 16), then
 *     [8] PAT_I = 32 << 16 | EXTRACT bits 29..20, [9] PAT_F = EXTRACT bits 19..15 (its high half 0: see the holds),
 *     [10] frac << 16 | (32768 - frac), [11] rr, [12] -(1 << 21), [13] 32 << 16 | EXTRACT bits 22..13,
 *     [14] 32767 << 16 | EXTRACT bits 12..8, [15] k3 << 16 | (32768 - k3)
 *   slots: saw (0) d, inv1, inv2, c2; square (1) d, inv1, 1 << 29; pulse (2) 2d, inv1;
 *     saw-pulse (3) dh = (d + 1) >> 1, 2 inv1, 2 inv2, c2; c2 = 2^29 - d inv2
 *   pairs[i] = (tf_cos[i + 1] - tf_cos[i]) << 16 | tf_cos[i] (taffy_pairs.h)
 * void tf_out(int16_t *out, const int16_t *y, int32_t *dc_q10, int32_t gain)
 *   the C's DC rejection and output gain over the 128 samples (controls held)
 * void tf_out_fade(int16_t *out, const int16_t *y, const int16_t *yp, int32_t *dc_q10, int32_t gain,
 *                  const uint32_t *w)
 *   the same after the knob crossfade y = yp + (((y - yp) * i) >> 7) (yp: the previous block's setup);
 *   w[i - 1] = 256 i << 16 | (32768 - 256 i) for i = 1..127, w[127] read and not used (taffy_pairs.h tf_fade)
 *
 * Why it is exact:
 *   cosq(p) reads only p's bits 14..0, so each bent phase is built as a 32-bit T with p = T >> 15 (the resonance's
 *     T = x rr - 2^21, p = T >> 8): EXTRACT (Z) takes the table index and the 5-bit fraction f5 straight from T
 *     (the floor shifts and the & 0x7FFF are bit fields of two's complement), and
 *     a + ((d (f5 << 10)) >> 15) = (32 a + d f5) >> 5: one integer MAC pair (A0 = 32 a, += d f5) and a shift
 *   saw:      x < d ? x inv1 : 16384 << 15 + (x - d) inv2 = x inv2 + c2 (the true value fits 31 bits, so the wrapped
 *             32-bit sum is it); the unused arm may overflow (x inv1 up to 2^35) and is dropped by the select
 *   square:   (x & 0x4000) << 15 + (xm < d ? xm inv1 : 2^29), xm = x & 0x3FFF = (ph << 1) >> 18; (xl inv1) >> 16 =
 *             (xm inv1) >> 15 since xl = 2 xm and xl < 2d <=> xm < d
 *   pulse:    x < 2d ? x inv1 : PAT_F (bits 29..15 zero, so cosq gives tf_cos[0] = 32767, the C's hold)
 *   saw-pulse: bend(2x) with 2 inv1, 2 inv2 and 2x < d <=> x < dh; x >= 16384 (ph bit 31) holds as pulse does
 *   a + (((b - a) frac) >> 15) = floor((a (32768 - frac) + b frac) / 2^15): fractional products (2 a c, 2 b frac),
 *     the high half truncated; frac >= 1 here, so 32768 - frac fits 16 bits; the same for k3 >= 1
 *   (cosq(..) (32767 - x)) >> 15: 2 c 32767 - 2 c x in A1, high half truncated
 *   the crossfade: floor((yp (32768 - 256 i) + y 256 i) / 2^15) for i = 1..127 (fractional, truncated); i = 0 is yp
 *   DC: dc += (v 1024 - dc) >> 10 in 32 bits (v 1024 as an integer product); q = dc >> 10 lies in -32768..32767
 *   output: engine_sat16(((v - q) g) >> 15) = 2 v g - 2 q g in A0, the high half truncated and saturated to 16 bits
 *     (v - q stays exact in the 40-bit accumulator)
 * All values that meet a 16-bit multiply are in -32768..32767 (shapes, mixes, crossfade: floored convex
 * combinations of table values).
 *
 * Registers in tf_eval: R7 ph, R2 the next pitch, R4 / R5 the table patterns (the resonance's while it runs), P5 k,
 * P0 pairs, P1 / P2 table rows, I0 pitch, I1 y, I2 &k[2]; R0, R1, R3, R6 and R2 between its uses are scratch.
 * Every function saves the callee-saved registers and the caller's L0..L3 (set to 0 here: linear addressing);
 * M registers are not touched. Host builds (screen and golden harnesses) assemble it empty: taffy.c keeps the C loop.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

/* 11 words pushed: stack arguments at SP + 44 + 12, + 16 .. */
.macro ENTER
	[--SP] = (R7:4, P5:3);
	[--SP] = L0;
	[--SP] = L1;
	[--SP] = L2;
	[--SP] = L3;
	R3 = 0;
	L0 = R3;
	L1 = R3;
	L2 = R3;
	L3 = R3;
.endm

.macro LEAVE
	L3 = [SP++];
	L2 = [SP++];
	L1 = [SP++];
	L0 = [SP++];
	(R7:4, P5:3) = [SP++];
	RTS;
.endm

/* ---- tf_eval: shape a's bent phase into R0 (the phase step first; the previous y stored) ---- */

.macro A_SAW
	R7 = R7 + R2 (NS) || R1 = [P5 + 0] || W[I1++] = R0.L;       /* d */
	R0 = R7 >> 17 || R2 = [P5 + 4] || R3 = [I2];                /* x; inv1, inv2 */
	CC = R0 < R1;
	R6 = R7 >> 17 || R1 = [P5 + 12];                            /* c2 */
	R0 *= R2;
	R6 *= R3;
	R6 = R6 + R1 (NS);
	IF !CC R0 = R6;
.endm

.macro A_SQUARE
	R7 = R7 + R2 (NS) || R1 = [P5 + 0] || W[I1++] = R0.L;       /* d */
	R0 = R7 << 1 || R2 = [P5 + 4];                              /* inv1 */
	R0 = R0 >> 18 || R3 = [P5 + 8];                             /* xm; 2^29 */
	CC = R0 < R1;
	R0 *= R2;
	R6 = R7 >> 31;
	IF !CC R0 = R3;
	R6 = R6 << 29;
	R0 = R0 + R6 (NS);
.endm

.macro A_PULSE
	R7 = R7 + R2 (NS) || R1 = [P5 + 0] || W[I1++] = R0.L;       /* 2d */
	R0 = R7 >> 17 || R2 = [P5 + 4];                             /* x; inv1 */
	CC = R0 < R1;
	R0 *= R2;
	IF !CC R0 = R5;
.endm

/* shape b's bent phase into R1; R2 = its slot's word 0 on entry; R0 and P1 kept */

.macro B_SQUARE
	R1 = R7 << 1 || R3 = [P5 + 20];                             /* inv1 */
	R1 = R1 >> 18;                                              /* xm */
	CC = R1 < R2;
	P1 = P0 + (P1 << 2);
	R1 *= R3;
	R3 = R7 >> 31 || R2 = [P5 + 24];                            /* 2^29 */
	IF !CC R1 = R2;
	R3 = R3 << 29;
	R1 = R1 + R3 (NS);
.endm

.macro B_PULSE
	R1 = R7 >> 17 || R3 = [P5 + 20];                            /* x; inv1 */
	CC = R1 < R2;
	R1 *= R3;
	P1 = P0 + (P1 << 2);
	IF !CC R1 = R5;
.endm

.macro B_SAWPULSE
	R1 = R7 >> 17 || R3 = [P5 + 20];                            /* x; 2 inv1 */
	CC = R1 < R2;                                               /* x < dh */
	R1 *= R3;
	P1 = P0 + (P1 << 2);
	R6 = R7 >> 17 || R3 = [P5 + 24];                            /* 2 inv2 */
	R6 *= R3;
	MNOP || R3 = [P5 + 28] || NOP;                              /* c2 */
	R6 = R6 + R3 (NS);
	IF !CC R1 = R6;
	CC = BITTST(R7, 31);                                        /* x >= 16384 */
	IF CC R1 = R5;
.endm

/* shape a's table row into P1 (R2 = shape b's slot word 0) */
.macro FRONT_A
	R1 = EXTRACT(R0, R4.L) (Z) || R2 = [P5 + 16];
	P1 = R1;                                                    /* P1 = P0 + (P1 << 2) inside the B macro */
.endm

/* both cosines and the frac mix: y in R0.L; R3 = rr */
.macro BACK_AB
	R2 = EXTRACT(R1, R4.L) (Z);
	P2 = R2;                                                    /* four instructions before P2's next use */
	R0 = EXTRACT(R0, R5.L) (Z) || R3 = [P1];                    /* f5 a; row a */
	R1 = EXTRACT(R1, R5.L) (Z) || R2 = [P5 + 40];               /* f5 b; frac << 16 | 32768 - frac */
	A0 = R3.L * R4.H (IS);
	R0 = (A0 += R3.H * R0.L) (IS);
	R0 = R0 >>> 5;                                              /* a */
	P2 = P0 + (P2 << 2);
	A0 = R0.L * R2.L || R3 = [P2];                              /* row b */
	A1 = R3.L * R4.H (IS);
	R1 = (A1 += R3.H * R1.L) (IS);
	R1 = R1 >>> 5 || R3 = [P5 + 44];                            /* b; rr */
	R0.L = (A0 += R1.L * R2.H) (T);
.endm

/* shape a's cosine alone (frac = 0): y in R0; R3 = rr */
.macro COS_A
	R1 = EXTRACT(R0, R4.L) (Z);
	P1 = R1;
	P1 = P0 + (P1 << 2);
	R0 = EXTRACT(R0, R5.L) (Z) || R1 = [P1];
	A0 = R1.L * R4.H (IS) || R3 = [P5 + 44];
	R0 = (A0 += R1.H * R0.L) (IS);
	R0 = R0 >>> 5;
.endm

/* resonance and the k3 mix: y in R0.L; the next pitch into R2, R4 / R5 back to PAT_I / PAT_F */
.macro TAIL_R
	R6 = R7 >> 17 || R1 = [P5 + 48];                            /* x; -2^21 */
	R2 = R6.L * R3.L (IS) || R4 = [P5 + 52];                    /* x rr; resonance patterns */
	R2 = R2 + R1 (NS) || R5 = [P5 + 56];
	R1 = EXTRACT(R2, R4.L) (Z) || R3 = [P5 + 60];               /* k3 << 16 | 32768 - k3 */
	P1 = R1;
	R2 = EXTRACT(R2, R5.L) (Z);
	P1 = P0 + (P1 << 2);
	A0 = R0.L * R3.L || R1 = [P1];
	A1 = R1.L * R4.H (IS);
	R1 = (A1 += R1.H * R2.L) (IS);
	R1 = R1 >>> 5 || R4 = [P5 + 32] || R2 = [I0++];             /* c; PAT_I; the next pitch */
	A1 = R1.L * R5.H;
	R1.H = (A1 -= R1.L * R6.L) (T) || R5 = [P5 + 36];           /* reso; PAT_F */
2:
	R0.L = (A0 += R1.H * R3.H) (T);
.endm

.macro TAIL_N
2:
	MNOP || R2 = [I0++] || NOP;
.endm

.macro EVAL_BEGIN name
	.align 4
	.global \name
	.type \name, STT_FUNC
\name:
	ENTER
	P5 = [SP + 56];                                             /* k */
	P0 = [SP + 60];                                             /* pairs */
	I0 = R1;                                                    /* pitch */
	R0 += -2;
	I1 = R0;                                                    /* y, one sample behind */
	R7 = R2;                                                    /* ph */
	R3 = P5;
	R3 += 8;
	I2 = R3;                                                    /* &k[2] */
	R4 = [P5 + 32];
	R5 = [P5 + 36];
	R2 = [I0++];
	P1 = 128;
	LSETUP (1f, 2f) LC0 = P1;
1:
.endm

/* after the loop (the tail macro holds its end label 2) */
.macro EVAL_END name
	W[I1++] = R0.L;
	R0 = R7;
	LEAVE
	.size \name, . - \name
.endm

EVAL_BEGIN _tf_eval_saw
	A_SAW
	COS_A
	TAIL_N
EVAL_END _tf_eval_saw

EVAL_BEGIN _tf_eval_saw_r
	A_SAW
	COS_A
	TAIL_R
EVAL_END _tf_eval_saw_r

EVAL_BEGIN _tf_eval_saw_square
	A_SAW
	FRONT_A
	B_SQUARE
	BACK_AB
	TAIL_N
EVAL_END _tf_eval_saw_square

EVAL_BEGIN _tf_eval_saw_square_r
	A_SAW
	FRONT_A
	B_SQUARE
	BACK_AB
	TAIL_R
EVAL_END _tf_eval_saw_square_r

EVAL_BEGIN _tf_eval_square_pulse
	A_SQUARE
	FRONT_A
	B_PULSE
	BACK_AB
	TAIL_N
EVAL_END _tf_eval_square_pulse

EVAL_BEGIN _tf_eval_square_pulse_r
	A_SQUARE
	FRONT_A
	B_PULSE
	BACK_AB
	TAIL_R
EVAL_END _tf_eval_square_pulse_r

EVAL_BEGIN _tf_eval_pulse_sawpulse
	A_PULSE
	FRONT_A
	B_SAWPULSE
	BACK_AB
	TAIL_N
EVAL_END _tf_eval_pulse_sawpulse

EVAL_BEGIN _tf_eval_pulse_sawpulse_r
	A_PULSE
	FRONT_A
	B_SAWPULSE
	BACK_AB
	TAIL_R
EVAL_END _tf_eval_pulse_sawpulse_r

/* ---- output: v in R0.H; R5 dc, R6 = 1024 << 16 | gain; the 16-bit result into R2.L ---- */

.macro DCGAIN
	R1 = R0.H * R6.H (IS);                                      /* v 1024 */
	R1 = R1 - R5 (NS);
	R1 = R1 >>> 10;
	R5 = R5 + R1 (NS);
	R1 = R5 >>> 10;                                             /* q */
	A0 = R0.H * R6.L;
	R2.L = (A0 -= R1.L * R6.L) (T);
.endm

	.align 4
	.global _tf_out
	.type _tf_out, STT_FUNC
_tf_out:
	ENTER
	I1 = R0;                                                    /* out */
	I2 = R1;                                                    /* y */
	P3 = R2;                                                    /* &dc */
	R6 = [SP + 56];
	R6.H = 1024;
	R5 = [P3];
	R0.H = W[I2++];
	DCGAIN                                                      /* sample 0 */
	R0.H = W[I2++];
	P1 = 127;
	LSETUP (3f, 4f) LC0 = P1;
3:
	R1 = R0.H * R6.H (IS) || W[I1++] = R2.L || NOP;            /* the previous sample out */
	R1 = R1 - R5 (NS);
	R1 = R1 >>> 10;
	R5 = R5 + R1 (NS);
	R1 = R5 >>> 10;
	A0 = R0.H * R6.L;
4:
	R2.L = (A0 -= R1.L * R6.L) (T) || R0.H = W[I2++] || NOP;   /* reads y[128] at the end (the spare halfword) */
	W[I1++] = R2.L;
	[P3] = R5;
	LEAVE
	.size _tf_out, . - _tf_out

	.align 4
	.global _tf_out_fade
	.type _tf_out_fade, STT_FUNC
_tf_out_fade:
	ENTER
	I1 = R0;                                                    /* out */
	R1 += 2;
	I2 = R1;                                                    /* y from sample 1 */
	I3 = R2;                                                    /* yp */
	P3 = [SP + 56];                                             /* &dc */
	R6 = [SP + 60];
	R6.H = 1024;
	R5 = [P3];
	R0.H = W[I3++];                                             /* sample 0: v = yp[0] */
	DCGAIN
	R3.L = W[I3++];
	R3.H = W[I2++];
	R0 = [SP + 64];
	I0 = R0;                                                    /* the weights */
	R4 = [I0++];                                                /* i = 1 */
	P1 = 127;
	LSETUP (5f, 6f) LC0 = P1;
5:
	A1 = R3.L * R4.L || W[I1++] = R2.L || NOP;                 /* the previous sample out */
	R0.H = (A1 += R3.H * R4.H) (T);                             /* v */
	R1 = R0.H * R6.H (IS) || R3.L = W[I3++] || NOP;
	R1 = R1 - R5 (NS) || R3.H = W[I2++] || NOP;                /* reads one past the buffers at the end (spare) */
	R1 = R1 >>> 10 || R4 = [I0++] || NOP;                      /* the next sample's weights (w[127]: spare) */
	R5 = R5 + R1 (NS);
	R1 = R5 >>> 10;
	A0 = R0.H * R6.L;
6:
	R2.L = (A0 -= R1.L * R6.L) (T);
	W[I1++] = R2.L;
	[P3] = R5;
	LEAVE
	.size _tf_out_fade, . - _tf_out_fade

/* ---- uint32_t tf_recip29(uint32_t d, uint32_t r0, uint32_t b): 2^29 / d for setup(), where r0 = 2^29 >> b < d and
 * the quotient < 2^b (inv1: d 512..16384, b = 21; inv2: 32768 - d 16384..32256, b = 16): libgcc's restoring division
 * (__udivsi3) started b bits from the end, and the dividend's remaining bits are zero, so each step only doubles the
 * remainder; 5 packets a bit instead of 5 for all 32. The quotient bits collect inverted through CC, as in
 * __udivsi3. Only the call-clobbered R0..R3, P0 and LC0 ---- */
	.align 4
	.global _tf_recip29
	.type _tf_recip29, STT_FUNC
_tf_recip29:
	P0 = R2;
	R3 = 0;
	LSETUP (7f, 8f) LC0 = P0;
7:
	R1 = R1 << 1;
	R2 = R1 - R0;
	CC = R1 < R0 (IU);
	IF !CC R1 = R2;
8:
	R3 = ROT R3 BY 1;                                           /* CC = this bit of the quotient, inverted */
	R2 = P0;                                                    /* b */
	R0 = 1;
	R0 <<= R2;
	R0 += -1;
	R0 = R0 - R3;                                               /* 2^b - 1 - the inverted bits */
	RTS;
	.size _tf_recip29, . - _tf_recip29
#endif
