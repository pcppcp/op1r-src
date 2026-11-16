/* reedophone sample loops in Blackfin assembly: bit-exact with bore_c / radiate_c in reedophone.c (the reference;
 * tools/asm_check.c compares the two on the simulator). Included by reedophone.c on the Blackfin only.
 *
 * The two wave lines are one interleaved ring q[LN][2] = forward, back; every tap is a circular DAG pointer
 * (B = q, L = sizeof q), so no index masking. Sums run in the 40-bit accumulators and saturate on extraction.
 * io (rv_io_t, P0): k pairs  +0 B1|A1  +4 A2|zeta  +8 c3|c2  +12 c1|c0  +16 unused  +20 unused  +24 lpc|gq
 *                   +28 EQ b0|b1  +32 b2|-a1  +36 -a2|0  +40 LP b0|b1  +44 b2|-a1  +48 -a2|RADHP  +52 GAIN|0
 *                   +56 g  +60 out  +64 sqd  +68 q  +72 ip[4]  +88 x12  +92 pp  +96 lps  +100 hy ex ey lx ly */
#ifndef RV_ASM_H
#define RV_ASM_H
#include <stddef.h>
typedef char rv_io_layout[(offsetof(rv_io_t, g) == 56 && offsetof(rv_io_t, ip) == 72 && offsetof(rv_io_t, x12) == 88
                           && offsetof(rv_io_t, hy) == 100 && offsetof(rv_io_t, ly) == 116 && LN * 4 == 4096) ? 1 : -1];

#define RV_CLOBBERS "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P1", "P2", "P3", "P4", \
    "I0", "I1", "I2", "I3", "B0", "B1", "B2", "B3", "L0", "L1", "L2", "L3", "M0", "M1", "M2", "M3", \
    "A0", "A1", "LC0", "LT0", "LB0", "CC", "memory"

/* setup */
#define RV_BORE_HEAD \
    "P1 = [P0 + 56];\n\t"           /* g: breath in, (e + r) / 2 out */ \
    "P3 = [P0 + 64];\n\t"           /* sqrt table */ \
    "R0 = [P0 + 68];\n\t" \
    "B0 = R0; B1 = R0; B2 = R0; B3 = R0;\n\t" \
    "R0 = 4096 (X);\n\t"         /* sizeof q */ \
    "L0 = R0; L1 = R0; L2 = R0; L3 = R0;\n\t" \
    "R0 = [P0 + 72]; I0 = R0;\n\t"  /* unused */ \
    "R0 = [P0 + 76]; I1 = R0;\n\t"  /* back tap */ \
    "R0 = [P0 + 80]; I2 = R0;\n\t"  /* fractional read */ \
    "R0 = [P0 + 84]; I3 = R0;\n\t"  /* write */ \
    "M0 = 4 (X); M1 = -8 (X); M3 = 2 (X);\n\t" \
    "R4 = [P0 + 88]; R6 = [P0 + 92]; R5 = [P0 + 96];\n\t" \
    "P2 = 128 (X);\n\t"

/* reed, flow (R1.L = pmin on entry; leaves u, writes forward[w], pprev) */
#define RV_REED(AF_STORE) \
    "R3 = [P0 + 0];\n\t" \
    "A0 = R3.L * R6.L (IS);\n\t"    /* B1 pprev */ \
    "A0 += R3.H * R4.L (IS);\n\t"   /* A1 x1 */ \
    "R3 = [P0 + 4];\n\t" \
    "A0 += R3.L * R4.H (IS);\n\t"   /* A2 x2 */ \
    "A0 = A0 >>> 14;\n\t" \
    "R2.L = A0 (IS);\n\t"           /* x */ \
    "R4 = PACK(R4.L, R2.L);\n\t" \
    "R7 = 8192 (X);\n\t" \
    "R7 = R7 - R0;\n\t" \
    "R7.L = R7.L + R2.L (S);\n\t"   /* 1 - gamma + x */ \
    "A1 = R3.H * R7.L (IS);\n\t" \
    "A1 = A1 >>> 15;\n\t" \
    "R7.H = A1 (IS);\n\t" \
    "R7 >>>= 16;\n\t" \
    "R2 = 0;\n\t" \
    "R7 = MAX(R7, R2);\n\t"         /* A */ \
    "R1.H = R1.L + R1.L (S);\n\t" \
    "R1.H = R0.L - R1.H (S);\n\t"   /* V = gamma - 2 p- */ \
    "CC = BITTST(R1, 31);\n\t" \
    "R0 = ABS R1 (V);\n\t" \
    "R0 >>= 16;\n\t" \
    "R0 <<= 15;\n\t" \
    "R3 = R7.L * R7.L (IS);\n\t" \
    "R0 = R0 + R3;\n\t" \
    "R0 += 4;\n\t"                  /* A^2 + 4 |V| */ \
    "R3.L = SIGNBITS R0;\n\t" \
    "R3 = R3.L (X);\n\t" \
    "R3 += 1;\n\t" \
    "R3 >>>= 1;\n\t"                /* k */ \
    "R2 = R3 << 1;\n\t" \
    "R0 <<= R2;\n\t" \
    "R2 = R0 >> 24;\n\t" \
    "P4 = R2;\n\t" \
    "R0 <<= 8;\n\t" \
    "R0 >>= 17;\n\t"                /* f */ \
    "P4 = P3 + (P4 << 2);\n\t" \
    "R2 = [P4];\n\t" \
    "A0 = R2.H * R0.L (IS);\n\t" \
    "A0 = A0 >>> 15;\n\t" \
    "R0 = A0;\n\t" \
    "R2 = R2.L (Z);\n\t" \
    "R2 = R2 + R0;\n\t" \
    "R2 >>= R3;\n\t"                /* S = sqrt(A^2 + 4|V|) */ \
    "R2 = R2 - R7;\n\t" \
    "R0 = 0;\n\t" \
    "R2 = MAX(R2, R0);\n\t" \
    "R2 = R2.L * R7.L (FU);\n\t" \
    "R2 >>= 14;\n\t" \
    "R0 = -R2;\n\t" \
    "IF CC R2 = R0;\n\t"            /* u */ \
    "R0 = R1.L (X);\n\t" \
    "R3 = R0 + R2;\n\t" \
    "R0 = R0 + R3;\n\t" \
    "A0 = R3;\n\t" \
    "R3.L = A0 (IS);\n\t"           /* af = sat16(p- + u) */ \
    AF_STORE \
    "A1 = R0;\n\t" \
    "R0.H = A1 (IS);\n\t" \
    "R6 = PACK(R6.H, R0.H);\n\t"    /* pprev = sat16(2 p- + u) */

/* fractional read (I2 on the forward line), open end, output */
#define RV_END \
    "R3 = [P0 + 8];\n\t" \
    "R7.L = W[I2]; I2 += M0;\n\t" \
    "A0 = R7.L * R3.L (IS);\n\t" \
    "R7.L = W[I2]; I2 += M0;\n\t" \
    "A0 += R7.L * R3.H (IS);\n\t" \
    "R3 = [P0 + 12];\n\t" \
    "R7.L = W[I2]; I2 += M0;\n\t" \
    "A0 += R7.L * R3.L (IS);\n\t" \
    "R7.L = W[I2]; I2 += M1;\n\t" \
    "A0 += R7.L * R3.H (IS);\n\t" \
    "A0 = A0 >>> 14;\n\t" \
    "R7.L = A0 (IS);\n\t"           /* e */ \
    "R3 = [P0 + 24];\n\t" \
    "R2 = R5 >>> 15;\n\t" \
    "A1 = R5;\n\t" \
    "A1 += R7.L * R3.L (IS);\n\t" \
    "A1 -= R2.L * R3.L (IS);\n\t" \
    "R5 = A1;\n\t"                  /* end low-pass */ \
    "R2 = R5 >>> 15;\n\t" \
    "A0 = R2.L * R3.H (IS);\n\t" \
    "A0 = A0 >>> 15;\n\t" \
    "R2.L = A0 (IS);\n\t" \
    "R2 = -R2 (V);\n\t"             /* r */ \
    "W[I3++] = R2.L;\n\t" \
    "R0 = R7.L (X);\n\t" \
    "R2 = R2.L (X);\n\t" \
    "R0 = R0 + R2;\n\t" \
    "R0 >>>= 1;\n\t"

#define RV_BORE_TAIL \
    "[P0 + 88] = R4; [P0 + 92] = R6; [P0 + 96] = R5;\n\t" \
    "R0 = 0; L0 = R0; L1 = R0; L2 = R0; L3 = R0;\n\t"

static void bore_asm(rv_io_t *io)
{
    register rv_io_t *p0 __asm__("P0") = io;
    __asm__ volatile (
        RV_BORE_HEAD
        "LSETUP (.Lr1b%=, .Lr1e%=) LC0 = P2;\n"
        ".Lr1b%=:\n\t"
        "R0 = W[P1] (X);\n\t"
        "R1.L = W[I1];\n\t"
        "I1 += M0;\n\t"
        RV_REED("W[I3] = R3.L;\n\tI3 += M3;\n\t")
        RV_END
        ".Lr1e%=: W[P1++] = R0;\n\t"
        RV_BORE_TAIL
        : "+a"(p0) : : RV_CLOBBERS);
}

static void radiate_asm(rv_io_t *io)
{
    register rv_io_t *p0 __asm__("P0") = io;
    __asm__ volatile (
        "P1 = [P0 + 56];\n\t"
        "P2 = [P0 + 60];\n\t"
        "P4 = P1;\n\t"
        "P4 += -2;\n\t"
        "R2 = [P0 + 100]; R4 = [P0 + 104]; R5 = [P0 + 108]; R6 = [P0 + 112]; R7 = [P0 + 116];\n\t"
        "P3 = 128 (X);\n\t"
        "LSETUP (.Lrdb%=, .Lrde%=) LC0 = P3;\n"
        ".Lrdb%=:\n\t"
        "R0 = W[P1++] (X);\n\t"
        "R1 = W[P4++] (X);\n\t"
        "R0 = R0 - R1;\n\t"
        "R0 <<= 9;\n\t"
        "R2 = R2 + R0;\n\t"
        "R3 = [P0 + 48];\n\t"
        "R0 = R2 >>> 15;\n\t"
        "R1 = R2 << 17;\n\t"
        "R1 >>= 17;\n\t"
        "A0 = R0.L * R3.H (IS);\n\t"
        "A1 = R1.L * R3.H (IS);\n\t"
        "A1 = A1 >>> 15;\n\t"
        "A0 += A1;\n\t"
        "R2 = A0;\n\t"                 /* radiation high-pass */
        "A0 = A0 >>> 9;\n\t"
        "R0.L = A0 (IS);\n\t"
        "R3 = [P0 + 28];\n\t"
        "A0 = R0.L * R3.L (IS);\n\t"
        "A0 += R4.L * R3.H (IS);\n\t"
        "R3 = [P0 + 32];\n\t"
        "A0 += R4.H * R3.L (IS);\n\t"
        "A0 += R5.L * R3.H (IS);\n\t"
        "R3 = [P0 + 36];\n\t"
        "A0 += R5.H * R3.L (IS);\n\t"
        "A0 = A0 >>> 14;\n\t"
        "R1.L = A0 (IS);\n\t"          /* body EQ */
        "R4 = PACK(R4.L, R0.L);\n\t"
        "R5 = PACK(R5.L, R1.L);\n\t"
        "R3 = [P0 + 40];\n\t"
        "A0 = R1.L * R3.L (IS);\n\t"
        "A0 += R6.L * R3.H (IS);\n\t"
        "R3 = [P0 + 44];\n\t"
        "A0 += R6.H * R3.L (IS);\n\t"
        "A0 += R7.L * R3.H (IS);\n\t"
        "R3 = [P0 + 48];\n\t"
        "A0 += R7.H * R3.L (IS);\n\t"
        "A0 = A0 >>> 14;\n\t"
        "R0.L = A0 (IS);\n\t"          /* output low-pass */
        "R6 = PACK(R6.L, R1.L);\n\t"
        "R7 = PACK(R7.L, R0.L);\n\t"
        "R3 = [P0 + 52];\n\t"
        "A0 = R0.L * R3.L (IS);\n\t"
        "A0 = A0 >>> 8;\n\t"
        "R0.L = A0 (IS);\n\t"
        ".Lrde%=: W[P2++] = R0;\n\t"
        "[P0 + 100] = R2; [P0 + 104] = R4; [P0 + 108] = R5; [P0 + 112] = R6; [P0 + 116] = R7;\n\t"
        : "+a"(p0) : : RV_CLOBBERS);
}
#endif
