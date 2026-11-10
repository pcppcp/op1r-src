/* BLOODMOON's sample loop for the Blackfin (kennel): bloodmoon.c's bm_render loop in two passes, the same arithmetic
 * bit for bit.
 *
 * void bm_vib(int16_t *out, uint32_t *lfo, uint32_t vinc, uint32_t vinc3, uint32_t amts)
 *   the pitch modulation of the 128 samples into out[] (|mod| < 1750, so it fits): out[n] = (sin(vp) * va +
 *   sin(vq) * wa) >> 15, vp += vinc and vq += vinc3 after each, where lfo = { vp, vq } (updated), vq = vp * 3 +
 *   0x55555555 (the C's second LFO, kept as its own phase: (vp + vinc) * 3 = vp * 3 + vinc * 3 mod 2^32) and
 *   amts = wa << 16 | va. These two read the whole table, but they move slowly, so a block reads few of its lines.
 * void bm_osc(int16_t *out, uint32_t *ph, const uint32_t *g, const int32_t *inc, const int16_t *tab)
 *   the voice: reads out[n] (the modulation) and inc[n], writes the sample to out[n]; ph = { ph, pa, pb } (updated);
 *   g = { g2 << 16 | g1, g4 << 16 | g3, bb << 16 | ba, OUT_GAIN << 16 | vg }. Per sample:
 *     eff = inc + (((inc >> 12) * mod) >> 3); ph += eff; pa += eff + (eff >> 3) + (eff >> 4);
 *     pb += (eff << 1) + (eff >> 2) + (eff >> 3) + (eff >> 5)   (unsigned shifts on eff)
 *     organ = (s(ph) g1 + s(2 ph) g2 + s(3 ph) g3 + s(4 ph) g4) >> 15, bell = (s(pa) ba + s(pb) bb) >> 15
 *     out = ((((organ + bell) * vg) >> 15) * OUT_GAIN) >> 15
 *   |organ + bell| <= 18000 + 8800, so the two last products are 16 x 16 fractional multiplies with truncation (the
 *   same as >> 15 of the integer product) and op1_sat16 never clips.
 *
 * s(x) = OP1_SINE_TABLE[x >> 19] read from the table's first quadrant only (entries 0 .. 2048, 4 KB), as MBIRA does
 * (tab: those entries, op1_sin_tab(): the core's L1 copy or OP1_SINE_TABLE itself, the same numbers):
 * k = EXTRACT(x, bits 19..30) sign-extended, tab[|k|], negated when x < 0 (two at a time: PACK of the phases' high
 * halves, >>> 15 per half, xor, -|-). The unit's table is lrint(32767 sin(2 pi k / 8192)) and exactly symmetric, so
 * this is the same number; the six partials of all 512 lines a block (the C) read 129. R7.L holds EXTRACT's pattern.
 * Host builds assemble it empty: bloodmoon.c keeps its C loop there.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

/* one sample of bm_vib (the store of the one before rides in its first packet: \st) */
.macro VIB st
	R4 = R0 >> 19 \st;
	R5 = R1 >> 19;
	P1 = R4;
	P2 = R5;
	P1 = P0 + (P1 << 1);
	P2 = P0 + (P2 << 1);
	R0 = R0 + R2 (NS) || R4.L = W[P1] || NOP;
	R1 = R1 + R3 (NS) || R4.H = W[P2] || NOP;
	A0 = R4.L * R6.L;
	R7.L = (A0 += R4.H * R6.H) (T);      /* fractional with truncation: (l1 va + l2 wa) >> 15 */
.endm

	.align 4
	.global _bm_vib
	.type _bm_vib, STT_FUNC
/* R0 = out, R1 = lfo, R2 = vinc, [SP + 12] = vinc3, [SP + 16] = amts. Sample 0 before the loop, 1 .. 127 in it,
 * each storing the one before; the last store after it */
_bm_vib:
	[--SP] = (R7:4, P5:3);               /* 7 words: the stack arguments now at SP + 40 */
	P5 = R1;
	I0 = R0;
	R3 = 0;
	L0 = R3;
	R3 = [SP + 40];                      /* vinc3 */
	R6 = [SP + 44];                      /* wa << 16 | va */
	R0 = [P5];                           /* vp */
	R1 = [P5 + 4];                       /* vq */
	P0.H = 0x0101;
	P0.L = 0xE5E8;                       /* OP1_SINE_TABLE */
	P4 = 127;
	VIB
	LSETUP (.Lv_b, .Lv_e) LC0 = P4;
.Lv_b:
	VIB "|| W[I0++] = R7.L || NOP"
.Lv_e:
	NOP;
	W[I0++] = R7.L;
	[P5] = R0;
	[P5 + 4] = R1;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _bm_vib, .-_bm_vib

/* two folded reads: R4 = s(Rb) << 16 | s(Ra); R6 holds the signs, then the next gain word (I1) */
.macro PAIR ra, rb
	R4 = EXTRACT(\ra, R7.L) (X);
	R4 = ABS R4;
	P1 = R4;
	R4 = EXTRACT(\rb, R7.L) (X);
	R4 = ABS R4;
	P2 = R4;
	P1 = P0 + (P1 << 1);
	P2 = P0 + (P2 << 1);
	R6 = PACK(\rb\().H, \ra\().H) || R4.L = W[P1] || NOP;
	R6 = R6 >>> 15 (V) || R4.H = W[P2] || NOP;
	R4 = R4 ^ R6;
	R4 = R4 -|- R6 || R6 = [I1++] || NOP;
.endm

/* one sample of bm_osc. On entry R4 = mod (sign-extended), R5 = inc, R6.L = the sample before (stored in the first
 * packet: \st). R0 / R1 / R2 = ph / pa / pb, R7.L = EXTRACT's pattern, I1 = the gain words (circular, loaded as
 * each pair ends) */
.macro OSC st
	R3 = R5 >>> 12 \st;
	R3 *= R4;
	R3 = R3 >>> 3;
	R3 = R3 + R5 (NS);                   /* eff */
	R0 = R0 + R3 (NS);                   /* ph */
	R4 = R3 >> 3;
	R5 = R3 >> 4;
	R5 = R5 + R4 (NS);
	R5 = R5 + R3 (NS);
	R1 = R1 + R5 (NS);                   /* pa */
	R5 = R3 >> 2;
	R5 = R5 + R4 (NS);
	R4 = R3 >> 5;
	R5 = R5 + R4 (NS);
	R4 = R3 << 1;
	R5 = R5 + R4 (NS);
	R2 = R2 + R5 (NS);                   /* pb */
	R5 = R0 << 1;                        /* 2 ph */
	R3 = R5 + R0 (NS);                   /* 3 ph */
	PAIR R0, R5                          /* R6: g2 << 16 | g1 */
	A1 = R4.H * R6.H, A0 = R4.L * R6.L (IS);
	R5 = R0 << 2;                        /* 4 ph */
	PAIR R3, R5                          /* g4 << 16 | g3 */
	A1 += R4.H * R6.H, A0 += R4.L * R6.L (IS);
	R3 = (A0 += A1);
	R3 = R3 >>> 15;                      /* organ */
	PAIR R1, R2                          /* bb << 16 | ba */
	A1 = R4.H * R6.H, A0 = R4.L * R6.L (IS) || R6 = [I1++] || NOP;   /* OUT_GAIN << 16 | vg */
	R4 = (A0 += A1);
	R4 = R4 >>> 15;                      /* bell */
	R3 = R3 + R4 (NS);
	R3.L = R3.L * R6.L (T) || R4 = W[P3++] (X) || R5 = [I2++];       /* the next sample's mod and inc */
	R6.L = R3.L * R6.H (T);
.endm

	.align 4
	.global _bm_osc
	.type _bm_osc, STT_FUNC
/* R0 = out, R1 = ph, R2 = g, [SP + 12] = inc, [SP + 16] = tab. P3 = out (read), I3 = out (written), I1 = g (circular, 4 words),
 * I2 = inc. Sample 0 before the loop, 1 .. 127 in it (each storing the one before), the last store after; the reads
 * run one sample ahead (the 129th of each lands after the arrays and is not used) */
_bm_osc:
	[--SP] = (R7:4, P5:3);
	P5 = R1;
	P3 = R0;
	I3 = R0;
	I1 = R2;
	B1 = R2;
	R3 = 16;
	L1 = R3;
	R3 = 0;
	L2 = R3;
	L3 = R3;
	R3 = [SP + 40];
	I2 = R3;
	R7 = 0x130C (Z);                     /* EXTRACT: 12 bits from bit 19 */
	R0 = [P5];
	R1 = [P5 + 4];
	R2 = [P5 + 8];
	P0 = [SP + 44];                      /* tab: the first quadrant (L1 or OP1_SINE_TABLE) */
	R4 = W[P3++] (X);
	R5 = [I2++];
	P4 = 127;
	OSC
	LSETUP (.Lo_b, .Lo_e) LC0 = P4;
.Lo_b:
	OSC "|| W[I3++] = R6.L || NOP"
.Lo_e:
	NOP;
	W[I3++] = R6.L;
	[P5] = R0;
	[P5 + 4] = R1;
	[P5 + 8] = R2;
	R3 = 0;
	L1 = R3;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _bm_osc, .-_bm_osc
#endif
