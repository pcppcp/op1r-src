/* Preview frames of the grids sequencer page (grids/gr_ui.c) for render_grids.py, one JSON frame per line.
 * plus a test of the player glue (grids/gr_fw.c): notes on the 96-per-beat ticks, gates, swing, synth keys.
 *   cc -DOP1_HOST -I.. grids_render.c ../grids/gr_fw.c ../grids/gr_ui.c ../grids/gr_core.c -o build/grids_render */
#include "../op1_synth.h"
#include "../grids/gr_ui.h"
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

static void frame(const char *label, int m, int shift)
{
    nlines = 0; nt = 0;
    gr_ui_draw(&nlines, m, shift);
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
static void play(int m, int steps) { for (int t = 0; t < steps; t++) gr_ui_step(m, t % GR_STEPS); }
int sq_join(char *out, int n, const int *v, int k) { (void)out; (void)n; (void)v; return k; }
int sq_split(const char *s, int len, int *v, int k) { (void)s; (void)len; (void)v; return k; }
void gr_tick(int m); void gr_turn(int m, int knob, int delta, int shift); void gr_press(int m, int knob);
int gr_key_down(int m, int key); void gr_key_up(int m, int key); void gr_restart(int m);
static char nlog[2048]; static int now;
void host_gr_note(int m, int note, int vel, int on)
{ (void)m; sprintf(nlog + strlen(nlog), "%d%c%d/%d ", now, on ? '+' : '-', note, vel); }
static void ticks(int m, int n) { for (int i = 0; i < n; i++, now++) gr_tick(m); }

static void glue_test(void)
{
    gr_tick(1);                                             /* creates the bank: stopped */
    nlog[0] = 0; ticks(1, 400); assert(!strchr(nlog, '+'));   /* off by default: no notes */
    gr_press(1, 3); assert(gr_b[1].playing);                /* red press: play */
    gr_set *s = &gr_b[1].g.s;
    s->x = 0; s->y = 0; s->chaos = 0; s->density[0] = 255; s->density[1] = 0; s->density[2] = 0;   /* BD only */
    gr_restart(1); nlog[0] = 0; now = 0;
    ticks(1, 13);                                           /* step 0 (BD, level 143: accent? no) at tick 0, off 12 ticks later */
    if (!strstr(nlog, "0+53/80 12-53/0 ")) { fprintf(stderr, "log: %s\n", nlog); assert(0); }   /* (after the setup tick's note off) */
    uint32_t hits[3], acc[3]; gr_bar(&gr_b[1].g, hits, acc);
    int want = 0; for (int t = 0; t < 32; t += 2) want += hits[0] >> t & 1;   /* 1/16: even steps */
    nlog[0] = 0; now = 0; gr_restart(1); ticks(1, 384);
    int got = 0; for (char *p = nlog; (p = strchr(p, '+')); p++) got++;
    assert(got == want);                                    /* one bar: the even steps' hits */
    gr_b[1].swing = 42; nlog[0] = 0; now = 0; gr_restart(1);
    gr_b[1].g.s.density[0] = 255; ticks(1, 48);             /* step 2 (the second 16th) 8 ticks late, if it hits */
    if (hits[0] >> 2 & 1) assert(strstr(nlog, "32+53/"));
    gr_b[1].swing = 0;
    { extern int gr_host_oct; gr_host_oct = 1;                   /* drum octave +1: the pad's note-off is its key */
      gr_b[1].g.s.density[0] = 255; nlog[0] = 0; now = 0; gr_restart(1); ticks(1, 13); gr_host_oct = 0;
      if (!strstr(nlog, "0+65/80 12-53/0 ")) { fprintf(stderr, "drum octave log: %s\n", nlog); assert(0); } }
    /* synth: held keys give the notes, lowest -> BD */
    gr_tick(0); gr_press(0, 3); gr_set *z = &gr_b[0].g.s; z->x = 0; z->y = 0; z->density[0] = 255; z->density[1] = 0; z->density[2] = 0;
    { extern int gr_host_on_page; gr_host_on_page = 0;          /* off the grids page: keys pass through, not recorded */
      assert(gr_key_down(0, 7) == 0 && gr_key_down(0, 3) == 0); gr_key_up(0, 3); gr_key_up(0, 7);
      gr_host_on_page = 1; }
    { extern int gr_host_oct; gr_host_oct = 0;                   /* key held across an octave change: released by key */
      assert(gr_key_down(0, 5) == 1); gr_host_oct = 1; gr_key_up(0, 5); gr_host_oct = 0;
      extern int gr_host_held_n(void); assert(gr_host_held_n() == 0); }
    assert(gr_key_down(0, 7) == 1 && gr_key_down(0, 3) == 1);   /* midi 60, 56: BD plays 56 */
    nlog[0] = 0; now = 0; gr_restart(0); ticks(0, 2);
    if (!strstr(nlog, "0+56/")) { fprintf(stderr, "synth log: %s\n", nlog); assert(0); }
    gr_key_up(0, 3); gr_key_up(0, 7);
    gr_press(0, 3); assert(!gr_b[0].playing);               /* red press: stop, notes off */
    nlog[0] = 0; ticks(0, 400); assert(!strchr(nlog, '+'));
    gr_press(0, 3);
    gr_ui_init(0); gr_ui_init(1);
    fprintf(stderr, "grids glue ok\n");
}

int main(void)
{
    glue_test();
    gr_b[0].playing = gr_b[1].playing = 1;                  /* the frames show a running pattern */
    play(1, 11); frame("drum mode, defaults (centre of the map, fill 50%), step 10", 1, 0);
    gr_ui_turn(1, 0, -32, 0); gr_ui_turn(1, 1, -32, 0);
    for (int i = 0; i < 3; i++) { gr_ui_turn(1, 2, 12, 0); gr_ui_press(1, 2); }
    play(1, 21); frame("corner x 0 / y 0, fills 69%, step 20", 1, 0);
    gr_ui_turn(1, 0, 40, 0); gr_ui_turn(1, 1, 20, 0); gr_ui_turn(1, 3, 30, 0); gr_ui_press(1, 2);
    gr_b[1].res32 = 1; play(1, 33); frame("x 63% y 31%, chaos 47%, 1/32 steps, sd selected", 1, 0);
    frame("Shift: swing, resolution, pad, accent", 1, 1);
    gr_ui_press(0, 3); gr_ui_turn(0, 2, 30, 0); play(0, 5); frame("synth mode, stopped, bd fill 97%", 0, 0);
    frame("synth mode, Shift (bd note c2)", 0, 1);
    return 0;
}
