/* grids page (grids/gr_ui.c): the note names shown for a part's note (MIDI 0..127, octave -1..9) and pad.
 *   cc -DOP1_HOST -I.. grids_ui_test.c ../grids/gr_core.c */
#include "../grids/gr_ui.c"
#include <stdio.h>
#include <string.h>
void *host_raster(void) { return 0; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c) { (void)r; (void)a; (void)b; (void)w; (void)c; }
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t sz, const uint8_t *c) { (void)x; (void)y; (void)sz; (void)c; return o; }
void host_text_set(void *o, const char *s) { (void)o; (void)s; }
void host_text_draw(void *o, void *r, const int32_t *m) { (void)o; (void)r; (void)m; }

int main(void)
{
    static const struct { int m, n; const char *s; } c[] = {
        { 0, 0, "c-1" }, { 0, 11, "b-1" }, { 0, 12, "c0" }, { 0, 36, "c2" }, { 0, 61, "c#4" }, { 0, 127, "g9" }, { 1, 0, "pad 1" } };
    int fails = 0;
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        char t[16]; note_name(t, c[i].m, c[i].n);
        if (strcmp(t, c[i].s)) { printf("FAIL %s %d: \"%s\", want \"%s\"\n", c[i].m ? "pad" : "note", c[i].n, t, c[i].s); fails++; }
    }
    printf(fails ? "%d FAILED\n" : "grids ui: ok\n", fails);
    return fails != 0;
}
