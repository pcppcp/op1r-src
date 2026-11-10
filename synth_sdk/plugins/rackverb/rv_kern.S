/* rackverb: the block engine's four steps over a row of samples, two at a time (2 x 16-bit vector ops; rv_mv.c's
 * C loops are the reference, kern_check.py compares them on bcore). acc is 4-byte aligned; the ring span may start
 * on any word (16-bit loads and stores through I0). half(x) = (x >>> 1) + (x < 0), every add wraps at 16 bits.
 *   rv_vsum(acc, src, pairs)   acc += half(src)
 *   rv_vld(acc, src, pairs)    acc = half(src)
 *   rv_vstp(acc, dst, pairs)   dst = acc; acc += half(acc)
 *   rv_vstn(acc, dst, pairs)   dst = ~acc; acc = half(~acc)
 * pairs >= 1. Uses I0..I2 with L0..L2 = 0 (the caller restores its own L values, as for father_kern.S), LC0.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifdef __bfin__                          /* host builds (screen harness, goldens) compile it empty */
	.text

	.align 4
	.global _rv_vsum
	.type _rv_vsum, STT_FUNC
_rv_vsum:
	P1 = R2;
	I0 = R1;
	I1 = R0;
	I2 = R0;
	L0 = 0;
	L1 = 0;
	L2 = 0;
	R0.L = W[I0++];                   /* pipelined: the next pair loads while this one adds (reads one pair past) */
	R0.H = W[I0++];
	LSETUP (.Lsum0, .Lsum1) LC0 = P1;
.Lsum0:	R1 = R0 >>> 1 (V) || R3 = [I1++] || NOP;
	R0 = R0 >> 15 (V);
	R1 = R1 +|+ R0 || R0.L = W[I0++] || NOP;
	R3 = R3 +|+ R1 || R0.H = W[I0++] || NOP;
.Lsum1:	[I2++] = R3;
	RTS;
	.size _rv_vsum, .-_rv_vsum

	.align 4
	.global _rv_vld
	.type _rv_vld, STT_FUNC
_rv_vld:
	P1 = R2;
	I0 = R1;
	I1 = R0;
	L0 = 0;
	L1 = 0;
	R0.L = W[I0++];
	R0.H = W[I0++];
	LSETUP (.Lld0, .Lld1) LC0 = P1;
.Lld0:	R1 = R0 >>> 1 (V);
	R0 = R0 >> 15 (V);
	R1 = R1 +|+ R0 || R0.L = W[I0++] || NOP;
	R0.H = W[I0++];
.Lld1:	[I1++] = R1;
	RTS;
	.size _rv_vld, .-_rv_vld

	.align 4
	.global _rv_vstp
	.type _rv_vstp, STT_FUNC
_rv_vstp:
	P1 = R2;
	I0 = R1;
	I1 = R0;
	I2 = R0;
	L0 = 0;
	L1 = 0;
	L2 = 0;
	LSETUP (.Lstp0, .Lstp1) LC0 = P1;
.Lstp0:	R0 = [I1++];
	R1 = R0 >>> 1 (V) || W[I0++] = R0.L || NOP;
	R2 = R0 >> 15 (V) || W[I0++] = R0.H || NOP;
	R1 = R1 +|+ R2;
	R0 = R0 +|+ R1;
.Lstp1:	[I2++] = R0;
	RTS;
	.size _rv_vstp, .-_rv_vstp

	.align 4
	.global _rv_vstn
	.type _rv_vstn, STT_FUNC
_rv_vstn:
	P1 = R2;
	I0 = R1;
	I1 = R0;
	I2 = R0;
	L0 = 0;
	L1 = 0;
	L2 = 0;
	LSETUP (.Lstn0, .Lstn1) LC0 = P1;
.Lstn0:	R0 = [I1++];
	R0 = ~R0;
	R1 = R0 >>> 1 (V) || W[I0++] = R0.L || NOP;
	R0 = R0 >> 15 (V) || W[I0++] = R0.H || NOP;
	R1 = R1 +|+ R0;
.Lstn1:	[I2++] = R1;
	RTS;
	.size _rv_vstn, .-_rv_vstn
#endif
