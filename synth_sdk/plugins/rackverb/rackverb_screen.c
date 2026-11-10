/* rackverb screen: the owner's approved rack faceplate (OP1-Emulator docs/design/rackverb-v2/a-v2.svg, concept A
 * revision 2 at 320 x 160, 2026-11-06), its coordinates as they are. Static (rackverb.svg, gen_svg.py): frame, rack
 * ears, screws, the LED window, the knob rings and ticks, the strip's line, the unit's name with its stripes (one of
 * four logo groups shown). Live: the program number as two slanted 7-segment digits (unlit segments dim), the
 * program's name, each knob's label, pointer and value (page 1 FEEDBACK / CUTOFF / MIX; a pushed knob shows its page 2
 * SIZE / MODE / UNIT with the hidden page-1 value small in grey under it), and the strip: "PROG n/count", a stereo
 * L / R meter of the output's peaks (two rows of 20 steps of 2 dB, the top 3 red, unlit steps dim; rv_live[4..5])
 * and the AUTH / CLEAN tag. Text is kennel's stroke font (seq_text.h, from the core). Raster lines draw 1.25 x
 * thinner natively than SVG strokes (docs/screen-art.md), so the live widths here are the concept's x 1.25.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_synth.h"
#include "../../seq/seq_text.h"
#include "rv_quad.h"

extern volatile int16_t rv_live[3][6];
const char *rv_live_name(int p);
static void *logo[4];

static const uint8_t BLUE[4] = { 105, 142, 255, 255 }, GREEN[4] = { 0, 237, 149, 255 }, WHITE[4] = { 255, 255, 255, 255 },
                     RED[4] = { 255, 58, 93, 255 }, GREY[4] = { 101, 101, 121, 255 },
                     RED_OFF[4] = { 42, 16, 24, 255 }, GREEN_OFF[4] = { 16, 38, 28, 255 };
static const uint16_t HZ[33] = { 500, 551, 607, 668, 736, 810, 893, 983, 1083, 1193, 1314, 1447, 1594, 1755, 1933, 2129,
                                 2345, 2583, 2845, 3134, 3451, 3801, 4187, 4611, 5079, 5594, 6162, 6786, 7475, 8233, 9068,
                                 9987, 11000 };
static const char *const UNIT[4] = { "VERB I", "VERB II", "FEX", "QUAD" };
static const uint8_t COUNT[4] = { 63, 100, 63, QD_PRESETS };   /* rackverb.c U_COUNT */
static const uint8_t *const COL[3] = { BLUE, GREEN, WHITE };

#define Q(v) ((int32_t)((v) * 1024))
static int32_t units(const char *t)               /* sq_text's advance in grid units: 6 a glyph, 4 for '1' and '.' */
{
    int32_t u = 0;
    for (; *t; t++) u += (*t == '1' || *t == '.') ? 4 : 6;
    return u ? u - 2 : 0;
}
/* text with its ink box's top-left (or top-right / top-centre: align 0 / 1 / 2) at (x, y) px Q10; s Q10, w Q10 */
static int32_t text(void *r, int32_t x, int32_t y, int32_t s, int32_t w, const char *t, const uint8_t *c, int align)
{
    int32_t wd = units(t) * s;
    x = align == 1 ? x - wd - w / 2 : align == 2 ? x - wd / 2 : x + w / 2;
    return sq_text(r, x, y + w / 2, s, t, w, c);
}
static char *num(char *b, int32_t v, const char *unit) { return sq_cat(sq_num(b, (uint32_t)v, 1), unit); }

/* sin of a = 0..4095 (a full turn), Q14 */
static int32_t isin(int32_t a)
{
    static const int16_t S[17] = { 0, 1608, 3196, 4756, 6270, 7723, 9102, 10394, 11585, 12665, 13623, 14449, 15137,
                                   15679, 16069, 16305, 16384 };
    a &= 4095;
    int32_t q = a >> 10, i = a & 1023;
    if (q & 1) i = 1024 - i;
    int32_t k = i >> 6, f = i & 63, v = k >= 16 ? S[16] : S[k] + ((S[k + 1] - S[k]) * f >> 6);
    return q & 2 ? -v : v;
}

/* a 7-segment digit as the concept draws it: cell 26 x 40 px at (x, y), bars 3 px (4.1 raster: 3 px natively, measured), skewed 6 degrees about its centre */
static void digit(void *r, int32_t x, int32_t y, int d)
{
    static const uint8_t SEG7[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };   /* bit k: a..g */
    /* centre lines in px x 2 from the cell's top-left (w 26, h 40, t 3: the concept's rects) */
    static const uint8_t L[7][4] = { { 6, 3, 46, 3 }, { 49, 6, 49, 37 }, { 49, 43, 49, 74 }, { 6, 77, 46, 77 },
                                     { 3, 43, 3, 74 }, { 3, 6, 3, 37 }, { 6, 40, 46, 40 } };
    const int32_t sk = 108;                         /* tan 6 degrees, Q10 */
    for (int k = 0; k < 7; k++) {
        int32_t x0 = Q(x) + L[k][0] * 512, y0 = Q(y) + L[k][1] * 512, x1 = Q(x) + L[k][2] * 512, y1 = Q(y) + L[k][3] * 512;
        x0 -= sk * (y0 - Q(y + 20)) >> 10; x1 -= sk * (y1 - Q(y + 20)) >> 10;
        sq_line(r, x0, y0, x1, y1, Q(4.1), (SEG7[d % 10] >> k & 1) ? RED : RED_OFF);
    }
}

/* a knob's pointer: from r 3 to r 9 px at 135 + v * 270 degrees (v Q15) */
static void pointer(void *r, int32_t cx, int32_t v, const uint8_t *c)
{
    int32_t a = 1536 + v * 3072 / 32767, co = isin(a + 1024), si = isin(a);
    int32_t x = Q(cx), y = Q(64);
    sq_line(r, x + (3 * co >> 4), y + (3 * si >> 4), x + (9 * co >> 4), y + (9 * si >> 4), Q(2.5), c);
    sq_dot(r, x + (9 * co >> 4), y + (9 * si >> 4), Q(2.2), c);   /* the concept's round end */
}

static void init(void *s) { for (int i = 0; i < 4; i++) { char id[6] = { 'l', 'o', 'g', 'o', (char)('0' + i), 0 }; logo[i] = op1_find_node(s, id); } }

static void update(void *s, int dt)
{
    int pos = op1_fx_screen_pos() % 3;
    int unit = rv_live[pos][0] & 3, prog = rv_live[pos][1], mode = rv_live[pos][2], size = rv_live[pos][3];
    for (int i = 0; i < 4; i++) if (logo[i]) op1_node_set_visible(logo[i], i == unit);
    op1_screen_draw(s, dt);
    void *r = op1_raster();
    int16_t k[4];
    op1_fx_knobs_live(k);

    /* the LED window and the program's name: fitted to x 18.5..112 (the window's width and the room to the knobs) */
    digit(r, 27, 38, prog / 10 % 10);
    digit(r, 59, 38, prog % 10);
    const char *name = rv_live_name(pos);
    if (name) {
        int32_t s = Q(0.78), u = units(name);
        if (u * s > Q(92)) s = Q(92) / u;
        text(r, Q(18.5), Q(89.7), s, Q(1.15), name, WHITE, 0);   /* the concept's ink starts at 19.8; a stroke "1" starts a unit in */
    }

    /* the knobs: label, pointer, value; each by its own page, a pushed one with its page-1 value small under it */
    char v[3][12], p1[3][12];
    int p2[3];
    int32_t pv[3];
    num(p1[0], (k[0] * 100 + 16383) >> 15, "%");
    {
        int32_t i = k[1] * 32, f = i & 32767; i >>= 15;
        int32_t hz = i >= 32 ? HZ[32] : HZ[i] + ((HZ[i + 1] - HZ[i]) * f >> 15);
        if (hz < 1000) num(p1[1], (hz + 5) / 10 * 10, "HZ");
        else { hz = (hz + 50) / 100; char *e = sq_num(p1[1], (uint32_t)(hz / 10), 1); *e++ = '.'; num(e, hz % 10, "K"); }
    }
    num(p1[2], (k[2] * 100 + 16383) >> 15, "%");
    for (int i = 0; i < 3; i++) p2[i] = op1_fx_page[pos][i] != 0;
    static const char *const LAB[2][3] = { { "FEEDBACK", "CUTOFF", "MIX" }, { "SIZE", "MODE", "UNIT" } };
    if (p2[0]) { num(v[0], 50 + size * 50 / 32767, "%"); pv[0] = size; } else { sq_cat(v[0], p1[0]); pv[0] = k[0]; }
    if (p2[1]) { sq_cat(v[1], mode ? "CLEAN" : "AUTH"); pv[1] = mode ? 32767 : 0; } else { sq_cat(v[1], p1[1]); pv[1] = k[1]; }
    if (p2[2]) { sq_cat(v[2], UNIT[unit]); pv[2] = unit * 32767 / 3; } else { sq_cat(v[2], p1[2]); pv[2] = k[2]; }
    for (int i = 0; i < 3; i++) {
        int32_t cx = 147 + 62 * i;
        text(r, Q(cx), Q(34.6), Q(0.82), Q(1.15), LAB[p2[i]][i], COL[i], 2);
        pointer(r, cx, pv[i], COL[i]);
        text(r, Q(cx), Q(89.6), Q(1.0), Q(1.4), v[i], WHITE, 2);
        if (p2[i]) text(r, Q(cx), Q(98.4), Q(0.62), Q(1.0), p1[i], GREY, 2);
    }

    /* the strip: PROG n/count, the L / R meter, the mode tag */
    char b[16], *e = sq_cat(b, "PROG ");
    e = sq_num(e, (uint32_t)prog, 2);
    e = sq_cat(e, "/");
    sq_num(e, COUNT[unit], 1);
    text(r, Q(18.8), Q(112.8), Q(0.75), Q(1.15), b, GREY, 0);
    text(r, Q(88.2), Q(110.7), Q(0.6), Q(1.0), "L", GREY, 0);
    text(r, Q(88.2), Q(117.2), Q(0.6), Q(1.0), "R", GREY, 0);
    for (int ch = 0; ch < 2; ch++) {
        int32_t pk = rv_live[pos][4 + ch], lit = 20, y = Q(111.5 + 7 * ch);
        for (int32_t t = 32767; lit > 0 && pk < t; t = t * 813 >> 10) lit--;   /* 2 dB a step */
        for (int i = 0; i < 20; i++) {
            int32_t x = Q(94) + i * Q(8.6);
            sq_line(r, x, y, x + Q(7), y, Q(4 * 1.25), i < lit ? (i >= 17 ? RED : GREEN) : (i >= 17 ? RED_OFF : GREEN_OFF));
        }
    }
    text(r, Q(300), Q(111.8), Q(0.95), Q(1.4), mode ? "CLEAN" : "AUTH", RED, 1);
}
const op1_screen_t SCREEN_rackverb = { init, update };
