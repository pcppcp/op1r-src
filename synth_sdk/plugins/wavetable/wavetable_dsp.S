/* WAVETABLE's sample loop hand-scheduled for the Blackfin: the same arithmetic as wavetable_dsp.h's wavetable_sample
 * (and its host build), bit for bit, in 62 instruction packets per sample instead of about 170.
 *
 * void wt_block(WtRun *r, int16_t *out, const uint32_t *pitch)
 *   the block's 128 samples with the controls held (wavetable_dsp.h's fast path); r (wavetable_dsp.h WtRun):
 *     [0] ph1, [1] ph2 (updated), [2] y3 << 16 | y2, [3] y1 << 16 | y0 (updated), [4] pa, [5] pb (wt_pairs rows of
 *     the two waves), [6] the first K1, [7] the constants' ring, [8] its length in bytes (WT_RING * 4)
 *   ring words (WT_RING = 13), in the order the loop loads them: CV XF CV CS CA CV CF CA GC CM CV CA K1 (the ring
 *   wraps after each sample; the K1 at its end is the next sample's):
 *     CV = 0x8001 << 16 | 0x090F (-32767 for the soft clip's lower bound; EXTRACT's field: bit 9, 15 bits)
 *     XF = frac << 16 | (frac - 32768) (the crossfade between the waves)
 *     CS = 0x2000 << 16 | drive, CA = 32767 << 16 | 1, CF = 4096 << 16 | k, GC = g << 16 | (32768 - g),
 *     CM = makeup << 16 | level, K1 = k1 >> 3
 *   wt_pairs[w][i] = (w[i + 1] - w[i]) << 16 | w[i] (wavetable_pairs.h): one load per table read.
 *
 * Why it is exact (the C's floors, rebuilt from 16 x 16 products):
 *   a + ((d * f) >> 15): the fractional product 2 d f, its high half truncated, is floor(d f / 2^15); plus a in 16 bits
 *     (the result lies between two table points)
 *   ra + (((rb - ra) * frac) >> 15) = floor((rb frac - ra (frac - 32768)) / 2^15) (ra 2^15 is a whole multiple;
 *     frac - 32768 fits 16 bits for every frac, 0 included)
 *   (o1 + o2) >> 2: (o1 + o2) * 0x2000, fractional, high half
 *   soft(v) after (a * b) >> 12: (a b << 4) saturated, high half = clamp(a b >> 12, -32768, 32767), then MAX with
 *     -32767; soft(x) = (x (98304 - s)) >> 16 = x + ((x (32768 - s)) >> 16), s = (x x) >> 15 (fractional, truncated),
 *     x (32768 - s) = x (32767 - s) + x in the accumulator (integer)
 *   x - ((k y3) >> 12) = -((k y3 - 4096 x) >> 12) (4096 x is a whole multiple), clamped with MAX -32767 then negated
 *   y += (g (u - y)) >> 15 = floor((y (32768 - g) + u g) / 2^15) (fractional, truncated); the y stay in 16 bits (a
 *     floored weighted mean of 16-bit values)
 *   (soft * level) >> 12 clamped to -32768..32767: (product << 4) saturated, high half
 *   dd = ((inc >> 7) * K1) >> 12 with the 32-bit multiply (it wraps as the C's unsigned one does)
 * The A0 / A1 extractions go to low / high halves (even / odd registers for 32 bits), as the MACs require.
 *
 * DAG use: I0 / B0 / L0 the pitch array (circular, so the read one sample ahead stays in it), I1 / B1 / L1 the block
 * (circular: the store runs one sample behind, the first one lands on out[127] and is overwritten at the end), I2 the
 * filter state (r[2], r[3]), I3 / B3 / L3 the ring. The caller's L0..L3 come back on return (normally 0).
 * Host builds (screen and golden harnesses) assemble it empty: wavetable_dsp.h keeps the C loop there.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

/* one sample. In: R4 = this sample's increment, R5 = K1, R6 = the previous sample's output << 16 (stored here),
 * R0 / R1 = phases. Out: the same for the next sample. Free inside: R2, R3 (the filter state lives in r[2..3]). */
.macro WT_SAMPLE last
	R7 = R4 >> 7 || W[I1++] = R6.H || NOP;
	R7 *= R5;
	R7 = R7 >> 12 || R6 = [I3++] || NOP;                   /* CV */
	R5 = R4 + R7, R4 = R4 - R7;                             /* inc + dd, inc - dd */
	R0 = R0 + R4 (NS);
	R1 = R1 + R5 (NS);
	R4 = R0 >> 24;
	R5 = R1 >> 24;
	P2 = R4;
	P4 = R5;
	P3 = P0 + (P2 << 2);
	P2 = P3 + P1;
	P5 = P0 + (P4 << 2);
	P4 = P5 + P1;
	R2 = EXTRACT(R0, R6.L) (Z) || R4 = [P3] || NOP;        /* f1; wa at ph1 */
	R3 = EXTRACT(R1, R6.L) (Z) || R5 = [P5] || NOP;        /* f2; wa at ph2 */
	R6.L = (A0 = R4.H * R2.L) (T) || R7 = [P2] || NOP;     /* wb at ph1 */
	R6.H = (A1 = R5.H * R3.L) (T);
	R6.L = R6.L + R4.L (NS);
	R6.H = R6.H + R5.L (NS) || R4 = [P4] || NOP;           /* ra2 << 16 | ra1; wb at ph2 */
	R2.L = (A0 = R7.H * R2.L) (T) || R5 = [I3++] || NOP;   /* XF */
	R2.H = (A1 = R4.H * R3.L) (T);
	R2.L = R2.L + R7.L (NS) || R3 = [I3++] || NOP;         /* CV */
	R2.H = R2.H + R4.L (NS) || R7 = [I3++] || NOP;         /* rb2 << 16 | rb1; CS */
	A1 = R2.H * R5.H, A0 = R2.L * R5.H;
	R4.H = (A1 -= R6.H * R5.L), R4.L = (A0 -= R6.L * R5.L) (T);                     /* o2 << 16 | o1 */
	A1 = R4.L * R7.H || R5 = [I3++] || NOP;                /* CA */
	R4.H = (A1 += R4.H * R7.H) (T);                         /* (o1 + o2) >> 2 */
	R2 = R4.H * R7.L (IS);                                  /* x drive */
	R2 = R2 << 4 (S) || R7 = [I3++] || NOP;                /* CV */
	R2 = MAX(R2, R3) (V);                                   /* clamped in R2.H */
	R3.L = R2.H * R2.H (T);
	R3.L = R5.H - R3.L (NS);
	A0 = R2.H * R3.L (IS) || R6 = [I3++] || NOP;           /* CF */
	R4 = (A0 += R2.H * R5.L) (IS) || R3 = [I2++] || NOP;   /* y3 << 16 | y2 */
	R4.L = R4.H + R2.H (NS);                                /* x = soft(...) */
	A1 = R3.H * R6.L (IS);
	R5 = (A1 -= R4.L * R6.H) (IS);                          /* k y3 - 4096 x */
	R5 = R5 << 4 (S) || R6 = [I3++] || NOP;                /* CA */
	R5 = MAX(R5, R7) (V);
	R5 = -R5 (V);                                           /* x - (k y3 >> 12), clamped, in R5.H */
	R4.L = R5.H * R5.H (T) || R2 = [I2--] || NOP;          /* y1 << 16 | y0 */
	R4.L = R6.H - R4.L (NS) || R7 = [I3++] || NOP;         /* GC */
	A0 = R5.H * R4.L (IS);
	R4 = (A0 += R5.H * R6.L) (IS);
	R4.L = R4.H + R5.H (NS);                                /* u */
	A1 = R2.H * R7.L, A0 = R2.L * R7.L;
	R2.L = (A0 += R4.L * R7.H) (T);                         /* y0 */
	R2.H = (A1 += R2.L * R7.H) (T);                         /* y1 */
	A1 = R3.H * R7.L, A0 = R3.L * R7.L || R5 = [I3++] || NOP;                        /* CM */
	R3.L = (A0 += R2.H * R7.H) (T) || R6 = [I3++] || NOP;  /* y2; CV */
	R3.H = (A1 += R3.L * R7.H) (T);                         /* y3 */
	R4 = R3.H * R5.H (IS) || [I2++] = R3;                  /* y3 makeup */
	R4 = R4 << 4 (S) || [I2--] = R2 || R7 = [I3++];        /* CA */
	R4 = MAX(R4, R6) (V);
	R6.L = R4.H * R4.H (T);
	R6.L = R7.H - R6.L (NS);
	A0 = R4.H * R6.L (IS);
	R6 = (A0 += R4.H * R7.L) (IS);
	R6.L = R6.H + R4.H (NS);                                /* soft */
	R6 = R6.L * R5.L (IS) || R4 = [I0++] || NOP;           /* x level; the next increment */
\last: R6 = R6 << 4 (S) || R5 = [I3++] || NOP;         /* clamped in R6.H; the next K1 */
.endm

	.global _wt_block
	.type _wt_block, STT_FUNC
_wt_block:
	[--SP] = (R7:4, P5:3);
	[--SP] = FP;
	[--SP] = L0;
	[--SP] = L1;
	[--SP] = L2;
	[--SP] = L3;
	P0 = R0;                                                /* r */
	R3 = [P0 + 20];                                         /* n */
	P5 = R3;
	R3 <<= 2;
	I0 = R2;
	B0 = R2;
	L0 = R3;                                                /* pitch: n words */
	R3 >>= 1;
	L1 = R3;                                                /* out: n halfwords, the first store at out[n - 1] */
	B1 = R1;
	R1 = R1 + R3;
	R1 += -2;
	I1 = R1;
	R3 = 8;
	R3 = R0 + R3;
	I2 = R3;                                                /* r[2]: y3 << 16 | y2, then r[3] */
	R3 = 0;
	L2 = R3;
	R3 = [P0 + 28];
	I3 = R3;
	B3 = R3;
	R3 = [P0 + 32];
	L3 = R3;
	R0 = [P0];                                              /* ph1 */
	R1 = [P0 + 4];                                          /* ph2 */
	R5 = [P0 + 24];                                         /* K1 */
	FP = [P0 + 36];                                         /* the rows per sample, or 0 */
	[--SP] = P0;
	P1 = 1024 (X);                                          /* wb's row follows wa's */
	P0 = [P0 + 16];                                         /* wa's row */
	R4 = [I0++];
	R6 = 0;
	CC = FP == 0;
	IF !CC JUMP .Lwt_ramp;
	LSETUP(.Lwt_s, .Lwt_e) LC0 = P5;
.Lwt_s:
	WT_SAMPLE .Lwt_e
	JUMP.S .Lwt_done;
.Lwt_ramp:
	LSETUP(.Lwt_rs, .Lwt_re) LC0 = P5;
.Lwt_rs:
	P0 = [FP++];
	WT_SAMPLE .Lwt_re
.Lwt_done:
	W[I1++] = R6.H;                                         /* out[n - 1] */
	P0 = [SP++];
	[P0] = R0;
	[P0 + 4] = R1;
	L3 = [SP++];
	L2 = [SP++];
	L1 = [SP++];
	L0 = [SP++];
	FP = [SP++];
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _wt_block, .-_wt_block
/* void wt_prep(WtPrep *p): the ramp's per-sample words for wt_block, n samples from p->control (advanced by
 * p->delta each sample, as the C ramp does); p (wavetable_dsp.h WtPrep): [0] ring (n blocks of WT_RING words whose
 * constant words and halves the caller has filled), [1] rows, [2] control[7], [3] delta[7], [4] n, [5] wt_pairs.
 * Writes the low / high halves: XF (frac - 32768, frac) at +4 / +6, drive at +12, k at +24, 32768 - g / g at +32 /
 * +34, level / makeup at +36 / +38, the next sample's k1 >> 3 at +48; rows[i] = wt_pairs[position >> 15].
 * c0, c2, c5 and their deltas in R0..R5, c1, c3, c4, c6 in I0..I3 (deltas in M0..M3, the caller's M1 kept). */
	.global _wt_prep
	.type _wt_prep, STT_FUNC
_wt_prep:
	[--SP] = (R7:4, P5:3);
	[--SP] = M1;
	[--SP] = L0;
	[--SP] = L1;
	[--SP] = L2;
	[--SP] = L3;
	P5 = R0;
	R3 = 0;
	L0 = R3;
	L1 = R3;
	L2 = R3;
	L3 = R3;
	P0 = [P5];                                              /* ring */
	P2 = [P5 + 4];                                          /* rows */
	P3 = [P5 + 8];                                          /* control */
	P4 = [P5 + 12];                                         /* delta */
	R6 = [P5 + 20];                                         /* wt_pairs */
	P5 = [P5 + 16];                                         /* n */
	R0 = [P3];
	R1 = [P3 + 4];
	I0 = R1;
	R1 = [P3 + 8];
	R2 = [P3 + 12];
	I1 = R2;
	R2 = [P3 + 16];
	I2 = R2;
	R2 = [P3 + 20];
	R3 = [P3 + 24];
	I3 = R3;
	R3 = [P4 + 4];
	M0 = R3;
	R3 = [P4 + 12];
	M1 = R3;
	R3 = [P4 + 16];
	M2 = R3;
	R3 = [P4 + 24];
	M3 = R3;
	R3 = [P4];
	R4 = [P4 + 8];
	R5 = [P4 + 20];
	P1 = 52 (X);                                              /* WT_RING words */
	LSETUP(.Lwp_s, .Lwp_e) LC0 = P5;
.Lwp_s:
	R7 = R0 >>> 7;
	BITCLR(R7, 15);
	W[P0 + 6] = R7;
	BITSET(R7, 15);
	W[P0 + 4] = R7;
	R7 = R0 >>> 22;
	R7 <<= 10;
	R7 = R7 + R6;
	[P2++] = R7;
	R0 = R0 + R3;
	R7 = R1 >>> 7;
	W[P0 + 34] = R7;
	R7 = -R7;
	BITTGL(R7, 15);
	W[P0 + 32] = R7;
	R1 = R1 + R4;
	R7 = R2 >>> 7;
	W[P0 + 38] = R7;
	R2 = R2 + R5;
	R7 = I3;
	R7 >>>= 7;
	W[P0 + 36] = R7;
	R7 = I2;
	R7 >>>= 7;
	W[P0 + 12] = R7;
	R7 = I1;
	R7 >>>= 7;
	W[P0 + 24] = R7;
	I0 += M0;
	I1 += M1;
	I2 += M2;
	I3 += M3;
	R7 = I0;
	R7 >>>= 10;
	W[P0 + 48] = R7;
.Lwp_e:
	P0 = P0 + P1;
	[P3] = R0;                                              /* the controls where the next chunk starts */
	[P3 + 8] = R1;
	[P3 + 20] = R2;
	R7 = I0;
	[P3 + 4] = R7;
	R7 = I1;
	[P3 + 12] = R7;
	R7 = I2;
	[P3 + 16] = R7;
	R7 = I3;
	[P3 + 24] = R7;
	L3 = [SP++];
	L2 = [SP++];
	L1 = [SP++];
	L0 = [SP++];
	M1 = [SP++];
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _wt_prep, .-_wt_prep
#endif
