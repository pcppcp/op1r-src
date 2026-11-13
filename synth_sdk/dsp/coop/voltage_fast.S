/* The stock Voltage synth engine's per-sample voice loop rewritten (performance, 2026-11-04).
 *
 * Voltage (Slump.cpp) renders a voice with render(this = R0, out = R1, params = R2) at 0x01A49BF4 (vtable + 0x14 of
 * its voice class; no CALL or jump in the image targets it or its loop): 128 samples at 88.2 kHz into a 16-bit buffer,
 * then 32 of them (every fourth) into a display buffer. The setup (LINK 0x2c, seven registers: FP = SP + 0x48) leaves
 * per-block values in the frame and runs one LC0 loop 0x01A49CBA..0x01A49ED0 (111 instructions a sample, 24 frame
 * accesses, two conditional branches). Per sample, with d = the pitch word [I1++] (L1 0xFF801950) and T the 8192-entry
 * sine table (P3):
 *   m = M(d, K) (the stock 32 x 32 multiply, K = [SP+0x3c]); a = sat(d + m), b = sat(d - m);
 *   s1 = T[ph1 >>> 19 + 4096]                                  ph1 = this[0xc] += d; wrap = (new < old, signed)
 *   s2 = T[sext(K40 * fb + ph2.H) >>> 3 + 4096] -> fb           ph2 = this[0x14] += sat(R7 + b)
 *   s4 = T[sext(K40 * h8 + ph3.H) >>> 3 + 4096] -> h8           ph3 = this[4]    += d
 *   s3 = T[sext(K40 * h20 + ph4.H) >>> 3 + 4096] -> h20         ph4 = this[0x1c] += sat(a - R7);  R7 = sat(Kf + R7)
 *   f(x) = u + 0.5 u with u = x - (0x2667 x) x x (16-bit fractional, rounded, saturating as the stock)
 *   g4 = f(|s4|), g3 = f(|s3|), w = K30 * (g4 ^ g3); h28 = this[0x28] (halved on a wrap) - g4 * K28 -> h28
 *   mix = sat(sat(0.125 f(s2) + 0.25 f(w)) + 0.5 h28)
 *   z = this[0x24] = sat(sat(z + K38 * mix) - K38 * z.H)  (32-bit products)
 *   y = sat(0.5 s1 + z.H); out = sat((y >>> 1) + (y_prev >>> 1)); y in R6.L (this[0x2a] after the loop), R7 this[0x2c]
 * fb, h8, h20 are the halfwords this[0x18], [0x8], [0x20]; K40 = params[1], K30 = params[0], K28 and Kf from the
 * setup, K38 from the table 0x0106A5EC.
 *
 * The rewrite: every multiply, add, shift and saturation on the signal is the stock's with its operands and modes; what
 * changed is the bookkeeping and the pairing:
 *   - the table index as (T + 0x2000) + 2 * (v >>> 19) (one shift for the stock's shift, add and shift);
 *   - the two waveshapers of |s4| and |s3| run as one pair in the dual MAC (s4 in the high half, s3 in the low), and so
 *     do f(s2) and f(w) and their scales 0.125 / 0.25, 0.5 h28 with 0.5 s1, K40 h8 with K40 h20; the vector ABS takes
 *     both lookups (the stock's ABS (V) already worked on one half each);
 *   - the per-block values come from a 7-word circular stream in the third slot (I3 / B3 / L3 below the stock frame),
 *     the state words load and store in the parallel slots, h8 / h28 go through I2 (this + 8 / + 0x28);
 *   - the wrap halving of h28 is a conditional move (the stock branches over it; CC lives from the compare to the move:
 *     nothing in between writes CC);
 *   - the output store rides in the next pass's first packet: I0 runs circular over the buffer (B0 = out, L0 = 256)
 *     from out + 254, so the first pass's store lands on out[127] and the last one, after the loop, rewrites it.
 * 64 packets a sample against the stock's 111 instructions.
 *
 * Not reproduced: the stock loop's frame scratch ([SP+0x1c], [SP+0x34], [SP+0x44]) and its stores to the caller's
 * argument homes ([FP+8], B[FP+0xc], W[FP+0xe], [FP+0x10]), dead after the loop.
 *
 * Entry: JUMP.L from 0x01A49CB6 (the loop's LSETUP, the only way into it) to coop_voltage_loop_near. There: P1 = 128,
 * P3 = T, P4 = this + 4, I0 = out, I1 = the pitch words, R6 = this[0x2a], R7 = this[0x2c], the frame as above. Exit:
 * JUMP 0x01A49ED2 with R6.L = y, R7, P1 = out ([SP+0x20]); the code after reads only those, [SP+0x24] and I0 / P0 / P2
 * it sets itself. Uses LC0 / LT0 / LB0 (as the stock), I0-I3, B0, B3, M2 (L0 / L3 back to 0). g_coop_fast_voltage = 0
 * runs the stock loop. */

/* the stream, 7 words below the stock frame: CK = K40 : K30, K3c, Kf, CA = 0x2667 : 0x4000, CL = K28 : K38,
 * CB = 0x2000 : 0x1000, CL again (the order the loop body reads them) */
#define STREAM 32

/* test builds (-DCOOP_VOLTAGE_CHECK): count the calls of the rewrite. Uses P0 / R0. */
.macro HIT name
#ifdef COOP_VOLTAGE_CHECK
    P0.L = \name; P0.H = \name;
    R0 = [P0];
    R0 += 1;
    [P0] = R0;
#endif
.endm

    .section .near, "ax"
    .global _coop_voltage_loop_near
    .type   _coop_voltage_loop_near, STT_FUNC
_coop_voltage_loop_near:
    P0.L = _g_coop_fast_voltage; P0.H = _g_coop_fast_voltage;   /* R0 / P0 / CC are free here: the stock loop sets */
    R0 = [P0];                                                   /* each before reading it */
    CC = R0 == 0;
    IF CC JUMP 1f;
#ifdef OP1R_COOP_HITS
    P0.L = _coop_hits + 36; P0.H = _coop_hits + 36; R0 = [P0]; R0 += 1; [P0] = R0;   /* op1r test builds: the rewrite ran */
#endif
    P0.L = _coop_voltage_loop; P0.H = _coop_voltage_loop;        /* region A is beyond JUMP.L reach of the stock */
    JUMP (P0);
1:  P0.L = _stock_voltage_loop_top; P0.H = _stock_voltage_loop_top;   /* the replaced LSETUP, by hand */
    LT0 = P0;
    P0.L = _stock_voltage_loop_bottom; P0.H = _stock_voltage_loop_bottom;
    LB0 = P0;
    LC0 = P1;                                                    /* P1 = 128 */
    JUMP.L _stock_voltage_loop_top;
    .size _coop_voltage_loop_near, .-_coop_voltage_loop_near

    .text
    .align 4
    .global _coop_voltage_loop
    .type   _coop_voltage_loop, STT_FUNC
_coop_voltage_loop:
    HIT _g_voltage_hit_calls
    R0 = W[SP + 0x40] (Z);                     /* K40 (the stock reads only the low half of each of these words) */
    R1 = W[SP + 0x30] (Z);                     /* K30 */
    R0 = PACK (R0.L, R1.L);                    /* CK */
    R1 = [SP + 0x3c];                          /* K3c */
    R3 = [SP + 0x2c];                          /* Kf: 0 or 1 */
    R4 = W[SP + 0x28] (Z);                     /* K28 */
    R5 = W[SP + 0x38] (Z);                     /* K38 */
    R4 = PACK (R4.L, R5.L);                    /* CL */
    SP += -STREAM;
    [SP + 0x00] = R0;
    [SP + 0x04] = R1;
    [SP + 0x08] = R3;
    R0.H = 0x2667;
    R0.L = 0x4000;
    [SP + 0x0c] = R0;                          /* CA */
    [SP + 0x10] = R4;
    R0.H = 0x2000;
    R0.L = 0x1000;
    [SP + 0x14] = R0;                          /* CB */
    [SP + 0x18] = R4;
    I3 = SP;
    B3 = SP;
    L3 = 28;
    P0.L = 0x2000; P0.H = 0;
    P3 = P3 + P0;                              /* T + 0x2000 */
    I2 = P4;
    I2 += 4;                                   /* this + 8 */
    R0 = 0x20;
    M2 = R0;
    R0 = I0;
    B0 = R0;                                   /* out, circular */
    R1 = 254;
    R0 = R0 + R1;
    I0 = R0;
    L0 = 256;
    R0 = [P4 + 8];                             /* the loop's rotated loads for the first pass: ph1, d */
    R3 = [I1++];
    LSETUP (v_top, v_bot) LC0 = P1;
/* R0 = ph1, R3 = d, R5.L = the previous sample's output (stored in the first packet), R6.L = y_prev, R7 = the stock's
 * counter word; P3 = T + 0x2000, P4 = this + 4, I2 = this + 8 at the top of a pass, M2 = 0x20. */
v_top:
    R1 = R0 >>> 19 || R2 = W[P4 + 0x14] (X) || W[I0++] = R5.L;          /* i1; fb */
    P0 = R1;
    R5 = R0 + R3 (NS) || R1 = [P4 + 0x10] || R4 = [I3++];                /* ph1 + d; ph2; CK */
    CC = R5 < R0;                                                        /* the wrap */
    R2.H = R4.H * R2.L || [P4 + 8] = R5 || NOP;                          /* K40 * fb */
    R2.H = R2.H + R1.H (NS) || R5 = W[P4 + 0x1c] (X) || NOP;             /* + ph2.H; h20 */
    R2 = R2 >>> 19 || R0 = [P4] || R5.H = W[I2];                         /* i2; ph3; h8 : h20 */
    P1 = R2;
    R5.H = R4.H * R5.H, R5.L = R4.H * R5.L || R2 = [P4 + 0x18] || NOP;   /* K40 * h8 : K40 * h20; ph4 */
    R5.H = R5.H + R0.H (NS) || R1 = [I3++] || NOP;                       /* + ph3.H; K3c */
    R6.H = R5.L + R2.H (NS);                                             /* K40 * h20 + ph4.H */
    R5 = R5 >>> 19;                                                      /* i4 */
    R0 = R0 + R3 (NS);                                                   /* ph3 + d */
    P5 = R5;
    R5 = R6 >>> 19 || [P4] = R0 || NOP;                                  /* i3 */
    P2 = R5;
    P0 = P3 + (P0 << 1);
    P1 = P3 + (P1 << 1);
    P5 = P3 + (P5 << 1);
    P2 = P3 + (P2 << 1);
    A1 = R3.L * R1.L (FU) || R5.H = W[P5] || NOP;                        /* m = M(d, K3c); s4 */
    A1 = A1 >> 16 || R5.L = W[P2] || NOP;                                /* s3 */
    A1 += R3.H * R1.L (M), A0 = R3.H * R1.H || W[P4 + 0x1c] = R5 || NOP; /* h20 = s3 */
    A1 += R1.H * R3.L (M) || W[I2] = R5.H || NOP;                        /* h8 = s4 */
    A1 = A1 >>> 15 || I2 += M2 || NOP;                                   /* I2 = this + 0x28 */
    R1 = (A0 += A1) || R0 = [P4 + 0x10] || NOP;                          /* m; ph2 */
    R6.H = R6.L >>> 1;                                                   /* y_prev >>> 1 */
    R3 = R3 + R1, R1 = R3 - R1 (S);                                      /* a, b */
    R3 = R3 - R7 (S);                                                    /* sat(a - R7) */
    R1 = R7 + R1 (S);                                                    /* sat(R7 + b) */
    R2 = R2 + R3 (NS) || R3 = [I3++] || NOP;                             /* ph4 new; Kf */
    R0 = R0 + R1 (NS) || [P4 + 0x18] = R2 || NOP;                        /* ph2 new */
    R7 = R3 + R7 (S) || [P4 + 0x10] = R0 || R2 = [I3++];                 /* CA */
    R5 = ABS R5 (V) || R0.L = W[P1] || R3.L = W[I2];                     /* |s4| : |s3|; s2; h28 */
    R1.H = R2.H * R5.H, R1.L = R2.H * R5.L || W[P4 + 0x14] = R0 || NOP;  /* fb = s2 */
    R1.H = R1.H * R5.H, R1.L = R1.L * R5.L;
    R1.H = R1.H * R5.H, R1.L = R1.L * R5.L;
    R1 = R5 -|- R1 (S);
    R5.H = R1.H * R2.L, R5.L = R1.L * R2.L;
    R5 = R1 +|+ R5 (S);                                                  /* g4 : g3 */
    R1 = R5 >> 16;
    R1 = R1 ^ R5;                                                        /* R1.L = g4 ^ g3 */
    R0.H = R4.L * R1.L || R4 = [I3++] || NOP;                            /* w : s2; CL */
    R1.L = R5.H * R4.H || R4 = [I3++] || NOP;                            /* q = g4 * K28; CB */
    R5.L = R3.L * R2.L;                                                  /* h28 halved */
    IF CC R3 = R5;
    R3.H = R3.L - R1.L (S);                                              /* h28 new */
    R5.H = R2.H * R0.H, R5.L = R2.H * R0.L || R3.L = W[P0] || NOP;       /* s1 */
    R5.H = R5.H * R0.H, R5.L = R5.L * R0.L || W[I2] = R3.H || NOP;
    R1.H = R3.H * R2.L, R1.L = R3.L * R2.L || I2 -= M2 || NOP;           /* 0.5 h28 : 0.5 s1; I2 = this + 8 */
    R5.H = R5.H * R0.H, R5.L = R5.L * R0.L || R3 = [P4 + 0x20] || NOP;   /* z */
    R5 = R0 -|- R5 (S);
    R0.H = R5.H * R2.L, R0.L = R5.L * R2.L;
    R5 = R5 +|+ R0 (S) || R2 = [I3++] || NOP;                            /* f(w) : f(s2); CL */
    R5.H = R5.H * R4.H, R5.L = R5.L * R4.L;                              /* 0.25 f(w) : 0.125 f(s2) */
    R5.L = R5.L + R5.H (S);
    R5.L = R5.L + R1.H (S);                                              /* mix */
    R0 = R2.L * R5.L;                                                    /* K38 * mix */
    R4 = R2.L * R3.H;                                                    /* K38 * z.H */
    R3 = R3 + R0 (S);
    R3 = R3 - R4 (S);                                                    /* z new */
    R6.L = R1.L + R3.H (S) || [P4 + 0x20] = R3 || NOP;                   /* y */
    R5.L = R6.L >>> 1 || R0 = [P4 + 8] || R3 = [I1++];                   /* the next pass's ph1, d */
v_bot:
    R5.L = R5.L + R6.H (S);                                              /* out (stored in the next pass's first packet) */

    W[I0++] = R5.L;                            /* out[127] */
    L0 = 0;
    L3 = 0;
    SP += STREAM;
    P1 = [SP + 0x20];                          /* out, for the display copy after the loop */
    P0.L = _stock_voltage_loop_back; P0.H = _stock_voltage_loop_back;
    JUMP (P0);
    .size _coop_voltage_loop, .-_coop_voltage_loop
