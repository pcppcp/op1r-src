/* Host test + preview frames for screens/settings.c (T3 settings page): values, clamping, saved value.
 * "render": JSON frames for harness/render_usbout.py */
#include "../op1_synth.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../screens/settings.c"
#include "../input/debounce.c"
#include "../input/key_input.c"
uint8_t ci_beats = 4;
uint32_t key_host_cycles(void) { return 0; }
int key_host_queue_empty(void) { return 1; }
void key_host_wait(int semaphore, uint32_t timeout) { (void)semaphore; (void)timeout; }
void key_event_hook(int code, int pressed) { (void)code; (void)pressed; }

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
    settings_turn(0, 1024); assert(ci_beats == 8 && settings_host_saved == 1);
    frame("blue right: 8", render);
    settings_turn(0, 1024); assert(ci_beats == 16);
    frame("blue right: 16", render);
    settings_turn(0, 1024); assert(ci_beats == 16 && settings_host_saved == 2);   /* clamped */
    settings_turn(2, -4096); assert(ci_beats == 16);                             /* unused knobs */
    settings_turn(0, -1024); settings_turn(0, -1024); settings_turn(0, -1024); assert(ci_beats == 4);
    settings_host_saved = 2; st_init(0); assert(ci_beats == 16);                 /* loaded */
    assert(key_input_debounce_ms() == 5);                                     /* no saved setting */
    for (int i = 0; i < 5; ++i) settings_turn(1, -1024);
    assert(key_input_debounce_ms() == 0 && !strcmp(settings_host_debounce_saved, "00"));
    frame("green left: debounce OFF", render);
    settings_turn(1, -1024); assert(key_input_debounce_ms() == 0);              /* lower bound */
    key_input_set_debounce_ms(5); st_init(0);
    assert(key_input_debounce_ms() == 0);                                     /* OFF survives reload */
    for (int i = 0; i < 31; ++i) settings_turn(1, 1024);
    assert(key_input_debounce_ms() == 30 && !strcmp(settings_host_debounce_saved, "30"));
    frame("green right: debounce 30 MS", render);
    key_input_set_debounce_ms(5); st_init(0);
    assert(key_input_debounce_ms() == 30 && ci_beats == 16);                    /* both settings restored */
    settings_turn(1, -1024); assert(key_input_debounce_ms() == 29);
    settings_turn(0, -1024); assert(key_input_debounce_ms() == 29 && ci_beats == 8);
    st_init(0); assert(key_input_debounce_ms() == 29 && ci_beats == 8);          /* independent saves */
    const char *bad[] = { "", "-1", "31", "999", "1x", " 5", "5.0" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        settings_host_debounce_saved = bad[i]; st_init(0);
        assert(key_input_debounce_ms() == 5);
    }
    settings_host_debounce_saved = "1"; st_init(0);
    assert(key_input_debounce_ms() == 1);
    settings_turn(0, 512); settings_turn(1, 512);
    assert(ci_beats == 8 && key_input_debounce_ms() == 1);                      /* independent detent accumulators */
    settings_turn(1, 512); assert(key_input_debounce_ms() == 2);
    settings_turn(0, 512); assert(ci_beats == 16);
    settings_turn(1, -512); settings_turn(1, 512);
    assert(key_input_debounce_ms() == 2);                                     /* direction reversal resets remainder */
    if (!render) printf("ok\n");
    return 0;
}
