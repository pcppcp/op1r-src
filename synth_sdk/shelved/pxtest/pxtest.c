/* PXTEST: a test screen for the pixel snap (screens/pxsnap.h), as an FX that passes the sound through. Left: drawn as
 * before (raw raster lines at fractional positions); right: the same drawn snapped to the running panel's grid. The
 * top line names the panel scale R246 uses (1280 = 400 x 200, 1203 = 376 x 188, 1024 = 320 x 156). Not shipped. */
#include "op1_plugin.h"
#include "../../screens/stroke_font.h"

static void fx_init(int pos) { (void)pos; }
static void fx_render(int pos, int16_t *buf, const int16_t *p) { (void)pos; (void)buf; (void)p; }
const op1_fx_t FX_pxtest = { fx_init, fx_render, { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE } };

#define Q(v) ((int32_t)((v) * 1024))
static const uint8_t WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, BLUE[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, GREEN[4] = { 0x00, 0xED, 0x95, 0xFF },
                     GREY[4] = { 0x6F, 0x71, 0x73, 0xFF };
static int32_t raw_text(void *r, int32_t x, int32_t y, int32_t s, const char *t, int32_t w, const uint8_t *c)
{
    for (; *t; t++) {
        for (unsigned k = 0; k < sizeof sf_font / sizeof sf_font[0]; k++) if (sf_font[k].c == *t) {
            for (int j = 0; j < sf_font[k].n; j++) {
                const uint8_t *g = sf_font[k].seg[j];
                px_raw(r, x + g[0] * s, y + g[1] * s, x + g[2] * s, y + g[3] * s, w, c);
            }
            break;
        }
        x += (*t == '1' || *t == '.' ? 4 : 6) * s;
    }
    return x;
}
typedef void (*line_fn)(void *, int32_t, int32_t, int32_t, int32_t, int32_t, const uint8_t *);
static void snap_line(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c) { px_line(r, x0, y0, x1, y1, w, c, 1); }
static void column(void *r, int32_t x, int snapped)
{
    line_fn L = snapped ? snap_line : px_raw;
    for (int i = 0; i < 4; i++) L(r, x, Q(26) + Q(3.3) * i, x + Q(130), Q(26) + Q(3.3) * i, Q(1), WHITE);      /* 1 px, fractional y */
    for (int i = 0; i < 9; i++) L(r, x + Q(3.3) * i, Q(40), x + Q(3.3) * i, Q(62), Q(1), BLUE);              /* 1 px, fractional x */
    L(r, x + Q(40.4), Q(40.6), x + Q(129.7), Q(40.6), Q(1), GREEN);                                     /* a box */
    L(r, x + Q(40.4), Q(61.3), x + Q(129.7), Q(61.3), Q(1), GREEN);
    L(r, x + Q(40.4), Q(40.6), x + Q(40.4), Q(61.3), Q(1), GREEN);
    L(r, x + Q(129.7), Q(40.6), x + Q(129.7), Q(61.3), Q(1), GREEN);
    L(r, x, Q(118.5), x + Q(130), Q(118.5), Q(2), GREY);                                                  /* 2 px */
    L(r, x + Q(10), Q(124), x + Q(120), Q(134), Q(1), WHITE);                                            /* a diagonal */
    if (snapped) { sf_text(r, x, Q(70.4), Q(1.5), "OP1R 0123 PLK", Q(1), WHITE); sf_text(r, x, Q(88.7), Q(2), "SYNC 1/4", Q(1.2), WHITE); }
    else { raw_text(r, x, Q(70.4), Q(1.5), "OP1R 0123 PLK", Q(1), WHITE); raw_text(r, x, Q(88.7), Q(2), "SYNC 1/4", Q(1.2), WHITE); }
}
static char *num(char *p, int v) { char t[12]; int n = 0; do t[n++] = (char)('0' + v % 10); while (v /= 10); while (n) *p++ = t[--n]; *p = 0; return p; }
static void scr_init(void *scr) { (void)scr; }
static void scr_update(void *scr, int dt)
{
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    char t[32] = "PANEL ", *p = num(t + 6, px_dev_scale());
    *p++ = ' '; *p++ = 'O'; *p++ = 'F'; *p++ = 'F'; *p++ = ' '; num(p, px_dev_xoff());
    sf_text(r, Q(10), Q(6), Q(1.5), t, Q(1), WHITE);
    raw_text(r, Q(10), Q(17), Q(1), "RAW", Q(1), GREY);
    sf_text(r, Q(170), Q(17), Q(1), "SNAP", Q(1), GREY);
    column(r, Q(10), 0);
    column(r, Q(170), 1);
}
const op1_screen_t SCREEN_pxtest = { scr_init, scr_update };
