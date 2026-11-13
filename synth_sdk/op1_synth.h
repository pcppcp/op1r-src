/* SPDX-License-Identifier: GPL-3.0-or-later
 * op1r core header (private): the plugin API (op1_plugin.h) plus firmware helpers used only by core code.
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef OP1_SYNTH_H
#define OP1_SYNTH_H
#include "op1_plugin.h"

/* ---- writing long SQL statements safely ----
 * db2::exec (0x0196F950) asserts on any sqlite error (0x01919AA8, "db2.cpp:165 %s\n%s", bounded to 256 bytes). The
 * assert's stack printer (0x0191995C) follows FP as a frame chain; our -O2 code uses FP as a normal register (here:
 * the dots SQL buffer), so it read SQL text as a frame and faulted (user report: SEQSTAT 0x24 at 0x019199C0) instead
 * of showing the assert. The sqlite error itself was "out of memory" (fixed heap, page cache never capped: see
 * sqlite_cache_hook in dispatcher.c). This calls sqlite3_exec directly under db2's own lock and returns the error
 * code instead of asserting. op1_db_last_err keeps the last nonzero code (shown on the monitor page). */
extern int32_t op1_db_last_err;
#ifndef OP1_HOST
static inline int op1_db_write(void *db2_handle, const char *sql)
{
    void *sem = *(void *volatile *)0x01BAB718;                    /* db2 statement lock (created on the first exec) */
    if (sem) ((void (*)(void *, int))0xFFA0B6E8)(sem, 0);
    int rc = ((int (*)(void *, const char *, void *, void *, char **))0x019C9460)(*(void **)db2_handle, sql, 0, 0, 0);
    if (sem) ((void (*)(void *))0xFFA0B96C)(sem);
    if (rc) op1_db_last_err = rc;
    return rc;
}
#endif

/* safe mode (safeboot.c): set at the start of the boot, never changed after. Nonzero: no plugin files, op1r's own data
 * (settings, markers, MIDI sequencer file) neither read nor written this boot */
enum { SB_ON = 1, SB_RESET = 2 };                                  /* SB_RESET: op1rsettings cleared at this boot */
#ifdef OP1_HOST
__attribute__((weak)) volatile uint8_t op1r_safe_boot;            /* host tests set it */
#else
extern volatile uint8_t op1r_safe_boot;
#endif

#ifndef OP1_HOST
/* firmware text (stock font, docs/18): object of OP1_TEXT_SIZE bytes, built once (GUI thread, after boot);
 * x in Q10, y in px, size 12 = the battery page's text (template: BatteryScreen 0x0193D248 / draw 0x0193CFD8) */
#define OP1_TEXT_SIZE       0xA4
#define op1_text_ctor(obj, x_q10, y, size, rgba) \
    (((void *(*)(void *, int32_t, int32_t, int32_t, const uint8_t *, int32_t))0x01A61904)((obj), (x_q10), (y), (size), (rgba), 0))
#define op1_text_set(t, str) (((void (*)(void *, const char *))0x01A61AAC)((t), (str)))
#define op1_text_draw(t, r, m9) (((void (*)(void *, void *, const int32_t *))0x01A6259C)((t), (r), (m9)))
#endif

/* audio-thread cycle counters for the resource monitor (screens/sysmon.c): custom synth engines, sampler voices,
 * drum voices, synth envelopes, custom FX; incremented around the hooked calls */
enum { SMC_SYNTH, SMC_SAMPLER, SMC_DRUM, SMC_ENV, SMC_FX, SMC_N };
/* the sample browser (screens/sbrowse.c, exported to plugins): owner = who opened it (engine number 1..63, or
 * SB_SAMPLERPLUS); sb_pick returns the selection's synth type job id (-preset index, <= 0) and closes, 1 = none */
#define SB_SAMPLERPLUS 0x100
void sb_open(int owner), sb_close(void), sb_turn(int delta), sb_page(void);
int sb_is_open(int owner), sb_pick(void);
extern volatile uint32_t sm_cy[SMC_N];
#ifndef OP1_HOST
static inline uint32_t op1_cycles(void) { uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c)); return c; }
#else
static inline uint32_t op1_cycles(void) { return 0; }
#endif

/* ---- the pitch a sequencer player's key_down key sounds ----
 * R246 calls a player's key_down (vtable +0x14) from the keyboard (key = the key; the note then gets the keyboard
 * octave) and from VoiceCtl's USB MIDI note-on (0x01A859E4, its key_down call at 0x01A85BF0; the range below also
 * covers the note-off 0x01A858D8): key = MIDI note - 0x35, and an unconsumed key plays the MIDI note as it is, so
 * no synth octave. Drum pads get the drum octave either way. Take OP1_SEQ_CALLER() in key_down itself (not in a function it calls) and pass the mode's octave. */
#define OP1_SEQ_MIDI_CALLER(ret) ((uint32_t)(ret) >= 0x01A858D8u && (uint32_t)(ret) < 0x01A85C16u)
#ifdef OP1_HOST
#define OP1_SEQ_CALLER() 0u                                    /* host tests: the keyboard */
#else
#define OP1_SEQ_CALLER() ((uint32_t)__builtin_return_address(0))
#endif
static inline int op1_seq_key_note(int mode, int key, int octave, uint32_t caller)
{
    return key + 0x35 + (mode || !OP1_SEQ_MIDI_CALLER(caller) ? octave * 12 : 0);
}

#endif
