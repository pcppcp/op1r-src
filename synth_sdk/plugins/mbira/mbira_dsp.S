/* MBIRA's partial passes for steady pitch (no glide), hand-scheduled for the Blackfin: the same arithmetic as
 * mbira.c's pass2 / pass1 (and its host build), bit for bit (the loads and the block's store ride in parallel slots,
 * the two products are one multiply and one multiply-accumulate).
 *
 * void mb_pass2(int32_t *acc, const uint32_t *gains, uint32_t *ph, uint32_t ca, uint32_t cb, int first, const int16_t *tab)
 *   two partials: acc[j] (=|+=) (sin(pa) * ga + sin(pb) * gb) >> 15 for the 128 samples, pa += ca and pb += cb before
 *   each; gains[i] = gb << 16 | ga for the 8 samples of group i (both 0 .. 16384); ph = { pa, pb }, updated
 * void mb_pass1(int32_t *acc, const uint32_t *gains, uint32_t *ph, uint32_t ca, int first, const int16_t *tab)
 *   one partial: acc[j] (=|+=) (sin(pa) * ga) >> 16; gains[i] = ga (0 .. 32767); ph = { pa }, updated
 * void mb_pass2g(int32_t *acc, const uint32_t *gains, uint32_t *ph, const uint32_t *ia, const uint32_t *ib, int first,
 *                const int16_t *tab)
 * void mb_pass1g(int32_t *acc, const uint32_t *gains, uint32_t *ph, const uint32_t *ia, int first, const int16_t *tab)
 *   the same with the pitch gliding: sample j adds ia[j] (ib[j]) to the phase; the arrays hold 129 words (the last
 *   one is read ahead and not used)
 * void mb_ramp(uint32_t *dst, uint32_t c, int32_t d): dst[j] = c + j d for j = 0 .. 128 (the gliding increments)
 *
 * sin(x) = OP1_SINE_TABLE[x >> 19] (0x0101E5E8, 8192 int16), read from the first quadrant only (entries 0 .. 2048,
 * 4 KB; tab = those entries: the core's L1 copy, op1_l1_sine_quarter, or OP1_SINE_TABLE itself): k = EXTRACT(x, bits 19..30) sign-extended (-2048 .. 2047, the place around the half-wave's peak), the
 * value tab[|k|], negated when x < 0. The unit's table is lrint(32767 sin(2 pi k / 8192)), exactly symmetric, so
 * this is the same number as the whole-table read (mbira.c's sine()); with all 16 KB the partials' reads swept every
 * line of the 16 KB data cache each block. A pair takes the two signs together: PACK of the phases' high halves,
 * >>> 15 per half = 0 / -1, then (v ^ m) - m per half. 18 packets per sample for a pair adding (17 writing), 11 for
 * one partial (10); the glide variants the same.
 *
 * The block's store runs one sample behind (in the next sample's first packet); the first one writes back the word
 * in front of acc unchanged, so nothing outside the block changes (mbira.c keeps a pass's gains there). The 8
 * samples of a group are an inner hardware loop (LC1), not unrolled. R7 holds EXTRACT's pattern (0x130C: 12 bits from
 * bit 19) through the loops.
 * Host builds (screen and golden harnesses) assemble it empty: mbira.c keeps the C passes there.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

/* the folded table reads of a pair: R0, R1 = the phases (already stepped), out R4 = sb << 16 | sa (halves) */
.macro PAIR_READ
	R4 = EXTRACT(R0, R7.L) (X);
	R1 = R1 + R3 (NS);
	R4 = ABS R4;
	R5 = EXTRACT(R1, R7.L) (X);
	P1 = R4;
	R5 = ABS R5;
	P2 = R5;
	P1 = P0 + (P1 << 1);
	P2 = P0 + (P2 << 1);
	R5 = PACK(R1.H, R0.H) || R4.L = W[P1] || NOP;
	R5 = R5 >>> 15 (V) || R4.H = W[P2] || NOP;
	R4 = R4 ^ R5;
	R4 = R4 -|- R5;
.endm
/* the same, gliding: the next increments come from I2 / I3 in free parallel slots (after this sample's use) */
.macro PAIRG_READ
	R4 = EXTRACT(R0, R7.L) (X) || R2 = [I2++] || NOP;
	R1 = R1 + R3 (NS);
	R4 = ABS R4;
	R5 = EXTRACT(R1, R7.L) (X) || R3 = [I3++] || NOP;
	P1 = R4;
	R5 = ABS R5;
	P2 = R5;
	P1 = P0 + (P1 << 1);
	P2 = P0 + (P2 << 1);
	R5 = PACK(R1.H, R0.H) || R4.L = W[P1] || NOP;
	R5 = R5 >>> 15 (V) || R4.H = W[P2] || NOP;
	R4 = R4 ^ R5;
	R4 = R4 -|- R5;
.endm
/* one partial: R0 = the phase (already stepped), out R4 = s (32 bits) */
.macro ONE_READ
	R4 = EXTRACT(R0, R7.L) (X);
	R4 = ABS R4;
	P1 = R4;
	P1 = P0 + (P1 << 1);
	R5 = R0 >>> 31 || R4 = W[P1] (X) || NOP;
	R4 = R4 ^ R5;
	R4 = R4 - R5;
.endm
.macro ONEG_READ
	R4 = EXTRACT(R0, R7.L) (X) || R2 = [I2++] || NOP;
	R4 = ABS R4;
	P1 = R4;
	P1 = P0 + (P1 << 1);
	R5 = R0 >>> 31 || R4 = W[P1] (X) || NOP;
	R4 = R4 ^ R5;
	R4 = R4 - R5;
.endm

/* a pair adding to the block: all but the last packet (in LAST, where the loop's end label can sit) */
.macro PAIR_ADD_BODY
	R0 = R0 + R2 (NS) || [I1++] = R4 || NOP;
	PAIR_READ
	A0 = R4.L * R6.L (IS) || R5 = [I0++] || NOP;
	R4 = (A0 += R4.H * R6.H) (IS);
	R4 = R4 >>> 15;
.endm
.macro PAIRG_ADD_BODY
	R0 = R0 + R2 (NS) || [I1++] = R4 || NOP;
	PAIRG_READ
	A0 = R4.L * R6.L (IS) || R5 = [I0++] || NOP;
	R4 = (A0 += R4.H * R6.H) (IS);
	R4 = R4 >>> 15;
.endm
.macro ADD_LAST
	R4 = R4 + R5 (NS);
.endm
.macro PAIR_SET_BODY
	R0 = R0 + R2 (NS) || [I1++] = R4 || NOP;
	PAIR_READ
	A0 = R4.L * R6.L (IS);
	R4 = (A0 += R4.H * R6.H) (IS);
.endm
.macro PAIRG_SET_BODY
	R0 = R0 + R2 (NS) || [I1++] = R4 || NOP;
	PAIRG_READ
	A0 = R4.L * R6.L (IS);
	R4 = (A0 += R4.H * R6.H) (IS);
.endm
.macro PAIR_SET_LAST
	R4 = R4 >>> 15;
.endm
.macro ONE_ADD_BODY
	R0 = R0 + R2 (NS) || [I1++] = R4 || NOP;
	ONE_READ
	R4 = R4.L * R6.L (IS) || R5 = [I0++] || NOP;
	R4 = R4 >>> 16;
.endm
.macro ONEG_ADD_BODY
	R0 = R0 + R2 (NS) || [I1++] = R4 || NOP;
	ONEG_READ
	R4 = R4.L * R6.L (IS) || R5 = [I0++] || NOP;
	R4 = R4 >>> 16;
.endm
.macro ONE_SET_BODY
	R0 = R0 + R2 (NS) || [I1++] = R4 || NOP;
	ONE_READ
	R4 = R4.L * R6.L (IS);
.endm
.macro ONEG_SET_BODY
	R0 = R0 + R2 (NS) || [I1++] = R4 || NOP;
	ONEG_READ
	R4 = R4.L * R6.L (IS);
.endm
.macro ONE_SET_LAST
	R4 = R4 >>> 16;
.endm

/* a loop over the 16 groups of 8 samples, the samples an inner loop (small code: the I-cache is shared with R246):
 * the gains of group 0 come before it, each group's last packet loads the next group's (the 17th read lands on the
 * word after gains, which is read and not used); P4 = 16 on entry */
.macro GROUPS body, last, lb, le, ib, ie
	R6 = [P3++];
	LSETUP (\lb, \le) LC0 = P4;
	P4 = 8;
\lb:
	LSETUP (\ib, \ie) LC1 = P4;
\ib:
	\body
\ie:
	\last
\le:
	R6 = [P3++];
.endm

	.align 4
	.global _mb_ramp
	.type _mb_ramp, STT_FUNC
_mb_ramp:                                    /* (no packet stores a register its 32-bit op writes) */
	I2 = R0;
	R0 = 0;
	L2 = R0;
	P1 = 64;
	LSETUP (.Lramp_b, .Lramp_e) LC0 = P1;
.Lramp_b:
	R3 = R1 + R2 (NS) || [I2++] = R1 || NOP;
.Lramp_e:
	R1 = R3 + R2 (NS) || [I2++] = R3 || NOP;
	[I2++] = R1;
	RTS;
	.size _mb_ramp, .-_mb_ramp

/* the common entry: R0 = acc, R1 = gains, R2 = ph; saves R7:4, P5:3 (7 words: the stack arguments at SP + 40 ..),
 * L0..L3 = 0, I0 = acc (read), I1 = acc - 4 (written one sample behind), R4 = the word in front (written back
 * unchanged by the first store), P5 = ph, P3 = gains, P4 = 16 groups; each function then loads P0 = tab (its last
 * argument: SP + 48 after one stack argument less, SP + 52 with three) */
.macro ENTRY
	[--SP] = (R7:4, P5:3);
	P5 = R2;
	P3 = R1;
	R3 = 0;
	L0 = R3;
	L1 = R3;
	L2 = R3;
	L3 = R3;
	I0 = R0;
	R0 += -4;
	I1 = R0;
	P4 = R0;
	R4 = [P4];
	P4 = 16;
.endm

	.align 4
	.global _mb_pass2g
	.type _mb_pass2g, STT_FUNC
_mb_pass2g:
	ENTRY
	R2 = [SP + 40];                      /* ia */
	I2 = R2;
	R3 = [SP + 44];                      /* ib */
	I3 = R3;
	R6 = [SP + 48];                      /* first */
	P0 = [SP + 52];                      /* tab */
	R7 = 0x130C (Z);
	R0 = [P5];
	R1 = [P5 + 4];
	R2 = [I2++];
	R3 = [I3++];
	CC = R6 == 0;
	IF CC JUMP .Lp2g_add;
	GROUPS PAIRG_SET_BODY, PAIR_SET_LAST, .Lp2gs_b, .Lp2gs_e, .Lp2gs_ib, .Lp2gs_ie
	JUMP .Lp2g_done;
.Lp2g_add:
	GROUPS PAIRG_ADD_BODY, ADD_LAST, .Lp2ga_b, .Lp2ga_e, .Lp2ga_ib, .Lp2ga_ie
.Lp2g_done:
	[I1++] = R4;
	[P5] = R0;
	[P5 + 4] = R1;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _mb_pass2g, .-_mb_pass2g

	.align 4
	.global _mb_pass1g
	.type _mb_pass1g, STT_FUNC
_mb_pass1g:
	ENTRY
	R2 = [SP + 40];                      /* ia */
	I2 = R2;
	R6 = [SP + 44];                      /* first */
	P0 = [SP + 48];                      /* tab */
	R7 = 0x130C (Z);
	R0 = [P5];
	R2 = [I2++];
	CC = R6 == 0;
	IF CC JUMP .Lp1g_add;
	GROUPS ONEG_SET_BODY, ONE_SET_LAST, .Lp1gs_b, .Lp1gs_e, .Lp1gs_ib, .Lp1gs_ie
	JUMP .Lp1g_done;
.Lp1g_add:
	GROUPS ONEG_ADD_BODY, ADD_LAST, .Lp1ga_b, .Lp1ga_e, .Lp1ga_ib, .Lp1ga_ie
.Lp1g_done:
	[I1++] = R4;
	[P5] = R0;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _mb_pass1g, .-_mb_pass1g

	.align 4
	.global _mb_pass2
	.type _mb_pass2, STT_FUNC
_mb_pass2:
	ENTRY
	R2 = [SP + 40];                      /* ca */
	R3 = [SP + 44];                      /* cb */
	R6 = [SP + 48];                      /* first */
	P0 = [SP + 52];                      /* tab */
	R7 = 0x130C (Z);
	R0 = [P5];                           /* pa */
	R1 = [P5 + 4];                       /* pb */
	CC = R6 == 0;
	IF CC JUMP .Lp2_add;
	GROUPS PAIR_SET_BODY, PAIR_SET_LAST, .Lp2s_b, .Lp2s_e, .Lp2s_ib, .Lp2s_ie
	JUMP .Lp2_done;
.Lp2_add:
	GROUPS PAIR_ADD_BODY, ADD_LAST, .Lp2a_b, .Lp2a_e, .Lp2a_ib, .Lp2a_ie
.Lp2_done:
	[I1++] = R4;
	[P5] = R0;
	[P5 + 4] = R1;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _mb_pass2, .-_mb_pass2

	.align 4
	.global _mb_pass1
	.type _mb_pass1, STT_FUNC
_mb_pass1:
	ENTRY
	R2 = [SP + 40];                      /* ca */
	R6 = [SP + 44];                      /* first */
	P0 = [SP + 48];                      /* tab */
	R7 = 0x130C (Z);
	R0 = [P5];
	CC = R6 == 0;
	IF CC JUMP .Lp1_add;
	GROUPS ONE_SET_BODY, ONE_SET_LAST, .Lp1s_b, .Lp1s_e, .Lp1s_ib, .Lp1s_ie
	JUMP .Lp1_done;
.Lp1_add:
	GROUPS ONE_ADD_BODY, ADD_LAST, .Lp1a_b, .Lp1a_e, .Lp1a_ib, .Lp1a_ie
.Lp1_done:
	[I1++] = R4;
	[P5] = R0;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _mb_pass1, .-_mb_pass1

/* ---- the box and the voice's output (no wah) ----
 * void mb_box(const int32_t *acc, int32_t *bd, int32_t *st, int32_t bx, int32_t dbx8): the box's band-pass over the 16
 *   groups of 8 (mbira.c's C loop, bit for bit), st = { blo, bbd, bo, bp } in / out (the voice's four words), bx the
 *   box level at the block's start, dbx8 its step per group; per group i: in = acc[8 i] + .. + acc[8 i + 7],
 *   bd[2 i] = bp, bd[2 i + 1] = (bo - bp) >> 3, bp = bo, blo = clamp(blo + (bbd >> 3)), bbd = clamp(bbd + (((in >> 3)
 *   - blo - (bbd >> 2)) >> 3)) (clamp: -131071 .. 131071), bo = ((((bbd (bx >> 2)) >> 13) 3) >> 2, bx += dbx8
 *   (f = 1/8 at the groups' 11.025 kHz: 220 Hz; 2026-11-03, was 1/4 for 5.5 kHz)
 * void mb_out(const int32_t *acc, int16_t *out, const int32_t *bd, int32_t g): out[j] = sat16(((acc[j] + o_j) * g) >> 15)
 *   with the box's output o_j stepping through each group of 8: o = bd[2 i], then o += bd[2 i + 1] before each sample
 *   (bd = 0: no box). sat16(x >> 15) is the high half of x << 1 with saturation. */
	.align 4
	.global _mb_box
	.type _mb_box, STT_FUNC
_mb_box:
	[--SP] = (R7:4, P5:5);               /* 5 words: bx at SP + 20 + 12 */
	R3 = 0;
	L0 = R3;
	L1 = R3;
	I0 = R0;
	I1 = R1;
	P0 = R2;
	R6 = [SP + 32];                      /* bx */
	R0 = [SP + 36];                      /* dbx8 */
	P2 = R0;
	R2 = [P0];                           /* blo */
	R3 = [P0 + 4];                       /* bbd */
	R4 = [P0 + 8];                       /* bo */
	R5 = [P0 + 12];                      /* bp */
	R7.H = 1;
	R7.L = 0xFFFF;                       /* 131071 */
	P5 = 16;
	LSETUP (.Lbx_b, .Lbx_e) LC0 = P5;
.Lbx_b:
	R0 = [I0++];
	R1 = [I0++];
	.rept 6
	R0 = R0 + R1 (NS) || R1 = [I0++] || NOP;
	.endr
	R0 = R0 + R1 (NS) || [I1++] = R5 || NOP;   /* in; bd[2 i] = bp */
	R1 = R4 - R5 (NS);
	R1 >>>= 3;
	[I1++] = R1;                         /* bd[2 i + 1] */
	R5 = R4;                             /* bp = bo */
	R1 = R3 >>> 3;                       /* bbd >> 3 */
	R4 = -R7;
	R2 = R2 + R1 (NS);
	R2 = MIN (R2, R7);
	R2 = MAX (R2, R4);                   /* blo */
	R1 = R3 >>> 2;                       /* bbd >> 2 */
	R0 >>>= 3;
	R0 = R0 - R2 (NS);
	R0 = R0 - R1 (NS);
	R0 >>>= 3;
	R3 = R3 + R0 (NS);
	R3 = MIN (R3, R7);
	R3 = MAX (R3, R4);                   /* bbd */
	R0 = R6 >>> 2;
	R0 *= R3;
	R0 >>>= 13;
	R1 = R0 << 1;
	R4 = R0 + R1 (NS);
	R4 >>>= 2;                           /* bo */
	R1 = P2;
.Lbx_e:
	R6 = R6 + R1 (NS);                   /* bx += dbx8 */
	[P0] = R2;
	[P0 + 4] = R3;
	[P0 + 8] = R4;
	[P0 + 12] = R5;
	(R7:4, P5:5) = [SP++];
	RTS;
	.size _mb_box, .-_mb_box

/* two samples with the box: 9 packets */
.macro OUT2_BOX
	R6 = R6 + R7 (NS) || R0 = [I0++] || NOP;
	R0 = R0 + R6 (NS);
	R0 *= R5;
	R0 = R0 << 1 (S);
	R6 = R6 + R7 (NS) || R1 = [I0++] || W[I1++] = R0.H;
	R1 = R1 + R6 (NS);
	R1 *= R5;
	R1 = R1 << 1 (S);
	W[I1++] = R1.H;
.endm
/* two samples without: 7 packets */
.macro OUT2
	R0 = [I0++];
	R0 *= R5;
	R0 = R0 << 1 (S) || R1 = [I0++] || NOP;
	R1 *= R5;
	R1 = R1 << 1 (S) || W[I1++] = R0.H || NOP;
	W[I1++] = R1.H;
.endm

	.align 4
	.global _mb_out
	.type _mb_out, STT_FUNC
_mb_out:
	[--SP] = (R7:5);                     /* 3 words: g at SP + 12 + 12 */
	R5 = [SP + 24];                      /* g */
	R3 = 0;
	L0 = R3;
	L1 = R3;
	L2 = R3;
	I0 = R0;
	I1 = R1;
	I2 = R2;
	P0 = 16;
	CC = R2 == 0;
	IF CC JUMP .Lout_plain;
	LSETUP (.Lob_b, .Lob_e) LC0 = P0;
.Lob_b:
	R6 = [I2++];
	R7 = [I2++];
	.rept 3
	OUT2_BOX
	.endr
	R6 = R6 + R7 (NS) || R0 = [I0++] || NOP;
	R0 = R0 + R6 (NS);
	R0 *= R5;
	R0 = R0 << 1 (S);
	R6 = R6 + R7 (NS) || R1 = [I0++] || W[I1++] = R0.H;
	R1 = R1 + R6 (NS);
	R1 *= R5;
	R1 = R1 << 1 (S);
.Lob_e:
	W[I1++] = R1.H;
	(R7:5) = [SP++];
	RTS;
.Lout_plain:
	LSETUP (.Lop_b, .Lop_e) LC0 = P0;
.Lop_b:
	.rept 3
	OUT2
	.endr
	R0 = [I0++];
	R0 *= R5;
	R0 = R0 << 1 (S) || R1 = [I0++] || NOP;
	R1 *= R5;
	R1 = R1 << 1 (S) || W[I1++] = R0.H || NOP;
.Lop_e:
	W[I1++] = R1.H;
	(R7:5) = [SP++];
	RTS;
	.size _mb_out, .-_mb_out

/* ---- the box's output added to the block, and the wah ----
 * void mb_boxadd(int32_t *acc, const int32_t *bd): acc[j] += o_j (the box's output as in mb_out)
 * void mb_wah1(const int32_t *acc, int32_t *ww, int32_t *st, int32_t wf): the wah's band-pass over the 64 sample pairs:
 *   st = { wlo, wbd } in / out; ww[k + 2] = wbd * 3 after pair k (ww[0], ww[1] = wp, wo before the block)
 * void mb_wah2(const int32_t *acc, int16_t *out, const int32_t *ww, int32_t w, int32_t dw2, int32_t g): the output,
 *   pair k mixing in its interpolation between ww[k] and ww[k + 1] by w >> 3 (w += dw2 per pair) */
	.align 4
	.global _mb_boxadd
	.type _mb_boxadd, STT_FUNC
_mb_boxadd:
	[--SP] = (R7:6);
	R3 = 0;
	L0 = R3;
	L1 = R3;
	L2 = R3;
	I0 = R0;
	I1 = R0;
	I2 = R1;
	P0 = 16;
	LSETUP (.Lba_b, .Lba_e) LC0 = P0;
.Lba_b:
	R6 = [I2++];
	R7 = [I2++];
	.rept 4
	R6 = R6 + R7 (NS) || R0 = [I0++] || NOP;
	R0 = R0 + R6 (NS);
	R6 = R6 + R7 (NS) || R1 = [I0++] || [I1++] = R0;
	R1 = R1 + R6 (NS);
	[I1++] = R1;
	.endr
.Lba_e:
	NOP;
	(R7:6) = [SP++];
	RTS;
	.size _mb_boxadd, .-_mb_boxadd

	.align 4
	.global _mb_wah1
	.type _mb_wah1, STT_FUNC
_mb_wah1:
	[--SP] = (R7:4, P5:5);               /* 5 words: wf at SP + 20 + 12 */
	R3 = 0;
	L0 = R3;
	L1 = R3;
	I0 = R0;
	R1 += 8;
	I1 = R1;                             /* ww + 2 */
	P5 = R2;
	R0 = [P5];                           /* wlo */
	R1 = [P5 + 4];                       /* wbd */
	R2 = [SP + 32];                      /* wf */
	R3 = 2500 (X);
	R4.H = 1;
	R4.L = 0xFFFF;                       /* 131071 */
	R5 = -R4;
	P0 = 64;
	LSETUP (.Lw1_b, .Lw1_e) LC0 = P0;
.Lw1_b:
	R6 = [I0++];
	R7 = [I0++];
	R6 = R6 + R7 (NS);
	R6 = R6 >>> 2;                       /* (y0 + y1) >> 2 */
	R7 = R1;
	R7 *= R2;
	R7 = R7 >>> 13;
	R0 = R0 + R7 (NS);
	R0 = MIN(R0, R4);
	R0 = MAX(R0, R5);                    /* wlo */
	R7 = R1;
	R7 *= R3;
	R7 = R7 >>> 13;                      /* (2500 wbd) >> 13 */
	R6 = R6 - R0 (NS);
	R6 = R6 - R7 (NS);
	R6 *= R2;
	R6 = R6 >>> 13;
	R1 = R1 + R6 (NS);
	R1 = MIN(R1, R4);
	R1 = MAX(R1, R5);                    /* wbd */
	R7 = R1 << 1;
	R7 = R7 + R1 (NS);
.Lw1_e:
	[I1++] = R7;
	[P5] = R0;
	[P5 + 4] = R1;
	(R7:4, P5:5) = [SP++];
	RTS;
	.size _mb_wah1, .-_mb_wah1

	.align 4
	.global _mb_wah2
	.type _mb_wah2, STT_FUNC
_mb_wah2:
	[--SP] = (R7:4);                     /* 4 words: w at SP + 16 + 12 */
	R3 = 0;
	L0 = R3;
	L1 = R3;
	L2 = R3;
	I0 = R0;
	I1 = R1;
	I2 = R2;
	R4 = [SP + 28];                      /* w */
	R7 = [SP + 32];                      /* dw2 */
	R6 = [SP + 36];                      /* g */
	R2 = [I2++];                         /* wp */
	R3 = [I2++];                         /* wo */
	P0 = 32;
	LSETUP (.Lw2_b, .Lw2_e) LC0 = P0;
.Lw2_b:
	/* pair: wp in R2, wo in R3; the next wo into R2 */
	R5 = R4 >>> 3 || R0 = [I0++] || NOP;
	R4 = R4 + R7 (NS) || R1 = [I0++] || NOP;
	R2 = R2 + R3 (NS);
	R2 = R2 >>> 1;
	R2 = R2 - R0 (NS);
	R2 *= R5;
	R2 = R2 >>> 12;
	R0 = R0 + R2 (NS);
	R2 = R3 - R1 (NS);
	R0 *= R6;
	R0 = R0 << 1 (S);
	R2 *= R5;
	R2 = R2 >>> 12 || W[I1++] = R0.H || NOP;
	R1 = R1 + R2 (NS) || R2 = [I2++] || NOP;
	R1 *= R6;
	R1 = R1 << 1 (S);
	W[I1++] = R1.H;
	/* pair: wp in R3, wo in R2; the next wo into R3 */
	R5 = R4 >>> 3 || R0 = [I0++] || NOP;
	R4 = R4 + R7 (NS) || R1 = [I0++] || NOP;
	R3 = R3 + R2 (NS);
	R3 = R3 >>> 1;
	R3 = R3 - R0 (NS);
	R3 *= R5;
	R3 = R3 >>> 12;
	R0 = R0 + R3 (NS);
	R3 = R2 - R1 (NS);
	R0 *= R6;
	R0 = R0 << 1 (S);
	R3 *= R5;
	R3 = R3 >>> 12 || W[I1++] = R0.H || NOP;
	R1 = R1 + R3 (NS) || R3 = [I2++] || NOP;
	R1 *= R6;
	R1 = R1 << 1 (S);
.Lw2_e:
	W[I1++] = R1.H;
	(R7:4) = [SP++];
	RTS;
	.size _mb_wah2, .-_mb_wah2


/* ---- the buzz ----
 * void mb_buzz(int32_t *acc, const int32_t *gz, uint32_t *st): st = { noise, nprev, zlp } in / out; for each group i
 *   of 8 samples with contact gz[i] != 0, per sample pair (j, j + 1) (the noise at 44.1 kHz, 2026-11-03): n = lcg(noise)
 *   >> 16 (arithmetic), zlp += (((n - nprev) >> 1) - zlp) >> 1, z = (zlp * gz[i]) >> 15, acc[j] += z, acc[j + 1] += z,
 *   nprev = n; groups with gz[i] = 0 are left as they are */
.macro BUZZ_A                                /* a pair: n into R6, nprev in R1; the block's words through R1 */
	R0 *= R4;
	R0 = R0 + R5 (NS);
	R6 = R0 >>> 16;
	R7 = R6 - R1 (NS);
	R7 = R7 >>> 1 || R1 = [I0++] || NOP;
	R7 = R7 - R2 (NS);
	R7 = R7 >>> 1;
	R2 = R2 + R7 (NS);
	R7 = R2.L * R3.L (IS);
	R7 = R7 >>> 15;
	R1 = R1 + R7 (NS);
	[I1++] = R1;
	R1 = [I0++];
	R1 = R1 + R7 (NS);
	[I1++] = R1;
.endm
.macro BUZZ_B                                /* a pair: n into R1, nprev in R6; the block's words through R6 */
	R0 *= R4;
	R0 = R0 + R5 (NS);
	R1 = R0 >>> 16;
	R7 = R1 - R6 (NS);
	R7 = R7 >>> 1 || R6 = [I0++] || NOP;
	R7 = R7 - R2 (NS);
	R7 = R7 >>> 1;
	R2 = R2 + R7 (NS);
	R7 = R2.L * R3.L (IS);
	R7 = R7 >>> 15;
	R6 = R6 + R7 (NS);
	[I1++] = R6;
	R6 = [I0++];
	R6 = R6 + R7 (NS);
	[I1++] = R6;
.endm

	.align 4
	.global _mb_buzz
	.type _mb_buzz, STT_FUNC
_mb_buzz:
	[--SP] = (R7:4, P5:4);
	R3 = 0;
	L0 = R3;
	L1 = R3;
	I0 = R0;
	I1 = R0;
	P4 = R1;                             /* gz */
	P5 = R2;
	R0 = [P5];                           /* noise */
	R1 = [P5 + 4];                       /* nprev */
	R2 = [P5 + 8];                       /* zlp */
	R4.H = 0x0019;
	R4.L = 0x660D;                       /* 1664525 */
	R5.H = 0x3C6E;
	R5.L = 0xF35F;                       /* 1013904223 */
	M0 = 32 (X);
	P0 = 16;
	LSETUP (.Lbz_b, .Lbz_e) LC0 = P0;
.Lbz_b:
	R3 = [P4++];
	CC = R3 == 0;
	IF CC JUMP .Lbz_skip;
	BUZZ_A
	BUZZ_B
	BUZZ_A
	BUZZ_B                               /* four pairs; nprev is in R1 again */
	JUMP .Lbz_e;
.Lbz_skip:
	I0 += M0;
	I1 += M0;
.Lbz_e:
	NOP;
	[P5] = R0;
	[P5 + 4] = R1;
	[P5 + 8] = R2;
	(R7:4, P5:4) = [SP++];
	RTS;
	.size _mb_buzz, .-_mb_buzz
/* ---- the buzz contact per group of 8 ----
 * void mb_gz(int32_t *gz, const int32_t *in): in = { ph, c, dc8, b, step8, bz, dbz8, tab } (tab as the passes'): for each of the 16 groups,
 *   s = sin(ph + c), as = |s| (read as tab[|k|], the passes' first-quadrant fold, which is |s| already), g = as > 22937 ? 3 (as - 22937) + ((as - 22937) >> 3) : 0,
 *   gz = ((((b >> 10) g) >> 15) bz) >> 15; then ph += c << 3, c += dc8, b += step8, bz += dbz8
 *   (b: the tine's level one step in, a + step) */
	.align 4
	.global _mb_gz
	.type _mb_gz, STT_FUNC
_mb_gz:
	[--SP] = (R7:4, P5:4);
	P1 = R0;
	P5 = R1;
	R0 = [P5];                           /* ph */
	R1 = [P5 + 4];                       /* c */
	R2 = [P5 + 8];                       /* dc8 */
	R3 = [P5 + 12];                      /* b */
	R4 = [P5 + 16];                      /* step8 */
	R5 = [P5 + 20];                      /* bz */
	P0 = [P5 + 28];                      /* tab */
	P2 = 16;
	LSETUP (.Lgz_b, .Lgz_e) LC0 = P2;
.Lgz_b:
	R6 = R0 + R1 (NS);
	R6 = R6 << 1;
	R6 = R6 >>> 20;
	R6 = ABS R6;                         /* |sin| = tab[|k|], the first quadrant (k as in the passes) */
	P4 = R6;
	P4 = P0 + (P4 << 1);
	R6 = W[P4] (X);
	R7.H = 0;
	R7.L = 22937;
	R6 = R6 - R7 (NS);
	R7 = 0;
	R6 = MAX(R6, R7);
	R7 = R6 >>> 3;
	R7 = R7 + R6 (NS);
	R6 = R6 << 1;
	R6 = R6 + R7 (NS);                   /* g */
	R7 = R3 >>> 10;
	R7 = R7.L * R6.L (IS);
	R7 = R7 >>> 15;
	R7 = R7.L * R5.L (IS);
	R7 = R7 >>> 15;
	[P1++] = R7;
	R7 = R1 << 3;
	R0 = R0 + R7 (NS);
	R1 = R1 + R2 (NS);
	R3 = R3 + R4 (NS) || R7 = [P5 + 24] || NOP;
.Lgz_e:
	R5 = R5 + R7 (NS);
	(R7:4, P5:4) = [SP++];
	RTS;
	.size _mb_gz, .-_mb_gz

/* ---- the click's burst ----
 * int mb_burst(int32_t *acc, int32_t *st, int32_t cfa, const int16_t *noise): st = { click, clp, pos } in / out
 *   (pos is not written back), cfa = clp_a << 16 | cf, noise = MB_NOISE (1024, read in a circle from pos);
 *   while click > 8 at a sample's start: n = noise[pos++], clp += ((((n click) >> 15) - clp) clp_a) >> 15,
 *   click = (click cf) >> 15, acc[j] += clp >> 1; returns the samples done (1 .. 128). The product with clp_a is
 *   taken apart (x clp_a - clp clp_a, both within int16), the same integer. (n click) >> 15 and (click cf) >> 15
 *   are fractional multiplies with truncation (the high half of the doubled product: the same floor, and no
 *   saturation, as click is 0 .. 32767 and cf under 32768); click stays in R1.L with R1.H = 0.
 *   The block runs in 8 runs of 16 samples: while click >= 24 at a run's start, all 16 samples of the run have
 *   click > 8 at their start for every cf from 32034 (mbira.c's range: 32034 .. 32619) up, so the run skips the test
 *   (8 packets a sample; the bound found by trying every click and cf in
 *   OP1-Emulator); below 24 the rest of the block tests each sample at its top (mb_burst is called with click > 8),
 *   10 packets a sample. */
	.align 4
	.global _mb_burst
	.type _mb_burst, STT_FUNC
_mb_burst:
	[--SP] = (R7:4, P5:5);               /* 5 words: noise at SP + 20 + 12 */
	R3 = 0;
	L0 = R3;
	L1 = R3;
	I0 = R0;
	I1 = R0;
	P1 = R0;                             /* acc: the samples done are (I1 - acc) / 4 */
	P5 = R1;
	R3 = R2;                             /* clp_a : cf */
	R1 = [P5];                           /* click (0 .. 32767: R1.H = 0) */
	R2 = [P5 + 4];                       /* clp */
	R0 = [P5 + 8];                       /* pos */
	R4 = [SP + 32];                      /* noise */
	B2 = R4;
	R0 = R0 << 1;
	R4 = R4 + R0 (NS);
	I2 = R4;
	R4 = 2048 (X);
	L2 = R4;                             /* a circle of 1024 samples */
	R6.L = W[I2++];
	R5 = 24;
	P0 = 16;
	P2 = 8;                              /* runs of 16 left */
.Lru:
	CC = R1 < R5;                        /* click under 24: test every sample from here */
	IF CC JUMP .Lbu_chk;
	LSETUP (.Lru_b, .Lru_e) LC0 = P0;
.Lru_b:
	R7.L = R6.L * R1.L (T) || R6.L = W[I2++] || R0 = [I0++];
	A0 = R7.L * R3.H (IS);
	R4 = (A0 -= R2.L * R3.H) (IS);
	R4 = R4 >>> 15;
	R2 = R2 + R4 (NS);
	R7 = R2 >>> 1;
	R0 = R0 + R7 (NS);
.Lru_e:
	R1.L = R1.L * R3.L (T) || [I1++] = R0 || NOP;
	P2 += -1;
	CC = P2 == 0;
	IF !CC JUMP .Lru (BP);
	JUMP .Lbu_out;
.Lbu_chk:
	R5 = 8;
	R4 = P2;
	R4 <<= 4;
	P0 = R4;                             /* the samples left */
	LSETUP (.Lbu_b, .Lbu_e) LC0 = P0;
.Lbu_b:
	CC = R1 <= R5;
	IF CC JUMP .Lbu_out;
	R7.L = R6.L * R1.L (T) || R6.L = W[I2++] || R0 = [I0++];
	A0 = R7.L * R3.H (IS);
	R4 = (A0 -= R2.L * R3.H) (IS);
	R4 = R4 >>> 15;
	R2 = R2 + R4 (NS);
	R7 = R2 >>> 1;
	R0 = R0 + R7 (NS);
.Lbu_e:
	R1.L = R1.L * R3.L (T) || [I1++] = R0 || NOP;
.Lbu_out:
	R7 = 0;
	LC0 = R7;                            /* left by the jump: no loop counts on */
	L2 = R7;
	B2 = R7;
	[P5] = R1;
	[P5 + 4] = R2;
	R0 = I1;
	R4 = P1;
	R0 = R0 - R4;
	R0 >>= 2;
	(R7:4, P5:5) = [SP++];
	RTS;
	.size _mb_burst, .-_mb_burst

#endif
