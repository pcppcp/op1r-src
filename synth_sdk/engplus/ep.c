/* engine+ (docs/90): extra per-voice stages for the stock engines fm, digital, string, cluster, phase, dna, voltage and pulse in its unused knob words 5..8 (DRIVE, FILTER,
 * RESO, CRUSH; ep_dsp.c, after op1-sdk's engine_plus, MIT). On their screens, while Shift is held, the four
 * labels show and a turn edits that knob's extra word, saved in the sound like the stock knobs. The labels sit in the
 * columns fm's cubes leave free (x < 61 and >= 264): blue / green left, white / red right, a stock-font label and a
 * bar each. All four values 0 (every stock sound: see ep_def): the stock render untouched.
 *
 * ep_*_render   fm / digital voice render (vtable slots, make_firmware.py SLOTS): stock render, then the stages
 * ep_key        key hook: Shift down on the fm screen starts the overlay (the key itself stays stock)
 * ep_knob       knob hook: Shift + turn edits word 4 + k of the shown sound
 * the overlay   op1_overlay (screens/overlay.c) while Shift is held on the fm screen */
#ifndef OP1_HOST
#pragma GCC optimize ("Os", "no-tree-loop-distribute-patterns")
#endif
#include "../op1_synth.h"
#include "../seq/seq_text.h"
#include "ep_dsp.h"

#define EP_VOICES     8
#define EP_STEP       10485                             /* Q15 of a knob delta: ~100 clicks over the range */
#ifndef OP1_HOST
#define FM_RENDER     ((void (*)(void *, int16_t *, const int16_t *))0x019A23D4)
#define CUR_SCREEN    (*(volatile int *)0x018B65C8)
#define SYNTH_MODE    (*(volatile int *)0x01DE4C40 == 0)
#define SHIFT_HELD    (*(volatile uint8_t *)0x01DD7F78)
#define SYNTH_SLOT    (*(volatile int *)0xFF8016D4 & 7)
#define PATCH_SET_KNOBS(slot, w) (op1_patch_wait(), ((void (*)(void *, const int16_t *))0x01A00D14)((void *)(0x01DDD7E8 + (slot) * 0x38), (w)))
#else
extern void host_fm_render(void *, int16_t *, const int16_t *);
extern int host_cur_screen, host_synth_mode, host_shift, host_slot, host_saved;
#define FM_RENDER     host_fm_render
#define CUR_SCREEN    host_cur_screen
#define SYNTH_MODE    host_synth_mode
#define SHIFT_HELD    host_shift
#define SYNTH_SLOT    host_slot
#define PATCH_SET_KNOBS(slot, w) ((void)(slot), (void)(w), host_saved++)
#endif

/* the engines: their page's screen id and their stock words 4..7. fm's sounds carry 15000, 0, 100, 1500 there
 * (synth_types default and every factory preset; fm's render ignores them, emulator spectra equal), digital's 0.
 * The stage value is the word XOR that, so a stock sound reads all 0 (the stock render), values stay 0..32767. */
enum { EP_FM, EP_DIGITAL, EP_STRING, EP_CLUSTER, EP_PHASE, EP_DNA, EP_VOLTAGE, EP_PULSE, EP_ENGINES };
/* dim: the page's graphics reach the label columns (knob sweeps, docs/90): the frame is darkened under the labels */
static const struct { int screen; uint8_t dim; int16_t def[4]; } ep_eng[EP_ENGINES] = {
    { 0x00, 0, { 15000, 0, 100, 1500 } },               /* fm */
    { 0x01, 0, { 0, 0, 0, 0 } },                        /* digital */
    { 0x03, 1, { 0, 0, 0, 0 } },                        /* string */
    { 0x04, 1, { 0, 0, 0, 0 } },                        /* cluster */
    { 0x07, 1, { 0, 0, 0, 0 } },                        /* phase */
    { 0x32, 1, { 0, 0, 0, 0 } },                        /* dna */
    { 0x13, 1, { 0, 0, 0, 0 } },                        /* voltage */
    { 0x05, 1, { 0, 0, 0, 0 } },                        /* pulse */
};
#define EP_VAL(e, w, k) ((int16_t)((w) ^ ep_eng[e].def[k]))
static struct { void *self; ep_voice v; } ep_v[EP_VOICES];
static ep_voice *voice(void *self)                      /* audio thread only: the voice's stage state */
{
    int i, f = -1;
    for (i = 0; i < EP_VOICES; i++) {
        if (ep_v[i].self == self) return &ep_v[i].v;
        if (!ep_v[i].self && f < 0) f = i;
    }
    if (f < 0) return 0;
    ep_v[f].self = self; ep_reset(&ep_v[f].v);
    return &ep_v[f].v;
}

#ifdef EP_PROF
volatile uint32_t ep_prof[8][4];
#endif
static void ep_stages(int e, void *self, int16_t *out, const int16_t *knobs)   /* after the stock render */
{
    int16_t p[4] = { EP_VAL(e, knobs[4], 0), EP_VAL(e, knobs[5], 1), EP_VAL(e, knobs[6], 2), EP_VAL(e, knobs[7], 3) };
    if (!(p[0] | p[1] | p[2] | p[3])) return;           /* stock sound: untouched */
    ep_voice *v = voice(self);
#ifdef EP_PROF                                          /* profiling build: calls and cycles per engine, voices seen */
    { uint32_t t0 = op1_cycles();
      if (v) ep_process(v, out, OP1_BLOCK, p);
      uint32_t dt = op1_cycles() - t0;
      ep_prof[e][0]++; ep_prof[e][1] += dt; if (!ep_prof[e][2] || dt < ep_prof[e][2]) ep_prof[e][2] = dt;
      if (dt > ep_prof[e][3]) ep_prof[e][3] = dt; }   /* calls, cycles, min, max */
#else
    if (v) ep_process(v, out, OP1_BLOCK, p);
#endif
}
void ep_fm_render(void *self, int16_t *out, const int16_t *knobs)
{
    FM_RENDER(self, out, knobs); ep_stages(EP_FM, self, out, knobs);
}
#define EP_WRAP(name, e, addr) \
    void ep_##name##_render(void *self, int16_t *out, const int16_t *knobs) \
    { ((void (*)(void *, int16_t *, const int16_t *))(addr))(self, out, knobs); ep_stages(e, self, out, knobs); }
#ifndef OP1_HOST
EP_WRAP(digital, EP_DIGITAL, 0x01A51F7A)
EP_WRAP(string, EP_STRING, 0x01A4F098)
EP_WRAP(cluster, EP_CLUSTER, 0x01960288)
EP_WRAP(phase, EP_PHASE, 0x01A0BC5C)
EP_WRAP(dna, EP_DNA, 0x019B48D8)
EP_WRAP(voltage, EP_VOLTAGE, 0x01A49BF4)
EP_WRAP(pulse, EP_PULSE, 0xFFA04B88)                    /* in L1 (called, not read) */
#else
EP_WRAP(digital, EP_DIGITAL, host_fm_render)
EP_WRAP(string, EP_STRING, host_fm_render)
#endif

/* ---- keys, knobs, screen ---- */
static int ep_page(void)                                /* the engine whose page is shown, -1: none */
{
    if (!SYNTH_MODE) return -1;
    for (int e = 0; e < EP_ENGINES; e++) if (CUR_SCREEN == ep_eng[e].screen) return e;
    return -1;
}
static int ep_draw(void *r);
static const op1_tool_t ep_tool = { 0, 0, ep_draw };

int ep_key(int code, int pressed)                       /* Shift down on the fm page: the overlay (Shift stays stock) */
{
    if (code == 0x26 && pressed && ep_page() >= 0) op1_overlay(&ep_tool);
    return 0;
}

int ep_knob(int knob, int delta)                        /* Shift + turn on the fm page: the knob's extra word */
{
    int e = ep_page();
    if (knob < 0 || knob > 3 || !SHIFT_HELD || e < 0) return 0;
    int slot = SYNTH_SLOT;
    int16_t *w = (int16_t *)op1_synth_knobs(slot);
    int16_t o = EP_VAL(e, w[4 + knob], knob);
    int32_t v = (o < 0 ? 0 : o) + ((delta * EP_STEP) >> 15);
    int16_t n = EP_VAL(e, v < 0 ? 0 : v > 32767 ? 32767 : v, knob);
    if (n != w[4 + knob]) { w[4 + knob] = n; PATCH_SET_KNOBS(slot, w); }
    return 1;
}

#ifndef OP1_HOST
static uint32_t ep_tx[4][OP1_TEXT_SIZE / 4];
#else
static uint32_t ep_tx[4][64];
#endif
static uint8_t ep_made;
static const uint8_t ep_col[4][4] = { { 0x4D, 0x9E, 0xFF, 255 }, { 0x00, 0xED, 0x95, 255 }, { 0xFF, 0xFF, 0xFF, 255 },
                                      { 0xFF, 0x3A, 0x5D, 255 } };
static const uint8_t ep_grey[4] = { 0x6F, 0x71, 0x73, 255 };
static const char *const ep_lab[4] = { "drive", "filter", "reso", "crush" };
#define EP_X(k)   ((k) < 2 ? 8 : 268)                   /* free on fm (x < 61, >= 264) and digital (< 81, >= 204); dimmed elsewhere */
#define EP_Y(k)   ((k) & 1 ? 134 : 112)                 /* label baselines */
#define EP_W      44

static int ep_draw(void *r)                             /* GUI thread; 0 = overlay done */
{
    int e = ep_page();
    if (e < 0 || !SHIFT_HELD) return 0;              /* Shift released / page left: the overlay ends */
    if (ep_eng[e].dim) op1_raster_dim(r);                /* graphics under the labels: darkened, still seen */
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    if (!ep_made) {
        for (int k = 0; k < 4; k++) {
            op1_text_ctor(ep_tx[k], OP1_Q10(EP_X(k)), EP_Y(k) - 12, 12, ep_col[k]);  /* y: the text's top */
            op1_text_set(ep_tx[k], ep_lab[k]);
        }
        ep_made = 1;
    }
    const int16_t *w = op1_synth_knobs(SYNTH_SLOT);
    for (int k = 0; k < 4; k++) {
        int32_t x = OP1_Q10(EP_X(k)), y = OP1_Q10(EP_Y(k) + 5), val = EP_VAL(e, w[4 + k], k),
                len = OP1_Q10(EP_W) * (val < 0 ? 0 : val) / 32767;
        op1_text_draw(ep_tx[k], r, ident);
        if (len < OP1_Q10(EP_W)) sq_line(r, x + len, y, x + OP1_Q10(EP_W), y, 1024, ep_grey);
        if (len > 0) sq_line(r, x, y, x + len, y, 1024, ep_col[k]);
    }
    return 1;
}
