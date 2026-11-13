/* The stock nitro effect's two per-sample loops rewritten (performance, 2026-11-04).
 *
 * nitro is dual-mono: the stereo render 0x019A684C calls the mono render 0x019A62A4 twice (this + 4, this + 0x40; R1 =
 * the in-place I/O buffer, R2 = the parameter block); those are its only callers. The mono render (LINK 0x2c, seven
 * registers pushed, SP -= 12: FP = SP + 0x54) computes its coefficients, then runs two 64-sample loops:
 *   - LC1 0x019A64A8..0x019A6682: x = in[n]; A = S(s0) (s0 = this[4], the loop's own last stage output), the input
 *     driven against it, r = sat(sat(A - (x << 14)) << 2 ), u = sat((x << 14) - M(r, k1)), F = S(u), then a four-stage
 *     one-pole ladder y_k = sat(sat(z_k + M(y_k-1, g)) + M(sat(-z_k), g)) (y_-1 = F, states this[0x1c..0x28]) with the
 *     poles mixed, acc = M(c, F) + M(cc_0, y_0) + .. + M(cc_3, y_3) (saturating, in this order), and S(sat(acc << 2)),
 *     32-bit, into a 64-word L1 buffer at 0xFF801B50 (the stock keeps the inner three stages in an LC0 loop);
 *   - LC1 0x019A6698..0x019A67F6: v = that buffer[n]; G = S(s2) (s2 = this[8]), r = sat(sat(G - v) << 2), w = sat(v -
 *     M(r, k2)), H = S(w), the same ladder without the mix (coefficient g2, states this[0x2c..0x38]), and
 *     out[n] = S(y_3).H, in place.
 * S(v) is the stock's interpolated lookup in the 8192-entry sine table: i = 0x1000 - (v.H >>> 4), f = (v.H & 0xf) << 11,
 * sat(f * T[i] + (0x7fff - f) * T[i+1]) (16 x 16 fractional products). M(a, b) is the stock's 32 x 32 multiply (six MAC
 * instructions), kept instruction for instruction, operands in the stock's order.
 *
 * The rewrite: every multiply, add, shift and saturation on the signal is the stock's; the bookkeeping is shorter:
 *   - the lookup's index as (T + 0x2002) + 2 * ~(v >>> 20) (~(v >>> 20) = -(v.H >>> 4) - 1), f from v.H << 12 >> 1, and
 *     the two products summed in A0 (both are non-negative fractions below 1, so neither saturates alone and the
 *     40-bit sum saturated once equals the stock's two saturated products added with saturation);
 *   - x << 14 as one integer multiply by 0x4000; the ladder stages unrolled, states and coefficients loaded in the
 *     parallel slots straight from the object and the frame;
 *   - the second loop's G = S(s2) is the previous sample's output lookup S(y_3) (s2 is y_3): computed once.
 * Per sample about 132 + 87 packets against the stock's 180 + 131.
 *
 * Entry: JUMP.L from 0x019A64A4 (loop 1's LSETUP, the only way into the loops) to coop_nitro_loop_near. There: R0 =
 * this[4], R6 = g, R4 = g2, P0 = this, P5 = the sine table (the package retargets it to the L1 copy), I0 = the I/O
 * buffer, I1 = 0xFF801B50, P2 = 0x40, the frame words [FP+0x10] k1, [FP+0xc] k2, [FP-0x14] c, [FP-0x10..-4] cc_0..3.
 * Exit: JUMP 0x019A67F8 with P1 = this, R0 = the second loop's last y_3 (stored to this[8] there), I0 as on entry; the
 * code after reads only those and [SP+0x3c]; the epilogue restores R4-R7 / P3-P5. Not reproduced: the stock's spill
 * [FP+8] (dead after the loop). Uses LC1, I1-I3, B1, M0 (L registers stay 0). g_coop_fast_nitro = 0 runs the stock loops. */

/* dst (even) = S(v): index temp R4, frac pack R3, table words R4 / R5. R7 = 0x7FFF4000, P5 = T + 0x2002. The first
 * packet is the caller's (R4 = v >>> 20, with whatever rides in it). xs: an extra slot-3 access for the frac packet. */
.macro LOOKUP_REST v, dst, xs=NOP
    R4 = ~R4;
    P1 = R4;
    P1 = P5 + (P1 << 1);
    R3.L = \v\().H << 12 || R4 = W[P1] (X) || \xs;
    R3.L = R3.L >> 1 || R5 = W[P1 + 2] (X) || NOP;
.endm

/* (bfin gas does not substitute a macro parameter directly before ']': the stage macros write \name\() there.) */

/* the stock 32 x 32 multiply: dst = M(a, b) */
.macro MUL32 a, b, dst
    A1 = \a\().L * \b\().L (FU);
    A1 = A1 >> 16;
    A1 += \a\().H * \b\().L (M), A0 = \a\().H * \b\().H;
    A1 += \b\().H * \a\().L (M);
    A1 = A1 >>> 15;
    \dst = (A0 += A1);
.endm

/* loop 1, stage k: R0 = y_k-1, R2 = z_k, R5 = acc, R6 = g. Leaves R0 = y_k (stored to this[zoff]), acc += M(cc_k, y_k),
 * R2 = this[znext] (z_k+1; harmless for the last stage: this[0x2c]). cc_k = [FP - ccneg]. */
.macro STAGE1 ccneg, zoff, znext
    MUL32 R0, R6, R0
    R1 = -R2 (S);
    MUL32 R1, R6, R1
    R0 = R2 + R0 (S);
    R0 = R0 + R1 (S) || R1 = [FP - \ccneg\()] || NOP;
    A1 = R1.L * R0.L (FU) || [P4 + \zoff\()] = R0 || NOP;
    A1 = A1 >> 16 || R2 = [P4 + \znext\()] || NOP;
    A1 += R1.H * R0.L (M), A0 = R1.H * R0.H;
    A1 += R0.H * R1.L (M);
    A1 = A1 >>> 15;
    R1 = (A0 += A1);
    R5 = R5 + R1 (S);
.endm

/* loop 2, stage k: R0 = y_k-1 (stored to this[pzoff] in the first packet when pzoff != 0), R2 = z_k, R6 = g2. Leaves
 * R0 = y_k and R2 = this[znext] (z_k+1; znext = 0: no load, the last stage). */
.macro STAGE2 pzoff, znext=0
    .if \pzoff
    A1 = R0.L * R6.L (FU) || [P4 + \pzoff\()] = R0 || NOP;
    .else
    A1 = R0.L * R6.L (FU);
    .endif
    A1 = A1 >> 16;
    A1 += R0.H * R6.L (M), A0 = R0.H * R6.H;
    A1 += R6.H * R0.L (M);
    A1 = A1 >>> 15;
    R0 = (A0 += A1);
    R1 = -R2 (S);
    MUL32 R1, R6, R1
    R0 = R2 + R0 (S);
    .if \znext
    R0 = R0 + R1 (S) || R2 = [P4 + \znext\()] || NOP;
    .else
    R0 = R0 + R1 (S);
    .endif
.endm

/* test builds (-DCOOP_NITRO_CHECK): count the calls of the rewrite */
.macro HIT name
#ifdef COOP_NITRO_CHECK
    P1.L = \name; P1.H = \name;
    R1 = [P1];
    R1 += 1;
    [P1] = R1;
#endif
.endm

    .section .near, "ax"
    .global _coop_nitro_loop_near
    .type   _coop_nitro_loop_near, STT_FUNC
_coop_nitro_loop_near:
    P3.L = _g_coop_fast_nitro; P3.H = _g_coop_fast_nitro;   /* R7 / P3 / CC are free here: the stock loop sets */
    R7 = [P3];                                               /* each before reading it */
    CC = R7 == 0;
    IF CC JUMP 1f;
#ifdef OP1R_COOP_HITS
    P3.L = _coop_hits + 16; P3.H = _coop_hits + 16; R7 = [P3]; R7 += 1; [P3] = R7;   /* op1r test builds: the rewrite ran */
#endif
    P3.L = _coop_nitro_loop; P3.H = _coop_nitro_loop;        /* region A is beyond JUMP.L reach of the stock */
    JUMP (P3);
1:  P3.L = _stock_nitro_loop_top; P3.H = _stock_nitro_loop_top;   /* the replaced LSETUP, by hand */
    LT1 = P3;
    P3.L = _stock_nitro_loop_bottom; P3.H = _stock_nitro_loop_bottom;
    LB1 = P3;
    LC1 = P2;                                                /* P2 = 0x40 */
    JUMP.L _stock_nitro_loop_top;
    .size _coop_nitro_loop_near, .-_coop_nitro_loop_near

    .text
    .align 4
    .global _coop_nitro_loop
    .type   _coop_nitro_loop, STT_FUNC
_coop_nitro_loop:
    HIT _g_nitro_hit_calls
    M0 = R4;                                   /* g2, for the second loop */
    B1 = I1;                                   /* 0xFF801B50 */
    P4 = P0;                                   /* this */
    I3 = I0;                                   /* the input */
    R5 = 0x2002 (Z);
    P1 = R5;
    P5 = P5 + P1;                              /* T + 0x2002 */
    R7.H = 0x7fff;
    R7.L = 0x4000;
    R4 = R0 >>> 20;                            /* s0's index (loop 1's first packet, rotated) */
    LSETUP (a_top, a_bot) LC1 = P2;
a_top:                                         /* R0 = s0, R4 = s0 >>> 20 */
    LOOKUP_REST R0, R4, "R2.L = W[I3++]"         /* R2.L = x */
    R3.H = R7.H - R3.L (S) || R1 = [FP + 0x10] || NOP;      /* k1 */
    A0 = R3.L * R4.L;
    R4 = (A0 += R3.H * R5.L);                  /* A = S(s0) */
    R2 = R2.L * R7.L (IS);                     /* x << 14 */
    R4 = R4 - R2 (S);
    R4 = R4 << 2 (S);
    MUL32 R4, R1, R4
    R2 = R2 - R4 (S);                          /* u */
    R4 = R2 >>> 20;
    LOOKUP_REST R2, R4
    R3.H = R7.H - R3.L (S) || R1 = [FP - 0x14] || NOP;      /* c */
    A0 = R3.L * R4.L;
    R0 = (A0 += R3.H * R5.L);                  /* F = S(u) */
    A1 = R1.L * R0.L (FU) || R2 = [P4 + 0x1c] || NOP;       /* acc = M(c, F); z_0 */
    A1 = A1 >> 16;
    A1 += R1.H * R0.L (M), A0 = R1.H * R0.H;
    A1 += R0.H * R1.L (M);
    A1 = A1 >>> 15;
    R5 = (A0 += A1);
    STAGE1 0x10, 0x1c, 0x20
    STAGE1 0x0c, 0x20, 0x24
    STAGE1 0x08, 0x24, 0x28
    STAGE1 0x04, 0x28, 0x2c
    R5 = R5 << 2 (S);
    R4 = R5 >>> 20;
    LOOKUP_REST R5, R4
    R3.H = R7.H - R3.L (S);
    A0 = R3.L * R4.L;
    R2 = (A0 += R3.H * R5.L);                  /* S(sat(acc << 2)) */
a_bot:
    R4 = R0 >>> 20 || NOP || [I1++] = R2;      /* the next s0's index; the buffer word */

    [P4 + 0x4] = R0;                           /* s0 (0x019A6688) */
    R6 = M0;                                   /* g2 */
    I1 = B1;
    I2 = I0;
    R0 = [P4 + 0x8];                           /* s2 */
    R4 = R0 >>> 20;
    LOOKUP_REST R0, R4
    R3.H = R7.H - R3.L (S) || NOP || R2 = [I1++];           /* v_0 */
    A0 = R3.L * R4.L;
    R4 = (A0 += R3.H * R5.L);                  /* G = S(s2) */
    R1 = R4 - R2 (S) || R5 = [FP + 0xc] || NOP;             /* k2 */
    LSETUP (b_top, b_bot) LC1 = P2;
b_top:                                         /* R1 = sat(G - v), R2 = v, R5 = k2 */
    R1 = R1 << 2 (S);
    MUL32 R1, R5, R1
    R2 = R2 - R1 (S);                          /* w */
    R4 = R2 >>> 20;
    LOOKUP_REST R2, R4
    R3.H = R7.H - R3.L (S) || R2 = [P4 + 0x2c] || NOP;      /* z_0 */
    A0 = R3.L * R4.L;
    R0 = (A0 += R3.H * R5.L);                  /* H = S(w) */
    STAGE2 0, 0x30
    STAGE2 0x2c, 0x34
    STAGE2 0x30, 0x38
    STAGE2 0x34
    R4 = R0 >>> 20 || [P4 + 0x38] = R0 || NOP;
    LOOKUP_REST R0, R4, "R2 = [I1++]"            /* the next v (one past the buffer on the last pass: L1, read only) */
    R3.H = R7.H - R3.L (S);
    A0 = R3.L * R4.L;
    R4 = (A0 += R3.H * R5.L);                  /* S(y_3): this output and the next G */
b_bot:
    R1 = R4 - R2 (S) || R5 = [FP + 0xc] || W[I2++] = R4.H;

    P1 = P4;                                   /* R0 = y_3 */
    P0.L = _stock_nitro_loop_back; P0.H = _stock_nitro_loop_back;
    JUMP (P0);
    .size _coop_nitro_loop, .-_coop_nitro_loop
