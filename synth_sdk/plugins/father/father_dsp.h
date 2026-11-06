/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission. */
#ifndef FATHER_DSP_H
#define FATHER_DSP_H
#include <stdint.h>
#include "father_lines.h"
/* The tank runs at half the FX rate (MW_N samples per callback). Every ring is exactly as long as its longest read,
   and each callback reads before it writes, so with R246's no-write-allocate data cache the stores land in lines the
   reads brought in (docs/father-redesign.md). The rings come first: they start on 32-byte lines when the state does. */
typedef struct {
 int16_t tank[MW_TANK_WORDS];      /* eight lines back to back, line i at mw_off[i], mw_len[i] samples */
 int16_t ap[MW_AP_WORDS];          /* four input allpasses, ring length == delay */
 int16_t pre[2][MW_PRE_LEN];       /* full-rate predelay + early-tap rings (a mono call uses pre[0] only) */
 /* per-callback scratch (rows .. erow), read in the same callback; not state */
 int16_t rows[2][MW_N][4];         /* the damped tank reads: line i at rows[i >> 2][n][i & 3] (the write-back's pairs) */
 int16_t in[2][MW_N];              /* the allpassed tank input, left / right */
 int16_t erow[2][64];              /* early reflections, full rate */
 uint32_t phase[8];
 int32_t damp[8],early[2];
 int32_t dec[2][5];                /* half-band decimator history per channel */
 int32_t up[2][3];                 /* interpolator history per channel */
 uint16_t hd[8],ap_hd[4],pre_hd;
 uint16_t pad;
 int32_t mix,pre_samples,peak;
 int32_t gate_env,gate_gain;
 uint32_t gate_hold;
 uint8_t gate_enabled,gate_active,gate_armed;
} mw_state;
/* the persistent part of two states is equal (the scratch rows excluded) */
#define MW_SAME_STATE(a,b) (!memcmp((a),(b),__builtin_offsetof(mw_state,rows))&&\
 !memcmp(&(a)->phase,&(b)->phase,sizeof(mw_state)-__builtin_offsetof(mw_state,phase)))
#define MW_CLEAR_WORDS 1024u       /* father.c clears the state this many words per callback after a select */
void mw_init(mw_state *s);
void mw_init_fields(mw_state *s);   /* the non-zero fields of a cleared state (FX_SPREAD_INIT) */
void mw_process(mw_state *s,int16_t *l,int16_t *r,const int32_t q[4]);
#endif
