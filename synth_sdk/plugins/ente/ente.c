/* ENTE: sidechain compressor FX (docs/61). The detector listens to a selectable source, the gain goes on the FX's own
 * input; on the master (stereo) one linked gain for L and R.
 *   blue  threshold (-48 .. 0 dB)          press: attack (0.1 .. 50 ms)
 *   green ratio (1:1 .. 20:1 curved, top = inf)    press: makeup (0 .. +12 dB)
 *   white release (20 ms .. 1 s)            press: sidechain high-pass (off .. 250 Hz)
 *   red   source, a list knob (one entry per click, wraps; OP1_PRESS_LIST): SELF, T1..T4 (tape tracks, pre-fader),
 *         TAPE (their sum), IN (input), INST (master: the live instrument; synth / drum: its own input = SELF),
 *         BEAT 1/1 .. 1/16t (a pulse on the tempo grid, no audio)
 * Sources other than SELF / BEAT come from the core (op1_sc_source). Screen: a rubber duck squeezed by the gain
 * reduction (flat at 24 dB), the source top left. */
#include "op1_plugin.h"

enum { S_SELF, S_T1, S_T2, S_T3, S_T4, S_TAPE, S_IN, S_BEAT, S_INST };   /* T1..IN = OP1_SC_* numbers */
#define NDIV 8
/* the source list (16 entries everywhere); BEAT entries carry their division */
#define NENT 16
static const uint8_t ent_src[NENT] = { S_SELF, S_T1, S_T2, S_T3, S_T4, S_TAPE, S_IN, S_INST,
                                       S_BEAT, S_BEAT, S_BEAT, S_BEAT, S_BEAT, S_BEAT, S_BEAT, S_BEAT };
static const uint8_t div_w[NDIV] = { 4, 2, 1, 1, 1, 2, 1, 1 };   /* window in beats */
static const uint8_t div_k[NDIV] = { 1, 1, 1, 2, 4, 3, 3, 6 };   /* pulses per window */
#define HOLD 88                                            /* BEAT pulse: 2 ms at full level into the detector */

typedef struct {
    int32_t env;                /* detector, |x| << 8 */
    int32_t g;                  /* gain Q14 */
    int32_t lp;                 /* sidechain high-pass state (the low part) */
    int32_t hold;               /* BEAT: samples of pulse left */
    uint32_t beats;             /* BEAT: beats seen (window position); shortcut: 1/1, 1/2, 1/4T count from the FX start,
                                   not from the tape's bar 1; align to c.tape_pos if users want bar-locked pumps */
    volatile int32_t gr;        /* gain reduction of the last block, log2 Q10 (screen) */
    volatile uint8_t trig;      /* BEAT: pulses so far (screen) */
} ente_t;
static ente_t st[3];

static const uint16_t lg_tab[33] = { 0, 45, 90, 132, 174, 214, 254, 292, 330, 366, 402, 436, 470, 504, 536, 568, 599,
    629, 659, 689, 717, 745, 773, 800, 827, 853, 879, 904, 929, 953, 977, 1001, 1024 };            /* log2(1 + i/32) Q10 */
static const uint16_t ex_tab[33] = { 16384, 16743, 17109, 17484, 17867, 18258, 18658, 19066, 19484, 19911, 20347,
    20792, 21247, 21713, 22188, 22674, 23170, 23678, 24196, 24726, 25268, 25821, 26386, 26964, 27554, 28158, 28774,
    29405, 30048, 30706, 31379, 32066, 32768 };                                                    /* 2^(i/32) Q14 */

static int32_t log2_q10(uint32_t x)                        /* x > 0 */
{
    int n = 31 - __builtin_clz(x);
    uint32_t m = n >= 15 ? x >> (n - 15) : x << (15 - n);  /* 32768 .. 65535 */
    int32_t f = (int32_t)m - 32768, i = f >> 10, fr = f & 1023;
    return (n << 10) + lg_tab[i] + (((lg_tab[i + 1] - lg_tab[i]) * fr) >> 10);
}
static int32_t exp2_q14(int32_t v)                         /* 2^(v / 1024) in Q14, v <= 2048 */
{
    int32_t n = v >> 10, f = v & 1023, i = f >> 5, fr = f & 31;
    int32_t m = ex_tab[i] + (((ex_tab[i + 1] - ex_tab[i]) * fr) >> 5);
    return n >= 0 ? m << n : n < -15 ? 0 : m >> -n;
}
static inline int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static inline int32_t word(const int16_t *p, int i) { return p[i] < 0 ? 0 : p[i]; }   /* page-2 words 0..32767 */
static inline int ent_of(int32_t q15) { return op1_fx_list(q15, NENT); }
static inline int div_of(int e) { return e >= 8 ? e - 8 : 0; }

static void ente_init(int pos)
{
    ente_t *s = &st[pos % 3];
    s->env = s->lp = s->hold = 0; s->g = 16384; s->gr = 0; s->beats = 0;
}

/* BEAT: the sample of this block where a pulse of the division falls, or -1 */
static int beat_pulse(ente_t *s, int d)
{
    op1_beat_t c;
    op1_beat_clock(&c);
    if (c.len < (256 << 14) || c.step <= 0 || c.next < 0 || c.next > c.len) return -1;   /* no clock / stands still */
    int64_t len = c.len, win = len * div_w[d], sub = win / div_k[d];
    int64_t e0 = (int64_t)(s->beats % div_w[d]) * len + (len - c.next);   /* time into the window */
    if (e0 < 0) e0 += win;
    int64_t n0 = (e0 + sub - 1) / sub, t = n0 * sub;                       /* the first pulse at or after e0 */
    if (c.next < c.step) s->beats++;                                       /* a beat passes in this block */
    if (t >= e0 + c.step) return -1;
    return (int)((t - e0) * 64 / c.step);
}

static void process(int pos, int16_t *l, int16_t *r, const int16_t *p)
{
    pos %= 3;
    ente_t *s = &st[pos];
    int32_t k0 = op1_fx_param_q15(p, 0), k1 = op1_fx_param_q15(p, 1), k2 = op1_fx_param_q15(p, 2);
    int32_t w5 = word(p, 5), w6 = word(p, 6), w7 = word(p, 7);
    int ent = ent_of(op1_fx_param_q15(p, 3)), src = ent_src[ent];
    if (src == S_INST && pos != OP1_FX_MASTER) src = S_SELF;           /* the instrument FX's input is the instrument */
    /* per block: threshold, slope, makeup (log2 Q10), detector coefficients (Q20 per sample) */
    int32_t thr = 15360 - ((32767 - k0) * 8192 >> 15);                     /* 0 dBFS .. -48 dB */
    int32_t rq8 = 256 + ((((k1 * k1) >> 15) * 19 * 256) >> 15);            /* ratio Q8 1 .. 20, curved (k^2) */
    int32_t slope = k1 >= 32000 ? 1024 : 1024 - (1024 * 256) / rq8;
    int32_t mk = (w6 * 2048) >> 15;                                        /* 0 .. +12 dB */
    int32_t ta = 4 + (((w5 * w5) >> 15) * 2201 >> 15), tr = 882 + (((k2 * k2) >> 15) * 43218 >> 15);
    int32_t ca = (1 << 20) / ta, cr = (1 << 20) / tr;
    int32_t fc = (w7 * 250) >> 15, hc = src == S_BEAT || fc < 20 ? 0 : (fc * 4669) >> 10;   /* 2 pi fc / fs, Q15 */

    int16_t sc[64];
    const int16_t *x = 0;
    int pulse = -1;
    if (src == S_BEAT) { pulse = beat_pulse(s, div_of(ent)); if (pulse >= 0) s->trig++; }
    else if (src != S_SELF) { op1_sc_source(src == S_INST ? OP1_SC_INST : src, sc); x = sc; }

    int32_t env = s->env, g = s->g, lp = s->lp, hold = s->hold, grmax = 0, gt = g, peak = 0;
    for (int i = 0; i < OP1_FX_BLOCK; i++) {
        int32_t v;
        if (src == S_BEAT) { if (i == pulse) hold = HOLD; v = hold > 0 ? (hold--, 32767) : 0; }
        else {
            v = x ? x[i] : r ? (l[i] < 0 ? -l[i] : l[i]) > (r[i] < 0 ? -r[i] : r[i]) ? l[i] : r[i] : l[i];
            if (hc) { lp += ((v - lp) * hc) >> 15; v -= lp; }
            if (v < 0) v = -v;
        }
        int32_t d = (v << 8) - env;
        env += (int32_t)(((int64_t)d * (d > 0 ? ca : cr)) >> 20);
        if (!(i & 7)) {                                                    /* gain computer every 8 samples */
            int32_t over = env > 256 ? log2_q10((uint32_t)env >> 8 | 1) - thr : -1, gr = over > 0 ? (over * slope) >> 10 : 0;
            if (gr > grmax) grmax = gr;
            gt = exp2_q14(mk - gr);
        }
        g += (gt - g) >> 3;
        int32_t a = sat16((l[i] * g) >> 14);
        l[i] = (int16_t)a;
        if (a < 0) a = -a;
        if (r) { int32_t b = sat16((r[i] * g) >> 14); r[i] = (int16_t)b; if (b < 0) b = -b; if (b > a) a = b; }
        if (a > peak) peak = a;
    }
    s->env = env; s->g = g; s->lp = lp; s->hold = hold; s->gr = grmax;
#ifndef OP1_HOST
    volatile int16_t *m = op1_fx_meter(pos);
    int32_t mv = (*m * 0xfe00) >> 16;
    *m = (int16_t)(peak > mv ? peak : mv);
#endif
}

static void ente_render(int pos, int16_t *buf, const int16_t *p) { process(pos, buf, 0, p); }
static void ente_render_stereo(int pos, int16_t *l, int16_t *r, const int16_t *p) { process(pos, l, r, p); }

const op1_fx_t FX_ente = { ente_init, ente_render, { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_LIST(NENT) },
                           ente_render_stereo };


/* ======================= screen (GUI thread) =======================
 * The duck (white outline, red bill) as polylines in duck units (Q4, y up, feet at 0), flattened once from cubic
 * curves; each frame scaled by the squeeze (wider, flatter, feet on the floor). Round eye with a pupil, squinting
 * ('>') with red squeak lines above 60 % squeeze. Right: the gain reduction bar (track in ente.svg). Bottom row in
 * the stock font as dddd / sampler+: per knob a line in its colour, the label, the value (page 2: its own label). */
#include "op1_synth.h"
#include "seq/seq_text.h"
#pragma GCC push_options
#pragma GCC optimize("Os")

static const uint8_t C_WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, C_RED[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     C_GREEN[4] = { 0x00, 0xED, 0x95, 0xFF };
static const uint8_t C_BLUE[4] = { 0x4D, 0x9E, 0xFF, 0xFF };
static const uint8_t *const col4[4] = { C_BLUE, C_GREEN, C_WHITE, C_RED };
static const char *const src_name[S_INST + 1] = { "self", "t1", "t2", "t3", "t4", "tape", "in", "beat", "inst" };
static const char *const div_name[NDIV] = { "1/1", "1/2", "1/4", "1/8", "1/16", "1/4t", "1/8t", "1/16t" };   /* t: the stock font's T is another glyph */
#define Q(v) ((int32_t)(v) * 1024)
#define CX 160
#define FLOOR 104
#define SC 840                                              /* duck scale Q10: fits above the bottom row */
#define D(v) ((int16_t)((v) * 16))                          /* duck units Q4 */

/* path ops: M x y, L x y, C x1 y1 x2 y2 x y (duck units x 16), E = end of polyline, X = end of list; colour per path */
enum { M = 1, L, C, E, X };
static const int16_t duck_src[] = {
    M, D(-30), D(66.1), C, D(-32), D(64), D(-34), D(63), D(-36), D(62), C, D(-50), D(56), D(-62), D(44), D(-60), D(26),
    C, D(-60), D(8), D(-50), D(2), D(-30), D(1), C, D(-10), D(0), D(30), D(0), D(46), D(2),
    C, D(58), D(4), D(62), D(14), D(60), D(30), C, D(59), D(44), D(56), D(62), D(50), D(70),
    C, D(46), D(74), D(40), D(73), D(38), D(66), C, D(34), D(58), D(26), D(56), D(18), D(57),
    C, D(14), D(58), D(10), D(62), D(8.4), D(66.4), E,
    M, D(-39), D(95.5), L, D(-56), D(95), C, D(-62), D(94.5), D(-62), D(88), D(-57), D(87), L, D(-37.3), D(75.3), E,
    M, D(-37.3), D(75.3), L, D(-46), D(75), C, D(-52), D(74), D(-52), D(67), D(-46), D(67), L, D(-35), D(71.5), E, X };
#define HEAD_X (-11)
#define HEAD_Y 88
#define HEAD_R 29
#define NPT 160
static int16_t pts[NPT][2];                                /* flattened: x, y (Q4); pen-up = x == INT16_MIN */
static uint8_t pcol[NPT];                                  /* colour of the segment ending at this point: 0 white 1 red */
static int npt;
static int32_t shown;                                      /* displayed squeeze, Q10 (smoothed) */

static void put(int x, int y, int c) { if (npt < NPT) { pts[npt][0] = (int16_t)x; pts[npt][1] = (int16_t)y; pcol[npt++] = (uint8_t)c; } }
static void arc(int a0, int a1, int c)                     /* head circle, degrees, 12 degree steps */
{
    static const int16_t cs[16] = { 1024, 1002, 935, 828, 685, 512, 316, 107, -107, -316, -512, -685, -828, -935, -1002, -1024 };
    for (int a = a0, first = 1; ; a = a + 12 > a1 && a != a1 ? a1 : a + 12, first = 0) {
        int32_t t = ((a % 360) + 360) % 360, k = t / 12, f = t % 12;      /* cos by table (180 / 12 = 15 steps) */
        int32_t ci = k <= 15 ? cs[k] : cs[30 - k], cn = k + 1 <= 15 ? cs[k + 1] : cs[29 - k];
        int32_t co = ci + (cn - ci) * f / 12;
        int32_t u = (t + 270) % 360, ku = u / 12, fu = u % 12;            /* sin = cos(t - 90) */
        int32_t si0 = ku <= 15 ? cs[ku] : cs[30 - ku], si1 = ku + 1 <= 15 ? cs[ku + 1] : cs[29 - ku];
        int32_t si = si0 + (si1 - si0) * fu / 12;
        if (first) put(INT16_MIN, 0, c);
        put(D(HEAD_X) + (D(HEAD_R) * co >> 10), D(HEAD_Y) + (D(HEAD_R) * si >> 10), c);
        if (a == a1) break;
    }
}
static void flatten(void)
{
    const int16_t *p = duck_src;
    int x = 0, y = 0, path = 0;
    while (*p != X) {
        int op = *p++;
        if (op == M) { x = p[0]; y = p[1]; p += 2; put(INT16_MIN, 0, 0); put(x, y, path > 0); }
        else if (op == L) { x = p[0]; y = p[1]; p += 2; put(x, y, path > 0); }
        else if (op == C) {
            for (int k = 1; k <= 5; k++) {                     /* 5 segments per curve */
                int32_t t = k * 205, u = 1024 - t;                 /* Q10 */
                int32_t b0 = u * u >> 10, b1 = b0 * u >> 10, b2 = 3 * (b0 * t >> 10), b3 = 3 * ((t * t >> 10) * u >> 10), b4 = (t * t >> 10) * t >> 10;
                int32_t px = (b1 * x + b2 * p[0] + b3 * p[2] + b4 * p[4]) >> 10, py = (b1 * y + b2 * p[1] + b3 * p[3] + b4 * p[5]) >> 10;
                put(k == 5 ? p[4] : px, k == 5 ? p[5] : py, path > 0);
            }
            x = p[4]; y = p[5]; p += 6;
        } else if (op == E) path++;
    }
    arc(-48, 165, 0);                                      /* head: open at the bill and the neck */
    arc(206, 229, 0);
}

enum { TX_LAB, TX_VAL = 4, TX_N = 8 };
static uint32_t tx[TX_N][OP1_TEXT_SIZE / 4];
static char tx_shown[TX_N][14];
static uint8_t tx_made;
static void txt(void *r, int i, const char *s)            /* as dddd: set only when changed */
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    if (!tx_made) {
        for (int j = 0; j < 4; j++) {
            op1_text_ctor(tx[TX_LAB + j], Q(8 + 78 * j), 114, 12, col4[j]);
            op1_text_ctor(tx[TX_VAL + j], Q(8 + 78 * j), 127, 12, C_WHITE);
        }
        tx_made = 1;
    }
    int same = 1;
    for (int j = 0; j < 14; j++) { if (tx_shown[i][j] != s[j]) { same = 0; break; } if (!s[j]) break; }
    if (!same) { op1_text_set(tx[i], s); int j = 0; for (; j < 13 && s[j]; j++) tx_shown[i][j] = s[j]; tx_shown[i][j] = 0; }
    op1_text_draw(tx[i], r, ident);
}

static void ente_screen_init(void *scr) { (void)scr; if (!npt) flatten(); }

static void ente_screen_update(void *scr, int dt)
{
    int pos = op1_fx_screen_pos() % 3;
    ente_t *s = &st[pos];
    int16_t wsh[8]; const int16_t *wd = op1_knobs_shown(1, pos, op1_fx_knobs(pos), wsh);   /* page 2 as modulated */
    int ent = ent_of(op1_fx_param_q15(wd, 3)), src = ent_src[ent];
    int32_t target = s->gr * 1024 / 4096;                  /* 24 dB = flat */
    if (target > 1024) target = 1024;
    shown += (target - shown) / 3;
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    if (!npt) flatten();
    int32_t g = shown, sx = 1024 + g * 307 / 1024, sy = 1024 - g / 2;
    sx = sx * SC >> 10; sy = sy * SC >> 10;
#define TX(x) (Q(CX) + ((int32_t)(x) * sx >> 4))
#define TY(y) (Q(FLOOR) - ((int32_t)(y) * sy >> 4))
    for (int i = 1; i < npt; i++) {
        if (pts[i][0] == INT16_MIN || pts[i - 1][0] == INT16_MIN) continue;
        int32_t x0 = TX(pts[i - 1][0]), y0 = TY(pts[i - 1][1]), x1 = TX(pts[i][0]), y1 = TY(pts[i][1]);
        int32_t dx = x1 - x0, dy = y1 - y0, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        int32_t len = (ax > ay ? ax + ay * 3 / 8 : ay + ax * 3 / 8) >> 10;   /* px, approx */
        if (len > 0) { int32_t ex = dx / len, ey = dy / len;            /* 1 px on: butt ends overlap at the joins */
                       x0 -= ex; y0 -= ey; x1 += ex; y1 += ey; }
        sq_line(r, x0, y0, x1, y1, Q(3), pcol[i] ? C_RED : C_WHITE);
    }
    int32_t ex = TX(D(-26)), ey = TY(D(94));
    if (g < 614) {                                         /* round eye (not squashed) with the pupil to the front */
        static const int8_t ring[13][2] = { {5,0},{4,3},{3,4},{0,5},{-3,4},{-4,3},{-5,0},{-4,-3},{-3,-4},{0,-5},{3,-4},{4,-3},{5,0} };
        for (int i = 1; i < 13; i++)
            sq_line(r, ex + Q(ring[i - 1][0]), ey + Q(ring[i - 1][1]), ex + Q(ring[i][0]), ey + Q(ring[i][1]), Q(2), C_WHITE);
        sq_dot(r, ex - Q(1), ey - Q(0.5), Q(3), C_WHITE);
    } else {                                               /* squint '>' and squeak lines at the bill */
        sq_line(r, TX(D(-30)), TY(D(98)), TX(D(-23)), TY(D(94)), Q(3), C_WHITE);
        sq_line(r, TX(D(-23)), TY(D(94)), TX(D(-30)), TY(D(90)), Q(3), C_WHITE);
        int32_t bx = TX(D(-62)), by = TY(D(86));
        sq_line(r, bx - Q(5), by - Q(3), bx - Q(15), by - Q(9), Q(2), C_RED);
        sq_line(r, bx - Q(6), by, bx - Q(18), by, Q(2), C_RED);
        sq_line(r, bx - Q(5), by + Q(3), bx - Q(15), by + Q(9), Q(2), C_RED);
    }
    if (g > 0) sq_line(r, Q(300), Q(10), Q(300), Q(10) + g * 90, Q(6), C_GREEN);   /* gain reduction */

    /* the bottom row */
    int32_t k[4];
    for (int i = 0; i < 4; i++) { k[i] = op1_fx_param_q15(wd, i); sq_line(r, Q(8 + 78 * i), Q(110), Q(78 + 78 * i), Q(110), 0x800, col4[i]); }
    char v[16], *p;
    if (op1_fx_page[pos][0]) {
        int32_t w5 = word(wd, 5), ta = 4 + (((w5 * w5) >> 15) * 2201 >> 15), ms10 = (ta * 100 + 220) / 441;
        p = sq_num(v, (uint32_t)(ms10 / 10), 1); *p++ = '.'; p = sq_num(p, (uint32_t)(ms10 % 10), 1); sq_cat(p, " ms");
        txt(r, TX_LAB, "attack");
    } else {
        int32_t db = 48 - ((k[0] * 48 + 16383) >> 15);
        p = v; if (db) *p++ = '-'; p = sq_num(p, (uint32_t)db, 1); sq_cat(p, " db");
        txt(r, TX_LAB, "thresh");
    }
    txt(r, TX_VAL, v);
    if (op1_fx_page[pos][1]) { p = sq_cat(v, "+"); p = sq_num(p, (uint32_t)((word(wd, 6) * 12 + 16384) >> 15), 1); sq_cat(p, " db"); txt(r, TX_LAB + 1, "makeup"); }
    else {
        int32_t rq8 = 256 + ((((k[1] * k[1]) >> 15) * 19 * 256) >> 15), r10 = (rq8 * 10 + 128) >> 8;   /* as the DSP */
        if (k[1] >= 32000) sq_cat(v, "inf");
        else { p = sq_num(v, (uint32_t)(r10 / 10), 1); *p++ = '.'; p = sq_num(p, (uint32_t)(r10 % 10), 1); sq_cat(p, ":1"); }
        txt(r, TX_LAB + 1, "ratio");
    }
    txt(r, TX_VAL + 1, v);
    if (op1_fx_page[pos][2]) {
        int32_t fc = (word(wd, 7) * 250) >> 15;
        if (fc < 20) sq_cat(v, "off"); else { p = sq_num(v, (uint32_t)fc, 1); sq_cat(p, " hz"); }
        txt(r, TX_LAB + 2, "hipass");
    } else {
        int32_t tr = 882 + (((k[2] * k[2]) >> 15) * 43218 >> 15), ms = (tr * 10 + 220) / 441;
        if (ms < 1000) { p = sq_num(v, (uint32_t)ms, 1); sq_cat(p, " ms"); }
        else { p = sq_num(v, (uint32_t)(ms / 1000), 1); *p++ = '.'; p = sq_num(p, (uint32_t)(ms / 100 % 10), 1); sq_cat(p, " s"); }
        txt(r, TX_LAB + 2, "release");
    }
    txt(r, TX_VAL + 2, v);
    txt(r, TX_LAB + 3, "source");
    if (src == S_BEAT) { p = sq_cat(v, "beat "); sq_cat(p, div_name[div_of(ent)]); txt(r, TX_VAL + 3, v); }
    else txt(r, TX_VAL + 3, src_name[src]);
}
#pragma GCC pop_options

const op1_screen_t SCREEN_ente = { ente_screen_init, ente_screen_update };
