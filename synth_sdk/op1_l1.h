/* op1_l1.h: the L1 pool for plugins (op1_plugin.h includes it), Hundekraft kennel's API (op1r: l1pool.c, docs/67).
 * op1r's partitions: OP1_L1_SYNTH 8,192 B and OP1_L1_DRUM 512 B; OP1_L1_FX / OP1_L1_MASTER have no room (claims
 * return 0); op1_l1_sine_quarter() is the start of op1r's full L1 sine table (op1_sin). Kennel's text follows:
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef OP1_L1_H
#define OP1_L1_H
#include <stdint.h>
/* L1 pool (kennel cores from 2026-10-30, kennel/kn_l1.h): on-chip data SRAM (one cycle, no cache, no SDRAM) for a
 * plugin's hottest per-sample data, one partition per audio position:
 *   OP1_L1_SYNTH 8,192 B (the synth engine), OP1_L1_FX 8,320 B (the synth or drum FX: only one plays at a time),
 *   OP1_L1_MASTER 4,160 B (the master FX), OP1_L1_DRUM 512 B; op1_l1_fx_part(pos) picks FX or MASTER for an FX at pos;
 *   plus a shared read-only quarter of OP1_SINE_TABLE.
 * Every block it renders, a plugin calls op1_l1_claim(part, owner, bytes, &fresh) (owner: any address unique to the
 * plugin, e.g. its own state array; never op1_fx_scratch(pos), which every FX at the position shares).
 * It gets the partition (32-byte aligned) while it keeps claiming it, or 0: no pool
 * (Lander on, page 7's switch off, an older core), too big, or another plugin of that position still renders (release
 * tails after a switch); then it uses its SDRAM copy, so the output is the same either way. fresh = 1: the partition is
 * new to it (first claim, taken over, pool switched back on): copy the data in before using it. With op1_l1_claim only
 * constant tables and per-block scratch go there, never state that must survive losing the partition.
 * Persistent state (OP1_L1_API 2): op1_l1_claim_state(part, home, state_bytes, bytes, &fresh). `home` is the plugin's
 * own SDRAM copy of the state (word-aligned, state_bytes a multiple of 4) and also the owner. The state sits at the
 * partition's start, [0, state_bytes); the core copies home -> L1 when the plugin takes the partition and L1 -> home
 * exactly once when it loses it (another plugin takes over, page 7's switch goes off at a block's end, or the plugin
 * claims with another state_bytes), so the plugin never copies its state itself. Bytes [OP1_L1_ALIGN(state_bytes),
 * bytes) are the plugin's for tables and scratch as with op1_l1_claim (fresh = 1: copy the tables in). Returns 0:
 * work on `home` directly (it is then up to date). The rules (ARCHITECTURE.md "L1 pool"):
 *  - only code inside the audio callback touches the state: render, note_on (the voice loop calls it; R246's
 *    core_note_on also calls it on the key thread, and the core then moves no partition until it returns), an FX's init
 *    and render, an input engine's reset and process. Each of those calls op1_l1_claim_state first and uses what it
 *    returns (or home) for that call only: never keep the pointer across calls;
 *  - note_off (VoiceCtl's thread), screens and anything else on another thread never read or write the state: what
 *    they write (a gate flag, a request) and what a screen shows lives outside [home, home + state_bytes);
 *  - a reset (init, a restrike) writes through the claimed pointer like any other write.
 * op1_l1_sine_quarter(): OP1_SINE_TABLE[0..2048] (sin(2 pi k / 8192) x 32767, rounded) in L1, or 0 (use the table).
 * A file that imports these is refused by older cores (op1_l1_claim_state: cores from 2026-10-30, branch l1-state).
 * Plugin host builds get stubs that always return 0. */
#define OP1_L1_API 2
#define OP1_L1_ALIGN(n) (((uint32_t)(n) + 31u) & ~31u)   /* where the tables after a state start */
enum { OP1_L1_SYNTH, OP1_L1_FX, OP1_L1_MASTER, OP1_L1_DRUM, OP1_L1_PARTS };
static inline int op1_l1_fx_part(int pos) { return pos == 2 ? OP1_L1_MASTER : OP1_L1_FX; }
#if defined(OP1_HOST) && !defined(L1POOL_IMPL)           /* host builds: no pool (l1pool_test.c: the real one) */
static inline void *op1_l1_claim(int part, const void *owner, uint32_t bytes, int *fresh)
{ (void)part; (void)owner; (void)bytes; if (fresh) *fresh = 0; return 0; }
static inline void *op1_l1_claim_state(int part, void *home, uint32_t state_bytes, uint32_t bytes, int *fresh)
{ (void)part; (void)home; (void)state_bytes; (void)bytes; if (fresh) *fresh = 0; return 0; }
static inline const int16_t *op1_l1_sine_quarter(void) { return 0; }
#else
void *op1_l1_claim(int part, const void *owner, uint32_t bytes, int *fresh);
void *op1_l1_claim_state(int part, void *home, uint32_t state_bytes, uint32_t bytes, int *fresh);
const int16_t *op1_l1_sine_quarter(void);
#endif
#endif
