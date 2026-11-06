/* Granular's grain and output passes, hand-scheduled for the Blackfin: the same arithmetic as granular.c's gr_grain and
 * gr_out (kept there for host builds and GR_C_REF), bit for bit, in fewer instruction packets.
 *
 * void gr_grain(int32_t *acc, const int16_t *data, grain_t *g, int32_t cnt)
 *   cnt >= 1 samples of one grain added to acc[0 .. cnt): idx = pos >> 10, f = pos & 1023,
 *   s = a + ((b - a) f >> 10) with a = data[idx], b = data[idx + 1] (b f - a f as one integer MAC pair: exact, |.| <
 *   2^27), w = sine[(wph >> 19) + 4096], w2 = w w >> 15 (0 .. 32768: kept in 32 bits, so a table word of -32768 gives
 *   32768 as the C does), acc[k] += s w2 >> 15 (s signed x w2 unsigned, the mixed-mode MAC1 product); then pos += rate,
 *   wph += winc. g = { pos, rate, wph, winc }: pos and wph go back, advanced cnt times. 18 packets per sample.
 *   pos rides in I3 (+= M3 = rate) and wph in I2 (+= M2 = winc), each step in a parallel slot; the next sample's
 *   addresses are worked out and its three points loaded while this sample's sum is formed, so the loop reads no
 *   point the C would not (the last sample runs without the look-ahead). The sum's store runs one sample behind: the
 *   first one writes back the word in front of acc unchanged (granular.c keeps a spare word in front of the block).
 * void gr_out(int16_t *out, int32_t *acc, int32_t gain, int32_t sg)
 *   out[n] = sat16((((acc[n] gain) >> 15) sg) >> 13), acc[n] = 0, n = 0 .. 127: the products are the low 32 bits
 *   (Dreg *= Dreg, as the C's int multiply), sat16 as a saturating shift into the high half. 5 packets per sample.
 *
 * The sample data and the sine table are in SDRAM: gr_grain's three point loads per sample are the memory-bound part
 * (bcore counts packets, not the SDRAM / cache stalls). L0..L3 are set to 0 here; granular.c's gr_render restores
 * the caller's values after the block. M2 and M3 are saved and restored; M1 is not touched (R246's PmSynth keeps its
 * frame there).
 * int32_t gr_peak_skip(const int16_t *grp, int32_t groups)
 *   recorded_length's scan at note_on: how many whole groups of 16 peak buckets, from the top group (grp = its lowest
 *   bucket, word-aligned) down, hold nothing above 96 (groups >= 1 to check). Per group the halves' maximum by vector
 *   MAX, two loads per packet, then one saturating subtract against 96: about 11 packets per group instead of about
 *   64. It is called outside gr_render, so it saves and restores L0, L1 and M0 itself, and leaves LC0 at 0 (it can
 *   leave its loop early).
 * Host builds (the harness renders) assemble this file empty.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

/* one sample, and the next one's points: on entry R4 a, R5 b, R2 w, R1 f of this sample, R3 the sample before's sum,
 * I3 the next pos, I2 the next wph; P0 data, P3 the sine table + 4096 words, R7 the EXTRACT pattern. Its last packet,
 * R3 = R3 + R6 (the sum), follows the macro as the loop's end. */
.macro GR_STEP
	R6 = I3;                                         /* next pos */
	R0 = R6 >> 10 || I3 += M3 || [I1++] = R3;        /* next idx; the sample before's sum */
	A1 = R5.L * R1.L (IS);                           /* b f */
	P1 = R0;
	R3 = (A1 -= R4.L * R1.L) (IS);                   /* (b - a) f */
	R0 = I2;                                         /* next wph */
	R1 = EXTRACT(R6, R7.L) (Z) || I2 += M2;          /* next f */
	P1 = P0 + (P1 << 1);
	R3 = R3 >>> 10 || R6 = [I0++];                   /* acc[k] */
	R0 = R0 >> 19;
	R3 = R3 + R4 (NS);                               /* s */
	P2 = R0;
	R2 = R2.L * R2.L (IS) || R4 = W[P1++] (X);       /* w w; next a */
	P2 = P3 + (P2 << 1);
	R2 = R2 >> 15 || R5 = W[P1] (X);                 /* w2; next b */
	R3 = R3.L * R2.L (M, IS);                        /* s w2 */
	R3 = R3 >>> 15 || R2 = W[P2] (X);                /* next w */
.endm

	.align 4
	.global _gr_grain
	.type _gr_grain, STT_FUNC
_gr_grain:
	[--SP] = (R7:4, P5:3);
	[--SP] = M2;
	[--SP] = M3;
	P5 = R2;                                         /* g */
	P0 = R1;                                         /* data */
	I0 = R0;
	R0 += -4;
	I1 = R0;
	P1 = R0;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	L2 = R0;
	L3 = R0;
	R3 = [P1];                                       /* the word in front of acc, written back unchanged */
	P4 = [SP + 48];                                  /* cnt (12 + 7 regs + M2, M3) */
	P3.H = 0x0102;
	P3.L = 0x05E8;                                   /* OP1_SINE_TABLE 0x0101E5E8 + 4096 words */
	R7 = 10 (Z);                                     /* EXTRACT: position 0, length 10 */
	R6 = [P5];                                       /* pos */
	R5 = [P5 + 4];                                   /* rate */
	R0 = [P5 + 8];                                   /* wph */
	R2 = [P5 + 12];                                  /* winc */
	M3 = R5;
	M2 = R2;
	R2 = R2 + R0 (NS);
	I2 = R2;                                         /* wph of sample 1 */
	R2 = R0 >> 19;
	R1 = R5 + R6 (NS);
	I3 = R1;                                         /* pos of sample 1 */
	P2 = R2;
	R1 = R6 >> 10;
	P1 = R1;
	P2 = P3 + (P2 << 1);
	P1 = P0 + (P1 << 1);
	R1 = EXTRACT(R6, R7.L) (Z) || R2 = W[P2] (X);
	R4 = W[P1++] (X);
	R5 = W[P1] (X);
	P4 += -1;
	CC = P4 == 0;
	IF CC JUMP .Lg_last;
	LSETUP (.Lg_b, .Lg_e) LC0 = P4;
.Lg_b:
	GR_STEP
.Lg_e:
	R3 = R3 + R6 (NS);
.Lg_last:
	A1 = R5.L * R1.L (IS) || [I1++] = R3;
	R3 = (A1 -= R4.L * R1.L) (IS) || R6 = [I0++];
	R3 = R3 >>> 10;
	R3 = R3 + R4 (NS);
	R2 = R2.L * R2.L (IS);
	R2 = R2 >> 15;
	R3 = R3.L * R2.L (M, IS);
	R3 = R3 >>> 15;
	R3 = R3 + R6 (NS);
	[I1++] = R3;
	R0 = I3;
	[P5] = R0;                                       /* pos after cnt samples */
	R0 = I2;
	[P5 + 8] = R0;                                   /* wph */
	M3 = [SP++];
	M2 = [SP++];
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _gr_grain, .-_gr_grain

	.align 4
	.global _gr_out
	.type _gr_out, STT_FUNC
_gr_out:
	[--SP] = (R7:4);
	I2 = R0;                                         /* out */
	I0 = R1;                                         /* acc, read */
	I1 = R1;                                         /* acc, cleared */
	R4 = R2;                                         /* gain */
	R5 = [SP + 28];                                  /* sg (12 + 4 regs) */
	R7 = 0;
	L0 = R7;
	L1 = R7;
	L2 = R7;
	R0 = [I0++];
	/* sample 0 alone: its result waits in R1 for the loop's first store */
	R0 *= R4;
	R0 = R0 >>> 15 || [I1++] = R7;
	R0 *= R5;
	R0 = R0 >>> 13;
	R1 = R0 << 16 (S);
	R0 = [I0++];
	P1 = 63;
	LSETUP (.Lo_b, .Lo_e) LC0 = P1;
.Lo_b:                                               /* samples k (R0), k + 1 (R1); out[k - 1] waits in R1 */
	R0 *= R4;
	R0 = R0 >>> 15 || [I1++] = R7 || R6 = [I0++];   /* acc[k] = 0; acc[k + 1] */
	R0 *= R5;
	R0 = R0 >>> 13 || W[I2++] = R1.H;               /* out[k - 1] */
	R0 = R0 << 16 (S);
	R6 *= R4;
	R6 = R6 >>> 15 || W[I2++] = R0.H;               /* out[k] */
	R6 *= R5;
	R6 = R6 >>> 13 || [I1++] = R7 || R0 = [I0++];   /* acc[k + 1] = 0; acc[k + 2] */
.Lo_e:
	R1 = R6 << 16 (S);
	/* sample 127 (in R0) */
	R0 *= R4;
	R0 = R0 >>> 15 || [I1++] = R7;
	R0 *= R5;
	R0 = R0 >>> 13 || W[I2++] = R1.H;
	R0 = R0 << 16 (S);
	W[I2++] = R0.H;
	(R7:4) = [SP++];
	RTS;
	.size _gr_out, .-_gr_out

/* one group of 16 peak buckets (8 words, w7 .. w0 from the top): the halves' maximum into R2. The pairs load one
 * packet ahead into R0 / R1 and R3 / R6 by turns (no packet loads a register it reads); the last load is the next
 * group's top pair (at the last group, the 4 bytes below the peaks: the buffer's own data pointer, read and not
 * used). On entry R0 / R1 hold w7 / w6. */
.macro PK_GROUP
	R2 = MAX(R0, R1) (V) || R3 = [I0 ++ M0] || R6 = [I1 ++ M0];
	R2 = MAX(R2, R3) (V) || R0 = [I0 ++ M0] || R1 = [I1 ++ M0];
	R2 = MAX(R2, R6) (V);
	R2 = MAX(R2, R0) (V) || R3 = [I0 ++ M0] || R6 = [I1 ++ M0];
	R2 = MAX(R2, R1) (V);
	R2 = MAX(R2, R3) (V) || R0 = [I0 ++ M0] || R1 = [I1 ++ M0];
.endm
#define PK_GROUP_END R2 = MAX(R2, R6) (V)
/* a half above 96 in R2: 96 -| half (S) is negative there */
.macro PK_CHECK
	R3 = R7 -|- R2 (S);
	R3 = R3 & R5;
	CC = R3 == 0;
	IF !CC JUMP .Lp_found;
.endm

	.align 4
	.global _gr_peak_skip
	.type _gr_peak_skip, STT_FUNC
_gr_peak_skip:
	[--SP] = (R7:4);
	[--SP] = M0;
	[--SP] = L0;
	[--SP] = L1;
	P2 = R1;                                         /* groups >= 1 */
	[--SP] = R1;
	R2 = 28;
	R2 = R0 + R2 (NS);
	I0 = R2;                                         /* word 7 of the top group (buckets 14, 15) */
	R2 += -4;
	I1 = R2;                                         /* word 6 */
	R4 = -36;
	R4 = R4 + R2 (NS);                               /* word 7 - 40: (R4 - I0) / 32 = the group checked */
	R2 = 0;
	L0 = R2;
	L1 = R2;
	M0 = -8 (X);
	R5.H = 0x8000;
	R5.L = 0x8000;
	R7.H = 96;
	R7.L = 96;
	R0 = [I0 ++ M0] || R1 = [I1 ++ M0];
	PK_GROUP
	PK_GROUP_END;
	P2 += -1;
	CC = P2 == 0;
	IF CC JUMP .Lp_last;
	LSETUP (.Lp_b, .Lp_e) LC0 = P2;
.Lp_b:
	PK_CHECK
	PK_GROUP
.Lp_e:
	PK_GROUP_END;
.Lp_last:
	PK_CHECK
	R0 = [SP];                                        /* none: every group */
	JUMP .Lp_done;
.Lp_found:
	R0 = I0;
	R0 = R4 - R0;
	R0 >>= 5;
.Lp_done:
	R1 = 0;
	LC0 = R1;                                        /* the loop may have been left early */
	SP += 4;
	L1 = [SP++];
	L0 = [SP++];
	M0 = [SP++];
	(R7:4) = [SP++];
	RTS;
	.size _gr_peak_skip, .-_gr_peak_skip
#endif
