/* MONOLIT's oscillators, hand-scheduled for the Blackfin: the same arithmetic as monolit.c's C loop (kept there for
 * host builds and as kern_check.py's reference), bit for bit, in passes over the block so each keeps its values in
 * registers: an oscillator pass per saw (osc 1 stores the mix, osc 2 and 3 add to it) and the sub square's pass.
 * The ladder is monolit_fc.S (Moai's zero-delay ladder, gen_fc.py): render runs it on the mix, halved, into out[]
 * (2026-11-04: it was mo_ladder here, a unit-delay ladder). A pass of an oscillator whose level is 0 only moves its
 * phase (mo_adv). The C loop needs a 32 x 32
 * multiply for most of its products; here every product in the common path is a 16 x 16 MAC.
 *
 * In mo_osc's registers a phase is kept XOR 0x80000000 (p'), so a saw's base (t >> 16) - 32768 is p'.H, and t < inc (unsigned) is
 * p' < inc ^ 0x80000000 (signed); 0 - t is (0 - p') ^ 0x80000000.
 *
 * void mo_run(br_voice *v, int16_t *out, const int32_t *b): the oscillators, called by render. v starts with ph[4] (raw
 *   phases) and s[4] (the ladder); b (words) = { m0 | o1 << 16, rdt, m1 | o2 << 16, rdt, m2 | o3 << 16, rdt3,
 *   m0 | osub << 16, rsub, tm, tp, (10 .. 18 unused), bsel, pre, mix pad }. It saves and zeroes L0..L3 and calls the passes below, which take their arguments in
 *   registers (R0, R1, R2, R3, P1) and are called only from mo_run:
 * mo_osc (R0 &ph, R1 inc, R2 mix pad, R3 step, P1 { m | o << 16, rdt }): per sample i = inc + (int32_t)(inc >> 16)
 *   * m (|m| < 2^15: a signed x unsigned 16-bit product), p += i, mix[i] = read[i] + (o * saw(p, i, rdt) >> 15);
 *   step 0: read is a zero word (osc 1 stores), 4: read = mix (osc 2, 3 add). The store runs one sample behind,
 *   into the pad word first. The polyBLEP corrections (t < i or 0 - t < i) go out of line and use 32-bit products
 *   as C does, so they are exact for any increment.
 * mo_adv (R0 &ph, R1 inc, R2 m): an oscillator at level 0: only its phase moves (the sum of the i, mod 2^32).
 * mo_pre (R0 dst, R1 inc, R2 bsel): osc 3's (inc >> 2) * ratio4[bsel], ratio4 = { 2, 4, 6, 8, 1 }:
 *   (inc >> 2) << 1, inc & ~3, (inc & ~3) + ((inc & ~3) >> 1), (inc & ~3) << 1, inc >> 2.
 * mo_sub (R0 &ph, R1 inc, R2 mix pad, P1 { m | osub << 16, rsub, tm, tp }): isub = (inc + (inc >> 16) * m) >> 1,
 *   p += isub, mix[i] += osub * ((saw(p, isub, rsub) - saw(p + 2^31, isub, rsub)) >> 1) >> 15. With no correction
 *   the square is -16384 (p < 2^31) or 16384: tm = osub * -16384 >> 15, tp = osub * 16384 >> 15. A saw needs a
 *   correction only when y = p + isub (mod 2^32) or y ^ 2^31 is below 2 isub (no wrap: isub < 2^31); then the
 *   sample is done out of line exactly as C, both saws with both tests.
 *
 * Mixed-mode (IS,M) products are MAC1 only: first operand signed, second unsigned. mo_osc, mo_sub and mo_adv load the next
 * increment into R0 in the same packet that adds the current one (a parallel packet reads its operands before its
 * loads write, the usual Blackfin idiom); M1 is not touched (R246's PmSynth keeps its frame there).
 * Host builds assemble it empty.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

/* ---- mo_osc ---- */
/* R0 inc, R1 i then i ^ 2^31, R2 p', R3 the term, R4 read, R5 the sum, R6 rdt, R7 m | o << 16 */
	.align 4
	.global _mo_osc
	.type _mo_osc, STT_FUNC
_mo_osc:
	[--SP] = (R7:4);
	P0 = R0;
	I0 = R1;
	I2 = R2;
	M0 = R3;
	R2 += 4;
	CC = R3 == 0;
	R3 = 0;
	[--SP] = R3;                                     /* the zero word a storing pass reads */
	R3 = SP;
	IF CC R2 = R3;
	I1 = R2;
	R2 = [P0];
	BITTGL (R2, 31);
	R7 = [P1];
	R6 = [P1 + 4];
	R0 = [I0++];
	P1 = 128;
	LSETUP (.Lo_b, .Lo_e) LC0 = P1;
.Lo_b:
	R1 = (A1 = R7.L * R0.H) (IS,M) || R4 = [I1 ++ M0] || [I2++] = R5;
	R1 = R1 + R0 (NS) || R0 = [I0++] || NOP;
	R2 = R2 + R1;
	BITTGL (R1, 31);
	CC = R2 < R1;
	IF CC JUMP .Lo_a;
	R3 = -R2;
	CC = R3 < R1;
	IF CC JUMP .Lo_n;
	R3 = R2.H * R7.H (IS);
.Lo_t:
	R3 >>>= 15;
.Lo_e:
	R5 = R4 + R3;
	[I2++] = R5;
	BITTGL (R2, 31);
	[P0] = R2;
	SP += 4;
	(R7:4) = [SP++];
	RTS;
.Lo_a:                                               /* t < i: s += a * a >> 15, a = 32768 - (t * rdt >> 17) */
	R3 = R2;
	R5 = 0;
	JUMP.S .Lo_c;
.Lo_n:                                               /* 0 - t < i: s -= ... with 0 - t */
	R5 = -1;
.Lo_c:
	BITTGL (R3, 31);
	R3 *= R6;
	R3 >>= 17;
	R1 = 32768 (Z);
	R3 = R1 - R3;
	R3 *= R3;
	R3 >>>= 15;
	R3 = R3 ^ R5;
	R3 = R3 - R5;
	R1 = R2 >>> 16;
	R3 = R3 + R1;
	R1 = R7 >> 16;
	R3 *= R1;
	JUMP.S .Lo_t;
	.size _mo_osc, .-_mo_osc

/* ---- mo_adv: the phase only ---- */
	.align 4
	.global _mo_adv
	.type _mo_adv, STT_FUNC
_mo_adv:
	P0 = R0;
	I0 = R1;
	R1 = R2;
	R2 = [P0];
	A1 = 0;
	R0 = [I0++];
	P1 = 128;
	LSETUP (.La_b, .La_e) LC0 = P1;
.La_b:
	A1 += R1.L * R0.H (IS,M);
.La_e:
	R2 = R2 + R0 (NS) || R0 = [I0++] || NOP;
	R1 = A1.w;
	R2 = R2 + R1;
	[P0] = R2;
	RTS;
	.size _mo_adv, .-_mo_adv

/* ---- mo_pre: osc 3's increments ---- */
	.align 4
	.global _mo_pre
	.type _mo_pre, STT_FUNC
_mo_pre:
	I1 = R0;
	I0 = R1;
	R3 = -4;                                         /* ~3 */
	P1 = 128;
	R0 = [I0++];
	CC = R2 == 1;
	IF CC JUMP .Lp1;
	CC = R2 == 2;
	IF CC JUMP .Lp2;
	CC = R2 == 3;
	IF CC JUMP .Lp3;
	R1 = 4;
	CC = R2 == R1;
	IF CC JUMP .Lp4;
	LSETUP (.Lp0_b, .Lp0_e) LC0 = P1;              /* 2: (inc >> 2) << 1 */
.Lp0_b:
	R1 = R0 >> 2;
	R1 <<= 1;
.Lp0_e:
	MNOP || R0 = [I0++] || [I1++] = R1;
	RTS;
.Lp1:                                                /* 4: inc & ~3 */
	LSETUP (.Lp1_b, .Lp1_e) LC0 = P1;
.Lp1_b:
	R1 = R0 & R3;
.Lp1_e:
	MNOP || R0 = [I0++] || [I1++] = R1;
	RTS;
.Lp2:                                                /* 6: x + (x >> 1), x = inc & ~3 */
	LSETUP (.Lp2_b, .Lp2_e) LC0 = P1;
.Lp2_b:
	R1 = R0 & R3;
	R2 = R1 >> 1;
	R1 = R1 + R2;
.Lp2_e:
	MNOP || R0 = [I0++] || [I1++] = R1;
	RTS;
.Lp3:                                                /* 8: (inc & ~3) << 1 */
	LSETUP (.Lp3_b, .Lp3_e) LC0 = P1;
.Lp3_b:
	R1 = R0 & R3;
	R1 <<= 1;
.Lp3_e:
	MNOP || R0 = [I0++] || [I1++] = R1;
	RTS;
.Lp4:                                                /* 1: inc >> 2 */
	LSETUP (.Lp4_b, .Lp4_e) LC0 = P1;
.Lp4_b:
	R1 = R0 >> 2;
.Lp4_e:
	MNOP || R0 = [I0++] || [I1++] = R1;
	RTS;
	.size _mo_pre, .-_mo_pre

/* ---- mo_sub ---- */
/* R0 inc, R1 isub then 2 isub, R2 p, R3 y then the term, R4 read, R5 the sum, R6 rsub, R7 m | osub << 16;
 * P1 tm, P2 tp */
	.align 4
	.global _mo_sub
	.type _mo_sub, STT_FUNC
_mo_sub:
	[--SP] = (R7:4, P5:3);
	[--SP] = RETS;
	P0 = R0;
	I0 = R1;
	I2 = R2;
	R2 += 4;
	I1 = R2;
	R2 = [P0];
	R7 = [P1];
	R6 = [P1 + 4];
	P2 = [P1 + 12];
	P1 = [P1 + 8];
	R0 = [I0++];
	P3 = 128;
	LSETUP (.Ls_b, .Ls_e) LC0 = P3;
.Ls_b:
	R1 = (A1 = R7.L * R0.H) (IS,M) || R4 = [I1++] || [I2++] = R5;
	R1 = R1 + R0 (NS) || R0 = [I0++] || NOP;
	R1 >>= 1;
	R2 = R2 + R1;
	R3 = R2 + R1;
	R1 = R1 + R1;
	CC = R3 < R1 (IU);
	IF CC JUMP .Ls_r;
	BITTGL (R3, 31);
	CC = R3 < R1 (IU);
	IF CC JUMP .Ls_r;
	CC = R2 < 0;
	R3 = P1;
	IF CC R3 = P2;
.Ls_e:
	R5 = R4 + R3;
	[I2++] = R5;
	[P0] = R2;
	RETS = [SP++];
	(R7:4, P5:3) = [SP++];
	RTS;
.Ls_r:                                               /* exactly as C: both saws with both tests */
	[--SP] = R0;
	[--SP] = R4;
	R1 >>= 1;
	R3 = R2;
	CALL .Lsaw;
	R4 = R3;
	R3 = R2;
	BITTGL (R3, 31);
	CALL .Lsaw;
	R3 = R4 - R3;
	R3 >>>= 1;
	R0 = R7 >>> 16;
	R3 *= R0;
	R3 >>>= 15;
	R4 = [SP++];
	R0 = [SP++];
	JUMP.S .Ls_e;
/* R3 = saw(t = R3, inc = R1, rdt = R6); clobbers R0, R5 */
.Lsaw:
	R5 = R3;
	BITTGL (R5, 31);
	R5 >>>= 16;
	CC = R3 < R1 (IU);
	IF CC JUMP .Lsw_a;
	R3 = -R3;
	CC = R3 < R1 (IU);
	IF CC JUMP .Lsw_n;
	R3 = R5;
	RTS;
.Lsw_a:
	R3 *= R6;
	R3 >>= 17;
	R0 = 32768 (Z);
	R3 = R0 - R3;
	R3 *= R3;
	R3 >>>= 15;
	R3 = R5 + R3;
	RTS;
.Lsw_n:
	R3 *= R6;
	R3 >>= 17;
	R0 = 32768 (Z);
	R3 = R0 - R3;
	R3 *= R3;
	R3 >>>= 15;
	R3 = R5 - R3;
	RTS;
	.size _mo_sub, .-_mo_sub

/* ---- mo_run: the block ---- */
/* P3 the voice, P4 out, P5 b; R? pitch = OP1_PITCH_INC */
.macro PITCH r
	\r\().H = 0xFF80;
	\r\().L = 0x1950;
.endm
	.align 4
	.global _mo_run
	.type _mo_run, STT_FUNC
_mo_run:
	[--SP] = (P5:3);
	[--SP] = RETS;
	R3 = L0;
	[--SP] = R3;
	R3 = L1;
	[--SP] = R3;
	R3 = L2;
	[--SP] = R3;
	R3 = L3;
	[--SP] = R3;
	R3 = 0;
	L0 = R3;
	L1 = R3;
	L2 = R3;
	L3 = R3;
	P3 = R0;
	P4 = R1;
	P5 = R2;
	PITCH R1                                         /* osc 1: stores the mix */
	R2 = [P5 + 84];
	P1 = P5;
	CALL _mo_osc;
	R0 = P3;                                         /* osc 2 */
	R0 += 4;
	PITCH R1
	R3 = [P5 + 8];
	R2 = R3 >> 16;
	CC = R2 == 0;
	IF CC JUMP .Lr_a2;
	R2 = [P5 + 84];
	R3 = 4;
	P1 = P5;
	P1 += 8;
	CALL _mo_osc;
	JUMP.S .Lr_3;
.Lr_a2:
	R2 = R3;
	CALL _mo_adv;
.Lr_3:
	R0 = [P5 + 80];                                  /* osc 3 */
	PITCH R1
	R2 = [P5 + 76];
	CALL _mo_pre;
	R0 = P3;
	R0 += 8;
	R1 = [P5 + 80];
	R3 = [P5 + 16];
	R2 = R3 >> 16;
	CC = R2 == 0;
	IF CC JUMP .Lr_a3;
	R2 = [P5 + 84];
	R3 = 4;
	P1 = P5;
	P1 += 16;
	CALL _mo_osc;
	JUMP.S .Lr_s;
.Lr_a3:
	R2 = R3;
	CALL _mo_adv;
.Lr_s:
	R0 = P3;                                         /* the sub */
	R0 += 12;
	PITCH R1
	R2 = [P5 + 84];
	P1 = P5;
	P1 += 24;
	CALL _mo_sub;
	R3 = [SP++];
	L3 = R3;
	R3 = [SP++];
	L2 = R3;
	R3 = [SP++];
	L1 = R3;
	R3 = [SP++];
	L0 = R3;
	RETS = [SP++];
	(P5:3) = [SP++];
	RTS;
	.size _mo_run, .-_mo_run
#endif
