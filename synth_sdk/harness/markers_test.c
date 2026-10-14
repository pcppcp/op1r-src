/* Host test for tape/markers.c: add / duplicate guard / remove tolerance / jumps with wrap / save-load / limits.
 * "render <cursor> <zoom>": JSON lines of one frame for the preview. */
#include "../op1_synth.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
volatile uint8_t pu_mode;
void mt_draw(void) {}
#include "../tape/markers.c"

static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } L[512]; static int nl;
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c)
{ (void)r; const op1_pt *a = p0, *b = p1; L[nl].x0 = a->x; L[nl].y0 = a->y; L[nl].x1 = b->x; L[nl].y1 = b->y; L[nl].w = w; memcpy(L[nl].c, c, 4); nl++; }
void *host_raster(void) { return (void *)1; }

#define S 44100
int main(int argc, char **argv)
{
    if (argc > 3 && !strcmp(argv[1], "render")) {
        strcpy(tm_host_saved, "220500,264600,308700,396900,407000");          /* 5, 6, 7, 9, 9.23 s */
        tm_host_cursor = atoi(argv[2]); tm_host_zoom = (float)atof(argv[3]);
        if (argc > 4) pu_mode = (uint8_t)atoi(argv[4]);
        nl = 0; tm_draw();
        printf("[");
        for (int i = 0; i < nl; i++) printf("%s[%d,%d,%d,%d,%d,[%d,%d,%d,%d]]", i ? "," : "", L[i].x0, L[i].y0, L[i].x1, L[i].y1, L[i].w, L[i].c[0], L[i].c[1], L[i].c[2], L[i].c[3]);
        printf("]\n");
        return 0;
    }
    tm_host_saved[0] = 0;
    tm_host_cursor = 10 * S; tm_add();
    tm_host_cursor = 2 * S;  tm_add();
    tm_host_cursor = 2 * S + 1000; tm_add();                 /* < 50 ms: ignored */
    tm_host_cursor = 30 * S; tm_add();
    assert(tm_n == 3 && tm_pos[0] == 2 * S && tm_pos[1] == 10 * S && tm_pos[2] == 30 * S);
    assert(!strcmp(tm_host_saved, "88200,441000,1323000"));
    /* jumps: next / prev with wrap, one per detent */
    tm_host_cursor = 5 * S; tm_turn(1024); assert(tm_host_located == 10 * S);
    tm_host_cursor = 10 * S; tm_turn(1024); assert(tm_host_located == 30 * S);   /* on a marker: the next one */
    tm_host_cursor = 30 * S; tm_turn(1024); assert(tm_host_located == 2 * S);    /* wrap */
    tm_host_cursor = 2 * S;  tm_turn(-1024); assert(tm_host_located == 30 * S);  /* wrap back */
    tm_host_cursor = 12 * S; tm_turn(-1024); assert(tm_host_located == 10 * S);
    tm_host_located = -1; tm_turn(500); assert(tm_host_located == -1); tm_turn(600); assert(tm_host_located == 30 * S);   /* 2 part-detents = 1 */
    /* remove: within 3 px at 40 px/s = 3307 samples */
    tm_host_zoom = 40.0f;
    tm_host_cursor = 10 * S + 4000; tm_remove(); assert(tm_n == 3);
    tm_host_cursor = 10 * S + 3000; tm_remove(); assert(tm_n == 2 && tm_pos[1] == 30 * S);
    assert(!strcmp(tm_host_saved, "88200,1323000"));
    /* load: sorted, deduplicated, range-checked */
    strcpy(tm_host_saved, "5,3,99999999,7,x,8");
    tm_load(); assert(tm_n == 3 && tm_pos[0] == 5 && tm_pos[1] == 7 && tm_pos[2] == 8);
    /* limit */
    tm_n = 0;
    for (int i = 0; i < 40; i++) { tm_host_cursor = i * S; tm_add(); }
    assert(tm_n == TM_MAX);
    tm_host_cursor = -5; tm_n = 0; tm_add(); assert(tm_n == 0);
    /* zoom conversion vs float */
    float zs[] = { 0.5f, 1.0f, 5.0f, 40.0f, 41.7f, 145.4f, 1454.5f, 9999.0f, -3.0f, 0.0f };
    for (unsigned i = 0; i < sizeof zs / sizeof zs[0]; i++) {
        tm_host_zoom = zs[i];
        float want = zs[i] < 1 ? 1 : zs[i] > 4096 ? 4096 : zs[i];
        assert(abs(tm_zoom_q10() - (int)(want * 1024)) <= 1);
    }
    printf("ok\n");
    return 0;
}
