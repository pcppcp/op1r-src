/* The stock delay effect's stereo per-sample loop rewritten (performance, 2026-11-04).
 *
 * delay stereo render 0x0197DC42 (class vtable 0x0190FB8C + 0x14; no LINK: SP -= 0x18 and seven registers pushed, R0 =
 * this, R1 / R2 = the left / right in-place I/O buffers, [SP+0x40] = the parameter block). The effect is a varispeed
 * tape: a line of len 32-bit words (L = 44100 >> table[5 - (k0 >>> 11)], the line from the line table 0x01AB8BA8, coop's
 * own per object) and three 16.16 positions in the object, A (this[4], read), B (this[8], clear) and C (this[0xc],
 * write), all stepping by s = 2 max(k1, 0x21) and wrapping at L << 16. Per block the render reduces the positions,
 * writes the mono input m = (k3 / 2) * inR + (k3 / 2) * inL to the L1 buffer 0xFF801B50, then runs LC1
 * 0x0197DD4A..0x0197DF7A (64 samples, 160 instructions, 15 frame accesses each; two LC0 loops inside):
 *   - read: a 7-tap interpolating FIR at A: ph = A.L >>> 11, f = (A & 0x7ff) << 4, from the table 0x0106A730 (rows of
 *     32 halfwords, row k at + 64 k) e0 = T[31 - ph + 32 k], e1 = T[32 - ph + 32 k], c = (e0 * f, e1 * (0x7fff - f)),
 *     A0 += x_k.H * c.L, A0 += x_k.H * c.H (W32) over x_k = line[A.int + k] (circular);
 *   - out L = sat(t.H + inL), out R = sat(t.H + inR), in place (t = A0);
 *   - u = sat(sat(M(t, k2 << 16) << 1) + m), w = M(u, k1' << 16) (M = the stock's 32 x 32 multiply);
 *   - line[B.int] = 0;
 *   - write: the same FIR at C scattered: line[C.int + k] = sat(sat(line + M(w, c0 << 16)) + M(w, c1 << 16));
 *   - the three positions step.
 * Every 32 x 32 multiply here has a second operand whose low half is 0 (k2 << 16, k1' << 16, c << 16), so two of its six
 * terms are 0: M(a, b << 16) = sat((b * a.H << 1) + ((b * a.L (M)) >>> 15)), one dual MAC, a shift and the add, with
 * the stock's operands and modes. Tap 0 of the read and every tap of the write compute their 16 x 16 coefficient pair
 * with one dual multiply (the stock's two, same mode). Read taps 1..6 keep the stock's own packet
 * `c.L = c.L * f || c.H = W[P2 ++ P3]` (0x0197DDC2), whose multiply and load write the two halves of one register:
 * op1emu merges the halves (as the hardware is taken to), the GNU simulator keeps the load's whole-register write
 * (c.L stays e0); the same packet gives the stock's result under either rule.
 * Everything else is the stock's in the stock's order: the reads at A before the outputs, inL before out L, out L
 * before inR (so a shared I/O buffer is still correct), the clear at B after the reads, each write at C + k after the
 * read of C + k and before the read of C + k + 1 (a line shorter than 7 still behaves), the position update with the
 * stock's add / subtract / unsigned compare. What changed: the FIRs unrolled with the table and line loads in the
 * parallel slots, the line circular (B1 / L1) for the whole block, the per-block constants a circular stream (I3, read
 * in the third slot), the line address as (int + line / 4) << 2, 31 - ph as (~A << 16) >> 27, f as A << 21 >> 1, the
 * tap's last accumulate extracted straight to a register (the 32-bit extraction saturates as W32 does), the last write
 * at C + 6 carried into the next sample's first packet. 137 packets a sample against the stock's 266.
 *
 * Entry: JUMP.L from 0x0197DD46 (the LSETUP of LC1, the only way into the loop) to coop_delay_loop_near. There: P4 =
 * this, P0 = the left buffer, [SP+0x20] = the right buffer, P1 = 64, P5 = the line, [SP+0x38] = the table (the stock's
 * P2 too, but the stub uses P2), P3 = 62, I0 =
 * 0xFF801B50, I2 = 4 L, R3 = L << 16, R7 = k2 << 16, [SP+0x34] = s, W[SP+0x3e] = k1'. Exit: JUMP 0x0197DF7C, which reads
 * only [SP+0x20] / [SP+0x24] and the epilogue. Uses LC0 (the stock: LC1 / LC0), I1-I3, B1 / B3, L1 / L3 (back to 0),
 * and 4 * (K_N + 1) bytes below SP (SP back on exit). g_coop_fast_delay = 0, or a line not word-aligned, runs the stock
 * loop. */

/* the constant stream, in the order the loop reads it ([I3++], circular) */
#define K_7FFFa 0
#define K_RK    1          /* k2 high, k1' low */
#define K_ZERO  2
#define K_LQa   3          /* the line / 4 */
#define K_STa   4          /* s */
#define K_LENa  5          /* L << 16 */
#define K_7FFFb 6
#define K_LQb   7
#define K_STb   8
#define K_LENb  9
#define K_N     10

#define KSET(k, reg) [SP + 4 * (k)] = reg

    .section .near, "ax"
    .global _coop_delay_loop_near
    .type   _coop_delay_loop_near, STT_FUNC
_coop_delay_loop_near:
    P2.L = _g_coop_fast_delay; P2.H = _g_coop_fast_delay;  /* R0 / R1 / P2 / CC are free here: the stock loop sets */
    R0 = [P2];                                             /* each before reading it (the table is in [SP+0x38]) */
    CC = R0 == 0;
    IF CC JUMP 1f;
#ifdef OP1R_COOP_HITS
    P2.L = _coop_hits + 28; P2.H = _coop_hits + 28; R0 = [P2]; R0 += 1; [P2] = R0;   /* op1r test builds: the rewrite ran */
#endif
    R0 = P5;                                               /* the line address as (int + line / 4) << 2 */
    R1 = 3 (X);
    R0 = R0 & R1;
    CC = R0 == 0;
    IF !CC JUMP 1f;
    P2.L = _coop_delay_loop; P2.H = _coop_delay_loop;      /* region A is beyond JUMP.L reach of the stock */
    JUMP (P2);
1:  P2.L = _stock_delay_loop_top; P2.H = _stock_delay_loop_top;   /* the replaced LSETUP, by hand */
    LT1 = P2;
    P2.L = _stock_delay_loop_bottom; P2.H = _stock_delay_loop_bottom;
    LB1 = P2;
    LC1 = P1;                                              /* P1 = 0x40 */
    JUMP.L _stock_delay_loop_top;
    .size _coop_delay_loop_near, .-_coop_delay_loop_near

/* read tap k (k = 1..5): e0 sign-extended in C, x_k in R2; loads e1 into C.H, the next tap's e0 into N, x_k+1. The
 * first packet is the stock's own (0x0197DDC2): its multiply writes C.L while its load writes C.H. op1emu and the GNU
 * simulator then keep the load's whole-register write (C.L stays e0, unmultiplied); the hardware may merge the halves.
 * The same packet gives the same result as the stock under either rule. xa: the first packet's third slot (quote it). */
.macro RTAP C, N, xa=NOP
    \C\().L = \C\().L * R0.H || \C\().H = W[P2 ++ P3] || \xa;
    \C\().H = \C\().H * R0.L || \N = W[P2++] (X) || NOP;
    A0 += R2.H * \C\().L (W32);
    A0 += R2.H * \C\().H (W32) || NOP || R2 = [I1++];
.endm

/* write tap k (k = 0..5): R4 = its table pair (e0 low, e1 high), R0 = (fC, 0x7fff - fC), R2 = w; the previous tap's
 * sum R6 is stored first (st=1; I1 to C + k), the next pair loaded, the line word C + k read after that store. */
.macro WTAP st
    .if \st
    R1.H = R4.L * R0.H, R1.L = R4.H * R0.L || R4 = W[P2++] (X) || [I1++] = R6;
    .else
    R1.H = R4.L * R0.H, R1.L = R4.H * R0.L || R4 = W[P2++] (X) || NOP;
    .endif
    A1 = R1.H * R2.L (M), A0 = R1.H * R2.H || R4.H = W[P2 ++ P3] || R5 = [I1];
    A1 = A1 >>> 15;
    R6 = (A0 += A1);
    A1 = R1.L * R2.L (M), A0 = R1.L * R2.H;
    R6 = R6 + R5 (S);
    A1 = A1 >>> 15;
    R7 = (A0 += A1);
    R6 = R6 + R7 (S);
.endm

    .text
    .align 4
    .global _coop_delay_loop
    .type   _coop_delay_loop, STT_FUNC
_coop_delay_loop:
    R1 = [SP + 0x34];                                      /* s */
    R2 = W[SP + 0x3e] (Z);                                 /* k1' */
    R6 = [SP + 0x20];                                      /* the right buffer */
    P1 = -(4 * (K_N + 1)) (X);
    SP = SP + P1;
    B1 = P5;
    L1 = I2;                                               /* (I2 = 4 L: the stock's L1) */
    I2 = R6;
    R0 = P5;
    R0 >>= 2;                                              /* the line / 4 */
    R4 = PACK (R7.H, R2.L);
    R5 = 0x7fff (X);
    R6 = 0 (X);
    KSET(K_7FFFa, R5); KSET(K_7FFFb, R5);
    KSET(K_RK, R4);
    KSET(K_ZERO, R6);
    KSET(K_LQa, R0); KSET(K_LQb, R0);
    KSET(K_STa, R1); KSET(K_STb, R1);
    KSET(K_LENa, R3); KSET(K_LENb, R3);
    I3 = SP; B3 = SP; L3 = 4 * K_N (X);
    P5 = [SP + 4 * (K_N + 1) + 0x38];                      /* the table (the stock's P2, which the stub used) */
    R2 = R3;                                               /* the loop-top state: R0 = line / 4, R4 = s, R2 = L << 16, */
    R4 = R1;                                               /* R3 = B, R6 / I1 = the last write (a dummy word) */
    R3 = [P4 + 0x8];
    P1 = SP;
    P1 += 4 * K_N;
    I1 = P1;
    P1 = 64 (X);
    LSETUP (d_top, d_bot) LC0 = P1;
d_top:
/* --- B: the clear address (P1, used after the reads) and B's step; the last write of the previous sample */
    R5 = R3 >> 16 || R1 = [P4 + 0x4] || [I1++] = R6;           /* B.int ; A ; line[C + 6] */
    R5 = (R5 + R0) << 2;
    P1 = R5;
    R5 = R3 + R4 (NS);
    R3 = R5 - R2 (NS);
    CC = R2 <= R5 (IU);
    IF CC R5 = R3;
/* --- A: the line address (I1), the row (P2), A's step, f */
    R6 = R1 >> 16 || [P4 + 0x8] = R5 || NOP;                   /* A.int ; B stepped */
    R6 = (R6 + R0) << 2;
    I1 = R6;
    R7 = ~R1;
    R7 = R7 << 16 || NOP || R5 = [I3++];                       /* ; K_7FFFa */
    R7 = R7 >> 27;                                             /* 31 - ph */
    P2 = R7;
    R6 = R1 + R4 (NS);
    P2 = P5 + (P2 << 1);
    R7 = R6 - R2 (NS);
    CC = R2 <= R6 (IU);
    IF CC R6 = R7;
    R0 = R1 << 21 || [P4 + 0x4] = R6 || NOP;                   /* ; A stepped */
    R0 = R0 >> 1 || R1 = W[P2++] (X) || NOP;                   /* f ; e0 */
    R0.L = R5.L - R0.H (S) || R1.H = W[P2 ++ P3] || R2 = [I1++];   /* 0x7fff - f ; e1 ; x0 */
/* --- the read FIR (stock 0x0197DD94..0x0197DDE2) */
    R1.H = R1.H * R0.L, R1.L = R1.L * R0.H || R4 = W[P2++] (X) || R3 = [I3++];   /* tap 0 ; e0 of tap 1 ; K_RK */
    A0 = R2.H * R1.L (W32) || NOP || R5 = [I3++];                             /* ; ; K_ZERO */
    A0 += R2.H * R1.H (W32) || R6 = W[P0] (X) || R2 = [I1++];                 /* ; inL ; x1 */
    RTAP R4, R1, "R7 = [I0++]"                                             /* tap 1 ; m */
    RTAP R1, R4
    RTAP R4, R1
    RTAP R1, R4
    RTAP R4, R1
    R1.L = R1.L * R0.H || R1.H = W[P2 ++ P3] || NOP;          /* tap 6 */
    R1.H = R1.H * R0.L;
    A0 += R2.H * R1.L (W32);
    R0 = (A0 += R2.H * R1.H);                                  /* t */
/* --- the outputs and the feedback (stock 0x0197DDE2..0x0197DECC) */
    R6.L = R0.H + R6.L (S) || [P1] = R5 || NOP;                /* out L ; line[B] = 0 */
    A1 = R3.H * R0.L (M), A0 = R3.H * R0.H || W[P0++] = R6 || NOP;
    A1 = A1 >>> 15 || NOP || R6.L = W[I2];                     /* ; inR */
    R6.L = R0.H + R6.L (S) || R1 = [P4 + 0xc] || NOP;          /* out R ; C */
    R4 = (A0 += A1) || NOP || W[I2++] = R6.L;                  /* t * k2 */
    R4 = R4 << 1 (S) || NOP || R5 = [I3++];                    /* ; K_LQa */
    R2 = R4 + R7 (S) || NOP || R4 = [I3++];                    /* u ; K_STa */
    A1 = R3.L * R2.L (M), A0 = R3.L * R2.H || NOP || R6 = [I3++];   /* ; K_LENa */
    A1 = A1 >>> 15 || NOP || R3 = [I3++];                      /* ; K_7FFFb */
    R2 = (A0 += A1);                                           /* w */
/* --- C: the line address (I1), the row (P2), C's step, fC */
    R7 = R1 >> 16;
    R7 = (R7 + R5) << 2;
    I1 = R7;
    R7 = ~R1;
    R7 = R7 << 16;
    R7 = R7 >> 27;
    P2 = R7;
    R5 = R1 + R4 (NS);
    P2 = P5 + (P2 << 1);
    R7 = R5 - R6 (NS);
    CC = R6 <= R5 (IU);
    IF CC R5 = R7;
    R0 = R1 << 21 || [P4 + 0xc] = R5 || NOP;                   /* ; C stepped */
    R0 = R0 >> 1 || R4 = W[P2++] (X) || NOP;
    R0.L = R3.L - R0.H (S) || R4.H = W[P2 ++ P3] || NOP;
/* --- the write FIR (stock 0x0197DEBA..0x0197DF34) */
    WTAP 0
    WTAP 1
    WTAP 1
    WTAP 1
    WTAP 1
    WTAP 1
/* the last tap, its loads for the next sample: B, K_LQb, K_STb, K_LENb */
    R1.H = R4.L * R0.H, R1.L = R4.H * R0.L || NOP || [I1++] = R6;
    A1 = R1.H * R2.L (M), A0 = R1.H * R2.H || R3 = [P4 + 0x8] || R5 = [I1];
    A1 = A1 >>> 15 || NOP || R0 = [I3++];
    R6 = (A0 += A1) || NOP || R4 = [I3++];
    A1 = R1.L * R2.L (M), A0 = R1.L * R2.H;
    R6 = R6 + R5 (S) || NOP || R2 = [I3++];
    A1 = A1 >>> 15;
    R7 = (A0 += A1);
d_bot:
    R6 = R6 + R7 (S);

    [I1] = R6;                                                 /* line[C + 6] of the last sample */
    L1 = 0 (X);
    L3 = 0 (X);
    P1 = (4 * (K_N + 1)) (X);
    SP = SP + P1;
    P1.L = _stock_delay_loop_back; P1.H = _stock_delay_loop_back;
    JUMP (P1);
    .size _coop_delay_loop, .-_coop_delay_loop
