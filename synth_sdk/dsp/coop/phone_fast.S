/* The stock phone effect's two per-sample loops rewritten (performance, 2026-11-04): the output loop here, the
 * capture loop (coop_phone_capture) at the end of the file.
 *
 * phone is dual-mono: the stereo render 0x01A1C25C calls the mono render 0x01A1BB74 twice (this + 4, this + 0x90; R1 =
 * the in-place I/O buffer, R2 = the parameter block); those are its only callers. The mono render (LINK 0x20, seven
 * registers pushed, SP -= 12: FP = SP + 0x48) first runs a 64-sample capture loop (0x01A1BC00: input into a buffer at
 * 0x027D1FE0 + this[0x88] * 0x89D0 + sel * 0x2DF0, a gain ramp in W[this+0x28]), then the output loop LC1
 * 0x01A1BCB0..0x01A1BF34, 64 samples. Per output sample:
 *   - a phase ph = this[0x38] (16.16, wraps at 32) steps by inc = [SP+0x38]; N = clamp((ph + inc) >> 16 - ph >>> 16, 0,
 *     10) new samples are read from the capture buffer (index p = this[0x30], buffer sel = this[0x1c], length
 *     lens[sel] at [FP+0xc] = this + 0x7c; at the end of a buffer a counter / buffer switch, this[0x24] / B[this+0x11] /
 *     this[0x18]) into a 32-entry ring at this + 0x3c (write index this[0x34]);
 *   - each new sample then goes through a 32-bit one-pole high-pass, y = c1 * (x + y - xp) with the stock 32 x 32
 *     multiply (state this[8] / this[0xc], c1 = 0x7FFFFFFF - [0xFF801F50] saturated), and a 16 x 16 low-pass,
 *     s = s + c2 * y.H - c2 * s.H (state this[4], c2 = W[0xFF80A6EA]), s.H back into the ring. Stock runs these as three
 *     loops over the N ring entries (four inner LC0 loops with the first iterations peeled); every entry is
 *     independent of the others' later stages, so here they run as one chain per sample, state in registers;
 *   - a 7-tap interpolating FIR over the ring at ph >>> 16, coefficients from 0x0106A730 ([FP-4]; 32 phases a row,
 *     ph's top 5 fraction bits pick the row, the next 11 bits w blend two neighbouring phases): per tap
 *     acc += (s * t0) * w, acc += (s * t1) * (1 - w), 16-bit saturating, in the stock's order. Here the two products
 *     of a tap are one dual 16 x 16 multiply each (the same rounding as the stock's single ones);
 *   - out = (wet * W[this+0x28]) * wetgain + dry * drygain (W[SP+0x34] / W[SP+0x36]), saturated, in place.
 * Every multiply, add and shift on the signal has the stock's operands, order and mode; the integer bookkeeping (N,
 * the FIR row, the buffer reads) is the stock's arithmetic in fewer steps.
 *
 * Entry: JUMP.L from 0x01A1BCAC (the LC1 LSETUP, the only way into the loop) to coop_phone_loop_near. There: P3 = this,
 * P0 = this + 0x3c (the ring), I0 = the I/O buffer, I2 = 0xFF801F50, I3 = 0xFF80A6EA, R0 = 0, the frame words
 * [SP+0x28] 0x200000, [SP+0x30] 0x7FFFFFFF, [SP+0x34] the gains, [SP+0x38] inc, [SP+0x3c] -0x200000, [FP-4] the
 * table, [FP+0xc] lens, [FP+0x10] the buffer-switch count. Exit: JUMP 0x01A1BF36, which reads only P3 (this) and the
 * object; the epilogue restores R4-R7 / P3-P5. The stock loop's frame scratch ([FP-8], W[FP+8] / W[FP+0xa], dead
 * stores) is not written; its frame constants and the outgoing-argument words [SP+0..0xb] are reused as scratch (the
 * code after the loop reads none of them). Uses LC0 / LC1 (as the stock loop), I0-I3, B1 / B2 and L1 / L2 (back to 0).
 * g_coop_fast_phone = 0 runs the stock loop. */

/* scratch in the mono render's frame */
#define F_REM    0x00      /* steps left in this sample (the buffer-end path) */
#define F_PHN    0x04      /* ph + inc, before the wrap */
#define F_WRAP   0x08      /* 0x200000 (stock [SP+0x28]) */
#define F_C2     0x28      /* c2 in the low half */
#define F_LIM    0x2c      /* lens[sel] */
#define F_MAX    0x30      /* 0x7FFFFFFF (stock) */
#define F_GAINS  0x34      /* low: wet gain, high: dry gain (stock) */
#define F_INC    0x38      /* the phase step (stock) */
#define F_GAIN   0x3c      /* W[this+0x28], sign-extended */
#define F_UNWRAP -0x08     /* [FP-8]: -0x200000 (stock [SP+0x3c]) */

/* I3 = the capture buffer at this[0x30], [SP+F_LIM] = lens[this[0x1c]] (0x01A1BCEC's address arithmetic: 0x027D1FE0 +
 * this[0x88] * 0x89D0 + sel * 0x2DF0). Uses R0-R2, P2, P4. */
.macro CAPTURE_PTR
    R0 = [P3 + 0x88];
    R1 = 0x89d0 (Z);
    R0 *= R1;
    R1 = [P3 + 0x1c];
    R2 = 0x2df0 (X);
    R1 *= R2;
    R0 = R0 + R1;
    R1.L = 0x1fe0;
    R1.H = 0x27d;
    R0 = R0 + R1;
    R1 = [P3 + 0x30];
    R1 = R1 + R1;
    R0 = R0 + R1;
    I3 = R0;
    P2 = [FP + 0xc];
    P4 = [P3 + 0x1c];
    P2 = P2 + (P4 << 2);
    R0 = [P2];
    [SP + F_LIM] = R0;
.endm

/* test builds (-DCOOP_PHONE_CHECK): count how often the rare paths run; uses P2 / R0 only where they are free */
.macro HIT name
#ifdef COOP_PHONE_CHECK
    P2.L = \name; P2.H = \name;
    R0 = [P2];
    R0 += 1;
    [P2] = R0;
#endif
.endm

/* the same in the capture loop, through I0 (free there; L0 = 0), R6 scratch */
.macro HIT2 name
#ifdef COOP_PHONE_CHECK
    I0.L = \name; I0.H = \name;
    R6 = [I0];
    R6 += 1;
    [I0] = R6;
#endif
.endm

/* One new sample per pass (LC0 = the count): x (R3.H, R3.L = 0) loaded by the pass before; R0 = xp, R2.L = c2,
 * R4 = c1, R5 = y, R6 = s, R7 = c2 * s.H (kept from the pass before: the stock computes it from s before the update).
 * The stock stores x << 16 to this[0xc] every step; here that store also carries it into R0 for the next step. Loads
 * the next x (one past the last on the final pass; the caller steps I3 back). */
.macro CHAIN top, bot
\top:
    R1 = R3 + R5 (S) || [P3 + 0xc] = R3 || NOP;
    R1 = R1 - R0 (S) || R0 = [P3 + 0xc] || NOP;
    A1 = R4.L * R1.L (FU);
    A1 = A1 >> 16;
    A1 += R4.H * R1.L (M), A0 = R4.H * R1.H;
    A1 += R1.H * R4.L (M);
    A1 = A1 >>> 15;
    R5 = (A0 += A1);
    R1 = R2.L * R5.H;
    R6 = R6 + R1 (S);
    R6 = R6 - R7 (S) || NOP || R3.H = W[I3++];
\bot:
    R7 = R2.L * R6.H || NOP || W[I1++] = R6.H;
.endm

    .section .near, "ax"
    .global _coop_phone_loop_near
    .type   _coop_phone_loop_near, STT_FUNC
_coop_phone_loop_near:
    P1.L = _g_coop_fast_phone; P1.H = _g_coop_fast_phone;   /* R3 / P1 / CC are free here: the stock loop sets */
    R3 = [P1];                                               /* each before reading it */
    CC = R3 == 0;
    IF CC JUMP 1f;
#ifdef OP1R_COOP_HITS
    P1.L = _coop_hits + 12; P1.H = _coop_hits + 12; R3 = [P1]; R3 += 1; [P1] = R3;   /* op1r test builds: the rewrite ran */
#endif
    P1.L = _coop_phone_loop; P1.H = _coop_phone_loop;        /* region A is beyond JUMP.L reach of the stock */
    JUMP (P1);
1:  P1.L = _stock_phone_loop_top; P1.H = _stock_phone_loop_top;   /* the replaced LSETUP, by hand */
    LT1 = P1;
    P1.L = _stock_phone_loop_bottom; P1.H = _stock_phone_loop_bottom;
    LB1 = P1;
    LC1 = P2;                                                /* P2 = 0x40 */
    JUMP.L _stock_phone_loop_top;
    .size _coop_phone_loop_near, .-_coop_phone_loop_near

/* The capture loop's LSETUP at 0x01A1BBFC (the only way into 0x01A1BC00..0x01A1BC58) jumps here. R3 / P2 / CC are free
 * (the stock loop sets each before reading it); P0 = 0x40. */
    .global _coop_phone_capture_near
    .type   _coop_phone_capture_near, STT_FUNC
_coop_phone_capture_near:
    P2.L = _g_coop_fast_phone; P2.H = _g_coop_fast_phone;
    R3 = [P2];
    CC = R3 == 0;
    IF CC JUMP 1f;
#ifdef OP1R_COOP_HITS
    P2.L = _coop_hits + 12; P2.H = _coop_hits + 12; R3 = [P2]; R3 += 1; [P2] = R3;   /* op1r test builds: the rewrite ran */
#endif
    P2.L = _coop_phone_capture; P2.H = _coop_phone_capture;
    JUMP (P2);
1:  P2.L = _stock_phone_capture_top; P2.H = _stock_phone_capture_top;   /* the replaced LSETUP, by hand */
    LT0 = P2;
    P2.L = _stock_phone_capture_bottom; P2.H = _stock_phone_capture_bottom;
    LB0 = P2;
    LC0 = P0;
    JUMP.L _stock_phone_capture_top;
    .size _coop_phone_capture_near, .-_coop_phone_capture_near

    .text
    .align 4
    .global _coop_phone_loop
    .type   _coop_phone_loop, STT_FUNC
_coop_phone_loop:
    R0 = [SP + 0x28];                          /* the stock's frame constants, moved */
    [SP + F_WRAP] = R0;
    R0 = [SP + 0x3c];
    [FP + F_UNWRAP] = R0;
    R0 = W[P3 + 0x28] (X);                     /* the gain ramp's value (the capture loop's last; not written here) */
    [SP + F_GAIN] = R0;
    R0.L = W[I3];                              /* c2 */
    [SP + F_C2] = R0;
    R4 = [I2];
    R1 = [SP + F_MAX];
    R4 = R1 - R4 (S);                          /* c1 = 0x7FFFFFFF - [0xFF801F50] (S) */
    R5 = [P3 + 0x8];                           /* y */
    R6 = [P3 + 0x4];                           /* s */
    R7 = R0.L * R6.H;                          /* c2 * s.H */
    CAPTURE_PTR
    B1 = P0;                                   /* the ring, circular: chain writes (I1) and FIR reads (I2) */
    B2 = P0;
    L1 = 0x40 (X);
    L2 = 0x40 (X);
    P2 = [P3 + 0x34];
    P2 = P0 + (P2 << 1);
    I1 = P2;
    P1 = [FP - 0x4];                           /* the FIR table */
    P5 = 0x3e (X);
    R0 = [P3 + 0x38];                          /* ph */
    R3 = [SP + F_INC];
    P4 = 0x40 (X);
    LSETUP (s_top, s_bot) LC1 = P4;
s_top:                                         /* R0 = ph, R3 = inc */
    R1 = R0 + R3 (NS) || [P3 + 0x38] = R0 || NOP;
    R3 = R0 >>> 16 || [SP + F_PHN] = R1 || NOP;
    R0 = R1 >> 16 || R2 = [P3 + 0x30] || NOP;
    R0 = R0 - R3 (NS) || R1 = [SP + F_LIM] || NOP;
    CC = R0 <= 0;                              /* N = 0: no new samples */
    IF CC JUMP s_fir;
    R3 = 10 (X);
    R0 = MIN (R0, R3);                         /* N */
    R1 = R1 - R2 (NS);                         /* lim - p */
    CC = R0 < R1;
    IF !CC JUMP s_slow;                        /* the buffer ends within these N */
    P2 = R0;
    R2 = R2 + R0 (NS) || R0 = [P3 + 0xc] || NOP;
    R3 = R3 - R3 (NS) || [P3 + 0x30] = R2 || NOP;
    MNOP || R2 = [SP + F_C2] || R3.H = W[I3++];
    LSETUP (c_top, c_bot) LC0 = P2;
    CHAIN c_top, c_bot
    I3 -= 2;
s_fir:                                         /* R0-R3 free; R4-R7 the chain */
    R0 = [P3 + 0x38];                          /* ph */
    R1 = ~R0;
    R1 = R1 << 16 || R3 = [SP + F_MAX] || NOP;
    R1 = R1 >> 27;                             /* 31 - (ph.L >> 11): the row */
    R2 = R0 >>> 16;
    P2 = R1;
    P4 = R2;
    P2 = P1 + (P2 << 1);
    P4 = P0 + (P4 << 1);
    I2 = P4;
    R2.L = R0.L << 5 || R1 = W[P2++] (X) || NOP;
    R2.L = R2.L >> 1 || R1.H = W[P2 ++ P5] || NOP;   /* w = (ph & 0x7FF) << 4 */
    R2.H = R3.H - R2.L (S) || R0.L = W[I2++] || NOP;  /* 1 - w */
    /* tap 0 (the accumulator starts at 0: 0 + a + b is a + b, saturated once) */
    R3.H = R0.L * R1.H, R3.L = R0.L * R1.L || R1 = W[P2++] (X) || R0.L = W[I2++];
    R3.H = R3.H * R2.H, R3.L = R3.L * R2.L || R1.H = W[P2 ++ P5] || NOP;
    R0.H = R3.L + R3.H (S);
    /* taps 1-5 */
    .rept 5
    R3.H = R0.L * R1.H, R3.L = R0.L * R1.L || R1 = W[P2++] (X) || R0.L = W[I2++];
    R3.H = R3.H * R2.H, R3.L = R3.L * R2.L || R1.H = W[P2 ++ P5] || NOP;
    R0.H = R0.H + R3.L (S);
    R0.H = R0.H + R3.H (S);
    .endr
    /* tap 6, the output mix and the phase step */
    R3.H = R0.L * R1.H, R3.L = R0.L * R1.L || R1 = [SP + F_GAIN] || NOP;
    R3.H = R3.H * R2.H, R3.L = R3.L * R2.L || R2 = [SP + F_GAINS] || R1.H = W[I0];
    R0.H = R0.H + R3.L (S);
    R0.H = R0.H + R3.H (S) || R3 = [SP + F_WRAP] || NOP;
    R1.L = R0.H * R1.L || R0 = [SP + F_PHN] || NOP;          /* wet * gain */
    R1.H = R1.H * R2.H, R1.L = R1.L * R2.L || R2 = [FP + F_UNWRAP] || NOP;
    R1.L = R1.L + R1.H (S);
    CC = R3 <= R0;
    R2 = R0 + R2 (NS) || R3 = [SP + F_INC] || W[I0++] = R1.L;
s_bot:
    IF CC R0 = R2;

    [P3 + 0x38] = R0;                          /* write the state back */
    [P3 + 0x8] = R5;
    [P3 + 0x4] = R6;
    R0 = I1;
    R1 = P0;
    R0 = R0 - R1;
    R0 >>= 1;
    [P3 + 0x34] = R0;
    L1 = 0 (X);
    L2 = 0 (X);
    P0.L = _stock_phone_loop_back; P0.H = _stock_phone_loop_back;
    JUMP (P0);

s_slow:                                        /* R0 = N, R1 = lim - p (< N + 1), R2 = p */
    [SP + F_REM] = R0;
    HIT _g_phone_hit_slow
    R0 = [SP + F_REM];
s_chunk:                                       /* this many steps before the next buffer-end check */
    R3 = 1 (X);
    R1 = MAX (R1, R3);
    R1 = MIN (R1, R0);
    R0 = R0 - R1 (NS);
    [SP + F_REM] = R0;
    R2 = R2 + R1 (NS);
    [P3 + 0x30] = R2;
    P2 = R1;
    R0 = [P3 + 0xc];
    R3 = R3 - R3 (NS);
    R2 = [SP + F_C2];
    R3.H = W[I3++];
    LSETUP (k_top, k_bot) LC0 = P2;
    CHAIN k_top, k_bot
    I3 -= 2;
    R2 = [P3 + 0x30];
    R1 = [SP + F_LIM];
    CC = R1 <= R2;                             /* 0x01A1BD1A */
    IF !CC JUMP s_next;
    HIT _g_phone_hit_end
    R0 = [P3 + 0x24];                          /* the buffer end (0x01A1BD1E..0x01A1BD44) */
    R0 += 1;
    [P3 + 0x24] = R0;
    R1 = B[P3 + 0x11] (Z);
    CC = R1 == 0;
    IF CC JUMP s_end;
    R1 = [FP + 0x10];
    CC = R1 <= R0;
    IF !CC JUMP s_end;
    HIT _g_phone_hit_swap
    R0 = 0 (X);
    B[P3 + 0x11] = R0;
    [P3 + 0x24] = R0;
    R0 = [P3 + 0x1c];
    R1 = [P3 + 0x18];
    [P3 + 0x1c] = R1;
    [P3 + 0x18] = R0;
s_end:
    R0 = 0 (X);
    [P3 + 0x30] = R0;
    CAPTURE_PTR
s_next:
    R0 = [SP + F_REM];
    CC = R0 <= 0;
    IF CC JUMP s_fir;
    R2 = [P3 + 0x30];
    R1 = [SP + F_LIM];
    R1 = R1 - R2 (NS);
    JUMP s_chunk;
    .size _coop_phone_loop, .-_coop_phone_loop

/* P0 = the capture write pointer, base(this[0x14]) with q = 0 (0x01A1BC00's address arithmetic). Uses R6 / R7. */
.macro WRITE_BASE
    R7 = [P5 + 0x88];
    R6 = 0x89d0 (Z);
    R7 *= R6;
    R6 = [P5 + 0x14];
    R5 = 0x2df0 (X);
    R6 *= R5;
    R7 = R7 + R6;
    R6.L = 0x1fe0;
    R6.H = 0x27d;
    R7 = R7 + R6;
    P0 = R7;
.endm

/* The capture loop 0x01A1BC00..0x01A1BC58 (64 samples, before the output loop). Per sample: x = the input; the level
 * ramp g = W[this+0x28] steps to sat(g * 0x7FB5 + sat(|x| << 8) * 0x4A) (one dual multiply, as the stock); x goes into
 * the capture buffer base(this[0x14])[q], q = this[0x2c] + 1; at q >= [FP-8] the buffer is closed (path A, 0x01A1BFFE);
 * else x < -32 clears the trigger flag B[this+0x10] and x > 32 with the flag clear sets it and, past 0x80 samples, counts
 * this[0x20] towards [SP+0x3c], where the buffers swap (path B, 0x01A1BFBA..0x01A1BFFC). Here g, q, the write pointer and
 * the flag live in registers (the flag as the threshold x must exceed: 32 while clear, 0x7FFF while set); the level
 * product is computed at the loop's end and summed at the next pass's start, so the paths jump back to the loop's last
 * instruction as the stock's do.
 * Entry: P5 = this, P3 = P1 = the I/O block, P0 = 0x40, R4 = 0x004A7FB5, R5 = 0x89D0, R2 = 0x027D1FE0, R1 = 0x2DF0,
 * R0 = 0. Exit: JUMP 0x01A1BC5A with P5, P3, R0, R1, R2, R5 as on entry (the code after reads them). Uses LC0, and as
 * scratch only registers the code after the loop sets before reading. */
    .text
    .align 4
    .global _coop_phone_capture
    .type   _coop_phone_capture, STT_FUNC
_coop_phone_capture:
    R3 = W[P5 + 0x28] (X);                     /* g */
    WRITE_BASE
    R6 = R3.L (Z);                             /* so that the first pass's sum gives g */
    P2 = [P5 + 0x2c];                          /* q */
    P0 = P0 + (P2 << 1);
    P4 = [FP - 0x8];                           /* the buffer length */
    R7 = B[P5 + 0x10] (Z);
    CC = R7 == 0;
    R2 = 0x7fff (X);
    R1 = 0x20 (X);
    IF CC R2 = R1;                             /* the trigger threshold */
    R0 = -0x20 (X);
    R7 = 0x40 (X);
    LC0 = R7;
    LSETUP (q_top, q_bot) LC0;
q_top:
    R3.L = R6.L + R6.H (S) || R7 = W[P1++] (X) || NOP;   /* g, the previous pass's product */
    R6 = -R7 (NS) || W[P0++] = R7 || NOP;
    R6 = MAX (R6, R7) (V);
    R3.H = R6.L << 8 (S);
    P2 += 1;
    CC = P4 <= P2;
    IF CC JUMP q_a;
    CC = R7 < R0;                              /* x < -32: the flag clears */
    IF CC R2 = R1;
    CC = R2 < R7;                              /* x > 32 with the flag clear */
    IF CC JUMP q_rise;
q_bot:
    R6.H = R3.H * R4.H, R6.L = R3.L * R4.L;

    R3.L = R6.L + R6.H (S);
    W[P5 + 0x28] = R3;
    [P5 + 0x2c] = P2;
    CC = R2 == R1;                             /* threshold 32: the flag is clear */
    R7 = CC;
    BITTGL (R7, 0);
    B[P5 + 0x10] = R7;
    R0 = 0 (X);
    R1 = 0x2df0 (X);
    R2.L = 0x1fe0;
    R2.H = 0x27d;
    R5 = 0x89d0 (Z);
    P0.L = _stock_phone_capture_back; P0.H = _stock_phone_capture_back;
    JUMP (P0);

q_a:                                           /* the buffer is full (0x01A1BFFE..0x01A1C02C) */
    HIT2 _g_phone_hit_capa
    R7 = P2;
    P0 = [P5 + 0x14];
    P2 = [FP + 0xc];
    P0 = P2 + (P0 << 2);
    [P0] = R7;                                 /* lens[this[0x14]] = q */
    R7 = 0 (X);
    [P5 + 0x20] = R7;
    R7 = [P5 + 0x88];
    R6 = 0x89d0 (Z);
    R7 *= R6;
    R6 = [P5 + 0x1c];
    R5 = 0x2df0 (X);
    R6 *= R5;
    R7 = R7 + R6;
    R6.L = 0x1fe0;
    R6.H = 0x27d;
    R7 = R7 + R6;                              /* base(this[0x1c]) */
    P0 = [P5 + 0x1c];
    P0 = P2 + (P0 << 2);
    P0 = [P0];
    [P5 + 0x30] = P0;                          /* the read position = lens[this[0x1c]] */
    P2 = R7;
    P0 = P2 + (P0 << 1);
    R7 = 0 (X);
    W[P0] = R7;
    R7 = [FP + 0x10];
    [P5 + 0x24] = R7;
    P2 = 0 (X);
    WRITE_BASE
    JUMP q_bot;

q_rise:                                        /* the flag sets (0x01A1BFC8..) */
    R2 = 0x7fff (X);
    R7 = 0x80 (X);
    R6 = P2;
    CC = R7 < R6;
    IF !CC JUMP q_bot;
    HIT2 _g_phone_hit_rise
    R6 = [P5 + 0x20];
    R6 += 1;
    [P5 + 0x20] = R6;
    R7 = [SP + 0x3c];
    CC = R7 <= R6;
    IF !CC JUMP q_bot;
    HIT2 _g_phone_hit_capb
    R7 = P2;                                   /* path B: close this buffer, swap (0x01A1BFE2..) */
    P0 = [P5 + 0x14];
    P2 = [FP + 0xc];
    P0 = P2 + (P0 << 2);
    [P0] = R7;
    R7 = 1 (X);
    B[P5 + 0x11] = R7;
    R7 = [P5 + 0x18];
    R6 = [P5 + 0x14];
    [P5 + 0x14] = R7;
    [P5 + 0x18] = R6;
    R7 = 0 (X);
    [P5 + 0x20] = R7;
    P2 = 0 (X);
    WRITE_BASE
    JUMP q_bot;
    .size _coop_phone_capture, .-_coop_phone_capture
