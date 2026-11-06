/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission. */
#ifndef SISTER_DSP_H
#define SISTER_DSP_H
#include <stdint.h>
#include "sister_tables.h"

#define SI_PRE 4096          /* pre-delay / early-tap ring, power of two */
#define SI_AP 512            /* allpass rings (diffusers, in-loop), power of two */
#define SI_COMB 1024         /* pipe comb rings, power of two */
#define SI_CLEAR_WORDS 4096  /* ring words cleared per block after init (the wet path is silent meanwhile) */

typedef struct { int32_t in, out, agc, agc_prev; } si_follow_t;

/* The arrays of one block, each fully written before it is read within the block (nothing carries over): they may
   sit in si_state (wm) or in the L1 pool with the same output. o[4] and rsum[2] must stay contiguous (kernel strides). */
typedef struct {
 int16_t x[SI_N], erl[SI_N], err[SI_N], o[4][SI_N], tl[SI_N], tr[SI_N], rl[SI_N], rr[SI_N];
 int16_t dx[SI_N], rsum[2][SI_N];
 int32_t fl[SI_N], fr[SI_N];
 int16_t db[1 + 18 + 64 + 1];       /* decimator: [1] is the oldest of 18 history (copied in from si_state) + 64 new */
 int16_t ub[2][10 + SI_N];          /* interpolators: 10 history (copied in) + 32 new */
 int16_t out[2][64];
} si_ws;
/* The block parameters from the smoothed macros (the SPACE / VOICE / SWELL / MIX table lookups), cached under the
   macros they came from (key = sm[0..3]; key[0] = -1: invalid). A pure function of the key, so the copy in the L1
   pool needs no saving: it is marked invalid when the partition is fresh and recomputed. */
typedef struct {
 int32_t key[4];
 int32_t gains[4], damp, pre_t, win, depth, er_g;                       /* SPACE */
 int32_t g_choir, g_pipes, g_body, vdepth, vrate, qinv, klog, bright;   /* VOICE */
 int32_t g_fwd, g_rev, fade, delta;                                     /* SWELL */
 int32_t dg, wg, g_tilt;                                                /* MIX */
} si_par;
/* The constant tables a block reads (sister_tables.h): the SDRAM ones, or their copies in the L1 pool */
typedef struct {
 const int32_t (*fco)[2];
 const int32_t (*vowel)[3][2];
 const int32_t *root;
 const uint32_t *rate;
 const int16_t *hb, *hb2, *quarter, *exp2, *sq1mg, *sine;
 const uint16_t *line_len, *lap_len, *diff_len;
} si_tp;
typedef struct {                    /* the L1 copies (filled when fresh), with their pointers first */
 si_tp tp;
 int32_t fco[SI_FLOG_N][2], vowel[3][3][2], root[49];
 uint32_t rate[4];
 int16_t hb[6], hb2[6], exp2[258], sine[258], quarter[130], sq1mg[258];   /* even lengths: each starts 4-byte aligned */
 uint16_t line_len[4], lap_len[4], diff_len[4];
} si_l1tab;
#define SI_A32(n) (((n) + 31) & ~31u)
/* L1 pool layout (sister.c): si_ws at 0, si_par, the tables (copied in when fresh). SI_L1_CORE (no tables) fits the
   master partition (4,160 B), SI_L1_BYTES the synth / drum FX one (8,320 B). */
#define SI_L1_PAR  SI_A32(sizeof(si_ws))
#define SI_L1_TAB  (SI_L1_PAR + SI_A32(sizeof(si_par)))
#define SI_L1_CORE (SI_L1_TAB)
#define SI_L1_BYTES (SI_L1_TAB + sizeof(si_l1tab))

typedef struct {
#ifdef SI_PROF
 uint32_t prof[12];                 /* profiling builds: packets per stage, summed (budget_check.py --prof) */
#endif
 /* ---- scalar state: si_init sets it (cheap); the rings below are cleared lazily ---- */
 uint32_t clear_done;               /* ring words cleared so far */
 int32_t first, seeded;             /* smoothing starts at the knob values; seeded: first wet block */
 int32_t sm[4];                     /* smoothed macros, Q15 << 4 */
 int32_t key_cents;
 uint32_t pre_h; int32_t pre_d;     /* pre-delay, samples Q4 (slewed) */
 uint32_t ap_h;                     /* shared head of the allpass rings */
 int32_t line_h[4], line_d[4], damp[4], tdc[2]; uint32_t lfo[4];
 int32_t er_z[2];
 int32_t res[2][3];                 /* formant resonators: high y1, low y2 per band */
 int32_t res_x[2][2];               /* their input history x1, x2 */
 uint32_t vlfo[2]; int32_t vowel[2];
 uint32_t comb_h; int32_t comb_d[3], comb_z[3];
 si_follow_t pipe_lv, choir_lv;    /* resonator level followers */
 int32_t rev_h, anchor, prev_anchor;
 int32_t tau, prev_tau, win, prev_win, fade, prev_fade, delta, prev_delta;
 int32_t inv_fade, prev_inv_fade;
 /* per-block gains, kept for the next block's ramps */
 int32_t g_body, g_choir, g_pipes, g_fwd, g_rev, g_dry, g_wet, g_er, g_tilt, lim;
 int32_t tilt_z[2], dc[2];
 int32_t peak, wet_level;           /* telemetry: wet block peak, slow wet level */
 /* ---- per-block workspace (in the scratch, not the ISR stack) ---- */
 int16_t dh[18], uh[2][10];         /* decimator and interpolator history (the working buffers are si_ws db / ub) */
 const si_tp *t;                     /* the tables in use this block: the SDRAM ones or the L1 copies */
 si_ws *w;                          /* the block's arrays: wm, or the L1 pool's copy (si_process_l1) */
 si_par *par;                       /* the block parameters: parm, or the L1 pool's copy */
 si_par parm;
 si_ws wm;
 /* ---- rings (cleared SI_CLEAR_WORDS per block after init) ---- */
 int16_t pre[SI_PRE];
 int16_t diff[4][SI_AP];
 int16_t lap[4][SI_AP];
 int16_t comb[3][SI_COMB];
 int16_t line[SI_LINES_TOTAL];
 int16_t rev[SI_REV];
} si_state;

/* per FX position (sister.c): the knob conversion cache and the screen's telemetry in one 32-byte line */
typedef struct {
 int16_t raw[4], norm[4], raw_key, key_cents;   /* the last knob words and their conversions (the range maps divide) */
 volatile int16_t peak, level, vowel[2];        /* telemetry, once per block: wet block peak, its slow level, vowels */
 uint8_t valid, pad;
} __attribute__((aligned(32))) si_pos_t;
extern si_pos_t sister_pos[3];

void si_init(si_state *s);
/* l/r: 64 samples in place; r = 0 for the mono (synth/drum) call. q: macros 0..32767. key_cents: -2400..2400. */
void si_process(si_state *s, int16_t *l, int16_t *r, const int32_t q[4], int32_t key_cents);
/* the same with the L1 pool's partition (pool_bytes: SI_L1_BYTES, or SI_L1_CORE: the tables stay in SDRAM; fresh:
   copy the tables in), or pool = 0 */
void si_process_l1(si_state *s, int16_t *l, int16_t *r, const int32_t q[4], int32_t key_cents, void *pool, uint32_t pool_bytes, int fresh);
#endif
