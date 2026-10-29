/*
 * op1r plugin API: custom synth engines and effects for the OP-1 (firmware R246).
 * Copyright (C) 2026 pcppcp. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 *
 * Include this header (and nothing else of the firmware build) from plugin code. Rules for plugin code
 * (it runs in the audio interrupt; a crash or a long block locks the OP-1):
 *   - integers only (no float: soft-float is far too slow), no per-sample divisions, small per-block work;
 *   - no libc (no malloc, printf, memcpy): state lives in the voice object (synths, 32 bytes per voice) or in
 *     the FX position's scratch buffer (op1_fx_scratch, 176 KB per position);
 *   - keep state per FX position 0..2 (synth, drum, master).
 *
 * All custom engines run inside the hidden "iter" engine slot (instr type 14), so they use the
 * iter screen/graphics (iter.svg: bar0..3 follow the 4 knobs). Extra synth_types DB rows are made
 * possible by hooking instr_types' name compare (0x019B975C): every configured engine name maps to
 * type 14. The engine is selected per patch by a selector word in knobs[4] (= default_params[4]):
 *     knobs[4] == OP1_ENGINE_MAGIC + n   ->  engine n (1..OP1_MAX_ORDER) = plugin.json "order" (plugins/);
 *                                            an order without a plugin in this build -> engine 0
 *     anything else                      ->  engine 0 = original TE "iter"
 * See docs/14_custom_synth.md.
 *
 * Calling convention: GCC Blackfin == VisualDSP ABI (args R0,R1,R2; return R0).
 * Voices: 6, object `self` = 0x24 bytes in scratchpad 0xFFB00000 + i*0x24; word 0 = vtable (never
 * write it). Bytes 4..0x23 are yours (state is shared with the original iter when switching).
 */
#ifndef OP1_PLUGIN_H
#define OP1_PLUGIN_H
#define OP1_PLUGIN_API 1                 /* bumped when this interface changes incompatibly */
#include <stdint.h>
/* Host build (harness/): firmware addresses are replaced by mocks, see harness/op1_host.h. */
#ifdef OP1_HOST
#include "harness/op1_host.h"
#endif

#define OP1_BLOCK        128
#define OP1_VOICES       6
#define OP1_SAMPLE_RATE  44100
#define OP1_ENGINE_MAGIC 0x5300          /* knobs[4] = 0x5300 + engine index (21248 + n) */
#define OP1_MAX_ORDER    63              /* highest selector number per kind (synth, FX); gaps allowed */
#define OP1_SLOTS        (OP1_MAX_ORDER + 1)
#define OP1_FILE_FIRST   32              /* built-in plugins use 1..31, plugin files (docs/37) 32..63 */

#ifndef OP1_HOST
/* per-sample phase increment of the voice being rendered (2^32 = one cycle/sample) */
#define OP1_PITCH_INC    ((const int32_t *)0xFF801950)
/* firmware sine table: 8192 x int16, table[k] = sin(2*pi*k/8192) (verified on hardware via screen
 * rotations). Audio code may use index (int32 phase >> 19) + 4096 = -sin, inaudible; for geometry
 * (angles, windows that must be >= 0) index with (uint32 phase >> 19) or angle & 8191. */
#define OP1_SINE_TABLE   ((const int16_t *)0x0101E5E8)
#define OP1_VOICE_BASE   0xFFB00000u
/* note velocity per voice, int16 = velocity (1..127) << 8, written by VoiceCtl::note_on_synth */
#define OP1_VOICE_VEL    ((const volatile int16_t *)0xFF801064)
/* the stock envelope (T2 page) of the voice being rendered, for use inside render (e.g. as a timbre modulator):
 * OP1_BLOCK int16 values, one per output sample, 0..32767 (Q15), velocity and level included. The firmware computes
 * it just before render (Adsr_render) and still multiplies the output by it afterwards: reading it does not
 * replace the volume envelope. Valid only during render (one shared buffer, refilled per voice). */
#define OP1_VOICE_ENV    ((const int16_t *)0xFF801D50)
#endif

#define OP1_VOICE_SIZE   0x24u
static inline int op1_voice_index(const void *self)
{ return (int)(((uintptr_t)self - (uintptr_t)OP1_VOICE_BASE) / OP1_VOICE_SIZE); }
/* velocity of the voice's note as a Q15 gain (linear, 127 = 32766); 0 / out of range = full. Read it in render:
 * the firmware writes the velocity around the engine's note_on call. */
static inline int32_t op1_voice_vel_gain(const void *self)
{
    int32_t v = OP1_VOICE_VEL[op1_voice_index(self)] >> 8;
    return v > 0 && v <= 127 ? v * 258 : 32767;
}
/* voice object i (0..OP1_VOICES-1); readable from a screen update to show live voice state */
#define OP1_VOICE(i)     ((void *)((uintptr_t)OP1_VOICE_BASE + (uintptr_t)(i) * OP1_VOICE_SIZE))

/* One engine = these callbacks. params[0..3] = 4 knobs (0..32767), params[4] = selector,
 * params[5..7] = free preset words (default_params[5..7]). ADSR/gain applied after render. */
typedef struct {
    void (*note_on)(void *self, const int16_t *params);
    void (*render)(void *self, int16_t *out, const int16_t *params);   /* out[128] int16 */
    void (*note_off)(void *self);                                       /* may be 0 */
    uint8_t press[4];   /* encoder press per knob on the engine screen: OP1_PRESS_PAGE toggles page 2, where the
                         * knob edits preset word OP1_ENG_PAGE_WORD(k) (saved with the patch) instead of params[k];
                         * OP1_PRESS_BROWSE (engines with "uses_sample") opens the core's sample browser: the sampler
                         * presets as in sampler+, white loads one into the slot's sample buffer, the patch stays;
                         * OP1_PRESS_SHIFT: no page, Shift + turn edits OP1_ENG_PAGE_WORD(k) (k = 0..2), the press
                         * stays stock (op1r 48; older cores: Shift + turn edits params[k]) */
} op1_engine_t;
/* page-2 word per knob: blue 5, green 6, white 7, red 7 (white and red share word 7: page one of them only) */
#define OP1_ENG_PAGE_WORD(k) ((k) < 3 ? 5 + (k) : 7)
extern volatile uint8_t op1_eng_page[4];   /* synth engine screen: page 2 on per knob (blue..red) */
#ifndef OP1_HOST
static inline int op1_shift_held(void) { return *(volatile uint8_t *)0x01DD7F78 != 0; }   /* any thread, read only */
#endif

/* ---- access to the built-in sampler's per-slot sample (record it with the "sampler" engine) ----
 * 0x01DF6F78[slot] -> { SampleBuffer *sb; rate }   SampleBuffer: +0 int16 *data (8 guard samples,
 * then up to 264600 samples = 6 s mono 44.1 kHz), +4 int16 peak[2067] (max per 256 samples),
 * +0x1038 int capacity. Current synth slot: *(int*)0x01AAAC0C.  Engines with "uses_sample" in their
 * plugin.json also get the slot's synth_<slot>.raw loaded at boot (like the sampler). */
#define OP1_SAMPLE_GUARD   8
#define OP1_SAMPLE_MAX     264600
#define OP1_PEAK_BUCKETS   0x813
#ifndef OP1_HOST
static inline int op1_current_slot(void) { return *(volatile int *)0x01AAAC0C; }
static inline const uint8_t *op1_sample_buffer(int slot)
{
    uint32_t *obj = ((uint32_t **)0x01DF6F78)[slot & 7];
    return obj ? (const uint8_t *)obj[0] : 0;
}
static inline const int16_t *op1_sample_data(const uint8_t *sb)
{ return *(int16_t *const *)sb + OP1_SAMPLE_GUARD; }
static inline const int16_t *op1_sample_peaks(const uint8_t *sb) { return (const int16_t *)(sb + 4); }
#endif

/* ---- custom effects (run inside the hidden "filter" FX slot, fx type 5, graphics mmmf.svg) ----
 * Mono FX objects (synth or drum FX, one active at a time) live at 0xFFB00600. The firmware calls
 *   init(self, pos)                       pos 0 = synth FX, 1 = drum FX
 *   render(self, int16 buf[64], params)  in place, 64 samples per call; params[0..3] = 4 FX knobs
 *                                          (0..32767), params[4] = selector (0x5300+n), [5..7] free.
 * Each position owns a scratch buffer shared by all built-in FX (delay/spring/...): 176460 bytes
 * (44115 int32) at op1_fx_scratch(pos); only the active FX of that position uses it.
 * FX output meter: int16 at 0xFF801774 + pos*16 (peak with decay, shown on screen). */
#define OP1_FX_BLOCK        64
#define OP1_FX_SCRATCH_BYTES 176460
#ifndef OP1_HOST
static inline void *op1_fx_scratch(int pos) { return ((void **)0x01AB8BA0)[pos]; }
/* FX screens: which FX position is shown (0 synth, 1 drum) and its knob words of the current slot
 * (raw words [0..3] knobs - range depends on the FX type, use op1_fx_knobs_live for 0..32767; [4] selector) */
/* FX positions: 0 synth, 1 drum, 2 master (stereo, mixer). The FX screens' edit mode 0x01DF4E3C is 0/1/3 (3 =
 * master, as read_fx_param's pos). Master knob words: 0xFF8016C4 (one set), master FX type 0xFF801090. */
#define OP1_FX_MASTER 2
static inline int op1_fx_screen_pos(void) { int m = *(volatile int *)0x01DF4E3C; return m == 3 ? OP1_FX_MASTER : (m == 1); }
static inline const int16_t *op1_fx_knobs(int pos)
{
    if (pos == OP1_FX_MASTER) return (const int16_t *)0xFF8016C4;
    int slot = *(volatile int *)(pos ? 0xFF8016D8 : 0xFF8016D4) & 7;
    return (const int16_t *)((pos ? 0xFF8015C4 : 0xFF801444) + slot * 16);
}
/* raw FX knob word -> 0..32767 using the filter type's (fx type 5) range table {min, max, ?} per knob at
 * 0x01DE5C50 + type*0x30 (filled at boot; same scaling as read_fx_param). Custom FX run in the filter
 * slot, so their raw knob words cover only [min, max] of the filter's knobs. */
static inline int32_t op1_fx_param_q15(const int16_t *p, int i)
{
    const int16_t *rg = (const int16_t *)(0x01DE5C50 + 5 * 0x30 + i * 6);
    int32_t lo = rg[0], hi = rg[1], v = p[i];
    if (hi <= lo) return v < 0 ? 0 : v;                  /* table not filled: raw */
    v = (v - lo) * 32767 / (hi - lo);
    return v < 0 ? 0 : (v > 32767 ? 32767 : v);
}
static inline volatile int16_t *op1_fx_meter(int pos) { return (volatile int16_t *)(0xFF801774 + pos * 16); }
#endif

typedef struct {
    void (*init)(int pos);                                         /* FX (re)created at pos      */
    void (*render)(int pos, int16_t *buf, const int16_t *params);   /* 64 samples, in place       */
    uint8_t press[4];        /* encoder press per knob (blue..red): OP1_PRESS_NONE / _PAGE / _LATCH */
    /* optional, plugin.json "stereo": true: the master position (pos 2) calls this instead of render with the mix's
     * own left / right (64 samples each, in place); synth / drum FX still get render (their bus is mono) */
    void (*render_stereo)(int pos, int16_t *l, int16_t *r, const int16_t *params);
} op1_fx_t;
/* Encoder presses on a custom FX page (dispatcher.c hooks the KeyHandler dispatch):
 *  OP1_PRESS_PAGE : press toggles the knob between its normal parameter (params[k]) and a second
 *                   parameter in params[5 + k] (k = 0..2 only: words 5..7, 0..32767, saved with the patch)
 *  OP1_PRESS_LATCH: press toggles a flag (not saved), e.g. freeze
 * State per FX position (0 synth, 1 drum, 2 master): op1_fx_page[pos][k] = 1 while page 2 / latch is on.
 * FX state must be kept per position 0..2 (the master can run the same custom FX as an instrument). */
enum { OP1_PRESS_NONE = 0, OP1_PRESS_PAGE = 1, OP1_PRESS_LATCH = 2, OP1_PRESS_BROWSE = 3, OP1_PRESS_SHIFT = 4 };   /* BROWSE, SHIFT: synth engines */
extern volatile uint8_t op1_fx_page[3][4];

/* ---- beat clock (read-only; read it once per render block, never per sample) ----
 * The stock metronome's beat clock (metronome.cpp state at 0x01AADABC; tape/countin.c counts in on it). It runs
 * whether the metronome is heard or not, at the OP-1 tempo (BPM setting or MIDI clock), restarts on a beat when the
 * tape starts and, while it follows the tape, moves with the tape speed (and stands still while the tape is stopped).
 * It is the OP-1's only shared beat phase: the op1r sequencers (euclid, dots, shield, peal) run at the same tempo
 * (player ticks, 96 per beat) but count their steps from where each one was started.
 * Times are in 1/16384 output samples (44.1 kHz), as the firmware keeps them: beat length = 2646000 / BPM * 16384
 * (120 BPM: 22050 << 14). The metronome render (64 samples) runs once per audio callback; `next` is exact for the
 * block being rendered if the FX runs before it in the callback, else it is one block (1.45 ms) early. */
typedef struct {
    int32_t len;            /* beat length, 1/16384 samples (<= 0: no clock yet) */
    int32_t next;           /* time to the next beat from the first sample of this block, 1/16384 samples (0 .. len) */
    int32_t step;           /* clock advance per 64-sample block: 0x100000 = real time, else the tape speed; 0 = stands still */
    int32_t tape_pos;       /* tape position (samples); with follows_tape a beat sits at a multiple of len >> 14 */
    uint8_t follows_tape;   /* 1: the clock moves with the tape (stands still while the tape is stopped) */
} op1_beat_t;
#ifndef OP1_HOST
static inline void op1_beat_clock(op1_beat_t *b)
{
    const volatile uint8_t *m = (const volatile uint8_t *)0x01AADABC;
    b->len = *(const volatile int32_t *)(m + 0x1C);
    b->next = *(const volatile int32_t *)(m + 0x20);
    b->follows_tape = m[0x14];
    uint32_t sp = *(const volatile uint32_t *)0xFF802970;       /* tape speed: phase step (sp >> 21) << 14, as the render */
    b->step = b->follows_tape ? (int32_t)((sp >> 21) << 14) : 0x100000;
    b->tape_pos = *(const volatile int32_t *)0xFF802974 >> 6;  /* 1/64 samples */
}
#else
/* host: the harness sets the clock through host_beat() (one clock per translation unit: include the plugin source) */
static inline op1_beat_t *host_beat(void) { static op1_beat_t b; return &b; }
static inline void op1_beat_clock(op1_beat_t *b) { *b = *host_beat(); }
#endif

static inline int16_t op1_sat16(int32_t x)
{ return x > 32767 ? 32767 : (x < -32768 ? -32768 : (int16_t)x); }
static inline int32_t op1_mul16(int32_t a, int32_t b) { return (a * b) >> 15; }  /* Q15 */

/* ---- custom screens (GUI thread) — see docs/16 ----
 * An engine may own a screen: an SVG in content/display plus init/update callbacks. The SDK builds a
 * screen object (iter screen slot 0x14) and shows it while that engine is selected. Geometry is SVG
 * user units (320x160) in Q10 fixed point (1.0 = 1024). */
typedef struct { int32_t x, y; } op1_pt;                 /* Q10 */
typedef struct {
    void (*init)(void *scr);                              /* after SVG load: look up nodes */
    void (*update)(void *scr, int dt);                    /* every frame while visible */
} op1_screen_t;
#define OP1_Q10(v)          ((int32_t)((v) * 1024))

/* ---- sequencer plugins (plugin.json "kind": "seq", docs/44) ----
 * An extra sequencer type: a seq_types row with its name; selecting it runs it as the finger type (3) with
 * active[mode] set. The core shows its screen in the finger screen slot (0x2D), routes keys / encoders while that
 * screen is shown (left / right = keyboard octave are the core's) and calls midi_start on MIDI START. */
typedef struct {
    volatile uint8_t *active;                         /* [2]: selected for synth (0) / drum (1); written by the core */
    void *(*player_get)(void);                        /* PlayerManager::create_player case 3 while active */
    int  (*key)(int mode, int code, int pressed, int knob, int shift);   /* key on its page, 1 = handled (swallowed);
                                                         knob = encoder 0..3 for presses 0x3B..0x3E, else -1 */
    void (*turn)(int mode, int knob, int delta, int shift);              /* encoder turn on its page (knob 0..3) */
    void (*midi_start)(void);                         /* MIDI START (may be called while not active) */
    const char *alias;                                /* an older name that also selects it, or 0 */
} op1_seq_t;
#define OP1_SEQ_MAX 8                                  /* sequencer plugins loaded at once */

/* ---- drum engine plugin (plugin.json "kind": "drum", docs/43): drum+ ----
 * One at a time. The core keeps the DrumSampler vtable slots, the "op1r" key in drum patches (also without the plugin),
 * the drum_types row (id 2); the plugin builds its own page screens (ui_build / ui_load). The core calls these when a
 * plugin is loaded, else the stock code. pad = the stock pad words (int16[8]). */
typedef struct {
    void (*render)(void *self, int16_t *out, const void *pad);   /* drum voice (audio interrupt): may call the stock */
    void (*note_on)(void *self, const void *pad);
    void (*note_off)(void *self, const void *pad);
    void (*slot_loaded)(int slot, void *op1r);       /* drum patch loaded: its "op1r" object (Json::Value) or 0 */
    int  (*is_plus)(int slot);                        /* slot is drum+ (browser row, pages) */
    void (*type_job)(void *job);                      /* drum type browser job (stock job for other rows) */
    void (*ui_build)(void);                           /* boot, screen constructors: build its page objects */
    void (*ui_load)(void);                            /* boot, screens loaded: load their SVG, take the stock ones */
    void *(*screen_for)(int id);                      /* object for 0x10 / 0x17 (0: none) */
    void (*sync)(int editor_open_on_10);
    int  (*knob)(int knob, int delta);                /* 1 = consumed */
    int  (*key)(int code, int pressed);
} op1_drum_t;

/* ---- sampler engine plugin (plugin.json "kind": "sampler", docs/46): sampler+ ----
 * One at a time. The core keeps the SynthSampler vtable slots (render / note_on / note_off), the Adsr_render entry
 * hook, the "op1r" key of synth patches, the synth type job and the synth_types row 'sampler+' (id 100); the plugin
 * decides per slot (its own flag + slot type 6) and calls the stock code otherwise. knobs = the slot's 8 words. */
typedef struct {
    void (*render)(void *self, int16_t *out, const int16_t *knobs);   /* SynthSampler voice (audio interrupt) */
    void (*note_on)(void *self, const int16_t *knobs);
    void (*note_off)(void *self);
    int  (*adsr)(void *adsr, int16_t *env, const int16_t *words, const int16_t *vel);   /* 1 = voice done */
    void (*slot_loaded)(int slot, void *op1r);       /* synth patch loaded: its "op1r" object (Json::Value) or 0 */
    int  (*is_plus)(int slot);
    void (*type_job)(void *job);                      /* synth type browser job (stock job for other rows) */
    void (*ui_build)(void);                           /* boot: its page objects for screens 6 / 0x16 */
    void (*ui_load)(void);
    void *(*screen_for)(int id);                      /* object for 6 / 0x16 (stock when the slot is not sampler+) */
    void (*sync)(int editor_open_on_6);
    int  (*knob)(int knob, int delta);                /* 1 = consumed */
    int  (*key)(int code, int pressed);
} op1_sampler_t;
#define OP1_SAMPLER_ROW 100                           /* synth_types id of the 'sampler+' row */

/* ---- tool plugins (plugin.json "kind": "tool"): keys, the main loop, an overlay over any page ----
 * In load order: key after op1r's own (update prompt, MIDI sequencer, drum+, sample editor) and before the stock
 * handlers, service every main-loop pass, and after op1_overlay(t) t->draw over the current page each frame until
 * it returns 0. */
typedef struct {
    int  (*key)(int code, int pressed);               /* KeyHandler thread; 1 = swallowed (its releases too) */
    void (*service)(void);                            /* main loop */
    int  (*draw)(void *raster);                       /* GUI thread; 0 = done */
} op1_tool_t;
#define OP1_TOOL_MAX 4
void op1_overlay(const op1_tool_t *t);                /* any thread */
/* instrument bus fade around a sound change (create_synth / FX swap cut the running voices), main loop only:
 * op1_inst_fade(1) starts the fade out and returns 1 until the bus is silent (a few ms: call it again on the next
 * passes), then 0; op1_inst_fade(0) fades back in. */
int op1_inst_fade(int out);
/* debug print: in op1emu (OP1EMU_DBG, set by tools/emu_ctl.py) the text and both values go to the emulator log as
 * "dbg: text a b"; on the OP-1 an empty call. Any thread; not per sample. Host builds: stderr. */
#ifndef OP1_HOST
void op1_dbg(const char *text, int a, int b);
#else
#include <stdio.h>
static inline void op1_dbg(const char *text, int a, int b) { fprintf(stderr, "dbg: %s %d %d\n", text, a, b); }
#endif
#ifndef OP1_HOST
#define op1_find_node(scr, id) (((void *(*)(void *, const char *))0x01A3B580)((scr), (id)))
#define op1_node_set_transform(node, m9) (((void (*)(void *, const int32_t *))0x019EAD58)((node), (m9)))
/* node +0x50: visible flag; a group (<g>) with 0 skips its children (group draw 0x019EB818) */
#define op1_node_set_visible(node, on) (((volatile uint8_t *)(node))[0x50] = (uint8_t)((on) != 0))
/* the colour of an SVG node's own line / fill style: RGBA bytes, writable, shown from the next frame (Screen::
 * set_linestyle 0x01A3B854 / set_fillstyle 0x01A3B96C, as stock screens recolour parts). Call once at init (the node
 * id must exist and have that style, else the firmware asserts) and keep the pointer. One style per element (equal
 * colours are not shared, docs/16): a multi-element shape needs an id and a pointer per element. */
#define op1_node_line_rgba(scr, id) (((uint8_t *(*)(void *, const char *))0x01A3B854)((scr), (id)))
#define op1_node_fill_rgba(scr, id) (((uint8_t *(*)(void *, const char *))0x01A3B96C)((scr), (id)))
#define op1_screen_draw(scr, dt) (((void (*)(void *, int))0x01A3B4C8)((scr), (dt)))   /* render SVG */
#define op1_raster()        (((void *(*)(void))0x01A3B490)())
#define op1_raster_line(r, p0, p1, w_q10, rgba) \
    (((void (*)(void *, const op1_pt *, const op1_pt *, int32_t, const uint8_t *))0x01A24A4C)((r), (p0), (p1), (w_q10), (rgba)))
#define op1_raster_dim(r)   (((void (*)(void *))0x01A2357A)(r))   /* Raster::dim_all: the frame darkened (HELP) */
#define OP1_SCREEN_SLOT     (*(volatile int *)0x01DF4E38)  /* instrument slot the screen shows */
static inline const int16_t *op1_synth_knobs(int slot) { return (const int16_t *)(0xFF8013C4 + (slot & 7) * 16); }
#endif /* !OP1_HOST */
/* Knob values as the stock screens see them (incl. LFO modulation): ReadInterface 0x01A28210(slot)
 * returns a vector of 8 floats normalised 0..1 (struct {?, float *begin, float *end}). -> Q15 0..32767 */
static inline int32_t op1_f01_to_q15(uint32_t f)          /* IEEE float in [0,1] -> 0..32767, no libgcc */
{
    if ((int32_t)f <= 0) return 0;                        /* <= +0, or negative */
    int e = (int)((f >> 23) & 0xff) - 127;
    if (e >= 0) return 32767;
    uint32_t m = (f & 0x7fffff) | 0x800000;               /* 1.m * 2^e */
    int sh = 23 - 15 - e;                                 /* value*32768 = m >> sh */
    if (sh >= 32) return 0;
    uint32_t v = m >> sh;
    return v > 32767 ? 32767 : (int32_t)v;
}
#ifndef OP1_HOST
static inline void op1_synth_knobs_live(int slot, int16_t out[4])
{
    const uint32_t *const *vec = (const uint32_t *const *)((void *(*)(int))0x01A28210)(slot);
    const uint32_t *b = vec[1], *e = vec[2];
    for (int i = 0; i < 4; i++)
        out[i] = (b && b + i < e) ? (int16_t)op1_f01_to_q15(b[i]) : op1_synth_knobs(slot)[i];
}
/* FX knobs of the shown FX screen as the stock screens see them: read_fx_param 0x01A28DA0(screen slot,
 * screen pos 0x01DF4E3C) scales the raw words with a per-FX-type range table (filled at runtime; the raw
 * words of the filter-type slot do not span 0..32767). -> Q15 0..32767 */
static inline void op1_fx_knobs_live(int16_t out[4])
{
    int pos = *(volatile int *)0x01DF4E3C;
    extern void *read_fx_param_safe(int slot, int pos);       /* dispatcher.c: sanitize + 0x01A28DA0 */
    const uint32_t *const *vec = (const uint32_t *const *)read_fx_param_safe(OP1_SCREEN_SLOT, pos);
    const uint32_t *b = vec[1], *e = vec[2];
    for (int i = 0; i < 4; i++)
        out[i] = (b && b + i < e) ? (int16_t)op1_f01_to_q15(b[i]) : op1_fx_knobs(pos == 3 ? OP1_FX_MASTER : (pos & 1))[i];
}
#endif
/* rotate node around (cx,cy) by sin/cos (Q15): matrix for row vectors [a b 0; c d 0; tx ty 1] (Q10) */
static inline void op1_node_rotate(void *node, int32_t cx, int32_t cy, int32_t s15, int32_t c15)
{
    int32_t c = c15 >> 5, s = s15 >> 5;                 /* Q15 -> Q10 */
    int32_t m[9] = { c, s, 0, -s, c, 0,
                     cx - ((cx * c - cy * s) >> 10), cy - ((cx * s + cy * c) >> 10), 1024 };
    op1_node_set_transform(node, m);
}
#endif
