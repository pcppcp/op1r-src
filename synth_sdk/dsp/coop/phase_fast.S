/* The stock Phase synth engine's per-sample voice loop rewritten (performance, 2026-11-04).
 *
 * Phase (Pd.cpp) renders a voice with render(this = R0, out = R1, params = R2) at 0x01A0BC5C (vtable + 0x14 of its
 * voice class; no CALL or jump in the image targets it or its loop): 128 samples at 88.2 kHz into a 16-bit buffer. The
 * setup (LINK 0x24, seven registers: FP = SP + 0x40) leaves per-block values in the frame and runs one LC0 loop
 * 0x01A0BCE4..0x01A0BD5A whose body is split: part A in the loop range, part B out of line at 0x01A0BD88..0x01A0BE80,
 * which branches back to the loop's last instruction (the NOP at 0x01A0BD5A), so loopscan sees a software loop. The
 * voice (28 bytes): ph1 = this[4], ph2 = this[0xc], W[this + 0x10], the flag B[this + 0x14], the count this[0x18].
 * Per sample, with d = the pitch word [I0++] (L1 0xFF801950) and T the 8192-entry sine table (P3):
 *   A: s = T[ph1 >>> 19 + 4096]; ph1 += d; w1 = (new < old, signed)
 *      t = s * p2; out = t >>> 1; gH = sat(t + 0x4000)                         (16-bit fractional, as the stock)
 *      on w1: ph2 = (p3' == 0) ? ph1 : 0x80000000 (then W[this + 0x10] = 0); count = 0; flag = 1
 *   B (flag != 0): q = (ph1 >>> 1) + 0x40000000 (S); g = q.H * p1 (the stock's sat((ph1.H >>> 1) + 0x4000) is q.H:
 *      neither can overflow); r = sat(sat(d + M(sat(d << 2), p0sq << 16)) + M(M(sat(d << 8), q), p3sq))
 *      s2 = T[ph2 >>> 19 + 4096]; ph2 += r; w2 = (new < old, signed); h = sat(0x7fff - g)
 *      out = s2 * h * h * h * gH; on w2: count += 1, flag = 0 when p3' < count
 * M is the stock's six-instruction 32 x 32 multiply; p0sq = p0 * p0, p3sq = M(p3 << 16, p3 << 16), p3' = p3 >>> 10
 * ([SP+0x3e], [SP+0x38], [SP+0x30]), p1, p2 = params[1], [2].
 *
 * The rewrite: every multiply, add, shift and saturation on the signal is the stock's with its operands and modes; what
 * changed is the bookkeeping:
 *   - the table index as (T + 0x2000) + 2 * (v >>> 19) (one shift for the stock's shift, add and shift);
 *   - ph1 / ph2 load and store in the parallel slots (ph1 for the next pass in the loop's last packet, with the output
 *     store through I1), the per-block values from the frame in the parallel slots, packed by the setup into two words:
 *     [SP+0x1c] = p1 : p2, [SP+0x24] = 0x7fff : p0sq (the stock's own words there are its loop's only);
 *   - the flag in P4 (written back after the loop), tested with one compare; the wrap of ph1 and of ph2 and the count
 *     out of line (rare), without the stock's flag bytes in the frame;
 *   - M(sat(d << 2), p0sq << 16) without its two zero terms (the low half is 0): 3 instructions, not 6;
 *   - q.H for the stock's separately computed sat((ph1.H >>> 1) + 0x4000).
 * 13 packets a sample when the flag is 0 and 45 when it is 1, against the stock's 30 and 82.
 *
 * Not reproduced: the stock loop's frame flag bytes (B[SP+0x50], B[SP+0x51]) and its stores to the caller's argument
 * homes ([FP+8], [FP+0xc]), dead after the loop.
 *
 * Entry: JUMP.L from 0x01A0BCE0 (the loop's LSETUP, the only way into it) to coop_phase_loop_near. There: P4 = 128,
 * P3 = T, P2 = this, P0 = P5 = out, I0 = the pitch words, the frame as above. Exit: JUMP 0x01A0BD5C with P2 = this,
 * P0 = out and the voice written; the code after reads only those (and sets P1 itself). Uses LC0 / LT0 / LB0 (as the
 * stock), I1, P1, P4 (callee-saved, popped by the stock epilogue). g_coop_fast_phase = 0 runs the stock loop. */

/* test builds (-DCOOP_PHASE_CHECK): count the calls of the rewrite and its wrap paths. Uses P1 and the register r. */
.macro HIT name, r
#ifdef COOP_PHASE_CHECK
    P1.L = \name; P1.H = \name;
    \r = [P1];
    \r += 1;
    [P1] = \r;
#endif
.endm

    .section .near, "ax"
    .global _coop_phase_loop_near
    .type   _coop_phase_loop_near, STT_FUNC
_coop_phase_loop_near:
    P1.L = _g_coop_fast_phase; P1.H = _g_coop_fast_phase;       /* R0 / P1 / CC are free here: the stock loop sets */
    R0 = [P1];                                                   /* each before reading it */
    CC = R0 == 0;
    IF CC JUMP 1f;
#ifdef OP1R_COOP_HITS
    P1.L = _coop_hits + 32; P1.H = _coop_hits + 32; R0 = [P1]; R0 += 1; [P1] = R0;   /* op1r test builds: the rewrite ran */
#endif
    P1.L = _coop_phase_loop; P1.H = _coop_phase_loop;            /* region A is beyond JUMP.L reach of the stock */
    JUMP (P1);
1:  P1.L = _stock_phase_loop_top; P1.H = _stock_phase_loop_top;   /* the replaced LSETUP, by hand */
    LT0 = P1;
    P1.L = _stock_phase_loop_bottom; P1.H = _stock_phase_loop_bottom;
    LB0 = P1;
    LC0 = P4;                                                    /* P4 = 128 */
    JUMP.L _stock_phase_loop_top;
    .size _coop_phase_loop_near, .-_coop_phase_loop_near

    .text
    .align 4
    .global _coop_phase_loop
    .type   _coop_phase_loop, STT_FUNC
_coop_phase_loop:
    HIT _g_phase_hit_calls, R0
    R0 = W[SP + 0x1c] (Z);                     /* p2 (the stock reads only the low half of this word) */
    R1 = W[SP + 0x28] (Z);                     /* p1 (likewise) */
    R0 = PACK (R1.L, R0.L);
    [SP + 0x1c] = R0;                          /* p1 : p2 */
    R0 = W[SP + 0x3e] (Z);                     /* p0sq, the high half of [SP+0x3c] */
    R0.H = 0x7fff;
    [SP + 0x24] = R0;                          /* 0x7fff : p0sq */
    P1.L = 0x2000; P1.H = 0;
    P3 = P3 + P1;                              /* T + 0x2000 */
    I1 = P5;                                   /* out */
    R0 = B[P2 + 0x14] (Z);
    P1 = R0;                                   /* the flag */
    R4 = [P2 + 4];                             /* the loop's rotated load for the first pass: ph1 */
    LSETUP (p_top, p_bot) LC0 = P4;
    P4 = P1;
/* R4 = ph1 at the top of a pass; P2 = this, P3 = T + 0x2000, P4 = the flag, I0 = the pitch words, I1 = out. */
p_top:
    R0 = R4 >>> 19 || R1 = [SP + 0x1c] || R6 = [I0++];                  /* i1; p1 : p2; d */
    P1 = R0;
    R2 = R4 + R6 (NS) || R3 = [SP + 0x20] || NOP;                        /* ph1 + d; K = 0x40000000 */
    P1 = P3 + (P1 << 1);
    CC = R2 < R4;                                                        /* w1 */
    R4 = R2 >>> 1 || R0 = W[P1] (X) || NOP;                              /* ph1 >>> 1 (for q); s */
    R1.L = R0.L * R1.L || [P2 + 4] = R2 || NOP;                          /* t = s * p2 */
    R7.L = R1.L >>> 1 || R5 = [P2 + 0xc] || NOP;                         /* out A; ph2 */
    R7.H = R1.L + R3.H (S) || R0 = [SP + 0x24] || NOP;                   /* gH; 0x7fff : p0sq */
    IF CC JUMP p_wrap1;
    CC = P4 == 0;
    IF CC JUMP p_bot;
/* part B. R0 = 0x7fff : p0sq, R1 = p1 : t, R2 = ph1, R3 = K, R4 = ph1 >>> 1, R5 = ph2, R6 = d, R7 = gH : out A */
p_b:
    R4 = R4 + R3 (S);                                                    /* q */
    R2 = R5 >>> 19;                                                      /* i2 */
    P1 = R2;
    R3 = R6 << 2 (S);                                                    /* d2 */
    P1 = P3 + (P1 << 1);
    A1 = R0.L * R3.L (M), A0 = R0.L * R3.H;                              /* M(d2, p0sq << 16), its low terms 0 */
    A1 = A1 >>> 15;
    R2 = (A0 += A1);
    R2 = R6 + R2 (S);                                                    /* sat(d + m0) */
    R3 = R6 << 8 (S);                                                    /* d8 */
    A1 = R3.L * R4.L (FU);                                               /* m1 = M(d8, q) */
    A1 = A1 >> 16;
    A1 += R3.H * R4.L (M), A0 = R3.H * R4.H;
    A1 += R4.H * R3.L (M);
    A1 = A1 >>> 15 || R3 = W[P1] (X) || NOP;                             /* s2 */
    R6 = (A0 += A1);
    R1.L = R4.H * R1.H || R4 = [SP + 0x38] || NOP;                       /* g = q.H * p1; p3sq */
    A1 = R6.L * R4.L (FU);                                               /* m2 = M(m1, p3sq) */
    A1 = A1 >> 16;
    A1 += R6.H * R4.L (M), A0 = R6.H * R4.H;
    A1 += R4.H * R6.L (M);
    A1 = A1 >>> 15;
    R4 = (A0 += A1);
    R2 = R2 + R4 (S);
    R2 = R2 + R5 (NS);                                                   /* ph2 new */
    CC = R2 < R5;                                                        /* w2 */
    R1.L = R0.H - R1.L (S) || [P2 + 0xc] = R2 || NOP;                    /* h */
    R3.L = R3.L * R1.L;
    R3.L = R3.L * R1.L;
    R3.L = R3.L * R1.L;
    R7.L = R3.L * R7.H;                                                  /* out B */
    IF CC JUMP p_wrap2;
p_bot:
    MNOP || R4 = [P2 + 4] || W[I1++] = R7.L;                             /* the next pass's ph1; out */

    R0 = P4;
    B[P2 + 0x14] = R0;                         /* the flag */
    P1.L = _stock_phase_loop_back; P1.H = _stock_phase_loop_back;
    JUMP (P1);

/* ph1 wrapped: R2 = ph1 new; R5 and P1 are free (R5 = ph2 is reloaded); the flag becomes 1, so part B follows */
p_wrap1:
    HIT _g_phase_hit_wrap1, R5
    R5 = [SP + 0x30];                          /* p3 >>> 10 */
    CC = R5 == 0;
    IF CC JUMP 1f;
    R5 = [SP + 0x2c];                          /* 0x80000000 */
    [P2 + 0xc] = R5;
    W[P2 + 0x10] = R5;
    JUMP 2f;
1:  [P2 + 0xc] = R2;
2:  R5 = 0;
    [P2 + 0x18] = R5;
    P4 = 1;
    R5 = [P2 + 0xc];
    JUMP p_b;

/* ph2 wrapped: R7.L = out; R0-R6 are free */
p_wrap2:
    HIT _g_phase_hit_wrap2, R0
    R0 = [P2 + 0x18];
    R0 += 1;
    [P2 + 0x18] = R0;
    R1 = [SP + 0x30];
    CC = R1 < R0;
    R1 = 0;
    IF CC P4 = R1;
    JUMP p_bot;
    .size _coop_phase_loop, .-_coop_phase_loop
