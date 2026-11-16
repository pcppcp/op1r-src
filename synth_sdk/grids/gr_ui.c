/* grids sequencer page (docs/52): settings per mode, knob turns / presses, the screen. The player glue (gr_fw.c, next
 * step) calls gr_ui_step on each Grids step; the host renders the screen through gr_ui_draw (harness/grids_render.c).
 *   blue  : X              Shift: swing
 *   green : Y              Shift: resolution 1/16 (the module's default 4 ppqn: even steps) / 1/32 (24 ppqn)
 *   white : fill of the selected part      Shift: its pad (drum) / note (synth)      press: next part
 *   red   : chaos          Shift: accent amount                                      press: play / stop */
#include "../op1_synth.h"
#include "gr_core.h"
#include "gr_ui.h"

gr_bank gr_b[2];                                              /* 0 synth, 1 drum */

static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     c_grey[4] = { 0x6F, 0x71, 0x73, 0xFF }, c_dim[4] = { 0x3A, 0x3B, 0x3D, 0xFF };
static const uint8_t *const col4[4] = { c_blue, c_green, c_white, c_red };
static const uint8_t *const part_col[GR_PARTS] = { c_blue, c_green, c_white };
static const char *const part_name[GR_PARTS] = { "bd", "sd", "hh" };
static const uint8_t drum_pad_def[GR_PARTS] = { 0, 2, 9 };   /* kick, snare, closed hat keys (shortcut: per kit) */
static const uint8_t synth_note_def[GR_PARTS] = { 36, 43, 48 };

void gr_ui_init(int m)
{
    gr_bank *b = &gr_b[m & 1];
    uint8_t *p = (uint8_t *)b;
    for (unsigned i = 0; i < sizeof *b; i++) p[i] = 0;
    gr_init(&b->g, 0x12345u + (uint32_t)m);
    b->g.s.x = 128; b->g.s.y = 128; b->g.s.chaos = 0;
    for (int i = 0; i < GR_PARTS; i++) {
        b->g.s.density[i] = 128;
        b->note[i] = m ? drum_pad_def[i] : synth_note_def[i];
    }
    b->accent = 47;                                           /* velocity 127 accents, 80 others */
    b->playing = 0;                                           /* stopped until red press (not saved) */
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
void gr_ui_turn(int m, int knob, int detents, int shift)      /* detents: whole steps of the encoder */
{
    gr_bank *b = &gr_b[m & 1];
    gr_set *s = &b->g.s;
    if (!shift) switch (knob) {
        case 0: s->x = (uint8_t)clampi(s->x + detents * 4, 0, 255); break;
        case 1: s->y = (uint8_t)clampi(s->y + detents * 4, 0, 255); break;
        case 2: s->density[b->sel] = (uint8_t)clampi(s->density[b->sel] + detents * 4, 0, 255); break;
        default: s->chaos = (uint8_t)clampi(s->chaos + detents * 4, 0, 255); break;
    } else switch (knob) {
        case 0: b->swing = (uint8_t)clampi(b->swing + detents, 0, 42); break;
        case 1: if (detents) b->res32 = detents > 0; break;
        case 2: b->note[b->sel] = (uint8_t)clampi(b->note[b->sel] + detents, 0, m ? 23 : 127); break;
        default: b->accent = (uint8_t)clampi(b->accent + detents * 4, 0, 127); break;
    }
}
void gr_ui_press(int m, int knob)
{
    gr_bank *b = &gr_b[m & 1];
    if (knob == 2) b->sel = (uint8_t)((b->sel + 1) % GR_PARTS);
    else if (knob == 3) b->playing ^= 1;
}
/* one Grids step (0..31) on the player's clock: hits / accents as gr_eval; skipped steps at 1/16 return 0 */
int gr_ui_step(int m, int step)
{
    gr_bank *b = &gr_b[m & 1];
    b->step = (uint8_t)step;
    if (!b->res32 && (step & 1)) return 0;
    return gr_eval(&b->g, step);
}

/* ---- screen ---- */
#define Q(v) ((v) << 10)
#define MAP_X 12                                              /* node (0, 0); 18 px apart */
#define MAP_Y 18
#define NODE  18
#define LX0   112                                             /* lanes: step 0's x, 6 px a step */
#define LSTEP 6
#define LY0   30                                              /* BD baseline; SD, HH 26 px below each */
#define LDY   26
enum { TX_LAB, TX_VAL = 4, TX_PRS = 8, TX_PART = 10, TX_N = 13 };
static uint32_t tx[TX_N][OP1_TEXT_SIZE / 4];
static char shown[TX_N][14];
static uint8_t tx_made;

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    if (x0 == x1 && y0 == y1) return;
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
static void txt(void *r, int i, const char *s)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    int same = 1;
    for (int j = 0; j < 14; j++) { if (shown[i][j] != s[j]) { same = 0; break; } if (!s[j]) break; }
    if (!same) { op1_text_set(tx[i], s); int j = 0; for (; j < 13 && s[j]; j++) shown[i][j] = s[j]; shown[i][j] = 0; }
    op1_text_draw(tx[i], r, ident);
}
static void dimc(uint8_t *o, const uint8_t *c) { for (int i = 0; i < 3; i++) o[i] = (uint8_t)(c[i] * 2 / 5); o[3] = 0xFF; }
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *num(char *p, int v) { char t[8]; int n = 0; if (v < 0) { *p++ = '-'; v = -v; } do t[n++] = (char)('0' + v % 10); while ((v /= 10)); while (n) *p++ = t[--n]; *p = 0; return p; }
static void pct(char *p, int v255) { put(num(p, (v255 * 100 + 127) / 255), "%"); }
static void note_name(char *p, int m, int n)
{
    static const char *const nn[12] = { "c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b" };
    if (m) { num(put(p, "pad "), n + 1); return; }
    num(put(p, nn[n % 12]), n / 12 - 1);
}

void gr_ui_draw(void *r, int m, int shift)
{
    gr_bank *b = &gr_b[m & 1];
    const gr_set *s = &b->g.s;
    if (!tx_made) {
        for (int k = 0; k < 4; k++) {
            op1_text_ctor(tx[TX_LAB + k], Q(8 + 78 * k), 114, 12, col4[k]);
            op1_text_ctor(tx[TX_VAL + k], Q(8 + 78 * k), 127, 12, c_white);
        }
        op1_text_ctor(tx[TX_PRS], Q(8 + 78 * 2), 94, 12, c_white);
        op1_text_ctor(tx[TX_PRS + 1], Q(8 + 78 * 3), 94, 12, c_red);
        for (int i = 0; i < GR_PARTS; i++) op1_text_ctor(tx[TX_PART + i], Q(LX0 - 20), LY0 - 12 + LDY * i, 12, part_col[i]);
        tx_made = 1;
    }
    /* the map: 5 x 5 nodes, the X / Y point */
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++) {
            int32_t x = Q(MAP_X + NODE * i), y = Q(MAP_Y + NODE * j);
            ln(r, x - Q(2), y, x + Q(2), y, 0x300, c_grey);
            ln(r, x, y - Q(2), x, y + Q(2), 0x300, c_grey);
        }
    int32_t cx = Q(MAP_X) + s->x * Q(NODE * 4) / 255, cy = Q(MAP_Y) + s->y * Q(NODE * 4) / 255;
    ln(r, cx - Q(7), cy, cx - Q(2), cy, 0x600, c_red); ln(r, cx + Q(2), cy, cx + Q(7), cy, 0x600, c_red);
    ln(r, cx, cy - Q(7), cx, cy - Q(2), 0x600, c_red); ln(r, cx, cy + Q(2), cx, cy + Q(7), 0x600, c_red);

    /* the lanes: this bar's pattern (chaos included) */
    uint32_t hits[GR_PARTS], acc[GR_PARTS];
    gr_bar(&b->g, hits, acc);
    for (int i = 0; i < GR_PARTS; i++) {
        uint8_t dim[4];
        dimc(dim, part_col[i]);
        const uint8_t *c = i == b->sel ? part_col[i] : dim;
        int32_t y = Q(LY0 + LDY * i);
        ln(r, Q(LX0 - 2), y + Q(1), Q(LX0 + LSTEP * GR_STEPS), y + Q(1), 0x300, c_dim);   /* the ground */
        for (int t = 0; t < GR_STEPS; t++) {
            if (!b->res32 && (t & 1)) continue;
            if (!(hits[i] >> t & 1)) continue;
            int32_t x = Q(LX0 + LSTEP * t + 1), h = acc[i] >> t & 1 ? Q(14) : Q(7);
            ln(r, x, y, x, y - h, i == b->sel ? 0x800 : 0x600, c);
        }
        txt(r, TX_PART + i, part_name[i]);
    }
    if (b->playing) {                                          /* the playhead */
        int32_t x = Q(LX0 + LSTEP * b->step + 1);
        ln(r, x, Q(LY0 - 18), x, Q(LY0 + LDY * 2 + 4), 0x300, c_red);
    }

    /* the bottom row */
    for (int k = 0; k < 4; k++) ln(r, Q(8 + 78 * k), Q(110), Q(78 + 78 * k), Q(110), 0x800, col4[k]);
    char v[16], l[16];
    if (!shift) {
        txt(r, TX_LAB, "x"); pct(v, s->x); txt(r, TX_VAL, v);
        txt(r, TX_LAB + 1, "y"); pct(v, s->y); txt(r, TX_VAL + 1, v);
        put(put(l, part_name[b->sel]), " fill"); txt(r, TX_LAB + 2, l); pct(v, s->density[b->sel]); txt(r, TX_VAL + 2, v);
        txt(r, TX_LAB + 3, "chaos"); pct(v, s->chaos); txt(r, TX_VAL + 3, v);
    } else {
        txt(r, TX_LAB, "swing"); num(v, b->swing); txt(r, TX_VAL, v);
        txt(r, TX_LAB + 1, "res"); txt(r, TX_VAL + 1, b->res32 ? "1/32" : "1/16");
        put(put(l, part_name[b->sel]), m ? " pad" : " note"); txt(r, TX_LAB + 2, l); note_name(v, m, b->note[b->sel]); txt(r, TX_VAL + 2, v);
        txt(r, TX_LAB + 3, "accent"); num(v, b->accent); txt(r, TX_VAL + 3, v);
    }
    txt(r, TX_PRS, "next");
    txt(r, TX_PRS + 1, b->playing ? "stop" : "play");
}
