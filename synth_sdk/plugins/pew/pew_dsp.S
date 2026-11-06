/* Pew's sample loop, hand-scheduled for the Blackfin: the same arithmetic as pew.c's C loop (kept there for host
 * builds), bit for bit, in two passes over the block so each keeps its state in registers.
 *
 * void pew_osc(uint32_t *mh, uint32_t *st)
 *   st = { ph, hph, hv, pitch, low, mh table, pl }; per sample inc = pitch[i] >> 1, ph += inc,
 *   hph += inc + ((int32_t)(inc >> 16) * hv >> 5) (one 16 x 16 multiply: inc >> 16 < 2^15, |hv| <= 16048, never
 *   wraps), lo = read(low, ph), up | perc = read(mh table, hph) (perc << 16 | upper per point,
 *   one 32-bit load, so the dual subtract, multiply and add do both at once), hi = up + (perc * pl >> 15) (a
 *   truncating fractional multiply, the 16-bit sum: pew.c keeps the bound of |up| + |perc| under 32,000), then
 *   mh[i + 1] = hi << 16 | lo (the store runs one sample behind: mh[0] is a spare word it writes first);
 *   read(w, p) = a + ((b - a) * f >> 15) with a = w[p >> 23], b the next point, f = bits 8..22 of p (EXTRACT);
 *   b - a fits 16 bits in all three tables (pew.c's tables are smooth: the steepest step, every bar out, is under
 *   4,000), so the product is a truncating fractional multiply
 *   and the sum stays between a and b. ph and hph go back to st. Reads pitch[128] (one word ahead, not used).
 * void pew_out(int16_t *out, const uint32_t *mh, const int32_t *c)
 *   c = { horn_amp << 16 | drum_amp, 2 gain, cb or 0, knee+, knee- }: y = (lo * drum_amp + hi * horn_amp) >> 14 (one
 *   40-bit sum, below 2^31 in size), y += cb[i], the overdrive as c = clamp(y, knee-, knee+), y = c + ((y - c) >>> 2)
 *   (the C's two branches, floor shifts on both sides), out[i] = sat16(y * 2gain >> 16) as a 32 x 16 chain in MAC1:
 *   (y.L * 2gain >> 16) + y.H * 2gain (signed x unsigned), exact for any y, saturated by the 16-bit extraction as
 *   engine_sat16 (2 gain <= 65535 fits unsigned 16). Reads mh[128] (one word ahead, not used).
 *
 * Mixed-mode (IS,M) products are MAC1 only: first operand signed, second unsigned. Each pass zeroes the L registers
 * it uses; pew.c restores the caller's L0..L3 after the block, as mbira.c does. M1 is not touched (R246's PmSynth
 * keeps its frame there). No shift sits beside a P-register store (anomaly 05000074).
 * Host builds (screen and golden harnesses) assemble it empty: pew.c keeps the C loop there.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

/* ---- pew_osc: 23 packets per sample ---- */
/* R5 ph, R6 hph, R7 hv, R4.L the EXTRACT pattern (bits 8..22), R4.H pl, R1 the next pitch word; P3 the low table,
 * P4 the mh table */
	.align 4
	.global _pew_osc
	.type _pew_osc, STT_FUNC
_pew_osc:
	[--SP] = (R7:4, P5:3);
	[--SP] = R1;
	P0 = R1;
	I2 = R0;
	R0 = 0;
	L0 = R0;
	L2 = R0;
	R5 = [P0];
	R6 = [P0 + 4];
	R7 = [P0 + 8];
	R0 = [P0 + 12];
	P3 = [P0 + 16];
	P4 = [P0 + 20];
	I0 = R0;
	R4 = [P0 + 24];                                  /* pl */
	R4 <<= 16;
	R0 = 0x080F (Z);                                 /* EXTRACT: position 8, length 15 */
	R4 = R4 | R0;
	P1 = 128;
	R1 = [I0++];
	LSETUP (.La_b, .La_e) LC0 = P1;
.La_b:
	R0 = R1 >> 1 || [I2++] = R3;                     /* inc; the sample before (mh[-1] the first time) */
	R5 = R5 + R0 (NS);                               /* ph */
	R2 = R5 >> 23;
	P0 = R2;
	R1 = R0.H * R7.L (IS);                           /* (inc >> 16) hv, 16 x 16 */
	P0 = P3 + (P0 << 1);
	R1 = R1 >>> 5;
	R0 = R0 + R1 (NS) || R2 = W[P0] (X) || NOP;      /* a (low) */
	R6 = R6 + R0 (NS) || R1 = W[P0 + 2] (X) || NOP;  /* hph; b (low) */
	R0 = R6 >> 23;
	P0 = R0;
	R3 = EXTRACT(R5, R4.L) (Z);                      /* f (low) */
	R1.L = R1.L - R2.L (NS);
	P1 = P4 + (P0 << 2);
	R1.L = R1.L * R3.L (T) || R0 = [P1] || NOP;      /* a (perc | upper) */
	R1.L = R1.L + R2.L (NS) || R3 = [P1 + 4] || NOP; /* lo; b (perc | upper) */
	R2 = EXTRACT(R6, R4.L) (Z);                      /* f (upper, perc); R2.H = 0 */
	R3 = R3 -|- R0;
	R3.H = R3.H * R2.L, R3.L = R3.L * R2.L (T);
	R3 = R3 +|+ R0;                                  /* perc | up */
	R2.L = R3.H * R4.H (T);                          /* perc * pl */
	R3.H = R3.L + R2.L (NS);                         /* hi */
.La_e:
	R3 = PACK(R3.H, R1.L) || R1 = [I0++];            /* hi | lo; the next pitch word (one ahead) */
	[I2++] = R3;
	R2 = [SP++];
	P0 = R2;
	[P0] = R5;
	[P0 + 4] = R6;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _pew_osc, .-_pew_osc

/* ---- pew_out: 11 packets per sample (12 with the click) ---- */
/* R7 horn_amp | drum_amp, R6 2 gain, R4 knee+, R5 knee-; R0 the word, R1 y, R2 work, R3.H the sample out */
.macro OUT_SAMPLE st, ck
	.if \st
	A0 = R0.L * R7.L (IS) || W[I1++] = R3.H;         /* the sample before */
	.else
	A0 = R0.L * R7.L (IS);
	.endif
	R2 = (A0 += R0.H * R7.H) (IS);
	.if \ck
	R1 = R2 >>> 14 || R0 = [I2++];
	R1 = R1 + R0 (NS);
	.else
	R1 = R2 >>> 14;
	.endif
	R2 = MAX(R1, R5);
	R2 = MIN(R2, R4);                                /* c */
	R1 = R1 - R2 (NS);
	R1 = R1 >>> 2;
	R1 = R1 + R2 (NS);                               /* y */
	A1 = R1.L * R6.L (FU);
	A1 = A1 >> 16 || R0 = [I0++];                    /* the next word (one ahead) */
.endm
/* the sample's last packet (the loops' end label sits on it) */
.macro OUT_LAST
	R3.H = (A1 += R1.H * R6.L) (IS,M);
.endm

	.align 4
	.global _pew_out
	.type _pew_out, STT_FUNC
_pew_out:
	[--SP] = (R7:4);
	P0 = R2;
	I1 = R0;
	I0 = R1;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	L2 = R0;
	R7 = [P0];
	R6 = [P0 + 4];
	R0 = [P0 + 8];
	I2 = R0;
	R4 = [P0 + 12];
	R5 = [P0 + 16];
	P1 = 127;
	CC = R0 == 0;
	R0 = [I0++];
	IF CC JUMP .Ld_plain;
	OUT_SAMPLE 0, 1
	OUT_LAST
	LSETUP (.Ld_cb, .Ld_ce) LC0 = P1;
.Ld_cb:
	OUT_SAMPLE 1, 1
.Ld_ce:
	OUT_LAST
	JUMP .Ld_done;
.Ld_plain:
	OUT_SAMPLE 0, 0
	OUT_LAST
	LSETUP (.Ld_pb, .Ld_pe) LC0 = P1;
.Ld_pb:
	OUT_SAMPLE 1, 0
.Ld_pe:
	OUT_LAST
.Ld_done:
	W[I1++] = R3.H;
	(R7:4) = [SP++];
	RTS;
	.size _pew_out, .-_pew_out
#endif
