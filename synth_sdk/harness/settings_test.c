/* Host test + preview frames for screens/settings.c (T3 settings page): values, clamping, saved value.
 * "render": JSON frames for harness/render_usbout.py */
#include "../op1_synth.h"
#include "../screens/op1r_settings.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../screens/settings.c"
uint8_t ci_beats = 4;

static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } L[256]; static int nl;
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
    nl = nt = 0; st_update(0, 1);
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
    st_init(0);
    assert(ci_beats == 4);
    frame("count-in 4 (default)", render);
    settings_turn(0, 1024); settings_flush(); assert(ci_beats == 8 && op1r_setting_get("countin", -1) == 1);
    frame("blue right: 8", render);
    settings_turn(0, 1024); assert(ci_beats == 16);
    frame("blue right: 16", render);
    settings_turn(0, 1024); settings_flush(); assert(ci_beats == 16 && op1r_setting_get("countin", -1) == 2);   /* clamped */
    settings_turn(1, -4096); assert(ci_beats == 16);                             /* other knobs: nothing yet */
    settings_turn(0, -1024); settings_turn(0, -1024); settings_turn(0, -1024); assert(ci_beats == 4);
    op1r_setting_put("countin", 2); st_init(0); assert(ci_beats == 16);                 /* loaded */
    /* green: keyboard velocity 1 .. 127, 4 steps per detent (2 events of 1024), default 127 (the stock keys) */
    assert(kb_vel == 127);
    settings_turn(1, 1024); settings_turn(1, 1024); assert(kb_vel == 127);              /* clamped at 127 */
    for (int i = 0; i < 10; i++) settings_turn(1, -1024);                                /* 5 detents down */
    settings_flush(); assert(kb_vel == 107 && op1r_setting_get("kbvel", -1) == 107);
    frame("green left 5 detents: velocity 107", render);
    for (int i = 0; i < 100; i++) settings_turn(1, -1024); assert(kb_vel == 1);           /* clamped at 1 */
    settings_turn(1, 512); assert(kb_vel == 2);
    settings_flush(); kb_vel = 127; st_init(0); assert(kb_vel == 2);                      /* loaded */
    op1r_setting_put("kbvel", 0); kb_vel = 127; st_init(0); assert(kb_vel == 127);       /* out of range: kept */
    if (!render) printf("ok\n");
    return 0;
}
