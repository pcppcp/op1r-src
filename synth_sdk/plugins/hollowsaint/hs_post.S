/* kennel: Hollow Saint's per-voice post-processing loops, hand-scheduled for the Blackfin: the same arithmetic as the
 * C loops in hollow_saint.c (kept there for host builds and as the reference, HS_C_REF), bit for bit.
 *
 * void hs_bound(int32_t *hb)
 *   hb[m] = clamp(hb[m], -65535, 65535) for 64 samples (the voice, bounded once before the relic and the cathedral).
 *   2 packets per sample.
 * void hs_dark(int32_t *hb, int32_t *st, const int32_t *k)
 *   section 4 in place on hb: y = clamp(hb, +-65535), the darkening low-pass (k[0] != 0) or none (dark = y), the DC
 *   blocker, the gain and the clamp to +-26000. st = { dark, dc8 } (updated); k = { kd, dcoef, go for the 8
 *   sub-blocks, one spare word (read after the last sub-block, not used) }. 17 packets per sample darkening, 12
 *   transparent.
 * void hs_up(const int32_t *hb, int16_t *out, int32_t *up)
 *   section 5: the 2x half-band interpolation into out (128 samples, as 16-bit stores: nothing establishes that R246's
 *   out is 4-aligned); up = the last three 22.05 kHz samples (updated). Exact for any int32 in hb (32-bit products,
 *   the int16 cast as a low-half store), so a step that rewrites hb between hs_dark and hs_up keeps it exact.
 *   5 packets per sample.
 *
 * Products are 32-bit multiplies (they wrap as the C's int32 products do). No packet stores a register another op of
 * it writes or loads one another reads; no shift beside a P-register store (anomaly 05000074; no P-register stores
 * in the loops at all). No store outside hb[0..63] / out[0..127] / up / st: stores run a sample behind with the loops
 * rotated so the first one lands at index 0; reads go up to two words past hb's end (not used). L0 / L1 are saved,
 * zeroed (linear addressing) and restored; L2 / L3 and M1 are not touched.
 * Host builds assemble this file empty: hollow_saint.c keeps the C loops there.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

/* ---- hs_bound: R2 65535, R3 -65535; even samples via R4 -> R0, odd via R5 -> R1; stores a sample behind ---- */
	.align 4
	.global _hs_bound
	.type _hs_bound, STT_FUNC
_hs_bound:
	[--SP] = (R7:4);
	[--SP] = L0;
	[--SP] = L1;
	I0 = R0;
	I1 = R0;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	R2 = 65535 (Z);
	R3 = -R2;
	R4 = [I0++];
	R5 = [I0++];
	R0 = MIN(R4, R2);
	R0 = MAX(R0, R3);
	P0 = 32;
	LSETUP (.Lbd_b, .Lbd_e) LC0 = P0;
.Lbd_b:
	R1 = MIN(R5, R2) || [I1++] = R0 || R4 = [I0++];
	R1 = MAX(R1, R3);
	R0 = MIN(R4, R2) || [I1++] = R1 || R5 = [I0++];
.Lbd_e:
	R0 = MAX(R0, R3);
	L1 = [SP++];
	L0 = [SP++];
	(R7:4) = [SP++];
	RTS;
	.size _hs_bound, .-_hs_bound

/* ---- hs_dark ----
 * R7 y (clamped; loaded a sample ahead), R3 dark, R4 dc8, R5 dcoef, R6 go, R2 / R0 scratch and the result (stored a
 * sample behind through I1), R1 / R2 the clamp constants, loaded from the stack table at P1 just before use:
 * [P1] 65535, [P1 + 4] -65535, [P1 + 8] 26000, [P1 + 12] -26000. P2 walks k's go words (the outer loop). The inner
 * loops start at the low-pass of sample n and end with the next sample's input clamp (its first packet stores x(n)),
 * so the input clamp of sample 0 is done before the loop and the loops' last pass clamps hb[64], not used. */
	.align 4
	.global _hs_dark
	.type _hs_dark, STT_FUNC
_hs_dark:
	[--SP] = (R7:4, P5:3);
	[--SP] = L0;
	[--SP] = L1;
	SP += -16;
	P0 = R1;
	P2 = R2;
	P1 = SP;
	I0 = R0;
	I1 = R0;
	P3 = R0;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	R1 = 65535 (Z);
	R2 = -R1;
	[P1] = R1;
	[P1 + 4] = R2;
	R0 = 26000 (Z);
	[P1 + 8] = R0;
	R0 = -R0;
	[P1 + 12] = R0;
	R3 = [P0];
	R4 = [P0 + 4];
	R0 = [P2++];                                     /* kd */
	R5 = [P2++];                                     /* dcoef */
	R6 = [P2++];                                     /* go, sub-block 0 */
	R7 = [I0++];
	R7 = MIN(R7, R1);
	R7 = MAX(R7, R2);
	P4 = 8;
	CC = R0 == 0;
	IF CC JUMP .Ldk_flat;

	LSETUP (.Ldk_ob, .Ldk_oe) LC1 = P4;
.Ldk_ob:
	LSETUP (.Ldk_ib, .Ldk_ie) LC0 = P4;
.Ldk_ib:
	R2 = R7 - R3 (NS);                               /* dark += (((y - dark) >> 1) * dcoef) >> 14 */
	R2 = R2 >>> 1;
	R2 *= R5;
	R2 = R2 >>> 14;
	R3 = R3 + R2 (NS);
	R2 = R3 << 8;                                    /* dc8 += (y * 256 - dc8) >> 8, y = dark */
	R2 = R2 - R4 (NS);
	R2 = R2 >>> 8;
	R4 = R4 + R2 (NS);
	R2 = R4 >>> 8 || R1 = [P1 + 8];
	R0 = R3 - R2 (NS) || R7 = [I0++];                /* y -= dc8 >> 8 */
	R0 *= R6;
	R0 = R0 >>> 15 || R2 = [P1 + 12];
	R0 = MIN(R0, R1);
	R0 = MAX(R0, R2) || R1 = [P1];
	R7 = MIN(R7, R1) || [I1++] = R0 || R2 = [P1 + 4];   /* the next sample's input clamp */
.Ldk_ie:
	R7 = MAX(R7, R2);
.Ldk_oe:
	R6 = [P2++];
	JUMP .Ldk_done;

/* transparent: dark = y, so dark ends as the clamp of hb[63] (taken before the loop overwrites it) */
.Ldk_flat:
	R3 = [P3 + 252];
	R3 = MIN(R3, R1);
	R3 = MAX(R3, R2);
	LSETUP (.Ldf_ob, .Ldf_oe) LC1 = P4;
.Ldf_ob:
	LSETUP (.Ldf_ib, .Ldf_ie) LC0 = P4;
.Ldf_ib:
	R2 = R7 << 8;
	R2 = R2 - R4 (NS);
	R2 = R2 >>> 8;
	R4 = R4 + R2 (NS);
	R2 = R4 >>> 8 || R1 = [P1 + 8];
	R0 = R7 - R2 (NS);
	R0 *= R6;
	R0 = R0 >>> 15 || R2 = [P1 + 12] || R7 = [I0++];
	R0 = MIN(R0, R1);
	R0 = MAX(R0, R2) || R1 = [P1];
	R7 = MIN(R7, R1) || [I1++] = R0 || R2 = [P1 + 4];
.Ldf_ie:
	R7 = MAX(R7, R2);
.Ldf_oe:
	R6 = [P2++];

.Ldk_done:
	[P0] = R3;
	[P0 + 4] = R4;
	SP += 16;
	L1 = [SP++];
	L0 = [SP++];
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _hs_dark, .-_hs_dark

/* ---- hs_up ----
 * Sample k: a, b, c, d = x[k-3], x[k-2], x[k-1], x[k] (x[-3..-1] = up) in R0..R3, rotating: sample k's a sits in
 * R(k mod 4), and the packet after its last use loads x[k+1] into it (the next sample's d). The midpoint
 * (9 (b + c) - a - d) >> 4 goes to R4 (even k) / R6 (odd k) and is stored in the next sample's first packet, after
 * which comes b; R5 = 9. Sample 0 is peeled (no midpoint before it), samples 1..60 loop (4 per pass), 61..63 follow. */
.macro UPS a, b, c, d, t, tp, first, lbl
	.if \first
	\t = \b + \c (NS);
	.else
	\t = \b + \c (NS) || W[I1++] = \tp\().L;
	.endif
	\t *= R5;
	\t = \t - \a (NS) || W[I1++] = \b\().L;
	\t = \t - \d (NS) || \a = [I0++];
	.ifnb \lbl
\lbl:
	.endif
	\t = \t >>> 4;
.endm

	.align 4
	.global _hs_up
	.type _hs_up, STT_FUNC
_hs_up:
	[--SP] = (R7:4);
	[--SP] = L0;
	[--SP] = L1;
	P0 = R2;
	I0 = R0;
	I1 = R1;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	R5 = 9;
	R0 = [P0];
	R1 = [P0 + 4];
	R2 = [P0 + 8];
	R3 = [I0++];
	UPS R0, R1, R2, R3, R4, R6, 1
	P1 = 15;
	LSETUP (.Lup_b, .Lup_e) LC0 = P1;
.Lup_b:
	UPS R1, R2, R3, R0, R6, R4, 0
	UPS R2, R3, R0, R1, R4, R6, 0
	UPS R3, R0, R1, R2, R6, R4, 0
	UPS R0, R1, R2, R3, R4, R6, 0, .Lup_e
	UPS R1, R2, R3, R0, R6, R4, 0
	UPS R2, R3, R0, R1, R4, R6, 0
	UPS R3, R0, R1, R2, R6, R4, 0
	W[I1++] = R6.L;
	[P0] = R0;
	[P0 + 4] = R1;
	[P0 + 8] = R2;
	L1 = [SP++];
	L0 = [SP++];
	(R7:4) = [SP++];
	RTS;
	.size _hs_up, .-_hs_up
#endif
