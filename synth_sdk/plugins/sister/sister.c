/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission. */
#include "op1_plugin.h"
#include "sister_dsp.h"
_Static_assert(sizeof(si_state) <= OP1_FX_SCRATCH_BYTES, "SISTER state must fit the FX scratch");
_Static_assert(SI_L1_BYTES <= 8320 && SI_L1_CORE <= 4160, "SISTER's L1 layout must fit the FX and master partitions (op1_l1.h)");
si_pos_t sister_pos[3];
_Static_assert(sizeof(si_pos_t) == 32, "one line per position");
static si_state *state(int p) { return (si_state *)op1_fx_scratch(p); }
static void init(int p) { si_state *s = state(p); if (s) si_init(s); sister_pos[p].valid = 0; }
/* page-2 green (word 6): the PIPES key, -24..+24 semitones from C3 (OP1_PRESS_PAGE_SEMI). Word 0 is an unset
   word (older sounds, a fresh master slot) and means C3. */
static int32_t key_of(int16_t w) { return w <= 0 ? 0 : op1_semi_cents(w); }
static void stereo(int p, int16_t *l, int16_t *r, const int16_t *params)
{
 si_state *s = state(p);
 if (!s) return;
 si_pos_t *g = &sister_pos[p];
 int32_t q[4];
 /* the range conversion divides: cache each unchanged word (Father's pattern) */
 for (unsigned i = 0; i < 4; i++) {
  if (!g->valid || g->raw[i] != params[i]) { g->raw[i] = params[i]; g->norm[i] = (int16_t)op1_fx_param_q15(params, i); }
  q[i] = g->norm[i];
 }
 if (!g->valid || g->raw_key != params[6]) { g->raw_key = params[6]; g->key_cents = (int16_t)key_of(params[6]); }
 g->valid = 1;
 void *pool = 0; int fresh = 0;
 const uint32_t bytes = p == 2 ? SI_L1_CORE : SI_L1_BYTES;   /* the master partition is smaller */
#ifdef __bfin__          /* L1 pool: the block arrays, the parameters and the tables, per instance */
 /* owner: this plugin's own per-position line, not the state. The state is R246's FX scratch, which every FX at the
    position shares (Warble claims with it too), so after Warble the core saw no takeover, fresh stayed 0 and the
    tables were never copied in: s->t->hb read the old L1 contents (alignment fault in si_k_decim, 2026-11-01) */
 pool = op1_l1_claim(op1_l1_fx_part(p), g, bytes, &fresh);
#endif
 si_process_l1(s, l, r, q, g->key_cents, pool, bytes, fresh);
 g->peak = (int16_t)s->peak; g->level = (int16_t)s->wet_level;
 g->vowel[0] = (int16_t)s->vowel[0]; g->vowel[1] = (int16_t)s->vowel[1];
}
static void mono(int p, int16_t *b, const int16_t *params) { stereo(p, b, 0, params); }
const op1_fx_stereo_t FX_sister = { { init, mono, { OP1_PRESS_NONE, OP1_PRESS_PAGE_SEMI, OP1_PRESS_NONE, OP1_PRESS_NONE } }, stereo };
