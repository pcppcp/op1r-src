/* Brassophone's sample loop (brass.c, brass_render) hand-scheduled for the Blackfin: the same arithmetic, bit for bit,
 * in 39 instruction packets per sample instead of about 127 (GCC -O2).
 *
 * void br_loop(br_blk *b)   (br_blk in brass.c: ph, lp, vph and envq, which it updates; out, warp_table and
 *                            OP1_PITCH_INC; k[], the block's ten constants, read in a circle through I3 / B3 / L3)
 *
 * Identities (each exact for every input the C code can see; knobs 0..32767, envelope any int16):
 *   - Q15 products that the C writes as (a * b) >> 15 are fractional multiplies with a truncating 16-bit extraction
 *     (2ab >> 16, (T)); the operands never are both -32768.
 *   - interp_amount: the saturating 16-bit add of interp_start (0..32767) and width * ev >> 15, then max with 0, is
 *     the C's clamp to 0..32767.
 *   - pitch bend: 892 * min(2 ev, 32767) >> 15 = min(1784 ev >> 15, 891); bend and vibrato come from one dual MAC.
 *   - ((inc >> 12) * mod) >> 3 = 2 (inc >> 16) mod + ((((inc >> 12) & 15) mod) >> 3): the low part in A1 (IS),
 *     shifted, then the high part added as a fractional product (which doubles it).
 *   - the warp table's row 64 repeats row 63 (all 32767), so rows s and s + 1 can always be read; at s = 63 the C's
 *     32768 - 32767 = 1 times frac >> 15 adds 0 as well.
 *   - both columns' row interpolation is one vector subtract, one dual MAC and one vector add ({col + 1 : col}).
 *   - the sine index: the warped phase is 0..32767 (it interpolates table values), so (x * 8191) >> 15 <= 8190 and
 *     the C's clamp never acts.
 *   - lp += ((sample - lp) * 16384) >> 15 is lp = (lp + sample) >>> 1; lp stays within int16, vel gain <= 32767, so
 *     (lp * vg) >> 16 is the product's high half and op1_sat16 never acts.
 *
 * Schedule: each pass of the loop finishes the previous sample (its sine lookup, low-pass and store) while it starts
 * the next one; the code before the loop starts sample 0 and the code after it finishes sample 127. Registers: R7 ph,
 * R6 lp, R5 envq, R0..R4 per sample; P0 sine table, P1 the previous sample's sine address, P2 the vibrato's, P3
 * inc[], P4 &vph (read and written once per sample); I0 out, I1 / I2 table rows s + 1 / s, I3 k[]; M2 / M3 the table
 * (+ one row). It saves and restores L0..L3 (L0..L2 zero inside, L3 = 40), leaves M1 alone, and uses LC0.
 * bcore counts packets, not cycles: the I1 / I2 writes just before their first use still stall on the hardware.
 * Host builds assemble it empty: brass.c keeps the C loop there, and -DBRASS_C_REF keeps it on the Blackfin too.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text
	.align 4
	.global _br_loop
	.type _br_loop, STT_FUNC
_br_loop:
	[--SP] = (R7:4, P5:3);
	[--SP] = L0;
	[--SP] = L1;
	[--SP] = L2;
	[--SP] = L3;
	P5 = R0;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	L2 = R0;
	R7 = [P5];                           /* ph */
	R6 = [P5 + 4];                       /* lp */
	P4 = P5;
	P4 += 8;                             /* &vph: read and written once per sample */
	R5 = [P5 + 12];                      /* envq */
	R0 = [P5 + 16];
	I0 = R0;                             /* out */
	R0 = [P5 + 20];
	M3 = R0;                             /* warp_table */
	R1 = 514;
	R0 = R0 + R1;
	M2 = R0;                             /* warp_table + one row */
	P3 = [P5 + 24];                      /* inc */
	R1 = [P5 + 64];                      /* k[9]: env_step */
	R0 = P5;
	R0 += 28;
	I3 = R0;
	B3 = R0;
	R0 = 40;
	L3 = R0;                             /* k[] read in a circle, once per sample */
	P0.H = 0x0101;
	P0.L = 0xE5E8;                       /* OP1_SINE_TABLE */

	/* sample 0 up to its sine index (the loop body without the previous sample's output) */
	R5 = R5 + R1 (NS) || R1 = [I3++] || NOP;            /* envq += env_step; vinc */
	R3 = [P4];                                          /* vph */
	R0 = [I3++];                                        /* {vamt : 1784} */
	R4 = R3 >> 19;
	P2 = R4;
	P2 = P0 + (P2 << 1);
	R3 = R3 + R1 (NS) || R2 = [I3++] || NOP;            /* {width : 257} */
	R1 = R5 >>> 7 || [P4] = R3 || R4 = [I3++];          /* ev; interp_start */
	R3.L = (A0 = R1.L * R2.H) (T) || R1.H = W[P2] || NOP;
	R3 = R3 +|+ R4 (S) || R4 = [I3++] || NOP;           /* 0 */
	R0.H = (A1 = R1.H * R0.H), R0.L = (A0 = R1.L * R0.L) (T) || R1 = [I3++] || NOP;   /* {32767 : 891} */
	R3 = MAX(R3, R4) (V) || R4 = [I3++] || NOP;         /* EXTRACT(7, 8) */
	R0 = MIN(R0, R1) (V) || R1 = [P3++] || NOP;
	R4 = EXTRACT(R3, R4.L) (Z);
	R0.L = R0.L + R0.H (NS);
	R7 = R7 + R1 (NS);
	R1.L = R1.L >> 12;
	A1 = R1.L * R0.L (IS);
	A1 = A1 >>> 3;
	R1 = (A1 += R1.H * R0.L) || R0 = [I3++] || NOP;     /* EXTRACT(11, 15) */
	R7 = R7 + R1 (NS);
	R1 = R7 >> 26;
	R1 = R1.L * R2.L (IS) || R2 = [I3++] || NOP;        /* {vel gain : 8191} */
	R1 = (R1 + R4) << 1;
	I2 = R1;
	I1 = R1;
	R0 = EXTRACT(R7, R0.L) (Z) || I2 += M3 || NOP;
	R3.L = R3.L << 9 || I1 += M2 || NOP;
	R3.L = R3.L >> 1 || R4.L = W[I2++] || R1.L = W[I1++];
	MNOP || R4.H = W[I2] || R1.H = W[I1];
	R1 = R1 -|- R4;
	R1.H = (A1 = R1.H * R0.L), R1.L = (A0 = R1.L * R0.L) (T);
	R1 = R1 +|+ R4;
	R0.L = R1.H - R1.L (NS);
	R0.L = (A0 = R0.L * R3.L) (T);
	R0.L = R0.L + R1.L (NS) || R1 = [I3++] || NOP;      /* env_step */
	R0.L = (A0 = R0.L * R2.L) (T);
	P1 = R0;
	P1 = P0 + (P1 << 1);

	/* samples 1 .. 127, each with the output of the one before */
	P2 = 127;
	LSETUP (.Lbr_b, .Lbr_e) LC0 = P2;
.Lbr_b:
	R5 = R5 + R1 (NS) || R3 = [P4] || R1 = [I3++];         /* envq += env_step; vph; vinc */
	R4 = R3 >> 19 || R0 = [I3++] || NOP;                   /* vibrato index; {vamt : 1784} */
	P2 = R4;
	P2 = P0 + (P2 << 1);
	R3 = R3 + R1 (NS) || R2 = W[P1] (X) || NOP;            /* vph += vinc; sine(n - 1) (P1 set four packets ago) */
	R6 = R6 + R2 (NS) || R2 = [I3++] || NOP;               /* lp + sine; {width : 257} */
	R1 = R5 >>> 7 || [P4] = R3 || R4 = [I3++];             /* ev; interp_start */
	R3.L = (A0 = R1.L * R2.H) (T) || R1.H = W[P2] || NOP;  /* width * ev >> 15; R1 = {vibrato sine : ev} */
	R3 = R3 +|+ R4 (S) || R4 = [I3++] || NOP;              /* + interp_start, saturated; 0 */
	R0.H = (A1 = R1.H * R0.H), R0.L = (A0 = R1.L * R0.L) (T) || R1 = [I3++] || NOP;   /* {vibrato : ev * 1784 >> 15}; {32767 : 891} */
	R3 = MAX(R3, R4) (V) || R4 = [I3++] || NOP;            /* R3.L = interp_amount; EXTRACT(7, 8) */
	R0 = MIN(R0, R1) (V) || R1 = [P3++] || NOP;            /* bend = min(ev * 1784 >> 15, 891); inc[n] */
	R4 = EXTRACT(R3, R4.L) (Z);                            /* col */
	R0.L = R0.L + R0.H (NS);                               /* mod */
	R7 = R7 + R1 (NS);                                     /* ph += inc[n] */
	R1.L = R1.L >> 12;                                     /* R1 = {inc >> 16 : (inc >> 12) & 15} */
	A1 = R1.L * R0.L (IS);
	A1 = A1 >>> 3;
	R1 = (A1 += R1.H * R0.L) || R0 = [I3++] || NOP;        /* ((inc >> 12) * mod) >> 3; EXTRACT(11, 15) */
	R7 = R7 + R1 (NS);                                     /* ph */
	R1 = R7 >> 26;                                         /* s */
	R1 = R1.L * R2.L (IS) || R2 = [I3++] || NOP;           /* s * 257; {vel gain : 8191} */
	R1 = (R1 + R4) << 1;
	I2 = R1;
	I1 = R1;
	R0 = EXTRACT(R7, R0.L) (Z) || I2 += M3 || NOP;         /* frac; I2 = &table[s][col] */
	R3.L = R3.L << 9 || I1 += M2 || NOP;                   /* I1 = &table[s + 1][col] */
	R3.L = R3.L >> 1 || R4.L = W[I2++] || R1.L = W[I1++];  /* col_frac */
	R6 = R6 >>> 1 || R4.H = W[I2] || R1.H = W[I1];         /* lp(n - 1); R4 = row s, R1 = row s + 1 ({col + 1 : col}) */
	R1 = R1 -|- R4;
	R1.H = (A1 = R1.H * R0.L), R1.L = (A0 = R1.L * R0.L) (T);
	R1 = R1 +|+ R4;                                        /* {w1 : w0} */
	R4 = R6.L * R2.H (IS);                                 /* out(n - 1) << 16 */
	R0.L = R1.H - R1.L (NS) || W[I0++] = R4.H || NOP;
	R0.L = (A0 = R0.L * R3.L) (T);
	R0.L = R0.L + R1.L (NS) || R1 = [I3++] || NOP;         /* warped phase; env_step */
	R0.L = (A0 = R0.L * R2.L) (T);                         /* sine index (R0.H = 0 from the EXTRACT) */
	P1 = R0;
.Lbr_e:
	P1 = P0 + (P1 << 1);

	/* sample 127's output */
	R0 = W[P1] (X);
	R6 = R6 + R0 (NS);
	R6 = R6 >>> 1;
	R4 = R6.L * R2.H (IS);
	W[I0++] = R4.H;
	[P5] = R7;
	[P5 + 4] = R6;
	[P5 + 12] = R5;
	L3 = [SP++];
	L2 = [SP++];
	L1 = [SP++];
	L0 = [SP++];
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _br_loop, .-_br_loop
#endif
