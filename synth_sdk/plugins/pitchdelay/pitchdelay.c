/* PITCH DELAY: ESC_CODE's "Pitch Delay" (Delay Trio 1.0.0, github.com/VolkerJunginger/OP-1), a port of Tom Erbe /
 * SoundHack's +pitchdelay~, as an op1r FX: a delay up to 1 s whose repeats are pitch-shifted (two crossfaded heads).
 * The DSP is pitch_dsp.c (ESC_CODE's, GPL-3.0-or-later; SoundHack's BSD terms in SoundHack-LICENSE.txt); this file is
 * the op1r glue and screen.
 *   blue   TIME      1 ms .. 1 s
 *   green  FEEDBACK  0 .. 200 %
 *   white  PITCH     0 .. +12 semitones and just intervals (35 steps)   white press -> OCTAVE (word 7, -3 .. +3, steps)
 *   red    MIX       dry .. wet                                         Shift + red -> LOOP DEPTH (Shift word 3, 10 .. 100 %)
 * The Opus version edits octave and loop depth with white / red press modes; here octave is white's page 2 and loop
 * depth red's Shift value (op1r FX pages have no red page 2). Delay line: the
 * position's FX scratch (176,416 of 176,460 bytes), state per position (synth, drum, master: the mono sum).
 * Copyright (C) 2026 pcppcp (op1r glue and screen). GPL-3.0-or-later with the OP-1 firmware permission. */
#include "op1_plugin.h"
#include "pitch_dsp.h"

static struct pd_state st[3];
typedef char pd_fits[PD_SAMPLES * 2 <= OP1_FX_SCRATCH_BYTES ? 1 : -1];

static void pd_fx_init(int pos);
static int pitch_index(const int16_t *p) { int i = (op1_fx_param_q15(p, 2) * 35) >> 15; return i > 34 ? 34 : i; }
static int octave(const int16_t *p) { int w = p[7] > 0 ? p[7] : 0, o = (w * 7) >> 15; return (o > 6 ? 6 : o) - 3; }
static int depth(int pos) { int w = op1_fx_shift_words(pos)[3]; return 10 + ((w * 90 + 16383) >> 15); }
static void params(const int16_t *p, int16_t par[8])
{
    for (int i = 0; i < 4; i++) par[i] = (int16_t)op1_fx_param_q15(p, i);
    par[2] = (int16_t)(pitch_index(p) * PD_PITCH_STEP);
    for (int i = 4; i < 8; i++) par[i] = 0;
}
static void pd_fx_render(int pos, int16_t *buf, const int16_t *p)
{
    pos %= 3;
    short *ring = (short *)op1_fx_scratch(pos);
    if (!ring) return;
    int16_t par[8];
    params(p, par);
    pd_render(&st[pos], ring, buf, 0, par, octave(p), depth(pos));
}
const op1_fx_t FX_pitchdelay = { pd_fx_init, pd_fx_render, { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_FX_PAGE_STEPS(7), OP1_PRESS_NONE } };
static void reg(void) { op1_fx_shift_ext(&FX_pitchdelay, 3, 1, 5461); }   /* loop depth 25 % (the original's default) */
static void pd_fx_init(int pos) { reg(); pd_init(&st[pos % 3]); }      /* the ring is not cleared: "filled" gates every read */

/* ======================= screen (GUI thread) =======================
 * The echoes as bars from the left, spaced by TIME, each FEEDBACK times the one before; each repeat sits higher (or
 * lower) by the pitch interval plus octave, so the repeats climb or fall. Bottom row as ente. */
#include "../delayed/esc_ui.h"
#pragma GCC push_options
#pragma GCC optimize("Os")
static void pd_screen_init(void *scr) { (void)scr; reg(); }
static void pd_screen_update(void *scr, int dt)
{
    int pos = op1_fx_screen_pos() % 3;
    int16_t wsh[8]; const int16_t *wd = op1_knobs_shown(1, pos, op1_fx_knobs(pos), wsh);
    int16_t par[8]; params(wd, par);
    int oct = octave(wd), cents = pd_cents(pitch_index(wd)) + oct * 1200;   /* per repeat */
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    int32_t sp = Q(8) + par[0] * 52 / 32, fb = par[1] * 2, amp = Q(40);
    sq_line(r, Q(12), Q(58), Q(308), Q(58), 0x400, C_DIM);       /* the dry pitch */
    esc_vline(r, Q(16), Q(38), Q(78), Q(3), C_BLUE);             /* the input */
    for (int k = 0; k < 40; k++) {
        int32_t x = Q(16) + sp * (k + 1);
        if (x > Q(306) || amp < Q(2)) break;
        int32_t y = Q(58) - (int32_t)cents * (k + 1) * 1024 / 50;    /* 2 px per semitone */
        if (y < Q(14) + amp / 2 || y > Q(102) - amp / 2) break;  /* off the screen: the climb goes on unseen */
        esc_vline(r, x, y - amp / 2, y + amp / 2, Q(3), C_WHITE);
        amp = (int32_t)((int64_t)amp * fb >> 15);
        if (amp > Q(40)) amp = Q(40);
    }
    char v[16], *p;
    esc_ms(v, pd_time(par[0])); esc_row(r, 0, "time", v);
    esc_pct(v, par[1], 200); esc_row(r, 1, "feedback", v);
    if (op1_fx_page[pos][2]) { p = v; if (oct > 0) *p++ = '+'; if (oct < 0) *p++ = '-'; p = sq_num(p, (uint32_t)(oct < 0 ? -oct : oct), 1); sq_cat(p, " oct"); esc_row(r, 2, "octave", v); }
    else esc_row(r, 2, "pitch", pd_pitch_label(pitch_index(wd)));
    if (op1_shift_held()) { p = sq_num(v, (uint32_t)depth(pos), 1); sq_cat(p, "%"); esc_row(r, 3, "loop depth", v); }
    else { esc_pct(v, par[3], 100); esc_row(r, 3, "mix", v); }
}
#pragma GCC pop_options
const op1_screen_t SCREEN_pitchdelay = { pd_screen_init, pd_screen_update };
