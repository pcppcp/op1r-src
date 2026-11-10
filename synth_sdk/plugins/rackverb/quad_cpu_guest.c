/* rackverb's per-block cost on bcore with the cache / SDRAM model (quad_cpu.py): the whole FX (rackverb.c's render,
 * the wrapper included) at one position, its scratch at the unit's address for that position (master 0x01B0EE48),
 * QUAD's state in that position's fast lease (heap 0, 0x00800000 + pos x 224 KiB) or, QC_FAST 0, its big-buffer lease
 * (0x02C00000 + pos MiB), a held chord (three saws) feeding it.
 * Every block: CYCLES around the one render call (packets + the model's stalls of the variant CYCLES follows, cold:
 * both caches flushed at blk_end), then blk_end. Per program: the blocks of a program change (the 6 held calls, the
 * fade, the clear, the build: "chg") and the steady blocks after it ("run": the next STEADY blocks); prints
 * "P unit prog chg_max run_max run_avg hash" per program, cycles, and an FNV-1a hash of every output sample so far
 * (bit-exactness before / after a change: quad_cpu.py --json, then --compare).
 * Configuration (-D from quad_cpu.py): QC_POS, QC_UNIT, QC_FIRST, QC_LAST, QC_STEP, QC_STEADY, QC_SIZE (0 still, 1
 * swinging), QC_FAST (1: op1_fx_fast gives the heap-0 lease), QC_L1 (1: op1_l1_claim / _state hand out the l1
 * stand-in, as the unit's pool), QC_POISON (op1_l1_claim
 * fills the partition with junk first: the rows must be written before read; costs cycles, for --compare only).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include <stdint.h>
#include "op1_plugin.h"

void t2_write(const char *, int);
uint32_t t2_r246_calls[2];
extern const op1_fx_stereo_t FX_rackverb;

static const struct { uint32_t pos, unit, first, last, step, steady, size, l1; } cfg =
    { QC_POS, QC_UNIT, QC_FIRST, QC_LAST, QC_STEP, QC_STEADY, QC_SIZE, QC_L1 };

volatile uint8_t op1_fx_wrap[3][4];
void *op1_fx_big(int pos, uint32_t *size) { *size = 0x00100000u; return (void *)(0x02C00000u + (uint32_t)(pos % 3) * 0x00100000u); }
/* the core's fast leases in heap 0 (kn_mem.h KN_FX_FAST_SIZE a position, 16 KB aligned: the pool lease's cache sets),
 * QC_FAST 0: none, as a core without heap 0 room (QUAD's state then in op1_fx_big) */
void *op1_fx_fast(int pos, uint32_t *size)
{
 if (!QC_FAST) { *size = 0; return 0; }
 *size = 0x00038000u; return (void *)(0x00800000u + (uint32_t)(pos % 3) * 0x00038000u);
}
/* the L1 pool's partitions (kn_mem: FX 8,320 B, MASTER 4,160 B): one stand-in, counted as L1 by quad_cpu.py */
uint8_t l1_mem[8320] __attribute__((aligned(32)));
static const void *l1_owner;
static uint32_t l1_state;
static uint32_t l1_cap(int part) { return part == OP1_L1_MASTER ? 4160u : part == OP1_L1_FX ? 8320u : 0u; }
void *op1_l1_claim(int part, const void *owner, uint32_t bytes, int *fresh)
{
 if (!cfg.l1 || bytes > l1_cap(part)) { if (fresh) *fresh = 0; return 0; }
#ifdef QC_POISON                                         /* scratch claims: junk in the partition every call */
 { static uint32_t r = 12345; uint32_t *w = (uint32_t *)l1_mem; for (uint32_t i = 0; i < bytes / 4; i++) { r = r * 1664525u + 1013904223u; w[i] = r; } }
#endif
 if (fresh) *fresh = owner != l1_owner;
 l1_owner = owner;
 return l1_mem;
}
void *op1_l1_claim_state(int part, void *home, uint32_t state_bytes, uint32_t bytes, int *fresh)
{
 if (!cfg.l1 || bytes > l1_cap(part)) { if (fresh) *fresh = 0; return 0; }
 int f = home != l1_owner || state_bytes != l1_state;
 if (f) { const uint32_t *h = home; uint32_t *d = (uint32_t *)l1_mem; for (uint32_t i = 0; i < state_bytes / 4; i++) d[i] = h[i]; }
 if (fresh) *fresh = f;
 l1_owner = home; l1_state = state_bytes;
 return l1_mem;
}
const int16_t *op1_l1_sine_quarter(void) { return 0; }

static char line[16384];
static int n;
static void put(const char *p) { while (*p && n < (int)sizeof line - 1) line[n++] = *p++; }
static void num(uint32_t v) { char b[11]; int i = 10; b[i] = 0; do b[--i] = (char)('0' + v % 10); while (v /= 10); put(b + i); }
static inline uint32_t cyc(void) { uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c)); return c; }
__attribute__((noinline)) void blk_end(void) { __asm__ volatile ("nop;"); }

static int16_t L[64], R[64], params[8];
static uint32_t ph[3];
static uint32_t blocks, hash = 2166136261u;
static void mix(const int16_t *b) { uint32_t h = hash; for (int i = 0; i < 64; i++) h = (h ^ (uint16_t)b[i]) * 16777619u; hash = h; }
static void chord(void)                                  /* C3 E3 G3 saws, about -10 dBFS each, stereo the same */
{
 static const uint32_t inc[3] = { 12596000u, 15870000u, 18874000u };
 for (int i = 0; i < 64; i++) {
  int32_t v = 0;
  for (int k = 0; k < 3; k++) { ph[k] += inc[k]; v += (int32_t)ph[k] >> 19; }
  L[i] = R[i] = (int16_t)(v * 3);
 }
}
static uint32_t blk_render(void)
{
 chord();
 if (cfg.size) params[5] = (int16_t)((blocks & 64) ? 32767 : 1);   /* SIZE swinging between its ends */
 uint32_t c0 = cyc();
 if (cfg.pos == 2) FX_rackverb.render_stereo(2, L, R, params); else FX_rackverb.mono.render((int)cfg.pos, L, params);
 uint32_t c = cyc() - c0;
 mix(L); if (cfg.pos == 2) mix(R);
 blocks++;
 blk_end();
 return c;
}
static int16_t prog_word(uint32_t prog) { return (int16_t)((prog * 32768u + 16384u) / 100u); }

int main(void)
{
 static uint32_t scratch[3][(OP1_FX_SCRATCH_BYTES + 3) / 4] __attribute__((unused));
 ((void **)0x01AB8BA0)[0] = (void *)0x01AE5000u;     /* the synth / drum / master positions' scratch (unit layout) */
 ((void **)0x01AB8BA0)[1] = (void *)0x01AE5000u;
 ((void **)0x01AB8BA0)[2] = (void *)0x01B0EE48u;
 params[0] = 0; params[1] = 31763; params[2] = 16384; params[5] = 32767; params[6] = 0;
 params[7] = (int16_t)(cfg.unit * 8192u + 4096u);
 params[3] = prog_word(cfg.first);
 if (cfg.pos == 2) FX_rackverb.mono.init(2); else FX_rackverb.mono.init((int)cfg.pos);
 for (int k = 0; k < 120; k++) blk_render();                 /* the first build */
 for (uint32_t p = cfg.first; p <= cfg.last; p += cfg.step) {
  params[3] = prog_word(p);
  uint32_t chg = 0, mx = 0, sum = 0;
  for (int k = 0; k < 6 + 1 + 30 + 2; k++) { uint32_t c = blk_render(); if (c > chg) chg = c; }   /* held, fade, clear, build */
  for (uint32_t k = 0; k < cfg.steady; k++) { uint32_t c = blk_render(); if (c > mx) mx = c; sum += c; }
  put("P "); num(cfg.unit); put(" "); num(p); put(" "); num(chg); put(" "); num(mx); put(" "); num(sum / cfg.steady); put(" "); num(hash); put("\n");
 }
 put("END\n");
 t2_write(line, n);
 return 0;
}
