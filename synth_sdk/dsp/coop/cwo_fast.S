/* The stock cwo effect's stereo render, its per-sample loop rewritten (performance, 2026-11-04).
 *
 * cwo stereo render 0x019450FC (class vtable 0x0190F594 +0x14, LINK 0x308, FP = SP + 0x348). Per block it builds four
 * 64-element stack arrays with __cxa_vec_ctor (0x01AA3862, element ctor 0x0194370E: one zero halfword, called through a
 * pointer 64 times per array), fills all of them with 0x019446E0 (two calls, each writing all 64 entries of two arrays
 * before anything reads them), then runs a 64-iteration software loop 0x019457D6..0x01945AC6 that CALLs six helpers per
 * sample and passes every value through the 0x308-byte frame:
 *   0x01943B6E  a two-state filter on the feedback pair W[this+0x10/0x12] (state this+0x24)
 *   0x0194444E  (twice) a cubic soft clip, y = 2x - k (2x)^3, times the gain W[0x01BA4E34]
 *   0x019438D0  the stereo delay line: write the pair at the index, read at idx - 2 (D >> 11 + 1 or + 3) with an
 *               allpass-style fractional interpolation (2 taps, or a 4-step run while the delay time moves fast)
 *   0x01943CA6  a 32-bit filter pair (state this+0x38)
 *   0x01943FB6  four allpass chains of 4 stages on 32-bit state, polyphase (state this+0x50, toggle [this+0x50])
 * and the output mix (out = 2 * (fb * W[this+0x1a] + in * W[this+0x1c]), saturated).
 *
 * coop_cwo_loop is that loop with every helper inlined. Every multiply, add and shift on the signal is the stock's, with
 * the stock's operands, operand order and mode (the 32 x 32 multiply sequences verbatim); what changed is where values
 * live (registers instead of the frame), the order of independent loads and stores (no two touch the same word), the
 * allpass stages unrolled with four row pointers so their state accesses fit the parallel slots, the per-block constants
 * hoisted, and the integer split of the delay time into whole samples and fraction written in fewer steps (the same
 * values for every D >= 1: i = max((D >> 11) - 1, 0), f = D - (i << 11)). The two debug asserts in 0x019438D0 (delay
 * out of range, fraction out of range: a message then an abort, 0x01A9F8EC) are not reproduced.
 *
 * Entry: JUMP.L from 0x019457D2 (`R2 = 0x40`, the only way into the loop) to coop_cwo_loop_near. There: P3 = SP + 0xAC
 * (array 0; arrays 1/2/3 at +0x80/+0x100/+0x180), P4 = 0x01BA4E34, P5 = this + 0x10, the ramp steps at
 * [SP+0x7C/0x84/0x80/0x78/0x2AC] (16-bit) and [SP+0x9C] (32-bit, also the delay-time step), the in-place I/O buffers at
 * [SP+0x2C0] (left) and [SP+0x2BC] (right). Exit: JUMP.L 0x01945AC8, which reads P4 (unchanged here) and frame words the
 * loop never writes ([SP+0x50/0x58/0x60/0x64/0x68/0x70/0x74/0xAC], [FP-0x38]); R4-R7 / P3 / P5 are restored by the
 * epilogue. Uses LC0 (the stock helpers do), LC1 (saved and restored), I0-I3 (L0-L3 = 0 by the ABI, as the stock helpers
 * assume), M1, and as scratch the frame's outgoing-argument words [SP+0x00..0x1F] (no calls are made) and the stock
 * loop's temporaries [FP-0x30..FP-0x08] (the code after the loop reads neither).
 * g_coop_fast_cwo = 0 runs the stock loop and the stock array constructors (A/B, kill switch). */

/* the stock 32 x 32 fractional multiply, d = a * b */
.macro MUL32 d, a, b
    A1 = \a\().L * \b\().L (FU);
    A1 = A1 >> 16;
    A1 += \a\().H * \b\().L (M), A0 = \a\().H * \b\().H;
    A1 += \b\().H * \a\().L (M);
    A1 = A1 >>> 15;
    \d = (A0 += A1);
.endm

/* One allpass step of 0x01943FB6: out = c * (in + z) - m (saturating, the stock's multiply), then m = in, z = out.
 * T holds z on entry (loaded by the step before) and out on exit; R5 is scratch. Riding in the parallel slots: lR =
 * [p() + o()], the next step's z, into this step's input register once m is stored; [sP + so] = sR, the previous step's
 * z; cl = [ca], a coefficient for a later step. (`\()` after an argument inside brackets: the assembler does not
 * substitute `[rg` otherwise.) */
.macro STEP in, T, c, mP, mo, lR=, lP=, lo=, sR=, sP=, so=, cl=, ca=
    \T = \in + \T (S) || R5 = [\mP\() + \mo\()] || NOP;
    A1 = \c\().L * \T\().L (FU) || [\mP\() + \mo\()] = \in || NOP;
  .ifnb \lR
    A1 = A1 >> 16 || \lR = [\lP\() + \lo\()] || NOP;
  .else
    A1 = A1 >> 16;
  .endif
  .ifnb \sR
    A1 += \c\().H * \T\().L (M), A0 = \c\().H * \T\().H || [\sP\() + \so\()] = \sR || NOP;
  .else
    A1 += \c\().H * \T\().L (M), A0 = \c\().H * \T\().H;
  .endif
  .ifnb \cl
    A1 += \T\().H * \c\().L (M) || NOP || \cl = [\ca\()];
  .else
    A1 += \T\().H * \c\().L (M);
  .endif
    A1 = A1 >>> 15;
    \T = (A0 += A1);
    \T = \T - R5 (S);
.endm

/* scratch: the frame's outgoing-argument words (SP + 0..0x23; the saved registers follow) */
#define K_W46    0x00      /* the W[this+0x14/0x16] ramp steps, packed (low: [SP+0x7C], high: [SP+0x84]) */
#define K_WCE    0x04      /* the W[this+0x1c/0x1e] ramp steps, packed (low: [SP+0x78], high: [SP+0x2AC]) */
#define K_WA     0x08      /* the W[this+0x1a] ramp step ([SP+0x80]) */
#define K_D      0x0c      /* the delay-time step ([SP+0x9C]) */
#define K_MASK   0x10      /* 0x1FFFF, the delay index mask */
#define K_BUF    0x14      /* [0x01AB8B9C], the delay buffer */
#define K_TAB    0x18      /* 0x01BA66AC, the interpolation coefficient table */
#define K_ADD    0x1c      /* 6 while the delay time moves fast (|step| >= 61), else 2 */
/* and the stock loop's temporaries below FP (the code after the loop reads [FP-0x38] and nothing else down here) */
#define S_LC1    -0x30
#define S_LT1    -0x2c
#define S_LB1    -0x28
#define S_DLR7   -0x20     /* 0x019438D0's [SP+0x30] */
#define S_DLCOEF -0x1c     /* 0x019438D0's [SP+0x34]: the interpolation coefficient pair */
#define S_P3     -0x10
#define S_LIM    -0x0c     /* 0x20000, the delay index wrap */
#define S_FAST   -0x08     /* 1 while the delay time moves fast */

    .section .near, "ax"
    .global _coop_cwo_loop_near
    .type   _coop_cwo_loop_near, STT_FUNC
_coop_cwo_loop_near:
    P0.L = _g_coop_fast_cwo; P0.H = _g_coop_fast_cwo;   /* R0-R4 / R6 / R7 / P0-P2 / CC are free here: the loop */
    R0 = [P0];                                           /* sets each before reading it */
    CC = R0 == 0;
    IF CC JUMP 1f;
#ifdef OP1R_COOP_HITS
    P0.L = _coop_hits + 4; P0.H = _coop_hits + 4; R0 = [P0]; R0 += 1; [P0] = R0;   /* op1r test builds: the rewrite ran */
#endif
    P0.L = _coop_cwo_loop; P0.H = _coop_cwo_loop;        /* region A is beyond JUMP.L reach of the stock */
    JUMP (P0);
1:  R2 = 0x40 (X);                                       /* the replaced instruction, then the stock loop */
    JUMP.L _stock_cwo_loop_entry;
    .size _coop_cwo_loop_near, .-_coop_cwo_loop_near

/* CALLed from the four __cxa_vec_ctor sites (0x0194569A / 0x019456B6 / 0x019456DC / 0x019456EE). The arrays they zero
 * are fully written by 0x019446E0 before any read, so with the switch on this returns at once; the code after each call
 * reads none of R0-R3 / P0-P2 / CC. */
    .global _coop_cwo_vec_ctor
    .type   _coop_cwo_vec_ctor, STT_FUNC
_coop_cwo_vec_ctor:
    P0.L = _g_coop_fast_cwo; P0.H = _g_coop_fast_cwo;
    R3 = [P0];                                           /* 0x01AA3862 loads R3 itself */
    CC = R3 == 0;
    IF !CC JUMP 1f;
    JUMP.L _stock_vec_ctor;
1:  RTS;
    .size _coop_cwo_vec_ctor, .-_coop_cwo_vec_ctor

    .text
    .align 4
    .global _coop_cwo_loop
    .type   _coop_cwo_loop, STT_FUNC
_coop_cwo_loop:
    R0 = LC1; [FP + S_LC1] = R0;
    R0 = LT1; [FP + S_LT1] = R0;
    R0 = LB1; [FP + S_LB1] = R0;
    R0 = [SP + 0x7c]; R1 = [SP + 0x84]; R0 = PACK (R1.L, R0.L); [SP + K_W46] = R0;
    R0 = [SP + 0x78]; R1 = [SP + 0x2ac]; R0 = PACK (R1.L, R0.L); [SP + K_WCE] = R0;
    R0 = [SP + 0x80]; [SP + K_WA] = R0;
    R0 = [SP + 0x9c]; [SP + K_D] = R0;
    R1 = -R0 (NS);                                       /* 0x019438D0: CC = 61 <= max(-step, step) */
    R0 = MAX (R1, R0);
    R1 = 0x3d (X);
    CC = R1 <= R0;
    R2 = CC;
    [FP + S_FAST] = R2;
    R2 <<= 2;
    R2 += 2;
    [SP + K_ADD] = R2;
    R0 = 0xffff (Z); BITSET (R0, 16); [SP + K_MASK] = R0;
    R0 = 0 (X); BITSET (R0, 17); [FP + S_LIM] = R0;
    P0.L = 0x8b9c; P0.H = 0x1ab;
    R0 = [P0]; [SP + K_BUF] = R0;
    R0.L = 0x66ac; R0.H = 0x1ba; [SP + K_TAB] = R0;
    R0 = [SP + 0x2c0]; I0 = R0;                          /* left in / out */
    R0 = [SP + 0x2bc]; I1 = R0;                          /* right in / out */
    I2.L = 0x668c; I2.H = 0x1ba;                         /* the allpass coefficients a_k, b_k (0x01943FB6) */
    I3.L = 0x669c; I3.H = 0x1ba;
    R0 = -12 (X); M1 = R0;                               /* the 4th load of each steps back to a_0 / b_0 */
    P0.L = .Lbody; P0.H = .Lbody;                        /* the body is beyond LSETUP's end reach (the assembler */
    LT1 = P0;                                            /* wraps that offset without a word), so the loop */
    P0.L = .Lbody_end; P0.H = .Lbody_end;                /* registers are set directly */
    LB1 = P0;
    P0 = 64 (X);
    LC1 = P0;
    JUMP .Lbody;

/* ---- out of line (here, within the branch reach of the body) ---- */
.Lflag:                                                  /* B[Q+0xc] set: a read before the start gives silence */
    CC = R7 < 0;
    IF !CC JUMP .Lcont;
    R0 = 0 (X);
    W[P5 + 0x20] = R0;
    W[P5 + 0x22] = R0;
    W[P5 + 0x1c] = R0;
    W[P5 + 0x1e] = R0;
    JUMP .Ltail;

.Lwrap:                                                  /* the index wrapped: 0, and the flag cleared */
    R0 = 0 (X);
    [P5 + 0x18] = R0;
    B[P5 + 0x24] = R0;
    JUMP .Lwrap_back;

.Lcomplex:                                               /* the fast-moving case (0x019439CE..0x01943A6C) */
    [FP + S_DLCOEF] = R5;
    R1.H = R1.L << 0;
    R3.H = R0.L << 0;
    R0.H = R3.H << 0;
    R4 = PACK (R2.L, R4.L);
    W[P5 + 0x20] = R1;
    W[P5 + 0x1c] = R2;
    W[P5 + 0x1e] = R4;
    R2.H = R4.L << 0;
    W[P5 + 0x22] = R0;
    R7 += 4;
    [FP + S_P3] = P3;
    P3 = P5;
    P3 += 0x1e;                                          /* &W[Q+6] (a high-half store takes no offset) */
    P1 = 4 (X);
    LSETUP (.Lc_b, .Lc_e) LC0 = P1;
.Lc_b:
    R6 = [SP + K_MASK];
    R5 = R7 & R6;
    R6 = R5;
    P0 = R6;
    R5 += 1;
    P1 = R5;
    R7 += 2;
    R6.H = R1.H - R2.L (S) || [FP + S_DLR7] = R7 || NOP;
    P0 = P2 + (P0 << 1);
    R7.H = R0.H - R4.L (S) || R5 = W[P0] (X) || NOP;
    P1 = P2 + (P1 << 1);
    R3.L = R5.L << 0 || R5 = W[P1] (X) || NOP;
    W[P5 + 0x20] = R2;
    R6.L = R3.L - R1.L (S);
    R0.L = R5.L << 0 || R5 = [FP + S_DLCOEF] || NOP;
    W[P5 + 0x22] = R4;
    R7.L = R0.L - R3.H (S);
    R5.H = R5.H * R6.H, R5.L = R5.L * R6.L || R6 = [FP + S_DLCOEF] || NOP;
    R7.H = R6.H * R7.H, R7.L = R6.L * R7.L;
    R1.L = R7.L + R7.H (S);
    R5.H = R5.L + R5.H (S);
    R6.H = R2.H + R1.L (S);
    R6.L = R4.H + R5.H (S);
    W[P3] = R6.H;
    R3.H = R4.L << 0 || W[P5 + 0x1c] = R6 || NOP;
    R1.L = R2.L << 0 || R7 = [FP + S_DLR7] || NOP;
    R4 = PACK (R1.H, R6.H);
    R2 = PACK (R0.H, R6.L);
    R1.H = R3.L << 0;
.Lc_e:
    R0.H = R0.L << 0;
    P3 = [FP + S_P3];
    JUMP .Ltail;

/* ---- the loop ---- */
.Lbody:
    /* ramps (0x019457E0..0x01945830): W[this+0x14/0x16/0x1a/0x1c/0x1e] += steps (S), [this+0x20] += step;
     * cE = the new W[this+0x1e] in R4.H */
    R2 = [P5 + 4];
    R3 = [SP + K_W46];
    R2.L = R2.L + R3.L (S) || R4 = [P5 + 0xc] || NOP;
    R2.H = R3.H + R2.H (S) || R3 = [SP + K_WCE] || NOP;
    R4.L = R3.L + R4.L (S) || [P5 + 4] = R2 || NOP;
    R4.H = R3.H + R4.H (S) || R2 = W[P5 + 0xa] (X) || NOP;
    R3 = [SP + K_WA];
    R2.L = R3.L + R2.L (S) || [P5 + 0xc] = R4 || NOP;
    R3 = [P5 + 0x10];
    R1 = [SP + K_D];
    R3 = R1 + R3 (NS) || W[P5 + 0xa] = R2 || NOP;
    [P5 + 0x10] = R3;

    /* 0x01943B6E: state W[this+0x24/0x26], inputs W[this+0x10/0x12], coefficient cE -> o1 R6.L, o2 R6.H */
    R1 = W[P5 + 0x14] (X);
    R0.H = R1.L * R4.H || R2 = W[P5 + 0x16] (X) || NOP;
    R2.H = R1.L + R0.H (S) || R1 = W[P5] (X) || NOP;
    R0.H = R2.L * R4.H || R3 = W[P5 + 2] (X) || NOP;
    R0.H = R2.L + R0.H (S);
    R1.L = R2.H - R1.L (S);
    R1.H = R0.H - R3.L (S);
    R1.L = R1.L * R4.H;
    R1.H = R1.H * R4.H;
    R1.L = R2.H + R1.L (S);
    R1.H = R0.H + R1.H (S);
    R2.L = R1.L * R4.H;
    R0.L = R1.H * R4.H;
    R3.L = R1.H + R0.L (S) || NOP || R4.L = W[I0];       /* in left */
    R2.L = R1.L + R2.L (S) || W[P5 + 0x16] = R3 || R4.H = W[I1];   /* in right */
    R0.L = R2.H + R1.L (S) || W[P5 + 0x14] = R2 || NOP;
    R6.L = R0.L + R2.L (S) || R7 = [P4 + 0x38] || NOP;   /* c38 = W[0x01BA4E6C] */
    R0.H = R0.H + R1.H (S) || R5 = [P5 + 4] || NOP;      /* w6 = W[this+0x16] in R5.H */
    R6.H = R0.H + R3.L (S) || R3 = W[P4] (X) || NOP;     /* g = W[0x01BA4E34] */

    /* the inputs plus the filtered feedback (0x0194587A..0x019458E4), each through 0x0194444E (k = W[0x01BA4E40])
     * -> y1g R6.L, y2g R6.H */
    R2.L = R4.L * R3.L;
    R2.H = R6.L * R5.H;
    R2.L = R2.L + R2.H (S);
    R2.H = R6.L * R7.L;
    R2.L = R2.L + R2.H (S);                              /* x1 */
    R1.L = R4.H * R3.L;
    R1.H = R6.H * R5.H;
    R0.H = R6.H * R7.L;
    R1.H = R1.L + R1.H (S) || R5 = [P4 + 0xc] || NOP;
    R2.H = R1.H + R0.H (S) || R7 = [P5 + 0x10] || NOP;   /* x2; the delay time */
    R0.H = R2.L + R2.L (S) || P2 = [SP + K_BUF] || NOP;
    R1.L = R5.L * R0.H || P1 = [P5 + 0x18] || NOP;       /* the delay index */
    R1.L = R1.L * R0.H || R4 = [SP + K_ADD] || NOP;
    R1.L = R1.L * R0.H;
    R0.H = R0.H - R1.L (S);
    R6.L = R0.H * R3.L;
    R0.H = R2.H + R2.H (S);
    R1.L = R5.L * R0.H;
    R1.L = R1.L * R0.H;
    R1.L = R1.L * R0.H;
    R0.H = R0.H - R1.L (S);
    R6.H = R0.H * R3.L;

    /* 0x019438D0, the delay line, object Q = this + 0x28 = P5 + 0x18: [Q] index, W[Q+4/6/8/0xa], B[Q+0xc] */
    R1 = 1 (X);
    R7 = MAX (R7, R1);                                   /* D = max([this+0x20], 1) */
    P1 = P2 + (P1 << 1);
    R0 = R7 >>> 11 || W[P1++] = R6 || NOP;               /* buf[idx] = y1g */
    R0 += -1;
    R2 = R2 - R2 (NS) || W[P1] = R6.H || NOP;            /* buf[idx + 1] = y2g */
    R0 = MAX (R0, R2) || R3 = [P5 + 0x18] || NOP;        /* i (whole samples) */
    R2 = R0 << 11;
    R5 = R7 - R2 (NS);                                   /* f (fraction, 0..0xFFF) */
    R0 = R0 + R4 (NS);
    R0 = R0 << 1;
    R7 = R3 - R0 (NS);                                   /* r = idx - 2 (i + 2 or 6) */
    R0 = B[P5 + 0x24] (Z);
    CC = R0 == 0;
    IF !CC JUMP .Lflag;
.Lcont:
    R1 = [SP + K_MASK];
    R6 = R7;
    R6 += 2;
    R3 = R7 & R1;
    R0 = R6 & R1;
    P1 = R3;
    P0 = R0;
    P1 = P2 + (P1 << 1);
    P0 = P2 + (P0 << 1);
    R2 = W[P1] (X);                                      /* L at r */
    R4 = W[P1 + 2] (X);                                  /* R at r */
    R1 = W[P0] (X);                                      /* L at r + 2 */
    R0 = W[P0 + 2] (X);                                  /* R at r + 2 */
    P1 = [SP + K_TAB];
    P0 = R5;
    P0 = P1 + (P0 << 2);
    R5 = [P0];                                           /* the coefficient pair for f */
    R3 = [FP + S_FAST];
    CC = R3 == 0;
    IF !CC JUMP .Lcomplex;
    /* the 2-tap case (0x01943AA2..0x01943AFA) */
    R6 += 2;
    R3 = [SP + K_MASK];
    R6 = R6 & R3;
    P1 = R6;
    P0 = P5;
    P0 += 0x18;
    P1 = P2 + (P1 << 1);
    R3 = W[P0 + 6] (X);
    R3.H = R0.L - R3.L (S) || R7 = W[P0 + 4] (X) || NOP;
    R1.H = R1.L - R7.L (S) || R6 = W[P1] (X) || NOP;     /* L at r + 4 */
    R0 = W[P0 + 8] (X);
    R1.L = R6.L - R0.L (S) || R0 = W[P1 + 2] (X) || NOP; /* R at r + 4 */
    R6 = W[P0 + 0xa] (X);
    R3.L = R0.L - R6.L (S) || W[P0 + 0xa] = R3 || NOP;
    R1.H = R5.H * R1.H, R1.L = R5.L * R1.L || W[P0 + 8] = R7 || NOP;
    R0.H = R5.H * R3.H, R0.L = R5.L * R3.L;
    R1.L = R1.L + R1.H (S);
    R0.L = R0.L + R0.H (S);
    R1.L = R2.L + R1.L (S);
    R0.L = R4.L + R0.L (S) || W[P0 + 4] = R1 || NOP;
    W[P0 + 6] = R0;
.Ltail:                                                  /* 0x01943A80: idx += 2, wraps at 0x20000 */
    R0 = [P5 + 0x18];
    R1 = [FP + S_LIM];
    R0 += 2;
    CC = R1 <= R0;
    [P5 + 0x18] = R0;
    IF CC JUMP .Lwrap;
.Lwrap_back:

    /* 0x01943CA6, object this + 0x38, inputs the delay outputs W[Q+4/6], coefficients [0x01BA4E50/54/4C]
     * -> outA R7.H, outB R0.H */
    P0 = P5;
    P0 += 0x28;
    R0 = W[P5 + 0x1c] (Z);
    R6 = R0 << 16 || R2 = [P4 + 0x1c] || NOP;
    R7 = [P0 + 8];
    A1 = R6.L * R2.L (FU) || [P0 + 8] = R6 || NOP;
    A1 = A1 >> 16 || R1 = [P4 + 0x20] || NOP;
    A1 += R6.H * R2.L (M), A0 = R6.H * R2.H || R3 = [P0] || NOP;
    A1 += R2.H * R6.L (M) || R0 = W[P5 + 0x1e] (Z) || NOP;
    A1 = A1 >>> 15;
    R6 = (A0 += A1) || R5 = [P0 + 0x10] || NOP;
    MUL32 R7, R7, R2
    A1 = R3.L * R1.L (FU);
    A1 = A1 >> 16;
    A1 += R3.H * R1.L (M), A0 = R3.H * R1.H;
    A1 += R1.H * R3.L (M);
    R0 <<= 16;
    A1 = A1 >>> 15;
    R7 = R6 + R7 (S);
    R6 = (A0 += A1);
    A1 = R0.L * R2.L (FU);
    A1 = A1 >> 16;
    A1 += R0.H * R2.L (M), A0 = R0.H * R2.H;
    R7 = R7 - R6 (S);
    A1 += R2.H * R0.L (M);
    R6 = R3 + R7 (S) || R3 = [P0 + 0xc] || NOP;
    A1 = A1 >>> 15 || [P0 + 0xc] = R0 || NOP;
    R0 = (A0 += A1);
    A1 = R3.L * R2.L (FU);
    A1 = A1 >> 16;
    A1 += R3.H * R2.L (M), A0 = R3.H * R2.H;
    A1 += R2.H * R3.L (M) || R2 = [P0 + 4] || NOP;
    A1 = A1 >>> 15;
    R3 = (A0 += A1);
    A1 = R2.L * R1.L (FU);
    A1 = A1 >> 16;
    A1 += R2.H * R1.L (M), A0 = R2.H * R1.H;
    R6 = R7 + R6 (S);
    A1 += R1.H * R2.L (M) || [P0] = R6 || NOP;
    R1 = R6 - R5 (S);
    A1 = A1 >>> 15;
    R3 = R0 + R3 (S) || R0 = [P4 + 0x18] || NOP;
    R7 = (A0 += A1);
    A1 = R1.L * R0.L (FU);
    R3 = R3 - R7 (S) || R7 = [P0 + 0x14] || NOP;
    A1 = A1 >> 16;
    R2 = R2 + R3 (S);
    A1 += R1.H * R0.L (M), A0 = R1.H * R0.H;
    R3 = R3 + R2 (S);
    A1 += R0.H * R1.L (M) || [P0 + 4] = R3 || NOP;
    R1 = R3 - R7 (S);
    A1 = A1 >>> 15;
    R2 = (A0 += A1);
    A1 = R1.L * R0.L (FU) || R4 = [P0 + 0x18] || NOP;    /* MUL32 R0, R1, R0; the toggle [this+0x50] */
    A1 = A1 >> 16;
    A1 += R1.H * R0.L (M), A0 = R1.H * R0.H;
    A1 += R0.H * R1.L (M);
    A1 = A1 >>> 15;
    R0 = (A0 += A1);
    R2 = R5 + R2 (S);
    R1 = R7 + R0 (S) || [P0 + 0x10] = R2 || NOP;
    R7 = R6 - R2 (S) || [P0 + 0x14] = R1 || NOP;
    R0 = R3 - R1 (S);

    /* 0x01943FB6, object this + 0x50: [+0] toggle t, [+4/8] the previous inputs, state words at
     * this + 0x9C + 0x20 j + 8 k + 4 t (row j = -2..5, stage k = 0..3). Rows -2/-1 via P1, 0/1 via P0, 2/3 via P2,
     * 4/5 via P3. Inputs X = outA << 16, Y = outB << 16, U / V = the previous ones -> oA R0, oB R1, oC R2, oD R3 */
    R7.L = 0;
    R0.L = 0;
    [FP + S_P3] = P3;
    P1 = R4;
    P1 = P0 + (P1 << 2);
    P1 += 0x24;                                          /* row -2 */
    P3 = 0x40 (X);
    P0 = P1 + P3;                                        /* row 0 */
    P2 = P0 + P3;                                        /* row 2 */
    P3 = P2 + P3;                                        /* row 4 */
    BITTGL (R4, 0);
    [P5 + 0x40] = R4;
    R1 = [P5 + 0x44];
    R2 = [P5 + 0x48];
    [P5 + 0x44] = R7;
    [P5 + 0x48] = R0;
    MNOP || R6 = [I2++] || R4 = [I3++];                  /* a_0, b_0 */
    R3 = [P0];
    /* stage 0: X R7, Y R0, U R1, V R2, T R3 */
    STEP R7, R3, R6, P1, 0x00, lR=R7, lP=P0, lo=0x20
    STEP R0, R7, R6, P1, 0x20, lR=R0, lP=P3, lo=0x00, sR=R3, sP=P0, so=0x00
    STEP R1, R0, R4, P2, 0x00, lR=R1, lP=P3, lo=0x20, sR=R7, sP=P0, so=0x20, cl=R6, ca=I2++
    STEP R2, R1, R4, P2, 0x20, lR=R2, lP=P0, lo=0x08, sR=R0, sP=P3, so=0x00
    /* stage 1: X R3, Y R7, U R0, V R1, T R2 */
    STEP R3, R2, R6, P1, 0x08, lR=R3, lP=P0, lo=0x28, sR=R1, sP=P3, so=0x20, cl=R4, ca=I3++
    STEP R7, R3, R6, P1, 0x28, lR=R7, lP=P3, lo=0x08, sR=R2, sP=P0, so=0x08
    STEP R0, R7, R4, P2, 0x08, lR=R0, lP=P3, lo=0x28, sR=R3, sP=P0, so=0x28, cl=R6, ca=I2++
    STEP R1, R0, R4, P2, 0x28, lR=R1, lP=P0, lo=0x10, sR=R7, sP=P3, so=0x08
    /* stage 2: X R2, Y R3, U R7, V R0, T R1 */
    STEP R2, R1, R6, P1, 0x10, lR=R2, lP=P0, lo=0x30, sR=R0, sP=P3, so=0x28, cl=R4, ca=I3++
    STEP R3, R2, R6, P1, 0x30, lR=R3, lP=P3, lo=0x10, sR=R1, sP=P0, so=0x10
    STEP R7, R3, R4, P2, 0x10, lR=R7, lP=P3, lo=0x30, sR=R2, sP=P0, so=0x30, cl=R6, ca="I2 ++ M1"
    STEP R0, R7, R4, P2, 0x30, lR=R0, lP=P0, lo=0x18, sR=R3, sP=P3, so=0x10
    /* stage 3: X R1, Y R2, U R3, V R7, T R0 */
    STEP R1, R0, R6, P1, 0x18, lR=R1, lP=P0, lo=0x38, sR=R7, sP=P3, so=0x30, cl=R4, ca="I3 ++ M1"
    STEP R2, R1, R6, P1, 0x38, lR=R2, lP=P3, lo=0x18, sR=R0, sP=P0, so=0x18
    STEP R3, R2, R4, P2, 0x18, lR=R3, lP=P3, lo=0x38, sR=R1, sP=P0, so=0x38
    STEP R7, R3, R4, P2, 0x38, sR=R2, sP=P3, so=0x18
    [P3 + 0x38] = R3;
    P3 = [FP + S_P3];

    /* the output (0x01945A0E..0x01945ABE): W[this+0x10/0x12] for the next sample, then the in-place outputs */
    R6 = W[P3] (X);                                      /* array 0 */
    R5 = W[P3 + 0x100] (X);                              /* array 2 */
    R6.H = R0.H * R5.L || R4 = W[P5 + 4] (X) || NOP;
    R5 = W[P3 + 0x180] (X);                              /* array 3 */
    R2.L = R1.H * R5.L;
    R5.L = R4.L * R2.H;
    R7.H = R4.L * R3.H;
    R4 = W[P3 + 0x80] (X);                               /* array 1 */
    R6.L = R5.L * R6.L;
    R6.L = R6.L - R6.H (S);
    R0.H = R7.H * R4.L || W[P5] = R6 || NOP;
    R2.L = R0.H - R2.L (S) || R0 = W[P5 + 0xa] (X) || NOP;
    R4.L = R6.L * R0.L || W[P5 + 2] = R2 || NOP;
    R0.H = R2.L * R0.L || R1 = W[P5 + 0xc] (X) || R5.L = W[I0];
    R5.H = W[I1];
    R1.H = R5.L * R1.L;
    R6.L = R4.L + R1.H (S);
    R3.L = R6.L + R6.L (S);
    R3.H = R5.H * R1.L;
    W[I0++] = R3.L;
    R1.L = R0.H + R3.H (S);
    R7.L = R1.L + R1.L (S);
    W[I1++] = R7.L;
.Lbody_end:
    P3 += 2;

    R0 = [FP + S_LT1]; LT1 = R0;
    R0 = [FP + S_LB1]; LB1 = R0;
    R0 = [FP + S_LC1]; LC1 = R0;
    P0.L = _stock_cwo_loop_back; P0.H = _stock_cwo_loop_back;
    JUMP (P0);
    .size _coop_cwo_loop, .-_coop_cwo_loop
