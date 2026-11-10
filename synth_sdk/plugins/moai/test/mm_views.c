/* Host screen preview of one view: the real engine, UI and screen code with mocked firmware calls; prints the last
 * frame as JSON (node transforms + raster lines), as harness/screen_harness.c, for preview.py.
 *   mm_views <view 0..8> <k0> <k1> <k2> <k3> <w5> <w6> <w7> [note 0/1] [frames] [osc3 off 0/1] [highlights a,b,c,d | -]
 *            [sync | shift | -] [id:value,... session parameters, as the views set them]
 * MM_TURN=k.. (environment): knobs k.. (digits 0..3) turned just before the last frame (what they change lights up)
 * sync: the decay on 1/8 (as a fast turn would put it); shift: Shift held (home: the second layer)
 * views: 0 home, 1 advanced, 2..4 osc 1..3, 5 filter, 6 contour, 7 analog, 8 noise row */
#include "../../../op1_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24] __attribute__((aligned(4)));
int16_t host_voice_vel[6]; int16_t host_accel[3]; int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];
int host_shift; int host_set_knobs_calls;
void host_set_knobs(int slot, const int16_t *w) { (void)slot; (void)w; host_set_knobs_calls++; }
#define MAXN 16
static char node_id[MAXN][16]; static int32_t node_m[MAXN][9]; static int nnodes; static uint8_t node_hid[MAXN];
#define MAXL 6000
static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } lines[MAXL]; static int nlines; static int raster_obj;
void *host_find_node(void *s, const char *id)
{
    (void)s;
    for (int i = 0; i < nnodes; i++) if (!strcmp(node_id[i], id)) return (void *)(intptr_t)(i + 1);
    snprintf(node_id[nnodes], 16, "%s", id);
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    memcpy(node_m[nnodes], ident, sizeof ident);
    return (void *)(intptr_t)(++nnodes);
}
void host_node_set_transform(void *n, const int32_t *m) { memcpy(node_m[(intptr_t)n - 1], m, 36); }
void host_node_set_visible(void *n, int on) { node_hid[(intptr_t)n - 1] = !on; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; nlines = 0; }
void *host_raster(void) { return &raster_obj; }
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c)
{
    const op1_pt *a = p0, *b = p1; (void)r;
    if (nlines == MAXL) return;
    lines[nlines].x0 = a->x; lines[nlines].y0 = a->y; lines[nlines].x1 = b->x; lines[nlines].y1 = b->y;
    lines[nlines].w = w; memcpy(lines[nlines].c, c, 4); nlines++;
}
void *read_fx_param_safe(int slot, int pos) { (void)slot; (void)pos; return 0; }
#define MM_HOST_UI
#include "../moai.c"
#include "../moai_ui.c"

int main(int argc, char **argv)
{
    if (argc < 9) return 2;
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    int view = atoi(argv[1]);
    int16_t *w = host_knobs[0];
    for (int i = 0; i < 4; i++) w[i] = (int16_t)atoi(argv[2 + i]);
    w[4] = 0x5301; for (int i = 0; i < 3; i++) w[5 + i] = (int16_t)atoi(argv[6 + i]);
    int note = argc > 9 ? atoi(argv[9]) : 0, frames = argc > 10 ? atoi(argv[10]) : 4;
    if (argc > 12 && argv[12][0] != '-') sscanf(argv[12], "%hhd,%hhd,%hhd,%hhd", &ui_hl[0], &ui_hl[1], &ui_hl[2], &ui_hl[3]);
    int sync = argc > 13 && !strcmp(argv[13], "sync");
    host_shift = argc > 13 && !strcmp(argv[13], "shift");
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = 25480370;
    host_voice_vel[0] = 100 << 8;
    int16_t out[OP1_BLOCK];
    mg_screen_init(0);
    mg_screen_update(0, 16);                                     /* first frame: home, derived values */
    if (view) set_view(view);
    if (argc > 14) for (const char *a = argv[14]; *a;) {
        int id, val, n;
        if (sscanf(a, "%d:%d%n", &id, &val, &n) != 2) break;
        if (id >= 0 && id < P_N) P[id] = (int16_t)pclamp(id, val);
        a += n; if (*a == ',') a++;
    }
    if (sync) { ui_sync[1] = 4; ui_contour_seen = P[P_CONTOUR]; }
    if (note) ENGINE_moai.note_on(host_voice_mem, w);
    for (int f = 0; f < frames; f++) {
        if (note) for (int b = 0; b < 6; b++) ENGINE_moai.render(host_voice_mem, out, w);
        if (argc > 11 && atoi(argv[11])) { mm_osc3_off = 1; mm_osc3_why = 1; }
        if (f == frames - 1 && getenv("MM_TURN")) for (const char *t = getenv("MM_TURN"); *t; t++) ui_idle[(*t - '0') & 3] = 3;
        mg_screen_update(0, 16);
    }
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
        printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", lines[i].x0, lines[i].y0, lines[i].x1, lines[i].y1,
               lines[i].w, lines[i].c[0], lines[i].c[1], lines[i].c[2], lines[i].c[3]);
    printf("]}\n");
    return 0;
}
