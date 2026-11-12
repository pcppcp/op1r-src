/* Host test kit for one plugin: the <name>_test.c that `pluginkit.py new` writes includes this (once, from the test
 * only) and `pluginkit.py check` builds it with the plugin's sources, -DOP1_HOST and ASan / UBSan.
 * It defines the firmware mocks (as audio_harness.c / screen_harness.c), records what a screen or overlay draws, prints
 * it as a JSON frame (pluginkit.py draws it with render.py's SVG code), and has small helpers to run a synth voice and
 * an FX and to count failed checks. main() is the test's own: end it with `return pt_done();`. */
#ifndef PLUGIN_TEST_H
#define PLUGIN_TEST_H
#include "op1_synth.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_voice_env[128]; int16_t host_accel[3] = { 0, 0, 16384 };
int16_t host_knobs[8][8]; int host_slot; int host_shift; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];
void *read_fx_param_safe(int slot, int pos) { (void)slot; (void)pos; return 0; }
int op1_sc_source(int src, int16_t *out) { (void)src; memset(out, 0, 64 * sizeof *out); return 0; }
int op1_inst_fade(int out) { (void)out; return 0; }
const op1_tool_t *pt_overlay;                                   /* the last op1_overlay() call */
void op1_overlay(const op1_tool_t *t) { pt_overlay = t; }

/* sequencer notes (the seq template's note output on the host): count, last pitch and velocity, notes held now */
int pt_seq_on[2], pt_seq_off[2], pt_seq_last[2], pt_seq_vel[2], pt_seq_held[2];
void host_seq_note_on(int mode, int note, int vel) { mode &= 1; pt_seq_on[mode]++; pt_seq_last[mode] = note; pt_seq_vel[mode] = vel; pt_seq_held[mode]++; }
void host_seq_note_off(int mode, int note) { (void)note; mode &= 1; pt_seq_off[mode]++; pt_seq_held[mode]--; }

/* ---- checks ---- */
static int pt_fails;
#define CHECK(c, ...) do { if (!(c)) { pt_fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static inline int pt_done(void) { printf(pt_fails ? "%d check(s) failed\n" : "ok\n", pt_fails); return pt_fails != 0; }

/* ---- what a screen / overlay draws (one frame: op1_screen_draw or pt_frame_start clears it) ---- */
#define PT_NODES 256
static char pt_node_id[PT_NODES][32]; static int32_t pt_node_m[PT_NODES][9]; static uint8_t pt_node_hid[PT_NODES];
static int pt_nnodes;
#define PT_LINES 4096
static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } pt_lines[PT_LINES];
static int pt_nlines, pt_raster_obj, pt_dimmed;
typedef struct { int32_t x, y, size; uint8_t c[4]; char s[32]; } pt_text_t;
static const pt_text_t *pt_texts[64]; static int pt_ntexts;
static char pt_style_id[64][16]; static uint8_t pt_style_c[64][4]; static int pt_nstyles;

void *host_find_node(void *scr, const char *id)
{
    (void)scr;
    for (int i = 0; i < pt_nnodes; i++) if (!strcmp(pt_node_id[i], id)) return (void *)(intptr_t)(i + 1);
    if (pt_nnodes == PT_NODES) return 0;
    snprintf(pt_node_id[pt_nnodes], 32, "%s", id);
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    memcpy(pt_node_m[pt_nnodes], ident, sizeof ident);
    return (void *)(intptr_t)(++pt_nnodes);
}
void host_node_set_transform(void *node, const int32_t *m9) { memcpy(pt_node_m[(intptr_t)node - 1], m9, 36); }
void host_node_set_visible(void *node, int on) { pt_node_hid[(intptr_t)node - 1] = !on; }
uint8_t *host_node_style_rgba(void *scr, const char *id)
{
    (void)scr;
    for (int i = 0; i < pt_nstyles; i++) if (!strcmp(pt_style_id[i], id)) return pt_style_c[i];
    if (pt_nstyles == 64) { static uint8_t spare[4]; return spare; }
    snprintf(pt_style_id[pt_nstyles], 16, "%s", id); memset(pt_style_c[pt_nstyles], 0xFF, 4);
    return pt_style_c[pt_nstyles++];
}
static inline void pt_frame_start(void) { pt_nlines = 0; pt_ntexts = 0; pt_dimmed = 0; }
void host_screen_draw(void *scr, int dt) { (void)scr; (void)dt; pt_frame_start(); }   /* the SVG: pluginkit.py */
void *host_raster(void) { return &pt_raster_obj; }
void host_raster_dim(void *r) { (void)r; pt_dimmed = 1; }
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c)
{
    const op1_pt *a = p0, *b = p1; (void)r;
    if (pt_nlines == PT_LINES) return;
    pt_lines[pt_nlines].x0 = a->x; pt_lines[pt_nlines].y0 = a->y; pt_lines[pt_nlines].x1 = b->x; pt_lines[pt_nlines].y1 = b->y;
    pt_lines[pt_nlines].w = w; memcpy(pt_lines[pt_nlines].c, c, 4); pt_nlines++;
}
void *host_text_ctor(void *obj, int32_t x_q10, int32_t y, int32_t size, const uint8_t *rgba)
{ pt_text_t *t = obj; t->x = x_q10; t->y = y; t->size = size; memcpy(t->c, rgba, 4); t->s[0] = 0; return obj; }
void host_text_set(void *t, const char *s) { snprintf(((pt_text_t *)t)->s, 32, "%s", s); }
void host_text_draw(void *t, void *r, const int32_t *m9) { (void)r; (void)m9; if (pt_ntexts < 64) pt_texts[pt_ntexts++] = t; }

/* lines inside the 320 x 160 screen (Q10): a quick check that a screen draws where it can be seen */
static inline int pt_lines_on_screen(void)
{
    for (int i = 0; i < pt_nlines; i++)
        if (pt_lines[i].x0 < 0 || pt_lines[i].x1 < 0 || pt_lines[i].y0 < 0 || pt_lines[i].y1 < 0 ||
            pt_lines[i].x0 > 320 << 10 || pt_lines[i].x1 > 320 << 10 || pt_lines[i].y0 > 160 << 10 || pt_lines[i].y1 > 160 << 10)
            return 0;
    return 1;
}

/* the current frame as one JSON line (the format of screen_harness.c, plus "label"); pluginkit.py check runs the test
 * with the argument "render" and turns these lines into a PNG */
static inline void pt_frame(const char *label)
{
    printf("{\"label\": \"%s\", \"dim\": %d, \"nodes\": {", label, pt_dimmed);
    for (int i = 0; i < pt_nnodes; i++) {
        printf("%s\"%s\": [", i ? ", " : "", pt_node_id[i]);
        for (int j = 0; j < 9; j++) printf("%s%d", j ? ", " : "", pt_node_m[i][j]);
        printf("]");
    }
    printf("}, \"hidden\": [");
    for (int i = 0, n = 0; i < pt_nnodes; i++) if (pt_node_hid[i]) printf("%s\"%s\"", n++ ? ", " : "", pt_node_id[i]);
    printf("], \"lines\": [");
    for (int i = 0; i < pt_nlines; i++)
        printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", pt_lines[i].x0, pt_lines[i].y0, pt_lines[i].x1,
               pt_lines[i].y1, pt_lines[i].w, pt_lines[i].c[0], pt_lines[i].c[1], pt_lines[i].c[2], pt_lines[i].c[3]);
    printf("], \"styles\": {");
    for (int i = 0; i < pt_nstyles; i++)
        printf("%s\"%s\": [%d, %d, %d]", i ? ", " : "", pt_style_id[i], pt_style_c[i][0], pt_style_c[i][1], pt_style_c[i][2]);
    printf("}, \"texts\": [");
    for (int i = 0; i < pt_ntexts; i++)
        printf("%s[%d, %d, %d, [%d, %d, %d], \"%s\"]", i ? ", " : "", pt_texts[i]->x, pt_texts[i]->y, pt_texts[i]->size,
               pt_texts[i]->c[0], pt_texts[i]->c[1], pt_texts[i]->c[2], pt_texts[i]->s);
    printf("]}\n");
}

/* ---- audio ---- */
static inline int32_t pt_inc(int midi, int rate) { return (int32_t)lrint(4294967296.0 * 440.0 * pow(2, (midi - 69) / 12.0) / rate); }
static int32_t pt_fx_scratch[3][OP1_FX_SCRATCH_BYTES / 4];
static inline void pt_init(void)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    for (int i = 0; i < 3; i++) host_fx_scratch[i] = pt_fx_scratch[i];
    for (int i = 0; i < 128; i++) host_voice_env[i] = 24000;   /* stock envelope: sustaining */
}
/* note_on on voice 0, `blocks` render calls (689 per second), note_off before the last quarter; -> peak |sample|.
 * Fails the test on a write to the firmware's word of the voice object. */
static inline int pt_synth_note(const op1_engine_t *e, int midi, int vel, const int16_t *knobs, int blocks)
{
    int16_t out[OP1_BLOCK]; int peak = 0;
    void *v = OP1_VOICE(0);
    *(uint32_t *)v = 0xdeadbeef;
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = pt_inc(midi, OP1_VOICE_RATE);
    host_voice_vel[0] = (int16_t)(vel << 8);
    e->note_on(v, knobs);
    for (int b = 0; b < blocks; b++) {
        if (b == blocks * 3 / 4 && e->note_off) e->note_off(v);
        e->render(v, out, knobs);
        for (int i = 0; i < OP1_BLOCK; i++) if (abs(out[i]) > peak) peak = abs(out[i]);
    }
    CHECK(*(uint32_t *)v == 0xdeadbeef, "the engine wrote the voice's first word (it belongs to the firmware)");
    return peak;
}
/* FX at `pos`: `blocks` blocks of 64 (689 per second) of a saw at `level` (0 = silence); -> peak |sample| */
static inline int pt_fx_run(const op1_fx_t *f, int pos, const int16_t *knobs, int blocks, int level)
{
    int16_t buf[OP1_FX_BLOCK]; int peak = 0; uint32_t ph = 0;
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < OP1_FX_BLOCK; i++) { ph += (uint32_t)pt_inc(48, OP1_SAMPLE_RATE); buf[i] = (int16_t)(((int32_t)ph >> 16) * level >> 15); }
        f->render(pos, buf, knobs);
        for (int i = 0; i < OP1_FX_BLOCK; i++) if (abs(buf[i]) > peak) peak = abs(buf[i]);
    }
    return peak;
}
#endif
