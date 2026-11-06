/* ELG's sample loop, hand-scheduled for the Blackfin: the same arithmetic as elg.c's C loop (kept there for host
 * builds), bit for bit, in five passes over the block so each keeps its state in registers (about 58 packets per
 * sample instead of about 190).
 *
 * void elg_inc(uint32_t *t, const uint32_t *pitch, const int32_t *c)
 *   the three phase increments per sample: t[3i] = base, t[3i+1] = base + (base * det >> 15),
 *   t[3i+2] = (base * ratio >> 12) - (base * det >> 16), with base = inc + (inc * dr >> 15) (64-bit products, as
 *   elg.c's mul_u32_s); c = { wdr, ddr, wdet, ddet, step }: the ramped controls as control << 9 (the high half is the
 *   sample's value control >> 7, the carry from the low half steps it) and their per-sample deltas
 * void elg_mix(uint32_t *ph, const uint32_t *t, int16_t *out)
 *   the three saws and their sum / 3 (truncated): ph = { p0, p1, p2 } each XOR 0x80000000, so a phase's signed high
 *   half is (p >> 16) - 32768; out[i] = the sum / 3 (|sum| < 2^17: sum * 43691 >> 17, plus 1 when negative)
 * void elg_shape(int16_t *out, int32_t wdrive, int32_t ddrive)
 *   in place: x = clamp(x * drive >> 12, +-32767), then x * (98304 - (x * x >> 15)) >> 16, split as
 *   (x * (32768 - xx) >> 16) + x so the 16-bit unsigned factor is at most 32768
 * void elg_svf(int16_t *out, int32_t *st, const int32_t *c)
 *   in place: the Chamberlin low-pass and the output gain; st = { low, band }, updated; c = { wf, df, wgain, dgain,
 *   40000 } (the damping q = 20000, doubled). Every a * b >> 15 with a 32-bit a is a 32 x 16 product in an
 *   accumulator: (a.L * 2b >> 16) + a.H * 2b (MAC1, signed x unsigned), exact for any a; the output's low * gain
 *   doubles low instead (MAC0) and saturates in the 16-bit extraction, as engine_sat16
 * int32_t elg_peak(const int16_t *out): max |out[i]| (0 for silence), two samples per packet
 *
 * Mixed-mode (IS,M) products are MAC1 only: first operand signed, second unsigned (GCC's usmulhisi3, bfin-sim).
 * Each pass zeroes the L registers it uses; elg.c restores the caller's L0..L3 after the block, as mbira.c does.
 * M1 is not touched (R246's PmSynth keeps its frame there).
 * Host builds (screen and golden harnesses) assemble it empty: elg.c keeps the C loop there.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

/* ---- elg_inc: 14..16 packets per sample ---- */
/* R6 wdr, R4 ddr, R7 wdet, R5 ddet; R0 inc, R1 / R3 products, R2 base */
.macro INC_HEAD
	R3 = R6 +|+ R6 || R0 = [I0++] || NOP;           /* .H = 2 dr */
	A1 = R6.H * R0.L (IS,M);                         /* dr * inc.L */
	A1 = A1 >>> 15;
	R1 = (A1 += R3.H * R0.H) (IS,M);                 /* inc * dr >> 15 */
	R2 = R0 + R1 (NS);                               /* base */
	R3 = R7 +|+ R7 || [I1++] = R2;                   /* .H = 2 det */
	A0 = R2.L * R7.H (FU);
	A0 = A0 >> 15;
	R0 = (A0 += R2.H * R3.H) (FU);                   /* base * det >> 15 */
	R1 = R2 + R0 (NS);
	R0 = R0 >> 1 || [I1++] = R1;                     /* base * det >> 16 */
.endm
.macro INC_TAIL
	R6 = R6 + R4 (NS) || [I1++] = R1;
.endm

	.align 4
	.global _elg_inc
	.type _elg_inc, STT_FUNC
_elg_inc:
	[--SP] = (R7:4);
	P0 = R2;
	I1 = R0;
	I0 = R1;
	R3 = 0;
	L0 = R3;
	L1 = R3;
	R6 = [P0];
	R4 = [P0 + 4];
	R7 = [P0 + 8];
	R5 = [P0 + 12];
	R3 = [P0 + 16];
	P1 = 128;
	CC = R3 == 0;
	IF CC JUMP .Linc0;
	CC = R3 == 1;
	IF CC JUMP .Linc1;
	CC = R3 == 2;
	IF CC JUMP .Linc2;
	CC = R3 == 3;
	IF CC JUMP .Linc3;
	LSETUP (.Li4_b, .Li4_e) LC0 = P1;                 /* ratio 8: 4 base */
.Li4_b:
	INC_HEAD
	R1 = R2 << 2;
	R1 = R1 - R0 (NS);
	INC_TAIL
.Li4_e:
	R7 = R7 + R5 (NS);
	JUMP .Linc_done;
.Linc0:                                              /* ratio 2: base */
	LSETUP (.Li0_b, .Li0_e) LC0 = P1;
.Li0_b:
	INC_HEAD
	R1 = R2 - R0 (NS);
	INC_TAIL
.Li0_e:
	R7 = R7 + R5 (NS);
	JUMP .Linc_done;
.Linc1:                                              /* ratio 3: base + (base >> 1) */
	LSETUP (.Li1_b, .Li1_e) LC0 = P1;
.Li1_b:
	INC_HEAD
	R1 = R2 >> 1;
	R1 = R1 - R0 (NS);
	R1 = R1 + R2 (NS);
	INC_TAIL
.Li1_e:
	R7 = R7 + R5 (NS);
	JUMP .Linc_done;
.Linc2:                                              /* ratio 4: 2 base */
	LSETUP (.Li2_b, .Li2_e) LC0 = P1;
.Li2_b:
	INC_HEAD
	R1 = R2 << 1;
	R1 = R1 - R0 (NS);
	INC_TAIL
.Li2_e:
	R7 = R7 + R5 (NS);
	JUMP .Linc_done;
.Linc3:                                              /* ratio 6: 3 base */
	LSETUP (.Li3_b, .Li3_e) LC0 = P1;
.Li3_b:
	INC_HEAD
	R1 = R2 << 1;
	R1 = R1 + R2 (NS);
	R1 = R1 - R0 (NS);
	INC_TAIL
.Li3_e:
	R7 = R7 + R5 (NS);
.Linc_done:
	(R7:4) = [SP++];
	RTS;
	.size _elg_inc, .-_elg_inc

/* ---- elg_mix: 10 packets per sample ---- */
/* R0 / R2 / R4 phases, R7 = 1 << 16 | 43691; R1 base, R3 / R6 increments, R5 the sum / 3 */
.macro MIX_SUM
	A1 = R0.H * R7.L (IS,M);
	A1 += R2.H * R7.L (IS,M);
	A1 += R4.H * R7.L (IS,M);                        /* sum * 43691 */
	A1 = A1 >>> 17;
	R5 = A1;
	R3 = R5 >>> 31;
	R5 = R5 - R3 (NS);                               /* truncated toward 0 */
.endm

	.align 4
	.global _elg_mix
	.type _elg_mix, STT_FUNC
_elg_mix:
	[--SP] = (R7:4);
	P0 = R0;
	I0 = R1;
	I1 = R2;
	R3 = 0;
	L0 = R3;
	L1 = R3;
	R0 = [P0];
	R2 = [P0 + 4];
	R4 = [P0 + 8];
	R7.H = 1;
	R7.L = 0xAAAB;
	R1 = [I0++];
	R0 = R0 + R1 (NS) || R3 = [I0++] || NOP;
	R2 = R2 + R3 (NS) || R6 = [I0++] || NOP;
	R4 = R4 + R6 (NS) || R1 = [I0++] || NOP;
	P1 = 127;
	LSETUP (.Lmx_b, .Lmx_e) LC0 = P1;
.Lmx_b:
	MIX_SUM
	R0 = R0 + R1 (NS) || R3 = [I0++] || W[I1++] = R5.L;
	R2 = R2 + R3 (NS) || R6 = [I0++] || NOP;
.Lmx_e:
	R4 = R4 + R6 (NS) || R1 = [I0++] || NOP;          /* the last one reads t[384], not used */
	MIX_SUM
	W[I1++] = R5.L;
	[P0] = R0;
	[P0 + 4] = R2;
	[P0 + 8] = R4;
	(R7:4) = [SP++];
	RTS;
	.size _elg_mix, .-_elg_mix

/* ---- elg_shape: 10 packets per sample ---- */
/* R6 wdrive (one delta behind), R5 ddrive, R7 = 1 << 16 | (-32767 & 0xFFFF), R4 = 0x8000 << 16; R0 in, R2 x, R3 out */
.macro SHAPE sto
	.if \sto
	R6 = R6 + R5 (NS) || R0.L = W[I0++] || W[I1++] = R3.L;
	.else
	R6 = R6 + R5 (NS) || R0.L = W[I0++] || NOP;
	.endif
	A0 = R0.L * R6.H (IS);
	A0 = A0 >>> 12;
	R2.L = A0 (IS);                                  /* x * drive >> 12, saturated to 16 bits */
	R2 = MAX (R2, R7) (V);                           /* .L: and to -32767 */
	R1.H = (A1 = R2.L * R2.L) (T);                   /* x * x >> 15 (fraction mode doubles, truncates) */
	R1 = R4 -|- R1;                                  /* .H: 32768 - xx, unsigned */
	A1 = R2.L * R1.H (IS,M);
	A1 = A1 >>> 16;
.endm
#define SHAPE_LAST R3 = (A1 += R2.L * R7.H) (IS,M)      /* + x */

	.align 4
	.global _elg_shape
	.type _elg_shape, STT_FUNC
_elg_shape:
	[--SP] = (R7:4);
	I0 = R0;
	I1 = R0;
	R3 = 0;
	L0 = R3;
	L1 = R3;
	R6 = R1;
	R5 = R2;
	R6 = R6 - R5 (NS);
	R7.H = 1;
	R7.L = 0x8001;
	R4 = 0;
	R4.H = 0x8000;
	SHAPE 0
	SHAPE_LAST;
	P1 = 127;
	LSETUP (.Lsh_b, .Lsh_e) LC0 = P1;
.Lsh_b:
	SHAPE 1
.Lsh_e:
	SHAPE_LAST;
	W[I1++] = R3.L;
	(R7:4) = [SP++];
	RTS;
	.size _elg_shape, .-_elg_shape

/* ---- elg_svf: 22 packets per sample ---- */
/* R6 low, R7 band, R4 / R5 = -65535 / 65535; I2 wf (M2 df), I3 wgain (M3 dgain); P0 in (x), I1 out, P1 -> 40000;
 * R0 x, then x - low, high, 2 low; R2 2f, then wgain; R1 products; R3 40000, then the output */
.macro SVF sto
	R2 = I2;
	R2 = R2 +|+ R2 || I2 += M2 || NOP;               /* .H = 2 f */
	.if \sto
	A1 = R7.L * R2.H (FU) || R0 = W[P0++] (X) || W[I1++] = R3.L;
	.else
	A1 = R7.L * R2.H (FU) || R0 = W[P0++] (X) || NOP;
	.endif
	A1 = A1 >> 16;
	R1 = (A1 += R7.H * R2.H) (IS,M);                 /* f * band >> 15 */
	R6 = R6 + R1 (NS) || R3 = [P1] || NOP;           /* low */
	R0 = R0 - R6 (NS);
	A1 = R7.L * R3.L (FU);
	A1 = A1 >> 16;
	R1 = (A1 += R7.H * R3.L) (IS,M);                 /* q * band >> 15 */
	R0 = R0 - R1 (NS);                               /* high */
	A1 = R0.L * R2.H (FU);
	A1 = A1 >> 16;
	R1 = (A1 += R0.H * R2.H) (IS,M);                 /* high * f >> 15 */
	R7 = R7 + R1 (NS);
	R7 = MAX (R7, R4);
	R7 = MIN (R7, R5);                               /* band */
	R2 = I3;
	R0 = R6 + R6 (NS) || I3 += M3 || NOP;
	A0 = R0.L * R2.H (FU);
	A0 = A0 >> 16;
.endm
#define SVF_LAST R3.L = (A0 += R0.H * R2.H) (IS)        /* sat16(low * gain >> 15) */

	.align 4
	.global _elg_svf
	.type _elg_svf, STT_FUNC
_elg_svf:
	[--SP] = (R7:4);
	P0 = R0;
	I1 = R0;
	P2 = R1;
	I0 = R1;
	P1 = R2;
	R3 = 0;
	L0 = R3;
	L1 = R3;
	L2 = R3;
	L3 = R3;
	R6 = [P2];
	R7 = [P2 + 4];
	R0 = [P1];
	I2 = R0;
	R0 = [P1 + 4];
	M2 = R0;
	R0 = [P1 + 8];
	I3 = R0;
	R0 = [P1 + 12];
	M3 = R0;
	P1 += 16;
	R5 = 0;
	R5.L = 0xFFFF;                                   /* 65535 */
	R4 = -R5;
	SVF 0
	SVF_LAST;
	P2 = 127;
	LSETUP (.Lsv_b, .Lsv_e) LC0 = P2;
.Lsv_b:
	SVF 1
.Lsv_e:
	SVF_LAST;
	W[I1++] = R3.L;
	P2 = I0;
	[P2] = R6;
	[P2 + 4] = R7;
	(R7:4) = [SP++];
	RTS;
	.size _elg_svf, .-_elg_svf

/* ---- elg_peak: 2 packets per two samples ---- */
	.align 4
	.global _elg_peak
	.type _elg_peak, STT_FUNC
_elg_peak:
	I0 = R0;
	R3 = 0;
	L0 = R3;
	R1 = 0;                                          /* lanes: max */
	R2 = 0;                                          /* lanes: min */
	R3 = [I0++];
	P1 = 63;
	LSETUP (.Lpk_b, .Lpk_e) LC0 = P1;
.Lpk_b:
	R1 = MAX (R1, R3) (V);
.Lpk_e:
	R2 = MIN (R2, R3) (V) || R3 = [I0++] || NOP;
	R1 = MAX (R1, R3) (V);
	R2 = MIN (R2, R3) (V);
	R0 = R1.L (X);
	R3 = R1 >>> 16;
	R0 = MAX (R0, R3);
	R3 = R2.L (X);
	R3 = -R3;
	R0 = MAX (R0, R3);
	R3 = R2 >>> 16;
	R3 = -R3;
	R0 = MAX (R0, R3);
	RTS;
	.size _elg_peak, .-_elg_peak
#endif
