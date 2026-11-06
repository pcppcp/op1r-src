/* Nostromo's inner loops, hand-scheduled for the Blackfin: the same arithmetic as nostromo.c's C loops (the host
 * build's, and the NT_C_REF reference), bit for bit, in fewer instruction packets.
 *
 * void nt_k_filt(int32_t *x, int n, int32_t *k)
 *   nt_filt_b over x[0 .. n - 1] (n >= 1): k = { l, x1, x2, e, e1, e2, e3, bt, bx, gm, gb }, the state in and out.
 *   Per sample the damping and the two crossover poles, each c17(d) * coef + carried fraction (one 32-bit multiply,
 *   the clamps as MAX / MIN), floor >> 15 and the fraction as EXTRACT of bits 0..14; then y = c17(acc) gm + e and
 *   z = c17(x2) gb + (y & 32767), x = (y >> 15) + (z >> 15). The state lives in k (loaded and stored in the parallel
 *   slots); x is read through I0 and written through I1. 37 packets per sample (the C: about 65).
 *
 * void nt_k_ap(int32_t *x, int n, const int32_t *a)
 *   nt_ap_b over x[0 .. n - 1] (n >= 1): a = { ring, head, tail, g, soft } (head / tail: ring indices of the block's
 *   first tick; both step back a sample a tick and wrap: the ring is read and written in a circle through I0 / I1).
 *   m15(tail, g) is the fractional product with the round-toward-zero bias (tail < 0: + 32767) and a truncating
 *   extraction (a 16 x 16 product, exact); m15(acc, -g) takes the clamped acc (17 bits) through a 32-bit multiply and
 *   the C's bias; s16(acc) is a saturating << 16 whose high half is stored. soft: nt_knee as min(|acc|, (|acc| +
 *   86016) >> 2, (|acc| + 454656) >> 4) with acc's sign, then saturated (the same values as the knee's branches).
 *   16 packets per sample, soft 26 (the C: about 29).
 *
 * void nt_k_map(int32_t *x, int n, const int32_t *a)
 *   nt_map_b over x[0 .. n - 1] (n >= 1): a = { ring, head, len << 15, amp, lfo } (head: the ring index of the block's
 *   first tick). The read position i = head - j + (pos >> 15) moves by the change of pos >> 15 less one a tick: I0 (in
 *   a circle) reads ring[i + 1], then ring[i], then steps by M0 = 2 (q - q_prev). r = a + m15(b - a, f) as one
 *   fractional sum, 65536 a + 2 f b - 2 f a + the bias (b < a: the saturated 16-bit difference gives the sign), whose
 *   high half is r (between a and b: 16 bits); m15(r, DD1) by the fractional product with the bias; m15(acc, DD1) of
 *   the clamped acc as nt_k_ap. 33 packets per sample (the C: about 58).
 *
 * void nt_k_taps(int32_t *sum, int n, const int32_t *a)
 *   the 14 output taps (nt_tap_b), four lines a pass in a circle each (I0 .. I3): a = { ring, then per pass four ring
 *   indices and two sign words (tap 1 | tap 0, tap 3 | tap 2; +1, -1, 0 for a pass's unused slot) } x 4 passes;
 *   sum[j] += the pass's signed taps (integer products with +-1 on both accumulators). 4 packets per sample and pass.
 * void nt_k_dl(int32_t *x, int n, const int32_t *a), a = { ring, head, read }: nt_dl_b, 2 packets per sample.
 * void nt_k_add(int32_t *x, int n, const int32_t *a), a = { ring, at }: nt_add_b, 2 packets per sample.
 * void nt_k_wr(const int32_t *x, int n, const int32_t *a), a = { ring, at }: nt_wr_b, 2 packets per sample.
 *
 * void nt_k_front(int32_t *x, int n, int32_t *a)
 *   the ticks' front for a segment over which the fade, the ADC's gain step and the read positions hold (nostromo.c's
 *   nt_front_k cuts the block there): a = { pd, pw, pw - pd_cur, pw - pd_next, F (fade << 5, 0: none), sh, rg, rgg,
 *   m (nt_q12's mask, -1 without the ADC), apk (in and out), fast }; per tick in = nt_tz(x, sh) + m15(rg 4 >> sh, rgg),
 *   pd[pw] = s16(in), v = pd[pw - pd_cur] << sh crossfaded towards pd[pw - pd_next] << sh by F (F += 32 a tick), the
 *   ADC (|v| into apk, then v & m in magnitude), x = m15(v, 8192) = nt_tz(c17(v), 2). The pre-delay line is read and
 *   written in circles of NT_PD (I0 .. I2). General: 49 packets per tick (C about 87 with the LFO and reflections);
 *   fast (HIFI, no fade, no regeneration): 9; HIFI with either (no ADC, sh 0): 31, the crossfade and m15(v, 8192) as
 *   fractional sums (nt_k_map's form, F / 2 in the products).
 * void nt_k_er(int32_t *e, int n, const int32_t *a)
 *   reflection taps (nt_er) added into e: a = { pd, nA, F0, sh, taps, then per tap { pw - d_cur, pw - d_next, g } };
 *   the first nA ticks crossfade from the d_cur read to the d_next read (F0 + 32 j), the rest read d_next. A tap a
 *   pass in 32-bit arithmetic as the C (25 packets per tick fading, 12 after) for sh 1; with sh 0 dual fractional products
 *   with the round-toward-zero bias where v g < 0: without a fade four taps a pass (15 packets per tick and pass), with
 *   one two taps a pass, the crossfade as one fractional sum 65536 a + 2 F b - 2 F a + the bias (b < a) as nt_k_map's
 *   (F / 2 in the products: F reaches 32768), 20 packets per tick and pass fading, 10 after.
 *
 * void nt_k_dec(int32_t *x, const int16_t *buf, const int32_t *a), a = { hbx, hbi }
 *   STD's decimator over the block (the half phase 0, hbi even: nostromo.c checks): the 64 inputs into the
 *   history ring (I2, a circle of 32), each pair one output x = nt_shr(16384 c + the half-band's 16 taps, 15) by
 *   integer products in A0 (exact: every C sum fits int32); the taps read as words through I0 / I1 (circles of 16
 *   words), the odd ones in the high halves. 28 packets per output (the C: about 190).
 * void nt_k_int(int32_t *wet, const int32_t *x, int32_t *a), a = { hbw, hbj, hbo (in and out), kd (96 words) }
 *   STD's interpolator for the block's 32 tank samples, 64 wet samples (the half phase 0): the history and the new
 *   samples (each to 8x full scale) linear in kd, with split words (h + 32768) >> 16 | h & 0xffff, so nt_hbi's sum
 *   is 65536 A1 + A0 of 16 dual integer products (A1 << 16 into A0: 40 bits hold it), rounded toward zero as |b| >>
 *   14 with b's sign; the ring gets the last 16 back. About 45 packets per tank sample (the C: about 390).
 *
 * L0..L3 are zero on return. M1 is not used.
 * Host builds assemble it empty: nostromo.c keeps the C loops there.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

	.align 4
	.global _nt_k_filt
	.type _nt_k_filt, STT_FUNC
_nt_k_filt:
	[--SP] = (R7:4);
	I0 = R0;                             /* x, read */
	I1 = R0;                             /* x, written */
	P1 = R2;                             /* k */
	P2 = R1;                             /* n */
	R5 = 15;                             /* EXTRACT: bits 0 .. 14 */
	R6 = 65535 (Z);
	R7.H = -1;
	R7.L = 1;                            /* -65535 */
	R0 = [P1];                           /* l */
	R1 = [P1 + 28];                      /* bt */
	R2 = [P1 + 12];                      /* e */
	R4 = [I0++];                         /* x */
	LSETUP (.Lfi_b, .Lfi_e) LC0 = P2;
.Lfi_b:
	R3 = R0 - R4 (NS) || [P1 + 12] = R2;          /* l - x; e (the last sample's) */
	R3 = MAX (R3, R7) || R2 = [P1 + 16];          /* e1 */
	R3 = MIN (R3, R6) || R0 = [P1 + 4];           /* x1 */
	R3 *= R1;                                     /* c17(l - x) bt */
	R3 = R3 + R2 (NS) || R1 = [P1 + 32];          /* + e1; bx */
	R2 = R3 >>> 15;
	R3 = EXTRACT (R3, R5.L) (Z);
	R4 = R4 + R2 (NS) || [P1 + 16] = R3;          /* acc = l; e1 */
	R3 = R0 - R4 (NS) || [P1] = R4;               /* x1 - acc; l */
	R3 = MAX (R3, R7) || R2 = [P1 + 20];          /* e2 */
	R3 = MIN (R3, R6) || R0 = [P1 + 8];           /* x2 */
	R3 *= R1;
	R3 = R3 + R2 (NS);
	R2 = R3 >>> 15;
	R3 = EXTRACT (R3, R5.L) (Z);
	R2 = R4 + R2 (NS) || [P1 + 20] = R3;          /* x1; e2 */
	R3 = R0 - R2 (NS) || [P1 + 4] = R2;           /* x2 - x1; x1 */
	R3 = MAX (R3, R7) || R0 = [P1 + 24];          /* e3 */
	R3 = MIN (R3, R6);
	R3 *= R1;
	R3 = R3 + R0 (NS) || R1 = [P1 + 36];          /* gm */
	R0 = R3 >>> 15;
	R3 = EXTRACT (R3, R5.L) (Z);
	R2 = R2 + R0 (NS) || [P1 + 24] = R3;          /* x2; e3 */
	R3 = MAX (R4, R7) || [P1 + 8] = R2;           /* c17(acc); x2 */
	R3 = MIN (R3, R6) || R0 = [P1 + 12];          /* e */
	R3 *= R1;
	R3 = R3 + R0 (NS) || R4 = [P1 + 40];          /* y; gb */
	R2 = MAX (R2, R7);
	R2 = MIN (R2, R6);
	R2 *= R4;
	R0 = EXTRACT (R3, R5.L) (Z) || R1 = [P1 + 28];   /* y & 32767; bt */
	R2 = R2 + R0 (NS);                            /* z */
	R3 = R3 >>> 15 || R4 = [I0++] || NOP;         /* the next x */
	R0 = R2 >>> 15;
	R3 = R3 + R0 (NS);
.Lfi_e:
	R2 = EXTRACT (R2, R5.L) (Z) || R0 = [P1] || [I1++] = R3;   /* e; l; x out */
	[P1 + 12] = R2;
	(R7:4) = [SP++];
	RTS;
	.size _nt_k_filt, .-_nt_k_filt
/* nt_k_ap's frame: [SP] -g, [SP + 4] 86016, [SP + 8] 454656 */
	.align 4
	.global _nt_k_ap
	.type _nt_k_ap, STT_FUNC
_nt_k_ap:
	[--SP] = (R7:4);
	SP += -12;
	I2 = R0;                             /* x, read */
	R0 += -4;
	I3 = R0;                             /* x, written a sample behind (from the word before x) */
	R0 += 4;
	P2 = R1;                             /* n */
	P1 = R2;                             /* a */
	R0 = [P1];                           /* ring */
	B0 = R0;
	B1 = R0;
	R3 = [P1 + 4];
	R3 = R3 << 1;
	R3 = R3 + R0;
	I0 = R3;                             /* head */
	R3 = [P1 + 8];
	R3 = R3 << 1;
	R3 = R3 + R0;
	I1 = R3;                             /* tail */
	R0.H = 2;
	R0.L = 0;
	L0 = R0;                             /* 65536 words */
	L1 = R0;
	R3 = [P1 + 12];                      /* g */
	R5 = R3;
	R5.H = -32767;                       /* -32767 | g */
	R3 = -R3;
	[SP] = R3;                           /* -g */
	R3.H = 1;
	R3.L = 20480;                        /* 86016 */
	[SP + 4] = R3;
	R3.H = 6;
	R3.L = 61440;                        /* 454656 */
	[SP + 8] = R3;
	R6.H = -1;
	R6.L = 1;                            /* -65535 */
	R7 = 65535 (Z);
	R3 = [P1 + 16];                      /* soft */
	R4 = [I3];                           /* the first store writes the word before x back unchanged */
	R1.L = W[I1--];                      /* tail */
	R0 = [I2++];                         /* x */
	CC = R3 == 0;
	IF !CC JUMP .Lap_soft;
	LSETUP (.Lap_b, .Lap_e) LC0 = P2;
.Lap_b:
	R3 = R1 >>> 15 (V) || [I3++] = R4 || NOP;     /* x out (the last sample's) */
	A1 = R1.L * R5.L;                             /* tail g */
	R3.H = (A1 += R3.L * R5.H) (T);               /* m15(tail, g) */
	R2 = R1 << 16;
	R2 = R2 >>> 16;                               /* tail */
	R3 = R3 >>> 16 || R1 = [SP] || NOP;           /* -g */
	R4 = R0 + R3 (NS);                            /* acc */
	R3 = R4 << 16 (S) || R0 = [I2++] || NOP;      /* s16(acc); the next x */
	R4 = MAX (R4, R6) || W[I0--] = R3.H || NOP;
	R4 = MIN (R4, R7);
	R4 *= R1;
	R3 = R4 >>> 31 || R1.L = W[I1--] || NOP;      /* the next tail */
	R3 = R3 >> 17;
	R4 = R4 + R3 (NS);
	R4 = R4 >>> 15;
.Lap_e:
	R4 = R4 + R2 (NS);                            /* m15(acc, -g) + tail */
	JUMP .Lap_out;
.Lap_soft:
	LSETUP (.Las_b, .Las_e) LC0 = P2;
.Las_b:
	R3 = R1 >>> 15 (V) || [I3++] = R4 || NOP;
	A1 = R1.L * R5.L;
	R3.H = (A1 += R3.L * R5.H) (T);
	R2 = R1 << 16;
	R2 = R2 >>> 16;
	R3 = R3 >>> 16;
	R4 = R0 + R3 (NS) || R1 = [SP + 4];           /* acc; 86016 */
	R3 = ABS R4 || R0 = [SP + 8];                 /* |acc|; 454656 */
	R1 = R3 + R1 (NS);
	R0 = R3 + R0 (NS);
	R1 = R1 >>> 2;
	R0 = R0 >>> 4;
	R3 = MIN (R3, R1);
	R3 = MIN (R3, R0);
	R1 = -R3 (NS);
	CC = R4 < 0;
	IF CC R3 = R1;
	R3 = R3 << 16 (S) || R0 = [I2++] || NOP;      /* knee(acc); the next x */
	R4 = MAX (R4, R6) || W[I0--] = R3.H || R1 = [SP];
	R4 = MIN (R4, R7);
	R4 *= R1;
	R3 = R4 >>> 31 || R1.L = W[I1--] || NOP;
	R3 = R3 >> 17;
	R4 = R4 + R3 (NS);
	R4 = R4 >>> 15;
.Las_e:
	R4 = R4 + R2 (NS);
.Lap_out:
	[I3++] = R4;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	SP += 12;
	(R7:4) = [SP++];
	RTS;
	.size _nt_k_ap, .-_nt_k_ap
/* nt_k_map's frame: [SP] amp, [SP + 4] len << 15, [SP + 8] 15, [SP + 12] -65535, [SP + 16] 65535, [SP + 20] DD1 */
	.align 4
	.global _nt_k_map
	.type _nt_k_map, STT_FUNC
_nt_k_map:
	[--SP] = (R7:4);
	SP += -24;
	I2 = R0;                             /* x, read */
	R0 += -4;
	I3 = R0;                             /* x, written a sample behind (from the word before x) */
	P2 = R1;                             /* n */
	P1 = R2;                             /* a */
	R0 = [P1];                           /* ring */
	B0 = R0;
	B1 = R0;
	R3 = [P1 + 4];                       /* head */
	R1 = R3;
	R1 += 1;
	R1 = R1.L (Z);
	R1 = R1 << 1;
	R1 = R1 + R0;
	I0 = R1;                             /* ring[head + 1]: pos >> 15 is added in the loop */
	R3 = R3 << 1;
	R3 = R3 + R0;
	I1 = R3;                             /* head, written */
	R0.H = 2;
	R0.L = 0;
	L0 = R0;
	L1 = R0;
	R0 = [P1 + 12];
	[SP] = R0;                           /* amp */
	R1 = R0;                             /* amp, for the first sample */
	R0 = [P1 + 8];
	[SP + 4] = R0;                       /* len << 15 */
	R0 = 15;
	[SP + 8] = R0;
	R0.H = -1;
	R0.L = 1;
	[SP + 12] = R0;                      /* -65535 */
	R0 = 65535 (Z);
	[SP + 16] = R0;
	R0 = 22938 (Z);                      /* NT_DD1 */
	[SP + 20] = R0;
	R5 = R0;
	R5.H = -32767;                       /* -32767 | DD1 */
	P0 = [P1 + 16];                      /* lfo */
	R6 = 0;                              /* Q = 2 (pos >> 15) of the last sample */
	R0 = [I3];                           /* the first store writes the word before x back unchanged */
	R4 = [P0++];                         /* lfo */
	LSETUP (.Lmp_b, .Lmp_e) LC0 = P2;
.Lmp_b:
	R4 = R4.L * R1.L (IS) || [I3++] = R0 || R2 = [SP + 4];   /* amp lfo; x out (the last sample's); len << 15 */
	R4 = R4 >>> 8 || R3 = [SP + 8] || NOP;
	R4 = R4 + R2 (NS);                            /* pos */
	R3 = EXTRACT (R4, R3.L) (Z) || R0 = [I2++] || NOP;   /* f = pos & 32767; x */
	R4 = R4 >>> 15;
	R4 = R4 << 1;                                 /* 2 (pos >> 15) */
	R2 = R4 - R6 (NS);
	M0 = R2;
	R6 = R6 + R2 (NS) || I0 += M0 || NOP;         /* Q */
	R3.H = 16384;
	MNOP || R1.H = W[I0--] || NOP;                /* b = ring[i + 1] */
	MNOP || R1.L = W[I0] || NOP;                  /* a = ring[i] */
	R2.L = R1.H - R1.L (S);                       /* b - a, saturated: its sign */
	R2 = R2 >>> 15 (V);
	A1 = R1.H * R3.L, A0 = R1.L * R3.H;           /* 2 f b | 32768 a */
	A1 -= R1.L * R3.L, A0 += R1.L * R3.H;         /* - 2 f a | + 32768 a */
	A1 += R2.L * R5.H;                            /* + 2 x 32767 when b < a */
	A0 += A1;
	R2 = A0;                                      /* r in the high half */
	R3 = R2 >>> 15 (V);
	A1 = R2.H * R5.L;                             /* r DD1 */
	R3.H = (A1 += R3.H * R5.H) (T);               /* m15(r, DD1) */
	R3 = R3 >>> 16;
	R2 = R2 >>> 16;                               /* r */
	R0 = R0 - R3 (NS) || R7 = [SP + 12];          /* acc; -65535 */
	R3 = R0 << 16 (S) || R1 = [SP + 16];          /* s16(acc); 65535 */
	R0 = MAX (R0, R7) || W[I1--] = R3.H || R4 = [SP + 20];   /* DD1 */
	R0 = MIN (R0, R1);
	R0 *= R4;
	R3 = R0 >>> 31 || R4 = [P0++] || NOP;         /* the next lfo */
	R3 = R3 >> 17 || R1 = [SP] || NOP;            /* amp */
	R0 = R0 + R3 (NS);
	R0 = R0 >>> 15;
.Lmp_e:
	R0 = R0 + R2 (NS);                            /* m15(acc, DD1) + r */
	[I3++] = R0;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	SP += 24;
	(R7:4) = [SP++];
	RTS;
	.size _nt_k_map, .-_nt_k_map
/* \ireg = ring (R0) + 2 R3 (a ring index), a circle of 65536 words; R3 scratch */
.macro RINGPTR ireg, breg, lreg
	R3 = R3 << 1;
	R3 = R3 + R0;
	\ireg = R3;
	\breg = R0;
	R3.H = 2;
	R3.L = 0;
	\lreg = R3;
.endm

	.align 4
	.global _nt_k_taps
	.type _nt_k_taps, STT_FUNC
_nt_k_taps:
	[--SP] = (R7:4, P5:3);
	P3 = R0;                             /* sum */
	P4 = R1;                             /* n */
	P5 = R2;                             /* a */
	R0 = [P5++];                         /* ring */
	M3 = R0;
	P2 = 4;
	LSETUP (.Ltp_pass, .Ltp_pe) LC1 = P2;
.Ltp_pass:
	R0 = M3;
	R3 = [P5 + 0];
	RINGPTR I0, B0, L0
	R3 = [P5 + 4];
	RINGPTR I1, B1, L1
	R3 = [P5 + 8];
	RINGPTR I2, B2, L2
	R3 = [P5 + 12];
	RINGPTR I3, B3, L3
	R6 = [P5 + 16];                      /* signs: tap 1 | tap 0 */
	R7 = [P5 + 20];                      /* tap 3 | tap 2 */
	P5 += 24;
	P0 = P3;                             /* sum, read */
	P1 = P3;
	P1 += -4;                            /* sum, written a sample behind */
	R5 = [P1];                           /* the first store writes the word before sum back unchanged */
	R1.L = W[I0--];
	R1.H = W[I1--];
	R2.L = W[I2--];
	R2.H = W[I3--];
	LSETUP (.Ltp_b, .Ltp_e) LC0 = P4;
.Ltp_b:
	A1 = R1.H * R6.H, A0 = R1.L * R6.L (IS) || R4 = [P0++] || NOP;
	A1 += R2.H * R7.H, A0 += R2.L * R7.L (IS) || R1.L = W[I0--] || R1.H = W[I1--];
	R0 = (A0 += A1) || [P1++] = R5 || R2.L = W[I2--];
.Ltp_e:
	R5 = R0 + R4 (NS) || R2.H = W[I3--] || NOP;
.Ltp_pe:
	[P1] = R5;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	L2 = R0;
	L3 = R0;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _nt_k_taps, .-_nt_k_taps

	.align 4
	.global _nt_k_dl
	.type _nt_k_dl, STT_FUNC
_nt_k_dl:
	I2 = R0;                             /* x, read */
	R0 += -4;
	I3 = R0;                             /* x, written a sample behind (from the word before x) */
	P2 = R1;
	P1 = R2;
	R0 = [P1];
	R3 = [P1 + 4];
	RINGPTR I0, B0, L0            /* head, written */
	R3 = [P1 + 8];
	RINGPTR I1, B1, L1            /* read */
	R0 = [I2++];
	R3 = [I3];                           /* the first store writes the word before x back unchanged */
	LSETUP (.Ldl_b, .Ldl_e) LC0 = P2;
.Ldl_b:
	R1 = R0 << 16 (S) || R2.H = W[I1--] || [I3++] = R3;   /* s16(x); the line's sample; x out (the last one) */
.Ldl_e:
	R3 = R2 >>> 16 || W[I0--] = R1.H || R0 = [I2++];
	[I3++] = R3;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	RTS;
	.size _nt_k_dl, .-_nt_k_dl

	.align 4
	.global _nt_k_add
	.type _nt_k_add, STT_FUNC
_nt_k_add:
	I2 = R0;
	R0 += -4;
	I3 = R0;
	P2 = R1;
	P1 = R2;
	R0 = [P1];
	R3 = [P1 + 4];
	RINGPTR I0, B0, L0
	R2.H = W[I0--];
	R3 = [I3];                           /* the first store writes the word before x back unchanged */
	LSETUP (.Lad_b, .Lad_e) LC0 = P2;
.Lad_b:
	R1 = R2 >>> 16 || R0 = [I2++] || [I3++] = R3;
.Lad_e:
	R3 = R0 + R1 (NS) || R2.H = W[I0--] || NOP;
	[I3++] = R3;
	R0 = 0;
	L0 = R0;
	RTS;
	.size _nt_k_add, .-_nt_k_add

	.align 4
	.global _nt_k_wr
	.type _nt_k_wr, STT_FUNC
_nt_k_wr:
	I2 = R0;
	P2 = R1;
	P1 = R2;
	R0 = [P1];
	R3 = [P1 + 4];
	RINGPTR I0, B0, L0
	R0 = [I2++];
	LSETUP (.Lwr_b, .Lwr_e) LC0 = P2;
.Lwr_b:
	R1 = R0 << 16 (S);
.Lwr_e:
	MNOP || W[I0--] = R1.H || R0 = [I2++];
	R0 = 0;
	L0 = R0;
	RTS;
	.size _nt_k_wr, .-_nt_k_wr
/* nt_k_front's frame: [SP] -sh, [SP + 4] 2 - sh, [SP + 8] sh, [SP + 12] rgg, [SP + 16] F (running), [SP + 20] m,
 * [SP + 24] apk (running), [SP + 28] the step of F (32 while fading, else 0), [SP + 32] -65535, [SP + 36] 65535 */
	.align 4
	.global _nt_k_front
	.type _nt_k_front, STT_FUNC
_nt_k_front:
	[--SP] = (R7:4, P5:3);
	SP += -40;
	P0 = R0;                             /* x, in place */
	P2 = R1;                             /* n */
	P5 = R2;                             /* a */
	R0 = [P5];                           /* pd */
	B0 = R0;
	B1 = R0;
	B2 = R0;
	R3 = 44100 (Z);                      /* NT_PD words */
	L0 = R3;
	L1 = R3;
	L2 = R3;
	R3 = [P5 + 4];
	R3 = R3 << 1;
	R3 = R3 + R0;
	I0 = R3;                             /* pw, written */
	R3 = [P5 + 8];
	R3 = R3 << 1;
	R3 = R3 + R0;
	I1 = R3;                             /* pw - pd_cur */
	R3 = [P5 + 12];
	R3 = R3 << 1;
	R3 = R3 + R0;
	I2 = R3;                             /* pw - pd_next (fading; else as I1) */
	R0 = [P5 + 40];
	CC = R0 == 1;
	IF !CC JUMP .Lfr_gen;
	R7.H = -32767;
	R7.L = 8192;
	R0 = [P0];
	LSETUP (.Lff_b, .Lff_e) LC0 = P2;
.Lff_b:                                  /* HIFI, no fade, no regeneration: 9 packets */
	R1 = R0 << 16 (S);
	MNOP || W[I0++] = R1.H || R0 = [P0 + 4];      /* s16(in); the next x */
	MNOP || R3.H = W[I1++] || NOP;                /* v */
	R5 = R3 >>> 15 (V);
	A1 = R3.H * R7.L;
	R3.H = (A1 += R5.H * R7.H) (T);               /* m15(v, 8192) */
	R3 = R3 >>> 16;
.Lff_e:
	[P0++] = R3;
	JUMP .Lfr_out;
.Lfr_gen:
	R0 = [P5 + 20];                      /* sh */
	R1 = -R0;
	[SP] = R1;
	R1 = 2;
	R1 = R1 - R0;
	[SP + 4] = R1;
	[SP + 8] = R0;                       /* sh */
	R1 = [P5 + 28];
	[SP + 12] = R1;                      /* rgg */
	R1 = [P5 + 16];
	[SP + 16] = R1;                      /* F */
	R0 = 0;
	CC = R1 == 0;
	R2 = 32;
	IF !CC R0 = R2;
	[SP + 28] = R0;                      /* F's step */
	R1 = [P5 + 32];
	[SP + 20] = R1;                      /* m */
	R1 = [P5 + 36];
	[SP + 24] = R1;                      /* apk */
	P1 = [P5 + 24];                      /* rg */
	R1.H = -1;
	R1.L = 1;
	[SP + 32] = R1;                      /* -65535 */
	R1 = 65535 (Z);
	[SP + 36] = R1;
	R6.H = -1;
	R6.L = 1;                            /* -65535 */
	R7 = 65535 (Z);
	R0 = [P5 + 40];
	CC = R0 == 2;
	IF CC JUMP .Lfr_mid;
	LSETUP (.Lfg_b, .Lfg_e) LC0 = P2;
.Lfg_b:
	R0 = [P0];                                    /* x */
	R1 = R0 >>> 31 || R2 = [SP] || NOP;           /* -sh */
	R1 = R1.L * R2.L (IS) || R3 = [SP + 4] || NOP;   /* sh when x < 0; 2 - sh */
	R0 = R0 + R1 (NS);
	R0 = ASHIFT R0 BY R2.L || R1 = [P1++] || NOP;    /* in = nt_tz(x, sh); rg */
	R1 = ASHIFT R1 BY R3.L || R2 = [SP + 12] || NOP; /* rg 4 >> sh; rgg */
	R1 = MAX (R1, R6);
	R1 = MIN (R1, R7);
	R1 *= R2;
	R3 = R1 >>> 31;
	R3 = R3 >> 17;
	R1 = R1 + R3 (NS);
	R1 = R1 >>> 15;
	R0 = R0 + R1 (NS) || R2 = [SP + 8] || NOP;       /* in += m15(rg 4 >> sh, rgg); sh */
	R1 = R0 << 16 (S);
	MNOP || W[I0++] = R1.H || NOP;                /* pd[pw] = s16(in) */
	MNOP || R3.H = W[I1++] || R4.H = W[I2++];
	R3 = R3 >>> 16 || R5 = [SP + 16] || NOP;      /* F */
	R3 = ASHIFT R3 BY R2.L;                       /* v = pd[pw - pd_cur] << sh */
	R4 = R4 >>> 16;
	R4 = ASHIFT R4 BY R2.L;                       /* pd[pw - pd_next] << sh */
	R4 = R4 - R3 (NS);
	R4 = MAX (R4, R6);
	R4 = MIN (R4, R7);
	R4 *= R5;
	R0 = R4 >>> 31 || R1 = [SP + 28] || NOP;      /* F's step */
	R0 = R0 >> 17;
	R4 = R4 + R0 (NS);
	R4 = R4 >>> 15;
	R3 = R3 + R4 (NS) || R0 = [SP + 24] || NOP;   /* v += m15(.. - v, F); apk */
	R5 = R5 + R1 (NS) || R2 = [SP + 20] || NOP;   /* m */
	R4 = ABS R3 || [SP + 16] = R5 || NOP;
	R0 = MAX (R0, R4);
	[SP + 24] = R0;
	R4 = R4 & R2;
	R5 = -R4 (NS);
	CC = R3 < 0;
	IF CC R4 = R5;                                /* nt_q12(v, ask) */
	R4 = MAX (R4, R6);
	R4 = MIN (R4, R7);
	R5 = R4 >>> 31;
	R5 = R5 >> 30;
	R4 = R4 + R5 (NS);
	R4 = R4 >>> 2;                                /* m15(v, 8192) */
.Lfg_e:
	[P0++] = R4;
	R0 = [SP + 24];
	[P5 + 36] = R0;                      /* apk */
	JUMP .Lfr_out;
.Lfr_mid:                                /* sh 0, no ADC (HIFI with a fade or regeneration): 31 packets */
	R5 = [SP + 16];
	R5 = R5 >> 1;
	R5.H = 16384;                        /* 16384 | F / 2 */
	R0 = [SP + 28];
	R0 = R0 >> 1;
	[SP + 28] = R0;                      /* 0 | 16 (fading) */
	R6.H = -32767;
	R6.L = 8192;
	LSETUP (.Lfm_b, .Lfm_e) LC0 = P2;
.Lfm_b:
	R0 = [P0];                                    /* x */
	R1 = [P1++];                                  /* rg */
	R1 = R1 << 2 || R7 = [SP + 32] || NOP;        /* -65535 */
	R1 = MAX (R1, R7) || R2 = [SP + 36] || NOP;   /* 65535 */
	R1 = MIN (R1, R2) || R7 = [SP + 12] || NOP;   /* rgg */
	R1 *= R7;
	R2 = R1 >>> 31;
	R2 = R2 >> 17;
	R1 = R1 + R2 (NS);
	R1 = R1 >>> 15;
	R0 = R0 + R1 (NS);                            /* in */
	R1 = R0 << 16 (S);
	MNOP || W[I0++] = R1.H || NOP;                /* pd[pw] = s16(in) */
	MNOP || R3.H = W[I1++] || R4.H = W[I2++];     /* a = pd[pw - pd_cur], b = pd[pw - pd_next] */
	R2 = R4 -|- R3 (S);
	R2 = R2 >>> 15 (V);
	A1 = R3.H * R5.H;
	A1 += R3.H * R5.H;                            /* 65536 a */
	A1 += R4.H * R5.L;
	A1 += R4.H * R5.L;                            /* + 2 F b */
	A1 -= R3.H * R5.L;
	A1 -= R3.H * R5.L || R1 = [SP + 28] || NOP;   /* - 2 F a; F's step */
	R3.H = (A1 += R2.H * R6.H) (T);               /* v = a + m15(b - a, F) */
	R5 = R5 +|+ R1;
	R2 = R3 >>> 15 (V);
	A1 = R3.H * R6.L;
	R3.H = (A1 += R2.H * R6.H) (T);               /* m15(v, 8192) */
	R3 = R3 >>> 16;
.Lfm_e:
	[P0++] = R3;
.Lfr_out:
	R0 = 0;
	L0 = R0;
	L1 = R0;
	L2 = R0;
	SP += 40;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _nt_k_front, .-_nt_k_front

/* nt_k_er's frame: [SP] nA, [SP + 4] F0, [SP + 8] taps left, [SP + 12] 16 | 16 */
	.align 4
	.global _nt_k_er
	.type _nt_k_er, STT_FUNC
_nt_k_er:
	[--SP] = (R7:4, P5:3);
	SP += -16;
	P3 = R0;                             /* e */
	P4 = R1;                             /* n */
	P5 = R2;                             /* a */
	R0 = [P5];
	B0 = R0;
	B1 = R0;
	B2 = R0;
	B3 = R0;
	M3 = R0;                             /* pd */
	R3 = 44100 (Z);                      /* NT_PD words */
	L0 = R3;
	L1 = R3;
	L2 = R3;
	L3 = R3;
	R0 = [P5 + 4];
	[SP] = R0;                           /* nA */
	R1 = [P5 + 8];
	[SP + 4] = R1;                       /* F0 */
	R1 = [P5 + 12];                      /* sh */
	M2 = R1;
	R2 = [P5 + 16];
	[SP + 8] = R2;                       /* taps */
	P5 += 20;
	R2.H = 16;
	R2.L = 16;
	[SP + 12] = R2;
	R0 = R0 | R1;
	CC = R0 == 0;
	IF CC JUMP .Ler_fast;
	CC = R1 == 0;
	IF CC JUMP .Ler_pairs;
	R6.H = -1;
	R6.L = 1;                            /* -65535 */
	R7 = 65535 (Z);
.Ler_tap:                                /* a tap a pass */
	R0 = M3;
	R3 = [P5];
	R3 = R3 << 1;
	R3 = R3 + R0;
	I0 = R3;                             /* pd_cur's read */
	R3 = [P5 + 4];
	R3 = R3 << 1;
	R3 = R3 + R0;
	I1 = R3;                             /* pd_next's read (no fade: pd_cur's) */
	R5 = [P5 + 8];                       /* g */
	P5 += 12;
	R2 = M2;
	P0 = P3;
	P2 = [SP];                           /* nA */
	CC = P2 == 0;
	IF CC JUMP .Ler_plain;
	R4 = [SP + 4];                       /* F */
	LSETUP (.Lef_b, .Lef_e) LC0 = P2;
.Lef_b:                                  /* fading ticks: 25 packets */
	MNOP || R0.H = W[I0++] || R1.H = W[I1++];
	R0 = R0 >>> 16;
	R1 = R1 >>> 16;
	R0 = ASHIFT R0 BY R2.L;
	R1 = ASHIFT R1 BY R2.L;
	R1 = R1 - R0 (NS);
	R1 = MAX (R1, R6);
	R1 = MIN (R1, R7);
	R1 *= R4;
	R3 = R1 >>> 31;
	R3 = R3 >> 17;
	R1 = R1 + R3 (NS);
	R1 = R1 >>> 15;
	R0 = R0 + R1 (NS);                            /* v = va + m15(vb - va, F) */
	R4 += 32;
	R0 = MAX (R0, R6);
	R0 = MIN (R0, R7);
	R0 *= R5;
	R3 = R0 >>> 31;
	R3 = R3 >> 17;
	R0 = R0 + R3 (NS);
	R0 = R0 >>> 15 || R1 = [P0] || NOP;
	R0 = R0 + R1 (NS);                            /* e += m15(v, g) */
.Lef_e:
	[P0++] = R0;
.Ler_plain:
	R0 = [SP];
	R1 = P4;
	R1 = R1 - R0;
	P2 = R1;
	CC = P2 == 0;
	IF CC JUMP .Ler_next;
	LSETUP (.Lep_b, .Lep_e) LC0 = P2;
.Lep_b:                                  /* after the fade (or without one): 12 packets */
	MNOP || R1.H = W[I1++] || NOP;
	R1 = R1 >>> 16;
	R0 = ASHIFT R1 BY R2.L;
	R0 = MAX (R0, R6);
	R0 = MIN (R0, R7);
	R0 *= R5;
	R3 = R0 >>> 31;
	R3 = R3 >> 17;
	R0 = R0 + R3 (NS);
	R0 = R0 >>> 15 || R1 = [P0] || NOP;
	R0 = R0 + R1 (NS);
.Lep_e:
	[P0++] = R0;
.Ler_next:
	R0 = [SP + 8];
	R0 += -1;
	[SP + 8] = R0;
	CC = R0 == 0;
	IF !CC JUMP .Ler_tap;
	JUMP .Ler_out;
.Ler_fast:                               /* sh 0, no fade: four taps a pass (g 0 pads), 15 packets per tick */
	R6.H = -32767;
	R6.L = -32767;
	R7.H = 1;
	R7.L = 1;
.Ler_pass:
	R0 = M3;
	R2 = [SP + 8];                       /* taps left */
	R4 = [P5 + 4];                       /* tap 0: its read (b) */
	R4 = R4 << 1;
	R4 = R4 + R0;
	I0 = R4;
	R5 = [P5 + 8];                       /* g0 */
	R1 = R4;                             /* the pads read where tap 0 does */
	R3 = 0;                              /* their g */
	CC = R2 <= 1;
	IF CC JUMP 1f;
	R1 = [P5 + 16];
	R1 = R1 << 1;
	R1 = R1 + R0;
	R3 = [P5 + 20];
1:	I1 = R1;
	R5 = PACK (R3.L, R5.L);              /* g1 | g0 */
	R1 = R4;
	R3 = 0;
	CC = R2 <= 2;
	IF CC JUMP 2f;
	R1 = [P5 + 28];
	R1 = R1 << 1;
	R1 = R1 + R0;
	R3 = [P5 + 32];
2:	I2 = R1;
	R4 = R3;                             /* g2 (I0 is set: R4 is free) */
	R1 = I0;
	R3 = 0;
	CC = R2 <= 3;
	IF CC JUMP 3f;
	R1 = [P5 + 40];
	R1 = R1 << 1;
	R1 = R1 + R0;
	R3 = [P5 + 44];
3:	I3 = R1;
	R4 = PACK (R3.L, R4.L);              /* g3 | g2 */
	R2 += -4;
	[SP + 8] = R2;
	P5 += 48;
	P0 = P3;
	P1 = P3;
	LSETUP (.Lex_b, .Lex_e) LC0 = P4;
.Lex_b:
	MNOP || R0.L = W[I0++] || R0.H = W[I1++];
	MNOP || R1.L = W[I2++] || R1.H = W[I3++];
	R2 = R0 ^ R5;
	R3 = R1 ^ R4;
	R2 = R2 >>> 15 (V);                           /* -1 where v g < 0 (the gains have either sign) */
	R3 = R3 >>> 15 (V);
	A1 = R0.H * R5.H, A0 = R0.L * R5.L;
	R0.H = (A1 += R2.H * R6.H), R0.L = (A0 += R2.L * R6.L) (T);   /* m15(v1, g1) | m15(v0, g0) */
	A1 = R1.H * R4.H, A0 = R1.L * R4.L;
	R1.H = (A1 += R3.H * R6.H), R1.L = (A0 += R3.L * R6.L) (T);
	A1 = R0.H * R7.H, A0 = R0.L * R7.L (IS);
	A1 += R1.H * R7.H, A0 += R1.L * R7.L (IS) || R3 = [P0++] || NOP;
	R2 = (A0 += A1);
	R3 = R3 + R2 (NS);
.Lex_e:
	[P1++] = R3;
	R2 = [SP + 8];
	CC = R2 <= 0;
	IF !CC JUMP .Ler_pass;
	JUMP .Ler_out;
.Ler_pairs:                              /* sh 0 with a fade: two taps a pass, the crossfade as one fractional sum */
	R6.H = -32767;
	R6.L = -32767;
	R7.H = 16384;
	R7.L = 16384;
.Ler_pair:
	R0 = M3;
	R2 = [SP + 8];                       /* taps left */
	R3 = [P5];
	R3 = R3 << 1;
	R3 = R3 + R0;
	I0 = R3;                             /* tap 0: pd_cur's read */
	R3 = [P5 + 4];
	R3 = R3 << 1;
	R3 = R3 + R0;
	I1 = R3;                             /* tap 0: pd_next's */
	R5 = [P5 + 8];                       /* g0 */
	R4 = 0;                              /* g1 of a pad */
	CC = R2 <= 1;
	IF CC JUMP 1f;
	R0 = M3;
	R3 = [P5 + 12];
	R3 = R3 << 1;
	R3 = R3 + R0;
	I2 = R3;
	R3 = [P5 + 16];
	R3 = R3 << 1;
	R3 = R3 + R0;
	I3 = R3;
	R4 = [P5 + 20];
	JUMP 2f;
1:	I2 = I0;                             /* a pad reads where tap 0 does, g 0 */
	I3 = I1;
2:	R5 = PACK (R4.L, R5.L);              /* g1 | g0 */
	R2 += -2;
	[SP + 8] = R2;
	P5 += 24;
	R4 = [SP + 4];                       /* F0 */
	R4 = R4 >> 1;
	R4 = PACK (R4.L, R4.L);              /* F / 2 | F / 2 */
	P0 = P3;
	P2 = [SP];                           /* nA */
	LSETUP (.Lpf_b, .Lpf_e) LC0 = P2;
.Lpf_b:                                  /* fading: 20 packets per tick and pair */
	MNOP || R0.L = W[I0++] || R1.L = W[I1++];
	MNOP || R0.H = W[I2++] || R1.H = W[I3++];
	R3 = R1 -|- R0 (S);                           /* b - a, saturated: its sign */
	R3 = R3 >>> 15 (V);
	A1 = R0.H * R7.H, A0 = R0.L * R7.L;
	A1 += R0.H * R7.H, A0 += R0.L * R7.L;         /* 65536 a */
	A1 += R1.H * R4.H, A0 += R1.L * R4.L;
	A1 += R1.H * R4.H, A0 += R1.L * R4.L;         /* + 2 F b */
	A1 -= R0.H * R4.H, A0 -= R0.L * R4.L;
	A1 -= R0.H * R4.H, A0 -= R0.L * R4.L;         /* - 2 F a */
	R2.H = (A1 += R3.H * R6.H), R2.L = (A0 += R3.L * R6.L) (T) || R0 = [SP + 12] || NOP;   /* v = a + m15(b - a, F); 16 | 16 */
	R3 = R2 ^ R5;
	R3 = R3 >>> 15 (V);                           /* -1 where v g < 0 */
	A1 = R2.H * R5.H, A0 = R2.L * R5.L;
	R2.H = (A1 += R3.H * R6.H), R2.L = (A0 += R3.L * R6.L) (T);   /* m15(v, g) pair */
	A1 = R2.H * R7.H, A0 = R2.L * R7.L || R3 = [P0] || NOP;       /* 32768 x each; e */
	R2 = (A0 += A1);
	R2 = R2 >>> 15;
	R4 = R4 +|+ R0;                               /* F / 2 += 16 */
	R3 = R3 + R2 (NS);
.Lpf_e:
	[P0++] = R3;
	R0 = [SP];
	R1 = P4;
	R1 = R1 - R0;
	P2 = R1;
	CC = P2 == 0;
	IF CC JUMP .Lpp_next;
	LSETUP (.Lpp_b, .Lpp_e) LC0 = P2;
.Lpp_b:                                  /* after the fade: 10 packets per tick and pair */
	MNOP || R1.L = W[I1++] || R1.H = W[I3++];
	R3 = R1 ^ R5;
	R3 = R3 >>> 15 (V);
	A1 = R1.H * R5.H, A0 = R1.L * R5.L;
	R2.H = (A1 += R3.H * R6.H), R2.L = (A0 += R3.L * R6.L) (T);
	A1 = R2.H * R7.H, A0 = R2.L * R7.L || R3 = [P0] || NOP;
	R2 = (A0 += A1);
	R2 = R2 >>> 15;
	R3 = R3 + R2 (NS);
.Lpp_e:
	[P0++] = R3;
.Lpp_next:
	R2 = [SP + 8];
	CC = R2 <= 0;
	IF !CC JUMP .Ler_pair;
	JUMP .Ler_out;
.Ler_out:
	R0 = 0;
	L0 = R0;
	L1 = R0;
	L2 = R0;
	L3 = R0;
	SP += 16;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _nt_k_er, .-_nt_k_er
/* the half-band coefficients nt_hb[0 .. 7] in pairs: R4 = hb1 | hb0, R5 = hb3 | hb2, R6 = hb5 | hb4, R7 = hb7 | hb6 */
.macro HBCOEF
	R4.H = -3177;
	R4.L = 10327;
	R5.H = -893;
	R5.L = 1617;
	R6.H = -236;
	R6.L = 481;
	R7.H = -26;
	R7.L = 96;
.endm

	.align 4
	.global _nt_k_dec
	.type _nt_k_dec, STT_FUNC
_nt_k_dec:
	[--SP] = (R7:4);
	P1 = R0;                             /* x out */
	P0 = R1;                             /* buf */
	P2 = R2;                             /* a */
	R0 = [P2];                           /* hbx */
	B0 = R0;
	B1 = R0;
	B2 = R0;
	R3 = 64 (Z);
	L0 = R3;
	L1 = R3;
	L2 = R3;
	R3 = [P2 + 4];                       /* hbi */
	R2 = 31;
	R3 = R3 & R2;
	R3 = R3 << 1;
	R3 = R3 + R0;
	I2 = R3;                             /* hbx[hbi & 31] */
	R3 = 32;
	M0 = R3;
	R3 = 16384 (Z);
	HBCOEF
	P2 = 32;
	LSETUP (.Ldc_b, .Ldc_e) LC0 = P2;
.Ldc_b:                                  /* two input samples, one output: 28 packets */
	R0 = W[P0++] (X);
	R1 = W[P0++] (X);
	MNOP || W[I2++] = R0.L || NOP;
	MNOP || W[I2++] = R1.L || NOP;
	I1 = I2;
	I1 += M0;                            /* the centre's word (hbi - 16: even): the centre .L, the taps above .H */
	I0 = I1;
	I0 -= 4;                             /* the taps below: .H */
	MNOP || R0 = [I0--] || R1 = [I1++];
	A0 = R1.L * R3.L (IS);               /* 16384 centre */
	A0 += R1.H * R4.L (IS);
	A0 += R0.H * R4.L (IS) || R1 = [I1++] || NOP;
	A0 += R1.H * R4.H (IS) || R0 = [I0--] || NOP;
	A0 += R0.H * R4.H (IS) || R1 = [I1++] || NOP;
	A0 += R1.H * R5.L (IS) || R0 = [I0--] || NOP;
	A0 += R0.H * R5.L (IS) || R1 = [I1++] || NOP;
	A0 += R1.H * R5.H (IS) || R0 = [I0--] || NOP;
	A0 += R0.H * R5.H (IS) || R1 = [I1++] || NOP;
	A0 += R1.H * R6.L (IS) || R0 = [I0--] || NOP;
	A0 += R0.H * R6.L (IS) || R1 = [I1++] || NOP;
	A0 += R1.H * R6.H (IS) || R0 = [I0--] || NOP;
	A0 += R0.H * R6.H (IS) || R1 = [I1++] || NOP;
	A0 += R1.H * R7.L (IS) || R0 = [I0--] || NOP;
	A0 += R0.H * R7.L (IS) || R1 = [I1++] || NOP;
	A0 += R1.H * R7.H (IS) || R0 = [I0--] || NOP;
	R2 = (A0 += R0.H * R7.H) (IS);       /* a */
	R1 = R2 >>> 31;
	R1 = R1 >> 17;
	R2 = R2 + R1 (NS);
	R2 = R2 >>> 15;                      /* nt_shr(a, 15) */
.Ldc_e:
	[P1++] = R2;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	L2 = R0;
	(R7:4) = [SP++];
	RTS;
	.size _nt_k_dec, .-_nt_k_dec

/* nt_k_int's frame: [SP] -NT_HW, [SP + 4] NT_HW, [SP + 8] 32768, [SP + 12] hbo (running) */
	.align 4
	.global _nt_k_int
	.type _nt_k_int, STT_FUNC
_nt_k_int:
	[--SP] = (R7:4, P5:3);
	SP += -16;
	P3 = R0;                             /* wet, 64 */
	P0 = R1;                             /* x, 32 */
	P5 = R2;                             /* a */
	R0 = [P5];                           /* hbw */
	B2 = R0;
	R3 = 64 (Z);
	L2 = R3;
	R3 = [P5 + 4];                       /* hbj */
	R2 = 15;
	R3 = R3 & R2;
	R3 = R3 << 2;
	R3 = R3 + R0;
	I2 = R3;                             /* hbw[hbj & 15]: the oldest */
	R3 = [P5 + 8];
	[SP + 12] = R3;                      /* hbo */
	R3.H = -4;
	R3.L = 1;                            /* -262143 */
	[SP] = R3;
	R3.H = 3;
	R3.L = -1;                           /* 262143 */
	[SP + 4] = R3;
	R3 = 32768 (Z);
	[SP + 8] = R3;
	P1 = [P5 + 12];                      /* kd: h, 48 words, then the split words, 48 */
	R0 = P1;
	R1 = 192 (Z);
	R0 = R0 + R1;
	P2 = R0;
	P4 = 16;
	LSETUP (.Lin_h, .Lin_he) LC0 = P4;
.Lin_h:                                  /* the history, oldest first, and its split words (h + 32768) >> 16 | h & 0xffff */
	R2 = [I2++];
	R3 = R2 + R3 (NS) || [P1++] = R2 || NOP;
	R3 = R3 >>> 16;
	R3 = PACK (R3.L, R2.L);
	[P2++] = R3;
.Lin_he:
	R3 = [SP + 8];
	R0 = P2;
	R0 += -32;
	I0 = R0;                             /* split word 8: m for the first sample (its newest is 16) */
	R0 += 4;
	I1 = R0;                             /* m + 1 */
	R3 = 36;
	M2 = R3;
	R3 = 28;
	M3 = R3;
	HBCOEF
	P4 = 32;
	LSETUP (.Lin_b, .Lin_e) LC0 = P4;
.Lin_b:                                  /* one tank sample, two wet samples */
	R2 = [P0++];
	R3 = [SP];
	R2 = MAX (R2, R3) || R0 = [SP + 4] || NOP;
	R2 = MIN (R2, R0) || R3 = [SP + 8] || NOP;    /* to 8x full scale */
	R3 = R2 + R3 (NS) || [P1++] = R2 || NOP;
	R3 = R3 >>> 16;
	R3 = PACK (R3.L, R2.L);
	MNOP || [P2++] = R3 || R0 = [I0--];
	R2 = [SP + 12];
	MNOP || [P3++] = R2 || R1 = [I1++];           /* the odd sample before: hbo */
	R2 = [P1 + -36];
	MNOP || [P3++] = R2 || NOP;                   /* hbw[m & 15] */
	A1 = R0.H * R4.L, A0 = R0.L * R4.L (IS);
	A1 += R1.H * R4.L, A0 += R1.L * R4.L (IS) || R0 = [I0--] || NOP;
	A1 += R0.H * R4.H, A0 += R0.L * R4.H (IS) || R1 = [I1++] || NOP;
	A1 += R1.H * R4.H, A0 += R1.L * R4.H (IS) || R0 = [I0--] || NOP;
	A1 += R0.H * R5.L, A0 += R0.L * R5.L (IS) || R1 = [I1++] || NOP;
	A1 += R1.H * R5.L, A0 += R1.L * R5.L (IS) || R0 = [I0--] || NOP;
	A1 += R0.H * R5.H, A0 += R0.L * R5.H (IS) || R1 = [I1++] || NOP;
	A1 += R1.H * R5.H, A0 += R1.L * R5.H (IS) || R0 = [I0--] || NOP;
	A1 += R0.H * R6.L, A0 += R0.L * R6.L (IS) || R1 = [I1++] || NOP;
	A1 += R1.H * R6.L, A0 += R1.L * R6.L (IS) || R0 = [I0--] || NOP;
	A1 += R0.H * R6.H, A0 += R0.L * R6.H (IS) || R1 = [I1++] || NOP;
	A1 += R1.H * R6.H, A0 += R1.L * R6.H (IS) || R0 = [I0--] || NOP;
	A1 += R0.H * R7.L, A0 += R0.L * R7.L (IS) || R1 = [I1++] || NOP;
	A1 += R1.H * R7.L, A0 += R1.L * R7.L (IS) || R0 = [I0--] || NOP;
	A1 += R0.H * R7.H, A0 += R0.L * R7.H (IS) || R1 = [I1++] || NOP;
	A1 += R1.H * R7.H, A0 += R1.L * R7.H (IS);
	A1 = A1 << 16;
	A0 += A1;                                     /* nt_hbi's sum */
	A1 = 0;
	CC = A0 < A1;
	A0 = ABS A0;
	A0 = A0 >>> 14;
	R2 = A0;
	R3 = -R2 (NS);
	IF CC R2 = R3;                                /* hbo, toward zero */
	[SP + 12] = R2;
	I0 += M2;
.Lin_e:
	I1 -= M3;
	P1 += -64;
	P4 = 16;
	LSETUP (.Lin_w, .Lin_we) LC0 = P4;
.Lin_w:                                  /* the ring: the last 16 */
	R2 = [P1++];
.Lin_we:
	[I2++] = R2;
	R2 = [SP + 12];
	[P5 + 8] = R2;
	R0 = 0;
	L2 = R0;
	SP += 16;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _nt_k_int, .-_nt_k_int
#endif
