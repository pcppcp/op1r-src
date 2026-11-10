/* Preview frames of the pulse sync page (screens/pcpage.c) for render_pcpage.py, one JSON frame per line. */
#include "../op1_synth.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define MAXL 400
static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } lines[MAXL]; static int nlines;
void *host_raster(void) { return &nlines; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c)
{
    (void)r; const op1_pt *p = a, *q = b;
    assert(nlines < MAXL);
    lines[nlines].x0 = p->x; lines[nlines].y0 = p->y; lines[nlines].x1 = q->x; lines[nlines].y1 = q->y; lines[nlines].w = w;
    memcpy(lines[nlines].c, c, 4); nlines++;
}
typedef struct { int32_t x, y, sz; uint8_t c[4]; char s[32]; } txt_t;
static txt_t *drawn[32]; static int nt;
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t sz, const uint8_t *c)
{ txt_t *t = o; t->x = x; t->y = y; t->sz = sz; memcpy(t->c, c, 4); t->s[0] = 0; return o; }
void host_text_set(void *o, const char *s) { snprintf(((txt_t *)o)->s, 32, "%s", s); }
void host_text_draw(void *o, void *r, const int32_t *m) { (void)r; (void)m; if (nt < 32) drawn[nt++] = o; }
/* the pulse clock state the page shows (midi/pclk.c) */
volatile uint8_t pc_src = 3, pc_manual, pc_fall, pc_level, pc_chan;
int host_shift;
volatile int32_t pc_show_jump, pc_show_trig = 2400;
volatile uint32_t pc_pulses;
uint8_t pc_running; uint32_t pc_period, pc_count;
const int16_t pc_level_val[7] = { 0, 800, 1600, 3200, 6400, 12800, 25600 };
int32_t host_sync_mode = 2; static int in_ok = 1;
int pc_input_ok(void) { return in_ok; }
int op1r_setting_get(const char *k, int d) { (void)k; return d; }
void op1r_setting_put(const char *k, int v) { (void)k; (void)v; }
void pcp_draw(void *r);
void pc_set_src(int v) { pc_src = (uint8_t)v; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
static void frame(const char *label)
{
    nlines = 0; nt = 0;
    pcp_draw(&nlines);
    printf("{\"label\": \"%s\", \"lines\": [", label);
    for (int i = 0; i < nlines; i++)
        printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", lines[i].x0, lines[i].y0, lines[i].x1, lines[i].y1,
               lines[i].w, lines[i].c[0], lines[i].c[1], lines[i].c[2], lines[i].c[3]);
    printf("], \"text\": [");
    for (int i = 0; i < nt; i++)
        printf("%s[%d, %d, [%d, %d, %d], \"%s\", 0, %d]", i ? ", " : "", drawn[i]->x, drawn[i]->y, drawn[i]->c[0], drawn[i]->c[1],
               drawn[i]->c[2], drawn[i]->s, drawn[i]->sz);
    printf("]}\n");
}
int main(void)
{
    pc_running = 1; pc_period = 5512; pc_count = 9; pc_pulses = 1; pc_show_jump = 9000;
    frame("following: 4 ppq, 120.0 bpm, beat 3, a pulse just now");
    pc_pulses = 2; frame("following, between pulses");
    pc_running = 0; pc_show_jump = 300; frame("waiting for pulses");
    pc_src = 4; pc_manual = 1; pc_fall = 1; pc_level = 3; pc_show_trig = 3200; frame("24 ppq, manual start, falling edge, level 3");
    pc_chan = 2; frame("channel right"); host_shift = 1; frame("Shift held: the red column is the channel"); host_shift = 0; pc_chan = 0;
    pc_src = 3; pc_manual = 0; pc_fall = 0; pc_level = 0; pc_show_trig = 2400;
    host_sync_mode = 1; frame("sync not on follow");
    host_sync_mode = 2; in_ok = 0; frame("mic or radio selected");
    in_ok = 1; pc_src = 0; frame("rate: midi (the stock midi clock)");
    return 0;
}
