/* BUBBLER: ESC_CODE's "Bubbler" (Delay Trio 1.5.0, github.com/VolkerJunginger/OP-1), a port of Tom Erbe / SoundHack's
 * +bubbler~, as an op1r FX: a granular delay, up to eight overlapping grains read from the delay line with their own
 * timing, length, pitch and direction, through a filtered, saturated feedback loop.
 * The DSP is bubbler_dsp.c / bubbler_params.c (ESC_CODE's, GPL-3.0-or-later; SoundHack's BSD terms in
 * SoundHack-LICENSE.txt); this file is the op1r glue and screen.
 *   blue   TIME      grain delay 30 ms .. 1 s   blue press  -> SIZE    (word 5, grain length)    Shift: TIME VAR ("spread")
 *   green  FEEDBACK  0 .. 95 %                  green press -> PITCH   (word 6, 10 steps:          Shift: START VAR ("jitter")
 *                                                              -12 -7 -5 -3 off key +3 +5 +7 +12; key: the last note
 *                                                              the synth played, C4 = 0, op1_last_note)
 *   white  DENSITY   grains per time            white press -> FILTER  (word 7, feedback low-pass) Shift: SIZE VAR
 *   red    MIX       dry .. wet                                                                     Shift: REVERSE (chance)
 * Second Shift layer (Shift + an encoder press): blue PITCH VAR (the chance of an octave up / down per grain), green
 * RESO (the feedback filter's resonance). The Opus version's Shift pages 2 and 3, on op1r's presses (size, pitch,
 * filter) and the two Shift layers.
 * Delay line 65,536 samples (1.49 s; the original 131,072) in the position's FX scratch; state per position (synth,
 * drum, master: the mono sum).
 * Copyright (C) 2026 pcppcp (op1r glue and screen). GPL-3.0-or-later with the OP-1 firmware permission. */
#include "op1_plugin.h"
#include "bubbler_dsp.h"

static struct bb_state st[3];
static int mixbuf[3][64];
typedef char bb_fits[BB_STORAGE * 2 <= OP1_FX_SCRATCH_BYTES ? 1 : -1];
#define NPITCH 10
static const int8_t pitch_st[NPITCH] = { -12, -7, -5, -3, 0, 99, 3, 5, 7, 12 };   /* 99: key */
static const uint8_t pitch_mode[NPITCH] = { 2, 3, 6, 7, 0, 1, 8, 9, 4, 5 };   /* bubbler_dsp.c's offsets[] index, 1 = key */

static int pitch_step(const int16_t *p) { int w = p[6] > 0 ? p[6] : 0, i = (w * NPITCH) >> 15; return i > NPITCH - 1 ? NPITCH - 1 : i; }
/* our words -> the Opus words 0..7 (bubbler_params.c packs the extra controls into 4..7) */
static struct { int16_t in[9]; int16_t par[8]; } cache[3];
static const int16_t *params(int pos, const int16_t *p)
{
    int16_t k[4]; for (int i = 0; i < 4; i++) k[i] = (int16_t)op1_fx_param_q15(p, i);
    const int16_t *x = op1_fx_shift_words(pos);
    const int16_t in[9] = { k[2], (int16_t)(p[5] > 0 ? p[5] : 0), (int16_t)(p[7] > 0 ? p[7] : 0), x[0], x[1], x[2], x[3], x[4], x[5] };
    int16_t *q = cache[pos].par, same = q[7] != 0;
    for (int i = 0; i < 9; i++) same &= cache[pos].in[i] == in[i];
    if (!same) {                                              /* (divisions: only when a value changed) */
        struct bb_params v = { .scatter = in[3], .tone = (in[2] * 127 + 16383) >> 15, .pitch = 0, .reverse = in[6],
                               .density = (in[0] * 100 + 16383) / 32767, .size_var = in[5],
                               .pitch_var = (in[7] * 100 + 16383) / 32767, .jitter = in[4], .size = in[1],
                               .resonance = (in[8] * 63 + 16383) >> 15 };
        for (int i = 4; i < 8; i++) q[i] = 0;
        bb_encode(q, &v);                                     /* (q[7] gets 0x8000: never 0 afterwards) */
        for (int i = 0; i < 9; i++) cache[pos].in[i] = in[i];
    }
    q[0] = k[0]; q[1] = k[1]; q[3] = k[3];
    return q;
}
static void bb_fx_init(int pos);
static void bb_fx_render(int pos, int16_t *buf, const int16_t *p)
{
    pos %= 3;
    short *ring = (short *)op1_fx_scratch(pos);
    if (!ring) return;
    const int16_t *q = params(pos, p);
    st[pos].pitch_mode = pitch_mode[pitch_step(p)];
    if (st[pos].pitch_mode == 1) bb_note(op1_last_note());    /* key: the grains at the last played note (C4 = 0) */
    bb_render_work(&st[pos], ring, buf, 0, 64, q, mixbuf[pos]);
}
const op1_fx_t FX_bubbler = { bb_fx_init, bb_fx_render, { OP1_PRESS_PAGE, OP1_FX_PAGE_STEPS(NPITCH), OP1_PRESS_PAGE, OP1_PRESS_NONE } };
static void reg(void)                                         /* the Shift layer: Bubbler's defaults */
{
    op1_fx_shift_ext(&FX_bubbler, 0, 1, 4112);                /* time variation 12.5 % */
    op1_fx_shift_ext(&FX_bubbler, 1, 1, 4112);                /* start variation */
    op1_fx_shift_ext(&FX_bubbler, 2, 1, 4112);                /* size variation */
    op1_fx_shift_ext(&FX_bubbler, 3, 1, 0);                   /* reverse chance */
    op1_fx_shift_ext(&FX_bubbler, 4, 1, 0);                   /* layer 2: pitch variation (octave chance) */
    op1_fx_shift_ext(&FX_bubbler, 5, 1, 0);                   /* layer 2: resonance */
}
static void bb_fx_init(int pos) { reg(); pos %= 3; bb_init(&st[pos]); cache[pos].par[7] = 0; }   /* the ring: "filled" gates reads */

/* ======================= screen (GUI thread) =======================
 * Each live grain is a bubble: in its own column, rising from the bottom as it plays (falling, red, when reversed),
 * its size from the grain's length; pitched grains green (up) or blue (down). Bottom row as ente. */
#include "../delayed/esc_ui.h"
#pragma GCC push_options
#pragma GCC optimize("Os")
static void bubble(void *r, int32_t x, int32_t y, int32_t rad, const uint8_t *c)   /* an octagon */
{
    static const int8_t o[9][2] = { {10,4},{4,10},{-4,10},{-10,4},{-10,-4},{-4,-10},{4,-10},{10,-4},{10,4} };
    for (int i = 1; i < 9; i++)
        sq_line(r, x + rad * o[i - 1][0] / 10, y + rad * o[i - 1][1] / 10, x + rad * o[i][0] / 10, y + rad * o[i][1] / 10, Q(2), c);
}
static void bb_screen_init(void *scr) { (void)scr; reg(); }
static void bb_screen_update(void *scr, int dt)
{
    int pos = op1_fx_screen_pos() % 3;
    int16_t wsh[8]; const int16_t *wd = op1_knobs_shown(1, pos, op1_fx_knobs(pos), wsh);
    const int ps = pitch_step(wd);
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    sq_line(r, Q(12), Q(102), Q(308), Q(102), 0x400, C_DIM);
    const struct bb_state *s = &st[pos];
    for (int i = 0; i < BB_GRAINS; i++) {
        const struct bb_grain *g = &s->grain[i];
        if (!g->active || !g->length) continue;
        uint32_t len = g->length, age = g->age < len ? g->age : len;
        int32_t rad = Q(4) + (int32_t)(len > 22050 ? 22050 : len) * 10 / 22050 * 1024;   /* 4 .. 14 px */
        int32_t travel = Q(84) - 2 * rad, prog = (int32_t)((uint64_t)age * 1024 / len);   /* 0 .. 1024 */
        int32_t y = g->step < 0 ? Q(14) + rad + (travel * prog >> 10) : Q(98) - rad - (travel * prog >> 10);
        const uint8_t *c = g->step < 0 ? C_RED : g->step > 4096 + 64 ? C_GREEN : g->step < 4096 - 64 ? C_BLUE : C_WHITE;
        bubble(r, Q(40 + 32 * i), y, rad, c);
    }
    char v[16], *p;
    if (op1_shift_held()) {                                   /* the Shift layer */
        const int16_t *x = op1_fx_shift_words(pos);
        if (op1_fx_shift_layer(pos)) {                        /* layer 2 */
            esc_pct(v, x[4], 100); esc_row(r, 0, "pitch var", v);
            esc_pct(v, x[5], 100); esc_row(r, 1, "reso", v);
            esc_row(r, 2, "", ""); esc_row(r, 3, "", "");
            return;
        }
        esc_pct(v, x[0], 100); esc_row(r, 0, "spread", v);
        esc_pct(v, x[1], 100); esc_row(r, 1, "jitter", v);
        esc_pct(v, x[2], 100); esc_row(r, 2, "size var", v);
        esc_pct(v, x[3], 100); esc_row(r, 3, "reverse", v);
        return;
    }
    if (op1_fx_page[pos][0]) { esc_pct(v, esc_word(wd, 5), 100); esc_row(r, 0, "size", v); }
    else { esc_ms(v, bb_time_samples(op1_fx_param_q15(wd, 0))); esc_row(r, 0, "time", v); }
    if (op1_fx_page[pos][1]) {
        int stp = pitch_st[ps];
        if (!stp) sq_cat(v, "off"); else if (stp == 99) sq_cat(v, "key"); else { p = v; *p++ = stp > 0 ? '+' : '-'; p = sq_num(p, (uint32_t)(stp < 0 ? -stp : stp), 1); sq_cat(p, " st"); }
        esc_row(r, 1, "pitch", v);
    } else { esc_pct(v, op1_fx_param_q15(wd, 1), 95); esc_row(r, 1, "feedback", v); }
    if (op1_fx_page[pos][2]) { int hz = bb_cutoff(esc_word(wd, 7)); if (hz >= 1000) { p = sq_num(v, (uint32_t)(hz / 1000), 1); *p++ = '.'; p = sq_num(p, (uint32_t)(hz / 100 % 10), 1); sq_cat(p, " khz"); } else { p = sq_num(v, (uint32_t)hz, 1); sq_cat(p, " hz"); } esc_row(r, 2, "filter", v); }
    else { esc_pct(v, op1_fx_param_q15(wd, 2), 100); esc_row(r, 2, "density", v); }
    esc_pct(v, op1_fx_param_q15(wd, 3), 100); esc_row(r, 3, "mix", v);
}
#pragma GCC pop_options
const op1_screen_t SCREEN_bubbler = { bb_screen_init, bb_screen_update };
