/* Pixel snap for raster lines (after hundekraft 3.3.1 seq/seq_text.h sq_snap / sq_tline / sq_linemin, Hundekraft;
 * the idea u/Infamous_Call_6492's; GPL-3.0-or-later with the OP-1 firmware permission).
 *
 * R246 draws screen unit x at device pixel x * s + off and y at y * s; s is the screen matrix's scale (0x01DE5E68
 * word 3, set by the Raster constructor 0x01A2361C from the panel flags): 0x500 (Q10 1.25, the default 400 x 200
 * panel), 0x4B3 (376 x 188 panels, off = 1/16 px on x), 0x400 (320 x 156). A line of a whole number of pixels lights
 * whole pixels only when it lies on pixel centres (odd width) or pixel edges (even width); anywhere else it lights two
 * half-bright rows (soft on the OLED). px_line / px_tline put horizontal and vertical strokes on the running panel's
 * grid (across by the width's parity, along with their ends on pixel edges), make the width whole and at least `min`
 * (1: the weight stays, only the position moves); diagonals keep their ends. Coordinates are Q10 screen units, the
 * width Q10 device pixels (as op1_raster_line). Host builds: OP1_HOST_PANEL=default|b|a (or host_dev_scale). */
#ifndef PXSNAP_H
#define PXSNAP_H
#include <stdint.h>

#ifndef OP1_HOST
static inline int32_t px_dev_scale(void)
{ int32_t s = *(const volatile int32_t *)0x01DE5E74; return s == 0x4B3 || s == 0x400 ? s : 0x500; }
#else
#include <stdlib.h>
__attribute__((weak)) int32_t host_dev_scale;            /* 0: from OP1_HOST_PANEL */
static inline int32_t px_dev_scale(void)
{
    if (!host_dev_scale) {
        const char *p = getenv("OP1_HOST_PANEL");
        host_dev_scale = p && (p[0] == 'b' || p[0] == 'c') ? 0x4B3 : p && p[0] == 'a' ? 0x400 : 0x500;
    }
    return host_dev_scale;
}
#endif
static inline int32_t px_dev_xoff(void) { return (-320 * px_dev_scale()) & 1023; }   /* Q10 px */

/* v (Q10 units) on axis xa (1: x) -> Q10 device px: a pixel centre (odd) or the nearest pixel edge; and back */
static inline int32_t px_snap(int32_t v, int32_t odd, int32_t xa)
{
    int32_t p = (int32_t)((int64_t)v * px_dev_scale() >> 10) + (xa ? px_dev_xoff() : 0);
    return odd ? (p & ~1023) + 512 : (p + 512) & ~1023;
}
static inline int32_t px_unit(int32_t p, int32_t xa)
{
    int32_t s = px_dev_scale();
    return (int32_t)((((int64_t)(p - (xa ? px_dev_xoff() : 0)) << 10) + s / 2) / s);
}
static __attribute__((unused)) void px_raw(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
/* a glyph stroke: both ends snapped, square caps of whole pixels on straight strokes (the ends meet at corners) */
static __attribute__((unused)) void px_tline(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    int32_t n = (w + 512) >> 10, odd, h;
    if (n < 1) n = 1;
    odd = n & 1; h = n << 9;
    x0 = px_snap(x0, odd, 1); y0 = px_snap(y0, odd, 0); x1 = px_snap(x1, odd, 1); y1 = px_snap(y1, odd, 0);
    if (y0 == y1) { int32_t d = x1 < x0 ? -h : h; x0 -= d; x1 += d; }
    else if (x0 == x1) { int32_t d = y1 < y0 ? -h : h; y0 -= d; y1 += d; }
    px_raw(r, px_unit(x0, 1), px_unit(y0, 0), px_unit(x1, 1), px_unit(y1, 0), n << 10, c);
}
/* a line: straight ones on the grid, the width whole and at least min px; diagonals unchanged but for the width */
static __attribute__((unused)) void px_line(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c, int32_t min)
{
    int32_t n = (w + 512) >> 10, odd;
    if (n < min) n = min;
    odd = n & 1;
    if (y0 == y1) {
        int32_t a = px_snap(x0, 0, 1), b = px_snap(x1, 0, 1);
        if (a == b && x0 != x1) b += x1 > x0 ? 1024 : -1024;
        x0 = px_unit(a, 1); x1 = px_unit(b, 1); y0 = y1 = px_unit(px_snap(y0, odd, 0), 0);
    } else if (x0 == x1) {
        int32_t a = px_snap(y0, 0, 0), b = px_snap(y1, 0, 0);
        if (a == b && y0 != y1) b += y1 > y0 ? 1024 : -1024;
        y0 = px_unit(a, 0); y1 = px_unit(b, 0); x0 = x1 = px_unit(px_snap(x0, odd, 1), 1);
    }
    px_raw(r, x0, y0, x1, y1, n << 10, c);
}
#endif
