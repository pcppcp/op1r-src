/* Host harness for engine / FX screens: runs the real DSP + screen code against mocked firmware
 * calls and prints the last frame as JSON (node transforms + raster lines) for render.py.
 *
 *   cc -DOP1_HOST -DENGINE=ENGINE_fmbell -DSCREEN=SCREEN_fmbell -I.. screen_harness.c ../plugins/fmbell/fmbell.c -lm
 *   cc -DOP1_HOST -DFX=FX_tape -DSCREEN=SCREEN_tape -I.. screen_harness.c ../plugins/tape/tape.c -lm
 *   ./a.out k0 k1 k2 k3 [note=1] [frames=10] [released_after=-1] [pages_bitmask w5 w6 w7]
 * FX: "note" = input bursts (a short decaying tone every `note` frames, 0 = silence), released_after =
 * frame from which the input stays silent.
 */
#include "../op1_synth.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t  host_sine[8192];
int32_t  host_pitch_inc[128];
uint8_t  host_voice_mem[6 * 0x24];
int16_t  host_voice_vel[6]; int16_t host_voice_env[128]; int16_t host_accel[3] = { 0, 0, 16384 };
int16_t  host_knobs[8][8];
int      host_slot;
const uint8_t *host_sample_buffer[8];
int16_t  host_fx_knobs[3][8];
int16_t  host_fx_meter[3];
void    *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4];
volatile uint8_t op1_eng_page[4];

#define MAXN 512
static char node_id[MAXN][32]; static int32_t node_m[MAXN][9]; static uint8_t node_hid[MAXN]; static int nnodes;
#define MAXL 4096
static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } lines[MAXL]; static int nlines;
static int raster_obj;
/* stock-font text objects: position, size, colour, string; the frame's drawn ones go to the JSON (render.py draws
 * them in a stand-in font) */
typedef struct { int32_t x, y, size; uint8_t c[4]; char s[32]; } htext_t;
static const htext_t *texts[64]; static int ntexts;
void *host_text_ctor(void *obj, int32_t x_q10, int32_t y, int32_t size, const uint8_t *rgba)
{ htext_t *t = obj; t->x = x_q10; t->y = y; t->size = size; memcpy(t->c, rgba, 4); t->s[0] = 0; return obj; }
void host_text_set(void *t, const char *s) { snprintf(((htext_t *)t)->s, 32, "%s", s); }
void host_text_draw(void *t, void *r, const int32_t *m9) { (void)r; (void)m9; if (ntexts < 64) texts[ntexts++] = t; }

void *host_find_node(void *scr, const char *id)
{
    (void)scr;
    for (int i = 0; i < nnodes; i++) if (!strcmp(node_id[i], id)) return (void *)(intptr_t)(i + 1);
    if (nnodes == MAXN) { fprintf(stderr, "more than %d nodes\n", MAXN); exit(1); }
    snprintf(node_id[nnodes], 32, "%s", id);
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    memcpy(node_m[nnodes], ident, sizeof ident);
    return (void *)(intptr_t)(++nnodes);
}
void host_node_set_transform(void *node, const int32_t *m9) { memcpy(node_m[(intptr_t)node - 1], m9, 36); }
void host_node_set_visible(void *node, int on) { node_hid[(intptr_t)node - 1] = !on; }
#define MAXS 128
static char style_id[MAXS][16]; static uint8_t style_c[MAXS][4]; static int nstyles;   /* recoloured nodes: render.py */
uint8_t *host_node_style_rgba(void *scr, const char *id)
{
    (void)scr;
    for (int i = 0; i < nstyles; i++) if (!strcmp(style_id[i], id)) return style_c[i];
    if (nstyles == MAXS) { fprintf(stderr, "more than %d recoloured nodes\n", MAXS); exit(1); }
    snprintf(style_id[nstyles], 16, "%s", id); memset(style_c[nstyles], 0xFF, 4);
    return style_c[nstyles++];
}
void host_screen_draw(void *scr, int dt) { (void)scr; (void)dt; nlines = 0; ntexts = 0; }   /* SVG drawn by render.py */
void *host_raster(void) { return &raster_obj; }
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c)
{
    const op1_pt *a = p0, *b = p1; (void)r;
    if (nlines == MAXL) return;
    lines[nlines].x0 = a->x; lines[nlines].y0 = a->y; lines[nlines].x1 = b->x; lines[nlines].y1 = b->y;
    lines[nlines].w = w; memcpy(lines[nlines].c, c, 4); nlines++;
}

#ifdef FX
extern const op1_fx_t FX;
#else
extern const op1_engine_t ENGINE;
#endif
extern const op1_screen_t SCREEN;

int main(int argc, char **argv)
{
    for (int i = 0; i < 8192; i++)                          /* firmware layout: table[k] = sin(2 pi k/8192) */
        host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = (int32_t)(440.0 / 44100 * 4294967296.0);
    for (int i = 0; i < 4; i++) host_knobs[0][i] = (int16_t)(argc > 1 + i ? atoi(argv[1 + i]) : 16384);
    int note = argc > 5 ? atoi(argv[5]) : 1, frames = argc > 6 ? atoi(argv[6]) : 10;
    int rel = argc > 7 ? atoi(argv[7]) : -1;
    host_knobs[0][4] = OP1_ENGINE_MAGIC + 1;
#ifndef FX
    {   /* synthetic recorded sample for sampler-based engines (granular): 3 s of decaying tones */
        static int16_t data[8 + 3 * 44100 + 256];   /* + a peak bucket: engines read up to the bucket end, as on the device */
        static uint8_t sb[sizeof(void *) + 2 * OP1_PEAK_BUCKETS];
        int16_t *d = data + 8, *pk = (int16_t *)(sb + sizeof(void *));
        for (int i = 0; i < 3 * 44100; i++) {
            int t = i % 22050;
            d[i] = (int16_t)(host_sine[(i * 41) & 8191] * (22050 - t) / 22050 * 3 / 4);
        }
        for (int k = 0; k < OP1_PEAK_BUCKETS; k++) {
            int m = 0;
            for (int i = k * 256; i < (k + 1) * 256 && i < 3 * 44100; i++) m = abs(d[i]) > m ? abs(d[i]) : m;
            pk[k] = (int16_t)m;
        }
        *(int16_t **)sb = data;
        host_sample_buffer[0] = sb;
    }
#endif
#ifdef FX
    static int32_t scratch[2][44115];
    for (int p = 0; p < 2; p++) host_fx_scratch[p] = scratch[p];
    for (int i = 0; i < 5; i++) host_fx_knobs[0][i] = host_knobs[0][i];
    for (int i = 0; i < 3; i++) if (argc > 9 + i) host_fx_knobs[0][5 + i] = (int16_t)atoi(argv[9 + i]);
    FX.init(0);
    if (argc > 8) for (int i = 0; i < 4; i++) op1_fx_page[0][i] = (atoi(argv[8]) >> i) & 1;   /* page 2 / latch: pressed after
                                                                                              the FX was created */
    SCREEN.init(0);
    int16_t buf[OP1_FX_BLOCK]; uint32_t ph = 0, t = 0;
    for (int f = 0; f < frames; f++) {
        for (int b = 0; b < 17; b++) {                      /* ~1100 samples = 1/40 s per frame */
            for (int n = 0; n < OP1_FX_BLOCK; n++, t++) {
                int32_t a = 0;
                if (note > 0 && (rel < 0 || f < rel)) {
                    uint32_t age = t % (uint32_t)(note * 1102);         /* burst every `note` frames */
                    if (age < 4000) a = 20000 - (int32_t)age * 5;
                }
                ph += 0x05000000u;
                buf[n] = (int16_t)((a * host_sine[ph >> 19]) >> 15);
            }
            FX.render(0, buf, host_fx_knobs[0]);
        }
        SCREEN.update(0, 25);
    }
#else
    if (argc > 8) for (int i = 0; i < 4; i++) op1_eng_page[i] = (atoi(argv[8]) >> i) & 1;   /* page 2 */
    for (int i = 0; i < 3; i++) if (argc > 9 + i) host_knobs[0][5 + i] = (int16_t)atoi(argv[9 + i]);
    int16_t out[OP1_BLOCK];
    void *v0 = OP1_VOICE(0);
    *(uint32_t *)v0 = 0xdeadbeef;                           /* firmware vtable word */
    for (int i = 0; i < 128; i++) host_voice_env[i] = 24000;   /* stock envelope: sustaining */
    if (note) ENGINE.note_on(v0, host_knobs[0]);
    if (getenv("HOST_ACCEL")) sscanf(getenv("HOST_ACCEL"), "%hd,%hd,%hd", &host_accel[0], &host_accel[1], &host_accel[2]);   /* after note-on: tilted from home */
    SCREEN.init(0);
    for (int f = 0; f < frames; f++) {
        if (f == rel && ENGINE.note_off) ENGINE.note_off(v0);
        if (note) for (int b = 0; b < 6; b++) ENGINE.render(v0, out, host_knobs[0]);   /* ~60 fps */
        SCREEN.update(0, 16);
    }
    if (*(uint32_t *)v0 != 0xdeadbeef) { fprintf(stderr, "engine overwrote the voice vtable word\n"); return 1; }
#endif

    printf("{\"nodes\": {");
    for (int i = 0; i < nnodes; i++) {
        printf("%s\"%s\": [", i ? ", " : "", node_id[i]);
        for (int j = 0; j < 9; j++) printf("%s%d", j ? ", " : "", node_m[i][j]);
        printf("]");
    }
    printf("}, \"hidden\": [");
    for (int i = 0, n = 0; i < nnodes; i++) if (node_hid[i]) printf("%s\"%s\"", n++ ? ", " : "", node_id[i]);
    printf("], \"lines\": [");
    for (int i = 0; i < nlines; i++)
        printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", lines[i].x0, lines[i].y0,
               lines[i].x1, lines[i].y1, lines[i].w, lines[i].c[0], lines[i].c[1], lines[i].c[2], lines[i].c[3]);
    printf("], \"styles\": {");
    for (int i = 0; i < nstyles; i++)
        printf("%s\"%s\": [%d, %d, %d]", i ? ", " : "", style_id[i], style_c[i][0], style_c[i][1], style_c[i][2]);
    printf("}, \"texts\": [");
    for (int i = 0; i < ntexts; i++)
        printf("%s[%d, %d, %d, [%d, %d, %d], \"%s\"]", i ? ", " : "", texts[i]->x, texts[i]->y, texts[i]->size,
               texts[i]->c[0], texts[i]->c[1], texts[i]->c[2], texts[i]->s);
    printf("]}\n");
    return 0;
}
