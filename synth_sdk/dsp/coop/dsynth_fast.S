/* The stock DSynth voice loop rewritten: taken from coop-fw (core/sdram/stereo_thunks.S, coldsoldr, 2026), the
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
    .text
/* ---- the stock DSynth voice loop, rewritten (performance, 2026-11-04) ----
 * DSynth's render (0x0197E1D8) finishes each voice with a 128-sample loop (0x0197E974..0x0197EA6A): out = buf * gain *
 * env, << 1 (S); a one-pole lowpass with 32-bit state (s += c * (v - s), as two 32 x 32 products); x 8 (S); a DC
 * blocker (y = c2 * (y + w - w_prev)). The compiled loop is a software loop with four CALLs per sample to one-line
 * accessors (0x0197EF08 / EF1C / EF2C / EF40) and every intermediate value through the stack. This is the same
 * arithmetic, instruction for instruction (the 32 x 32 multiply sequences are the stock's), in a hardware loop with
 * the state in registers. Entry: JUMP.L from 0x0197E956, which every path into the loop goes through (0x0197E882,
 * 0x0197EB60 ... 0x0197EE00); R7 = the lowpass coefficient, [FP-0x2C] the gain (low half), [FP-0x10] the DC
 * coefficient, P5 = the voice object, [SP+0x30] the buffer, the envelope at L1 0xFF801D50. Exit: JUMP.L 0x0197EA6C,
 * which reads only P0 / P1 (= the object) and the stack words [FP-0x20] / [FP-0x34] set here; R4-R7 are restored by
 * the function's epilogue. Uses LC0 (the function itself sets up LC0 loops) and M0 (call-clobbered). */
    .section .near, "ax"
    .global _coop_dsynth_voice_loop
    .type   _coop_dsynth_voice_loop, STT_FUNC
_coop_dsynth_voice_loop:
    P1.L = _g_coop_fast_dsynth; P1.H = _g_coop_fast_dsynth;   /* 0: the stock loop (A/B, kill switch). R0 and CC */
    R0 = [P1];                                                 /* are free here: the code at 0x0197E95A sets both */
    CC = R0 == 0;
    IF CC JUMP 3f;
#ifdef OP1R_COOP_HITS
    P1.L = _coop_hits + 0; P1.H = _coop_hits + 0; R0 = [P1]; R0 += 1; [P1] = R0;   /* op1r test builds: the rewrite ran */
#endif
    P1 = FP;
    P1 += -10;
    [FP + -0x20] = P5;
    [FP + -0x34] = P1;
    P5 += 8;
    P3 = [SP + 0x30];
    P4.L = 0x1d50; P4.H = 0xff80;
    R0 = [FP + -0x2c];
    M0 = R0;                                       /* the gain (low half) */
    R6 = [FP + -0x10];                             /* the DC coefficient */
    R4 = [P5 + 0x2c];                              /* lowpass state s */
    R5 = [P5 + 0x38];                              /* DC state y */
    R3 = [P5 + 0x3c];                              /* DC input w_prev << 16 */
    P0 = 128;
    LSETUP (1f, 2f) LC0 = P0;
1:  R1 = W[P3] (X);
    R2 = M0;
    R1.L = R1.L * R2.L || R0 = W[P4++] (X) || NOP;
    R1.L = R1.L * R0.L;
    R1.H = R1.L << 1 (S);
    R1 = R1 >>> 16;
    R2 = R1 << 16;                                 /* v << 16 */
    A1 = R2.L * R7.L (FU);                         /* v * c */
    A1 = A1 >> 16;
    A1 += R2.H * R7.L (M), A0 = R2.H * R7.H;
    A1 += R7.H * R2.L (M);
    A1 = A1 >>> 15;
    R2 = (A0 += A1);
    R0 = -R4 (S);                                  /* -s * c */
    A1 = R0.L * R7.L (FU);
    A1 = A1 >> 16;
    A1 += R0.H * R7.L (M), A0 = R0.H * R7.H;
    A1 += R7.H * R0.L (M);
    A1 = A1 >>> 15;
    R0 = (A0 += A1);
    R2 = R4 + R2 (S);
    R4 = R2 + R0 (S);                              /* s' */
    R0 = R4 >>> 16;
    R2.L = R0.L << 3 (S);                          /* w */
    R2 = R2 << 16;
    R1 = R2 + R5 (S);
    R1 = R1 - R3 (S);
    R3 = R2;
    A1 = R6.L * R1.L (FU);                         /* y' = c2 * (y + w - w_prev) */
    A1 = A1 >> 16;
    A1 += R6.H * R1.L (M), A0 = R6.H * R1.H;
    A1 += R1.H * R6.L (M);
    A1 = A1 >>> 15;
    R5 = (A0 += A1);
    W[P3] = R5.H;
2:  P3 += 2;
    [P5 + 0x2c] = R4;
    [P5 + 0x38] = R5;
    [P5 + 0x3c] = R3;
    P1 = [FP + -0x20];
    P0 = P1;
    JUMP.L _stock_dsynth_loop_back;
3:  P1 = FP;                                       /* the two replaced instructions, then the stock loop */
    P1 += -10;
    JUMP.L _stock_dsynth_loop_entry;
    .size _coop_dsynth_voice_loop, .-_coop_dsynth_voice_loop
