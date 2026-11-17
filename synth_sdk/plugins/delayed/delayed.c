/* DELAYED: ESC_CODE's "Delayed" (Delay Trio 1.0.0, github.com/VolkerJunginger/OP-1), a port of Tom Erbe / SoundHack's
 * +delay~, as an op1r FX: a delay up to 1 s with a resonant feedback filter, an LFO on the delay time and freeze.
 * The DSP is delay_dsp.c (ESC_CODE's, GPL-3.0-or-later; SoundHack's BSD terms in SoundHack-LICENSE.txt); this file is
 * the op1r glue and screen. The Opus layout:
 *   blue   TIME      1 ms .. 1 s                  Shift + blue   LFO RATE   (Shift word 0; screen "lfo rate")
 *   green  FEEDBACK  0 .. 200 %                   Shift + green  LFO DEPTH  (Shift word 1)
 *   white  FILTER    the feedback low-pass        Shift + white  LFO SHAPE  sine tri square up down rand (word 2)
 *          white press -> RESO (word 7, 0 .. 63)
 *   red    MIX       dry .. wet                   Shift + red    LFO PHASE  -180 .. +180 deg (word 3)
 *          red press -> FREEZE (latch, not saved, as in the original)
 * Second Shift layer (Shift + an encoder press): blue SMOOTH (the Opus setting "smooth LFO", here per sound: softens
 * the LFO's jumps for square, up, down and rand). Delay line: the position's FX scratch (176,416 of 176,460 bytes), state per
 * position (synth, drum, master: on the master the core feeds the mono sum).
 * Copyright (C) 2026 pcppcp (op1r glue and screen). GPL-3.0-or-later with the OP-1 firmware permission. */
#include "op1_plugin.h"
#include "delay_dsp.h"

static struct dl_state st[3];
typedef char dl_fits[DL_SAMPLES * 2 <= OP1_FX_SCRATCH_BYTES ? 1 : -1];

static void dl_fx_init(int pos);
static void params(int pos, const int16_t *p, int16_t par[8])
{
    const int16_t *x = op1_fx_shift_words(pos);
    for (int i = 0; i < 4; i++) par[i] = (int16_t)op1_fx_param_q15(p, i);
    par[4] = x[0];                                           /* LFO rate */
    par[5] = x[1];                                           /* LFO depth */
    par[6] = (int16_t)((x[2] * 6) >> 15);                    /* shape 0..5 */
    par[7] = x[3];                                           /* phase: 16384 = 0 */
}
static int reso(const int16_t *p) { return ((p[7] > 0 ? p[7] : 0) * 63 + 16383) >> 15; }
static void dl_fx_render(int pos, int16_t *buf, const int16_t *p)
{
    pos %= 3;
    short *ring = (short *)op1_fx_scratch(pos);
    if (!ring) return;
    int16_t par[8];
    params(pos, p, par);
    dl_set_smooth(op1_fx_shift_words(pos)[4] >= 16384);        /* (a global in delay_dsp.c: set before each position) */
    dl_render(&st[pos], ring, buf, 0, 64, par, reso(p), op1_fx_page[pos][3] != 0);
}
const op1_fx_t FX_delayed = { dl_fx_init, dl_fx_render, { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_PAGE, OP1_PRESS_LATCH } };
static void reg(void)                                        /* the Shift layer (from init and the screen's init) */
{
    op1_fx_shift_ext(&FX_delayed, 0, 1, 14208);              /* LFO rate (the original's default) */
    op1_fx_shift_ext(&FX_delayed, 1, 1, 0);                  /* depth */
    op1_fx_shift_ext(&FX_delayed, 2, 6, 2730);               /* shape: sine */
    op1_fx_shift_ext(&FX_delayed, 3, 1, 16384);              /* phase 0 */
    op1_fx_shift_ext(&FX_delayed, 4, 2, 8192);               /* layer 2: smooth LFO off */
}
static void dl_fx_init(int pos) { reg(); dl_init(&st[pos % 3]); }   /* the ring is not cleared: "filled" gates every read */

/* ======================= screen (GUI thread) =======================
 * The echoes as bars from the left: spaced by TIME, each one FEEDBACK times the one before (capped at full height),
 * swaying sideways with the LFO; the filter darkens the later ones (grey). FREEZE: the bars red and still. Bottom row
 * as ente. */
#include "esc_ui.h"
#pragma GCC push_options
#pragma GCC optimize("Os")
static uint32_t lfo_ph;                                          /* the screen's own LFO phase (Q32) */
static void dl_screen_init(void *scr) { (void)scr; reg(); }
static char *hz(char *v, int h) { char *p; if (h >= 1000) { p = sq_num(v, (uint32_t)(h / 1000), 1); *p++ = '.'; p = sq_num(p, (uint32_t)(h / 100 % 10), 1); return sq_cat(p, " khz"); } p = sq_num(v, (uint32_t)h, 1); return sq_cat(p, " hz"); }
static void dl_screen_update(void *scr, int dt)
{
    int pos = op1_fx_screen_pos() % 3;
    int16_t wsh[8]; const int16_t *wd = op1_knobs_shown(1, pos, op1_fx_knobs(pos), wsh);
    int16_t par[8]; params(pos, wd, par);
    const int frozen = op1_fx_page[pos][3] != 0, shift = op1_shift_held();
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    int t = dl_time(par[0]);                                     /* samples */
    int32_t sp = Q(8) + par[0] * 52 / 32;                        /* 8 .. 60 px per echo (TIME is a log knob) */
    int32_t fb = par[1] * 2, amp = Q(70);                        /* 0 .. 200 % */
    if (!frozen) lfo_ph += (uint32_t)dl_rate_milli(par[4]) * (uint32_t)(dt > 0 ? dt : 16) * 4295u;   /* mHz x ms */
    int32_t sway = (int32_t)dl_wave(lfo_ph + ((uint32_t)(uint16_t)par[7] << 17) - 0x80000000u, par[6], 0, 0) * par[5] >> 15;
    int tone = par[2] >> 8;                                      /* 127: open */
    sq_line(r, Q(12), Q(98), Q(308), Q(98), 0x400, C_DIM);
    for (int k = 0; k < 40; k++) {
        int32_t x = Q(16) + sp * (k + 1) + (frozen ? 0 : (int32_t)((int64_t)sp * (k + 1) * sway >> 17));
        if (x > Q(306) || amp < Q(2)) break;
        const uint8_t *c = frozen ? C_RED : k * (127 - tone) > 200 ? C_GREY : C_WHITE;
        esc_vline(r, x, Q(98) - amp, Q(98), Q(3), c);
        amp = (int32_t)((int64_t)amp * fb >> 15);
        if (amp > Q(70)) amp = Q(70);
    }
    esc_vline(r, Q(16), Q(28), Q(98), Q(3), C_BLUE);             /* the input */
    char v[16], *p;
    if (shift && op1_fx_shift_layer(pos)) {                      /* layer 2 */
        esc_row(r, 0, "smooth", op1_fx_shift_words(pos)[4] >= 16384 ? "on" : "off");
        esc_row(r, 1, "", ""); esc_row(r, 2, "", ""); esc_row(r, 3, "", "");
        return;
    }
    if (shift) {                                                 /* the Shift layer: the LFO */
        static const char *const shapes[6] = { "sine", "tri", "square", "up", "down", "rand" };
        int m = dl_rate_milli(par[4]); p = sq_num(v, (uint32_t)(m / 1000), 1); *p++ = '.'; p = sq_num(p, (uint32_t)(m / 10 % 100), 2); sq_cat(p, " hz");
        esc_row(r, 0, "lfo rate", v);
        esc_pct(v, par[5], 100); esc_row(r, 1, "depth", v);
        esc_row(r, 2, "shape", shapes[par[6] < 0 ? 0 : par[6] > 5 ? 5 : par[6]]);
        int deg = par[7] * 360 / 32767 - 180; p = v; if (deg < 0) { *p++ = '-'; deg = -deg; } p = sq_num(p, (uint32_t)deg, 1); sq_cat(p, " deg");
        esc_row(r, 3, "phase", v);
        return;
    }
    esc_ms(v, t); esc_row(r, 0, "time", v);
    esc_pct(v, par[1], 200); esc_row(r, 1, "feedback", v);
    if (op1_fx_page[pos][2]) { p = sq_num(v, (uint32_t)(reso(wd) * 100 / 63), 1); sq_cat(p, "%"); esc_row(r, 2, "reso", v); }
    else { hz(v, dl_freq(par[2])); esc_row(r, 2, "filter", v); }
    if (frozen) esc_row(r, 3, "mix", "freeze"); else { esc_pct(v, par[3], 100); esc_row(r, 3, "mix", v); }
}
#pragma GCC pop_options
const op1_screen_t SCREEN_delayed = { dl_screen_init, dl_screen_update };
