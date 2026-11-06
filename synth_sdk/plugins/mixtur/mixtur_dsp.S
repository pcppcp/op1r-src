/* Mixtur's sample passes hand-scheduled for the Blackfin: the same arithmetic as mixtur_dsp.h's C loops (mixtur.c's,
 * by ultralazr), bit for bit, in fewer instruction packets.
 *
 * void mx_pass1(mx_p1_t *s, int16_t *xbuf)   master phase, dividers, polyBLEP -> the mixture (mx_p1_t in mixtur_dsp.h)
 *   8 packets per sample between events: p += inc[i]; an event (a master wrap at this sample, or one before the
 *   next: the lookahead) when p + inc carried or p + 2 inc does (CC = p < inc, then CC |= AC0 of p + inc);
 *   x = (A' + (p >> 16) B) >>> 17 with A' = A - off (the C's ((v >>> 15) + 0) >>> 2 by nested floors).
 *   Events run out of line (about 65 packets for a wrap, 30 for a lookahead) and return to the loop's last packet:
 *   the four counters in 16-bit lanes (c + 1 against n: the lane's sign is the keep mask), the reset gains as the
 *   sum of all gains minus the kept ones, bh = (c D) >> 17 by 32-bit multiplies, A by four unsigned MACs (the sum
 *   stays under 2^32: gains <= 32640, bh < 2^15), the polyBLEP as the C with 32-bit multiplies.
 *   Gain steps (fade): eight groups of 16 samples, the gains stepped by dg (16-bit lanes; the stepped gains lie in
 *   0 .. 32640) and B, the gain sum and A recomputed after each group (the eighth one's is not used).
 * void mx_pass2(mx_p2_t *s, const int16_t *xbuf, int16_t *out)   two formant SVFs, makeup x velocity, soft knee
 *   42 packets per sample (the C: about 55): both filters' products by dual 16 x 16 MACs (IS), the lp states
 *   through memory in free load / store slots, the constants loaded beside other work. The knee without the
 *   clamp to +-49152 and without a branch: m = min(|y|, KNEE + KNEE_T) (the clamp never changes it), t = max(m -
 *   KNEE, 0), a = m - ((t^2 >> 15) KNEE_A >> 15) (two fractional multiplies with truncation: 2 u v >> 16 = u v >>
 *   15 for 0 <= u, v < 32768), out = sat16(y < 0 ? -a : a) by a saturating shift into the high half (a reaches
 *   32768 at t = 19566, which the C's op1_sat16 makes 32767; -32768 stays). The loop is rotated: its last packet
 *   is the next sample's first MAC with this sample's store.
 * Both save and restore L0 / L1 (zero inside), leave M1 alone, use LC0 (pass 1 also LC1).
 * Host builds assemble it empty: mixtur.c uses the C loops there.
 * Copyright (C) 2026 ultralazr, Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

/* mx_p1_t offsets (P5) and those past 60 (P3 = P5 + 64) */
#define S_P	0
#define S_C01	4
#define S_C23	8
#define S_N01	12
#define S_N23	16
#define S_GQ01	20
#define S_GQ23	24
#define S_DG01	28
#define S_DG23	32
#define S_BH01	36
#define S_BH23	40
#define S_R01	44
#define S_R23	48
#define S_RINV	52
#define S_NSH	56
#define S_GSUM	60
#define T_D0	0
#define T_D1	4
#define T_D2	8
#define T_D3	12
#define T_INC	16
#define T_FADE	20
#define T_ONE2	24
#define T_LOOK	28

/* B = sum (gq_k R_k) >> 17, gsum = sum gq_k, A' = sum gq_k bh_k - (gsum << 14), from the gains in R0 = gq01 and
 * R4 = gq23; R5 = B, R6 = A'; uses R2, R3, A0, A1 (not R1, the next increment) */
.macro GAIN_TERMS
	R5 = [P5 + S_BH01];
	R3 = [P5 + S_BH23];
	A0 = R0.L * R5.L (FU);
	A0 += R0.H * R5.H (FU);
	A0 += R4.L * R3.L (FU) || R2 = [P5 + S_R01];
	R6 = (A0 += R4.H * R3.H) (FU);       /* A < 2^32 */
	R3 = (A1 = R0.H * R2.H), R2 = (A0 = R0.L * R2.L) (FU);
	R2 = R2 >> 17;
	R3 = R3 >> 17;
	R5 = R2 + R3 (NS);
	R2 = [P5 + S_R23];
	R3 = (A1 = R4.H * R2.H), R2 = (A0 = R4.L * R2.L) (FU);
	R2 = R2 >> 17;
	R3 = R3 >> 17;
	R5 = R5 + R2 (NS);
	R5 = R5 + R3 (NS);
	R2 = R0 +|+ R4;                      /* lanes <= 65280: exact as unsigned */
	R3 = R2 >> 16;
	R2 = R2.L (Z);
	R2 = R2 + R3 (NS);
	[P5 + S_GSUM] = R2;
	R2 <<= 14;
	R6 = R6 - R2 (NS);
.endm

	.align 4
	.global _mx_pass1
	.type _mx_pass1, STT_FUNC
_mx_pass1:
	[--SP] = (R7:4, P5:3);
	[--SP] = L0;
	P5 = R0;
	I0 = R1;
	R0 = 0;
	L0 = R0;
	P3 = 64;
	P3 = P3 + P5;
	R0 = [P5 + S_GQ01];
	R4 = [P5 + S_GQ23];
	GAIN_TERMS
	R1 = [P3 + T_FADE];
	CC = R1 == 0;
	R7 = [P5 + S_P];
	P0 = [P3 + T_INC];
	R1 = [P0++];
	R7 = R7 + R1 (NS);                   /* p after sample 0 */
	IF !CC JUMP .Lfade;

	P4.L = .Lsame_e;
	P4.H = .Lsame_e;
	P1 = 128;
	LSETUP (.Lsame_b, .Lsame_e) LC0 = P1;
.Lsame_b:
	CC = R7 < R1 (IU);                   /* wrap at this sample */
	R2 = R7 + R1 (NS);
	CC |= AC0;                           /* or before the next one */
	IF CC JUMP .Levent;
	R3 = R7.H * R5.L (FU) || R1 = [P0++];
	R3 = R3 + R6 (NS);
	R3 = R3 >>> 17;
.Lsame_e:
	R7 = R7 + R1 (NS) || W[I0++] = R3.L;
	JUMP .Ldone1;

.Lfade:
	P4.L = .Lfade_e;
	P4.H = .Lfade_e;
	P2 = 8;
	LSETUP (.Lgroup_b, .Lgroup_e) LC1 = P2;
.Lgroup_b:
	P1 = 16;
	LSETUP (.Lfade_b, .Lfade_e) LC0 = P1;
.Lfade_b:
	CC = R7 < R1 (IU);
	R2 = R7 + R1 (NS);
	CC |= AC0;
	IF CC JUMP .Levent;
	R3 = R7.H * R5.L (FU) || R1 = [P0++];
	R3 = R3 + R6 (NS);
	R3 = R3 >>> 17;
.Lfade_e:
	R7 = R7 + R1 (NS) || W[I0++] = R3.L;
	R0 = [P5 + S_GQ01];                  /* the gains' next step (int16 lanes, no saturation) */
	R2 = [P5 + S_DG01];
	R4 = [P5 + S_GQ23];
	R0 = R0 +|+ R2 || R2 = [P5 + S_DG23];
	R4 = R4 +|+ R2 || [P5 + S_GQ01] = R0;
	[P5 + S_GQ23] = R4;
	GAIN_TERMS
.Lgroup_e:
	R1 = R1;
.Ldone1:
	R7 = R7 - R1 (NS);                   /* the loop's last packet advanced p by the increment after the block */
	[P5 + S_P] = R7;
	L0 = [SP++];
	(R7:4, P5:3) = [SP++];
	RTS;

/* an event at the sample: R7 = p (this sample's), R1 = its increment, R2 = p + increment; P4 = the loop's last
 * packet. Leaves R3 = x and R1 = the next increment. */
.Levent:
	CC = R2 < R1 (IU);                   /* lookahead */
	R0 = CC;
	P1 = R0;
	CC = R7 < R1 (IU);                   /* wrap */
	[P3 + T_LOOK] = R2;
	R4 = 0;
	IF !CC JUMP .Llook;

	/* wrap: c += 1, reset where c reaches n (keep mask = sign of c - n per lane); kept gains */
	R0 = [P5 + S_C01];
	R1 = [P3 + T_ONE2];
	R0 = R0 +|+ R1 || R2 = [P5 + S_N01];
	R2 = R0 -|- R2;
	R2 = R2 >>> 15 (V) || R3 = [P5 + S_GQ01];
	R0 = R0 & R2;
	R3 = R3 & R2;
	[P5 + S_C01] = R0;
	R0 = [P5 + S_C23];
	R0 = R0 +|+ R1 || R2 = [P5 + S_N23];
	R2 = R0 -|- R2;
	R2 = R2 >>> 15 (V) || R4 = [P5 + S_GQ23];
	R0 = R0 & R2;
	R4 = R4 & R2;
	[P5 + S_C23] = R0;
	R3 = R3 +|+ R4;                      /* kept gains, lanes <= 65280 */
	R4 = R3 >> 16;
	R3 = R3.L (Z);
	R3 = R3 + R4 (NS);
	R4 = [P5 + S_GSUM];
	R4 = R4 - R3 (NS);                   /* hg: the gains of the dividers that reset */
	/* blep = ((hg >> 2) sq) >> 14, sq = (e e) >> 15, e = 32768 - min((p >> sh) rinv >> 14, 32767) */
	R0 = [P5 + S_NSH];
	R0 = LSHIFT R7 BY R0.L || R2 = [P5 + S_RINV];
	R0 *= R2;
	R0 = R0 >> 14;
	R2 = 32767;
	R0 = MIN (R0, R2);
	R0 = R2 - R0 (NS);
	R0 += 1;
	R0 *= R0;
	R0 >>>= 15;
	R4 >>>= 2;
	R4 *= R0;
	R4 >>>= 14;
	/* bh = (c D) >> 17 */
	R0 = [P5 + S_C01];
	R3 = [P3 + T_D0];
	R2 = R0.L (Z);
	R2 *= R3;
	R0 = R0 >> 16 || R3 = [P3 + T_D1];
	R0 *= R3;
	R2 = R2 >> 17;
	R0 = R0 >> 17;
	R1 = PACK (R0.L, R2.L) || R3 = [P3 + T_D2];
	R0 = [P5 + S_C23];
	R2 = R0.L (Z);
	R2 *= R3;
	R0 = R0 >> 16 || R3 = [P3 + T_D3];
	R0 *= R3;
	R2 = R2 >> 17;
	R0 = R0 >> 17;
	R3 = PACK (R0.L, R2.L) || [P5 + S_BH01] = R1;
	[P5 + S_BH23] = R3;
	/* A' = sum gq bh - (gsum << 14) */
	R0 = [P5 + S_GQ01];
	R2 = [P5 + S_GQ23];
	A0 = R0.L * R1.L (FU);
	A0 += R0.H * R1.H (FU);
	A0 += R2.L * R3.L (FU);
	R6 = (A0 += R2.H * R3.H) (FU) || R0 = [P5 + S_GSUM];
	R0 <<= 14;
	R6 = R6 - R0 (NS);

.Llook:
	CC = P1 == 0;
	IF CC JUMP .Lx;
	/* lookahead: hg = the gains of the dividers with c + 1 >= n; blep -= ((hg >> 2) sq) >> 14, sq = (d d) >> 15 */
	R0 = [P5 + S_C01];
	R1 = [P3 + T_ONE2];
	R0 = R0 +|+ R1 || R2 = [P5 + S_N01];
	R2 = R0 -|- R2;
	R2 = R2 >>> 15 (V) || R3 = [P5 + S_GQ01];
	R3 = R3 & R2;
	R0 = [P5 + S_C23];
	R0 = R0 +|+ R1 || R2 = [P5 + S_N23];
	R2 = R0 -|- R2;
	R2 = R2 >>> 15 (V) || R0 = [P5 + S_GQ23];
	R0 = R0 & R2;
	R3 = R3 +|+ R0;
	R0 = R3 >> 16;
	R3 = R3.L (Z);
	R3 = R3 + R0 (NS);
	R0 = [P5 + S_GSUM];
	R3 = R0 - R3 (NS);
	R0 = [P5 + S_NSH];
	R2 = [P3 + T_LOOK];
	R0 = LSHIFT R2 BY R0.L || R2 = [P5 + S_RINV];
	R0 *= R2;
	R0 = R0 >> 14;
	R2 = 32767;
	R0 = MIN (R0, R2);
	R0 *= R0;
	R0 >>>= 15;
	R3 >>>= 2;
	R3 *= R0;
	R3 >>>= 14;
	R4 = R4 - R3 (NS);

.Lx:	/* x = ((A' + (p >> 16) B) >>> 15 + blep) >>> 2 */
	R3 = R7.H * R5.L (FU) || R1 = [P0++];
	R3 = R3 + R6 (NS);
	R3 = R3 >>> 15;
	R3 = R3 + R4 (NS);
	R3 = R3 >>> 2;
	JUMP (P4);
	.size _mx_pass1, .-_mx_pass1

/* mx_p2_t offsets */
#define Q_F	0
#define Q_QG	4
#define Q_F2L	8
#define Q_CM	12
#define Q_KN	16
#define Q_ZERO	20
#define Q_KA	24
#define Q_LP1	28
#define Q_LP2	32
#define Q_BP1	36
#define Q_BP2	40

/* Registers: R6 / R7 bp1 / bp2, R5 = b2 | b1 (the halves of bp >> 2) and then x in its low half, R3 the
 * coefficient pair of the next MAC, R0..R4 per sample; I0 xbuf, I1 out, P5 the block. */
	.align 4
	.global _mx_pass2
	.type _mx_pass2, STT_FUNC
_mx_pass2:
	[--SP] = (R7:4, P5:5);
	[--SP] = L0;
	[--SP] = L1;
	P5 = R0;
	I0 = R1;
	I1 = R2;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	R6 = [P5 + Q_BP1];
	R7 = [P5 + Q_BP2];
	R0 = R6 >>> 2;
	R1 = R7 >>> 2;
	R5 = PACK (R1.L, R0.L) || R3 = [P5 + Q_F];
	R0 = (A0 = R5.L * R3.L), R1 = (A1 = R5.H * R3.H) (IS);   /* lp products of sample 0 */
	P1 = 128;
	LSETUP (.Lp2_b, .Lp2_e) LC0 = P1;
.Lp2_b:
	R0 = R0 >>> 11 || R2 = [P5 + Q_LP1];
	R1 = R1 >>> 11 || R4 = [P5 + Q_LP2];
	R2 = R2 + R0 (NS) || R3 = [P5 + Q_QG];       /* lp1 += b1 f1 >> 11 */
	R4 = R4 + R1 (NS) || [P5 + Q_LP1] = R2;      /* lp2 += b2 f2 >> 11 */
	R0 = (A0 = R5.L * R3.L), R1 = (A1 = R5.H * R3.L) (IS) || [P5 + Q_LP2] = R4;   /* b q */
	R0 = R0 >>> 11 || R5.L = W[I0++];
	R1 = R1 >>> 11;
	R0 = R0 + R2 (NS);
	R1 = R1 + R4 (NS);
	R2 = R5.L * R3.L (IS);
	R2 = R2 >>> 12;                              /* in = x q >> 12 */
	R0 = R2 - R0 (NS);                           /* hp1 */
	R1 = R2 - R1 (NS) || R3 = [P5 + Q_F];        /* hp2 */
	R0 = R0 >>> 3;
	R1 = R1 >>> 3;
	R2 = PACK (R1.L, R0.L);
	R0 = (A0 = R2.L * R3.L), R1 = (A1 = R2.H * R3.H) (IS) || R4 = [P5 + Q_F2L];
	R0 = R0 >>> 10;
	R1 = R1 >>> 10;
	R6 = R6 + R0 (NS);                           /* bp1 */
	R7 = R7 + R1 (NS);                           /* bp2 */
	R0 = R6 >>> 2;
	R1 = R7 >>> 2;
	R5 = PACK (R1.L, R0.L);
	R1 = R5.H * R4.L (IS);
	R1 = R1 >>> 15 || R2 = [P5 + Q_QG];
	R1 = R1 + R0 (NS);                           /* y = (bp1 >> 2) + ((bp2 >> 2) F2_LEVEL >> 15) */
	R1 = R1 >>> 1 || R3 = [P5 + Q_CM];
	R1 = R1.L * R2.H (IS) || R4 = [P5 + Q_KN];
	R1 = R1 >>> 8 || R2 = [P5 + Q_ZERO];         /* y = (y >> 1) gain >> 8 */
	R0 = ABS R1;
	R0 = MIN (R0, R3);                           /* m */
	R3 = R0 - R4 (NS);
	R3 = MAX (R3, R2) || R4 = [P5 + Q_KA];       /* t */
	R3.L = R3.L * R3.L (T);
	R3.L = R3.L * R4.L (T);                      /* the knee's correction; R3.H = 0 */
	R0 = R0 - R3 (NS);                           /* a */
	CC = R1 < 0;
	R2 = -R0;
	IF CC R0 = R2;
	R2 = R0 << 16 (S) || R3 = [P5 + Q_F];
.Lp2_e:
	R0 = (A0 = R5.L * R3.L), R1 = (A1 = R5.H * R3.H) (IS) || W[I1++] = R2.H;
	[P5 + Q_BP1] = R6;
	[P5 + Q_BP2] = R7;
	L1 = [SP++];
	L0 = [SP++];
	(R7:4, P5:5) = [SP++];
	RTS;
	.size _mx_pass2, .-_mx_pass2
#endif
