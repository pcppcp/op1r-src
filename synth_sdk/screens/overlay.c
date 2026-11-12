/* Tool plugins (op1_plugin.h op1_tool_t): key and service dispatch, and the overlay. While a tool's overlay is on,
 * the current page's screen object points at a copy of its vtable whose update runs the page's update, then the
 * tool's draw; the main loop follows screen changes and restores the page once draw returns 0. Two copies used in
 * turn, the original looked up from the object with interrupts off: a call through the last copy stays valid. */
#ifndef OP1_HOST
#pragma GCC optimize ("Os", "no-tree-loop-distribute-patterns")
#endif
#include "../op1_synth.h"
#include "../seq/seq_text.h"

#ifdef OP1_HOST
extern void **host_screen_table;
extern int host_cur_screen;
#define SCREEN_TABLE  host_screen_table
#define CUR_SCREEN    host_cur_screen
static uint32_t irq_off(void) { return 0; }
static void irq_on(uint32_t m) { (void)m; }
#else
#define SCREEN_TABLE  ((void **)0x01DF4E54)                     /* 0x44 screen objects; vtable at word 3 */
#define CUR_SCREEN    (*(volatile int *)0x018B65C8)
static uint32_t irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m)); return m; }
static void irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m)); }
#endif
#ifdef OP1_HOST
extern volatile int16_t host_inst_gain[2];
extern volatile int host_mode_cur, host_mode_req;
#define INST_GAIN     host_inst_gain[0]
#define INST_STEP     host_inst_gain[1]
#define MODE_AUDIO    host_mode_cur
#define MODE_REQ      host_mode_req
#else
#define INST_GAIN     (*(volatile int16_t *)0xFF80107C)         /* instrument bus gain (decimator, docs/29) */
#define INST_STEP     (*(volatile int16_t *)0xFF80107E)         /* its per-sample step */
#define MODE_AUDIO    (*(volatile int *)0xFF801038)             /* mode the audio side runs */
#define MODE_REQ      (*(volatile int *)0x01DE4C40)
#endif
#define OV_VT_WORDS   10                                        /* screen vtable: 5 {adj, fn} entries */

extern const op1_tool_t *op1_tools[OP1_TOOL_MAX];               /* plugload/plugload.c */
extern int op1_tool_count;

int tool_key(int code, int pressed)                             /* KeyHandler thread: 1 = swallowed */
{
    for (int i = 0; i < op1_tool_count; i++) if (op1_tools[i]->key && op1_tools[i]->key(code, pressed)) return 1;
    return 0;
}

static uintptr_t ov_vt[2][OV_VT_WORDS];                         /* used in turn */
static const uintptr_t *ov_orig[2];                             /* the wrapped objects' own vtables */
static uintptr_t *volatile ov_obj;                              /* the wrapped page object, 0: none */
static const op1_tool_t *volatile ov_tool;
static volatile uint8_t ov_done, ov_gen;
static uint8_t ov_i;
static void ov_update(void *self, int dt)                       /* GUI thread */
{
    uint32_t m = irq_off();
    const uintptr_t *vt = (const uintptr_t *)((uintptr_t *)self)[3];   /* its own once unwrapped */
    if (vt == ov_vt[0]) vt = ov_orig[0]; else if (vt == ov_vt[1]) vt = ov_orig[1];
    const op1_tool_t *t = self == ov_obj && !ov_done ? ov_tool : 0;
    uint8_t g = ov_gen;
    irq_on(m);
    ((void (*)(void *, int))vt[3 * 2 + 1])((uint8_t *)self + (int16_t)vt[3 * 2], dt);   /* entries {adj, fn} */
    if (t && !t->draw(op1_raster())) {
        m = irq_off();
        if (g == ov_gen) ov_done = 1;                           /* not after a newer op1_overlay */
        irq_on(m);
    }
}
static void ov_unwrap(void) { if (ov_obj) { ov_obj[3] = (uintptr_t)ov_orig[ov_i]; ov_obj = 0; } }   /* interrupts off */
static void ov_wrap(uintptr_t *obj)                             /* interrupts off */
{
    if (obj == ov_obj) return;
    ov_unwrap();
    ov_i ^= 1;
    ov_orig[ov_i] = (const uintptr_t *)obj[3];
    for (int i = 0; i < OV_VT_WORDS; i++) ov_vt[ov_i][i] = ov_orig[ov_i][i];
    ov_vt[ov_i][3 * 2] = 0; ov_vt[ov_i][3 * 2 + 1] = (uintptr_t)ov_update;
    obj[3] = (uintptr_t)ov_vt[ov_i]; ov_obj = obj;
}
static uintptr_t *page_obj(void)                                /* the current screen's object, 0: none */
{
    int id = CUR_SCREEN;
    return id >= 0 && id < 0x44 ? (uintptr_t *)SCREEN_TABLE[id] : 0;
}
void op1_overlay(const op1_tool_t *t)
{
    uintptr_t *obj = page_obj();
    if (!t || !t->draw || !obj) return;
    uint32_t m = irq_off();
    ov_wrap(obj);
    ov_tool = t; ov_done = 0; ov_gen++;
    irq_on(m);
}
/* safe mode (safeboot.c): a note over every page, the whole session (no tool plugins are loaded to want the overlay) */
static int safe_draw(void *r)
{
    static const uint8_t red[4] = { 0xFF, 0x3A, 0x5D, 255 };
    const char *t = op1r_safe_boot & SB_RESET ? "SAFE MODE  SETTINGS RESET" : "SAFE MODE";
    sq_text(r, OP1_Q10(316) - sq_width(OP1_Q10(1.2), t), OP1_Q10(3), OP1_Q10(1.2), t, 0x300, red);   /* top right */
    return 1;
}
static const op1_tool_t safe_tool = { 0, 0, safe_draw };
void tool_service(void)                                         /* main loop: services, then the overlay */
{
    if (op1r_safe_boot && !ov_tool) op1_overlay(&safe_tool);   /* (no page yet: tried again on the next pass) */
    { extern void sb_service(void); sb_service(); }              /* sample browser: page after an engine load */
    for (int i = 0; i < op1_tool_count; i++) if (op1_tools[i]->service) op1_tools[i]->service();
#ifndef OP1_HOST
    { extern void ext_engine_service(void); ext_engine_service(); }   /* extended synth engines (dispatcher.c, op1r 60) */
#endif
    if (!ov_obj) return;
    uintptr_t *obj = page_obj();
    uint32_t m = irq_off();
    if (ov_done || !obj) { ov_unwrap(); ov_tool = 0; }
    else if (obj != ov_obj) ov_wrap(obj);
    irq_on(m);
}

/* ---- instrument fade around a sound change (op1_inst_fade): the stock gain / step words of the instrument bus
 * (decimator 0xFFA005D4: gain += step per sample). The Core writes step = +0x40 every block before the voices
 * (0xFFA030B0; -0x40 only while a slot / mode switch fades), so the step is set from the voices' envelope call
 * (sp_adsr, before the decimator, each block with a voice) and the block's end checks the result. A step never
 * crosses 0 within a block (the decimator saturates only at +-0x7FFF: a negative gain plays inverted). */
#define IF_OUT_STEP   0x100                                     /* fade out: 128 samples (2 blocks) */
static volatile uint8_t if_state, if_pre;                       /* 0 idle, 1 fading out, 2 silent; pre ran */
static int16_t out_step(int g) { int s = g >> 6; return (int16_t)-(s < IF_OUT_STEP ? s : IF_OUT_STEP); }
void inst_fade_pre(void)                                        /* audio: per voice envelope, before the decimator */
{
    if (!if_state) return;
    if_pre = 1;
    int g = INST_GAIN;
    if (if_state == 2 || g < 64) { INST_GAIN = 0; INST_STEP = 0; if_state = 2; }
    else INST_STEP = out_step(g);
}
int inst_fade_active(void) { return if_state != 0; }
void inst_fade_block(void)                                      /* audio: block end (master stage) */
{
    if (!if_state) return;
    if (!if_pre) if_state = 2;                                  /* no voice playing: nothing to cut */
    if (if_state == 2) INST_GAIN = 0;
    if_pre = 0;
}
int op1_inst_fade(int out)              /* main loop; no waiting (call again on the next passes) */
{                                       /* out: 0 = silent, 1 = still fading; !out: back in (the Core's own step) */
    if (MODE_AUDIO != MODE_REQ) { if_state = 0; return 0; }   /* a mode switch fades on its own */
    if (!out) { if_state = 0; return 0; }
    if (if_state == 2) return 0;
    if (!if_state) {
        uint32_t m = irq_off(); if_pre = 0; if_state = 1; irq_on(m);
        extern volatile uint8_t sp_hook_live;
        sp_hook_live = 1;                                       /* the envelope hook sets the step (sp_core.c) */
    }
    return 1;
}
