/* Host test + preview frames for screens/scalepg.c (clock T4 scale page, docs/55). "render": JSON frames for
 * harness/render_usbout.py */
#include "../op1_synth.h"
#include "../screens/op1r_settings.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../screens/scalepg.c"
#include "../scale/sc_core.c"

static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } L[512]; static int nl;
typedef struct { int32_t x, y; uint8_t c[4]; char s[32]; } txt_t;
static txt_t *drawn[16]; static int nt;
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c)
{ (void)r; const op1_pt *a = p0, *b = p1; L[nl].x0 = a->x; L[nl].y0 = a->y; L[nl].x1 = b->x; L[nl].y1 = b->y; L[nl].w = w; memcpy(L[nl].c, c, 4); nl++; }
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t sz, const uint8_t *c)
{ txt_t *t = o; (void)sz; t->x = x; t->y = y; memcpy(t->c, c, 4); t->s[0] = 0; return o; }
void host_text_set(void *o, const char *s) { snprintf(((txt_t *)o)->s, 32, "%s", s); }
void host_text_draw(void *o, void *r, const int32_t *m) { (void)r; (void)m; drawn[nt++] = o; }
void *host_raster(void) { return (void *)1; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }

static void frame(const char *label, int render)
{
    nl = nt = 0; sp_update(0, 1);
    if (!render) return;
    printf("{\"label\": \"%s\", \"lines\": [", label);
    for (int i = 0; i < nl; i++) printf("%s[%d,%d,%d,%d,%d,[%d,%d,%d,%d]]", i ? "," : "", L[i].x0, L[i].y0, L[i].x1, L[i].y1, L[i].w, L[i].c[0], L[i].c[1], L[i].c[2], L[i].c[3]);
    printf("], \"text\": [");
    for (int i = 0; i < nt; i++) printf("%s[%d,%d,[%d,%d,%d],\"%s\"]", i ? "," : "", drawn[i]->x, drawn[i]->y, drawn[i]->c[0], drawn[i]->c[1], drawn[i]->c[2], drawn[i]->s);
    printf("]}\n");
}

int main(int argc, char **argv)
{
    int render = argc > 1 && !strcmp(argv[1], "render");
    sp_init(0); assert(sc_type == 0);
    frame("off (default)", render);
    scale_turn(1, 1024); assert(op1r_setting_get("scale", -1) == -1);              /* not per detent */
    scale_flush(); assert(sc_type == 1 && op1r_setting_get("scale", -1) == 16);
    frame("C major, snap", render);
    for (int i = 0; i < 3; i++) scale_turn(0, -1024);
    assert(sc_root == 9);   /* wraps: C -> A */
    for (int i = 0; i < 10; i++) scale_turn(1, 1024);
    assert(sc_type == 11);
    scale_turn(2, 1024); assert(sc_mode == 1);
    frame("A minor penta, white keys", render);
    for (int i = 0; i < 5; i++) scale_turn(1, 1024);
    assert(sc_type == 12);
    for (int i = 0; i < 4; i++) scale_turn(2, 1024);
    assert(sc_mode == 2);   /* clamped */
    scale_turn(3, 1024); assert(sc_mode == 2);
    op1r_setting_put("scale", 9 + 16 * 11 + 256); sp_init(0); assert(sc_root == 9 && sc_type == 11 && sc_mode == 1);
    if (!render) printf("ok\n");
    return 0;
}
