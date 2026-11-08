/* Host test + preview frames for screens/fxroute.c (master FX source, docs/54): capture / route math, values, saved.
 * "render": JSON frames for harness/render_usbout.py */
#include "../op1_synth.h"
#include "../screens/op1r_settings.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../screens/fxroute.c"
int16_t tr_out[4][64];

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
    nl = nt = 0; fr_update(0, 1);
    if (!render) return;
    printf("{\"label\": \"%s\", \"lines\": [", label);
    for (int i = 0; i < nl; i++) printf("%s[%d,%d,%d,%d,%d,[%d,%d,%d,%d]]", i ? "," : "", L[i].x0, L[i].y0, L[i].x1, L[i].y1, L[i].w, L[i].c[0], L[i].c[1], L[i].c[2], L[i].c[3]);
    printf("], \"text\": [");
    for (int i = 0; i < nt; i++) printf("%s[%d,%d,[%d,%d,%d],\"%s\"]", i ? "," : "", drawn[i]->x, drawn[i]->y, drawn[i]->c[0], drawn[i]->c[1], drawn[i]->c[2], drawn[i]->s);
    printf("]}\n");
}

static void fx_gain2(void *s, int16_t *l, int16_t *r, const int16_t *p) { (void)s; (void)p; for (int i = 0; i < 64; i++) { l[i] *= 2; r[i] *= 2; } }

void fx_tail(void *s, int16_t *l, int16_t *r, const int16_t *p) { (void)p; memcpy(l, s, 128); memcpy(r, s, 128); }

int main(int argc, char **argv)
{
    int render = argc > 1 && !strcmp(argv[1], "render");
    fr_init(0); assert(fr_src == 0);
    frame("master (default)", render);
    int16_t l[64], r[64];
    for (int i = 0; i < 64; i++) { l[i] = 100; r[i] = 50; }
    fr_route(fx_gain2, 0, l, r, 0); assert(l[0] == 200 && r[0] == 100);          /* master: whole mix */
    fxroute_turn(0, 1024); fxroute_flush(); assert(fr_src == 1 && op1r_setting_get("mfxsrc", -1) == 1);
    frame("blue right: instrument", render);
    for (int i = 0; i < 64; i++) { fr_host_tape[0][i] = 1000; fr_host_tape[1][i] = -300; }   /* tape alone */
    fr_snap();
    for (int i = 0; i < 64; i++) { fr_host_tape[0][i] += 40; fr_host_tape[1][i] += 10; }      /* + instrument */
    fr_capture();
    for (int i = 0; i < 64; i++) { l[i] = fr_host_tape[0][i]; r[i] = fr_host_tape[1][i]; }
    fr_route(fx_gain2, 0, l, r, 0); assert(l[5] == 1080 && r[5] == -280);         /* only the instrument doubled */
    fr_route(fx_gain2, 0, l, r, 0); assert(l[5] == 2160);                          /* no fresh capture: whole mix */
    fxroute_turn(1, 1024); fxroute_flush(); assert(fr_out == 1 && op1r_setting_get("mfxsrc", -1) == 3);
    frame("green right: mono tape", render);
    int16_t bus0[64];
    for (int blk = 0; blk < 40; blk++) {                                           /* gain estimate settles */
        for (int i = 0; i < 64; i++) { fr_host_bus[i] = (int16_t)(i * 100 - 3000); fr_host_tape[0][i] = 500; fr_host_tape[1][i] = 500; }
        fr_snap();
        for (int i = 0; i < 64; i++) { fr_host_tape[0][i] += fr_host_bus[i] / 2; fr_host_tape[1][i] += fr_host_bus[i] / 4; }   /* gains 0.5 / 0.25 */
        fr_capture();
        for (int i = 0; i < 64; i++) { l[i] = fr_host_tape[0][i]; r[i] = fr_host_tape[1][i]; }
        memcpy(bus0, fr_host_bus, sizeof bus0);
        fr_route(fx_gain2, 0, l, r, 0);
    }
    fr_host_meter[0] = 100; fr_host_peak = 50;                                    /* dry peak already added */
    for (int i = 0; i < 64; i++) { fr_host_bus[i] = bus0[i]; fr_host_tape[0][i] = 500; fr_host_tape[1][i] = 500; }
    fr_snap();
    for (int i = 0; i < 64; i++) { fr_host_tape[0][i] += fr_host_bus[i] / 2; fr_host_tape[1][i] += fr_host_bus[i] / 4; }
    fr_capture();
    for (int i = 0; i < 64; i++) { l[i] = fr_host_tape[0][i]; r[i] = fr_host_tape[1][i]; }
    fr_route(fx_gain2, 0, l, r, 0);
    assert(fr_host_peak == 6599 && fr_host_meter[0] == 100 - 50 + 6599);         /* wet peak 2 * 3300 x 0x7FFF */
    for (int i = 0; i < 64; i++) {
        assert(fr_host_bus[i] == 2 * bus0[i]);                                      /* recorded: wet mono */
        int el = 500 + bus0[i], er = 500 + bus0[i] / 2;                             /* monitor: wet x track gain */
        assert(abs(l[i] - el) <= 2 && abs(r[i] - er) <= 2);
    }
    /* bus silent (FX tail): the gain is kept, not dropped to 0 */
    static int16_t tail[64]; for (int i = 0; i < 64; i++) tail[i] = 1000;
    for (int i = 0; i < 64; i++) { fr_host_bus[i] = 0; fr_host_tape[0][i] = fr_host_tape[1][i] = 0; }
    fr_snap(); fr_capture();
    memset(l, 0, sizeof l); memset(r, 0, sizeof r);
    void fx_tail(void *, int16_t *, int16_t *, const int16_t *);
    fr_route(fx_tail, tail, l, r, 0);
    assert(fr_host_bus[0] == 1000 && abs(l[0] - 500) <= 2 && abs(r[0] - 250) <= 2);
    fxroute_turn(0, -1024); fxroute_turn(1, -1024); assert(fr_out == 1);            /* green: instrument only */
    fxroute_turn(0, 1024); fxroute_turn(1, -1024); fxroute_flush(); assert(fr_out == 0 && op1r_setting_get("mfxsrc", -1) == 1);
    fxroute_turn(0, 1024); assert(fr_src == 2); fxroute_turn(0, 1024); assert(fr_src == 2);   /* clamped at aux */
    fxroute_turn(0, -1024); assert(fr_src == 1);
    fxroute_turn(1, -4096); assert(fr_src == 1);
    fxroute_turn(0, -1024); fxroute_flush(); assert(fr_src == 0 && op1r_setting_get("mfxsrc", -1) == 0);
    op1r_setting_put("mfxsrc", 1); fr_init(0); assert(fr_src == 1);                            /* loaded */
    /* aux: post-fader sends from the track outs */
    fr_src = 0; fr_out = 0;
    fxroute_turn(0, 1024); fxroute_turn(0, 1024); fxroute_flush(); assert(fr_src == 2 && op1r_setting_get("mfxsrc", -1) == 4 && !fr_aux_live);
    frame("blue: aux", render);
    for (int i = 0; i < 64; i++) { tr_out[0][i] = 1000; tr_out[1][i] = 2000; tr_out[2][i] = 0; tr_out[3][i] = 0; l[i] = r[i] = 0; }
    for (int i = 0; i < 64; i++) l[i] = r[i] = 100;                               /* the mix: tape + live */
    fr_route(fx_gain2, 0, l, r, 0); assert(l[0] == 100 && r[0] == 100);            /* every send at 0: nothing sent, the mix stays dry */
    for (int i = 0; i < 64; i++) l[i] = r[i] = 0;
    for (int i = 0; i < 40; i++) fr_send_turn(0, 1024);                            /* track 1 send 40 */
    assert(fr_send[0] == 40 && fr_aux_live);
    fr_send_turn(1, 1024 * 200); assert(fr_send[1] == 100);                        /* clamped */
    {   /* sidechain sources (op1_sc_source, docs/61) */
        int16_t o[64];
        sc_want = 0; uint8_t fx_was = fr_src; fr_src = 0;
        tr_at = (uint8_t)(sc_blk + 1); fr_snap();                       /* track outs fresh for this block */
        for (int i = 0; i < 64; i++) { tr_out[0][i] = 1000; tr_out[1][i] = 2000; tr_out[2][i] = 30000; tr_out[3][i] = 30000; fr_host_input[i] = (int16_t)i; fr_host_bus[i] = -5; }
        assert(op1_sc_source(OP1_SC_T2, o) == 1 && o[7] == 2000 && sc_want == 32);
        assert(op1_sc_source(OP1_SC_TAPE, o) == 1 && o[0] == 32767);   /* sum saturates */
        assert(op1_sc_source(OP1_SC_IN, o) == 1 && o[9] == 9);
        assert(op1_sc_source(OP1_SC_INST, o) == 1 && o[3] == -5);
        assert(op1_sc_source(0, o) == 0 && o[0] == 0 && op1_sc_source(99, o) == 0);
        assert(fr_tracks_needed());                                     /* asked: the track work runs */
        fr_snap();                                                      /* next block, no fresh track outs */
        assert(op1_sc_source(OP1_SC_T1, o) == 0 && o[5] == 0);
        for (int b = 0; b < 40; b++) fr_snap();
        assert(!fr_tracks_needed() || fr_aux_live);                     /* not asked for 32 blocks: stops */
        fr_src = fx_was;
    }
    { extern uint8_t fr_host_mfx; fr_host_mfx = 0; assert(!fr_tracks_needed()); fr_host_mfx = 1; assert(fr_tracks_needed()); }   /* master FX off: no track work */
    fr_send_turn(1, -1024 * 50); assert(fr_send[1] == 50);
    fr_host_mute[1] = 0;                                                           /* track 2 muted: no send */
    fr_route(fx_gain2, 0, l, r, 0);                                                /* aux = 1000 * 0.4 = ~400, +aux */
    assert(abs(l[0] - 400) <= 3 && abs(r[0] - 400) <= 3);
    fr_save_sends(); fr_send[0] = 0; op1r_setting_put("mfxsrc", 4); fr_init(0); assert(fr_send[0] == 40 && fr_send[1] == 50);
    fr_host_mute[1] = 0x7FFF;
    fxroute_turn(1, 1024); fxroute_flush(); assert(fr_out == 1 && op1r_setting_get("mfxsrc", -1) == 5);             /* aux, mono tape */
    fr_host_track = 1;                                                             /* recording track 2: its send muted */
    for (int i = 0; i < 64; i++) { fr_host_bus[i] = 0; l[i] = r[i] = 0; }
    fr_route(fx_gain2, 0, l, r, 0);
    assert(abs(fr_host_bus[0] - 800) <= 4);                                        /* FX(aux) = 2 x 400 recorded */
    assert(abs(l[0] - 800) <= 8 && abs(r[0] - 800) <= 8);                          /* monitor at track 2's gains */
    if (!render) printf("ok\n");
    return 0;
}
