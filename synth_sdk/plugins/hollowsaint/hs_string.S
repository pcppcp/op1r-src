/* kennel: Hollow Saint's melody string (hollow_saint.c string_run), hand-scheduled for the Blackfin: the same samples
 * as the C loop (kept there for host builds and as the reference, HS_C_REF), bit for bit.
 *
 * void hs_string_plain(int32_t *k), hs_string_f, hs_string_r, hs_string_fr (f: the hollow sine pair, r: the grit)
 *   k: hollow_saint.c's string_asm() block (HK_* there, byte offsets below); the state goes in and comes back there.
 *
 * Two passes over the 64 samples (8 sub-blocks of 8), each with its own registers:
 *   pass 1, the source: phase, two polyBLEP saws, bow noise with the slip, [the sine pair], [the grit] -> raw.
 *     plain / r: raw fits int16 (|saw ga| + |saw2 gp| + |nz gn| <= 21852 + 9008 + 550), kept as halfwords;
 *     f / fr: raw + the sine pair's term (<= 19671) does not, so 32-bit words.
 *   pass 2, the filters: the tone one-pole and the nasal formant SVF -> hb (the same for all four).
 * Packets per sample: pass 1 plain 18, r 21, f 31, fr 34; pass 2 19.
 *
 * The phase is held XOR 0x80000000 (phx): its signed high half is the saw's plain ramp (ph >> 16) - 32768, the second
 * saw's is that of phx - pw, ph < pold is phx < phxold (signed), ph >= 2^30 is phx >= -2^30, and the sine pair reads
 * table[4096 + (phx >>> 19)] and table[4096 + ((3 phx) >>> 19)] (3 phx = (3 ph) ^ 0x80000000). The LCG state is held
 * XOR 0x80000000 too (the same recurrence, since the multiplier is odd): nz = r >>> 17.
 * The polyBLEP's test in the loop is a superset of the C's: the two ramps s packed in one register, |s| >= c with
 * c = 32768 - 2 dt (t < dt or t > 32767 - dt is s < -c or s >= c) in either half (k[CM] holds c - 1 in both halves,
 * c - 1 - |s| negative); out of line the C's exact tests and corrections, on both saws.
 * 16 x 16 products only where both operands are int16 by construction (the gains, the saws, the noise, the table);
 * the filters' products are 32-bit multiplies (they wrap as the C's int32 products do). (T): a fractional product
 * truncated to the high half is (a * b) >> 15, floored as the C's shift.
 * No packet stores a register another of its ops writes, or loads a register another of its ops reads; no shift
 * beside a P-register store (anomaly 05000074). L0..L3 are saved, zeroed (linear I addressing) and restored; M1 (and
 * every M register) untouched.
 * Host builds assemble it empty: hollow_saint.c keeps the C loop there.
 * Copyright (C) 2026 the hollow_saint authors. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and
 * NOTICE of the kennel SDK).
 */
#ifdef __bfin__

/* k: byte offsets (hollow_saint.c HK_* = these / 4). 0 .. 60 are read in parallel slots ([P5 + uimm6m4]) */
#define K_DOFF   0
#define K_PW     4
#define K_CM     8
#define K_LA     12
#define K_LC     16
#define K_K30    20
#define K_AM3    24
#define K_AM1    28
#define K_MJIT   32
#define K_FF     36
#define K_FQ     40
#define K_TA     44
#define K_PHOLD  48
#define K_NB     52       /* HS_BOW_CHEAP: the pair's second noise product (high half), next to PHOLD (I2) */
#define K_KF     56
#define K_PH     64
#define K_RNG    68
#define K_TONE   72
#define K_FLP    76
#define K_FBP    80
#define K_GRIT   84
#define K_INC    88
#define K_RAW    92
#define K_HB     96
#define K_SINE   100
#define K_DT     104
#define K_INV8   108
#define K_NIN1   112      /* pass 1's inner count: 8 (4 at 11.025 kHz), halved with HS_BOW_CHEAP (pairs) */
#define K_NIN2   116      /* pass 2's: 8 or 4 */
#define K_HALF   120      /* HS_HIGH_HALF: 1 at 11.025 kHz (HALF_IN before pass 1, HALF_OUT after pass 2) */
#define K_HINC   124      /* its summed increments (33 words) */
#define K_SPREV  128      /* the string's last 22.05 kHz sample */
#define K_GBQ    132      /* 8 words */
#define K_SUB    164      /* 8 x { G = ga | gp, GN = 1 | gn, GF = -gf | gf } */

	.text

/* ---- entry and exit: R0 = k ---- */
.macro ENTER
	[--SP] = (R7:4, P5:3);
	R1 = L0;
	R2 = L1;
	[--SP] = R1;
	[--SP] = R2;
	R1 = L2;
	R2 = L3;
	[--SP] = R1;
	[--SP] = R2;
	R1 = 0;
	L0 = R1;
	L1 = R1;
	L2 = R1;
	L3 = R1;
	P5 = R0;
.endm

.macro LEAVE
	R2 = [SP++];
	R1 = [SP++];
	L3 = R2;
	L2 = R1;
	R2 = [SP++];
	R1 = [SP++];
	L1 = R2;
	L0 = R1;
	(R7:4, P5:3) = [SP++];
	RTS;
.endm

/* ---- pass 1 set-up: R7 phx, R6 r, R0 the first increment, R3 the LCG multiplier; I0 inc, I1 raw (a spare element
 * in front: the stores run a sample behind), I2 k[PHOLD], P4 the sub-block gains, P2 the sine table's middle ---- */
.macro P1_SETUP
	R0 = [P5 + K_INC];
	I0 = R0;
	R0 = [P5 + K_RAW];
	I1 = R0;
	R0 = P5;
	R1 = K_PHOLD;
	R0 = R0 + R1;
	I2 = R0;
	P4 = P5;
	P1 = K_SUB;
	P4 = P4 + P1;
	P2 = [P5 + K_SINE];
	R7 = [P5 + K_PH];
	R6 = [P5 + K_RNG];
	R0 = [I0++];
	R3 = [P5 + K_LA];
	R2 = 0;
	P3 = 8;
.endm

.macro P1_FINISH w
	.if \w == 16
	W[I1] = R2.L;
	.else
	[I1] = R2;
	.endif
	[P5 + K_PH] = R7;
	[P5 + K_RNG] = R6;
.endm

/* ---- the polyBLEP out of line: S in R1 (the ramps, s1 high, s2 low) is corrected in place as the C's blep_saw does;
 * R0 and R1 are the only live registers it must keep apart from R4..R7 (saved) ----
 *   t = (s + 32768) >> 1; t < dt: x = (t inv8) >> 7, s -= 2x - (x^2 >> 15) - 32768;
 *   t > 32767 - dt: x = ((t - 32768) inv8) >> 7, s -= (x^2 >> 15) + 2x + 32768 */
.macro BLEP_HALF n
	R3 = R2 + R5 (NS);
	R3 >>= 1;                                        /* t */
	CC = R3 < R6;
	IF CC JUMP .Lbl1_\n;
	R4 = R3 + R6 (NS);
	CC = R4 < R5;                                    /* t + dt < 32768: no correction */
	IF CC JUMP .Lbl3_\n;
	R3 = R3 - R5 (NS);
	R3 *= R7;
	R3 >>>= 7;                                       /* x */
	R4 = R3.L * R3.L (IS);
	R4 >>>= 15;
	R4 = R4 + R3 (NS);
	R4 = R4 + R3 (NS);
	R4 = R4 + R5 (NS);
	R2 = R2 - R4 (NS);
	JUMP.S .Lbl3_\n;
.Lbl1_\n:
	R3 *= R7;
	R3 >>>= 7;                                       /* x */
	R4 = R3.L * R3.L (IS);
	R4 >>>= 15;
	R2 = R2 - R3 (NS);
	R2 = R2 - R3 (NS);
	R2 = R2 + R4 (NS);
	R2 = R2 + R5 (NS);
.Lbl3_\n:
.endm

.macro BLEP_RARE n
.Lrare_\n:
	[--SP] = (R7:4);
	R5 = 32768 (Z);
	R6 = [P5 + K_DT];
	R7 = [P5 + K_INV8];
	R2 = R1 >>> 16;
	BLEP_HALF h\n
	R1 = PACK(R2.L, R1.L);
	R2 = R1.L (X);
	BLEP_HALF l\n
	R1 = PACK(R1.H, R2.L);
	(R7:4) = [SP++];
	JUMP .Lback_\n;
.endm

/* ---- the grit out of line (rough, ph < pold): one LCG step, mjit = 32767 - ((u grit) >> 15), u = lcg >> 17 = (r >>> 17)
 * + 16384. R1 the LCG increment and R3 its multiplier stay; R0 R2 R4 R5 free ---- */
.macro GRIT_RARE n
.Lgrit_\n:
	R1 = [P5 + K_LC];
	R0 = R6;
	R0 *= R3;
	R0 = R0 + R1 (NS);
	R6 = R0;
	R0 >>>= 17;
	R2 = 16384 (Z);
	R0 = R0 + R2 (NS);
	R2 = [P5 + K_GRIT];
	R0 *= R2;
	R0 >>>= 15;
	R2 = 32767 (Z);
	R0 = R2 - R0 (NS);
	[P5 + K_MJIT] = R0;
	JUMP .Lgback_\n;
.endm

/* ---- HS_HIGH_HALF: HALF_IN sums the increments in pairs (hinc[2m] = inc[4m] + inc[4m + 1], hinc[2m + 1] = inc[4m + 2]
 * + inc[4m + 3]) and points k[INC] at them, 3 packets per 11.025 kHz sample; HALF_OUT brings pass 2's 32 samples (hb[32..63])
 * back to 22.05 kHz in place: hb[2m] = (prev + y[m]) >> 1, hb[2m + 1] = y[m], 3 packets per sample ---- */
.macro HALF_IN n
#ifdef HS_HIGH_HALF
	R0 = [P5 + K_HALF];
	CC = R0 == 0;
	IF CC JUMP .Lhin_\n;
	R0 = [P5 + K_INC];
	I0 = R0;
	R0 += 4;
	P2 = R0;
	R0 = [P5 + K_HINC];
	I1 = R0;
	[P5 + K_INC] = R0;
	R7 = M0;                                         /* the caller's M0 comes back below */
	R0 = 8;
	M0 = R0;
	P3 = 8;
	R0 = [I0 ++ M0];
	R1 = [P2 ++ P3];
	P0 = 32;
	LSETUP (.Lhinb_\n, .Lhine_\n) LC0 = P0;
.Lhinb_\n:
	R4 = R0 + R1 (NS) || R3 = [P2 ++ P3] || R2 = [I0 ++ M0];
	R5 = R2 + R3 (NS) || R1 = [P2 ++ P3] || [I1++] = R4;
.Lhine_\n:
	MNOP || R0 = [I0 ++ M0] || [I1++] = R5;
	M0 = R7;
.Lhin_\n:
#endif
.endm

.macro HALF_OUT n
#ifdef HS_HIGH_HALF
	R0 = [P5 + K_HALF];
	CC = R0 == 0;
	IF CC JUMP .Lhout_\n;
	R0 = [P5 + K_HB];
	I0 = R0;
	R1 = 128;
	R0 = R0 - R1;
	I1 = R0;
	R1 = [P5 + K_SPREV];
	R0 = [I0++];
	P0 = 16;
	LSETUP (.Lhoutb_\n, .Lhoute_\n) LC0 = P0;
.Lhoutb_\n:
	R2 = R1 + R0 (NS) || R3 = [I0++] || NOP;
	R2 = R2 >>> 1;
	R4 = R0 + R3 (NS) || [I1++] = R2 || NOP;
	R4 = R4 >>> 1 || [I1++] = R0 || NOP;
	MNOP || [I1++] = R4 || R0 = [I0++];
.Lhoute_\n:
	R1 = PACK(R3.H, R3.L) || [I1++] = R3 || NOP;
	[P5 + K_SPREV] = R1;
.Lhout_\n:
#endif
.endm

/* ---- pass 2, the filters, in place from raw into hb: 19 packets per sample.
 * R7 fbp, R6 flp, R5 tone; R0 raw (a sample ahead), R3 h = raw >> 1 (a sample ahead), R1 c1 / d, R2 c2 / c3 / y, R4
 * the coefficient for the next multiply. I2 = k[FBP]: the new fbp goes there and comes back as the next products'
 * copies. I1 hb, I3 the per-sub-block gbq; the raw read ahead: P1 (halfwords) or I0 (words), one element past the end */
.macro PASS2 w, n
	R0 = [P5 + K_RAW];
	.if \w == 16
	R0 += 2;
	P1 = R0;
	.else
	R0 += 4;
	I0 = R0;
	.endif
	R0 = [P5 + K_HB];
	I1 = R0;
	R0 = P5;
	R1 = K_FBP;
	R1 = R0 + R1;
	I2 = R1;
	R1 = K_GBQ;
	R1 = R0 + R1;
	I3 = R1;
	R7 = [P5 + K_FBP];
	R6 = [P5 + K_FLP];
	R5 = [P5 + K_TONE];
	.if \w == 16
	R0 = W[P1++] (X);
	.else
	R0 = [I0++];
	.endif
	R1 = R7;
	R4 = [P5 + K_FF];
	R3 = R0 >>> 1;
	P3 = 8;
	LSETUP (.Lp2o_b\n, .Lp2o_e\n) LC1 = P3;
	P3 = [P5 + K_NIN2];
.Lp2o_b\n:
	LSETUP (.Lp2i_b\n, .Lp2i_e\n) LC0 = P3;
.Lp2i_b\n:
	R1 *= R4;                                                    /* c1 ff */
	R1 = R1 >>> 15 || R4 = [P5 + K_FQ] || R2 = [I2];
	R6 = R6 + R1 (NS);                                           /* flp */
	R3 = R3 - R6 (NS);
	R2 *= R4;                                                    /* c2 fq */
	R2 = R2 >>> 15 || R4 = [P5 + K_FF] || NOP;
	R3 = R3 - R2 (NS);                                           /* hp */
	R3 *= R4;
	R3 = R3 >>> 15 || R4 = [P5 + K_TA] || NOP;
	R7 = R7 + R3 (NS);                                           /* fbp */
	R1 = R0 - R5 (NS) || [I2] = R7 || NOP;
	R1 = R1 >>> 1 || R2 = [I2] || NOP;                           /* c3 */
	R1 *= R4;
	.if \w == 16
	R1 = R1 >>> 14 || R0 = W[P1++] (X) || R4 = [I3];
	.else
	R1 = R1 >>> 14 || R4 = [I3] || R0 = [I0++];
	.endif
	R5 = R5 + R1 (NS);                                           /* tone */
	R2 *= R4;                                                    /* fbp gbq */
	R2 = R2 >>> 15 || R4 = [P5 + K_FF] || R1 = [I2];
	R2 = R5 + R2 (NS);
.Lp2i_e\n:
	R3 = R0 >>> 1 || [I1++] = R2 || NOP;
.Lp2o_e\n:
	I3 += 4;
	[P5 + K_FBP] = R7;
	[P5 + K_FLP] = R6;
	[P5 + K_TONE] = R5;
.endm

/* ======== hs_string_plain: pass 1 18 packets per sample. R7 phx, R6 r; R1 S, R4 the noise, R2 raw ======== */
	.align 4
	.global _hs_string_plain
	.type _hs_string_plain, STT_FUNC
_hs_string_plain:
	ENTER
	HALF_IN pl
	P1_SETUP
	LSETUP (.Lpo_b, .Lpo_e) LC1 = P3;
	P3 = [P5 + K_NIN1];
.Lpo_b:
	LSETUP (.Lpi_b, .Lpi_e) LC0 = P3;
.Lpi_b:
#ifdef HS_BOW_CHEAP
	/* HS_BOW_CHEAP: a pair, 20 + 12 packets. A: the LCG, both noise products (R4.L this sample's, R4.H the next's,
	 * kept in k[NB]), the slip from this sample's phase */
	R7 = R7 + R0 (NS) || R1 = [P5 + K_LC] || R5 = [I0++];
	R6 *= R3;
	R7 = R7 + R5 (NS) || R0 = [P5 + K_DOFF] || W[I1++] = R2.L;
	R6 = R6 + R1 (NS) || R5 = [P5 + K_PW] || NOP;
	R2.L = R6.L + R6.H (NS);                                     /* the second sample's 16 bits */
	R7 = R7 + R0 (NS) || R3 = [P5 + K_K30] || NOP;
	R5 = R7 - R5 (NS) || R0 = [P5 + K_AM3] || NOP;
	R1 = PACK(R7.H, R5.H) || R4 = [P5 + K_AM1] || NOP;
	CC = R7 < R3;
	IF CC R0 = R4;
	R4.L = ASHIFT R6.H BY R0.L || R3 = [P5 + K_CM] || NOP;
	R4.H = ASHIFT R2.L BY R0.L;
	R2 = ABS R1 (V) || R5 = [P4 + 0] || NOP;
	R2 = R3 -|- R2 || R0 = [P4 + 4] || NOP;
	CC = AN;
	IF CC JUMP .Lrare_pa;
.Lback_pa:
	R1.H = R1.H * R5.H, R1.L = R1.L * R5.L (T) || R3 = [P5 + K_LA] || NOP;
	R4.H = R4.H * R0.L, R4.L = R4.L * R0.L (T);
	R2.L = R1.H - R1.L (NS) || [P5 + K_NB] = R4 || R0 = [I0++];
	R2.L = R2.L + R4.L (NS);
	/* B: no LCG, the noise from A */
	R7 = R7 + R0 (NS) || R4 = [P5 + K_NB] || R5 = [I0++];
	R7 = R7 + R5 (NS) || R0 = [P5 + K_DOFF] || W[I1++] = R2.L;
	R7 = R7 + R0 (NS) || R5 = [P5 + K_PW] || NOP;
	R5 = R7 - R5 (NS) || R3 = [P5 + K_CM] || NOP;
	R1 = PACK(R7.H, R5.H);
	R2 = ABS R1 (V) || R5 = [P4 + 0] || NOP;
	R2 = R3 -|- R2;
	CC = AN;
	IF CC JUMP .Lrare_pb;
.Lback_pb:
	R1.H = R1.H * R5.H, R1.L = R1.L * R5.L (T) || R3 = [P5 + K_LA] || NOP;
	R2.L = R1.H - R1.L (NS) || R0 = [I0++] || NOP;
.Lpi_e:
	R2.L = R2.L + R4.H (NS);
#else
	R7 = R7 + R0 (NS) || R1 = [P5 + K_LC] || R5 = [I0++];
	R6 *= R3;                                                    /* r = r a + c */
	R7 = R7 + R5 (NS) || R0 = [P5 + K_DOFF] || W[I1++] = R2.L;
	R6 = R6 + R1 (NS) || R5 = [P5 + K_PW] || NOP;
	R7 = R7 + R0 (NS) || R3 = [P5 + K_K30] || NOP;             /* phx */
	R5 = R7 - R5 (NS) || R0 = [P5 + K_AM3] || NOP;
	R1 = PACK(R7.H, R5.H) || R4 = [P5 + K_AM1] || NOP;          /* S = s1 | s2 */
	CC = R7 < R3;                                                /* ph < 2^30: the slip */
	IF CC R0 = R4;
	R4.L = ASHIFT R6.H BY R0.L || R3 = [P5 + K_CM] || NOP;     /* nz, >> 2 on the stick */
	R2 = ABS R1 (V) || R5 = [P4 + 0] || NOP;
	R2 = R3 -|- R2 || R0 = [P4 + 4] || NOP;
	CC = AN;
	IF CC JUMP .Lrare_pl;
.Lback_pl:
	R1.H = R1.H * R5.H, R1.L = R1.L * R5.L (T) || R3 = [P5 + K_LA] || NOP;
	R4.L = R4.L * R0.L (T);
	R2.L = R1.H - R1.L (NS) || R0 = [I0++] || NOP;
.Lpi_e:
	R2.L = R2.L + R4.L (NS);
#endif
.Lpo_e:
	P4 += 12;
	P1_FINISH 16
#ifdef HS_BOW_CHEAP
	JUMP .Lcheap_p2_pl;
	BLEP_RARE pa
	BLEP_RARE pb
.Lcheap_p2_pl:
#endif
	PASS2 16, pl
	HALF_OUT pl
	LEAVE
#ifndef HS_BOW_CHEAP
	BLEP_RARE pl
#endif
	.size _hs_string_plain, .-_hs_string_plain

/* ======== hs_string_r: pass 1 21 packets per sample (the grit) ======== */
	.align 4
	.global _hs_string_r
	.type _hs_string_r, STT_FUNC
_hs_string_r:
	ENTER
	HALF_IN r
	P1_SETUP
	LSETUP (.Lro_b, .Lro_e) LC1 = P3;
	P3 = [P5 + K_NIN1];
.Lro_b:
	LSETUP (.Lri_b, .Lri_e) LC0 = P3;
.Lri_b:
#ifdef HS_BOW_CHEAP
	/* HS_BOW_CHEAP: a pair, 23 + 15 packets */
	R7 = R7 + R0 (NS) || R1 = [P5 + K_LC] || R5 = [I0++];
	R7 = R7 + R5 (NS) || R0 = [P5 + K_DOFF] || W[I1++] = R2.L;
	R7 = R7 + R0 (NS) || R5 = [P5 + K_PHOLD] || NOP;
	CC = R7 < R5;
	IF CC JUMP .Lgrit_ra;
.Lgback_ra:
	R6 *= R3;
	R6 = R6 + R1 (NS) || R5 = [P5 + K_PW] || [I2] = R7;
	R5 = R7 - R5 (NS) || R3 = [P5 + K_K30] || NOP;
	R1 = PACK(R7.H, R5.H) || R0 = [P5 + K_AM3] || NOP;
	R2.L = R6.L + R6.H (NS) || R4 = [P5 + K_AM1] || NOP;
	CC = R7 < R3;
	IF CC R0 = R4;
	R4.L = ASHIFT R6.H BY R0.L || R3 = [P5 + K_CM] || NOP;
	R4.H = ASHIFT R2.L BY R0.L;
	R2 = ABS R1 (V) || R5 = [P4 + 0] || NOP;
	R2 = R3 -|- R2 || R0 = [P4 + 4] || NOP;
	CC = AN;
	IF CC JUMP .Lrare_ra;
.Lback_ra:
	R1.H = R1.H * R5.H, R1.L = R1.L * R5.L (T) || R3 = [P5 + K_LA] || NOP;
	R4.H = R4.H * R0.L, R4.L = R4.L * R0.L (T) || R5 = [P5 + K_MJIT] || NOP;
	R2.L = R1.H - R1.L (NS) || [P5 + K_NB] = R4 || R0 = [I0++];
	R2.L = R2.L + R4.L (NS);
	R2.L = R2.L * R5.L (T);
	/* B */
	R7 = R7 + R0 (NS) || R4 = [P5 + K_PW] || R5 = [I0++];
	R7 = R7 + R5 (NS) || R0 = [P5 + K_DOFF] || W[I1++] = R2.L;
	R7 = R7 + R0 (NS) || R5 = [P5 + K_PHOLD] || NOP;
	CC = R7 < R5;
	IF CC JUMP .Lgrit_rb;
.Lgback_rb:
	R5 = R7 - R4 (NS) || [I2] = R7 || NOP;
	R1 = PACK(R7.H, R5.H) || R4 = [P5 + K_NB] || NOP;
	R2 = ABS R1 (V) || R0 = [P5 + K_CM] || NOP;
	R2 = R0 -|- R2 || R5 = [P4 + 0] || NOP;
	CC = AN;
	IF CC JUMP .Lrare_rb;
.Lback_rb:
	R1.H = R1.H * R5.H, R1.L = R1.L * R5.L (T) || R3 = [P5 + K_LA] || NOP;
	R2.L = R1.H - R1.L (NS) || R5 = [P5 + K_MJIT] || R0 = [I0++];
	R2.L = R2.L + R4.H (NS);
.Lri_e:
	R2.L = R2.L * R5.L (T);
#else
	R7 = R7 + R0 (NS) || R1 = [P5 + K_LC] || R5 = [I0++];
	R7 = R7 + R5 (NS) || R0 = [P5 + K_DOFF] || W[I1++] = R2.L;
	R7 = R7 + R0 (NS) || R5 = [P5 + K_PHOLD] || NOP;          /* phx */
	CC = R7 < R5;                                                /* ph < pold */
	IF CC JUMP .Lgrit_r;
.Lgback_r:
	R6 *= R3;
	R6 = R6 + R1 (NS) || R5 = [P5 + K_PW] || [I2] = R7;
	R5 = R7 - R5 (NS) || R3 = [P5 + K_K30] || NOP;
	R1 = PACK(R7.H, R5.H) || R0 = [P5 + K_AM3] || NOP;
	R2 = ABS R1 (V) || R4 = [P5 + K_AM1] || NOP;
	CC = R7 < R3;
	IF CC R0 = R4;
	R4.L = ASHIFT R6.H BY R0.L || R3 = [P5 + K_CM] || NOP;
	R2 = R3 -|- R2 || R5 = [P4 + 0] || NOP;
	CC = AN;
	IF CC JUMP .Lrare_r;
.Lback_r:
	R1.H = R1.H * R5.H, R1.L = R1.L * R5.L (T) || R0 = [P4 + 4] || NOP;
	R4.L = R4.L * R0.L (T) || R5 = [P5 + K_MJIT] || NOP;
	R2.L = R1.H - R1.L (NS) || R0 = [I0++] || NOP;
	R2.L = R2.L + R4.L (NS) || R3 = [P5 + K_LA] || NOP;
.Lri_e:
	R2.L = R2.L * R5.L (T);                                      /* grit */
#endif
.Lro_e:
	P4 += 12;
	P1_FINISH 16
#ifdef HS_BOW_CHEAP
	JUMP .Lcheap_p2_r;
	BLEP_RARE ra
	BLEP_RARE rb
	GRIT_RARE ra
	GRIT_RARE rb
.Lcheap_p2_r:
#endif
	PASS2 16, r
	HALF_OUT r
	LEAVE
#ifndef HS_BOW_CHEAP
	BLEP_RARE r
	GRIT_RARE r
#endif
	.size _hs_string_r, .-_hs_string_r

/* ======== hs_string_f: pass 1 31 packets per sample (the sine pair) ======== */
	.align 4
	.global _hs_string_f
	.type _hs_string_f, STT_FUNC
_hs_string_f:
	ENTER
	HALF_IN f
	P1_SETUP
	LSETUP (.Lfo_b, .Lfo_e) LC1 = P3;
	P3 = [P5 + K_NIN1];
.Lfo_b:
	LSETUP (.Lfi_b, .Lfi_e) LC0 = P3;
.Lfi_b:
#ifdef HS_BOW_CHEAP
	/* HS_BOW_CHEAP: a pair, 33 + 25 packets */
	R7 = R7 + R0 (NS) || R1 = [P5 + K_LC] || R5 = [I0++];
	R6 *= R3;
	R7 = R7 + R5 (NS) || R0 = [P5 + K_DOFF] || [I1++] = R2;
	R6 = R6 + R1 (NS) || R5 = [P5 + K_PW] || NOP;
	R7 = R7 + R0 (NS) || R3 = [P5 + K_K30] || NOP;
	R2 = R7 >>> 19 || R0 = [P5 + K_AM3] || NOP;
	P0 = R2;
	P0 = P2 + (P0 << 1);
	R2 = R7 + R7 (NS) || R4 = [P5 + K_AM1] || NOP;
	R2 = R2 + R7 (NS);
	R2 = R2 >>> 19;
	P1 = R2;
	P1 = P2 + (P1 << 1);
	R2.L = R6.L + R6.H (NS);
	R5 = R7 - R5 (NS);
	R1 = PACK(R7.H, R5.H);
	CC = R7 < R3;
	IF CC R0 = R4;
	R4.L = ASHIFT R6.H BY R0.L || R3 = [P5 + K_CM] || NOP;
	R4.H = ASHIFT R2.L BY R0.L;
	R2 = ABS R1 (V) || R5 = [P4 + 0] || NOP;
	R2 = R3 -|- R2 || R0 = [P4 + 4] || NOP;
	CC = AN;
	IF CC JUMP .Lrare_fa;
.Lback_fa:
	R1.H = R1.H * R5.H, R1.L = R1.L * R5.L (T) || R3 = [P4 + 8] || NOP;
	R4.H = R4.H * R0.L, R4.L = R4.L * R0.L (T) || R5.L = W[P0] || NOP;
	R2.L = R1.H - R1.L (NS) || R5.H = W[P1] || NOP;
	R2.L = R2.L + R4.L (NS) || R1 = [P5 + K_KF] || NOP;
	R4.L = R5.H * R1.L (T);
	A0 = R5.L * R3.H || [P5 + K_NB] = R4 || NOP;
	R4.L = (A0 -= R4.L * R3.L) (T);
	A0 = R2.L * R1.H (IS) || R0 = [I0++] || NOP;
	R2 = (A0 += R4.L * R1.H) (IS) || R3 = [P5 + K_LA] || NOP;
	/* B */
	R7 = R7 + R0 (NS) || R4 = [P5 + K_PW] || R5 = [I0++];
	R7 = R7 + R5 (NS) || R0 = [P5 + K_DOFF] || [I1++] = R2;
	R7 = R7 + R0 (NS) || R3 = [P5 + K_CM] || NOP;
	R2 = R7 >>> 19 || R0 = [P5 + K_NB] || NOP;
	P0 = R2;
	P0 = P2 + (P0 << 1);
	R2 = R7 + R7 (NS);
	R2 = R2 + R7 (NS);
	R2 = R2 >>> 19;
	P1 = R2;
	P1 = P2 + (P1 << 1);
	R5 = R7 - R4 (NS);
	R1 = PACK(R7.H, R5.H) || R4 = [P5 + K_KF] || NOP;
	R2 = ABS R1 (V) || R5 = [P4 + 0] || NOP;
	R2 = R3 -|- R2;
	CC = AN;
	IF CC JUMP .Lrare_fb;
.Lback_fb:
	R1.H = R1.H * R5.H, R1.L = R1.L * R5.L (T) || R3 = [P4 + 8] || NOP;
	R2.L = R1.H - R1.L (NS) || R5.L = W[P0] || NOP;
	R2.L = R2.L + R0.H (NS) || R5.H = W[P1] || NOP;
	R4.L = R5.H * R4.L (T);
	A0 = R5.L * R3.H;
	R4.L = (A0 -= R4.L * R3.L) (T);
	A0 = R2.L * R4.H (IS) || R0 = [I0++] || NOP;
.Lfi_e:
	R2 = (A0 += R4.L * R4.H) (IS) || R3 = [P5 + K_LA] || NOP;
#else
	R7 = R7 + R0 (NS) || R1 = [P5 + K_LC] || R5 = [I0++];
	R6 *= R3;
	R7 = R7 + R5 (NS) || R0 = [P5 + K_DOFF] || [I1++] = R2;
	R6 = R6 + R1 (NS) || R5 = [P5 + K_PW] || NOP;
	R7 = R7 + R0 (NS) || R3 = [P5 + K_K30] || NOP;
	R2 = R7 >>> 19 || R0 = [P5 + K_AM3] || NOP;
	P0 = R2;
	P0 = P2 + (P0 << 1);                                         /* sine(ph) */
	R2 = R7 + R7 (NS) || R4 = [P5 + K_AM1] || NOP;
	R2 = R2 + R7 (NS);
	R2 = R2 >>> 19;
	P1 = R2;
	P1 = P2 + (P1 << 1);                                         /* sine(3 ph) */
	R5 = R7 - R5 (NS);
	R1 = PACK(R7.H, R5.H);
	CC = R7 < R3;
	IF CC R0 = R4;
	R4.L = ASHIFT R6.H BY R0.L || R3 = [P5 + K_CM] || NOP;
	R2 = ABS R1 (V) || R5 = [P4 + 0] || NOP;
	R2 = R3 -|- R2 || R0 = [P4 + 4] || NOP;
	CC = AN;
	IF CC JUMP .Lrare_f;
.Lback_f:
	R1.H = R1.H * R5.H, R1.L = R1.L * R5.L (T) || R3 = [P4 + 8] || NOP;
	R4.L = R4.L * R0.L (T) || R5.L = W[P0] || NOP;
	R2.L = R1.H - R1.L (NS) || R5.H = W[P1] || NOP;
	R2.L = R2.L + R4.L (NS) || R1 = [P5 + K_KF] || NOP;
	R4.L = R5.H * R1.L (T);                                      /* m16(sine(3 ph), 6554) >> 15 */
	A0 = R5.L * R3.H;                                            /* -gf sine(ph) */
	R4.L = (A0 -= R4.L * R3.L) (T);                              /* (fl gf) >> 15 */
	A0 = R2.L * R1.H (IS) || R0 = [I0++] || NOP;
.Lfi_e:
	R2 = (A0 += R4.L * R1.H) (IS) || R3 = [P5 + K_LA] || NOP;
#endif
.Lfo_e:
	P4 += 12;
	P1_FINISH 32
#ifdef HS_BOW_CHEAP
	JUMP .Lcheap_p2_f;
	BLEP_RARE fa
	BLEP_RARE fb
.Lcheap_p2_f:
#endif
	PASS2 32, f
	HALF_OUT f
	LEAVE
#ifndef HS_BOW_CHEAP
	BLEP_RARE f
#endif
	.size _hs_string_f, .-_hs_string_f

/* ======== hs_string_fr: pass 1 34 packets per sample (the sine pair and the grit) ======== */
	.align 4
	.global _hs_string_fr
	.type _hs_string_fr, STT_FUNC
_hs_string_fr:
	ENTER
	HALF_IN fr
	P1_SETUP
	LSETUP (.Lgo_b, .Lgo_e) LC1 = P3;
	P3 = [P5 + K_NIN1];
.Lgo_b:
	LSETUP (.Lgi_b, .Lgi_e) LC0 = P3;
.Lgi_b:
#ifdef HS_BOW_CHEAP
	/* HS_BOW_CHEAP: a pair, 36 + 28 packets */
	R7 = R7 + R0 (NS) || R1 = [P5 + K_LC] || R5 = [I0++];
	R7 = R7 + R5 (NS) || R0 = [P5 + K_DOFF] || [I1++] = R2;
	R7 = R7 + R0 (NS) || R5 = [P5 + K_PHOLD] || NOP;
	CC = R7 < R5;
	IF CC JUMP .Lgrit_fra;
.Lgback_fra:
	R6 *= R3;
	R6 = R6 + R1 (NS) || R5 = [P5 + K_PW] || [I2] = R7;
	R2 = R7 >>> 19 || R3 = [P5 + K_K30] || NOP;
	P0 = R2;
	P0 = P2 + (P0 << 1);
	R2 = R7 + R7 (NS) || R0 = [P5 + K_AM3] || NOP;
	R2 = R2 + R7 (NS) || R4 = [P5 + K_AM1] || NOP;
	R2 = R2 >>> 19;
	P1 = R2;
	P1 = P2 + (P1 << 1);
	R2.L = R6.L + R6.H (NS);
	R5 = R7 - R5 (NS);
	R1 = PACK(R7.H, R5.H);
	CC = R7 < R3;
	IF CC R0 = R4;
	R4.L = ASHIFT R6.H BY R0.L || R3 = [P5 + K_CM] || NOP;
	R4.H = ASHIFT R2.L BY R0.L;
	R2 = ABS R1 (V) || R5 = [P4 + 0] || NOP;
	R2 = R3 -|- R2 || R0 = [P4 + 4] || NOP;
	CC = AN;
	IF CC JUMP .Lrare_fra;
.Lback_fra:
	R1.H = R1.H * R5.H, R1.L = R1.L * R5.L (T) || R3 = [P4 + 8] || NOP;
	R4.H = R4.H * R0.L, R4.L = R4.L * R0.L (T) || R5.L = W[P0] || NOP;
	R2.L = R1.H - R1.L (NS) || R5.H = W[P1] || NOP;
	R2.L = R2.L + R4.L (NS) || R1 = [P5 + K_KF] || NOP;
	R4.L = R5.H * R1.L (T);
	A0 = R5.L * R3.H || R1 = [P5 + K_MJIT] || NOP;
	R4.L = (A0 -= R4.L * R3.L) (T);
	A0 = R2.L * R1.L (IS) || [P5 + K_NB] = R4 || NOP;
	R2 = (A0 += R4.L * R1.L) (IS) || R3 = [P5 + K_LA] || R0 = [I0++];
	R2 = R2 >>> 15;
	/* B */
	R7 = R7 + R0 (NS) || R4 = [P5 + K_PW] || R5 = [I0++];
	R7 = R7 + R5 (NS) || R0 = [P5 + K_DOFF] || [I1++] = R2;
	R7 = R7 + R0 (NS) || R5 = [P5 + K_PHOLD] || NOP;
	CC = R7 < R5;
	IF CC JUMP .Lgrit_frb;
.Lgback_frb:
	R2 = R7 >>> 19 || R0 = [P5 + K_NB] || [I2] = R7;
	P0 = R2;
	P0 = P2 + (P0 << 1);
	R2 = R7 + R7 (NS);
	R2 = R2 + R7 (NS);
	R2 = R2 >>> 19;
	P1 = R2;
	P1 = P2 + (P1 << 1);
	R5 = R7 - R4 (NS) || R3 = [P5 + K_CM] || NOP;
	R1 = PACK(R7.H, R5.H) || R4 = [P5 + K_KF] || NOP;
	R2 = ABS R1 (V) || R5 = [P4 + 0] || NOP;
	R2 = R3 -|- R2;
	CC = AN;
	IF CC JUMP .Lrare_frb;
.Lback_frb:
	R1.H = R1.H * R5.H, R1.L = R1.L * R5.L (T) || R3 = [P4 + 8] || NOP;
	R2.L = R1.H - R1.L (NS) || R5.L = W[P0] || NOP;
	R2.L = R2.L + R0.H (NS) || R5.H = W[P1] || NOP;
	R4.L = R5.H * R4.L (T) || R1 = [P5 + K_MJIT] || NOP;
	A0 = R5.L * R3.H;
	R4.L = (A0 -= R4.L * R3.L) (T);
	A0 = R2.L * R1.L (IS) || R3 = [P5 + K_LA] || NOP;
	R2 = (A0 += R4.L * R1.L) (IS) || R0 = [I0++] || NOP;
.Lgi_e:
	R2 = R2 >>> 15;
#else
	R7 = R7 + R0 (NS) || R1 = [P5 + K_LC] || R5 = [I0++];
	R7 = R7 + R5 (NS) || R0 = [P5 + K_DOFF] || [I1++] = R2;
	R7 = R7 + R0 (NS) || R5 = [P5 + K_PHOLD] || NOP;
	CC = R7 < R5;
	IF CC JUMP .Lgrit_fr;
.Lgback_fr:
	R6 *= R3;
	R6 = R6 + R1 (NS) || R5 = [P5 + K_PW] || [I2] = R7;
	R2 = R7 >>> 19 || R3 = [P5 + K_K30] || NOP;
	P0 = R2;
	P0 = P2 + (P0 << 1);
	R2 = R7 + R7 (NS) || R0 = [P5 + K_AM3] || NOP;
	R2 = R2 + R7 (NS) || R4 = [P5 + K_AM1] || NOP;
	R2 = R2 >>> 19;
	P1 = R2;
	P1 = P2 + (P1 << 1);
	R5 = R7 - R5 (NS);
	R1 = PACK(R7.H, R5.H);
	CC = R7 < R3;
	IF CC R0 = R4;
	R4.L = ASHIFT R6.H BY R0.L || R3 = [P5 + K_CM] || NOP;
	R2 = ABS R1 (V) || R5 = [P4 + 0] || NOP;
	R2 = R3 -|- R2 || R0 = [P4 + 4] || NOP;
	CC = AN;
	IF CC JUMP .Lrare_fr;
.Lback_fr:
	R1.H = R1.H * R5.H, R1.L = R1.L * R5.L (T) || R3 = [P4 + 8] || NOP;
	R4.L = R4.L * R0.L (T) || R5.L = W[P0] || NOP;
	R2.L = R1.H - R1.L (NS) || R5.H = W[P1] || NOP;
	R2.L = R2.L + R4.L (NS) || R1 = [P5 + K_KF] || NOP;
	R4.L = R5.H * R1.L (T);
	A0 = R5.L * R3.H || R1 = [P5 + K_MJIT] || NOP;
	R4.L = (A0 -= R4.L * R3.L) (T);
	A0 = R2.L * R1.L (IS) || R3 = [P5 + K_LA] || NOP;
	R2 = (A0 += R4.L * R1.L) (IS) || R0 = [I0++] || NOP;
.Lgi_e:
	R2 = R2 >>> 15;                                              /* (raw mjit) >> 15 */
#endif
.Lgo_e:
	P4 += 12;
	P1_FINISH 32
#ifdef HS_BOW_CHEAP
	JUMP .Lcheap_p2_fr;
	BLEP_RARE fra
	BLEP_RARE frb
	GRIT_RARE fra
	GRIT_RARE frb
.Lcheap_p2_fr:
#endif
	PASS2 32, fr
	HALF_OUT fr
	LEAVE
#ifndef HS_BOW_CHEAP
	BLEP_RARE fr
	GRIT_RARE fr
#endif
	.size _hs_string_fr, .-_hs_string_fr

#endif
