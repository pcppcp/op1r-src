/* The stock punch effect's stereo render, both channels in one pass (performance, 2026-11-04).
 *
 * punch's stereo class (vtable 0x01910C3C, render at +0x14 = 0x019E662C) is dual-mono: it calls the mono render
 * 0x019E6260 for (this + 4, left) and (this + 0xD4, right), 64 int16 in place, params = its 4th argument. Per mono
 * object o: A = o + 4 and B = o + 0x64 (24 words each), g = [o + 0xC4], peak W[o + 0xC8], display slot [o + 0xCC].
 * Per block (0x019E627E..0x019E6372), with p0..p6 = W[params + 0/2/4/6]:
 *   N = p4 >>> 10 (the stage count), T = [0xFF801F50 + 4 ((p0 >>> 7) + (p4 >>> 12))] (a table in L1),
 *   g = smooth(g, T) (two 32 x 16 products, saturating sum), Kk = max(sat((p2 << 16) - sum_k<N |B[k]|), 0),
 *   Wmix = sat(0x7FFF - p6).
 * Per sample (0x019E6376..0x019E65A6), with L(v) the interpolated table lookup of v.H (idx = 0x1000 - (v.H >>> 4),
 * f = v.H & 15, y = sat((f << 11) * tab[idx] + (0x7FFF - (f << 11)) * tab[idx + 1]), fractional) and M32 the stock
 * 32 x 32 fractional multiply (the six-instruction A1 / A0 sequence):
 *   w = sat(x * p6 - M32(L(A[N-1]), Kk));
 *   for k = 0 .. N-1: y = L(M32(w, g)); if k: B[k-1] = y; w = sat(sat(y - B[k]) + A[k]); A[k] = w;
 *   B[N-1] = L(M32(w, g));
 *   out = sat16(rnd16(x * Wmix) + (p6 x L(A[N-1])).H)   (the 32 x 16 product, the stock's sequence without its zero terms)
 * then A[j] = A[N-1] and B[j] = B[N-1] for j = N..23, the peak W[o+0xC8] = max(rnd(W[o+0xC8] * 0x7F00), outputs)
 * (signed), written to the display slot W[0xFF801774 + 16 slot].
 *
 * coop_punch_render_st is that, for both channels at once: every stage of the left chain interleaved with the
 * right's (one channel's multiply / lookup latency covered by the other's work), the per-block constants in
 * registers, A as a hardware circular buffer (I0 / I1, L = 4N), and L(A[N-1]) computed once per sample (the output's
 * lookup is the next sample's first one: nothing writes A[N-1] in between). Every multiply, add and shift on the
 * signal is the stock's, with the stock's operands, operand order and mode; what changed: the table index as
 * ~(v >>> 20) from a base 4097 entries up (the same address: v >>> 20 = v.H >>> 4 for every v), the weight as
 * (v.H << 12) >> 1 in 16 bits (= (v.H & 15) << 11), the interpolation's two products summed in A0 + A1 (no product or sum can
 * saturate: both weights are below 0x8000 and add up to 0x7FFF), x * p6 as one 16 x 16 -> 32 product (the stock's
 * M32 of x << 16 and p6 << 16: every other term is zero) and the output product without its two zero terms.
 *
 * Entry: the stereo vtable's render entry (0x01910C50) points here (tools/l1_stereo_insert.py). It runs the stock
 * stereo render (0x019E662C, untouched) when g_coop_fast_punch = 0 (A/B, kill switch), when either object's display
 * slot is -1 (the stock asserts there), when both buffers are the same, or when N is outside 1..24 (N > 24 writes
 * past A and B into the next fields; N < 1 would run a 2^32 hardware loop in the stock). The table base is read
 * from the stock's own `P5.L / P5.H` immediates at 0x019E636A / 0x019E636E (stock disassembly), so the package's
 * retarget at the L1 sine copy (l1_stereo_insert.py sine_patches) carries over. */

#define F_KL    -0x04       /* Kk, left / right */
#define F_KR    -0x08
#define F_WMIX  -0x0c       /* Wmix in .L */
#define F_NM1   -0x10       /* N - 1 */
#define F_THIS  -0x14
#define F_BUFL  -0x18
#define F_BUFR  -0x1c
#define F_N     -0x20
#define F_GL    -0x24       /* g, left / right */
#define F_GR    -0x28
#define F_T     -0x2c       /* the gain target T */
#define F_P2S   -0x30       /* p2 << 16 */
#define F_SL    -0x34       /* s, left (setup only) */

/* The stock 32 x 32 fractional multiply, d = a * b (operands and order as the stock's) */
.macro MUL32 d, a, b
    A1 = \a\().L * \b\().L (FU);
    A1 = A1 >> 16;
    A1 += \a\().H * \b\().L (M), A0 = \a\().H * \b\().H;
    A1 += \b\().H * \a\().L (M);
    A1 = A1 >>> 15;
    \d = (A0 += A1);
.endm

/* One stage of both chains. In: R0 = w left, R2 = w right (st: the right A store of the stage before pending).
 * Out: R0 / R2 = the new w (the right one's A store pending). Left: R1 scratch, P1 the table address, P2 -> B[k-1]
 * (first: B[0]), I0 -> A[k]. Right: R3 scratch, R7 the B / A value, P4, P3, I1. R4 / R5 = g, R6.H = 0x7FFF,
 * P5 = the table base + 4097 entries, P0 = 2. `lab` labels the last packet (a loop end). */
.macro STAGE first, st, lab=, nm1=0
  .if \st
    A1 = R0.L * R4.L (FU) || [I1++] = R2 || NOP;
  .else
    A1 = R0.L * R4.L (FU);
  .endif
    A1 = A1 >> 16;
    A1 += R0.H * R4.L (M), A0 = R0.H * R4.H;
    A1 += R4.H * R0.L (M);
    A1 = A1 >>> 15;
    R0 = (A0 += A1);                                    /* u left */
    A1 = R2.L * R5.L (FU);
    R1 = R0 >>> 20;
    A1 = A1 >> 16;
    R1 = ~R1;
    A1 += R2.H * R5.L (M), A0 = R2.H * R5.H;
    P1 = R1;
    A1 += R5.H * R2.L (M);
    R1.L = R0.H << 12;
    P1 = P5 + (P1 << 1);                                /* &tab[idx] */
    A1 = A1 >>> 15;
    R1.L = R1.L >> 1;                                      /* f << 11 */
    R2 = (A0 += A1);                                    /* u right */
    R1.H = R6.H - R1.L (S) || R0.L = W[P1 ++ P0] || NOP;
    R3 = R2 >>> 20 || R0.H = W[P1] || NOP;
    A1 = R1.H * R0.H, A0 = R1.L * R0.L;
    R3 = ~R3;
    R0 = (A0 += A1);                                    /* y left */
    P4 = R3;
  .if \first
    R3.L = R2.H << 12 || R1 = [P2] || NOP;                  /* B[0] */
  .else
    R3.L = R2.H << 12 || R1 = [P2 + 4] || NOP;              /* B[k] */
  .endif
    P4 = P5 + (P4 << 1);
  .if \first
    R0 = R0 - R1 (S) || NOP || R1 = [I0];               /* A[k] */
  .else
    R0 = R0 - R1 (S) || [P2++] = R0 || R1 = [I0];       /* B[k-1] = y */
  .endif
    R3.L = R3.L >> 1 || R2.L = W[P4 ++ P0] || NOP;
    R0 = R0 + R1 (S) || R2.H = W[P4] || NOP;            /* w left */
  .if \first
    R3.H = R6.H - R3.L (S) || R7 = [P3] || [I0++] = R0;
  .else
    R3.H = R6.H - R3.L (S) || R7 = [P3 + 4] || [I0++] = R0;
  .endif
    A1 = R3.H * R2.H, A0 = R3.L * R2.L;
    R2 = (A0 += A1);                                    /* y right */
  .if \first
    R2 = R2 - R7 (S) || NOP || R7 = [I1];
  .else
    R2 = R2 - R7 (S) || [P3++] = R2 || R7 = [I1];
  .endif
  .ifnb \lab
\lab:
  .endif
  .if \nm1
    R2 = R2 + R7 (S) || P1 = [FP + F_NM1] || NOP;       /* w right; N - 1 */
  .else
    R2 = R2 + R7 (S);
  .endif
.endm

    .text
    .align 4
    .global _coop_punch_render_st
    .type   _coop_punch_render_st, STT_FUNC
_coop_punch_render_st:
    P0.L = _g_coop_fast_punch; P0.H = _g_coop_fast_punch;
    R3 = [P0];
    CC = R3 == 0;
    IF CC JUMP .Lstock;
#ifdef OP1R_COOP_HITS
    P0.L = _coop_hits + 8; P0.H = _coop_hits + 8; R3 = [P0]; R3 += 1; [P0] = R3;   /* op1r test builds: the rewrite ran */
#endif
    LINK 0x34;
    [--SP] = (R7:4, P5:3);
    P1 = R0;
    R3 = [P1 + 0xd0];                                   /* the left object's display slot */
    CC = R3 == -1;
    IF CC JUMP .Lstock_pop;
    R3 = [P1 + 0x1a0];                                  /* the right one's */
    CC = R3 == -1;
    IF CC JUMP .Lstock_pop;
    CC = R1 == R2;
    IF CC JUMP .Lstock_pop;
    P2 = [FP + 0x14];                                   /* params */
    R4 = W[P2 + 4] (X);
    R4 >>>= 10;                                         /* N */
    CC = R4 <= 0;
    IF CC JUMP .Lstock_pop;
    R5 = 24 (X);
    CC = R5 < R4;
    IF CC JUMP .Lstock_pop;
    JUMP .Lgo;
.Lstock_pop:
    (R7:4, P5:3) = [SP++];
    UNLINK;
.Lstock:
    P0.L = _stock_punch_render; P0.H = _stock_punch_render;
    JUMP (P0);
.Lgo:

    [FP + F_THIS] = R0;
    [FP + F_BUFL] = R1;
    [FP + F_BUFR] = R2;
    [FP + F_N] = R4;
    R4 += -1;
    [FP + F_NM1] = R4;
    R0 = W[P2] (X);                                     /* p0 */
    R1 = W[P2 + 2] (X);                                 /* p2 */
    R2 = W[P2 + 4] (X);                                 /* p4 */
    R3 = W[P2 + 6] (X);                                 /* p6 */
    R1 <<= 16;
    [FP + F_P2S] = R1;
    R0 >>>= 7;                                          /* 0x019E6280..0x019E62CC */
    R2 >>>= 12;
    R0 = R0 + R2;
    P0 = R0;
    P1.L = 0x1f50; P1.H = 0xff80;                       /* the gain target table in L1 (stock 0x019E62B0) */
    P1 = P1 + (P0 << 2);
    R0 = [P1];
    [FP + F_T] = R0;
    R6 = R3;                                            /* R6.L = p6 */
    R6.H = 0x7fff;
    R7.L = R6.H - R3.L (S);                             /* Wmix */
    [FP + F_WMIX] = R7;
    P0.L = 0x636c; P0.H = 0x019e;                       /* the stock's table immediates (0x019E636A / 0x019E636E) */
    R0 = W[P0] (Z);
    R1 = W[P0 + 4] (Z);
    R1 <<= 16;
    R0 = R0 | R1;
    R1 = 0x2002 (Z);                                    /* 4097 entries up */
    R0 = R0 + R1;
    P5 = R0;
    P0 = 2 (X);

    P3 = [FP + F_THIS];
    P3 += 4;                                            /* the left mono object */
    CALL .Lchan;
    [FP + F_GL] = R2;
    [FP + F_KL] = R1;
    [FP + F_SL] = R0;
    P3 = [FP + F_THIS];
    R0 = 0xd4 (X);
    R1 = P3;
    R0 = R0 + R1;
    P3 = R0;                                            /* the right one */
    CALL .Lchan;
    [FP + F_GR] = R2;
    [FP + F_KR] = R1;
    R5 = R2;                                            /* g right */
    R2 = R0;                                            /* s right */
    R0 = [FP + F_SL];                                   /* s left */
    R1 = [FP + F_KL];
    R4 = [FP + F_GL];

    R7 = [FP + F_THIS];
    R3 = [FP + F_N];
    R3 <<= 2;
    L0 = R3;
    L1 = R3;
    R3 = 8 (X);
    R3 = R3 + R7;
    I0 = R3;                                            /* A left = this + 8 */
    B0 = R3;
    R3 = 0xd8 (X);
    R3 = R3 + R7;
    I1 = R3;                                            /* A right = this + 0xd8 */
    B1 = R3;
    R3 = 0x68 (X);
    R3 = R3 + R7;
    M2 = R3;                                            /* B left = this + 0x68 */
    R3 = 0x138 (X);
    R3 = R3 + R7;
    M3 = R3;                                            /* B right = this + 0x138 */
    R3 = [FP + F_BUFL];
    I2 = R3;
    R3 = [FP + F_BUFR];
    I3 = R3;

    P1 = 64 (X);
    LSETUP (.Lo_b, .Lo_e) LC1 = P1;
.Lo_b:
    /* w = x * p6 - M32(s, Kk), both channels (R0 = s left, R1 = Kk left, R2 = s right) */
    A1 = R0.L * R1.L (FU) || R3 = [FP + F_KR] || NOP;
    A1 = A1 >> 16;
    A1 += R0.H * R1.L (M), A0 = R0.H * R1.H;
    A1 += R1.H * R0.L (M);
    A1 = A1 >>> 15 || NOP || R1.L = W[I2];              /* x left */
    R0 = (A0 += A1);
    A1 = R2.L * R3.L (FU);
    R1 = R1.L * R6.L;                                   /* x * p6 */
    A1 = A1 >> 16;
    R0 = R1 - R0 (S);
    A1 += R2.H * R3.L (M), A0 = R2.H * R3.H;
    P2 = M2;
    A1 += R3.H * R2.L (M);
    P3 = M3;
    A1 = A1 >>> 15 || NOP || R3.L = W[I3];              /* x right */
    R2 = (A0 += A1);
    R3 = R3.L * R6.L;
    R2 = R3 - R2 (S);

    STAGE 1, 0, nm1=1
    CC = P1 == 0;
    IF CC JUMP .Lfin;
    LSETUP (.Li_b, .Li_e) LC0 = P1;
.Li_b:
    STAGE 0, 1, .Li_e
.Lfin:
    /* B[N-1] = L(M32(w, g)); z = L(w) (the next sample's s); the outputs */
    A1 = R0.L * R4.L (FU) || [I1++] = R2 || NOP;
    A1 = A1 >> 16;
    A1 += R0.H * R4.L (M), A0 = R0.H * R4.H;
    A1 += R4.H * R0.L (M);
    A1 = A1 >>> 15;
    R1 = (A0 += A1);                                    /* u left */
    A1 = R2.L * R5.L (FU);
    R7 = R1 >>> 20;
    A1 = A1 >> 16;
    R7 = ~R7;
    A1 += R2.H * R5.L (M), A0 = R2.H * R5.H;
    P1 = R7;
    A1 += R5.H * R2.L (M);
    R1.L = R1.H << 12;
    P1 = P5 + (P1 << 1);
    A1 = A1 >>> 15;
    R1.L = R1.L >> 1;
    R3 = (A0 += A1);                                    /* u right */
    R1.H = R6.H - R1.L (S) || R4.L = W[P1 ++ P0] || NOP;
    R5 = R3 >>> 20 || R4.H = W[P1] || NOP;
    A1 = R1.H * R4.H, A0 = R1.L * R4.L;
    R5 = ~R5;
    R4 = (A0 += A1);                                    /* y left */
    P4 = R5;
    R3.L = R3.H << 12 || [P2] = R4 || NOP;                  /* B[N-1] left */
    P4 = P5 + (P4 << 1);
    R3.L = R3.L >> 1;
    R3.H = R6.H - R3.L (S) || R5.L = W[P4 ++ P0] || NOP;
    R7 = R0 >>> 20 || R5.H = W[P4] || NOP;
    A1 = R3.H * R5.H, A0 = R3.L * R5.L;
    R7 = ~R7;
    R5 = (A0 += A1);                                    /* y right */
    P1 = R7;
    R1.L = R0.H << 12 || [P3] = R5 || NOP;                  /* B[N-1] right */
    P1 = P5 + (P1 << 1);
    R7 = R2 >>> 20;
    R1.L = R1.L >> 1;
    R7 = ~R7;
    R1.H = R6.H - R1.L (S) || R0.L = W[P1 ++ P0] || NOP;
    P4 = R7;
    R3.L = R2.H << 12 || R0.H = W[P1] || NOP;
    P4 = P5 + (P4 << 1);
    A1 = R1.H * R0.H, A0 = R1.L * R0.L;
    R3.L = R3.L >> 1;
    R0 = (A0 += A1);                                    /* z left */
    R3.H = R6.H - R3.L (S) || R2.L = W[P4 ++ P0] || NOP;
    A1 = R6.L * R0.L (M), A0 = R6.L * R0.H || R2.H = W[P4] || NOP;
    A1 = A1 >>> 15 || R7 = [FP + F_WMIX] || NOP;
    R1 = (A0 += A1);                                    /* p6 x z left */
    A1 = R3.H * R2.H, A0 = R3.L * R2.L || NOP || R7.H = W[I2];
    R2 = (A0 += A1);                                    /* z right */
    R7.H = R7.H * R7.L;
    A1 = R6.L * R2.L (M), A0 = R6.L * R2.H;
    R7.H = R7.H + R1.H (S);
    A1 = A1 >>> 15 || NOP || W[I2++] = R7.H;            /* out left */
    R3 = (A0 += A1) || NOP || R7.H = W[I3];
    R7.H = R7.H * R7.L || R1 = [FP + F_KL] || NOP;
    R7.H = R7.H + R3.H (S) || R4 = [FP + F_GL] || NOP;
.Lo_e:
    MNOP || R5 = [FP + F_GR] || W[I3++] = R7.H;         /* out right */

    L0 = 0;
    L1 = 0;
    /* the peaks (0x019E65CA..0x019E65F6), both channels as one vector */
    P3 = [FP + F_THIS];
    R0 = [FP + F_BUFL];
    I2 = R0;
    R0 = [FP + F_BUFR];
    I3 = R0;
    R0 = W[P3 + 0xcc] (X);
    R1 = W[P3 + 0x19c] (X);
    R2 = PACK (R1.L, R0.L);
    R1 = 0x7f00 (X);
    R3.L = R2.L * R1.L;
    R3.H = R2.H * R1.L;
    P1 = 63 (X);
    MNOP || R2.L = W[I2++] || R2.H = W[I3++];
    LSETUP (.Lpk, .Lpk) LC0 = P1;
.Lpk:
    R3 = MAX (R3, R2) (V) || R2.L = W[I2++] || R2.H = W[I3++];
    R3 = MAX (R3, R2) (V);
    W[P3 + 0xcc] = R3;
    R3 >>= 16;
    W[P3 + 0x19c] = R3;

    /* A[j] = A[N-1], B[j] = B[N-1] for j = N..23 (0x019E65A8..0x019E65C8), then the display slots */
    P3 += 4;
    CALL .Lcopy;
    P3 = [FP + F_THIS];
    P2 = 0xd4 (X);
    P3 = P3 + P2;                                       /* the right mono object */
    CALL .Lcopy;

    (R7:4, P5:3) = [SP++];
    UNLINK;
    RTS;

/* Per block, one mono object P3 (0x019E627E..0x019E6372 for one channel): writes g to [P3 + 0xC4] and returns
 * R2 = g, R1 = Kk, R0 = s = L(A[N-1]). Keeps R4, R6, P0, P3, P5. */
.Lchan:
    R7 = [P3 + 0xc4];
    R5 = 0 (X);
    R1 = 0x7000 (X);
    A1 = R1.L * R7.L (M), A0 = R1.L * R7.H;
    A1 += R7.H * R5.L (M);
    R0 = 0 (X);
    BITSET (R0, 28);
    A1 = A1 >>> 15;
    R2 = [FP + F_T];
    R1 = (A0 += A1);
    A1 = R0.H * R2.L (M), A0 = R0.H * R2.H;
    A1 += R2.H * R5.L (M);
    A1 = A1 >>> 15;
    R2 = (A0 += A1);
    R2 = R1 + R2 (S);
    [P3 + 0xc4] = R2;                                   /* g */
    P1 = 0x64 (X);
    P1 = P3 + P1;
    I0 = P1;                                            /* B */
    R7 = [I0++];
    R3 = -R7 (NS);
    R7 = MAX (R3, R7);
    R1 = 0 (X);
    P1 = [FP + F_NM1];
    CC = P1 == 0;
    IF CC JUMP 1f;
    LSETUP (.Ls_b, .Ls_e) LC0 = P1;
.Ls_b:
    R1 = R1 + R7 (S) || R3 = [I0++] || NOP;
    R7 = -R3;
.Ls_e:
    R7 = MAX (R7, R3);
1:  R1 = R1 + R7 (S);
    R5 = [FP + F_P2S];
    R1 = R5 - R1 (S);
    R7 = 0 (X);
    R1 = MAX (R1, R7);                                  /* Kk */
    P1 = [FP + F_NM1];
    P1 = P3 + (P1 << 2);
    R3 = [P1 + 4];                                      /* A[N-1] */
    R7 = R3 >>> 20;
    R7 = ~R7;
    P1 = R7;
    P1 = P5 + (P1 << 1);
    R5.L = R3.H << 12;
    R5.L = R5.L >> 1;
    R5.H = R6.H - R5.L (S);
    R0.L = W[P1 ++ P0];
    R0.H = W[P1];
    A1 = R5.H * R0.H, A0 = R5.L * R0.L;
    R0 = (A0 += A1);                                    /* s */
    RTS;

/* One mono object P3: the tail copy and the display slot write. */
.Lcopy:
    R0 = [FP + F_N];
    R1 = 24 (X);
    CC = R0 < R1;
    IF !CC JUMP 1f;
    R1 = R1 - R0;
    P2 = R1;
    P1 = [FP + F_NM1];
    P0 = P3 + (P1 << 2);
    R2 = [P0 + 4];                                      /* A[N-1] */
    P1 = 0x64 (X);
    P1 = P0 + P1;
    R3 = [P1];                                          /* B[N-1] */
    P0 += 8;
    P1 += 4;
    LSETUP (.Lc_b, .Lc_e) LC0 = P2;
.Lc_b:
    [P0++] = R2;
.Lc_e:
    [P1++] = R3;
1:  P0.L = 0x1774; P0.H = 0xff80;                       /* the display slots (stock 0x019E65F8) */
    P1 = [P3 + 0xcc];
    P1 = P1 << 1;
    P1 = (P1 + P1) << 2;
    P0 = P0 + P1;
    R0 = W[P3 + 0xc8] (X);
    W[P0] = R0;
    RTS;
    .size _coop_punch_render_st, .-_coop_punch_render_st
