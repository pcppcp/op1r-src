/* kennel: Hollow Saint's per-sample voice loops (the Lo-Fi Relic, the drone and the wheel buzz), hand-scheduled for
 * the Blackfin: the same arithmetic as the C loops of relic_run, drone_run and buzz_run in hollow_saint.c (kept
 * there for host builds and as the reference, HS_C_REF), bit for bit. Those C functions stay the callers: they
 * compute the per-block and per-sub-block values into a table k (layouts below, K_* word indices), call the kernel
 * for the 64 samples and copy the state back.
 *
 * void hs_relic(int32_t *h, int32_t *k)
 *   relic_run's loop in place on h (hb, or the shared bus buffer). k: 0 1664525, 1 0xDA29F01B (the LCG's addend
 *   times 1664525^-1: (r + c) * 1664525 = r * 1664525 + 1013904223), 2 rinc, 3 sh - 32, 4 hold - half (in/out),
 *   5 mask, 6 a, 7 lp2 (in/out), 8 -16384, 9 hiss, 10 make, 11 pthr, 12 drive, 13 32767, 14 half, 15 popsz,
 *   16..23 mix per sub-block, 24 rng, 25 lp, 26 pop, 27 rph (in/out). 43 packets per sample, plus 17 on a sample-
 *   rate wrap (the saturator, as the C: only a held sample needs it) and 5 on a pop.
 * void hs_drone(int32_t *b, const uint32_t *ri, int32_t *k)
 *   drone_run's loop, added to b. Phases p0 / p1 are held XOR 0x80000000 (the saw's ramp is the high half; the
 *   polyBLEP window, t < dt or t > 32767 - dt, is one unsigned compare: p + (dt << 17) + 2^31 <= (dt << 18) - 1).
 *   k: 0 roff, 1 foff, 2 A0, 3 L0, 4 A1, 5 L1 (the window tests), 6 fifth, 7 fd, 8 qd, 9 sub, 10 65535, 11 -65535,
 *   12 gd (this sub-block), 13 p2 (in/out), 14 OP1_SINE_TABLE, 15 spare, 16 rdt, 17 rinv, 18 fdt, 19 finv,
 *   20 p0 ^ 2^31, 21 p1 ^ 2^31, 22 dlp, 23 dbp (in/out), 24..32 gd per sub-block (one spare). 41 packets per
 *   sample, plus ~22 per polyBLEP correction.
 * void hs_buzz(int32_t *b, const uint32_t *ri, int32_t *k)
 *   buzz_run's loop, added to b. k: 0 tsh, 1 0xDA29F01B, 2 1664525, 3 -16384, 4 32768, 5 13693 (tlc), 6 fr, 7 rq,
 *   8 tg, 9 gz (this sub-block), 10 tlp, 11 rlp, 12 rattle (in/out), 13 65535, 14 -65535, 15 amp (this sub-block),
 *   16 jit, 17 bnz, 18 32767, 19 tph, 20 rng, 21 slap, 22 rbp (in/out), 23 spare, 24..50 { tg, gz, amp } per
 *   sub-block (one spare triple). 45 packets per sample, plus 17 on a slap (a new trompette cycle).
 *
 * m16 products are 16 x 16 (.L * .L (IS): the C's int16 casts); every other product is a 32-bit multiply (it wraps
 * as the C's int32 product). The int16 clamps are a saturating << 16 and >>> 16. Phase wraps: new < inc (unsigned)
 * is old + inc wrapping. Stores to the buffer run a sample behind through a circular I1 over its 64 words that starts
 * at word 63 (the first pass writes word 63's own value back; the last result goes there after the loop). Out-of-line
 * code for the rare branches (the relic's wrap and pops, the polyBLEP corrections, the slaps) jumps back into the
 * loop body. No packet stores a register another op of it writes or loads one another reads; no shift beside a
 * P-register store (anomaly 05000074). Reads go one word past ri's end (not used). L0..L3 are saved and restored,
 * M1 is not touched. Host builds assemble this file empty: hollow_saint.c keeps the C loops there.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

.macro SAVE_L
	[--SP] = L0;
	[--SP] = L1;
	[--SP] = L2;
	[--SP] = L3;
.endm
.macro REST_L
	L3 = [SP++];
	L2 = [SP++];
	L1 = [SP++];
	L0 = [SP++];
.endm

/* ---- hs_relic ----
 * R7 rng, R6 lp, R4 pop, R3 rph (advanced one sample ahead: the wrap flag for sample n is computed at the end of
 * sample n - 1 and stays in CC across the next sample's LCG), R5 / R0 / R1 / R2 scratch (R5 holds lp2 through the
 * low-passes; lp2 and hold - half live in k). Top of a sample: R0 the previous result (stored by the first packet),
 * R1 the LCG addend, R2 pthr, CC the wrap flag. P1 = k, I0 reads h, I1 writes h (circular), I2 = this sub-block's
 * mix (k + 64, stepped per sub-block), I3 = &k[11] (pthr). */
	.align 4
	.global _hs_relic
	.type _hs_relic, STT_FUNC
_hs_relic:
	[--SP] = (R7:4, P5:3);
	SAVE_L
	P1 = R1;
	I0 = R0;
	B1 = R0;
	R2 = 252;
	R2 = R0 + R2;
	I1 = R2;
	R2 = 256;
	L1 = R2;
	R2 = 0;
	L0 = R2;
	L2 = R2;
	L3 = R2;
	R2 = 64;
	R2 = R1 + R2;
	I2 = R2;
	R2 = 44;
	R2 = R1 + R2;
	I3 = R2;
	R7 = [P1 + 96];                                  /* rng */
	R6 = [P1 + 100];                                 /* lp */
	R4 = [P1 + 104];                                 /* pop */
	R3 = [P1 + 108];                                 /* rph */
	R0 = [P1 + 8];                                   /* rinc */
	R3 = R3 + R0 (NS);
	CC = R3 < R0 (IU);                               /* sample 0's wrap */
	R0 = [I1];                                       /* word 63, written back unchanged by the first pass */
	R1 = [P1 + 4];
	R2 = [I3];
	P4 = 8;
	LSETUP (.Lr_ob, .Lr_oe) LC1 = P4;
.Lr_ob:
	LSETUP (.Lr_ib, .Lr_ie) LC0 = P4;
.Lr_ib:
	R7 = R7 + R1 (NS) || R5 = [P1] || [I1++] = R0;  /* rng = (rng + c) * 1664525 */
	R7 *= R5;
	IF CC JUMP .Lr_wrap;                             /* hold = y: the saturator on this x */
.Lr_w1:
	CC = R7 < R2 (IU);
	IF CC JUMP .Lr_pop;
.Lr_p1:
	R2 = R4 >>> 1 || R0 = [P1 + 12];                 /* pop >> 1; sh - 32 */
	R0 = LSHIFT R7 BY R0.L || R1 = [P1 + 16];        /* r >> (32 - sh); hold - half */
	R0 = R0 + R1 (NS) || R5 = [P1 + 20];
	R0 = R0 & R5;
	R0 = R0 + R4 (NS) || R1 = [P1 + 24];             /* + pop; a */
	R4 = R4 - R2 (NS) || R5 = [P1 + 28];             /* pop -= pop >> 1; lp2 */
	R0 = R0 << 16 (S);
	R0 = R0 >>> 16;                                  /* q */
	R0 = R0 - R6 (NS);                               /* lp += m16((q - lp) >> 1, a) >> 14, clamped */
	R0 = R0 >>> 1;
	R0 = R0.L * R1.L (IS);
	R0 = R0 >>> 14;
	R6 = R6 + R0 (NS);
	R6 = R6 << 16 (S);
	R6 = R6 >>> 16;
	R0 = R6 - R5 (NS);                               /* lp2 likewise */
	R0 = R0 >>> 1 || R2 = [P1 + 32];                 /* -16384 */
	R0 = R0.L * R1.L (IS);
	R0 = R0 >>> 14 || R1 = [P1 + 36];                /* hiss */
	R5 = R5 + R0 (NS);
	R5 = R5 << 16 (S);
	R5 = R5 >>> 16;
	R0 = R7 >> 17;                                   /* nz = (r >> 17) - 16384 */
	R0 = R0 + R2 (NS) || [P1 + 28] = R5;
	R0 = R0.L * R1.L (IS) || R2 = [P1 + 40];         /* make */
	R0 = R0 >>> 15;
	R0 = R0 + R5 (NS) || R1 = [P1 + 8];              /* rinc */
	R0 = R0 << 16 (S);
	R0 = R0 >>> 16;
	R0 = R0.L * R2.L (IS) || R5 = [I0++];            /* wet; x */
	R0 = R0 >>> 14 || R2 = [I2];                     /* mix */
	R3 = R3 + R1 (NS);                               /* the next sample's rph */
	R0 = R0 - R5 (NS);
	R0 = R0 >>> 1;
	R0 *= R2;
	CC = R3 < R1 (IU);                               /* the next sample's wrap */
	R0 = R0 >>> 14 || R1 = [P1 + 4] || R2 = [I3];
.Lr_ie:
	R0 = R0 + R5 (NS);                               /* x + (((wet - x) >> 1) * mix >> 14) */
.Lr_oe:
	I2 += 4;
	[I1++] = R0;
	R1 = [P1 + 8];
	R3 = R3 - R1 (NS);                               /* undo the 65th advance */
	[P1 + 96] = R7;
	[P1 + 100] = R6;
	[P1 + 104] = R4;
	[P1 + 108] = R3;
	REST_L
	(R7:4, P5:3) = [SP++];
	RTS;

/* the held sample: y = (3 s - (m16(m16(s, s) >> 15, s) >> 15)) >> 1, s = clamp(m16(x >> 1, drive) >> 12, +-32767);
 * stored as hold - half. Uses R0 / R1 / R5; R2 (pthr) stays. */
.Lr_wrap:
	R0 = [I0];
	R0 = R0 >>> 1 || R1 = [P1 + 48];                 /* drive */
	R0 = R0.L * R1.L (IS);
	R1 = [P1 + 52];                                  /* 32767 */
	R0 = R0 >>> 12;
	R0 = MIN(R0, R1);
	R1 = -R1;
	R0 = MAX(R0, R1);
	R1 = R0.L * R0.L (IS);
	R1 = R1 >>> 15;
	R1 = R1.L * R0.L (IS);
	R1 = R1 >>> 15;
	R5 = R0 + R0 (NS);
	R5 = R5 + R0 (NS);
	R5 = R5 - R1 (NS);
	R1 = [P1 + 56];                                  /* half */
	R5 = R5 >>> 1;
	R5 = R5 - R1 (NS);
	[P1 + 16] = R5;
	JUMP .Lr_w1;
/* a pop: (r & 0x100) ? popsz : -popsz. Uses R0 */
.Lr_pop:
	R4 = [P1 + 60];
	R0 = -R4;
	CC = BITTST (R7, 8);
	IF !CC R4 = R0;
	JUMP .Lr_p1;
	.size _hs_relic, .-_hs_relic

#ifdef HS_RELIC_LITE
/* ---- hs_relic_lite (HS_RELIC_LITE): the Lo-Fi Relic with one tape-head pole and no dither: q = (hold & mask) + pop.
 * The LCG still steps per sample (it drives the hiss and the pops). k as hs_relic's, except k[4] = hold & mask
 * (in/out) and k[28] = hold (in/out); k[3], k[7] (lp2) and k[14] are not used. 33 packets per sample. */
	.align 4
	.global _hs_relic_lite
	.type _hs_relic_lite, STT_FUNC
_hs_relic_lite:
	[--SP] = (R7:4, P5:3);
	SAVE_L
	P1 = R1;
	I0 = R0;
	B1 = R0;
	R2 = 252;
	R2 = R0 + R2;
	I1 = R2;
	R2 = 256;
	L1 = R2;
	R2 = 0;
	L0 = R2;
	L2 = R2;
	L3 = R2;
	R2 = 64;
	R2 = R1 + R2;
	I2 = R2;
	R2 = 44;
	R2 = R1 + R2;
	I3 = R2;
	R7 = [P1 + 96];                                  /* rng */
	R6 = [P1 + 100];                                 /* lp */
	R4 = [P1 + 104];                                 /* pop */
	R3 = [P1 + 108];                                 /* rph */
	R0 = [P1 + 8];                                   /* rinc */
	R3 = R3 + R0 (NS);
	CC = R3 < R0 (IU);                               /* sample 0's wrap */
	R0 = [I1];                                       /* word 63, written back unchanged by the first pass */
	R1 = [P1 + 4];
	R2 = [I3];
	P4 = 8;
	LSETUP (.Lrl_ob, .Lrl_oe) LC1 = P4;
.Lrl_ob:
	LSETUP (.Lrl_ib, .Lrl_ie) LC0 = P4;
.Lrl_ib:
	R7 = R7 + R1 (NS) || R5 = [P1] || [I1++] = R0;  /* rng = (rng + c) * 1664525 */
	R7 *= R5;
	IF CC JUMP .Lrl_wrap;
.Lrl_w1:
	CC = R7 < R2 (IU);
	IF CC JUMP .Lrl_pop;
.Lrl_p1:
	R2 = R4 >>> 1 || R0 = [P1 + 16];                 /* pop >> 1; hold & mask */
	R0 = R0 + R4 (NS) || R1 = [P1 + 24];             /* + pop; a */
	R4 = R4 - R2 (NS) || R5 = [P1 + 32];             /* pop -= pop >> 1; -16384 */
	R0 = R0 << 16 (S);
	R0 = R0 >>> 16;                                  /* q */
	R0 = R0 - R6 (NS);                               /* lp += m16((q - lp) >> 1, a) >> 14, clamped */
	R0 = R0 >>> 1;
	R0 = R0.L * R1.L (IS);
	R0 = R0 >>> 14 || R1 = [P1 + 36];                /* hiss */
	R6 = R6 + R0 (NS);
	R6 = R6 << 16 (S);
	R6 = R6 >>> 16;
	R0 = R7 >> 17;                                   /* nz = (r >> 17) - 16384 */
	R0 = R0 + R5 (NS);
	R0 = R0.L * R1.L (IS) || R2 = [P1 + 40];         /* make */
	R0 = R0 >>> 15;
	R0 = R0 + R6 (NS) || R1 = [P1 + 8];              /* rinc */
	R0 = R0 << 16 (S);
	R0 = R0 >>> 16;
	R0 = R0.L * R2.L (IS) || R5 = [I0++];            /* wet; x */
	R0 = R0 >>> 14 || R2 = [I2];                     /* mix */
	R3 = R3 + R1 (NS);                               /* the next sample's rph */
	R0 = R0 - R5 (NS);
	R0 = R0 >>> 1;
	R0 *= R2;
	CC = R3 < R1 (IU);                               /* the next sample's wrap */
	R0 = R0 >>> 14 || R1 = [P1 + 4] || R2 = [I3];
.Lrl_ie:
	R0 = R0 + R5 (NS);
.Lrl_oe:
	I2 += 4;
	[I1++] = R0;
	R1 = [P1 + 8];
	R3 = R3 - R1 (NS);                               /* undo the 65th advance */
	[P1 + 96] = R7;
	[P1 + 100] = R6;
	[P1 + 104] = R4;
	[P1 + 108] = R3;
	REST_L
	(R7:4, P5:3) = [SP++];
	RTS;

/* the held sample, as hs_relic's, stored in k[28] and as hold & mask in k[4] */
.Lrl_wrap:
	R0 = [I0];
	R0 = R0 >>> 1 || R1 = [P1 + 48];                 /* drive */
	R0 = R0.L * R1.L (IS);
	R1 = [P1 + 52];                                  /* 32767 */
	R0 = R0 >>> 12;
	R0 = MIN(R0, R1);
	R1 = -R1;
	R0 = MAX(R0, R1);
	R1 = R0.L * R0.L (IS);
	R1 = R1 >>> 15;
	R1 = R1.L * R0.L (IS);
	R1 = R1 >>> 15;
	R5 = R0 + R0 (NS);
	R5 = R5 + R0 (NS);
	R5 = R5 - R1 (NS);
	R1 = [P1 + 20];                                  /* mask */
	R5 = R5 >>> 1;
	[P1 + 112] = R5;
	R5 = R5 & R1;
	[P1 + 16] = R5;
	JUMP .Lrl_w1;
.Lrl_pop:
	R4 = [P1 + 60];
	R0 = -R4;
	CC = BITTST (R7, 8);
	IF !CC R4 = R0;
	JUMP .Lrl_p1;
	.size _hs_relic_lite, .-_hs_relic_lite
#endif

/* ---- hs_drone ----
 * R7 p0 ^ 2^31, R6 p1 ^ 2^31, R5 dlp, R3 dbp, R0 / R1 / R2 / R4 scratch; p2 lives in k (I2). Top of a sample: R1
 * this sample's ri, R4 the previous result (stored by the first packet). P1 = k, P0 reads b, P3 walks the gd table,
 * P5 = the sine table, P2 the sine entry; I0 reads ri, I1 writes b (circular), I2 = &k[13]. */

	.align 4
	.global _hs_drone
	.type _hs_drone, STT_FUNC
_hs_drone:
	[--SP] = (R7:4, P5:3);
	SAVE_L
	P1 = R2;
	P0 = R0;
	I0 = R1;
	B1 = R0;
	R1 = 252;
	R1 = R0 + R1;
	I1 = R1;
	R1 = 256;
	L1 = R1;
	R1 = 0;
	L0 = R1;
	L2 = R1;
	L3 = R1;
	R1 = 52;
	R1 = R2 + R1;
	I2 = R1;
	P5 = [P1 + 56];
	P3 = P1;
	P4 = 96;
	P3 = P3 + P4;
	R7 = [P1 + 80];
	R6 = [P1 + 84];
	R5 = [P1 + 88];
	R3 = [P1 + 92];
	R0 = [P3++];
	[P1 + 48] = R0;                                  /* gd, sub-block 0 */
	R4 = [I1];
	R1 = [I0++];
	P4 = 8;
	LSETUP (.Ld_ob, .Ld_oe) LC1 = P4;
.Ld_ob:
	LSETUP (.Ld_ib, .Ld_ie) LC0 = P4;
.Ld_ib:
	R2 = R1 >> 1 || [I1++] = R4 || R0 = [P1];       /* r >> 1; roff */
	R6 = R6 + R1 (NS) || R4 = [I2];                  /* p2 */
	R7 = R7 + R1 (NS);
	R7 = R7 + R0 (NS) || R1 = [P1 + 4];              /* p0 += r + roff; foff */
	R6 = R6 + R2 (NS) || R0 = [P1 + 8];              /* A0 */
	R6 = R6 + R1 (NS);                               /* p1 += r + (r >> 1) + foff */
	R4 = R4 + R2 (NS) || R1 = [P1 + 12];             /* p2 += r >> 1; L0 */
	R0 = R7 + R0 (NS) || [I2] = R4;
	CC = R0 <= R1 (IU);
	IF CC JUMP .Ld_b0;
	R1 = R7 >>> 17 || R0 = [P1 + 16];                /* saw0 >> 1; A1 */
.Ld_r0:
	R0 = R6 + R0 (NS) || R2 = [P1 + 20];             /* L1 */
	CC = R0 <= R2 (IU);
	IF CC JUMP .Ld_b1;
	R2 = R6 >>> 16 || R0 = [P1 + 24];                /* saw1; fifth */
.Ld_r1:
	R2 = R2.L * R0.L (IS);
	R2 = R2 >>> 16 || R0 = [P1 + 28];                /* fd */
	R1 = R1 + R2 (NS);                               /* x */
	R0 *= R3;
	R0 = R0 >>> 15 || R2 = [P1 + 32];                /* qd */
	R5 = R5 + R0 (NS);                               /* dlp += (fd * dbp) >> 15 */
	R1 = R1 >>> 1;
	R1 = R1 - R5 (NS);
	R2 *= R3;
	R2 = R2 >>> 15 || R0 = [P1 + 28];
	R1 = R1 - R2 (NS);                               /* hp */
	R0 *= R1;
	R0 = R0 >>> 15 || R1 = [P1 + 36];                /* sub */
	R3 = R3 + R0 (NS);                               /* dbp += (fd * hp) >> 15 */
	R0 = R4 >> 19;
	P2 = R0;
	P2 = P5 + (P2 << 1);
	R0 = R5 + R5 (NS) || R2 = W[P2] (X);             /* sine(p2) */
	R2 = R2.L * R1.L (IS) || R4 = [P1 + 40];         /* 65535 */
	R2 = R2 >>> 15 || R1 = [P1 + 44];                /* -65535 */
	R0 = R0 + R2 (NS) || R2 = [P1 + 48];             /* y; gd */
	R0 = MIN(R0, R4);
	R0 = MAX(R0, R1) || R4 = [P0++];                 /* b */
	R2 *= R0;
	R2 = R2 >>> 15 || R1 = [I0++];                   /* the next r */
.Ld_ie:
	R4 = R2 + R4 (NS);
	R0 = [P3++];
.Ld_oe:
	[P1 + 48] = R0;                                  /* the next sub-block's gd (the last reads the spare) */
	[I1++] = R4;
	[P1 + 80] = R7;
	[P1 + 84] = R6;
	[P1 + 88] = R5;
	[P1 + 92] = R3;
	REST_L
	(R7:4, P5:3) = [SP++];
	RTS;

/* polyBLEP corrections: saw0 (rdt, rinv) back with R1 = saw0 >> 1 and R0 = A1; saw1 (fdt, finv) with R2 = saw1 and
 * R0 = fifth (R4 reloaded with p2) */
.Ld_b0:
	R0 = R7 >> 17;
	BITTGL (R0, 14);                                 /* t = (ph ^ 2^31) >> 17 */
	R1 = [P1 + 64];
	CC = R0 < R1;
	R2 = 0x8000 (Z);
	R1 = R0 - R2 (NS);
	IF !CC R0 = R1;                                  /* t, or t - 32768 at the top */
	R1 = [P1 + 68];
	R0 *= R1;
	R0 = R0 >>> 7;                                   /* x */
	R1 = R0.L * R0.L (IS);
	R1 = R1 >>> 15;
	R1 = R1 + R2 (NS);
	R2 = -R1;
	IF !CC R1 = R2;                                  /* +(m16(x, x) >> 15) + 32768, or minus it */
	R0 = R0 + R0 (NS);
	R2 = R7 >>> 16;
	R2 = R2 - R0 (NS);
	R1 = R2 + R1 (NS);                               /* the corrected saw */
	R1 = R1 >>> 1;
	R0 = [P1 + 16];
	JUMP .Ld_r0;
.Ld_b1:
	R0 = R6 >> 17;
	BITTGL (R0, 14);                                 /* t = (ph ^ 2^31) >> 17 */
	R2 = [P1 + 72];
	CC = R0 < R2;
	R4 = 0x8000 (Z);
	R2 = R0 - R4 (NS);
	IF !CC R0 = R2;                                  /* t, or t - 32768 at the top */
	R2 = [P1 + 76];
	R0 *= R2;
	R0 = R0 >>> 7;                                   /* x */
	R2 = R0.L * R0.L (IS);
	R2 = R2 >>> 15;
	R2 = R2 + R4 (NS);
	R4 = -R2;
	IF !CC R2 = R4;                                  /* +(m16(x, x) >> 15) + 32768, or minus it */
	R0 = R0 + R0 (NS);
	R4 = R6 >>> 16;
	R4 = R4 - R0 (NS);
	R2 = R4 + R2 (NS);                               /* the corrected saw */
	R0 = [P1 + 24];
	R4 = [I2];
	JUMP .Ld_r1;
	.size _hs_drone, .-_hs_drone

/* ---- hs_buzz ----
 * R7 tph, R6 rng, R5 slap, R3 rbp, R0 / R1 / R2 / R4 scratch; tlp, rlp and rattle live in k. Top of a sample: R1
 * this sample's ri, R2 tsh, R4 the previous result (stored by the first packet). P1 = k, P0 reads b, P3 walks the
 * sub-block table; I0 reads ri, I1 writes b (circular), I2 = &k[11] (rlp), I3 = &k[10] (tlp). */
	.align 4
	.global _hs_buzz
	.type _hs_buzz, STT_FUNC
_hs_buzz:
	[--SP] = (R7:4, P5:3);
	SAVE_L
	P1 = R2;
	P0 = R0;
	I0 = R1;
	B1 = R0;
	R1 = 252;
	R1 = R0 + R1;
	I1 = R1;
	R1 = 256;
	L1 = R1;
	R1 = 0;
	L0 = R1;
	L2 = R1;
	L3 = R1;
	R1 = 44;
	R1 = R2 + R1;
	I2 = R1;
	R1 = 40;
	R1 = R2 + R1;
	I3 = R1;
	P3 = P1;
	P4 = 96;
	P3 = P3 + P4;
	R7 = [P1 + 76];
	R6 = [P1 + 80];
	R5 = [P1 + 84];
	R3 = [P1 + 88];
	R0 = [P3++];
	[P1 + 32] = R0;                                  /* tg, gz, amp of sub-block 0 */
	R0 = [P3++];
	[P1 + 36] = R0;
	R0 = [P3++];
	[P1 + 60] = R0;
	R4 = [I1];
	R1 = [I0++];
	R2 = [P1];
	P4 = 8;
	LSETUP (.Lz_ob, .Lz_oe) LC1 = P4;
.Lz_ob:
	LSETUP (.Lz_ib, .Lz_ie) LC0 = P4;
.Lz_ib:
	R1 = LSHIFT R1 BY R2.L || [I1++] = R4;          /* inc = r << tsh */
	R7 = R7 + R1 (NS) || R2 = [P1 + 16];             /* 32768 */
	CC = R7 < R1 (IU);
	IF CC JUMP .Lz_slap;
.Lz_s1:
	R1 = R7 >> 16 || R4 = [P1 + 40];                 /* tlp */
	R1 = R1 - R2 (NS) || R0 = [P1 + 20];             /* tlc */
	R1 = R1 - R4 (NS) || R2 = [P1 + 32];             /* tg */
	R1 = R1 >>> 1;
	R1 *= R0;
	R1 = R1 >>> 14 || R0 = [P1 + 4];
	R4 = R4 + R1 (NS);                               /* tlp */
	R6 = R6 + R0 (NS) || R1 = [P1 + 8] || [I3] = R4;
	R6 *= R1;                                        /* rng = (rng + c) * 1664525 */
	R2 *= R4;
	R2 = R2 >>> 15 || R0 = [P1 + 12];                /* (tlp * tg) >> 15; -16384 */
	R1 = R6 >> 17 || R4 = [P1 + 48];                 /* rattle */
	R1 = R1 + R0 (NS);                               /* nz */
	R1 = R1.L * R4.L (IS);
	R1 = R1 >>> 14;
	R1 = R1 + R5 (NS);                               /* exc */
	R0 = R4 >>> 4;
	R4 = R4 - R0 (NS);
	R0 = R5 >>> 2;
	R5 = R5 - R0 (NS) || [P1 + 48] = R4;
	R0 = R1 >>> 1 || R4 = [P1 + 24];                 /* fr */
	R2 = R2 + R0 (NS);
	R1 = R1 >>> 2;
	R4 *= R3;
	R4 = R4 >>> 15 || R0 = [I2];                     /* rlp */
	R0 = R0 + R4 (NS);
	R1 = R1 - R0 (NS) || R4 = [P1 + 28];             /* rq */
	R4 *= R3;
	R4 = R4 >>> 15 || [I2] = R0;
	R1 = R1 - R4 (NS) || R0 = [P1 + 24];             /* hp */
	R0 *= R1;
	R0 = R0 >>> 15 || R4 = [P1 + 28];
	R3 = R3 + R0 (NS) || R1 = [P1 + 52];             /* rbp; 65535 */
	R4 *= R3;
	R4 = R4 >>> 13 || R0 = [P1 + 36];                /* ring; gz */
	R2 = R2 + R4 (NS);                               /* y */
	R2 = MIN(R2, R1) || R4 = [P1 + 56];
	R2 = MAX(R2, R4) || R1 = [I0++];                 /* the next r */
	R0 *= R2;
	R0 = R0 >>> 15 || R4 = [P0++];
.Lz_ie:
	R4 = R0 + R4 (NS) || R2 = [P1];                  /* tsh */
	R0 = [P3++];
	[P1 + 32] = R0;
	R0 = [P3++];
	[P1 + 36] = R0;
	R0 = [P3++];
.Lz_oe:
	[P1 + 60] = R0;                                  /* the next sub-block's (the last reads the spare) */
	[I1++] = R4;
	[P1 + 76] = R7;
	[P1 + 80] = R6;
	[P1 + 84] = R5;
	[P1 + 88] = R3;
	REST_L
	(R7:4, P5:3) = [SP++];
	RTS;

/* a new trompette cycle: slap = m16(amp, 32767 - (m16(rng >> 17, jit) >> 15)) >> 15 after one LCG step,
 * rattle = m16(slap, bnz) >> 15. Uses R0 / R1 / R4; R2 (32768) stays */
.Lz_slap:
	R0 = [P1 + 4];
	R1 = [P1 + 8];
	R6 = R6 + R0 (NS);
	R6 *= R1;
	R0 = R6 >> 17;
	R1 = [P1 + 64];                                  /* jit */
	R0 = R0.L * R1.L (IS);
	R0 = R0 >>> 15;
	R1 = [P1 + 72];                                  /* 32767 */
	R1 = R1 - R0 (NS);
	R0 = [P1 + 60];                                  /* amp */
	R0 = R0.L * R1.L (IS);
	R4 = [P1 + 68];                                  /* bnz */
	R5 = R0 >>> 15;
	R4 = R4.L * R5.L (IS);
	R4 = R4 >>> 15;
	[P1 + 48] = R4;
	JUMP .Lz_s1;
	.size _hs_buzz, .-_hs_buzz
#endif
