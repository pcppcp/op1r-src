/* kennel: Hollow Saint's shared-stage loops (HS_SHARED_ROOM, HS_SHARED_RELIC, HS_CAT11K), hand-scheduled for the
 * Blackfin: the same arithmetic as the C loops of body_voice, bus_voice and bus_stage in hollow_saint.c (kept there
 * for host builds and as the reference, HS_C_REF), bit for bit. The C functions stay the callers: they compute the
 * per-block and per-sub-block values into a table k (word layouts below), call the kernel and take the state back.
 *
 * void hs_body(int32_t *hb, int32_t *k, const int32_t *bl)
 *   body_voice's HS_CAT11K loop (32 pairs): the body combs on the pair's mean / 8, the body mixed into hb, the room
 *   input (+ bells / 16 when bl != 0) x w into racc. k: 0 cb1, 1 p1 (in/out), 2 cb2, 3 p2 (in/out), 4 bp (in/out),
 *   5 w, 6 racc, 7..22 { gdry, gbw << 16 | bfb (16 bits each) } per sub-block, 23..24 spare. The combs are circular
 *   I2 / I3 (L 90 / 114 bytes). 28 packets per pair (25 without bells).
 * void hs_share(int32_t *hb, const int32_t *k, int taping)
 *   bus_voice's loop: k: 0 BUS.inv, 1 BUS.out, 2 BUS.acc, 3 OP1_VOICE_ENV, 4..11 BUS.gd. 8 packets per sample, 13
 *   taping.
 * void hs_tape_in(int32_t *acc, int32_t *rb, int32_t gin)
 *   rb[m] = clamp((clamp(acc[m], +-32767) * gin) >> 12, +-65535) and acc[m] = 0. 6 packets per sample.
 * void hs_tape_out(const int32_t *rb, int32_t *o, const int32_t *k)
 *   o[m] = (((clamp(rb[m], +-32767) * k[0]) >> 15) * mix) >> 15, k[1..8] = mix per sub-block. 6 per sample.
 * void hs_room(int32_t *rx, int32_t *k)
 *   bus_stage's diffusers and room (32 samples at 11.025 kHz): k: 0 ca1, 1 pa1, 2 ca2, 3 pa2, 4 cr, 5 pr - 1 (the
 *   comb one entry behind, circularly: its writes run a sample behind; out: pr), 6 rdl (in/out), 7 rfb, 8 rdamp,
 *   9 room (r per sample into room[1..32]), 10 8036, 11 -8036. rx is cleared. 28 packets per sample.
 * void hs_room_out(int32_t *o, const int32_t *k)
 *   the room into o, darkened: k: 0 room (room[0] = the last block's last r, room[1..32] this block's), 1 spare,
 *   2 dark (in/out), 3 dcoef, 4..11 grw per sub-block, 12 32767, 13 -32767, 14 spare. 22 packets per pair.
 *
 * m16 products are 16 x 16 (.L / .H halves, (IS): the C's int16 casts); every other product is a 32-bit multiply
 * (it wraps as the C's int32 product). int16 stores of sat16 are a saturating << 16 and the high half. Stores that
 * run a sample behind go through a circular I1 over the 64-word buffer that starts at word 63 (the first pass writes
 * word 63's own value back; the last result goes there after the loop), or (the room comb) start one entry behind
 * with that entry's own value. No packet stores a register another op of it writes or loads one another reads; no
 * shift beside a P-register store (anomaly 05000074). Reads go up to two words past a buffer's end (not used).
 * L0..L3 are saved and restored, M1 is not touched. Host builds assemble this file empty.
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

	.align 4
	.global _hs_body
	.type _hs_body, STT_FUNC
_hs_body:
	[--SP] = (R7:4, P5:3);
	SAVE_L
	P5 = R1;
	P0 = R0;
	P1 = R0;
	P3 = R2;
	B1 = R0;
	R1 = 252;
	R1 = R0 + R1;
	I1 = R1;
	R1 = 256;
	L1 = R1;
	R1 = [P5];
	B2 = R1;
	R1 = [P5 + 4];
	I2 = R1;
	R1 = 90;
	L2 = R1;
	R1 = [P5 + 8];
	B3 = R1;
	R1 = [P5 + 12];
	I3 = R1;
	R1 = 114;
	L3 = R1;
	R1 = [P5 + 24];
	I0 = R1;
	P2 = R1;
	R1 = 0;
	L0 = R1;
	R3 = [P5 + 16];
	R6 = [P5 + 20];
	P4 = 28;
	P4 = P5 + P4;
	R5 = [P4++];
	R7 = [P4++];
	R1 = [I1];
	R0 = [P0++];
	R4 = [P0++];
	CC = R2 == 0;
	R2 = 8;
	LC1 = R2;
	IF CC JUMP .Lbo_n;
	LSETUP (.Lbo_bob, .Lbo_boe) LC1;
.Lbo_bob:
	R2 = 2;
	LC0 = R2;
	LSETUP (.Lbo_bib, .Lbo_bie) LC0;
.Lbo_bib:
	R2 = R0 + R4 (NS) || [I1++] = R1;
	R2 = R2 >>> 4 || R0.L = W[I2] || R1.L = W[I3];      /* x = (hb[m] + hb[m + 1]) >> 4; *p1, *p2 */
	R0 = R7.L * R0.L (IS);
	R0 = R0 >>> 15;
	R1 = R7.L * R1.L (IS);
	R1 = R1 >>> 15;
	R0 = R2 + R0 (NS);                               /* y1 */
	R1 = R2 - R1 (NS) || W[I2++] = R0.L;             /* y2 */
	R2 = R0 + R1 (NS) || W[I3++] = R1.L;
	R4 = R2 >>> 1;                                   /* b1 (the next bp) */
	R0 = R3 + R4 (NS) || R2 = [P3++];            /* bl[q] */
	R0 = R0 >>> 1;                                   /* b0 */
	R1 = R4 >>> 2;                                   /* rx */
	R2 = R2 >>> 4;
	R1 = R1 + R2 (NS);
	R1 *= R6;
	R1 = R1 >>> 13 || R2 = [P2++];
	R2 = R2 + R1 (NS) || R3 = [P1++];                /* racc[q] += (rx * w) >> 13; hb[m] */
	R0 = R7.H * R0.L (IS) || [I0++] = R2;
	R0 = R0 >>> 12;
	R3 *= R5;
	R3 = R3 >>> 15 || R1 = [P1++];
	R0 = R0 + R3 (NS);                               /* hb[m] x gdry + m16(b0, gbw) >> 12 */
	R2 = R7.H * R4.L (IS) || [I1++] = R0;
	R2 = R2 >>> 12 || R0 = [P0++];
	R1 *= R5;
	R1 = R1 >>> 15 || R3 = [P0++];
	R1 = R2 + R1 (NS);
	R2 = R0 + R3 (NS) || [I1++] = R1;
	R2 = R2 >>> 4 || R0.L = W[I2] || R1.L = W[I3];      /* x = (hb[m] + hb[m + 1]) >> 4; *p1, *p2 */
	R0 = R7.L * R0.L (IS);
	R0 = R0 >>> 15;
	R1 = R7.L * R1.L (IS);
	R1 = R1 >>> 15;
	R0 = R2 + R0 (NS);                               /* y1 */
	R1 = R2 - R1 (NS) || W[I2++] = R0.L;             /* y2 */
	R2 = R0 + R1 (NS) || W[I3++] = R1.L;
	R3 = R2 >>> 1;                                   /* b1 (the next bp) */
	R0 = R4 + R3 (NS) || R2 = [P3++];            /* bl[q] */
	R0 = R0 >>> 1;                                   /* b0 */
	R1 = R3 >>> 2;                                   /* rx */
	R2 = R2 >>> 4;
	R1 = R1 + R2 (NS);
	R1 *= R6;
	R1 = R1 >>> 13 || R2 = [P2++];
	R2 = R2 + R1 (NS) || R4 = [P1++];                /* racc[q] += (rx * w) >> 13; hb[m] */
	R0 = R7.H * R0.L (IS) || [I0++] = R2;
	R0 = R0 >>> 12;
	R4 *= R5;
	R4 = R4 >>> 15 || R1 = [P1++];
	R0 = R0 + R4 (NS);                               /* hb[m] x gdry + m16(b0, gbw) >> 12 */
	R2 = R7.H * R3.L (IS) || [I1++] = R0;
	R2 = R2 >>> 12 || R0 = [P0++];
	R1 *= R5;
	R1 = R1 >>> 15 || R4 = [P0++];
.Lbo_bie:
	R1 = R2 + R1 (NS);
	R5 = [P4++];
.Lbo_boe:
	R7 = [P4++];                                     /* the next sub-block's (the last reads the spare) */
	JUMP .Lbo_done;
.Lbo_n:
	LSETUP (.Lbo_nob, .Lbo_noe) LC1;
.Lbo_nob:
	R2 = 2;
	LC0 = R2;
	LSETUP (.Lbo_nib, .Lbo_nie) LC0;
.Lbo_nib:
	R2 = R0 + R4 (NS) || [I1++] = R1;
	R2 = R2 >>> 4 || R0.L = W[I2] || R1.L = W[I3];      /* x = (hb[m] + hb[m + 1]) >> 4; *p1, *p2 */
	R0 = R7.L * R0.L (IS);
	R0 = R0 >>> 15;
	R1 = R7.L * R1.L (IS);
	R1 = R1 >>> 15;
	R0 = R2 + R0 (NS);                               /* y1 */
	R1 = R2 - R1 (NS) || W[I2++] = R0.L;             /* y2 */
	R2 = R0 + R1 (NS) || W[I3++] = R1.L;
	R4 = R2 >>> 1;                                   /* b1 (the next bp) */
	R0 = R3 + R4 (NS);
	R0 = R0 >>> 1;                                   /* b0 */
	R1 = R4 >>> 2;                                   /* rx */
	R1 *= R6;
	R1 = R1 >>> 13 || R2 = [P2++];
	R2 = R2 + R1 (NS) || R3 = [P1++];                /* racc[q] += (rx * w) >> 13; hb[m] */
	R0 = R7.H * R0.L (IS) || [I0++] = R2;
	R0 = R0 >>> 12;
	R3 *= R5;
	R3 = R3 >>> 15 || R1 = [P1++];
	R0 = R0 + R3 (NS);                               /* hb[m] x gdry + m16(b0, gbw) >> 12 */
	R2 = R7.H * R4.L (IS) || [I1++] = R0;
	R2 = R2 >>> 12 || R0 = [P0++];
	R1 *= R5;
	R1 = R1 >>> 15 || R3 = [P0++];
	R1 = R2 + R1 (NS);
	R2 = R0 + R3 (NS) || [I1++] = R1;
	R2 = R2 >>> 4 || R0.L = W[I2] || R1.L = W[I3];      /* x = (hb[m] + hb[m + 1]) >> 4; *p1, *p2 */
	R0 = R7.L * R0.L (IS);
	R0 = R0 >>> 15;
	R1 = R7.L * R1.L (IS);
	R1 = R1 >>> 15;
	R0 = R2 + R0 (NS);                               /* y1 */
	R1 = R2 - R1 (NS) || W[I2++] = R0.L;             /* y2 */
	R2 = R0 + R1 (NS) || W[I3++] = R1.L;
	R3 = R2 >>> 1;                                   /* b1 (the next bp) */
	R0 = R4 + R3 (NS);
	R0 = R0 >>> 1;                                   /* b0 */
	R1 = R3 >>> 2;                                   /* rx */
	R1 *= R6;
	R1 = R1 >>> 13 || R2 = [P2++];
	R2 = R2 + R1 (NS) || R4 = [P1++];                /* racc[q] += (rx * w) >> 13; hb[m] */
	R0 = R7.H * R0.L (IS) || [I0++] = R2;
	R0 = R0 >>> 12;
	R4 *= R5;
	R4 = R4 >>> 15 || R1 = [P1++];
	R0 = R0 + R4 (NS);                               /* hb[m] x gdry + m16(b0, gbw) >> 12 */
	R2 = R7.H * R3.L (IS) || [I1++] = R0;
	R2 = R2 >>> 12 || R0 = [P0++];
	R1 *= R5;
	R1 = R1 >>> 15 || R4 = [P0++];
.Lbo_nie:
	R1 = R2 + R1 (NS);
	R5 = [P4++];
.Lbo_noe:
	R7 = [P4++];                                     /* the next sub-block's (the last reads the spare) */
.Lbo_done:
	[I1++] = R1;
	R0 = I2;
	[P5 + 4] = R0;
	R0 = I3;
	[P5 + 12] = R0;
	[P5 + 16] = R3;
	REST_L
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _hs_body, .-_hs_body

	.align 4
	.global _hs_share
	.type _hs_share, STT_FUNC
_hs_share:
	[--SP] = (R7:4, P5:3);
	SAVE_L
	P5 = R1;
	I0 = R0;
	P3 = R0;
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
	R7 = [P5];                                       /* inv */
	P1 = [P5 + 4];                                   /* out */
	R1 = [P5 + 8];                                   /* acc */
	P2 = R1;
	I2 = R1;
	P0 = [P5 + 12];                                  /* env */
	P4 = 4;
	R6 = 26000 (Z);
	R5 = -R6;
	R4 = 0;
	R1 = [I1];
	R0 = W[P0 ++ P4] (X);
	CC = R2 == 0;
	IF CC JUMP .Lsh_plain;
/* taping: R3 = gd of the sub-block; P3 reads hb again */
	P5 += 16;
	R3 = [P5++];
	R2 = 8;
	LC1 = R2;
	LSETUP (.Lsh_tob, .Lsh_toe) LC1;
.Lsh_tob:
	R2 = 8;
	LC0 = R2;
	LSETUP (.Lsh_tib, .Lsh_tie) LC0;
.Lsh_tib:
	R0 = MAX(R0, R4) || [I1++] = R1 || R2 = [I0++];   /* e; y */
	R2 *= R0;
	R2 = R2 >>> 15 || R1 = [P2++];
	R1 = R1 + R2 (NS) || R2 = [P3++];                /* acc[m] += (y * e) >> 15 */
	R2 *= R3;
	R2 = R2 >>> 15 || [I2++] = R1;                   /* y = (y * gd) >> 15 */
	R0 *= R7;
	R0 = R0 >>> 15 || R1 = [P1++];                   /* sh */
	R1 *= R0;
	R1 = R1 >>> 11 || R0 = W[P0 ++ P4] (X);
	R1 = R1 + R2 (NS);
	R1 = MIN(R1, R6);
.Lsh_tie:
	R1 = MAX(R1, R5);
.Lsh_toe:
	R3 = [P5++];
	JUMP .Lsh_done;
.Lsh_plain:
	P3 = 64;
	LSETUP (.Lsh_pb, .Lsh_pe) LC0 = P3;
.Lsh_pb:
	R0 = MAX(R0, R4) || [I1++] = R1 || R2 = [P1++];   /* e; out */
	R0 *= R7;
	R0 = R0 >>> 15 || R1 = [I0++];                   /* sh; y */
	R2 *= R0;
	R2 = R2 >>> 11 || R0 = W[P0 ++ P4] (X);
	R1 = R1 + R2 (NS);
	R1 = MIN(R1, R6);
.Lsh_pe:
	R1 = MAX(R1, R5);
.Lsh_done:
	[I1++] = R1;
	REST_L
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _hs_share, .-_hs_share

	.align 4
	.global _hs_tape_in
	.type _hs_tape_in, STT_FUNC
_hs_tape_in:
	[--SP] = (R7:4, P5:3);
	SAVE_L
	P0 = R0;
	I0 = R0;
	B1 = R1;
	R3 = 252;
	R3 = R1 + R3;
	I1 = R3;
	R3 = 256;
	L1 = R3;
	R3 = R2;
	R2 = 0;
	L0 = R2;
	L2 = R2;
	L3 = R2;
	R7 = 32767 (Z);
	R6 = -R7;
	R5 = 65535 (Z);
	R4 = -R5;
	R1 = [I1];
	R0 = [P0++];
	P1 = 64;
	LSETUP (.Lti_b, .Lti_e) LC0 = P1;
.Lti_b:
	R0 = MIN(R0, R7) || [I0++] = R2;                 /* acc[m] = 0 */
	R0 = MAX(R0, R6) || [I1++] = R1;
	R0 *= R3;
	R1 = R0 >>> 12;
	R1 = MIN(R1, R5) || R0 = [P0++];
.Lti_e:
	R1 = MAX(R1, R4);
	[I1++] = R1;
	REST_L
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _hs_tape_in, .-_hs_tape_in

	.align 4
	.global _hs_tape_out
	.type _hs_tape_out, STT_FUNC
_hs_tape_out:
	[--SP] = (R7:4, P5:3);
	SAVE_L
	I0 = R0;
	B1 = R1;
	R3 = 252;
	R3 = R1 + R3;
	I1 = R3;
	R3 = 256;
	L1 = R3;
	P0 = R2;
	R3 = 0;
	L0 = R3;
	L2 = R3;
	L3 = R3;
	R7 = 32767 (Z);
	R6 = -R7;
	R5 = [P0++];                                     /* nE >> 2 */
	R4 = [P0++];                                     /* mix, sub-block 0 */
	R1 = [I1];
	R0 = [I0++];
	P1 = 8;
	LSETUP (.Lto_ob, .Lto_oe) LC1 = P1;
.Lto_ob:
	LSETUP (.Lto_ib, .Lto_ie) LC0 = P1;
.Lto_ib:
	R0 = MIN(R0, R7) || [I1++] = R1;
	R0 = MAX(R0, R6);
	R0 *= R5;
	R1 = R0 >>> 15;
	R1 *= R4;
.Lto_ie:
	R1 = R1 >>> 15 || R0 = [I0++];
.Lto_oe:
	R4 = [P0++];                                     /* the last reads k[9] (not used) */
	[I1++] = R1;
	REST_L
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _hs_tape_out, .-_hs_tape_out

	.align 4
	.global _hs_room
	.type _hs_room, STT_FUNC
_hs_room:
	[--SP] = (R7:4, P5:3);
	SAVE_L
	P5 = R1;
	P0 = R0;
	I0 = R0;
	R1 = [P5];
	B1 = R1;
	R1 = [P5 + 4];
	I1 = R1;
	R1 = 156;
	L1 = R1;
	R1 = [P5 + 8];
	B2 = R1;
	R1 = [P5 + 12];
	I2 = R1;
	R1 = 240;
	L2 = R1;
	R1 = [P5 + 16];
	B3 = R1;
	R1 = [P5 + 20];
	I3 = R1;
	R1 = 1032;
	L3 = R1;
	R1 = 0;
	L0 = R1;
	R7 = 19661 (Z);                                  /* AP_G */
	R6 = [P5 + 24];                                  /* rdl */
	R5 = [P5 + 40];                                  /* 8036 */
	R4 = 0;
	P1 = [P5 + 36];
	P1 += 4;                                         /* room[1..] */
	R0.H = W[I3];                                    /* the entry behind: written back unchanged */
	R2 = [P0++];
	R1.H = W[I1];
	R3 = [P5 + 44];
	P2 = 32;
	LSETUP (.Lrm_b, .Lrm_e) LC0 = P2;
.Lrm_b:
	R2 = MIN(R2, R5) || W[I3++] = R0.H;
	R2 = MAX(R2, R3) || [I0++] = R4;                 /* x; rx[q] = 0 */
	R1 = R1 >>> 16;                                  /* a = *pa1 */
	R3 = R7.L * R1.L (IS);
	R3 = R3 >>> 15 || R0.H = W[I2];
	R2 = R2 - R3 (NS);                               /* w = x - (m16(AP_G, a) >> 15) */
	R3 = R7.L * R2.L (IS);
	R3 = R3 >>> 15;
	R1 = R1 + R3 (NS);                               /* z = a + (m16(AP_G, w) >> 15) */
	R2 = R2 << 16 (S);
	R0 = R0 >>> 16 || W[I1++] = R2.H;                /* *pa1++ = sat16(w); a = *pa2 */
	R3 = R7.L * R0.L (IS);
	R3 = R3 >>> 15;
	R1 = R1 - R3 (NS);                               /* w */
	R3 = R1 << 16 (S);
	R1 *= R7;
	R1 = R1 >>> 15 || W[I2++] = R3.H;                /* *pa2++ = sat16(w) */
	R0 = R0 + R1 (NS) || R2.H = W[I3];               /* z = a + ((AP_G * w) >> 15); r = *pr */
	R1 = R2 >>> 16 || R3 = [P5 + 32];                /* rdamp */
	R2 = R1 - R6 (NS) || [P1++] = R1;                /* room[q + 1] = r */
	R2 = R2 >>> 1;
	R2 *= R3;
	R2 = R2 >>> 14 || R3 = [P5 + 28];                /* rfb */
	R6 = R6 + R2 (NS);                               /* rdl */
	R3 *= R6;
	R3 = R3 >>> 15 || R1.H = W[I1] || R2 = [P0++];   /* the next a, x */
	R0 = R0 + R3 (NS);
.Lrm_e:
	R0 = R0 << 16 (S) || R3 = [P5 + 44];             /* sat16(z + ((rfb * rdl) >> 15)), stored a sample behind; -8036 */
	W[I3++] = R0.H;
	R0 = I1;
	[P5 + 4] = R0;
	R0 = I2;
	[P5 + 12] = R0;
	R0 = I3;
	[P5 + 20] = R0;
	[P5 + 24] = R6;
	REST_L
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _hs_room, .-_hs_room

	.align 4
	.global _hs_room_out
	.type _hs_room_out, STT_FUNC
_hs_room_out:
	[--SP] = (R7:4, P5:3);
	SAVE_L
	P5 = R1;
	I0 = R0;
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
	P0 = [P5];
	P1 = P0;
	P1 += 4;
	R3 = [P5 + 8];                                   /* dark */
	R5 = [P5 + 12];                                  /* dcoef */
	R7 = [P5 + 48];
	R6 = [P5 + 52];
	P2 = 16;
	P2 = P5 + P2;
	R4 = [P2++];                                     /* grw, sub-block 0 */
	R2 = [I1];
	R0 = [P0++];
	R1 = [P1++];
	P3 = 8;
	P4 = 4;
	LSETUP (.Lro_ob, .Lro_oe) LC1 = P3;
.Lro_ob:
	LSETUP (.Lro_ib, .Lro_ie) LC0 = P4;
.Lro_ib:
	R0 = R0 + R1 (NS) || [I1++] = R2;
	R0 = R0 >>> 1;                                   /* r0 = (rp + r1) >> 1 */
	R0 = R0.L * R4.L (IS);
	R0 = R0 >>> 13;                                  /* y */
	R0 = R0 - R3 (NS);
	R0 = R0 >>> 1;
	R0 *= R5;
	R0 = R0 >>> 14 || R2 = [I0++];
	R3 = R3 + R0 (NS);                               /* dark */
	R2 = R2 + R3 (NS);
	R2 = MIN(R2, R7);
	R2 = MAX(R2, R6);
	R1 = R1.L * R4.L (IS) || [I1++] = R2;
	R1 = R1 >>> 13 || R2 = [I0++];
	R1 = R1 - R3 (NS);
	R1 = R1 >>> 1;
	R1 *= R5;
	R1 = R1 >>> 14 || R0 = [P0++];
	R3 = R3 + R1 (NS);
	R2 = R2 + R3 (NS) || R1 = [P1++];
	R2 = MIN(R2, R7);
.Lro_ie:
	R2 = MAX(R2, R6);
.Lro_oe:
	R4 = [P2++];                                     /* the last reads k[12] (not used) */
	[I1++] = R2;
	[P5 + 8] = R3;
	REST_L
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _hs_room_out, .-_hs_room_out

/* ---- hs_bells(int32_t *w, int32_t *k) ----
 * bell_voice's (and section 3's) partial loops: w[q] = the sum over the partials of m16(sine(ph += bi), e) >> 15,
 * 32 samples. k: 0 OP1_SINE_TABLE, 1 n (1..4), then per partial { ph (in/out), bi, e }. The first partial stores (the
 * C's cleared sum plus its term), the others add. Two samples per pass, their table reads a pass ahead: 6 packets
 * per sample for the first partial, 7 for the others. R4 ph, R5 bi, R6 e; P0 / P1 the two table entries, P5 the
 * table, P4 the partial; I0 reads w, I1 writes it, B0 holds it. */
	.align 4
	.global _hs_bells
	.type _hs_bells, STT_FUNC
_hs_bells:
	[--SP] = (R7:4, P5:3);
	SAVE_L
	P4 = R1;
	B0 = R0;                                         /* w */
	R1 = 0;
	L0 = R1;
	L1 = R1;
	P5 = [P4++];
	P3 = [P4++];                                     /* n */
	P2 = 16;
	R3 = B0;
	I0 = R3;
	I1 = R3;
	R4 = [P4];
	R5 = [P4 + 4];
	R6 = [P4 + 8];
	R4 = R4 + R5 (NS);
	R0 = R4 >> 19;
	R4 = R4 + R5 (NS);
	R1 = R4 >> 19;
	P0 = R0;
	P1 = R1;
	P0 = P5 + (P0 << 1);
	P1 = P5 + (P1 << 1);
	LSETUP (.Lbl_sb, .Lbl_se) LC0 = P2;
.Lbl_sb:
	R4 = R4 + R5 (NS) || R2 = W[P0] (X);
	R0 = R4 >> 19 || R3 = W[P1] (X);
	R2 = R2.L * R6.L (IS);
	R3 = R3.L * R6.L (IS);
	R2 = R2 >>> 15;
	R3 = R3 >>> 15 || [I1++] = R2;
	R4 = R4 + R5 (NS) || [I1++] = R3;
	R1 = R4 >> 19;
	P0 = R0;
	P1 = R1;
	P0 = P5 + (P0 << 1);
.Lbl_se:
	P1 = P5 + (P1 << 1);
.Lbl_next:
	R4 = R4 - R5 (NS);
	R4 = R4 - R5 (NS);                               /* undo the pass ahead */
	[P4] = R4;
	P4 += 12;
	P3 += -1;
	CC = P3 == 0;
	IF CC JUMP .Lbl_done;
	R3 = B0;
	I0 = R3;
	I1 = R3;
	R4 = [P4];
	R5 = [P4 + 4];
	R6 = [P4 + 8];
	R4 = R4 + R5 (NS);
	R0 = R4 >> 19;
	R4 = R4 + R5 (NS);
	R1 = R4 >> 19;
	P0 = R0;
	P1 = R1;
	P0 = P5 + (P0 << 1);
	P1 = P5 + (P1 << 1);
	LSETUP (.Lbl_ab, .Lbl_ae) LC0 = P2;
.Lbl_ab:
	R4 = R4 + R5 (NS) || R2 = W[P0] (X) || R7 = [I0++];    /* sine of sample 2i; w[2i] */
	R0 = R4 >> 19 || R3 = W[P1] (X) || R1 = [I0++];       /* sine of 2i + 1; w[2i + 1] */
	R2 = R2.L * R6.L (IS);
	R3 = R3.L * R6.L (IS);
	R2 = R2 >>> 15;
	R3 = R3 >>> 15;
	R2 = R2 + R7 (NS);
	R3 = R3 + R1 (NS) || [I1++] = R2;
	R4 = R4 + R5 (NS) || [I1++] = R3;
	R1 = R4 >> 19;
	P0 = R0;
	P1 = R1;
	P0 = P5 + (P0 << 1);
.Lbl_ae:
	P1 = P5 + (P1 << 1);
	JUMP .Lbl_next;
.Lbl_done:
	REST_L
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _hs_bells, .-_hs_bells
#endif
