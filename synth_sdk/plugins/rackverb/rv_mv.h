/* rackverb: the Midiverb-I-class machine (4 ops, 128 steps a sample, a 16 K x 16-bit ring) and the generator that
 * writes our own programs for it from a program row (rv_programs.h). Written from the published description of the
 * machine (Eric Brombaugh's MIDIVerb_RE slides, MIT; thement's midiverb_emulator, GPL-3.0); no ROM or ROM-derived
 * program is used. Design: OP1-Emulator docs/midiverb-design-2026-11-04.md.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef RV_MV_H
#define RV_MV_H
#include <stdint.h>

#define RV_RING  16384                 /* words */
#define RV_STEPS 128
#define RV_IO_IN 0                     /* step 0 writes the ADC word */
#define RV_IO_R  0x60                  /* steps 0x60 / 0x70 send their word to the right / left DAC */
#define RV_IO_L  0x70
#define RV_LFOS  4                     /* modulated taps a program may have */
#define RV_BLOCK_MIN 48                /* the most machine samples a call: delays from here up run block-wise */
enum { RV_SUM, RV_LD, RV_STP, RV_STN };

/* program row (rv_programs.h, written by gen_programs.py from the program names) */
enum { RV_DEFEAT, RV_REV, RV_GATE, RV_REVERSE, RV_ECHO, RV_TAPS, RV_MOD, RV_THICK, RV_SGEN, RV_BLOOM };
enum { RV_FLAT, RV_HPF, RV_BPF, RV_LPF };            /* echo / tap filters */
enum { RV_AMBI, RV_WIDE, RV_THICKS, RV_PAN };       /* echo / tap stereo */
typedef struct {
    uint8_t shape;
    uint8_t a, b, c;                   /* REV: size 0..3 (small..xlarge), tone 0 bright 1 warm 2 dark, diffusion 0..2
                                          GATE / REVERSE / BLOOM: -, regen 0..15 (1/16), taps' rise 0 flat..3 steep
                                          ECHO / TAPS: taps 1..8, filter RV_FLAT.., stereo RV_AMBI..
                                          MOD: voices 1..2, feedback 0..15 (signed 4 bit: 8..15 = -8..-1), trig 0/1
                                          THICK / SGEN: width 0..3, -, - */
    uint16_t t;                        /* REV: RT60 in 10 ms; GATE / REVERSE / BLOOM: window ms; ECHO / TAPS: time ms;
                                          MOD: base delay in 0.01 ms; THICK / SGEN: spread ms */
    uint16_t u;                        /* REV: pre-delay ms; ECHO: regeneration 0..15; MOD: depth in 0.01 ms;
                                          BLOOM: RT60 in 10 ms */
    uint16_t v;                        /* MOD: LFO rate in mHz; ECHO / TAPS: low-pass/high-pass corner hint 0..3 */
    char name[24];
} rv_row_t;

typedef struct {                       /* a modulated tap: RV_LFO_READS steps reading line[w - d], patched per call */
    uint8_t step, lfo;                 /* first step of the group, LFO 0/1 */
    uint16_t w;                        /* the line's write address */
    int32_t d4, depth4;                /* base delay and depth in 1/4 samples (at SIZE 100 %) */
} rv_mod_t;
#define RV_LFO_READS 4                 /* a modulated read: k reads at d + 1, 4 - k at d: quarter-sample steps */

typedef struct {
    /* the program: per step the op, the line's write address and the read delay at SIZE 100 % */
    uint8_t op[RV_STEPS];
    uint16_t w[RV_STEPS], d[RV_STEPS];
    uint16_t p[RV_STEPS];              /* address = (p + n) & mask, from w, d and SIZE (rv_mv_size) */
    uint32_t e[RV_STEPS];              /* the kernel's form: p | keep acc << 16 | store << 17 | invert << 18 */
    rv_mod_t mod[RV_LFOS];
    uint8_t nmod, used, trig;          /* modulated groups, steps used (of 125), triggered LFO */
    uint8_t kind[RV_STEPS];            /* block engine: 0 a step, 1 a low-pass group's first step (4 steps), 2 inside
                                          one, 3 skipped (the I/O steps, the pads) */
    uint8_t act[RV_STEPS], nact;       /* the steps that do something (not the I/O steps, not the pads) */
    uint8_t chunk;                     /* block engine: samples a pass, at most the shortest read delay (rv_mv_size) */
    uint16_t ring_used;
    int32_t lfo_ph[2], lfo_inc[2];     /* triangle LFOs: phase 0..65535 up and down, per machine sample in Q16 */
    int32_t size_q15;
} rv_prog_t;

typedef struct {
    int16_t ring[RV_RING];             /* 32 KB; keep it first (32-byte aligned with the state) */
    rv_prog_t prog;
    int16_t acc;
    uint16_t n;                        /* pass counter, the ring's base */
    int16_t row[RV_BLOCK_MIN + 2] __attribute__((aligned(4)));   /* the block engine's acc row */
} rv_mv_t;

/* fs2: the machine rate x 2 in Hz (46875 for 23,437.5 Hz, 62500 for 31,250 Hz). -> steps used, -1 if it doesn't fit */
int rv_mv_build(rv_prog_t *p, const rv_row_t *row, int32_t fs2);
void rv_mv_size(rv_prog_t *p, int32_t size_q15);     /* SIZE 50..100 %: every read delay > 1 scales; then p[] */
void rv_mv_lfo(rv_prog_t *p, int samples);           /* advance the LFOs one call and patch the modulated reads */
void rv_mv_trigger(rv_prog_t *p);                    /* a triggered flange: restart the sweep */
void rv_mv_run(rv_mv_t *m, const int16_t *in, int16_t *l, int16_t *r, int count);   /* the kernel (branch-free) */
void rv_mv_run_steps(rv_mv_t *m, const int16_t *in, int16_t *l, int16_t *r, int count);   /* branch-free, a sample at a time */
void rv_mv_run_ref(rv_mv_t *m, const int16_t *in, int16_t *l, int16_t *r, int count);   /* the plain reference */
#endif
