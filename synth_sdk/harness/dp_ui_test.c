/* drum+ pages on the host: edits / press cycles / labels (asserts), and "render" frames (JSON lines + texts) for
 * harness/render_dpui.py.  cc -O1 -DOP1_HOST -I.. dp_ui_test.c ../drumplus/dp_plus.c ../drumplus/dp_voice.c -lm */
#include "../drumplus/dp_ui.c"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } L[4096]; static int nl;
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c)
{ (void)r; const op1_pt *a = p0, *b = p1; if (nl < 4096) { L[nl].x0 = a->x; L[nl].y0 = a->y; L[nl].x1 = b->x; L[nl].y1 = b->y; L[nl].w = w; memcpy(L[nl].c, c, 4); nl++; } }
void *host_raster(void) { return (void *)1; }
typedef struct { int32_t x, y; uint8_t c[4]; char s[32]; } htxt;
static htxt tstore[32]; static void *tkeys[32]; static int ntk;
static htxt *T(void *o) { for (int i = 0; i < ntk; i++) if (tkeys[i] == o) return &tstore[i]; tkeys[ntk] = o; return &tstore[ntk++]; }
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t sz, const uint8_t *c)
{ (void)sz; htxt *t = T(o); t->x = x; t->y = y; memcpy(t->c, c, 4); t->s[0] = 0; return o; }
void host_text_set(void *o, const char *s) { snprintf(T(o)->s, 32, "%s", s); }
static htxt *drawn[32]; static int nt;
void host_text_draw(void *o, void *r, const int32_t *m) { (void)r; (void)m; drawn[nt++] = T(o); }

static int16_t data[200000];
static void frame(int page, int shift, const dp_extra *x, const int16_t *pw, dpu_wave *w)
{
    nl = nt = 0;
    dpu_draw((void *)1, page, shift, 7, x, pw, w, 22050);
    printf("{\"lines\": [");
    for (int i = 0; i < nl; i++)
        printf("%s[%d,%d,%d,%d,%d,[%d,%d,%d,255]]", i ? "," : "", L[i].x0, L[i].y0, L[i].x1, L[i].y1, L[i].w, L[i].c[0], L[i].c[1], L[i].c[2]);
    printf("], \"text\": [");
    for (int i = 0; i < nt; i++)
        printf("%s{\"s\": \"%s\", \"x\": %d, \"y\": %d, \"c\": [%d,%d,%d]}", i ? "," : "", drawn[i]->s, drawn[i]->x, drawn[i]->y, drawn[i]->c[0], drawn[i]->c[1], drawn[i]->c[2]);
    printf("]}\n");
}

int main(int argc, char **argv)
{
    dp_extra x; memset(&x, 0, sizeof x);
    int16_t pw[8] = { 3 << 9, 0, 0, 8192, 8192, 0, 0, 26000 };
    char a[16], b[16], c[16];
    /* turns: fields, clamping, auto-on */
    assert(dpu_turn_target(0, 0, 0) == -1 && dpu_turn_target(0, 3, 1) == -1);       /* pitch, volume: stock */
    assert(dpu_turn(&x, 1, 3, 0, 40) && x.cutoff == 40 && x.filter == 1);           /* cutoff turns the filter on */
    assert(dpu_turn(&x, 1, 1, 0, 30) && x.decay == 30 && x.env_on == 1);
    assert(dpu_turn(&x, 0, 2, 0, 1) && x.retrig == 2);                              /* off -> 2 */
    assert(dpu_turn(&x, 0, 2, 0, -1) && x.retrig == 0);                             /* 2 -> off */
    assert(dpu_turn(&x, 0, 1, 0, 99) && x.pitch_env == 48);
    assert(!dpu_turn(&x, 0, 1, 0, 5));                                              /* at the limit: no change */
    /* presses */
    assert(dpu_press(&x, pw, 0, 0) == 2 && pw[4] == 16384);                         /* reverse on */
    assert(dpu_press(&x, pw, 0, 1) == 2 && pw[3] == 16384);                         /* one-shot -> loop */
    assert(dpu_press(&x, pw, 0, 1) == 2 && pw[3] == 4096);                          /* -> gate (stock minimum, not 0: error 12) */
    assert(dpu_press(&x, pw, 0, 1) == 2 && pw[3] == 8192);                          /* gate -> one-shot */
    for (int i = 0; i < 5; i++) dpu_press(&x, pw, 0, 2);
    assert(x.choke == 0);
    assert(dpu_press(&x, pw, 1, 3) == 1 && x.vel_start == 40);
    dpu_press(&x, pw, 1, 3); dpu_press(&x, pw, 1, 3); assert(x.vel_start == 127);
    dpu_press(&x, pw, 1, 3); assert(x.vel_start == 0);
    dpu_texts(&x, pw, 1, 0, 3, a, b, c); assert(!strcmp(a, "cutoff"));
    dpu_texts(&x, pw, 0, 0, 0, a, b, c); assert(!strcmp(b, "+3") && !strcmp(c, "rev"));
    dpu_texts(&x, pw, 0, 1, 3, a, b, c); assert(!strcmp(a, "volume") && !strcmp(b, "79"));
    dpu_texts(&x, pw, 1, 0, 1, a, b, c); assert(!strcmp(c, "lin"));
    {   /* a shared kit's pad words: start / end word -32768 is sample -529200 (dp_bound >> 11); stay in the buffer (ASan) */
        int16_t *big = calloc(DPU_N, sizeof *big);
        static const int32_t bad[][2] = { { -529200, 529199 }, { -1, 5000 }, { 400000, 600000 }, { -529200, -1000 } };
        for (int i = 0; i < 4; i++) {
            dpu_wave w; memset(&w, 0, sizeof w); w.key0 = w.key1 = -1;
            dpu_wave_update(&w, big, bad[i][0], bad[i][1]);
            assert(w.key0 >= 0 && w.key1 <= DPU_N && w.len <= DPU_N);
        }
        free(big);
    }
    if (argc < 2 || strcmp(argv[1], "render")) { printf("OK: drum+ pages (edits, presses, labels)\n"); return 0; }
    /* render: a decaying hit in the region, the mock-up's values */
    for (int i = 0; i < 200000; i++) data[i] = (int16_t)(20000 * exp(-i / 9000.0) * sin(i * 0.05) * (0.6 + 0.4 * sin(i * 0.0011)));
    dpu_wave w; memset(&w, 0, sizeof w); w.key0 = w.key1 = -1;
    dpu_wave_update(&w, data, 1000, 41000);
    memset(&x, 0, sizeof x);
    x.pitch_env = 12; x.pitch_time = 60; x.retrig = 4; x.retrig_div = 2; x.choke = 1; x.mono = 1; x.drive = 40; x.fine = 12;
    x.env_on = 1; x.attack = 20; x.hold = 0; x.decay = 75; x.release = 50; x.curve_exp = 1; x.filter = 1; x.cutoff = 80;
    x.resonance = 60; x.vel_pitch = 2; x.human = 20; x.vel_cutoff = 30; x.vel_start = 40;
    int16_t pw2[8] = { 3 << 9, 0, 0, 16384, 16384, 0, 0, 26000 };
    frame(0, 0, &x, pw2, &w); frame(0, 1, &x, pw2, &w); frame(1, 0, &x, pw2, &w); frame(1, 1, &x, pw2, &w);
    return 0;
}
