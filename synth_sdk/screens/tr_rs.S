/* Track outs: two tracks' resampling in one pass (screens/usbtrack.c tr_rs_pair, the C kept for host builds and
 * -DTR_RS_C_REF), bit for bit. R246's resampler arithmetic per channel (tape_resample_read 0xFFA06194): 16 fractional
 * MACs with 32-bit saturation (W32), the gain times the accumulator's high half rounded, the gain stepped after each
 * output. The source holds the pair as words (track a low half, track b high half), so one dual MAC does a tap of
 * both: A0 += c * s.L (a), A1 += c * s.H (b), with the next coefficient (P2, stride 64 halfwords) and source word (I0)
 * loaded in the same instruction: 16 cycles + about 12 per output pair.
 *   void tr_rs_pair(const tr_rs_job *j)   j: 0 src, 4 coef, 8 out_a, 12 out_b, 16 pos, 20 inc, 24 g (int16), 26 gi
 * Saves R4..R7, P3..P5; L0 / L1 / L2 stay 0 (I0..I2 linear). Reads one coefficient row and one source word past the
 * last tap (the loop's look-ahead; the caller pads the source). Host builds assemble it empty. */
#ifdef __bfin__
	.text
	.align 4
	.global _tr_rs_pair
	.type _tr_rs_pair, STT_FUNC
_tr_rs_pair:
	[--SP] = (R7:4, P5:3);
	P0 = R0;
	P4 = [P0];                          /* src */
	P3 = [P0 + 4];                      /* coef */
	R0 = [P0 + 8];
	I2 = R0;                            /* out a */
	R0 = [P0 + 12];
	I1 = R0;                            /* out b */
	R6 = [P0 + 16];                     /* pos */
	R1 = [P0 + 20];                     /* inc */
	R7 = W[P0 + 24] (X);                /* g */
	R5 = W[P0 + 26] (X);                /* gi */
	P1 = 128 (X);                       /* coefficient row stride (64 halfwords) */
	P5 = 16 (X);
	P0 = 64 (X);
	LSETUP (.Lo_b, .Lo_e) LC1 = P0;
.Lo_b:
	R6 = R6 + R1;                       /* pos += inc */
	R0 = R6 >>> 6;                      /* idx */
	R4 = ~R6;
	R4 <<= 26;
	R4 >>= 26;                          /* 63 - (pos & 63) */
	P0 = R0;
	P2 = R4;
	P0 = P4 + (P0 << 2);
	P2 = P3 + (P2 << 1);
	I0 = P0;
	R2.H = W[P2++P1];
	A1 = A0 = 0 || R3 = [I0++] || NOP;
	LSETUP (.Li, .Li) LC0 = P5;
.Li:	A1 += R2.H * R3.H, A0 += R2.H * R3.L (W32) || R2.H = W[P2++P1] || R3 = [I0++];
	R3 = A0.W;
	R4.H = R7.L * R3.H;                 /* g x high half, rounded */
	R3 = A1.W;
	R0.H = R7.L * R3.H || W[I2++] = R4.H || NOP;
	W[I1++] = R0.H;
.Lo_e:	R7.L = R7.L + R5.L (S);             /* the gain ramp */
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _tr_rs_pair, .-_tr_rs_pair
#endif
