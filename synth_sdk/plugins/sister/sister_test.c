/* SISTER host test (sanitizers): exact dry at MIX 0, silence stays silence, tails end in exact silence,
 * per-position state, cached knob conversion, every knob corner on full-scale input, the kernel C references.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
 *   cc -O1 -g -fsanitize=address,undefined -I.. -I. sister_test.c sister_dsp.c sister_kern.c -o sister_test */
#define OP1_HOST
#define op1_fx_scratch unused_host_scratch
#define op1_fx_param_q15 unused_host_q15
#include "op1_plugin.h"
#undef op1_fx_scratch
#undef op1_fx_param_q15
#include "sister_dsp.h"
#include "sister_kern.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static si_state inst[3];
static unsigned conversions;
static void *op1_fx_scratch(int p) { return &inst[p]; }
static int32_t op1_fx_param_q15(const int16_t *p, int i) { conversions++; return p[i] < 0 ? 0 : p[i]; }
#include "sister.c"

static uint32_t rng = 1;
static int16_t rnd(void) { rng = rng * 1664525u + 1013904223u; return (int16_t)(rng >> 16); }
static si_state s;

int main(void)
{
 _Static_assert(sizeof(si_state) <= OP1_FX_SCRATCH_BYTES, "scratch");
 assert(SI_KERN_OFFSETS_OK || sizeof(void *) != 4);
 int16_t l[64], r[64], a[64], b[64];
 int32_t q[4];

 /* MIX 0: the output is the input, sample for sample, stereo and mono, with the other knobs moving */
 si_init(&s);
 for (unsigned k = 0; k < 3000; k++) {
  q[0] = (int32_t)(k * 37u & 32767); q[1] = (int32_t)(k * 101u & 32767); q[2] = (int32_t)(k * 53u & 32767); q[3] = 0;
  for (unsigned i = 0; i < 64; i++) { a[i] = l[i] = rnd(); b[i] = r[i] = rnd(); }
  si_process(&s, l, k & 1 ? r : 0, q, (int32_t)(k % 4800) - 2400);
  assert(!memcmp(l, a, sizeof l));
  if (k & 1) assert(!memcmp(r, b, sizeof r));
 }
 puts("MIX 0 exactly dry (3000 blocks, mono and stereo, other knobs and key moving): PASS");

 /* silence in, silence out, from a cleared state, whatever the knobs do */
 si_init(&s);
 for (unsigned k = 0; k < 4000; k++) {
  for (unsigned j = 0; j < 4; j++) q[j] = (int32_t)((k * (j + 17) * 193u) & 32767);
  memset(l, 0, sizeof l); memset(r, 0, sizeof r);
  si_process(&s, l, r, q, (int32_t)(k % 4800) - 2400);
  for (unsigned i = 0; i < 64; i++) assert(!l[i] && !r[i]);
 }
 puts("silence stays silent (4000 blocks, knobs moving): PASS");

 /* a tail ends in exact silence (no limit cycle), at every VOICE end, short and long SPACE, SWELL 0 and 99 */
 for (unsigned c = 0; c < 12; c++) {
  si_init(&s);
  q[0] = c & 1 ? 32767 : 0; q[1] = (int32_t)((c >> 1) % 3) * 16383; q[2] = c >= 6 ? 32767 : 0; q[3] = 32767;
  unsigned last = 0;
  const unsigned blocks = c & 1 ? 40000 : 12000;   /* RT60 13 s at SPACE 99 */
  for (unsigned k = 0; k < blocks; k++) {
   for (unsigned i = 0; i < 64; i++) l[i] = r[i] = k >= 20 && k < 60 ? rnd() / 2 : 0;
   si_process(&s, l, r, q, 0);
   for (unsigned i = 0; i < 64; i++) if (l[i] || r[i]) last = k;
  }
  printf("  space %s voice %5d swell %s: last non-zero block %u (%.1f s)\n", c & 1 ? "99" : " 0", q[1], q[2] ? "99" : " 0",
         last, last * 64 / 44100.0);
  assert(last + 400 < blocks);
 }
 puts("tails end in exact silence: PASS");

 /* every knob corner (and key extremes) on full-scale alternating input: sanitizers watch every tap */
 int peak = 0;
 for (unsigned e = 0; e < 64; e++) {
  for (unsigned j = 0; j < 4; j++) q[j] = (e & (1u << j)) ? 32767 : 0;
  const int32_t key = e & 16 ? 2400 : e & 32 ? -2400 : 0;
  si_init(&s);
  for (unsigned k = 0; k < 2500; k++) {
   for (unsigned i = 0; i < 64; i++) { l[i] = k < 300 ? (i & 1 ? 32767 : -32768) : 0; r[i] = k < 300 ? (i & 2 ? 32767 : -32768) : 0; }
   si_process(&s, l, (e & 1) ? 0 : r, q, key);
   if (q[3] == 32767 && k > 20)                      /* wet only: the wet path's own level */
    for (unsigned i = 0; i < 64; i++) { int v = l[i] < 0 ? -l[i] : l[i]; if (v > peak) peak = v; }
  }
 }
 printf("64 corners on full-scale alternating input: PASS (wet peak %d = %.2f FS)\n", peak, peak / 32768.0);
 assert(peak < 32767);

 /* positions are independent; cached knob conversion is exact; init restarts a position */
 int16_t params[3][8] = { { 1000, 0, 0, 12000, 0, 0, 16384, 0 }, { 18000, 7000, 24000, 20000, 0, 0, 0, 0 },
                          { 32000, 32767, 30000, 32000, 0, 0, 20000, 0 } };
 for (int p = 0; p < 3; p++) FX_sister.mono.init(p);
 static si_state ref[3];
 for (int p = 0; p < 3; p++) si_init(&ref[p]);
 unsigned expected = 0;
 for (unsigned blk = 0; blk < 1500; blk++)
  for (int p = 0; p < 3; p++) {
   if (blk == 750 && p == 2) { FX_sister.mono.init(p); si_init(&ref[p]); expected += 4; }
   if (!blk) expected += 4;
   if (blk % 13 == 0 && blk) { unsigned k = (blk / 13) % 4; int16_t v = (int16_t)((unsigned)rnd() >> 1); if (v != params[p][k]) { params[p][k] = v; expected++; } }
   for (int i = 0; i < 64; i++) { a[i] = l[i] = rnd() / 4; b[i] = r[i] = rnd() / 4; }
   for (int k = 0; k < 4; k++) q[k] = params[p][k];
   const int32_t key = params[p][6] <= 0 ? 0 : op1_semi_cents(params[p][6]);
   if (p == 2) { FX_sister.render_stereo(p, l, r, params[p]); si_process(&ref[p], a, b, q, key); assert(!memcmp(r, b, sizeof r)); }
   else { FX_sister.mono.render(p, l, params[p]); si_process(&ref[p], a, 0, q, key); }
   assert(!memcmp(l, a, sizeof l));
   assert(conversions == expected);
  }
 printf("4500 synth/drum/master blocks equal a separate state each; %u conversions: PASS\n", conversions);
 puts("SISTER host test PASS");
 return 0;
}
