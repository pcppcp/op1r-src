/* SID's block passes, hand-scheduled for the Blackfin: the same arithmetic as sid_dsp.c's render loops (and its host
 * build), bit for bit, one oscillator at a time over the 32-sample call, then the filter. The oscillator passes add
 * their samples into acc[0..n); the filter reads acc and clears it again for the next call.
 *
 * Phases are held as p << 4 (the 28-bit phase in bits 4..31): the add wraps by itself (the C's & MASK) and the
 * phase MSB is the sign bit. sid_pass (sid_dsp.c) is the oscillator's state and loop constants, 32-bit words:
 *   0 phase (in/out)  4 step  8 gain  12 mask (sync) / companion phase << 4 (ring)
 *   16 k1  20 k2  24 k3  28 k4  (per kind below)  32..63 scratch for the constants a pass cycles through (I2/L2)
 * Masks hold one bit per sample, sample 0 in bit 31.
 *
 * void sid_tri(int32_t *acc, sid_pass *p, unsigned n)      triangle, acc += SDIV(w - 32768, 3)
 * void sid_trig(int32_t *acc, sid_pass *p, unsigned n)     the same times the gain: acc += SDIV(y * gain, 8)
 *                                                          (gain 0 .. 255: the C's SDIV as a bias of 255 below zero)
 * (SDIV(x, k): x / 2^k rounded toward zero, as sid_dsp.c)
 * void sid_ring(int32_t *acc, sid_pass *p, unsigned n)     triangle of p ^ the companion's MSB (12: companion
 *                                                          phase << 4, read before it advances by k1 = its step << 4)
 * void sid_saw(int32_t *acc, sid_pass *p, unsigned n)      saw (k1 steep, k2 recip)
 * void sid_sawg(int32_t *acc, sid_pass *p, unsigned n)     the same times the gain
 * uint32_t sid_sync(int32_t *acc, sid_pass *p, unsigned n) saw restarted where the mask (12) has a bit; returns the
 *                                                          rises (MSB 0 -> 1) with sample i in bit n - 1 - i
 * void sid_pulse(int32_t *acc, sid_pass *p, unsigned n)    trapezoid pulse (k1 steep, k2 pw, k3 / k4 the peaks)
 * void sid_noise(int32_t *acc, sid_pass *p, unsigned n)    LFSR noise (k1 the LFSR, in/out; k2 its sample, gain
 *                                                          applied; k3 non-zero: clock every sample; k4 and
 *                                                          32: noise_low, noise_high)
 * void sid_noiseg(int32_t *acc, sid_pass *p, unsigned n)   the same with the gain
 * void sid_filter(int16_t *out, int32_t *acc, sid_filt *f, unsigned n)
 *   the 8580 lowpass, DC coupling and output clamp; f = { band, low, dc, cutoff, target, resonance, glide }
 *   (glide 0: the cutoff stays where it is for the call; the smoothing is the identity there)
 *
 * The block's store runs one sample behind (in the next sample's first packet); the first one writes back the word
 * in front of acc unchanged, so acc[-1] must exist. No packet stores a register another of its ops writes, or loads a
 * register another of its ops reads. L2 / B2 (and L3 / B3 in the filter) are 0 again on return.
 * Host builds (screen and golden harnesses) assemble it empty: sid_dsp.c keeps the C loops there.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifdef __bfin__
	.text

/* R0 acc, R1 pass, R2 n -> P5 pass, P4 n, I0 acc (reads), I1 acc - 1 (stores), \res = acc[-1] */
.macro OSC_ENTER res
	[--SP] = (R7:4, P5:3);
	P5 = R1;
	P4 = R2;
	R3 = 0;
	L0 = R3;
	L1 = R3;
	I0 = R0;
	R0 += -4;
	I1 = R0;
	P3 = R0;
	\res = [P3];
.endm

.macro OSC_LEAVE res
	[I1++] = \res;
	R0 = R0 >> 4;
	[P5] = R0;
	R3 = 0;
	L2 = R3;
	B2 = R3;
	(R7:4, P5:3) = [SP++];
	RTS;
.endm

/* ---- triangle: 11 packets a sample ---- */
	.align 4
	.global _sid_tri
	.type _sid_tri, STT_FUNC
_sid_tri:
	OSC_ENTER R4
	R0 = [P5];
	R0 <<= 4;
	R1 = [P5 + 4];
	R1 <<= 4;
	LSETUP (.Ltri_b, .Ltri_e) LC0 = P4;
.Ltri_b:
	R0 = R0 + R1 (NS) || R7 = [I0++] || [I1++] = R4;
	R5 = R0 >>> 31;
	R5 = R5 ^ R0;                        /* folded: bits 15..30 are the wave */
	R5 >>= 15;
	BITTGL (R5, 15);
	R5 = R5.L (X);                       /* wave - 32768 */
	R6 = R5 >>> 31;
	R6 >>= 29;
	R5 = R5 + R6;
	R5 >>>= 3;
.Ltri_e:
	R4 = R5 + R7;
	OSC_LEAVE R4
	.size _sid_tri, .-_sid_tri

/* ---- triangle with the gain: 13 packets ---- */
	.align 4
	.global _sid_trig
	.type _sid_trig, STT_FUNC
_sid_trig:
	OSC_ENTER R4
	R0 = [P5];
	R0 <<= 4;
	R1 = [P5 + 4];
	R1 <<= 4;
	R2 = [P5 + 8];                       /* gain (.L) */
	R3.H = 7;
	R3.L = -255;
	LSETUP (.Ltrig_b, .Ltrig_e) LC0 = P4;
.Ltrig_b:
	R0 = R0 + R1 (NS) || R7 = [I0++] || [I1++] = R4;
	R5 = R0 >>> 31;
	R5 = R5 ^ R0;
	R5 <<= 1;
	BITTGL (R5, 31);                     /* .H: wave - 32768 */
	R6 = R5 >>> 15 (V);                  /* .H: -1 below zero */
	R4 = R6 & R3;                        /* .H: 7 below zero */
	R5 = R5 +|+ R4;
	R5 = R5 >>> 3 (V);                   /* .H: y = SDIV(wave - 32768, 3) */
	A1 = R5.H * R2.L (IS);
	R5 = (A1 += R6.H * R3.L) (IS);       /* y * gain, + 255 when y < 0 (gain >= 0: then y * gain <= 0, and */
	R5 >>>= 8;                           /* y * gain + 255 >> 8 is SDIV(y * gain, 8)) */
.Ltrig_e:
	R4 = R5 + R7;
	OSC_LEAVE R4
	.size _sid_trig, .-_sid_trig

/* ---- ring: triangle of p ^ the companion's MSB, 13 packets ---- */
	.align 4
	.global _sid_ring
	.type _sid_ring, STT_FUNC
_sid_ring:
	OSC_ENTER R4
	R0 = [P5];
	R0 <<= 4;
	R1 = [P5 + 4];
	R1 <<= 4;
	R2 = [P5 + 12];                      /* companion phase << 4 */
	R3 = [P5 + 16];                      /* companion step << 4 */
	LSETUP (.Lring_b, .Lring_e) LC0 = P4;
.Lring_b:
	R0 = R0 + R1 (NS) || R7 = [I0++] || [I1++] = R4;
	R6 = R0 ^ R2;
	R2 = R2 + R3;
	R6 = R6 >>> 31;                      /* MSB of p ^ ring */
	R5 = R0 ^ R6;                        /* folded (bit 31 is dropped below) */
	R5 >>= 15;
	BITTGL (R5, 15);
	R5 = R5.L (X);
	R6 = R5 >>> 31;
	R6 >>= 29;
	R5 = R5 + R6;
	R5 >>>= 3;
.Lring_e:
	R4 = R5 + R7;
	OSC_LEAVE R4
	.size _sid_ring, .-_sid_ring

/* ---- saw: 18 packets (21 with the gain). I2 cycles -65536, 65535, 7 << 16 | -255 & 65535 ---- */
.macro SAW_SETUP
	R0 = [P5];
	R0 <<= 4;
	R1 = [P5 + 4];
	R1 <<= 4;
	R2 = [P5 + 16];                      /* steep (.L, < 21846) */
	R3 = [P5 + 8];
	R2 = PACK (R3.L, R2.L);              /* gain (.H) */
	R3 = [P5 + 20];                      /* recip */
	R5 = P5;
	R5 += 32;
	I2 = R5;
	B2 = R5;
	R5 = 12;
	L2 = R5;
	R5 = -1;
	R5 <<= 16;
	[P5 + 32] = R5;                      /* -65536 */
	R5 = ~R5;
	[P5 + 36] = R5;                      /* 65535 */
	R5.H = 7;
	R5.L = -255;
	[P5 + 40] = R5;                      /* (the gain's biases, sid_sawg) */
.endm

/* w = p >> 12; w += w * steep >> 16; past 65535: 65535 - ((w - 65536) * recip >> 4); R5 = wave (.L) */
.macro SAW_WAVE
	R5 = R0 >> 16 || R4 = [I2++] || NOP;      /* R4 = -65536 */
	R6 = R5.L * R2.L (FU);
	R6 >>= 16;
	R5 = R5 + R6;
	R6 = R5 + R4;
	R6 *= R3;
	R6 = R6 >> 4 || R4 = [I2++] || NOP;       /* R4 = 65535 */
	R6 = R4 - R6;
	CC = R4 < R5;
	IF CC R5 = R6;
.endm

	.align 4
	.global _sid_saw
	.type _sid_saw, STT_FUNC
_sid_saw:
	OSC_ENTER R4
	SAW_SETUP
	LSETUP (.Lsaw_b, .Lsaw_e) LC0 = P4;
.Lsaw_b:
	R0 = R0 + R1 (NS) || R7 = [I0++] || [I1++] = R4;
	SAW_WAVE
	BITTGL (R5, 15);
	R5 = R5.L (X);                            /* wave - 32768 */
	R6 = R5 >>> 31 || R4 = [I2++] || NOP;     /* (the biases, unused here) */
	R6 >>= 29;
	R5 = R5 + R6;
	R5 >>>= 3;
.Lsaw_e:
	R4 = R5 + R7;
	OSC_LEAVE R4
	.size _sid_saw, .-_sid_saw

	.align 4
	.global _sid_sawg
	.type _sid_sawg, STT_FUNC
_sid_sawg:
	OSC_ENTER R4
	SAW_SETUP
	LSETUP (.Lsawg_b, .Lsawg_e) LC0 = P4;
.Lsawg_b:
	R0 = R0 + R1 (NS) || [I1++] = R4 || NOP;
	SAW_WAVE
	R5 <<= 16;
	BITTGL (R5, 31);                          /* .H: wave - 32768 */
	R6 = R5 >>> 15 (V) || R4 = [I2++] || NOP; /* .H: -1 below zero; R4 = the biases */
	R7 = R6 & R4;                             /* .H: 7 below zero */
	R5 = R5 +|+ R7;
	R5 = R5 >>> 3 (V) || R7 = [I0++] || NOP;  /* .H: y */
	A1 = R5.H * R2.H (IS);
	R5 = (A1 += R6.H * R4.L) (IS);            /* y * gain + 255 when y < 0 */
	R5 >>>= 8;
.Lsawg_e:
	R4 = R5 + R7;
	OSC_LEAVE R4
	.size _sid_sawg, .-_sid_sawg

/* ---- sync: saw restarted by the mask's bits, the rises out; 26 packets. The mask register rotates through CC:
 * each sample's ROT takes its restart bit and puts the previous sample's rise in bit 0. I2 cycles 0, -65536, 65535.
 * The saw's select is done without CC here (CC carries the rise to the next ROT). ---- */
	.align 4
	.global _sid_sync
	.type _sid_sync, STT_FUNC
_sid_sync:
	OSC_ENTER R7
	R0 = [P5];
	R0 <<= 4;
	R1 = [P5 + 4];
	R1 <<= 4;
	R2 = [P5 + 16];                      /* steep */
	R3 = [P5 + 20];                      /* recip */
	R6 = [P5 + 12];                      /* restart mask */
	R5 = P5;
	R5 += 32;
	I2 = R5;
	B2 = R5;
	R5 = 12;
	L2 = R5;
	R5 = 0;
	[P5 + 32] = R5;
	R5 = -1;
	R5 <<= 16;
	[P5 + 36] = R5;
	R5 = ~R5;
	[P5 + 40] = R5;
	CC = R5 < 0;                         /* (false: the bit the first ROT puts in) */
	LSETUP (.Lsync_b, .Lsync_e) LC0 = P4;
.Lsync_b:
	R6 = ROT R6 BY 1 || R5 = [I2++] || [I1++] = R7;   /* CC = restart; R5 = 0 */
	R4 = ~R0;
	R0 = R0 + R1;
	IF CC R0 = R5;
	R4 = R4 & R0;
	CC = R4 < 0;                         /* rise */
	R5 = R0 >> 16 || R4 = [I2++] || NOP; /* R4 = -65536 */
	R7 = R5.L * R2.L (FU);
	R7 >>= 16;
	R5 = R5 + R7;
	R7 = R5 + R4;
	R7 *= R3;
	R7 = R7 >> 4 || R4 = [I2++] || NOP;  /* R4 = 65535 */
	R7 = R4 - R7;                        /* the wrapped value */
	R4 = R4 - R5;
	R4 >>>= 31;                          /* -1 past 65535 */
	R7 = R7 ^ R5;
	R7 = R7 & R4;
	R5 = R5 ^ R7;
	BITTGL (R5, 15);
	R5 = R5.L (X);
	R4 = R5 >>> 31 || R7 = [I0++] || NOP;
	R4 >>= 29;
	R5 = R5 + R4;
	R5 >>>= 3;
.Lsync_e:
	R7 = R5 + R7;
	R6 = ROT R6 BY 1;                    /* the last rise */
	[I1++] = R7;
	R0 >>= 4;
	[P5] = R0;
	R0 = R6;
	R3 = 0;
	L2 = R3;
	B2 = R3;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _sid_sync, .-_sid_sync

/* ---- pulse: 17 packets. I2 cycles peakA, peakB, 0, -1, pw << 16. R3 = pw << 16 | 65535 ---- */
	.align 4
	.global _sid_pulse
	.type _sid_pulse, STT_FUNC
_sid_pulse:
	OSC_ENTER R7
	R0 = [P5];
	R0 <<= 4;
	R1 = [P5 + 4];
	R1 <<= 4;
	R2 = [P5 + 16];                      /* steep (< 65536) */
	R5 = P5;
	R5 += 32;
	I2 = R5;
	B2 = R5;
	R5 = 20;
	L2 = R5;
	R5 = [P5 + 24];
	[P5 + 32] = R5;                      /* peakA */
	R5 = [P5 + 28];
	[P5 + 36] = R5;                      /* peakB */
	R5 = 0;
	[P5 + 40] = R5;
	R5 = -1;
	[P5 + 44] = R5;
	R5 = [P5 + 20];                      /* pw (<= 65535) */
	R5 <<= 16;
	[P5 + 48] = R5;
	R3 = R5;
	R5 = -1;
	R3 = PACK (R3.H, R5.L);              /* pw << 16 | 65535 */
	LSETUP (.Lpul_b, .Lpul_e) LC0 = P4;
.Lpul_b:
	R0 = R0 + R1 (NS) || R6 = [I2++] || [I1++] = R7;  /* R6 = peakA */
	R7 = PACK (R0.H, R0.H);
	R7 = R3 -|- R7;                      /* .H pw - t, .L 65535 - t */
	R5 = (A1 = R7.H * R2.L), R4 = (A0 = R7.L * R2.L) (FU);
	R5 = R6 - R5 (NS) || R7 = [I2++] || NOP;  /* t < pw: peakA - (pw - t) * steep; R7 = peakB */
	R4 = R4 - R7 (NS) || R6 = [I2++] || NOP;  /* else (65535 - t) * steep - peakB; R6 = 0 */
	R5 = MAX (R5, R6) || R7 = [I2++] || NOP;   /* R7 = -1 */
	R4 = MIN (R4, R7) || R6 = [I2++] || NOP;   /* (>= 0: 65535 after the & 65535); R6 = pw << 16 */
	CC = R0 < R6 (IU);
	IF CC R4 = R5;
	BITTGL (R4, 15);
	R4 = R4.L (X);
	R5 = R4 >>> 31 || R7 = [I0++] || NOP;
	R5 >>= 29;
	R4 = R4 + R5;
	R4 >>>= 3;
.Lpul_e:
	R7 = R4 + R7;
	OSC_LEAVE R7
	.size _sid_pulse, .-_sid_pulse

/* ---- noise: the LFSR clocks when phase bit 24 changes (or every sample); between clocks the sample stays.
 * 6 packets a sample without a clock. The LFSR keeps junk above bit 22 in the loop (nothing reads it); masked on the
 * way out. ---- */
.macro NOISE_CLOCK gain
	R5 = R2 << 5;
	R5 = R5 ^ R2;
	CC = BITTST (R5, 22);
	R2 = ROT R2 BY 1;
	R5 = R2 << 22;
	R5 >>= 22;                           /* bits 0..9 */
	R6 = R2 << 11;
	R6 >>= 21;                           /* bits 10..20 */
	P1 = R5;
	P2 = R6;
	P1 = P3 + (P1 << 1);
	P2 = P4 + (P2 << 1);
	R5 = W[P1] (Z);
	R6 = W[P2] (Z);
	R5 = R5 | R6;
	R5 <<= 16;
	BITTGL (R5, 31);
	.if \gain
	R5 >>>= 19;                          /* (wave - 32768) >> 3: exact, the wave's low 8 bits are 0 */
	R6 = [P5 + 8];
	R3 = R5.L * R6.L (IS);
	R6 = R3 >>> 31;
	R6 >>= 24;
	R3 = R3 + R6;
	R3 >>>= 8;                           /* SDIV(y * gain, 8) */
	.else
	R3 = R5 >>> 19;
	.endif
.endm

.macro NOISE_BODY gain, always, tag
	R6 = [P5 + 24];
	CC = R6 == 0;
	IF CC JUMP .Lnz_test\tag;
	LSETUP (.Lnz_ab\tag, .Lnz_ae\tag) LC0 = P0;
.Lnz_ab\tag:
	R0 = R0 + R1 (NS) || R7 = [I0++] || [I1++] = R4;
	NOISE_CLOCK \gain
.Lnz_ae\tag:
	R4 = R3 + R7;
	JUMP .Lnz_out\tag;
.Lnz_test\tag:
	LSETUP (.Lnz_tb\tag, .Lnz_te\tag) LC0 = P0;
.Lnz_tb\tag:
	R5 = R0 + R1 (NS) || R7 = [I0++] || [I1++] = R4;
	R6 = R5 ^ R0;
	R0 = R5;
	CC = BITTST (R6, 28);
	IF !CC JUMP .Lnz_te\tag;
	NOISE_CLOCK \gain
.Lnz_te\tag:
	R4 = R3 + R7;
.Lnz_out\tag:
.endm

.macro NOISE_FN name, gain
	.align 4
	.global _\name
	.type _\name, STT_FUNC
_\name:
	OSC_ENTER R4
	P0 = P4;
	R0 = [P5];
	R0 <<= 4;
	R1 = [P5 + 4];
	R1 <<= 4;
	R2 = [P5 + 16];                      /* LFSR */
	R3 = [P5 + 20];                      /* its sample */
	P3 = [P5 + 28];                      /* noise_low */
	P4 = [P5 + 32];                      /* noise_high */
	NOISE_BODY \gain, 0, \name
	R5 = R2 << 9;
	R5 >>= 9;
	[P5 + 16] = R5;
	OSC_LEAVE R4
	.size _\name, .-_\name
.endm

	NOISE_FN sid_noise, 0
	NOISE_FN sid_noiseg, 1

/* ---- uint32_t sid_udiv(uint32_t n, uint32_t d, unsigned b): n / d for sid_configure when n / d < 2^b (1 <= b <= 31,
 * 0 < d < 2^31): libgcc's restoring division (__udivsi3) started b bits from the end, where n >> b < d already, so 5b
 * packets in the loop instead of 160. Only the call-clobbered R0..R3, P0, P1 and LC0 ---- */
	.align 4
	.global _sid_udiv
	.type _sid_udiv, STT_FUNC
_sid_udiv:
	P0 = R2;
	R3 = -R2;
	R3 = LSHIFT R0 BY R3.L;              /* the partial remainder: n >> b */
	R2 += -32;
	R2 = -R2;                            /* 32 - b */
	P1 = R2;
	R0 = LSHIFT R0 BY R2.L;              /* n's low b bits at the top */
	LSETUP (.Ldiv_b, .Ldiv_e) LC0 = P0;
.Ldiv_b:
	R0 = ROT R0 BY 1;                    /* CC = the next bit of n; in: the last quotient bit, inverted */
	R3 = ROT R3 BY 1;
	R2 = R3 - R1;
	CC = R3 < R1 (IU);
.Ldiv_e:
	IF !CC R3 = R2;
	R0 = ROT R0 BY 1;
	R0 = ~R0;
	R2 = P1;
	R0 = LSHIFT R0 BY R2.L;
	R2 = -R2;
	R0 = LSHIFT R0 BY R2.L;              /* the b quotient bits */
	RTS;
	.size _sid_udiv, .-_sid_udiv

/* ---- the filter: 8580 lowpass, DC coupling, output clamp ----
 * high = acc + SDIV(band * res, 12) + low; band = clamp(band - cmul(high)); low = clamp(low + cmul(band));
 * dc += SDIV(low * 512 - dc, 11); out = sat16(low - SDIV(dc, 9)); cmul(x) = (x * chi + (x * clo >> 12)) >> 14
 * with chi = cutoff >> 12, clo = cutoff & 4095. R0 band, R1 low, R3 cutoff, R4 clo, R5 chi, R2 / R6 / R7 temps,
 * dc in f (P5 + 8). I3 cycles (glide: 256, 4095,) 0, -65536, 65536, -65536, 65536, (glide: target,) resonance.
 * 41 packets a sample settled, 47 gliding. */
.macro FILT_SAMPLE glide, end
	.if \glide
	R7 = R6 - R3 (NS) || R5 = [I3++] || NOP;          /* target - cutoff; R5 = 256 */
	R7 = R7 + R5 (NS) || R4 = [I3++] || NOP;          /* R4 = 4095 */
	R7 >>>= 9;
	R3 = R3 + R7;                                /* cutoff */
	R4 = R3 & R4;                                /* clo */
	R5 = R3 >>> 12;                              /* chi */
	.endif
	R6 = R0;
	R6 *= R2;                                    /* band * res */
	R7 = R6 >>> 31 || R2 = [I0++] || NOP;        /* R2 = the oscillators' sum */
	R7 >>= 20;
	R6 = R6 + R7;
	R6 = R6 >>> 12 || R7 = [I3++] || NOP;        /* R7 = 0 */
	R6 = R6 + R2 (NS) || [I1++] = R7 || NOP;          /* (acc cleared for the next call) */
	R6 = R6 + R1;                                /* high */
	R7 = R6;
	R7 *= R4;
	R6 *= R5;
	R7 = R7 >>> 12 || R2 = [I3++] || NOP;        /* R2 = -65536 */
	R6 = R6 + R7;
	R6 = R6 >>> 14 || R7 = [I3++] || NOP;        /* R7 = 65536 */
	R0 = R0 - R6;
	R0 = MAX (R0, R2);
	R0 = MIN (R0, R7);                           /* band */
	R6 = R0;
	R7 = R0;
	R6 *= R5;
	R7 *= R4;
	R7 = R7 >>> 12 || R2 = [I3++] || NOP;        /* R2 = -65536 */
	R6 = R6 + R7;
	R6 = R6 >>> 14 || R7 = [I3++] || NOP;        /* R7 = 65536 */
	R1 = R1 + R6;
	R1 = MAX (R1, R2);
	R1 = MIN (R1, R7);                           /* low */
	R6 = R1 << 9 || R2 = [P5 + 8] || NOP;        /* R2 = dc */
	R6 = R6 - R2;
	R7 = R6 >>> 31;
	R7 >>= 21;
	R6 = R6 + R7;
	R6 >>>= 11;
	R2 = R2 + R6;                                /* dc */
	R7 = R2 >>> 31;
	R7 >>= 23;
	R7 = R7 + R2 (NS) || [P5 + 8] = R2 || NOP;   /* (no shift beside a P store: anomaly 05000074) */
	R7 >>>= 9;                                   /* SDIV(dc, 9) */
	R7 = R1 - R7;
	.if \glide
	R7 = R7 << 16 (S) || R6 = [I3++] || NOP;     /* R6 = target */
	.else
	R7 = R7 << 16 (S);
	.endif
\end:
	MNOP || W[I2++] = R7.H || R2 = [I3++];       /* R2 = resonance */
.endm

	.align 4
	.global _sid_filter
	.type _sid_filter, STT_FUNC
_sid_filter:
	[--SP] = (R7:4, P5:3);
	P5 = R2;
	P4 = [SP + 40];                      /* n */
	R3 = 0;
	L0 = R3;
	L1 = R3;
	L2 = R3;
	I2 = R0;                             /* out */
	I0 = R1;                             /* acc (read) */
	I1 = R1;                             /* acc (cleared) */
	R4 = P5;
	R4 += 32;
	I3 = R4;
	B3 = R4;
	R0 = [P5];                           /* band */
	R1 = [P5 + 4];                       /* low */
	R3 = [P5 + 12];                      /* cutoff */
	R2 = [P5 + 20];                      /* resonance */
	R6 = [P5 + 24];
	CC = R6 == 0;
	IF CC JUMP .Lflt_set;
	R6 = 256;
	[P5 + 32] = R6;
	R6 = 4095;
	[P5 + 36] = R6;
	R6 = 0;
	[P5 + 40] = R6;
	R6 = -1;
	R6 <<= 16;
	[P5 + 44] = R6;
	[P5 + 52] = R6;
	R6 = -R6;
	[P5 + 48] = R6;
	[P5 + 56] = R6;
	R6 = [P5 + 16];
	[P5 + 60] = R6;                      /* target */
	[P5 + 64] = R2;
	R6 = 36;
	L3 = R6;
	R6 = [P5 + 16];
	LSETUP (.Lflg_b, .Lflg_e) LC0 = P4;
.Lflg_b:
	FILT_SAMPLE 1, .Lflg_e
	JUMP .Lflt_done;
.Lflt_set:
	R5 = R3 >>> 12;                      /* chi */
	R6 = 4095;
	R4 = R3 & R6;                        /* clo */
	R6 = 0;
	[P5 + 32] = R6;
	R6 = -1;
	R6 <<= 16;
	[P5 + 36] = R6;
	[P5 + 44] = R6;
	R6 = -R6;
	[P5 + 40] = R6;
	[P5 + 48] = R6;
	[P5 + 52] = R2;
	R6 = 24;
	L3 = R6;
	LSETUP (.Lfls_b, .Lfls_e) LC0 = P4;
.Lfls_b:
	FILT_SAMPLE 0, .Lfls_e
.Lflt_done:
	[P5] = R0;
	[P5 + 4] = R1;
	[P5 + 12] = R3;
	R3 = 0;
	L3 = R3;
	B3 = R3;
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _sid_filter, .-_sid_filter

#endif
