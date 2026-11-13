/* The stock spring effect's per-sample loop rewritten (performance, 2026-11-04).
 *
 * spring's stereo render 0x01A32FFC (LINK 0x40, seven registers pushed: FP = SP + 0x5C; R0 = this, R1 = L, R2 = R,
 * [FP+0x14] = the parameters) sets up its coefficients and then runs one 64-sample LC0 loop 0x01A3310E..0x01A33694
 * (the LSETUP at 0x01A3310A has its end above the start: objdump prints it wrong, loopscan cannot parse it). The loop
 * has no branches and no calls: per sample four one-pole stages with the stock 32 x 32 multiply (states this[4],
 * this[8], this[0x18], this[0x28]), nine delay lines in the tank (T = [FP-0x10], coop's own tank per object through
 * coop_tank_spring_render) each with a one-pole state (this[0xc..0x24], this[0x2c..0x34]), four halfword feedback
 * values (this[0x38..0x3e]) and nine write indices (this[0x40..0x60]); the wet pair is added to L / R in place.
 * Line k (base T + off, len samples, tap d samples back): old = W[w], tap = W[w - 2d] (+ 2 len below the base),
 * f = sat(m * old + tap) (m = R2.L, or g8 = [FP-8].L on lines 3, 5, 6, 9), a = f * R2.H, s' = sat(sat(s + R3.L * a) -
 * R3.L * s.H), c = gf * s'.H (gf = [FP+8].L), W[w] = sat(r + c) with r the running R3.H; w = (w + 1) mod len.
 *
 *   line   off     len    d      idx          line   off     len    d      idx          line   off     len    d
 *   1      0x0000  0x17e   89    this+0x40    4      0x3690  0xd5a   977   this+0x4c    7      0x8dd0  0x254   199
 *   2      0x02fc  0x4f8  233    this+0x44    5      0x5144  0x738  1187   this+0x50    8      0x9278  0xda2   521
 *   3      0x0cec  0x14d2 1597   this+0x48    6      0x5fb4  0x170e 1493   this+0x54    9      0xadbc  0x1af2 2207
 *
 * The stock spends most of each sample on index bookkeeping: per line it reloads the index from the object, forms
 * both addresses with a compare for the tap's wrap, increments the index with a compare and stores it (about 19
 * instructions a line), and keeps its temporaries in the frame. Here the 18 read / write pointers live in coop's frame
 * as addresses, loaded and stored in the parallel slots and post-incremented; the block is cut into segments in which
 * no pointer reaches its line's end (a pointer wraps at most once a block, every line is longer than 64 samples, so
 * a block has one segment or a few, about 1.8 on average), and the wraps are done between segments. Every
 * multiply, add, negate and saturation on the signal is the stock's instruction with the stock's operands and order;
 * the products x * c with x = (16-bit) << 16 drop the stock's two zero terms (A1 = x.L * c.L = 0, c.H * x.L = 0).
 * 162 packets a sample against the stock's 393.
 *
 * Hoisted (invariant in the stock loop; nothing in it stores to them, as words or halves): c = R7 (= [0xFF801F50 + 4
 * k], set before the loop), R2 (from 0x01DE624C), R3.L (W[0x0106A5EC + 2 k]), g4 = [FP-4].L, gb = [FP-0x14].L, g8 =
 * [FP-8].L, gf = [FP+8].L, T = [FP-0x10]. The stock loop's stores go to the object, the tank, I0 / I1, and its scratch
 * [FP-0xc] (W[SP+0x50]), [SP+0x1c], [SP+0x20] / W[SP+0x20] / W[SP+0x22], [FP+0xc], [FP+0x10] (the caller's argument
 * homes), none read after the loop. The code after the loop (0x01A33696) reads only P0 = [SP+0x24] (this; loaded inside
 * the stock loop) and the object; the epilogue restores R4-R7 / P3-P5.
 *
 * Entry: JUMP.L from 0x01A3310A (the LSETUP, the only way into the loop) to coop_spring_loop_near. There: I0 = L, I1 =
 * R, P2 = this + 0x28, P4 = this + 4, P5 = this + 0x38, P3 = 64, R2 / R3 / R7 as above. Exit: JUMP 0x01A33696 with
 * P0 = this. Uses LC0 / LT0 / LB0 (as the stock), I2 / I3 (L registers stay 0), a LINK 0x80 frame of its own.
 * The stock loop runs instead when g_coop_fast_spring = 0, when L and R are the same buffer, or when an index is out
 * of range. */

/* coop's frame (FP = SP + 8 after the LINK); [FP-F_G4] is also read as a halfword through I2 */
#define F_G4    0x04
#define F_GB    0x08
#define F_TC    0x0c
#define F_TE    0x10
#define F_TF    0x14
#define F_TG    0x18
#define F_TI    0x1c
#define F_TJ    0x20
#define F_W1    0x24
#define F_T1    0x28
#define F_W2    0x2c
#define F_T2    0x30
#define F_W3    0x34
#define F_T3    0x38
#define F_W4    0x3c
#define F_T4    0x40
#define F_W5    0x44
#define F_T5    0x48
#define F_W6    0x4c
#define F_T6    0x50
#define F_W7    0x54
#define F_T7    0x58
#define F_W8    0x5c
#define F_T8    0x60
#define F_W9    0x64
#define F_T9    0x68
#define F_T     0x6c       /* the tank */
#define F_REM   0x70       /* samples left in the block */
#define F_K5    0x74       /* R5 in the loop: g8 high, gf low */
#define F_THIS  0x78       /* this + 0x38 */

/* test builds (-DCOOP_SPRING_CHECK): count events. Uses P1 / R1. */
.macro HIT name
#ifdef COOP_SPRING_CHECK
    P1.L = \name; P1.H = \name;
    R1 = [P1];
    R1 += 1;
    [P1] = R1;
#endif
.endm

/* the index at [P5 + idxoff] must be below len (else the stock loop runs) */
.macro VALID idxoff, len
    R0 = [P5 + \idxoff\()];
    R1 = \len (Z);
    CC = R0 < R1 (IU);
    IF !CC JUMP 9f;
.endm

/* the line's two pointers from its index: P1 = this + 0x38, R5 = T. Uses R0, R1, R4. */
.macro LINE_INIT idxoff, off, len, d, wslot, tslot
    R0 = [P1 + \idxoff\()];
    R1 = \off (Z);
    R1 = R1 + R5;                  /* base */
    R4 = R0 << 1;
    R4 = R4 + R1;
    [FP - \wslot\()] = R4;         /* w = base + 2 idx */
    R4 = \d (Z);
    R0 = R0 - R4;
    CC = R0 < 0;
    R4 = \len (Z);
    R4 = R0 + R4;
    IF CC R0 = R4;                 /* (idx - d) mod len, as the stock's compare */
    R0 = R0 << 1;
    R0 = R0 + R1;
    [FP - \tslot\()] = R0;
.endm

/* a pointer at its line's end goes back to the base; R5 = min(R5, bytes to the end). R6 = T. Uses R0, R1, R4. */
.macro FIXMIN slot, endoff, len2
    R0 = [FP - \slot\()];
    R1 = R0 - R6;
    R4 = \endoff (Z);
    R4 = R4 - R1;
    CC = R4 == 0;
    IF !CC JUMP 1f;
    HIT _g_spring_hit_wrap
    R1 = \len2 (Z);
    R0 = R0 - R1;
    [FP - \slot\()] = R0;
    R4 = R1;
1:  R5 = MIN(R5, R4);
.endm

/* the write index back to the object: P1 = this + 0x38, R6 = T. Uses R0, R1. */
.macro FINAL_IDX idxoff, wslot, off, len
    R0 = [FP - \wslot\()];
    R0 = R0 - R6;
    R1 = \off (Z);
    R0 = R0 - R1;
    R0 >>= 1;
    R1 = \len (Z);
    CC = R0 == R1;
    R1 = 0;
    IF CC R0 = R1;
    [P1 + \idxoff\()] = R0;
.endm

    .section .near, "ax"
    .global _coop_spring_loop_near
    .type   _coop_spring_loop_near, STT_FUNC
_coop_spring_loop_near:
    P0.L = _g_coop_fast_spring; P0.H = _g_coop_fast_spring;   /* R0 / P0 / CC are free here: the stock loop sets */
    R0 = [P0];                                                 /* each before reading it */
    CC = R0 == 0;
    IF CC JUMP _coop_spring_loop_stock;
#ifdef OP1R_COOP_HITS
    P0.L = _coop_hits + 24; P0.H = _coop_hits + 24; R0 = [P0]; R0 += 1; [P0] = R0;   /* op1r test builds: the rewrite ran */
#endif
    P0.L = _coop_spring_loop; P0.H = _coop_spring_loop;        /* region A is beyond JUMP.L reach of the stock */
    JUMP (P0);
    .global _coop_spring_loop_stock
_coop_spring_loop_stock:                                       /* the replaced LSETUP, by hand */
    P0.L = _stock_spring_loop_top; P0.H = _stock_spring_loop_top;
    LT0 = P0;
    P0.L = _stock_spring_loop_bottom; P0.H = _stock_spring_loop_bottom;
    LB0 = P0;
    LC0 = P3;                                                  /* P3 = 64 */
    JUMP.L _stock_spring_loop_top;
    .size _coop_spring_loop_near, .-_coop_spring_loop_near

    .text
    .align 4
    .global _coop_spring_loop
    .type   _coop_spring_loop, STT_FUNC
_coop_spring_loop:
    HIT _g_spring_hit_calls
    R0 = I0;
    R1 = I1;
    CC = R0 == R1;
    IF CC JUMP 9f;
    VALID 0x08, 0x17e
    VALID 0x0c, 0x4f8
    VALID 0x10, 0x14d2
    VALID 0x14, 0xd5a
    VALID 0x18, 0x738
    VALID 0x1c, 0x170e
    VALID 0x20, 0x254
    VALID 0x24, 0xda2
    VALID 0x28, 0x1af2
    JUMP 8f
9:  HIT _g_spring_hit_stock
    P0.L = _coop_spring_loop_stock; P0.H = _coop_spring_loop_stock;
    JUMP (P0);                                 /* nothing changed yet: R0, R1, P0, P1, CC are free in the stock loop */

8:  LINK 0x80;
    P0 = [FP];                                 /* the stock frame */
    R0 = [P0 + -0x4];
    [FP - F_G4] = R0;                          /* g4 (low half) */
    R0 = [P0 + -0x14];
    [FP - F_GB] = R0;                          /* gb */
    R0 = [P0 + -0x8];
    R1 = [P0 + 0x8];
    R5 = PACK (R0.L, R1.L);                    /* g8 : gf */
    [FP - F_K5] = R5;
    R5 = [P0 + -0x10];                         /* T */
    [FP - F_T] = R5;
    [FP - F_THIS] = P5;
    I3 = P5;                                   /* this + 0x38: W[I3] = this[0x38] */
    R0 = FP;
    R0 += -F_G4;
    I2 = R0;                                   /* W[I2] = g4 */
    P1 = P5;
    LINE_INIT 0x08, 0x0000, 0x17e,   89, F_W1, F_T1
    LINE_INIT 0x0c, 0x02fc, 0x4f8,  233, F_W2, F_T2
    LINE_INIT 0x10, 0x0cec, 0x14d2, 1597, F_W3, F_T3
    LINE_INIT 0x14, 0x3690, 0xd5a,  977, F_W4, F_T4
    LINE_INIT 0x18, 0x5144, 0x738, 1187, F_W5, F_T5
    LINE_INIT 0x1c, 0x5fb4, 0x170e, 1493, F_W6, F_T6
    LINE_INIT 0x20, 0x8dd0, 0x254,  199, F_W7, F_T7
    LINE_INIT 0x24, 0x9278, 0xda2,  521, F_W8, F_T8
    LINE_INIT 0x28, 0xadbc, 0x1af2, 2207, F_W9, F_T9
    P5 = 2;
    R0 = 64;
    [FP - F_REM] = R0;
    R6 = R5;
    R5 = 128;

seg_min:                                       /* R5 = bytes left, R6 = T */
    FIXMIN F_W1, 0x02fc, 0x02fc
    FIXMIN F_T1, 0x02fc, 0x02fc
    FIXMIN F_W2, 0x0cec, 0x09f0
    FIXMIN F_T2, 0x0cec, 0x09f0
    FIXMIN F_W3, 0x3690, 0x29a4
    FIXMIN F_T3, 0x3690, 0x29a4
    FIXMIN F_W4, 0x5144, 0x1ab4
    FIXMIN F_T4, 0x5144, 0x1ab4
    FIXMIN F_W5, 0x5fb4, 0x0e70
    FIXMIN F_T5, 0x5fb4, 0x0e70
    FIXMIN F_W6, 0x8dd0, 0x2e1c
    FIXMIN F_T6, 0x8dd0, 0x2e1c
    FIXMIN F_W7, 0x9278, 0x04a8
    FIXMIN F_T7, 0x9278, 0x04a8
    FIXMIN F_W8, 0xadbc, 0x1b44
    FIXMIN F_T8, 0xadbc, 0x1b44
    FIXMIN F_W9, 0xe3a0, 0x35e4
    FIXMIN F_T9, 0xe3a0, 0x35e4
    R5 >>= 1;                                  /* samples in this segment (1..64) */
    R0 = [FP - F_REM];
    CC = R5 == R0;
    IF CC JUMP 2f;
    HIT _g_spring_hit_split
2:  R0 = R0 - R5;
    [FP - F_REM] = R0;
    P0.L = lp_top; P0.H = lp_top;
    LT0 = P0;
    P0.L = lp_bot; P0.H = lp_bot;
    LB0 = P0;
    LC0 = R5;
    R5 = [FP - F_K5];
    R0 = W[P2 + 0x16] (X);                     /* h3 */
    R4 = [P4];                                 /* s0 */
    R6 = -R4 (S);                              /* lp_top's two packets without the output stores */
    A1 = R6.L * R7.L (FU) || P1 = [FP - F_T1] || NOP;
    P3 = [FP - F_W2];
    P0 = [FP - F_W9];                          /* lp_entry stores it back */
    JUMP lp_entry;

/* The loop. Loop-carried: R0.L = h3 (W[this+0x3e]), R4 = s0, R1 = the previous sample's output pair (stored in the
 * first two packets). R5 = g8 : gf, P5 = 2, I2 -> g4, I3 -> this[0x38]. Write pointers alternate P0 / P3, the tap
 * pointer is P1. Value names as in the stock: x = g4 * h, s' the new state, a the filtered line sample, c = gf * s'.H. */
    .align 4
lp_top:
    R6 = -R4 (S) || P1 = [FP - F_T1] || W[I0++] = R1.L;
    A1 = R6.L * R7.L (FU) || P3 = [FP - F_W2] || W[I1++] = R1.H;
lp_entry:
    /* The device refuses a parallel P-register store beside an accumulator shift (EXCAUSE 0x22 here in p5b and p5c,
     * 2026-11-04; op1emu runs it; the stock code never pairs them, tools/asm_lint.py): three plain instructions. */
    A1 = A1 >> 16;
    [FP - F_W9] = P0;
    R1.H = W[I2];                                                       /* g4 */
    A1 += R6.H * R7.L (M), A0 = R6.H * R7.H || P0 = [FP - F_W1] || R1.L = W[I3];   /* h0 */
    A1 += R7.H * R6.L (M);
    A1 = A1 >>> 15;
    R6 = (A0 += A1);                           /* M(-s0) */
    R0.H = R1.H * R0.L;                        /* x0 = g4 * h3 */
    R1.L = R1.H * R1.L;                        /* x1 = g4 * h0 */
    A1 = R0.H * R7.L (M), A0 = R0.H * R7.H;
    A1 = A1 >>> 15;
    R0 = (A0 += A1);                           /* M(x0) */
    R0 = R4 + R0 (S);
    R0 = R0 + R6 (S) || R6 = [FP - F_GB] || NOP;                       /* s0' */
    /* stage 1 */
    R4 = -R0 (V) || [P4] = R0 || R1.H = W[I0];                          /* R4.H = -s0'.H; inL */
    R1.H = R1.H * R6.L || R0 = [P4 + 0x4] || R4.L = W[I1];              /* inL * gb; s1; inR */
    R4.L = R4.L * R6.L;                                                 /* inR * gb */
    R4.H = R4.H + R1.H (S) || R6.L = W[P0] || NOP;                      /* old1 */
    R4.L = R4.H + R4.L (S) || R6.H = W[P1 ++ P5] || NOP;                /* tC; tap1 */
    A1 = R1.L * R7.L (M), A0 = R1.L * R7.H || [FP - F_TC] = R4 || NOP;
    A1 = A1 >>> 15;                                                     /* (no P store beside a shift: see lp_entry) */
    [FP - F_T1] = P1;
    R1 = (A0 += A1) || P1 = [FP - F_T2] || NOP;                         /* M(x1) */
    R1 = R0 + R1 (S);
    R0 = -R0 (S);
    A1 = R0.L * R7.L (FU);
    A1 = A1 >> 16;
    A1 += R0.H * R7.L (M), A0 = R0.H * R7.H;
    A1 += R7.H * R0.L (M);
    A1 = A1 >>> 15;
    R0 = (A0 += A1);
    R0 = R1 + R0 (S);                                                   /* s1' */
    R3.H = R4.L - R0.H (S) || [P4 + 0x4] = R0 || NOP;                   /* r = tC - s1'.H */
    /* line 1 */
    R6.L = R2.L * R6.L || R4 = [P4 + 0x8] || NOP;                       /* s2 */
    R6.L = R6.L + R6.H (S);
    R1.L = R6.L * R2.H;                                                 /* a1 */
    R0 = R3.L * R1.L || R6.L = W[P3] || NOP;                            /* old2 */
    R0 = R4 + R0 (S) || R6.H = W[P1 ++ P5] || NOP;                      /* tap2 */
    R4 = R3.L * R4.H || [FP - F_T2] = P1 || NOP;
    R0 = R0 - R4 (S) || P1 = [FP - F_T3] || NOP;                        /* s2' */
    R1.H = R5.L * R0.H || [P4 + 0x8] = R0 || NOP;                       /* b1 */
    R4.L = R3.H + R1.H (S) || R0 = [P4 + 0xc] || NOP;                   /* write1; s3 */
    /* line 2 */
    R6.L = R2.L * R6.L || W[P0++] = R4 || NOP;
    R6.L = R6.L + R6.H (S) || [FP - F_W1] = P0 || NOP;
    R6.H = R6.L * R2.H || P0 = [FP - F_W3] || NOP;                      /* a2 */
    R1.L = R1.L + R6.H (S);                                             /* a1 + a2 */
    R4 = R3.L * R6.H;
    R4 = R0 + R4 (S);
    R0 = R3.L * R0.H;
    R4 = R4 - R0 (S);                                                   /* s3' */
    R6.L = R5.L * R4.H || [P4 + 0xc] = R4 || NOP;                       /* c2 */
    R0.L = R3.H + R6.L (S) || R4.L = W[P0] || NOP;                      /* write2; old3 */
    R1.H = R1.H + R6.L (S) || R4.H = W[P1 ++ P5] || NOP;                /* tE = b1 + c2; tap3 */
    R6 = -R6 (V) || W[P3++] = R0 || NOP;                                /* tD = -c2 */
    /* line 3 */
    R4.L = R5.H * R4.L || [FP - F_W2] = P3 || NOP;
    R4.L = R4.L + R4.H (S) || [FP - F_T3] = P1 || NOP;
    R4.L = R4.L * R2.H || R0 = [P4 + 0x10] || NOP;                      /* a3; s4 */
    R1.L = R4.L + R1.L (S) || P1 = [FP - F_T4] || NOP;                  /* h0' = a3 + (a1 + a2) */
    R4 = R3.L * R4.L || W[P2 + 0x10] = R1 || NOP;
    R4 = R0 + R4 (S) || [FP - F_TE] = R1 || NOP;
    R0 = R3.L * R0.H || R1 = W[P2 + 0x12] (X) || NOP;                   /* h1 */
    R4 = R4 - R0 (S) || NOP || R1.H = W[I2];                            /* s4'; g4 */
    R0.H = R5.L * R4.H || [P4 + 0x10] = R4 || NOP;                      /* c4 */
    R3.H = R3.H + R0.H (S) || R4 = [P4 + 0x14] || NOP;                  /* r += c4; s5 */
    R6.L = R0.H + R6.L (S) || W[P0 ++ P5] = R3.H || NOP;                /* tF = c4 + tD; write3 */
    /* stage 5 */
    R0.H = R1.H * R1.L || [FP - F_TF] = R6 || NOP;                      /* x5 = g4 * h1 */
    A1 = R0.H * R7.L (M), A0 = R0.H * R7.H || [FP - F_W3] = P0 || NOP;
    A1 = A1 >>> 15 || P3 = [FP - F_W4] || NOP;
    R0 = (A0 += A1) || R1 = [FP - F_TC] || NOP;
    R0 = R4 + R0 (S);
    R4 = -R4 (S);
    A1 = R4.L * R7.L (FU) || R6.L = W[P3] || NOP;                       /* old4 */
    A1 = A1 >> 16 || R6.H = W[P1 ++ P5] || NOP;                         /* tap4 */
    A1 += R4.H * R7.L (M), A0 = R4.H * R7.H || [FP - F_T4] = P1 || NOP;
    A1 += R7.H * R4.L (M) || P1 = [FP - F_T5] || NOP;
    A1 = A1 >>> 15;
    R4 = (A0 += A1);
    R4 = R0 + R4 (S);                                                   /* s5' */
    R3.H = R1.L - R4.H (S) || [P4 + 0x14] = R4 || NOP;                  /* r = tC - s5'.H */
    /* line 4 */
    R6.L = R2.L * R6.L || R0 = [P4 + 0x18] || NOP;                      /* s6 */
    R6.L = R6.L + R6.H (S) || P0 = [FP - F_W5] || NOP;
    R6.H = R6.L * R2.H;                                                 /* a4 */
    R4 = R3.L * R6.H;
    R4 = R0 + R4 (S) || R1.H = W[P1 ++ P5] || NOP;                      /* tap5 */
    R0 = R3.L * R0.H || R6.L = W[P0] || NOP;                            /* old5 */
    R4 = R4 - R0 (S) || [FP - F_T5] = P1 || NOP;                        /* s6' */
    R1.L = R5.L * R4.H || [P4 + 0x18] = R4 || NOP;                      /* c6 */
    R0.L = R3.H + R1.L (S) || P1 = [FP - F_T6] || NOP;                  /* write4 */
    R4 = -R1 (V) || W[P3++] = R0 || NOP;                                /* tG = -c6 */
    /* line 5 */
    R6.L = R5.H * R6.L || [FP - F_W4] = P3 || NOP;
    R6.L = R6.L + R1.H (S) || R0 = [P4 + 0x1c] || NOP;                  /* s7 */
    R1.H = R6.L * R2.H || P3 = [FP - F_W6] || NOP;                      /* a5 */
    R6.H = R6.H + R1.H (S) || [FP - F_TG] = R4 || NOP;                  /* a4 + a5 */
    R4 = R3.L * R1.H || R6.L = W[P3] || NOP;                            /* old6 */
    R4 = R0 + R4 (S);
    R0 = R3.L * R0.H;
    R4 = R4 - R0 (S);                                                   /* s7' */
    R1.H = R5.L * R4.H || [P4 + 0x1c] = R4 || NOP;                      /* c7 */
    R0.L = R3.H + R1.H (S) || R4.H = W[P1 ++ P5] || NOP;                /* write5; tap6 */
    R1.L = R1.L + R1.H (S) || W[P0++] = R0 || NOP;                      /* c6 + c7 */
    /* line 6 */
    R6.L = R5.H * R6.L || [FP - F_W5] = P0 || NOP;
    R6.L = R6.L + R4.H (S) || R0 = [P4 + 0x20] || NOP;                  /* s8 */
    R4.H = R6.L * R2.H || [FP - F_T6] = P1 || NOP;                      /* a6 */
    R6.L = R4.H + R6.H (S) || P1 = [FP - F_T7] || NOP;                  /* h1' = a6 + (a4 + a5) */
    R4 = R3.L * R4.H || W[P2 + 0x12] = R6 || NOP;
    R4 = R0 + R4 (S) || P0 = [FP - F_W7] || NOP;
    R0 = R3.L * R0.H || R6 = [FP - F_TE] || NOP;
    R4 = R4 - R0 (S) || R0 = W[P2 + 0x14] (X) || NOP;                   /* s8'; h2 */
    R0.H = R5.L * R4.H || [P4 + 0x20] = R4 || R1.H = W[I2];             /* c8; g4 */
    R3.H = R3.H + R0.H (S) || R4 = [FP - F_TG] || NOP;                  /* r += c8 */
    R0.H = R0.H + R4.L (S) || W[P3 ++ P5] = R3.H || NOP;                /* c8 + tG; write6 */
    R6.H = R6.H + R0.H (S) || R4 = [FP - F_TF] || NOP;                  /* tI.H = tE + (c8 + tG) */
    R6.L = R4.L + R1.L (S) || [FP - F_W6] = P3 || NOP;                  /* tI.L = tF + (c6 + c7) */
    /* stage 9 */
    R0.H = R1.H * R0.L || R4 = [P2] || NOP;                             /* x9 = g4 * h2; s9 */
    A1 = R0.H * R7.L (M), A0 = R0.H * R7.H || [FP - F_TI] = R6 || NOP;
    A1 = A1 >>> 15 || R6.L = W[P0] || NOP;                              /* old7 */
    R0 = (A0 += A1) || R6.H = W[P1 ++ P5] || NOP;                       /* tap7 */
    R0 = R4 + R0 (S) || [FP - F_T7] = P1 || NOP;
    R4 = -R4 (S) || R1 = [FP - F_TC] || NOP;
    A1 = R4.L * R7.L (FU) || P1 = [FP - F_T8] || NOP;
    A1 = A1 >> 16 || P3 = [FP - F_W8] || NOP;
    A1 += R4.H * R7.L (M), A0 = R4.H * R7.H;
    A1 += R7.H * R4.L (M);
    A1 = A1 >>> 15;
    R4 = (A0 += A1);
    R4 = R0 + R4 (S);                                                   /* s9' */
    R3.H = R1.L - R4.H (S) || [P2] = R4 || NOP;                         /* r = tC - s9'.H */
    /* line 7 */
    R6.L = R2.L * R6.L || R0 = [P2 + 0x4] || NOP;                       /* s10 */
    R6.L = R6.L + R6.H (S);
    R6.H = R6.L * R2.H;                                                 /* a7 */
    R4 = R3.L * R6.H;
    R4 = R0 + R4 (S) || R1.H = W[P1 ++ P5] || NOP;                      /* tap8 */
    R0 = R3.L * R0.H || R6.L = W[P3] || NOP;                            /* old8 */
    R4 = R4 - R0 (S) || [FP - F_T8] = P1 || NOP;                        /* s10' */
    R1.L = R5.L * R4.H || [P2 + 0x4] = R4 || NOP;                       /* c10 */
    R0.L = R3.H + R1.L (S) || P1 = [FP - F_T9] || NOP;                  /* write7 */
    R4 = -R1 (V) || W[P0++] = R0 || NOP;                                /* tJ = -c10 */
    /* line 8 */
    R6.L = R2.L * R6.L || [FP - F_W7] = P0 || NOP;
    R6.L = R6.L + R1.H (S) || R0 = [P2 + 0x8] || NOP;                   /* s11 */
    R1.H = R6.L * R2.H || P0 = [FP - F_W9] || NOP;                      /* a8 */
    R6.H = R6.H + R1.H (S) || [FP - F_TJ] = R4 || NOP;                  /* a7 + a8 */
    R4 = R3.L * R1.H || R6.L = W[P0] || NOP;                            /* old9 */
    R4 = R0 + R4 (S);
    R0 = R3.L * R0.H;
    R4 = R4 - R0 (S);                                                   /* s11' */
    R1.H = R5.L * R4.H || [P2 + 0x8] = R4 || NOP;                       /* c11 */
    R0.L = R3.H + R1.H (S) || R4.H = W[P1 ++ P5] || NOP;                /* write8; tap9 */
    R1.L = R1.L + R1.H (S) || W[P3++] = R0 || NOP;                      /* c10 + c11 */
    /* line 9 */
    R6.L = R5.H * R6.L || [FP - F_W8] = P3 || NOP;
    R6.L = R6.L + R4.H (S) || R0 = [P2 + 0xc] || NOP;                   /* s12 */
    R4.H = R6.L * R2.H || [FP - F_T9] = P1 || NOP;                      /* a9 */
    R6.L = R4.H + R6.H (S);                                             /* h2' = a9 + (a7 + a8) */
    R4 = R3.L * R4.H || W[P2 + 0x14] = R6 || NOP;
    R4 = R0 + R4 (S) || R6 = [FP - F_TI] || NOP;
    R0 = R3.L * R0.H;
    R4 = R4 - R0 (S);                                                   /* s12' */
    R0.H = R5.L * R4.H || [P2 + 0xc] = R4 || NOP;                       /* c12 */
    R3.H = R3.H + R0.H (S) || R4 = [FP - F_TJ] || NOP;                  /* r += c12 */
    R1.H = R0.H + R4.L (S) || W[P0 ++ P5] = R3.H || NOP;                /* c12 + tJ; write9 */
    /* the wet pair, h3' and the output */
    R6 = R6 +|+ R1 (S) || R0 = W[P2 + 0x12] (X) || R4.H = W[I3];        /* wet; h1'; h0' */
    R0.L = R0.L + R4.H (S) || R4 = W[P2 + 0x14] (X) || R1.L = W[I0];    /* h1' + h0'; h2'; inL */
    R0.L = R4.L + R0.L (S) || R4 = [P4] || R1.H = W[I1];                /* h3'; the next s0; inR */
lp_bot:
    R1 = R6 +|+ R1 (S) || W[P2 + 0x16] = R0 || NOP;                     /* out = wet + in; h3' */

    W[I0++] = R1.L;                            /* the segment's last output, */
    W[I1++] = R1.H;
    [FP - F_W9] = P0;                          /* and its line-9 write pointer (both stored in lp_top's packets) */
    R0 = [FP - F_REM];
    CC = R0 == 0;
    IF CC JUMP 3f;
    R6 = [FP - F_T];
    R5 = R0 << 1;
    P0.L = seg_min; P0.H = seg_min;
    JUMP (P0);

3:  R6 = [FP - F_T];
    P1 = [FP - F_THIS];
    FINAL_IDX 0x08, F_W1, 0x0000, 0x17e
    FINAL_IDX 0x0c, F_W2, 0x02fc, 0x4f8
    FINAL_IDX 0x10, F_W3, 0x0cec, 0x14d2
    FINAL_IDX 0x14, F_W4, 0x3690, 0xd5a
    FINAL_IDX 0x18, F_W5, 0x5144, 0x738
    FINAL_IDX 0x1c, F_W6, 0x5fb4, 0x170e
    FINAL_IDX 0x20, F_W7, 0x8dd0, 0x254
    FINAL_IDX 0x24, F_W8, 0x9278, 0xda2
    FINAL_IDX 0x28, F_W9, 0xadbc, 0x1af2
    UNLINK;
    P0 = P2;
    P0 += -0x28;                               /* this, as the stock's [SP+0x24] */
    P1.L = _stock_spring_loop_back; P1.H = _stock_spring_loop_back;
    JUMP (P1);
    .size _coop_spring_loop, .-_coop_spring_loop
