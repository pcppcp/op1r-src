/* Free tape machine in L1 (docs/82): the dead Lander audio range 0xFFA01D60..0xFFA020A8 (840 bytes, docs/56).
 * 0xFFA01D60: mode-2 guard as before (jump over the range).
 * _l1_tpskip: CALLed from tape_play_mix 0xFFA06E04 (in place of R0 = [SP + 0x68], the gather loop's head). With the
 * free machine live, R246's 4-track gather (clip lookups, loop crossfade, mix) is skipped: the per-track gains
 * (stack L[4] at FP - 0x48, R[4] at FP - 0x40) go to tm_g, the loop-crossfade flag is cleared, and it returns to the
 * loop's exit 0xFFA071DC (everything after reloads from the stack / FP; P5 is set at entry, untouched by the loop).
 * R246's transport, resampler (position advance) and start / stop ramp still run; tm_block replaces the output. */
	.section .l1tape,"ax"
	.global _l1_tape_guard
_l1_tape_guard:
	jump.l _l1_lander_end;
	.align 2
	.global _l1_tpskip
_l1_tpskip:
	[--SP] = ASTAT; [--SP] = P0; [--SP] = R1;
	P0.L = _tm_live; P0.H = _tm_live;
	R1 = B[P0] (Z);
	CC = R1 == 0;
	if CC jump 1f;
	P0.L = _tm_g; P0.H = _tm_g;
	R1 = [FP + -0x48]; [P0] = R1;
	R1 = [FP + -0x44]; [P0 + 4] = R1;
	R1 = [FP + -0x40]; [P0 + 8] = R1;
	R1 = [FP + -0x3C]; [P0 + 12] = R1;
	P0.L = 0x17F4; P0.H = 0x018C;
	R1 = 0; B[P0] = R1;
	R1.L = 0x71DC; R1.H = 0xFFA0;
	RETS = R1;
	R1 = [SP++]; P0 = [SP++]; ASTAT = [SP++];
	rts;
1:	R1 = [SP++]; P0 = [SP++]; ASTAT = [SP++];
	R0 = [SP + 0x68];
	rts;
