/* Clouds' inner sample loops, hand-scheduled for the Blackfin: the same arithmetic as clouds_dsp.c's C loops, bit for
 * bit, in fewer instruction packets. clouds_dsp.c calls them on the Blackfin unless CLOUDS_C_REF is defined; the host
 * builds (and a Blackfin build with CLOUDS_C_REF, the reference to check the kernels against) keep the C loops, and
 * this file assembles empty there.
 *
 * Every function leaves L0..L3 at 0, saves and restores the M registers it uses (cl_diffuse: M0, M2, M3), never touches
 * M1 and leaves no hardware loop armed.
 * Clouds: Mutable Instruments Clouds (c) 2014 Emilie Gillet, MIT license; this port: see clouds_dsp.h.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

/* int32_t cl_norm(int32_t *out, int32_t g, int32_t gn, int n): play_granular's normalization, g and gn within 1 .. 65535
 *   for t < n: g += ((gn - g) * 328) >> 15; out[t] = mulq15(out[t], g); returns g. g stays within 0 .. 65535 (it
 *   steps toward gn and can end 1 short of it), so mulq15 = 2 (out >> 16) g + ((out & 65535) g >> 15), g unsigned. */
	.align 4
	.global _cl_norm
	.type _cl_norm, STT_FUNC
_cl_norm:
	[--SP] = (R7:4);
	I0 = R0;
	I1 = R0;
	L0 = 0;
	L1 = 0;
	P1 = [SP + 28];                     /* n */
	R7 = 328 (X);
	LSETUP(.Lnorm_a, .Lnorm_b) LC0 = P1;
.Lnorm_a:
	R3 = R2 - R1 (NS) || R0 = [I0++] || NOP;
	R3 *= R7;
	R3 >>>= 15;
	R1 = R1 + R3 (NS);
	R5 = (A1 = R0.H * R1.L) (M, IS);
	R4 = (A0 = R0.L * R1.L) (IU);
	R4 = R4 >> 15;
	R4 = R4 + R5 (NS);
	R4 = R4 + R5 (NS);
.Lnorm_b:
	[I1++] = R4;
	R0 = R1;
	(R7:4) = [SP++];
	RTS;
	.size _cl_norm, .-_cl_norm

/* void cl_diffuse(int32_t *io, int32_t *db, int32_t wp, int32_t amount, int n): the diffuser (four allpasses in one
 *   2048-word ring, clouds_dsp.c diffuser()), wp = the first sample's write position (the C's wp after its first
 *   decrement), amount 1 .. 32767. K58(a) = (a * 5) >> 3 as (4a + a) >>> 3; io += mulq15(amount, acc - io) =
 *   2 ((x >> 16) amount) + ((x & 65535) amount >> 15) with x = acc - io. The ring in four circular pointers: I0
 *   reads line 3's tail (wp + 1021) and writes line 0 (wp); Ik (k = 1..3) reads line k - 1's tail (wp + 125, 306,
 *   576) and, 2 words on, writes line k (wp + 127, 308, 578). The eight words of a sample are all different and none
 *   is the next sample's, so the order inside a sample is free. M0 = 8, M2 = -12, M3 = -4084 (saved and restored). */
	.align 4
	.global _cl_diffuse
	.type _cl_diffuse, STT_FUNC
_cl_diffuse:
	[--SP] = (R7:4, P5:5);
	R7 = M0;
	R6 = M2;
	R5 = M3;
	[--SP] = R7;
	[--SP] = R6;
	[--SP] = R5;                        /* 5 + 3 words: amount, n at SP + 32 + 12 .. */
	P0 = R0;                            /* io */
	R7 = [SP + 44];                     /* amount */
	P1 = [SP + 48];                     /* n */
	B0 = R1;
	B1 = R1;
	B2 = R1;
	B3 = R1;
	R3 = 8192 (Z);
	L0 = R3;
	L1 = R3;
	L2 = R3;
	L3 = R3;
	R3 = 2047 (Z);
	R0 = 1021 (X);
	R0 = R0 + R2 (NS);
	R0 = R0 & R3;
	R0 <<= 2;
	R0 = R0 + R1;
	I0 = R0;
	R0 = 125 (X);
	R0 = R0 + R2 (NS);
	R0 = R0 & R3;
	R0 <<= 2;
	R0 = R0 + R1;
	I1 = R0;
	R0 = 306 (X);
	R0 = R0 + R2 (NS);
	R0 = R0 & R3;
	R0 <<= 2;
	R0 = R0 + R1;
	I2 = R0;
	R0 = 576 (X);
	R0 = R0 + R2 (NS);
	R0 = R0 & R3;
	R0 <<= 2;
	R0 = R0 + R1;
	I3 = R0;
	M0 = 8 (X);
	M2 = -12 (X);
	R0 = -4084 (X);
	M3 = R0;
	LSETUP(.Ldif_a, .Ldif_b) LC0 = P1;
.Ldif_a:
	MNOP || R4 = [P0] || R3 = [I0 ++ M3];               /* io; line 3's tail */
	R1 = [I1 ++ M0];                                    /* line 0's tail */
	R2 = R1 << 2;
	R2 = R2 + R1 (NS);
	R2 = R2 >>> 3;
	R0 = R4 + R2 (NS);
	R2 = R0 << 2 || [I0--] = R0 || NOP;
	R2 = R2 + R0 (NS);
	R2 = -R2;
	R2 = R2 >>> 3;
	R0 = R2 + R1 (NS) || R1 = [I2 ++ M0] || NOP;        /* line 1's tail */
	R2 = R1 << 2;
	R2 = R2 + R1 (NS);
	R2 = R2 >>> 3;
	R0 = R0 + R2 (NS);
	R2 = R0 << 2 || [I1 ++ M2] = R0 || NOP;
	R2 = R2 + R0 (NS);
	R2 = -R2;
	R2 = R2 >>> 3;
	R0 = R2 + R1 (NS) || R1 = [I3 ++ M0] || NOP;        /* line 2's tail */
	R2 = R1 << 2;
	R2 = R2 + R1 (NS);
	R2 = R2 >>> 3;
	R0 = R0 + R2 (NS);
	R2 = R0 << 2 || [I2 ++ M2] = R0 || NOP;
	R2 = R2 + R0 (NS);
	R2 = -R2;
	R2 = R2 >>> 3;
	R0 = R2 + R1 (NS);
	R2 = R3 << 2;                                       /* line 3 */
	R2 = R2 + R3 (NS);
	R2 = R2 >>> 3;
	R0 = R0 + R2 (NS);
	R2 = R0 << 2 || [I3 ++ M2] = R0 || I0 -= M3;
	R2 = R2 + R0 (NS);
	R2 = -R2;
	R2 = R2 >>> 3;
	R0 = R2 + R3 (NS);
	R2 = R0 - R4 (NS);                                  /* x = acc - io */
	R5 = (A1 = R7.L * R2.L) (M, IS);
	R2 = (A0 = R2.H * R7.L) (IS);
	R5 = R5 >>> 15;
	R2 = R2 + R2 (NS);
	R2 = R2 + R5 (NS);
	R4 = R4 + R2 (NS);
.Ldif_b:
	[P0++] = R4;
	L0 = 0;
	L1 = 0;
	L2 = 0;
	L3 = 0;
	R5 = [SP++];
	R6 = [SP++];
	R7 = [SP++];
	M3 = R5;
	M2 = R6;
	M0 = R7;
	(R7:4, P5:5) = [SP++];
	RTS;
	.size _cl_diffuse, .-_cl_diffuse

/* int cl_seed(uint32_t *st, int cnt, uint32_t thr, int32_t space, int det): play_granular's per-sample seeding tests
 *   while nothing else can seed (no trigger, not every sample): st = { rng, phasor }; for i < cnt: phasor += 256,
 *   rng = rng * 1664525 + 1013904223; a seed when rng < thr (unsigned) or, with det, phasor >= space (signed).
 *   Returns the seed's i (st holds that sample's state) or cnt (st after the cnt samples). i = cnt - LC0 inside the
 *   loop (LC0 counts the passes left, this one included). 5 packets per sample (8 with the phasor test). */
	.align 4
	.global _cl_seed
	.type _cl_seed, STT_FUNC
_cl_seed:
	[--SP] = (R7:4);
	P0 = R0;
	P1 = R1;
	R7 = [SP + 28];                     /* space */
	R0 = [SP + 32];                     /* det */
	R3 = [P0];                          /* rng */
	R4 = [P0 + 4];                      /* phasor */
	R5.L = 0x660D;
	R5.H = 0x0019;                      /* 1664525 */
	R6.L = 0xF35F;
	R6.H = 0x3C6E;                      /* 1013904223 */
	CC = R1 <= 0;
	IF CC JUMP .Lseed_end;
	CC = R0 == 0;
	R0 = 256 (Z);
	IF CC JUMP .Lseed_p;
	LSETUP(.Lseed_da, .Lseed_db) LC0 = P1;
.Lseed_da:
	R3 *= R5;
	R3 = R3 + R6 (NS);
	R4 = R4 + R0 (NS);
	CC = R3 < R2 (IU);
	IF CC JUMP .Lseed_hit;
	CC = R7 <= R4;
	IF CC JUMP .Lseed_hit;
.Lseed_db:
	NOP;                                /* (a loop does not end on a branch) */
	JUMP .Lseed_end;
.Lseed_p:                               /* no phasor test: the phasor steps after the generator's test */
	LSETUP(.Lseed_pa, .Lseed_pb) LC0 = P1;
.Lseed_pa:
	R3 *= R5;
	R3 = R3 + R6 (NS);
	CC = R3 < R2 (IU);
	IF CC JUMP .Lseed_hitp;
.Lseed_pb:
	R4 = R4 + R0 (NS);
.Lseed_end:
	[P0] = R3;
	[P0 + 4] = R4;
	R0 = R1;                            /* cnt: no seed */
	(R7:4) = [SP++];
	RTS;
.Lseed_hitp:
	R4 = R4 + R0 (NS);
.Lseed_hit:
	[P0] = R3;
	[P0 + 4] = R4;
	R0 = LC0;
	R0 = R1 - R0;
	R2 = 0;
	LC0 = R2;                           /* the loop left early: disarmed */
	(R7:4) = [SP++];
	RTS;
	.size _cl_seed, .-_cl_seed

/* void cl_rec(int16_t *dst, const int16_t *src, int n): buf_write_fade's plain write, dst[i] = clip16((src[i] * 32767)
 *   >> 15), as the truncated high half of the doubled product (src is int16, so the clip never acts); a pair per three
 *   packets. Reads up to two words past src[n - 1]. */
	.align 4
	.global _cl_rec
	.type _cl_rec, STT_FUNC
_cl_rec:
	I0 = R1;
	I1 = R0;
	L0 = 0;
	L1 = 0;
	R3.L = 0x7FFF;
	R3.H = 0x7FFF;
	R1 = R2 >> 1;
	P1 = R1;
	R2 <<= 31;
	R0.L = W[I0++];
	R0.H = W[I0++];
	CC = P1 == 0;
	IF CC JUMP .Lrec_odd;
	LSETUP(.Lrec_a, .Lrec_b) LC0 = P1;
.Lrec_a:
	R1.H = (A1 = R0.H * R3.H), R1.L = (A0 = R0.L * R3.L) (T) || R0.L = W[I0++] || NOP;
	MNOP || W[I1++] = R1.L || R0.H = W[I0++];
.Lrec_b:
	MNOP || W[I1++] = R1.H || NOP;
.Lrec_odd:
	CC = R2 == 0;
	IF CC JUMP .Lrec_end;
	R1.L = (A0 = R0.L * R3.L) (T);
	W[I1] = R1.L;
.Lrec_end:
	RTS;
	.size _cl_rec, .-_cl_rec
#endif
