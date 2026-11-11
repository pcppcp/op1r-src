/* Our master section's loops (docs/81 stage 3b-3), written from the bit-exact models (dsp/cw.c, dsp/mon.c):
 *   void mm_cw(int32_t *out, int sync)    codec write, stereo: L, R x cur^2 (+ 24-bit rounding), sync: L unity
 *   void mm_spk(int32_t *out)             codec write, speaker: mono high-pass, even words
 *   void mm_mon(void)                     master monitor -> the bus 0xFF800BB4, peak W[0xFF801084]
 * Volume ramp cur W[0x01DE4F70] += step W[0x01DE4F72] per sample. mul32 below is ADI's documented
 * mult_fr1x32x32 sequence. In L1 (.l1master). GPL-3.0-or-later with the OP-1 firmware permission. */
/* Our master section (docs/81 3b-3) in place of R246's Core_mix_master 0xFFA00E50: keeps what the caller may hold
 * across the call besides the C-saved registers (M1-M3, the second hardware loop), L0-L3 0 for C. */
	.section .l1master.0,"ax"
	.global _l1_master
	.align 2
_l1_master:
	[--SP] = RETS;
	[--SP] = M1; [--SP] = M2; [--SP] = M3;
	[--SP] = LC1; [--SP] = LT1; [--SP] = LB1;
	[--SP] = L0; [--SP] = L1; [--SP] = L2; [--SP] = L3;
	L0 = 0; L1 = 0; L2 = 0; L3 = 0;
	SP += -12;
	call _master_section;
	SP += 12;
	L3 = [SP++]; L2 = [SP++]; L1 = [SP++]; L0 = [SP++];
	LB1 = [SP++]; LT1 = [SP++]; LC1 = [SP++];
	M3 = [SP++]; M2 = [SP++]; M1 = [SP++];
	RETS = [SP++];
	rts;

	.section .l1master.3,"ax"

/* Rd = mul32(Ra, Rb) (fract32): A1 / A0 used */
#define MUL32(Rd, Ra, Rb, AL, AH, BL, BH) \
	A1 = AL * BL (FU); \
	A1 = A1 >> 16; \
	A1 += AH * BL (M), A0 = AH * BH; \
	A1 += BH * AL (M); \
	A1 = A1 >>> 15; \
	Rd = (A0 += A1);

	.global _mm_cw
	.align 2
_mm_cw:						/* R0 out, R1 sync */
	[--SP] = (R7:4, P5:4);
	P2 = R0;
	P0.L = 0x0E34; P0.H = 0xFF80;		/* master L */
	P1.L = 0x0F34; P1.H = 0xFF80;		/* master R */
	P4.L = 0x4F70; P4.H = 0x01DE;
	R7 = [P4];				/* L cur, H step */
	R5 = 0;
	BITSET (R5, 22);			/* 0x400000 */
	R4 = R1;				/* sync */
	P5 = 64;
	LSETUP (1f, 2f) LC0 = P5;
1:	R7.L = R7.L + R7.H (S);			/* cur */
	R6.L = R7.L * R7.L;			/* g */
	R6.H = 0x7FFF;
	R3 = R6 >> 16;				/* sync: L unity */
	CC = R4 == 0;
	IF CC R3 = R6;
	R1 = [P0++];
	R0 = R3.L * R1.H;
	R2.L = R3.L * R1.L (T);
	R2 = R2.L (X);
	R2 = R2 << 1;
	R0 = R0 + R2 (S);
	R0 = R0 + R5 (S);
	[P2++] = R0;
	R1 = [P1++];
	R0 = R6.L * R1.H;
	R2.L = R6.L * R1.L (T);
	R2 = R2.L (X);
	R2 = R2 << 1;
	R0 = R0 + R2 (S);
	R0 = R0 + R5 (S);
2:	[P2++] = R0;
	W[P4] = R7;
	(R7:4, P5:4) = [SP++];
	RTS;

	.global _mm_spk
	.align 2
_mm_spk:					/* R0 out */
	[--SP] = (R7:4, P5:3);
	P2 = R0;
	P0.L = 0x0E34; P0.H = 0xFF80;
	P1.L = 0x0F34; P1.H = 0xFF80;
	P4.L = 0x4F70; P4.H = 0x01DE;
	P5.L = 0xAC50; P5.H = 0x01AA;		/* state: v', x' */
	R7 = [P4];
	R4 = [P5];				/* v */
	R5 = [P5 + 4];				/* x' */
	R6.L = 0xCB8E; R6.H = 0x7BB8;		/* c */
	P3 = 64;
	LSETUP (1f, 2f) LC0 = P3;
1:	R7.L = R7.L + R7.H (S);
	R0 = [P0++];
	R1 = [P1++];
	R0 >>>= 1;
	R1 >>>= 1;
	R0 = R0 + R1 (S);			/* x */
	R1 = R0 + R4 (S);
	R1 = R1 - R5 (S);
	R5 = R0;
	MUL32(R4, R6, R1, R6.L, R6.H, R1.L, R1.H)	/* v */
	R1.L = R7.L * R7.L;
	R1 <<= 16;				/* G */
	MUL32(R0, R1, R4, R1.L, R1.H, R4.L, R4.H)
	[P2] = R0;
2:	P2 += 8;
	[P5] = R4;
	[P5 + 4] = R5;
	W[P4] = R7;
	(R7:4, P5:3) = [SP++];
	RTS;

	.global _mm_mon
	.align 2
_mm_mon:
	[--SP] = (R7:4, P5:4);
	P0.L = 0x0E34; P0.H = 0xFF80;
	P1.L = 0x0F34; P1.H = 0xFF80;
	P2.L = 0x0BB4; P2.H = 0xFF80;		/* the bus */
	P4.L = 0xEF6E; P4.H = 0x01B4;
	R7 = W[P4] (X);				/* m */
	P4.L = 0x1084; P4.H = 0xFF80;
	R4 = W[P4] (X);				/* peak */
	P5 = 64;
	LSETUP (1f, 2f) LC0 = P5;
1:	R0 = [P0++];
	R1 = [P1++];
	R3.L = R0.H * R7.L;
	R3.H = R1.H * R7.L;
	R0 = R3 >>> 1 (V);
	R0.L = R0.L + R0.H (S);			/* v */
	R2 = ABS R0 (V);
	R4 = MAX (R4, R2) (V);
2:	W[P2++] = R0;
	W[P4] = R4;
	(R7:4, P5:4) = [SP++];
	RTS;
