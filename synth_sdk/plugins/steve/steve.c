/* "steve" - Steve Reich's phase process as an FX (docs/51): capture a loop, play it as 2..4 copies that drift apart
 * (It's Gonna Rain, Come Out) or move one unit at a time and lock (Piano Phase). DSP in steve/st_core.c.
 *   blue  : drift -2 .. +2 % (centre detent = unison) | steps: unit = loop / 4, 6, 8, 12, 16
 *           page 2: transition, 0 = drift mode, else steps moving at 0.1 .. 5 %
 *   green : loop length, free 10 ms .. the buffer (lower 3/5), synced 1/4 beat .. 2 bars (upper 2/5)
 *           page 2: loops held on each unit (steps)
 *   white : copies 2..4; press: capture (synced lengths start on the next beat), Shift + press: back to unison
 *   red   : dry / wet; press: freeze (captures blocked)
 * Memory: synth / drum FX in the position's scratch (2 s), the master a static 8 s buffer (heap 0, at load).
 * Master: stereo (render_stereo): A left, B right, C / D between; the dry stays stereo. */
#include "op1_plugin.h"
#include "../../steve/st_core.h"

#define MASTER_SAMPLES (8 * 44100 + 1024)
static int16_t master_buf[MASTER_SAMPLES];
static st_t st[3];
static uint8_t cap_seen[3];                                 /* white latch bit last seen: a change = a press */
static const int32_t PCT = 42949673;                        /* 1 % in Q32 */
static const uint8_t divs[5] = { 4, 6, 8, 12, 16 };
static const int32_t trans_q[8] = { 4294967, 8589935, 17179869, 34359738, 64424509, 107374182, 150323855, 214748365 };
                                                            /* 0.1 0.2 0.4 0.8 1.5 2.5 3.5 5 % (Q32) */
#ifdef OP1_HOST
static uint8_t st_shift;
#define SHIFT st_shift
#else
#define SHIFT (*(volatile uint8_t *)0x01DD7F78)
#endif

static int32_t beat_samples(void)                           /* the OP-1 tempo's beat, samples (none yet: 120 BPM) */
{
    op1_beat_t b;
    op1_beat_clock(&b);
    return b.len > 0 ? b.len >> 14 : 22050;
}
/* green: length in samples (cap: the buffer's), *sync = 1 in the synced part */
static int32_t st_length(int32_t v, int32_t cap, int *sync)
{
    static const uint8_t q8[6] = { 1, 2, 4, 8, 16, 32 };    /* in quarter beats: 1/4 beat .. 8 beats */
    if (v < 19660) {                                        /* free: 10 ms .. 10 s (capped), doubling per tenth */
        *sync = 0;
        int32_t oct = v * 10 / 19660, frac = v * 10 % 19660 * 256 / 19660;   /* per capture / frame: divisions fine */
        int32_t lo = 441 << oct, l = lo + (lo * frac >> 8);
        return l > cap ? cap : l;
    }
    *sync = 1;
    int32_t l = beat_samples() * q8[(v - 19660) * 6 / 13108 > 5 ? 5 : (v - 19660) * 6 / 13108] / 4;
    return l > cap ? cap : l;
}
static void st_params(int pos, const int16_t *p, st_par *q)
{
    int32_t v0 = op1_fx_param_q15(p, 0), v1 = op1_fx_param_q15(p, 1), w5 = p[5] < 0 ? 0 : p[5], w6 = p[6] < 0 ? 0 : p[6];
    q->trans = w5 < 1024 ? 0 : trans_q[(w5 - 1024) * 8 / 31744 > 7 ? 7 : (w5 - 1024) * 8 / 31744];
    int32_t d = v0 - 16384;
    d = d > 512 ? d - 512 : d < -512 ? d + 512 : 0;         /* centre detent: unison */
    q->drift = d * (2 * PCT / 15872);
    q->div = divs[v0 * 5 >> 15];
    q->hold = 1 + (w6 * 16 >> 15);
    q->copies = 2 + (op1_fx_param_q15(p, 2) * 3 >> 15);
    q->mix = op1_fx_param_q15(p, 3);
    (void)pos; (void)v1;
}
static void st_control(int pos, const int16_t *p)           /* the white press: capture / unison */
{
    uint8_t c = op1_fx_page[pos][2];
    if (c == cap_seen[pos]) return;
    cap_seen[pos] = c;
    if (SHIFT) { st_unison(&st[pos]); return; }
    if (op1_fx_page[pos][3]) return;                        /* frozen */
    int sync;
    int32_t len = st_length(op1_fx_param_q15(p, 1), st[pos].cap - ST_XF - 2, &sync);
    int32_t delay = 0;
    if (sync) { op1_beat_t b; op1_beat_clock(&b); delay = b.len > 0 ? b.next >> 14 : 0; }
    st_capture(&st[pos], len, delay);
    op1_dbg("steve capture: pos, len", pos, len);
}

static void sv_init(int pos)
{
    pos %= 3;
    if (pos == OP1_FX_MASTER) st_init(&st[pos], master_buf, MASTER_SAMPLES);
    else st_init(&st[pos], (int16_t *)op1_fx_scratch(pos), OP1_FX_SCRATCH_BYTES / 2);
    cap_seen[pos] = op1_fx_page[pos][2];                    /* the latch as it is: no capture at creation */
}
static void sv_render(int pos, int16_t *buf, const int16_t *p)
{
    pos %= 3;
    st_par q;
    st_params(pos, p, &q);
    st_control(pos, p);
    st_render(&st[pos], &q, buf, 0, buf, 0, OP1_FX_BLOCK);
}
static void sv_render_stereo(int pos, int16_t *l, int16_t *r, const int16_t *p)
{
    pos %= 3;
    st_par q;
    st_params(pos, p, &q);
    st_control(pos, p);
    st_render(&st[pos], &q, l, r, l, r, OP1_FX_BLOCK);
}

const op1_fx_t FX_steve = { sv_init, sv_render, { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_LATCH, OP1_PRESS_LATCH },
                            sv_render_stereo };

/* ======================= screen (GUI thread) =======================
 * steve.svg is empty. Drawn here: one rabbit per copy, each on its lane, hopping from left to right at its copy's
 * playhead (3 frames, the hop follows the distance run: a faster copy hops ahead); unit ticks on the ground in steps
 * mode; while capturing A runs along in red. Texts in the stock font: state, offset, loops; bottom row as granular. */
#ifndef OP1_HOST
#pragma GCC optimize ("Os")
#endif
#include "op1_synth.h"
#define Q(v)  ((v) << 10)
#define TS    12
#define X0      10                                           /* rabbits: left end of the run, px */
#define X1      290                                          /* right end (the rabbit's nose ends at X1 + RW) */
#define RW      22                                           /* rabbit width */
#define HOP     7                                            /* px run per animation frame */
#define LANE_Y0 36                                           /* ground of lane 0 (A); lanes 16 px apart */
#define LANE_DY 17

static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     c_grey[4] = { 0x6F, 0x71, 0x73, 0xFF }, c_lilac[4] = { 0xDF, 0xD9, 0xFF, 0xFF },
                     c_dim[4] = { 0x3A, 0x3B, 0x3D, 0xFF };
static const uint8_t *const col4[4] = { c_blue, c_green, c_white, c_red };
static const uint8_t *const copy_col[ST_COPIES] = { c_blue, c_white, c_green, c_lilac };
enum { TX_LAB, TX_VAL = 4, TX_PRS = 8, TX_OFS = 10, TX_LOOPS, TX_STATE, TX_N };
static uint32_t tx[TX_N][OP1_TEXT_SIZE / 4];
static char shown[TX_N][14];
static uint8_t tx_made;

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    if (x0 == x1 && y0 == y1) return;                       /* the OP-1 drops zero-length lines */
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
static void txt(void *r, int i, const char *s)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    if (!s || !s[0]) return;
    int same = 1;
    for (int j = 0; j < 14; j++) { if (shown[i][j] != s[j]) { same = 0; break; } if (!s[j]) break; }
    if (!same) { op1_text_set(tx[i], s); int j = 0; for (; j < 13 && s[j]; j++) shown[i][j] = s[j]; shown[i][j] = 0; }
    op1_text_draw(tx[i], r, ident);
}
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *num(char *p, int32_t v, int dec)               /* v in 1/10^dec */
{
    char t[12]; int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v || n <= dec);
    while (n) { *p++ = t[--n]; if (n == dec && dec) *p++ = '.'; }
    *p = 0;
    return p;
}
static void len_text(char *s, int32_t v, int32_t cap)
{
    int sync;
    int32_t l = st_length(v, cap, &sync);
    if (sync) {
        static const char *const n[6] = { "1/4 beat", "1/2 beat", "1 beat", "2 beats", "1 bar", "2 bars" };
        int i = (v - 19660) * 6 / 13108;
        put(s, n[i > 5 ? 5 : i]);
    } else {
        int32_t ms = (l * 10 + 220) / 441;
        if (ms < 1000) put(num(s, ms, 0), "ms"); else put(num(s, ms / 10, 2), "s");
    }
}
/* the rabbit, facing right, 3 frames (crouch, leap, land): polylines in px relative to its tail on the ground,
 * y up negative; -128 ends a polyline, -127 the frame */
static const int8_t rabbit_pts[3][64] = {
    { 2, 0, 0, -3, 3, -7, 7, -9, 11, -8, 13, -7, -128, 13, -7, 14, -10, 17, -11, 19, -9, 18, -7, 15, -6, -128, 14, -10, 13, -16, -128, 16, -11, 17, -16, -128, 15, -6, 15, 0, 17, 0, -128, 2, 0, 12, 0, -128, 0, -3, -2, -4, -1, -6, -128, -127 },   /* crouch: sitting */
    { 0, -6, 4, -10, 10, -12, 15, -11, -128, 15, -11, 17, -14, 20, -14, 21, -12, 19, -10, 16, -10, -128, 17, -14, 12, -18, -128, 18, -14, 14, -19, -128, 16, -10, 21, -6, -128, 1, -6, -4, -2, -128, 1, -6, 15, -10, -128, 0, -6, -2, -7, -1, -9, -128, -127 },   /* leap: stretched, off the ground */
    { 1, -8, 5, -11, 10, -10, 14, -6, -128, 14, -6, 16, -9, 19, -9, 20, -7, 18, -5, 15, -4, -128, 16, -9, 15, -15, -128, 17, -9, 19, -15, -128, 15, -4, 16, 0, 18, 0, -128, 2, -7, 0, -4, -128, 2, -7, 14, -4, -128, 1, -8, -1, -9, 0, -11, -128, -127 },   /* land: front paws down */ };
static void rabbit(void *r, int32_t x, int32_t y, int f, const uint8_t *c)
{
    const int8_t *p = rabbit_pts[f];
    while (*p != -127) {
        while (p[2] != -128) { ln(r, x + p[0] * 1024, y + p[1] * 1024, x + p[2] * 1024, y + p[3] * 1024, 0x600, c); p += 2; }
        p += 3;
    }
}

static void sv_screen_init(void *scr) { (void)scr; }

static void sv_screen_update(void *scr, int dt)
{
    int pos = op1_fx_screen_pos() % 3;
    const st_t *s = &st[pos];
    int16_t wsh[8]; const int16_t *w = op1_knobs_shown(1, pos, op1_fx_knobs(pos), wsh);   /* page 2 as modulated */
    int16_t k[4];
    op1_fx_knobs_live(k);
    st_par q;
    st_params(pos, w, &q);
    if (!tx_made) {
        for (int i = 0; i < 4; i++) {
            op1_text_ctor(tx[TX_LAB + i], Q(8 + 78 * i), 114, TS, col4[i]);
            op1_text_ctor(tx[TX_VAL + i], Q(8 + 78 * i), 127, TS, c_white);
        }
        op1_text_ctor(tx[TX_PRS], Q(8 + 78 * 2), 94, TS, c_white);
        op1_text_ctor(tx[TX_PRS + 1], Q(8 + 78 * 3), 94, TS, c_red);
        op1_text_ctor(tx[TX_STATE], Q(X0), 4, TS, c_grey);
        op1_text_ctor(tx[TX_OFS], Q(200), 4, TS, c_red);
        op1_text_ctor(tx[TX_LOOPS], Q(262), 4, TS, c_grey);
        tx_made = 1;
    }
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    int copies = q.copies, steps = q.trans > 0, page[3];
    for (int i = 0; i < 3; i++) page[i] = op1_fx_page[pos][i] != 0;
    int32_t cap = s->cap - ST_XF - 2;

    /* the rabbits: one per copy, hopping left to right along its lane at the copy's playhead */
    for (int c = 0; c < copies; c++) {
        int32_t gy = LANE_Y0 + LANE_DY * c;
        ln(r, Q(X0), Q(gy + 1), Q(X1 + RW), Q(gy + 1), 0x300, c_dim);         /* the ground */
        if (steps && s->len)                                                   /* the units */
            for (int i = 0; i <= q.div; i++) {
                int32_t x = Q(X0 + RW / 2) + Q(X1 - X0) * i / q.div;
                ln(r, x, Q(gy + 1), x, Q(gy + 4), 0x300, c_grey);
            }
        int32_t x, f = 0;
        const uint8_t *col = copy_col[c];
        if (s->state == ST_PLAY) {
            x = Q(X0) + (int32_t)((int64_t)Q(X1 - X0) * s->p[c].i / s->len);
            f = (x >> 10) / HOP % 3;                                           /* the hop follows the distance run */
        } else if (s->state == ST_CAPTURE && c == 0) {                         /* capturing: A runs along, red */
            x = Q(X0) + (int32_t)((int64_t)Q(X1 - X0) * s->wp / (s->want + ST_XF + 1));
            f = (x >> 10) / HOP % 3; col = c_red;
        } else { x = Q(X0); col = s->state == ST_ARMED ? c_red : c_grey; }    /* sitting at the start */
        rabbit(r, x, Q(gy), f, col);
    }

    /* offset and loops */
    char v[16];
    if (s->len && s->state == ST_PLAY) {
        put(num(v, (int32_t)((uint64_t)st_offset(s, 1) * 1000 / (uint32_t)s->len), 1), "%");
        txt(r, TX_OFS, v);
        put(num(put(v, "x"), (int32_t)s->loops, 0), "");
        txt(r, TX_LOOPS, v);
    }
    txt(r, TX_STATE, s->state == ST_ARMED ? "wait for beat" : s->state == ST_CAPTURE ? "capturing"
                     : s->state == ST_EMPTY ? "no loop" : steps ? (s->held[1] >= 0 ? "locked" : "moving")
                     : q.drift ? "drifting" : "unison");

    /* the bottom row */
    for (int i = 0; i < 4; i++) ln(r, Q(8 + 78 * i), Q(110), Q(78 + 78 * i), Q(110), 0x800, col4[i]);
    if (page[0]) { txt(r, TX_LAB, "trans"); if (q.trans) { int32_t t = (int32_t)(((int64_t)q.trans * 10 + PCT / 2) / PCT); put(num(v, t, 1), "%"); } else put(v, "drift"); }
    else if (steps) { txt(r, TX_LAB, "unit"); put(num(put(v, "1/"), q.div, 0), ""); }
    else { txt(r, TX_LAB, "drift"); int64_t d = (int64_t)q.drift * 100; int32_t t = (int32_t)((d + (d < 0 ? -PCT / 2 : PCT / 2)) / PCT); put(num(t > 0 ? put(v, "+") : v, t, 2), "%"); }
    txt(r, TX_VAL, v);
    if (page[1]) { txt(r, TX_LAB + 1, "hold"); num(v, q.hold, 0); }
    else { txt(r, TX_LAB + 1, "len"); len_text(v, op1_fx_param_q15(w, 1), cap); }
    txt(r, TX_VAL + 1, v);
    txt(r, TX_LAB + 2, "copies"); num(v, q.copies, 0); txt(r, TX_VAL + 2, v);
    txt(r, TX_LAB + 3, "mix"); put(num(v, (q.mix * 100 + 16383) >> 15, 0), "%"); txt(r, TX_VAL + 3, v);
    txt(r, TX_PRS, "capture");
    if (op1_fx_page[pos][3]) txt(r, TX_PRS + 1, "freeze");
    (void)k;
}

const op1_screen_t SCREEN_steve = { sv_screen_init, sv_screen_update };
