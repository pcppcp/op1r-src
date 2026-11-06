/* Scale page (clock + T4, docs/55), in the style of the settings page: blue root, green scale, white mode; a
 * one-octave keyboard with the scale's notes lit (root blue). Saved in op1rsettings (key 'scale' = root + 16 type
 * + 256 mode). */
#include "../op1_synth.h"
#include "stroke_font.h"
#include "op1r_settings.h"
#include "../scale/sc_core.h"

static void sp_load(void)
{
    int v = op1r_setting_get("scale", -1);                  /* root + 16 type + 256 mode */
    if (v < 0) return;
    int r = v & 15, t = v >> 4 & 15, m = v >> 8;
    if (r < 12 && t < SC_NTYPES && m < SC_NMODES) { sc_root = (uint8_t)r; sc_type = (uint8_t)t; sc_mode = (uint8_t)m; }
}
static void sp_save(void) { op1r_setting_put("scale", sc_root + 16 * sc_type + 256 * sc_mode); }

static int32_t acc[3];
static uint8_t sp_dirty;
/* saved at the next key event (dispatcher.c), not per detent: one DB write per page visit instead of one per value.
 * shortcut: a change made right before power-off without any key press is lost, flush from a power hook if needed */
void scale_flush(void) { if (sp_dirty) { sp_dirty = 0; sp_save(); } }
void scale_turn(int knob, int delta)                        /* blue root, green scale, white mode: one per detent */
{
    if (knob > 2) return;
    if ((acc[knob] > 0 && delta < 0) || (acc[knob] < 0 && delta > 0)) acc[knob] = 0;
    acc[knob] += delta;
    int st = acc[knob] / 1024; if (!st) return;
    acc[knob] = 0;
    uint8_t *v = knob == 0 ? &sc_root : knob == 1 ? &sc_type : &sc_mode;
    int n = knob == 0 ? 12 : knob == 1 ? SC_NTYPES : SC_NMODES;
    int x = *v + (st > 0 ? 1 : -1);
    if (knob == 0) x = (x + 12) % 12;                       /* root wraps */
    else if (x < 0 || x >= n) return;
    *v = (uint8_t)x;
    sp_dirty = 1;
}

static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_grey[4] = { 0x6F, 0x71, 0x73, 0xFF };
#define SP_S   OP1_Q10(5) / 2
#define SP_W   0x600
#define SP_LX  OP1_Q10(24)
#define SP_VX  OP1_Q10(110)
#define KX     OP1_Q10(56)                                  /* keyboard: 7 white keys of 28.5, x 60..260, y 86..134 */
#define KW     29184
#define KY0    OP1_Q10(84)
#define KY1    OP1_Q10(134)
#define KYB    OP1_Q10(114)                                 /* black keys end */

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint8_t *c) { sf_line(r, x0, y0, x1, y1, SP_W, c); }

static void sp_keyboard(void *r)
{
    static const int8_t white[7] = { 0, 2, 4, 5, 7, 9, 11 }, black[5] = { 1, 3, 6, 8, 10 }, bpos[5] = { 1, 2, 4, 5, 6 };
    uint16_t m = sc_mask();
    ln(r, KX, KY0, KX + 7 * KW, KY0, c_grey); ln(r, KX, KY1, KX + 7 * KW, KY1, c_grey);
    for (int i = 0; i <= 7; i++)                            /* white key edges: under a black key from its end */
        ln(r, KX + i * KW, (i == 1 || i == 2 || i == 4 || i == 5 || i == 6) ? KYB : KY0, KX + i * KW, KY1, c_grey);
    for (int i = 0; i < 5; i++) {                           /* black keys: outline, between the white keys */
        int32_t x0 = KX + bpos[i] * KW - KW / 3, x1 = KX + bpos[i] * KW + KW / 3;
        ln(r, x0, KY0, x0, KYB, c_grey); ln(r, x1, KY0, x1, KYB, c_grey); ln(r, x0, KYB, x1, KYB, c_grey);
    }
    for (int i = 0; i < 7; i++) {                           /* lit: a bar at the bottom of the key */
        int pc = (white[i] - sc_root + 12) % 12;
        if (!(m >> pc & 1)) continue;
        const uint8_t *c = pc == 0 && sc_type ? c_blue : c_white;
        int32_t x0 = KX + i * KW + KW / 5, x1 = KX + (i + 1) * KW - KW / 5, y = KY1 - OP1_Q10(6);
        ln(r, x0, y, x1, y, c); ln(r, x0, y - OP1_Q10(2), x1, y - OP1_Q10(2), c);
    }
    for (int i = 0; i < 5; i++) {
        int pc = (black[i] - sc_root + 12) % 12;
        if (!(m >> pc & 1)) continue;
        const uint8_t *c = pc == 0 && sc_type ? c_blue : c_white;
        int32_t x0 = KX + bpos[i] * KW - KW / 6, x1 = KX + bpos[i] * KW + KW / 6, y = KYB - OP1_Q10(5);
        ln(r, x0, y, x1, y, c); ln(r, x0, y - OP1_Q10(2), x1, y - OP1_Q10(2), c);
    }
}

static void sp_init(void *scr) { (void)scr; sp_load(); }
void scale_boot(void) { sp_load(); }                        /* boot: the scale applies before the page is opened */
static void sp_update(void *scr, int dt)
{
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    sf_text(r, SP_LX, OP1_Q10(12), SP_S, "ROOT", SP_W, c_blue);
    sf_text(r, SP_VX, OP1_Q10(12), SP_S, sc_root_names[sc_root], SP_W, c_blue);
    sf_text(r, SP_LX, OP1_Q10(34), SP_S, "SCALE", SP_W, c_green);
    sf_text(r, SP_VX, OP1_Q10(34), SP_S, sc_type_names[sc_type], SP_W, c_green);
    sf_text(r, SP_LX, OP1_Q10(56), SP_S, "MODE", SP_W, c_white);
    sf_text(r, SP_VX, OP1_Q10(56), SP_S, sc_mode_names[sc_mode], SP_W, c_white);
    sp_keyboard(r);
}
const op1_screen_t SCREEN_scale = { sp_init, sp_update };
