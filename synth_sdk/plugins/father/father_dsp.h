/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission. */
#ifndef FATHER_DSP_H
#define FATHER_DSP_H
#include <stdint.h>
#include "father_lines.h"
#define MW_IN (2*MW_N)                 /* 22,050 Hz samples per call */
#define MW_NC (2*MW_N)                 /* CLASSIC (3.1.x): tank samples per call (the design 2x fast, see below) */
/* R246 calls an FX with 64 samples at 44,100 Hz. Father's design runs at 22,050 Hz inside (MW_IN samples per call:
   the input decimated 2:1 into the predelay ring, the early reflections, the wet output interpolated back) and the
   tank at 11,025 Hz (MW_N samples per call), so every delay, decay and filter constant keeps its design time.
   The gate detector and the predelay slew run once per two calls, the design's 2.9 ms callback. Every ring is exactly as long as its longest read,
   and each callback reads before it writes, so with R246's no-write-allocate data cache the stores land in lines the
   reads brought in (docs/father-redesign.md). The rings come first: they start on 32-byte lines when the state does.
   CLASSIC (classic = 1, white's page 2): hundekraft 3.1.x's Father bit for bit, the design 2x fast as it ran on the
   unit before 2026-11-03: no input decimation (the predelay and early reflections at 44,100 Hz, 64 a call), the tank
   at 22,050 Hz (MW_NC a call), no wet interpolation, the detector and slew every call, the gate's 22,050 Hz steps.
   The rings and every table are the same in both modes; only the per-call counts change. */
typedef struct {
 int16_t tank[MW_TANK_WORDS];      /* eight lines back to back, line i at mw_off[i], mw_len[i] samples */
 int16_t ap[MW_AP_WORDS];          /* four input allpasses, ring length == delay */
 int16_t pre[2][MW_PRE_LEN];       /* 22,050 Hz predelay + early-tap rings (a mono call uses pre[0] only) */
 /* per-callback scratch (rows .. erow), read in the same callback; not state */
 int16_t rows[2][MW_NC][4];         /* the damped tank reads: line i at rows[i >> 2][n][i & 3] (the write-back's pairs) */
 int16_t in[2][MW_NC];             /* the allpassed tank input, left / right */
 int16_t erow[2][64];              /* the wet: early + late at 22,050 Hz in [MW_IN..], then 64 at 44,100 Hz in place
                                      (CLASSIC: early + late at 44,100 Hz in [0..63]) */
 uint32_t phase[8];
 int32_t damp[8],early[2];
 int32_t dec[2][5];                /* half-band decimator history per channel */
 int32_t up[2][3];                 /* the late sum's interpolator history per channel */
 int32_t front[2][5];              /* the input's 2:1 decimator history per channel */
 int32_t up2[2][3];                /* the wet's 2x interpolator history per channel */
 int32_t gate_peak;                /* the input peak of the first call of a pair */
 uint16_t hd[8],ap_hd[4],pre_hd;
 uint16_t tick;                    /* 1 on the second call of a pair (the detector and slew run) */
 int32_t mix,pre_samples,peak;
 int32_t gate_env,gate_gain;
 uint32_t gate_hold;
 uint8_t gate_enabled,gate_active,gate_armed;
 uint8_t classic;                  /* 1: CLASSIC (set after a clear, father.c; a mode change clears the state) */
} mw_state;
/* the persistent part of two states is equal (the scratch rows excluded) */
#define MW_SAME_STATE(a,b) (!memcmp((a),(b),__builtin_offsetof(mw_state,rows))&&\
 !memcmp(&(a)->phase,&(b)->phase,sizeof(mw_state)-__builtin_offsetof(mw_state,phase)))
#define MW_CLEAR_WORDS 1024u       /* father.c clears the state this many words per callback after a select */
void mw_init(mw_state *s);
void mw_init_fields(mw_state *s);   /* the non-zero fields of a cleared state (FX_SPREAD_INIT) */
void mw_process(mw_state *s,int16_t *l,int16_t *r,const int32_t q[4]);
#endif
