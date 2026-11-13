/* The stock grid effect's stereo render, its per-sample loop rewritten (performance, 2026-11-04).
 *
 * grid stereo render 0x019AF83C (class vtable 0x01910224 +0x14; LINK 0x60, seven registers pushed, SP -= 12: FP = SP +
 * 0x88; R0 = this, R1 / R2 = the left / right in-place I/O buffers, [FP+0x14] = the parameter block). The effect is a
 * 2D wave mesh: a tank of two 260 x 260 planes of halfwords (T = the tank, coop's own per object through the
 * 0x019AF846 thunk; plane p at T + p * 0x21020, cell (x, y) at + y * 520 + 2x). Per block the render derives from the
 * four knobs b = 2 (k0 >> 7), a = 2 (k1 >> 7), K3 = k3, KK = sat(2 (0x7FFF - k3)), K44 = k2 * k2 (16-bit
 * fractional), H = (b - 2) >> 1, q = 0x400000 / (b - 3) (the CALL 0x01AA47AC), and four phases (stepping by q, wrapping
 * to 0 at 0x400000, each indexing the 4096-entry table 0x0101E5E8 at ph >>> 10). Then 64 samples, each:
 *   - walker 1 (x1 y1 p1 = this[0x18] [0x1c] [0x08]) injects the input: cell1 = (x1, y1, p1) += (inL * K3) *
 *     2 tabA, and the mirrored cell2 = (xa, ya, pa) += (inR * K3) * 2 tabB, where xa = H + x1 (- (b - 2) when >=
 *     b - 1), ya = y1 + 1 (+ 3 when xa wrapped), and ya >= a - 1 takes ya - (a - 2) in the other plane;
 *   - walker 1 steps: x1 += 1, at b - 1 back to 1 and y1 += 2, at a - 1 back to 1 and p1 toggles;
 *   - walker 2 (x2 y2 p2 = this[0x10] [0x14] [0x04]) runs the wave equation at (x2, y2) and at its mirror (xb, yb,
 *     pb): c = (2 tabC * K44) * (sum of the four neighbours in the other plane / 2 - c) (lap1), c' = (2 tabD * K44)
 *     * (sum / 2 - c') (lap2), saturating, in the stock's order;
 *   - out = sat((2 tabC) * c + inL * KK) left, sat((2 tabD) * c' + inR * KK) right, in place;
 *   - walker 2 steps like walker 1; a third walker (this[0x20] [0x24] [0x0c], 1..258) clears the cell it is on when
 *     that is outside the active b x a area.
 * The stock loop 0x019AF932..0x019AFCC8 (353 instructions, 88 frame accesses a sample) recomputes every address with
 * 32-bit multiplies and passes everything through the frame. Here every multiply, add and shift on the signal is the
 * stock's, with its operands, halves, order and modes; the tank and object accesses are the stock's, in the stock's
 * order wherever two can touch the same word (injection 1 store, injection 2, lap1 loads / store, lap2 loads / store,
 * the output's reload of the lap1 cell, the clear); what changed is the address arithmetic (the same addresses in fewer
 * steps: T + 2 (260 y + x) + p * 0x21020), where the per-block constants live (a circular stream through I3, read in
 * the parallel slots), the phases in frame words loaded and stored in parallel slots, and walker 1's step done first
 * (its fields are read before it). The other plane of p is taken as T + 0x21020 - p * 0x21020, which is the stock's
 * T + (p == 0) * 0x21020 for p in {0, 1}: the plane fields are 0 from both constructors (0x019AF25C, 0x019AF27E) and
 * only ever toggled with `CC = R0 == 0; R0 = CC` [C in the bytes].
 *
 * Entry: JUMP.L from 0x019AF92E (the LSETUP, the only way into the loop) to coop_grid_loop_near. There: P0 = 64, P4 =
 * this, I0 / I1 = the left / right buffers, I2 / I3 = their bases (read by the code after the loop), R2 = phase A,
 * R6 = phase D, R5 = 0x21020, [FP-0x10] = phase B, [FP+8] = phase C, the frame constants (below). Exit: JUMP 0x019AFCCA,
 * which reads I2 / I3 (restored), the parameter-free epilogue (R4-R7 / P3-P5 restored), and no frame word below FP.
 * Uses LC0 (as the stock), I2 / I3 / B3 / L3 (L3 back to 0), M0 / M1, and 4 * K_N bytes of stack below the stock frame
 * (SP back on exit). g_coop_fast_grid = 0 runs the stock loop. */

/* the stock frame (FP-relative) */
#define F_T      -0x04     /* the tank (stock) */
#define F_Q      -0x08     /* q (stock) */
#define F_C4     -0x0c     /* 0x400000 (stock) */
#define F_PHB    -0x10     /* phase B (stock) */
#define F_TAB    -0x14     /* 0x0101E5E8 (stock) */
#define F_PHD    -0x18     /* phase D (stock: loop scratch; the stock keeps it in R6) */
#define F_B1     -0x1c     /* b - 1 (stock) */
#define F_A1     -0x20     /* a - 1 (stock) */
#define F_PL     -0x24     /* 0x21020 (stock: a dead 1) */
#define F_K3     -0x28     /* K3 in the low half (stock) */
#define F_T1M    -0x2c     /* T + 0x21020 - 520 (stock: a copy of T) */
#define F_PHA    -0x30     /* phase A (stock: the loop's copy of R2) */
#define F_KK     -0x34     /* KK in the low half (stock) */
#define F_B2     -0x38     /* b - 2 (stock) */
#define F_H      -0x3c     /* H (stock) */
#define F_K44    -0x40     /* K44 in the low half (stock) */
#define F_ZERO   -0x48     /* 0 (stock: loop scratch) */
#define F_A2     -0x50     /* a - 2 (stock [SP+0x38]) */
#define F_I2     -0x54     /* saved I2 (stock: a copy of a - 2) */
#define F_I3     -0x58     /* saved I3 (stock: loop scratch) */
#define F_PHC    0x08      /* phase C (stock) */

/* the constant stream, in the order the loop reads it ([I3++], circular) */
#define K_B1a    0
#define K_ONEa   1
#define K_Ha     2
#define K_B2a    3
#define K_THREEa 4
#define K_260a   5
#define K_A1a    6
#define K_Qa     7
#define K_C4a    8
#define K_K3     9
#define K_B1b    10
#define K_Hb     11
#define K_ONEb   12
#define K_B2b    13
#define K_THREEb 14
#define K_260b   15
#define K_A1b    16
#define K_Qb     17
#define K_C4b    18
#define K_KF1    19        /* K44 low, 0x4000 high */
#define K_KF2    20        /* the same */
#define K_KK     21
#define K_ONEc   22
#define K_B1c    23
#define K_A1c    24
#define K_PLc    25
#define K_260c   26
#define K_N      27

#define KSET(k, reg) [SP + 4 * (k)] = reg

    .section .near, "ax"
    .global _coop_grid_loop_near
    .type   _coop_grid_loop_near, STT_FUNC
_coop_grid_loop_near:
    P1.L = _g_coop_fast_grid; P1.H = _g_coop_fast_grid;   /* R3 / P1 / CC are free here: the stock loop sets each */
    R3 = [P1];                                             /* before reading it */
    CC = R3 == 0;
    IF CC JUMP 1f;
#ifdef OP1R_COOP_HITS
    P1.L = _coop_hits + 20; P1.H = _coop_hits + 20; R3 = [P1]; R3 += 1; [P1] = R3;   /* op1r test builds: the rewrite ran */
#endif
    R3 = I2;                                               /* the same buffer for both sides: the stock reads the */
    R4 = I3;                                               /* right input after writing the left output (R4 is */
    CC = R3 == R4;                                         /* free here too) */
    IF CC JUMP 1f;
    P1.L = _coop_grid_loop; P1.H = _coop_grid_loop;        /* region A is beyond JUMP.L reach of the stock */
    JUMP (P1);
1:  P1.L = _stock_grid_loop_top; P1.H = _stock_grid_loop_top;   /* the replaced LSETUP, by hand */
    LT0 = P1;
    P1.L = _stock_grid_loop_bottom; P1.H = _stock_grid_loop_bottom;
    LB0 = P1;
    LC0 = P0;                                              /* P0 = 0x40 */
    JUMP.L _stock_grid_loop_top;
    .size _coop_grid_loop_near, .-_coop_grid_loop_near

    .text
    .align 4
    .global _coop_grid_loop
    .type   _coop_grid_loop, STT_FUNC
_coop_grid_loop:
    P1 = -(4 * K_N) (X);
    SP = SP + P1;
    [FP + F_PHA] = R2;
    [FP + F_PHD] = R6;
    [FP + F_PL] = R5;
    R0 = I2; [FP + F_I2] = R0;
    R0 = I3; [FP + F_I3] = R0;
    R0 = 0 (X); [FP + F_ZERO] = R0;
    R0 = [FP + F_T]; R0 = R0 + R5; R1 = 520 (X); R0 = R0 - R1; [FP + F_T1M] = R0;
    R0 = [FP + F_B1]; KSET(K_B1a, R0); KSET(K_B1b, R0); KSET(K_B1c, R0);
    R0 = 1 (X);       KSET(K_ONEa, R0); KSET(K_ONEb, R0); KSET(K_ONEc, R0);
    R0 = [FP + F_H];  KSET(K_Ha, R0); KSET(K_Hb, R0);
    R0 = [FP + F_B2]; KSET(K_B2a, R0); KSET(K_B2b, R0);
    R0 = 3 (X);       KSET(K_THREEa, R0); KSET(K_THREEb, R0);
    R0 = 260 (X);     KSET(K_260a, R0); KSET(K_260b, R0); KSET(K_260c, R0);
    R0 = [FP + F_A1]; KSET(K_A1a, R0); KSET(K_A1b, R0); KSET(K_A1c, R0);
    R0 = [FP + F_Q];  KSET(K_Qa, R0); KSET(K_Qb, R0);
    R0 = [FP + F_C4]; KSET(K_C4a, R0); KSET(K_C4b, R0);
    R0 = [FP + F_K3]; KSET(K_K3, R0);
    R0 = [FP + F_KK]; KSET(K_KK, R0);
    KSET(K_PLc, R5);
    R0 = [FP + F_K44]; R0.H = 0x4000; KSET(K_KF1, R0); KSET(K_KF2, R0);
    I3 = SP; B3 = SP; L3 = 4 * K_N (X);
    M0 = 1040 (X);
    M1 = 522 (X);
    LSETUP (g_top, g_bot) LC0 = P0;
g_top:
/* --- walker 1: step it (its fields are read first), cell1 = (x1, y1, p1) -> P1, cell2 = (xa, ya, pa) -> P2 */
    MNOP || R0 = [P4 + 0x18] || R7 = [I3++];                  /* x1 ; K_B1a */
    MNOP || R3 = [P4 + 0x1c] || R6 = [I3++];                  /* y1 ; K_ONEa */
    R2 = R0 + R6 (NS) || R4 = [P4 + 0x8] || R1 = [I3++];       /* x1 + 1 ; p1 ; K_Ha */
    R1 = R1 + R0 (NS) || [P4 + 0x18] = R2 || R5 = [I3++];      /* xa0 = H + x1 ; f18 = x1 + 1 ; K_B2a */
    CC = R7 <= R2;
    IF CC JUMP .Lw1;
.Lw1b:
    CC = R7 <= R1;                                             /* xa0 >= b - 1 */
    R5 = R1 - R5 (NS) || NOP || R2 = [I3++];                   /* ; K_THREEa */
    IF CC R1 = R5;                                             /* xa */
    R6 = R3 + R6 (NS) || R5 = [FP + F_PL] || R7 = [I3++];      /* y1 + 1 ; 0x21020 ; K_260a */
    R2 = R3 + R2 (NS);                                         /* y1 + 3 */
    IF CC R6 = R2;                                             /* ya0 */
    R3 *= R7;
    R4 *= R5;                                                  /* p1 * 0x21020 */
    R3 = (R3 + R0) << 1;                                       /* y1 * 520 + 2 x1 */
    R3 = R3 + R4 (NS) || R0 = [FP + F_T] || R5 = [I3++];       /* ; T ; K_A1a */
    R3 = R3 + R0 (NS) || R2 = [FP + F_PHA] || NOP;             /* cell1 ; phase A */
    P1 = R3;
    CC = R5 <= R6;                                             /* ya0 >= a - 1 */
    IF CC JUMP .La;
.Lab:
    R6 *= R7;
    R6 = (R6 + R1) << 1;
    R6 = R6 + R4 (NS) || R5 = [FP + F_PHB] || R3 = [I3++];     /* ; phase B ; K_Qa */
    R6 = R6 + R0 (NS) || P0 = [FP + F_TAB] || R4 = [I3++];     /* cell2 ; the table ; K_C4a */
    P2 = R6;
/* --- phases A / B */
    R0 = R2 >>> 10 || R7 = [FP + F_ZERO] || R1.L = W[I0];      /* ; 0 ; inL */
    R6 = R5 >>> 10;
    P3 = R0;
    P5 = R6;
    P3 = P0 + (P3 << 1);
    P5 = P0 + (P5 << 1);
    R2 = R3 + R2 (NS) || NOP || R6 = [I3++];                   /* q + A ; K_K3 */
    R5 = R3 + R5 (NS) || R0 = W[P3] (X) || R3.L = W[I1];       /* q + B ; tabA ; inR */
    CC = R4 <= R2;
    IF CC R2 = R7;
    CC = R4 <= R5;
    IF CC R5 = R7;
/* --- the injections (stock 0x019AF98A..0x019AFA22); walker 2's fields and constants ride along */
    R1.H = R1.L * R6.L || [FP + F_PHA] = R2 || R7 = [I3++];    /* inL * K3 ; ; K_B1b */
    R0.L = R0.L + R0.L (S) || [FP + F_PHB] = R5 || NOP;        /* 2 tabA */
    R3.L = R3.L * R6.L || R4 = W[P1] (X) || NOP;               /* inR * K3 ; cell1 */
    R1.H = R1.H * R0.L || R2 = W[P5] (X) || NOP;               /* ; tabB */
    R4.L = R1.H + R4.L (S) || R6 = [P4 + 0x10] || NOP;         /* ; x2 */
    R2.H = R2.L + R2.L (S) || W[P1] = R4 || R1 = [I3++];       /* 2 tabB ; cell1 ; K_Hb */
    R2.H = R3.L * R2.H || R0 = W[P2] (X) || R4 = [I3++];       /* ; cell2 ; K_ONEb */
    R0.L = R2.H + R0.L (S) || R5 = [P4 + 0x14] || R3 = [I3++]; /* ; y2 ; K_B2b */
/* --- walker 2: C1 = (x2, y2, p2) -> P0, its neighbours' row above in the other plane -> I2; C2 = (xb, yb, pb) -> P5,
 * its row above -> R6 */
    R1 = R1 + R6 (NS) || W[P2] = R0 || R2 = [I3++];            /* xb0 = H + x2 ; cell2 ; K_THREEb */
    CC = R7 <= R1;
    R3 = R1 - R3 (NS) || R0 = [P4 + 0x4] || R7 = [I3++];       /* ; p2 ; K_260b */
    IF CC R1 = R3;                                             /* xb */
    R4 = R5 + R4 (NS) || R3 = [FP + F_PL] || NOP;              /* y2 + 1 ; 0x21020 */
    R2 = R5 + R2 (NS);                                         /* y2 + 3 */
    IF CC R4 = R2;                                             /* yb0 */
    R5 *= R7;
    R0 *= R3;                                                  /* p2 * 0x21020 */
    R5 = (R5 + R6) << 1;                                       /* y2 * 520 + 2 x2 */
    R6 = R5 + R0 (NS) || R2 = [FP + F_T] || R3 = [I3++];       /* ; T ; K_A1b */
    R6 = R6 + R2 (NS);                                         /* C1 */
    P0 = R6;
    R5 = R5 - R0 (NS) || R6 = [FP + F_T1M] || NOP;
    R5 = R5 + R6 (NS);                                         /* (x2, y2 - 1) in the other plane */
    I2 = R5;
    CC = R3 <= R4;                                             /* yb0 >= a - 1 */
    IF CC JUMP .Lc;
.Lcb:
    R4 *= R7;
    R4 = (R4 + R1) << 1;
    R5 = R4 + R0 (NS) || R1 = [FP + F_PHC] || R7 = [I3++];     /* ; phase C ; K_Qb */
    R4 = R4 - R0 (NS) || R3 = [FP + F_PHD] || NOP;             /* ; phase D */
    R5 = R5 + R2 (NS) || P1 = [FP + F_TAB] || R0 = [I3++];     /* C2 ; the table ; K_C4b */
    R6 = R4 + R6 (NS) || R2 = [FP + F_ZERO] || NOP;            /* (xb, yb - 1) in the other plane ; 0 */
    P5 = R5;
/* --- phases C / D */
    R5 = R1 >>> 10;
    R4 = R3 >>> 10;
    P3 = R5;
    P2 = R4;
    P3 = P1 + (P3 << 1);
    P2 = P1 + (P2 << 1);
    R1 = R7 + R1 (NS) || R4 = W[P3] (X) || R5.L = W[I2];       /* q + C ; tabC ; up */
    R3 = R7 + R3 (NS) || I2 += M0 || NOP;                      /* q + D ; -> down */
    CC = R0 <= R1;
    IF CC R1 = R2;
    CC = R0 <= R3;
    IF CC R3 = R2;
/* --- lap1 at C1 (stock 0x019AFB1C..0x019AFBC6) */
    R4.H = R4.L + R4.L (S) || [FP + F_PHC] = R1 || R7.L = W[I2];   /* 2 tabC ; ; down */
    MNOP || I2 -= M1 || R1 = [I3++];                           /* -> left ; K_KF1 */
    R1.H = R7.L + R5.L (S) || [FP + F_PHD] = R3 || R2.L = W[I2];   /* down + up ; ; left */
    R1.L = R4.H * R1.L || R4.L = W[P0] || NOP;                 /* 2 tabC * K44 ; centre */
    R1.H = R1.H + R2.L (S) || I2 += 4 || R7 = [I3++];          /* + left ; -> right ; K_KF2 */
    R2 = -R4 (V) || R5 = W[P2] (X) || R0.L = W[I2];            /* ; tabD ; right */
    R0.L = R1.H + R0.L (S);                                    /* + right */
    I2 = R6;
    R1.H = R7.H * R0.L || NOP || R6 = [I3++];                  /* * 0x4000 ; K_KK */
    R0.H = R5.L + R5.L (S) || NOP || R5.L = W[I0];             /* 2 tabD ; inL */
    R2.H = R2.L + R1.H (S);
    R0.L = R0.H * R7.L;                                        /* 2 tabD * K44 */
    R2.L = R1.L * R2.H;                                        /* the new centre */
    R1.L = R5.L * R6.L || W[P0] = R2 || R5.L = W[I1];          /* inL * KK ; centre ; inR */
/* --- lap2 at C2 (stock 0x019AFBCA..0x019AFC00), after lap1's store */
    R1.H = R5.L * R6.L || NOP || R3.L = W[I2];                 /* inR * KK ; up */
    MNOP || I2 += M0 || NOP;
    MNOP || NOP || R2.L = W[I2];                               /* down */
    R3.H = R2.L + R3.L (S) || I2 -= M1 || NOP;                 /* down + up ; -> left */
    MNOP || R4.L = W[P5] || R5.L = W[I2];                      /* centre ; left */
    R3.H = R3.H + R5.L (S) || I2 += 4 || NOP;                  /* + left ; -> right */
    MNOP || NOP || R6.L = W[I2];                               /* right */
    R3.H = R3.H + R6.L (S) || NOP || R5 = [I3++];              /* + right ; K_ONEc */
    R3.H = R7.H * R3.H || R6 = [P4 + 0x10] || R7 = [I3++];     /* * 0x4000 ; x2 ; K_B1c */
    R2 = -R4 (V);
    R2.L = R2.L + R3.H (S);
    R4.L = R0.L * R2.L;                                        /* the new centre */
/* --- out (stock 0x019AFC02..0x019AFC4E), after lap2's store */
    R0.H = R0.H * R4.L || W[P5] = R4 || NOP;
    R6 = R6 + R5 (NS) || R2 = W[P0] (X) || NOP;                /* x2 + 1 ; the lap1 cell again */
    R2.L = R4.H * R2.L || [P4 + 0x10] = R6 || NOP;             /* ; f10 = x2 + 1 */
    R2.L = R2.L + R1.L (S) || R4 = [P4 + 0x20] || NOP;         /* ; f20 */
    R1.H = R0.H + R1.H (S) || W[I0++] = R2.L || R3 = [I3++];   /* ; out L ; K_A1c */
    CC = R7 <= R6;
    IF CC JUMP .Lw2;
.Lw2b:
    MNOP || R0 = [P4 + 0x24] || W[I1++] = R1.H;                /* f24 ; out R */
/* --- the clear walker (stock 0x019AFC74..0x019AFCC4): R4 = f20, R0 = f24 */
    CC = R7 <= R4;
    IF CC JUMP .Ldo;
    CC = R3 <= R0;
    IF !CC JUMP .Lno;
.Ldo:
    MNOP || R2 = [P4 + 0xc] || R6 = [I3++];                    /* fc ; K_PLc */
    R2 *= R6;
    MNOP || NOP || R6 = [I3++];                                /* K_260c */
    R0 *= R6;
    R0 = (R0 + R4) << 1;
    R0 = R0 + R2 (NS) || R2 = [FP + F_T] || NOP;
    R0 = R0 + R2 (NS) || R2 = [FP + F_ZERO] || NOP;
    P1 = R0;
    R0 = R4 + R5 (NS) || W[P1] = R2 || NOP;                    /* f20 + 1 ; clear */
    JUMP .Lnb;
.Lno:
    R0 = R4 + R5 (NS) || NOP || R6 = [I3++];                   /* f20 + 1 ; (K_PLc) */
    MNOP || NOP || R6 = [I3++];                                /* (K_260c) */
.Lnb:
    R1 = 259 (X);
    CC = R1 <= R0;
    IF CC JUMP .Lz;
.Lzb:
g_bot:
    [P4 + 0x20] = R0;

    R0 = [FP + F_I2]; I2 = R0;
    R0 = [FP + F_I3]; I3 = R0;
    L3 = 0 (X);
    P1 = (4 * K_N) (X);
    SP = SP + P1;
    P0.L = _stock_grid_loop_back; P0.H = _stock_grid_loop_back;
    JUMP (P0);

/* walker 1 wrapped (stock 0x019AFA2E): R2 / P1 / P2 / CC are free here */
.Lw1:
    R2 = 1 (X);
    [P4 + 0x18] = R2;
    P2 = [P4 + 0x1c];
    P2 += 2;
    [P4 + 0x1c] = P2;
    P1 = [FP + F_A1];
    CC = P1 <= P2;
    IF !CC JUMP .Lw1b;
    [P4 + 0x1c] = R2;
    R2 = [P4 + 0x8];
    CC = R2 == 0;
    R2 = CC;
    [P4 + 0x8] = R2;
    JUMP .Lw1b;

/* ya wrapped: ya - (a - 2) in the other plane (stock 0x019AF966); R3 is free */
.La:
    R3 = [FP + F_A2];
    R6 = R6 - R3;
    R3 = [FP + F_PL];
    R4 = R3 - R4;
    JUMP .Lab;

/* yb wrapped (stock 0x019AFA74); R3 / R5 are free */
.Lc:
    R3 = [FP + F_A2];
    R4 = R4 - R3;
    R3 = [FP + F_PL];
    R0 = R3 - R0;
    JUMP .Lcb;

/* walker 2 wrapped (stock 0x019AFC5A): R2 / R6 / P1 / P2 are free */
.Lw2:
    R6 = 1 (X);
    [P4 + 0x10] = R6;
    P2 = [P4 + 0x14];
    P2 += 2;
    [P4 + 0x14] = P2;
    P1 = [FP + F_A1];
    CC = P1 <= P2;
    IF !CC JUMP .Lw2b;
    [P4 + 0x14] = R6;
    R2 = [P4 + 0x4];
    CC = R2 == 0;
    R2 = CC;
    [P4 + 0x4] = R2;
    JUMP .Lw2b;

/* the clear walker's row wrapped (stock 0x019AFCAC): R1 / R2 are free; R0 = 1 is then stored at the loop bottom */
.Lz:
    R0 = 1 (X);
    R2 = [P4 + 0x24];
    R2 += 1;
    [P4 + 0x24] = R2;
    CC = R1 <= R2;
    IF !CC JUMP .Lzb;
    [P4 + 0x24] = R0;
    R2 = [P4 + 0xc];
    CC = R2 == 0;
    R2 = CC;
    [P4 + 0xc] = R2;
    JUMP .Lzb;
    .size _coop_grid_loop, .-_coop_grid_loop
