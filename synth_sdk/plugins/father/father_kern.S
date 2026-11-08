/* Father's inner loops, hand-scheduled for the Blackfin: the same arithmetic as father_dsp.c's C loops (the host
 * build's), bit for bit (kern_check.py), in fewer packets. The rings are read and written in circles (I0..I3 with B / L),
 * so no index wraps by hand.
 *
 * void fa_tank_write(const fa_tw_t *t)   t = { i[4], b[4], lb[4], rows, in, k02, k46, count } (father_dsp.c)
 *   one Hadamard group's write-back (tank_write): the four destination lines g, g + 2, g + 4, g + 6 as circles (base b,
 *   length lb bytes), i[0], i[1] their heads, i[2], i[3] one sample before theirs; rows the group's pairs (line 1 | 0,
 *   3 | 2 per sample), in the group's input, k02 = k[g + 2] << 16 | k[g], k46 = k[g + 6] << 16 | k[g + 4]. Per sample:
 *   the Hadamard sums in 32 bits (dual add / subtract), sat(half(x)) as (x - (x >>> 31)) << 15 with saturation (the
 *   high half holds it), then two lines per dual MAC: mul()'s round toward zero is a bias of 32767 for a negative
 *   product (s >>> 15 times -32767), the fractional products and truncating extraction give (v + bias) >> 15 in a half,
 *   and the saturating 16-bit add of (ni | in) is sat(... + in). The last two lines are stored one sample behind; the
 *   first such store writes back the word before their heads unchanged. 31 packets per sample.
 *
 * int32_t fa_line_read(const fa_tr_t *t)   t = { p, b, lb, row, frs, keeps, damp, pairs } (father_dsp.c)
 *   one tank line (tank_read): 2 pairs (MW_N 16, CLASSIC 32) samples from the circle (base b, lb bytes), p at p[-1] of the first sample; per
 *   sample v = (p0 (256 - fr) + p[-1] fr) >> 8 (frs = fr << 16 | 256 - fr: the C's p0 + ((p[-1] - p0) fr >> 8)), then
 *   damp = (v (32768 - keep) + damp keep + bias) >> 15 (keeps = keep << 16 | 32768 - keep, bias 32767 where damp < v:
 *   the C's v + mul(damp - v, keep)); damp to row[4 n] (the pairs' stride), returned.
 *
 * void fa_out(out_t *m)   m = { l, r, wl, wr, mix, step, peak, gain, mode, close_rate, pairs, open_add, open_shift }
 *   the 44,100 Hz output (out): per sample the wet pair (wr | wl) from the two rows; with mode 1 / 2 the gate gain
 *   opens / closes per sample (as the C, whose other branch cannot run for that target) and multiplies the wet pair
 *   (mul()); then sat(a + mul(w - a, wet)) as one fractional sum of a 32768 - a wet + w wet + bias. The pairs
 *   (out_r | out_l) go to pairs[], then a second pass stores r before l (so a mono call, r = l, keeps out_l) and takes
 *   the peak from the halves' maximum and minimum. m->mix, peak and gain are written back.
 *
 * int32_t fa_early(const fa_er_t *t)   t = { i[4], a, b, lb, out, coef, e, count } (father_dsp.c)
 *   one channel's early filter (early): x = (a[t0] >> 2) + (b[t1] >> 3) - (a[t2] >> 4) + (b[t3] >> 4), the taps from
 *   i[0..3] onwards (the rings a, b of lb bytes read in circles), e += mul(x - e, damping) as one fractional sum
 *   e (32768 - damping) + x damping + bias, coef = (32768 - damping) << 16 | damping. count (MW_IN 32, CLASSIC 64) samples to out; returns e.
 *   10 packets per sample.
 *
 * void fa_decim(const fa_dc_t *t)   t = { p, b, lb, hist, out, quads } (father_dsp.c)
 *   a 2:1 half-band (decimate): the input to 22,050 Hz (64 samples from the call's buffer as a circle of 128 bytes) or
 *   the predelay tap to the tank rate (32, CLASSIC 64, from p in the circle, base b, lb bytes); 8 quads inputs after hist[0..4],
 *   the last five samples of the previous tap; y[k] = sat((9 (x[2k-3] + x[2k-1]) + 16 x[2k-2] - x[2k-5] - x[2k+1]) >> 5)
 *   as one fractional sum of 1024 times the coefficients with a truncating, saturating extraction; the pairs
 *   (x[2k+1] | x[2k]) rotate through R0..R3. 4 quads samples to out; the last five inputs back to hist. 6 packets per sample.
 * void fa_ap2(const fa_ap_t *t)   t = { x, ia, ba, lba, ib, bb, lbb, pairs } (father_dsp.c)
 *   the two input allpasses of one channel (allpass), in place on x[0..2 pairs - 1]: the rings A, B are circles read and written
 *   at the same index (a ring is as long as its delay), two samples per pass in 16-bit halves: half() is
 *   (d - (d >>> 15)) >>> 1 per half, sat(x -/+ half()) the saturating 16-bit subtract / add.
 *
 * void fa_late(const fa_lt_t *t)   t = { r0, r1, e, up, count } (father_dsp.c, count tank samples)
 *   one channel's late sum (late): per tank sample t = r0[0] + r0[2] - r1[0] - r1[2] (the channel's lines in the two
 *   groups' rows, 8 bytes per sample), x = half(t + half(t)), the x2 interpolation (-1 9 9 -1)/16 from up[0..2] and
 *   x, each output added to e[] in place with 16-bit saturation (a saturating shift: the high half). up written back.
 *
 * L0..L3 are zero on return (father.c keeps the caller's values around mw_process, as MBIRA does).
 * Host builds (screen and golden harnesses) assemble it empty: father_dsp.c keeps the C loops there.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

	.align 4
	.global _fa_tank_write
	.type _fa_tank_write, STT_FUNC
_fa_tank_write:
	[--SP] = (R7:4, P5:3);
	SP += -8;                            /* k02, k46 */
	P5 = R0;
	R0 = [P5 + 0];   I0 = R0;
	R0 = [P5 + 4];   I1 = R0;
	R0 = [P5 + 8];   I2 = R0;
	R0 = [P5 + 12];  I3 = R0;
	R0 = [P5 + 16];  B0 = R0;
	R0 = [P5 + 20];  B1 = R0;
	R0 = [P5 + 24];  B2 = R0;
	R0 = [P5 + 28];  B3 = R0;
	R0 = [P5 + 32];  L0 = R0;
	R0 = [P5 + 36];  L1 = R0;
	R0 = [P5 + 40];  L2 = R0;
	R0 = [P5 + 44];  L3 = R0;
	P0 = [P5 + 48];                      /* rows: (line 1 | 0), (3 | 2) per sample */
	P4 = [P5 + 52];                      /* in */
	R0 = [P5 + 56];
	[SP + 0] = R0;                       /* k2 | k0 */
	R0 = [P5 + 60];
	[SP + 4] = R0;                       /* k6 | k4 */
	R4.L = W[I2];                        /* the words before the last two heads, stored back unchanged first */
	R4.H = W[I3];
	R7.H = 16384;
	R7.L = -32767;
	R2 = [P0++];
	R3 = [P0++];
	R0 = W[P4++] (X);
	P5 = [P5 + 64];                      /* count: MW_N (CLASSIC MW_NC) */
	LSETUP (.Ltw_b, .Ltw_e) LC0 = P5;
.Ltw_b:
	R1 = -R0 (NS) || NOP || W[I2++] = R4.L;
	R1 = R1 >>> 2 || NOP || W[I3++] = R4.H;
	R0 = R0 >>> 2 || R5 = [SP + 0] || NOP;
	R6 = PACK(R1.L, R0.L);               /* (-input) >> 2 | input >> 2 */
	A1 = R2.L * R7.H, A0 = R2.L * R7.H;  /* 32768 x2 | 32768 x0 */
	A1 -= R2.H * R7.H, A0 += R2.H * R7.H;
	A1 += R3.L * R7.H, A0 += R3.L * R7.H;
	R1 = (A1 -= R3.H * R7.H), R0 = (A0 += R3.H * R7.H);
	R0 = PACK(R1.H, R0.H);
	R0 = R0 >>> 15 (V);                  /* -1 where x < 0 */
	R4.H = (A1 += R0.H * R7.L), R4.L = (A0 += R0.L * R7.L) (T);   /* sat(half(x)): s2 | s0 */
	R0 = R4 >>> 15 (V);
	A1 = R4.H * R5.H, A0 = R4.L * R5.L;
	R4.H = (A1 += R0.H * R7.L), R4.L = (A0 += R0.L * R7.L) (T) || R5 = [SP + 4] || NOP;   /* mul(s, k) */
	R4 = R4 +|+ R6 (S);
	A1 = R2.L * R7.H, A0 = R2.L * R7.H || NOP || W[I0++] = R4.L;   /* 32768 x6 | 32768 x4 */
	A1 -= R2.H * R7.H, A0 += R2.H * R7.H || NOP || W[I1++] = R4.H;
	A1 -= R3.L * R7.H, A0 -= R3.L * R7.H || R2 = [P0++] || NOP;
	R1 = (A1 += R3.H * R7.H), R0 = (A0 -= R3.H * R7.H);
	R0 = PACK(R1.H, R0.H) || R3 = [P0++] || NOP;
	R0 = R0 >>> 15 (V);
	R4.H = (A1 += R0.H * R7.L), R4.L = (A0 += R0.L * R7.L) (T);   /* s6 | s4 */
	R0 = R4 >>> 15 (V);
	A1 = R4.H * R5.H, A0 = R4.L * R5.L;
	R4.H = (A1 += R0.H * R7.L), R4.L = (A0 += R0.L * R7.L) (T);
	R4 = R4 +|+ R6 (S);
.Ltw_e:
	R0 = W[P4++] (X);
	W[I2++] = R4.L;
	W[I3++] = R4.H;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	L2 = R0;
	L3 = R0;
	SP += 8;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _fa_tank_write, .-_fa_tank_write
	.align 4
	.global _fa_line_read
	.type _fa_line_read, STT_FUNC
_fa_line_read:
	[--SP] = (R7:4);
	P0 = R0;
	R0 = [P0 + 0];   I0 = R0;
	R0 = [P0 + 4];   B0 = R0;
	R0 = [P0 + 8];   L0 = R0;
	P1 = [P0 + 12];                      /* row, every 8 bytes */
	R5 = [P0 + 16];                      /* fr | 256 - fr */
	R4 = [P0 + 20];                      /* keep | 32768 - keep */
	R6 = [P0 + 24];                      /* damp */
	R7 = -32767;
	P2 = 8;
	R1.L = W[I0++];                      /* p[-1] */
	R0.L = W[I0++];                      /* p0 */
	P0 = [P0 + 28];                      /* pairs: MW_N / 2 (CLASSIC MW_NC / 2) */
	LSETUP (.Llr_b, .Llr_e) LC0 = P0;
.Llr_b:                                  /* two samples: p[-1] / p0 alternate between R1 and R0 */
	A0 = R0.L * R5.L (IS);
	R2 = (A0 += R1.L * R5.H) (IS) || R1.L = W[I0++];
	R2 >>>= 8;                           /* v */
	R3 = R6 - R2 (NS);
	R3 >>>= 31;                          /* -1 where damp < v */
	A1 = R2.L * R4.L (IS);
	A1 += R6.L * R4.H (IS);
	R3 = (A1 += R3.L * R7.L) (IS);
	R6 = R3 >>> 15;                      /* damp */
	W[P1++P2] = R6.L;
	A0 = R1.L * R5.L (IS);
	R2 = (A0 += R0.L * R5.H) (IS) || R0.L = W[I0++];
	R2 >>>= 8;
	R3 = R6 - R2 (NS);
	R3 >>>= 31;
	A1 = R2.L * R4.L (IS);
	A1 += R6.L * R4.H (IS);
	R3 = (A1 += R3.L * R7.L) (IS);
	R6 = R3 >>> 15;
.Llr_e:
	W[P1++P2] = R6.L;
	R0 = 0;
	L0 = R0;
	R0 = R6;
	(R7:4) = [SP++];
	RTS;
	.size _fa_line_read, .-_fa_line_read
/* fa_out: R7 = 16384 | -32767; P0 / P1 wl / wr, P2 / P3 l / r (P4 = 2), I2 the pairs out */
#define FM_MIX 0
#define FM_STEP 4
#define FM_GAIN 8
#define FM_RATE 12
#define FM_ARG 16
.macro MIX_LOAD
	R5.L = W[P0++P4];
	R5.H = W[P1++P4];
	R4.L = W[P2++P4];
	R4.H = W[P3++P4];
	R1 = [SP + FM_MIX];
	R2 = [SP + FM_STEP];
.endm
.macro MIX_OUT
	R1 = R1 + R2 (NS);
	R0 = R1 >>> 6 || [SP + FM_MIX] = R1 || NOP;   /* wet */
	R3 = R5 -|- R4 (S);
	R3 = R3 >>> 15 (V);                  /* -1 where w < a */
	A1 = R4.H * R7.H, A0 = R4.L * R7.H;
	A1 += R4.H * R7.H, A0 += R4.L * R7.H;
	A1 -= R4.H * R0.L, A0 -= R4.L * R0.L;
	A1 += R5.H * R0.L, A0 += R5.L * R0.L;
	R4.H = (A1 += R3.H * R7.L), R4.L = (A0 += R3.L * R7.L) (T);
.endm
/* w = mul(w, gain) for both halves, the gain in R2.L */
.macro MIX_GATED
	R3 = R5 >>> 15 (V);
	A1 = R5.H * R2.L, A0 = R5.L * R2.L;
	R5.H = (A1 += R3.H * R7.L), R5.L = (A0 += R3.L * R7.L) (T);
.endm
	.align 4
	.global _fa_out
	.type _fa_out, STT_FUNC
_fa_out:
	[--SP] = (R7:4, P5:3);
	SP += -20;
	[SP + FM_ARG] = R0;
	P5 = R0;
	R1 = [P5 + 16];
	[SP + FM_MIX] = R1;
	R1 = [P5 + 20];
	[SP + FM_STEP] = R1;
	R1 = [P5 + 28];
	[SP + FM_GAIN] = R1;
	R1 = [P5 + 36];
	[SP + FM_RATE] = R1;
	R6 = [P5 + 32];                      /* mode */
	P2 = [P5];
	P3 = [P5 + 4];
	P0 = [P5 + 8];
	P1 = [P5 + 12];
	R1 = [P5 + 40];
	I2 = R1;                             /* the pairs */
	R1 = 0;
	L2 = R1;
	P4 = 2;
	R7.H = 16384;
	R7.L = -32767;
	R1 = 64;
	LC0 = R1;
	CC = R6 == 0;
	IF !CC JUMP .Lfm_gate;
	LSETUP (.Lfm_b, .Lfm_e) LC0;
.Lfm_b:
	MIX_LOAD
	MIX_OUT
.Lfm_e:
	[I2++] = R4;
	JUMP .Lfm_pass;
.Lfm_gate:
	CC = R6 == 1;
	IF !CC JUMP .Lfm_close;
	R6 = [P5 + 48];                      /* open_shift */
	P5 = [P5 + 44];                      /* open_add */
	LSETUP (.Lfo_b, .Lfo_e) LC0;
.Lfo_b:                                  /* opening: gain += (open_add - gain) >> open_shift (32767 + 63, 6; CLASSIC + 31, 5) */
	MIX_LOAD
	R2 = [SP + FM_GAIN];
	R3 = P5;
	R3 = R3 - R2;
	R3 >>>= R6;
	R2 = R2 + R3;
	[SP + FM_GAIN] = R2;
	MIX_GATED
	R2 = [SP + FM_STEP];
	MIX_OUT
.Lfo_e:
	[I2++] = R4;
	JUMP .Lfm_pass;
.Lfm_close:
	LSETUP (.Lfc_b, .Lfc_e) LC0;
.Lfc_b:                                  /* closing: gain -= (gain close_rate + 32767) >> 15 */
	MIX_LOAD
	R2 = [SP + FM_GAIN];
	R3 = [SP + FM_RATE];
	R3 = R2.L * R3.L (IS);
	R0 = 32767 (Z);
	R3 = R3 + R0;
	R3 >>>= 15;
	R2 = R2 - R3;
	[SP + FM_GAIN] = R2;
	MIX_GATED
	R2 = [SP + FM_STEP];
	MIX_OUT
.Lfc_e:
	[I2++] = R4;
.Lfm_pass:
	R0 = [SP + FM_ARG];
	P0 = R0;
	R1 = [P0 + 40];
	I2 = R1;
	P2 = [P0];
	P3 = [P0 + 4];
	R6 = 0;
	R7 = 0;
	R0 = [I2++];
	R1 = 32;
	LC0 = R1;
	LSETUP (.Lfp_b, .Lfp_e) LC0;
.Lfp_b:                                  /* r before l: a mono call (r = l) keeps out_l */
	R6 = MAX (R6, R0) (V) || W[P3++P4] = R0.H || R1 = [I2++];
	R7 = MIN (R7, R0) (V) || W[P2++] = R0 || NOP;
	R6 = MAX (R6, R1) (V) || W[P3++P4] = R1.H || R0 = [I2++];
.Lfp_e:
	R7 = MIN (R7, R1) (V) || W[P2++] = R1 || NOP;
	R0 = R6.L (X);
	R1 = R6 >>> 16;
	R0 = MAX (R0, R1);
	R1 = R7.L (X);
	R1 = -R1;
	R0 = MAX (R0, R1);
	R1 = R7 >>> 16;
	R1 = -R1;
	R0 = MAX (R0, R1);
	R1 = [SP + FM_ARG];
	P0 = R1;
	[P0 + 24] = R0;                      /* peak */
	R1 = [SP + FM_MIX];
	[P0 + 16] = R1;
	R1 = [SP + FM_GAIN];
	[P0 + 28] = R1;
	SP += 20;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _fa_out, .-_fa_out
	.align 4
	.global _fa_early
	.type _fa_early, STT_FUNC
_fa_early:
	[--SP] = (R7:4, P5:3);
	P2 = R0;
	R0 = [P2 + 16];  B0 = R0;  B2 = R0;   /* a */
	R0 = [P2 + 20];  B1 = R0;  B3 = R0;   /* b */
	R0 = [P2 + 24];  L0 = R0;  L1 = R0;  L2 = R0;  L3 = R0;
	R0 = [P2 + 0];   I0 = R0;
	R0 = [P2 + 4];   I1 = R0;
	R0 = [P2 + 8];   I2 = R0;
	R0 = [P2 + 12];  I3 = R0;
	P0 = [P2 + 28];                      /* out */
	R7 = [P2 + 32];                      /* coef */
	R5 = [P2 + 36];                      /* e */
	R6 = -32767 (X);
	P5 = 2;
	R0.L = W[I0++];
	R0.H = W[I1++];
	R1.L = W[I2++];
	R1.H = W[I3++];
	R0 = R0 >>> 2 (V);
	P1 = [P2 + 40];                      /* count: MW_IN (CLASSIC 64) */
	LSETUP (.Lfe_b, .Lfe_e) LC0 = P1;
.Lfe_b:
	R0.H = R0.H >>> 1;                   /* a0 >> 2 | b1 >> 3 */
	R1 = R1 >>> 4 (V);
	R2 = R0 +|- R1;
	R2.L = R2.L + R2.H (NS) || R0.L = W[I0++] || R0.H = W[I1++];   /* x */
	R3.L = R2.L - R5.L (NS) || R1.L = W[I2++] || R1.H = W[I3++];
	R3.L = R3.L >>> 15;                  /* -1 when x < e */
	A0 = R5.L * R7.H;
	A0 += R2.L * R7.L;
	R5.L = (A0 += R3.L * R6.L) (T);      /* e */
.Lfe_e:
	R0 = R0 >>> 2 (V) || W[P0++P5] = R5.L || NOP;
	R0 = R5.L (X);
	R1 = 0;
	L0 = R1;
	L1 = R1;
	L2 = R1;
	L3 = R1;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _fa_early, .-_fa_early
	.align 4
	.global _fa_decim
	.type _fa_decim, STT_FUNC
_fa_decim:
	[--SP] = (R7:4);
	P0 = R0;
	R0 = [P0 + 0];   I0 = R0;
	R0 = [P0 + 4];   B0 = R0;
	R0 = [P0 + 8];   L0 = R0;
	P2 = [P0 + 12];                      /* hist */
	P1 = [P0 + 16];                      /* out */
	R5 = [P0 + 20];                      /* outputs / 4 */
	R0 = [P2 + 0];
	R1 = PACK(R0.L, R0.L);               /* x[-5] | . */
	R0 = [P2 + 4];
	R4 = [P2 + 8];
	R2 = PACK(R4.L, R0.L);               /* x[-3] | x[-4] */
	R0 = [P2 + 12];
	R4 = [P2 + 16];
	R3 = PACK(R4.L, R0.L);               /* x[-1] | x[-2] */
	R7.H = 9216;
	R7.L = 16384;
	R6 = 1024 (Z);
	P0 = R5;
	LSETUP (.Ldc_b, .Ldc_e) LC0 = P0;
.Ldc_b:
	A0 = R3.L * R7.L || R0.L = W[I0++] || NOP;
	A0 += R3.H * R7.H || R0.H = W[I0++] || NOP;
	A0 += R2.H * R7.H;
	A0 -= R1.H * R6.L;
	R4.L = (A0 -= R0.H * R6.L) (T);
	W[P1++] = R4;
	A0 = R0.L * R7.L || R1.L = W[I0++] || NOP;
	A0 += R0.H * R7.H || R1.H = W[I0++] || NOP;
	A0 += R3.H * R7.H;
	A0 -= R2.H * R6.L;
	R4.L = (A0 -= R1.H * R6.L) (T);
	W[P1++] = R4;
	A0 = R1.L * R7.L || R2.L = W[I0++] || NOP;
	A0 += R1.H * R7.H || R2.H = W[I0++] || NOP;
	A0 += R0.H * R7.H;
	A0 -= R3.H * R6.L;
	R4.L = (A0 -= R2.H * R6.L) (T);
	W[P1++] = R4;
	A0 = R2.L * R7.L || R3.L = W[I0++] || NOP;
	A0 += R2.H * R7.H || R3.H = W[I0++] || NOP;
	A0 += R1.H * R7.H;
	A0 -= R0.H * R6.L;
	R4.L = (A0 -= R3.H * R6.L) (T);
.Ldc_e:
	W[P1++] = R4;
	R0 = R1 >>> 16;  [P2 + 0] = R0;      /* the last five inputs */
	R0 = R2.L (X);   [P2 + 4] = R0;
	R0 = R2 >>> 16;  [P2 + 8] = R0;
	R0 = R3.L (X);   [P2 + 12] = R0;
	R0 = R3 >>> 16;  [P2 + 16] = R0;
	R0 = 0;
	L0 = R0;
	(R7:4) = [SP++];
	RTS;
	.size _fa_decim, .-_fa_decim

/* half() of both halves of \x into \h, \t a temporary */
.macro HALF2 h, x, t
	\t = \x >>> 15 (V);
	\h = \x -|- \t;
	\h = \h >>> 1 (V);
.endm
	.align 4
	.global _fa_ap2
	.type _fa_ap2, STT_FUNC
_fa_ap2:
	[--SP] = (R7:4);
	P2 = R0;
	P0 = [P2 + 0];                       /* x, read */
	P1 = P0;                             /* x, written */
	R0 = [P2 + 4];   I0 = R0;  I1 = R0;  /* A: read, then written in place */
	R0 = [P2 + 8];   B0 = R0;  B1 = R0;
	R0 = [P2 + 12];  L0 = R0;  L1 = R0;
	R0 = [P2 + 16];  I2 = R0;  I3 = R0;  /* B */
	R0 = [P2 + 20];  B2 = R0;  B3 = R0;
	R0 = [P2 + 24];  L2 = R0;  L3 = R0;
	P0 = [P2 + 28];                      /* pairs: MW_N / 2 (CLASSIC MW_NC / 2) */
	LSETUP (.Lap_b, .Lap_e) LC0 = P0;
.Lap_b:                                  /* two samples */
	R0 = [P1];
	R1.L = W[I0++];
	R1.H = W[I0++];                      /* d */
	HALF2 R3, R1, R2
	R4 = R0 -|- R3 (S);                  /* v = sat(x - half(d)) */
	HALF2 R3, R4, R2
	W[I1++] = R4.L;
	W[I1++] = R4.H;
	R1 = R1 +|+ R3 (S);                  /* y = sat(d + half(v)) */
	R5.L = W[I2++];
	R5.H = W[I2++];                      /* e */
	HALF2 R3, R5, R2
	R4 = R1 -|- R3 (S);                  /* B: v = sat(y - half(e)) */
	HALF2 R3, R4, R2
	W[I3++] = R4.L;
	W[I3++] = R4.H;
	R5 = R5 +|+ R3 (S);                  /* z = sat(e + half(v)) */
.Lap_e:
	[P1++] = R5;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	L2 = R0;
	L3 = R0;
	(R7:4) = [SP++];
	RTS;
	.size _fa_ap2, .-_fa_ap2
	.align 4
	.global _fa_late
	.type _fa_late, STT_FUNC
_fa_late:
	[--SP] = (R7:4, P5:3);
	P3 = R0;
	R0 = [P3 + 16];                      /* count: MW_N (CLASSIC MW_NC) */
	P0 = [P3 + 0];                       /* this channel's lines 0, 2 of group 0 (8 bytes a sample) */
	P1 = [P3 + 4];                       /* lines 4, 6 */
	P2 = [P3 + 8];                       /* e, in place */
	I0 = P2;                             /* e, read */
	P3 = [P3 + 12];                      /* up */
	R4 = [P3 + 0];
	R5 = [P3 + 4];
	R6 = [P3 + 8];
	P5 = 8;
	P4 = 2;
	LC0 = R0;
	LSETUP (.Llt_b, .Llt_e) LC0;
.Llt_b:
	R0 = W[P0 + 4] (X);
	R1 = W[P0 ++ P5] (X);
	R2 = W[P1 + 4] (X);
	R3 = W[P1 ++ P5] (X);
	R0 = R0 + R1;
	R2 = R2 + R3;
	R0 = R0 - R2;                        /* t */
	R1 = R0 >> 31;
	R1 = R0 + R1;
	R1 >>>= 1;                           /* half(t) */
	R1 = R1 + R0;
	R3 = R1 >> 31;
	R1 = R1 + R3;
	R1 >>>= 1;                           /* x */
	R2 = R5 + R6;
	R3 = R2 << 3;
	R2 = R2 + R3;                        /* 9 (u1 + u2) */
	R3 = R4 + R1;
	R2 = R2 - R3;
	R2 >>>= 4;                           /* the odd output */
	R3.L = W[I0++];
	R3 = R3.L (X);
	R3 = R3 + R5;                        /* the even output (u1) + e */
	R3 = R3 << 16 (S);
	R7.L = W[I0++];
	R7 = R7.L (X);
	R7 = R7 + R2;
	R7 = R7 << 16 (S);
	W[P2 ++ P4] = R3.H;
	W[P2 ++ P4] = R7.H;
	R4 = R5;
	R5 = R6;
.Llt_e:
	R6 = R1;
	[P3 + 0] = R4;
	[P3 + 4] = R5;
	[P3 + 8] = R6;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _fa_late, .-_fa_late
#endif
