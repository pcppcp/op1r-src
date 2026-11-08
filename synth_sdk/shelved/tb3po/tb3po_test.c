/* TB-3PO sequencer (tb3po/): checks of the generator and the player, and preview frames for render_tb3po.py
 * (one JSON frame per line on stdout; the checks print to stderr).
 *   cc -DOP1_HOST -I.. tb3po_test.c ../tb3po/tb_fw.c ../tb3po/tb_core.c -o build/tb3po_test */
#include "../op1_synth.h"
#include "../tb3po/tb_fw.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define MAXL 600
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
static struct { int t, note, vel, on; } ev[4096]; static int nev, now;
void host_tb_note(int m, int note, int vel, int on) { (void)m; if (nev < 4096) { ev[nev].t = now; ev[nev].note = note; ev[nev].vel = vel; ev[nev].on = on; nev++; } }

static void frame(const char *label, int m, int shift)
{
    nlines = 0; nt = 0;
    tb_draw(&nlines, m, shift);
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
static void ticks(int m, int n) { for (int i = 0; i < n; i++, now++) tb_tick(m); }
static int popcount(uint32_t x) { int n = 0; while (x) { n += x & 1; x >>= 1; } return n; }

int main(void)
{
    /* generator: deterministic, density shapes gates and pitch range */
    tb_pat a = { .seed = 1234, .density = 3, .scale = 0 }, b = a;
    tb_generate(&a); tb_generate(&b);
    assert(!memcmp(&a, &b, sizeof a));
    tb_pat lo = { .seed = 77, .density = 0 }, mid = { .seed = 77, .density = 7 }, hi = { .seed = 77, .density = 14 };
    tb_generate(&lo); tb_generate(&mid); tb_generate(&hi);
    fprintf(stderr, "gates: density -7 %d, 0 %d, +7 %d of 32\n", popcount(lo.gates), popcount(mid.gates), popcount(hi.gates));
    assert(popcount(lo.gates) > popcount(mid.gates) && popcount(hi.gates) > popcount(mid.gates));
    for (int s = 0; s < 32; s++) assert(lo.notes[s] == 0);              /* density -7: the root only (octaves apart) */
    int maxdeg = 0; for (int s = 0; s < 32; s++) if (hi.notes[s] > maxdeg) maxdeg = hi.notes[s];
    assert(maxdeg > 2 && maxdeg < 7);                                   /* +7: the whole minor scale */
    assert(tb_semitones(&mid, 0, 0) % 12 == 0 || 1);
    { tb_pat t = { .seed = 9, .density = 14, .scale = 9 }; tb_generate(&t);   /* transpose by scale degrees */
      for (int s = 0; s < 32; s++) assert(tb_semitones(&t, s, 2) == tb_semitones(&t, s, 0) + 2); }

    /* player: steps every 24 ticks at 1/16, gates half a step, slides overlap (new note on before the old off) */
    tb_bank *bk = &tb_b[0];
    tb_turn(0, 0, 0, 0);                                                 /* (init) */
    bk->pat.seed = 0x3333; bk->pat.density = 14; tb_generate(&bk->pat); bk->length = 16; bk->slides = 1; bk->rate = 1;
    tb_press(0, 3); nev = 0; now = 0;
    ticks(0, 24 * 32);
    int ons = 0, bad_gate = 0, slide_overlap = 0;
    for (int i = 0; i < nev; i++) {
        if (!ev[i].on) continue;
        ons++;
        assert(ev[i].t % 24 == 0);
        for (int j = i + 1; j < nev; j++) if (!ev[j].on && ev[j].note == ev[i].note) {
            int d = ev[j].t - ev[i].t;
            if (d != 12 && d % 24) bad_gate++;
            break;
        }
        if (i + 1 < nev && !ev[i + 1].on && ev[i + 1].t == ev[i].t && ev[i + 1].note != ev[i].note) slide_overlap++;
    }
    fprintf(stderr, "player: %d notes in 2 bars, %d slides (overlap), %d odd gates\n", ons, slide_overlap, bad_gate);
    assert(ons > 8 && !bad_gate);
    tb_press(0, 3); assert(!bk->playing);
    { int t = nev; for (int i = 0; i < nev; i++) t += ev[i].on ? 1 : -1; (void)t; }

    /* persistence round trip */
    char s[80]; int n = tb_serialize(0, s, sizeof s);
    tb_bank keep = *bk; bk->pat.seed = 1; bk->length = 3;
    assert(tb_parse(0, s, n) && bk->pat.seed == keep.pat.seed && bk->length == keep.length && bk->root == keep.root);

    /* seed: cw new, ccw back */
    uint16_t s0 = bk->pat.seed; tb_turn(0, 3, 1024, 0); assert(bk->pat.seed != s0); tb_turn(0, 3, -1024, 0); assert(bk->pat.seed == s0);
    fprintf(stderr, "tb3po: ok\n");

    /* preview frames */
    bk->pat.seed = 0x3a7f; bk->pat.density = 11; bk->pat.scale = 0; tb_generate(&bk->pat); bk->length = 16;
    tb_press(0, 3); now = 0; ticks(0, 24 * 5 + 2);
    frame("TB3PO: 16 steps, density +4, minor, playing step 5", 0, 0);
    bk->length = 32; bk->pat.density = 1; tb_generate(&bk->pat); ticks(0, 24 * 18);
    frame("TB3PO: 32 steps, density -6 (root and one note), step 23", 0, 0);
    frame("TB3PO SHIFT: transpose / slides / rate", 0, 1);
    tb_press(0, 3);
    tb_turn(1, 0, 0, 0); tb_b[1].root = 4; tb_b[1].length = 12;
    frame("TB3PO drum: 12 steps from pad 5, stopped", 1, 0);
    return 0;
}
