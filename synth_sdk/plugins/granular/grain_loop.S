#ifdef __bfin__
/* displayname808.granular 0.9.3: a grain over a stretch of samples, by hand (granular.c's grain_run, the same
 * numbers; the start checks it against the C loop and uses it only when they agree sample for sample).
 *
 *   void grain_loop_asm(struct gk *k);
 *   struct gk { u32 pos, rate, wph, winc; const short *data; const short *win; s32 *acc; s32 n; }   n >= 1
 *
 * For each of n samples, as the C:
 *   a = data[pos >> 10], b = data[(pos >> 10) + 1], f = pos & 1023, w = win[wph >> 20]
 *   s = a + ((b f - a f) >> 10)          (b f - a f on the multiply-accumulator: (b - a) f, exactly)
 *   acc[i] += (s w) >> 15
 *   pos += rate, wph += winc
 * and pos, wph written back.
 *
 * Laid out as op1r's grain loop (its .op1p, 0x182c), which ran on every op1r OP-1: one sample a pass, the next one's
 * sample, fraction and window read during it, each sum stored at the start of the next pass. So the first pass
 * stores acc[-1] (read in the start: its own value back) and acc must have a word before it (granular.c: acc_mem[0]
 * and the sums of the stretch before). 16 instructions a sample (op1r's 17 squared a sine). L0-L3 stay 0 (the
 * compiler's); LC0 / LT0 / LB0 saved and restored, so the caller's hardware loop state is as it was. */

    .text
    .align 4
    .global _grain_loop_asm
    .type _grain_loop_asm, STT_FUNC
_grain_loop_asm:
    [--SP] = (R7:4, P5:3);
    [--SP] = LC0;                       /* the hardware loop's registers as the caller had them, back at the end */
    [--SP] = LT0;
    [--SP] = LB0;
    P5 = R0;                            /* k */
    R6 = [P5];                          /* pos */
    R0 = [P5 + 4];                      /* rate */
    M3 = R0;
    R1 = R6 + R0 (NS);
    I3 = R1;                            /* pos of the next sample */
    R1 = [P5 + 12];                     /* winc */
    M2 = R1;
    R2 = [P5 + 8];                      /* wph */
    R0 = R2 + R1 (NS);
    I2 = R0;                            /* wph of the next sample */
    P0 = [P5 + 16];                     /* data */
    P3 = [P5 + 20];                     /* win */
    R0 = [P5 + 24];                     /* acc */
    I0 = R0;                            /* the sums read */
    R0 += -4;
    I1 = R0;                            /* the sums written, a word behind */
    P1 = R0;
    P4 = [P5 + 28];                     /* n */
    R7 = 10 (Z);                        /* EXTRACT: 10 bits from bit 0 */
    R3 = [P1];                          /* acc[-1]: the first pass stores it back */
    /* the first sample's a, b, f, w */
    R0 = R6 >> 10;
    P1 = R0;
    R2 = R2 >> 20;
    P2 = R2;
    R1 = EXTRACT (R6, R7.L) (Z);
    NOP;
    P1 = P0 + (P1 << 1);
    P2 = P3 + (P2 << 1);
    NOP;
    NOP;
    R4 = W[P1++] (X);
    R5 = W[P1] (X);
    R2 = W[P2] (X);
    P4 += -1;
    CC = P4 == 0;
    IF CC JUMP .Llast;
    LSETUP (.Lbeg, .Lend) LC0 = P4;
.Lbeg:
    R6 = I3;                                                    /* the next sample's pos */
    R0 = R6 >> 10 || I3 += M3 || [I1++] = R3;                   /* its index; pos on; the last sum stored */
    A1 = R5.L * R1.L (IS);                                      /* b f */
    P1 = R0;
    R3 = (A1 -= R4.L * R1.L) (IS);                              /* (b - a) f */
    R0 = I2;                                                    /* the next sample's wph */
    R1 = EXTRACT (R6, R7.L) (Z) || I2 += M2 || NOP;             /* its f; wph on */
    P1 = P0 + (P1 << 1);
    R3 = R3 >>> 10 || R6 = [I0++] || NOP;                       /* the sum so far */
    R0 = R0 >> 20;
    R3 = R3 + R4 (NS);                                          /* s */
    P2 = R0;
    R3 = R3.L * R2.L (IS) || R4 = W[P1++] (X) || NOP;           /* s w; the next a */
    P2 = P3 + (P2 << 1);
    R3 = R3 >>> 15 || R5 = W[P1] (X) || NOP;                    /* the next b */
.Lend:
    R3 = R3 + R6 (NS) || R2 = W[P2] (X) || NOP;                 /* the sum; the next w */
.Llast:
    NOP;
    NOP;
    NOP;
    /* the last sample: its sum stored after the one before */
    A1 = R5.L * R1.L (IS) || [I1++] = R3 || NOP;
    R3 = (A1 -= R4.L * R1.L) (IS) || R6 = [I0++] || NOP;
    R3 = R3 >>> 10;
    R3 = R3 + R4 (NS);
    R3 = R3.L * R2.L (IS);
    R3 = R3 >>> 15;
    R3 = R3 + R6 (NS);
    [I1++] = R3;
    R0 = I3;
    [P5] = R0;                          /* pos after the n samples */
    R0 = I2;
    [P5 + 8] = R0;                      /* wph */
    LB0 = [SP++];
    LT0 = [SP++];
    LC0 = [SP++];
    (R7:4, P5:3) = [SP++];
    RTS;
    .size _grain_loop_asm, .-_grain_loop_asm

#endif
#if defined(__linux__)
.section .note.GNU-stack,"",@progbits
#endif
