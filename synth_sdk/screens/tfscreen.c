/* Free tape machine screen (docs/82, approved preview display/tapemachine_preview2.py): replaces R246's tape screen
 * while the free machine is on (op1r settings, white: TAPE STOCK / FREE).
 *   top-left: the active track's number in a box (as stock); top: its head as m:ss:ms; top-right: its speed (blue)
 *   and "rev" (red); middle: a window of +-8 s around its own head (fixed in the middle), clips blue, its loop green;
 *   bottom: the four tracks as thin lanes around their own heads, the active one's clips blue, the others grey.
 * Keys (consumed on this screen): T1..T4 select; s1 / s2 / s3 (0x2D / 0x2E / 0x2F) set the active track's loop in /
 * out at its head and toggle it (out also switches it on), as stock does for the global loop. Knobs: white speed
 * (ratios; Shift: fine), red record level (Shift + red: marker jump), Shift + s5 reverse; blue / green R246's. Every other key stays R246's.
 * Text in the stock font (op1_text_*, lowercase); lines with Raster::line. Clips from db2::Clips (0x01AA8D70:
 * 512 x { track, start, stop } in samples, used flags at + 0x1800).
 * Swap like the MIDI sequencer view (seqdev/ms_fw.c): an iter-class screen object (empty usbout.svg) in the tape
 * screen's slot of the screen table; R246's object back when the machine is off. */
#include "../op1_synth.h"
#include "stroke_font.h"

typedef struct { int32_t speed; int32_t lin, lout; uint8_t lon, lset, mute, stop; } tm_track;   /* stop: Shift + STOP / PLAY, per track */
extern volatile tm_track tm_tr[4];
extern volatile uint8_t tm_on;
int32_t tm_head_pos(int t);

#define SCREEN_TABLE   ((void **)0x01DF4E54)
#define CUR_MODE_ID    (*(volatile int *)0xFF907D58)              /* tape = 2 */
#define CUR_SCREEN     (*(volatile int *)0x018B65C8)
#define SCREEN_CTOR    ((void *(*)(void *, int, const char *))0x01A3BAB0)
#define ITER_SCREEN_VT ((const uint32_t *)0x019103FC)
#define SVG_LOAD       ((void *(*)(void *))0x01A503D8)
#define SHIFT          (*(volatile uint8_t *)0x01DD7F78)
#define CLIPS          ((const volatile int32_t *)0x01AA8D70)     /* [i * 3 + 0] track, + 1 start, + 2 stop */
#define CLIP_USED      ((const volatile uint8_t *)0x01AAA570)
#define NCLIPS         512
#define SR             44100
#define REC_LEVEL      (*(volatile int16_t *)0x01B4EF6E)          /* R246's tape record level (stock slider: >> 5, docs/11) */
extern volatile int16_t tm_in_peak;                              /* tape/tm.c: peak of the bus R246 records; (0xFF801088,
                                                                  * R246's meter source, stays 0 with the instrument) */
#define IN_PEAK        tm_in_peak
#define T_MODE         (*(volatile int32_t *)0xFF802960)          /* 0 stop, 1 rec, 2 play, 3 wind */
#define TSPEED_RAW     (*(volatile int32_t *)0xFF802968)          /* transport speed (0: stood still) */
#define UI_STATE       (*(volatile int32_t *)0x018C17D4)          /* 6 = record armed (docs/25) */
void tm_set_head(int t, int32_t pos);
#define SEL_TRACK      (*(volatile int32_t *)0xFF802964)          /* R246's selected track: where its recording goes */
#define TAKE_A         (*(volatile int32_t *)0xFF8029E4)          /* the take being recorded (samples, tape_c range) */
#define TAKE_B         (*(volatile int32_t *)0xFF8029E8)
#define TAPE_VT        0x01911B5C                                 /* R246's TapeScreen vtable (docs/22) */
#define TPOS           (*(volatile int32_t *)0xFF802974)          /* samples << 6 */
#define LOCATE(s)      (((void (*)(int32_t))0x01A55024)(s))       /* tape_c::check_pos (0 .. 0xF2BFA0) */
#define TM_MAXPOS      0xF2BFA0
#define TE_BPM_BITS    (*(volatile uint32_t *)0x018C1878)         /* tempo, float (read as bits) */
static inline uint32_t now(void) { uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c)); return c; }
static int32_t bpm_q8(void)                                      /* as seqdev/ms_fw.c: float bits -> Q8, 120 if odd */
{
    const uint32_t b = TE_BPM_BITS; const int e = (int)((b >> 23) & 0xFF) - 127;
    if ((b >> 31) || e < 5 || e > 7) return 120 << 8;
    return (int32_t)(((b & 0x7FFFFF) | 0x800000) >> (15 - e));
}

static uint32_t tfs_scr[32], tfs_vt[10];
static void *tape_obj;
static int tfs_id = -1;
static uint8_t tfs_in, sel;

static const int32_t ratios[] = { 0x4000, 0x5555, 0x8000, 0xAAAB, 0xC000, 0x10000, 0x15555, 0x18000, 0x20000, 0x30000, 0x40000 };
static const char *const ratio_txt[] = { "1:4", "1:3", "1:2", "2:3", "3:4", "1:1", "4:3", "3:2", "2:1", "3:1", "4:1" };
#define NR ((int)(sizeof ratios / sizeof ratios[0]))

static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     c_grey[4] = { 0x6F, 0x71, 0x73, 0xFF }, c_dim[4] = { 0x40, 0x42, 0x44, 0xFF },
                     c_dgreen[4] = { 0x00, 0x77, 0x4A, 0xFF }, c_dred[4] = { 0x80, 0x1D, 0x2E, 0xFF };

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c) { if (x0 != x1 || y0 != y1) sf_line(r, x0, y0, x1, y1, w, c); }
static char *put(char *q, const char *s) { while (*s) *q++ = *s++; *q = 0; return q; }
static char *put_u(char *q, uint32_t v, int w) { char b[10]; int n = 0; do b[n++] = (char)('0' + v % 10); while ((v /= 10) || n < w); while (n) *q++ = b[--n]; *q = 0; return q; }
static void fmt_speed(char *q, int32_t sp)                       /* "1/2x", "1.37x" */
{
    if (sp < 0) sp = -sp;
    for (int i = 0; i < NR; i++) if (ratios[i] == sp) { put(q, ratio_txt[i]); return; }
    const uint32_t c = (uint32_t)(((int64_t)sp * 100 + 0x8000) >> 16);
    q = put_u(q, c / 100, 1); *q++ = '.'; put_u(q, c % 100, 2);
}

/* stock-font texts: built once, redrawn; set only when the string changes */
enum { T_NUM, T_NUMD, T_TIME, T_SPEED, T_REV, T_DIAG, T_C1, T_C2, T_D0, T_N = T_D0 + 5 };   /* T_TIME: minutes; T_D0..: s s ms ms ms */
static uint32_t tx[T_N][OP1_TEXT_SIZE / 4];
static char shown[T_N][16];
static uint8_t made;
static void txt(void *r, int i, const char *s)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    int same = 1;
    for (int j = 0; j < 16; j++) { if (shown[i][j] != s[j]) { same = 0; break; } if (!s[j]) break; }
    if (!same) { op1_text_set(tx[i], s); int j = 0; for (; j < 15 && s[j]; j++) shown[i][j] = s[j]; shown[i][j] = 0; }
    op1_text_draw(tx[i], r, ident);
}
#define TXT_CTOR(o, x, y, sz, c, a) (((void *(*)(void *, int32_t, int32_t, int32_t, const uint8_t *, int32_t))0x01A61904)( \
                                    (o), (x), (y), (sz), (c), (a)))   /* align: 0 left, 1 centred, 2 right */
static void make(void)
{
    TXT_CTOR(tx[T_NUM], OP1_Q10(13), 8, 12, c_white, 1);       /* as the MIDI sequencer view's track square */
    TXT_CTOR(tx[T_NUMD], OP1_Q10(13), 8, 12, c_grey, 1);        /* the same, a muted track (the colour is the text object's) */
    /* the head m:ss:ms in fixed cells (the stock font is proportional: one text would jitter): minutes right-aligned
     * at 126, each other digit centred in a 13 px cell, the colons in 7 px cells; the whole 114 .. 206, centred at 160 */
    TXT_CTOR(tx[T_TIME], OP1_Q10(126), 4, 20, c_white, 2);
    TXT_CTOR(tx[T_C1], OP1_Q10(130) + 512, 4, 20, c_white, 1);
    TXT_CTOR(tx[T_C2], OP1_Q10(163) + 512, 4, 20, c_white, 1);
    { static const int16_t dx[5] = { 140, 153, 173, 186, 199 };   /* cell centres + 0.5 px */
      for (int i = 0; i < 5; i++) TXT_CTOR(tx[T_D0 + i], OP1_Q10(dx[i]) + 512, 4, 20, c_white, 1); }
    TXT_CTOR(tx[T_SPEED], OP1_Q10(300), 4, 12, c_white, 2);   /* left of the meter */
    TXT_CTOR(tx[T_REV], OP1_Q10(315), 18, 12, c_red, 2);
#ifdef TF_DIAG
    TXT_CTOR(tx[T_DIAG], OP1_Q10(4), 92, 12, c_grey, 0);
#endif
    made = 1;
}

/* a track around its own head: clips, its loop; window +-win s over [x0, x1] */
static uint8_t rec_red;                                          /* recording or armed: the active track red */
static volatile uint8_t rec_held;                                /* REC held (arms, as stock) */
static void lane(void *r, int t, int32_t y, int32_t x0, int32_t x1, int32_t head, int32_t win, int32_t thick, int on, int big)
{
    const int32_t cx = (x0 + x1) / 2, half = (x1 - x0) / 2, span = win * SR;
    #define XOF(v) (cx + (int32_t)((int64_t)((v) - head) * half / span))
    #define CLAMP(v) ((v) < x0 ? x0 : (v) > x1 ? x1 : (v))
    ln(r, x0, y, x1, y, 0x400, c_dim);
    for (int i = 0; i < NCLIPS; i++) {
        if (!CLIP_USED[i] || CLIPS[i * 3] != t) continue;
        const int32_t a = XOF(CLIPS[i * 3 + 1]), b = XOF(CLIPS[i * 3 + 2]);
        if (b <= x0 || a >= x1) continue;
        const int under = on && CLIPS[i * 3 + 1] <= head && head < CLIPS[i * 3 + 2];   /* the clip under the head: red */
        ln(r, CLAMP(a), y, CLAMP(b), y, thick, tm_tr[t].mute || tm_tr[t].stop ? c_dim : on ? (rec_red || under ? c_red : c_blue) : c_grey);
    }
    if (on && T_MODE == 1 && TAKE_B >= TAKE_A) {                 /* the take, while it is recorded */
        const int32_t a = XOF(TAKE_A), b = XOF(TAKE_B);
        if (b > x0 && a < x1) ln(r, CLAMP(a), y, CLAMP(b) == CLAMP(a) ? CLAMP(a) + OP1_Q10(1) : CLAMP(b), y, thick, c_red);
    }
    const int lp = tm_tr[t].lon && tm_tr[t].lout > tm_tr[t].lin;
    if (lp || tm_tr[t].lset) {                                   /* loop in (also set but off): a line, a circle on top */
        const int32_t a = XOF(tm_tr[t].lin), h = thick / 2 + OP1_Q10(big ? 3 : 2), g = big ? 0x800 : 0x4CC;
        const uint8_t *c = on ? c_green : c_dgreen;
        if (a >= x0 && a <= x1) {
            ln(r, a, y - h, a, y + h, g, c);
            if (big) { const int32_t cy = y - h - OP1_Q10(3), q = OP1_Q10(2);   /* the circle: an octagon */
                static const int8_t ox[9] = { 2, 1, -1, -2, -2, -1, 1, 2, 2 }, oy[9] = { 1, 2, 2, 1, -1, -2, -2, -1, 1 };
                for (int k = 0; k < 8; k++) ln(r, a + ox[k] * q / 2, cy + oy[k] * q / 2, a + ox[k + 1] * q / 2, cy + oy[k + 1] * q / 2, 0x300, c); }
        }
        if (lp) {
            const int32_t b = XOF(tm_tr[t].lout);
            if (b >= x0 && b <= x1) {
                ln(r, b, y - h, b, y + h, g, c);
                if (big) { const int32_t cy = y - h - OP1_Q10(3);   /* loop out: a filled circle (stacked lines) */
                    static const int8_t hw[5] = { 1, 2, 2, 2, 1 };
                    for (int k = 0; k < 5; k++) ln(r, b - hw[k] * OP1_Q10(1), cy + (k - 2) * OP1_Q10(1), b + hw[k] * OP1_Q10(1), cy + (k - 2) * OP1_Q10(1), 0x480, c); }
            }
            if (big && b > x0 && a < x1) ln(r, CLAMP(a), y - h, CLAMP(b), y - h, g, c);
        }
    }
    #undef XOF
    #undef CLAMP
}

static void tfs_update(void *scr, int dt)
{
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    if (!made) { make(); extern void tm_ready(void); tm_ready(); }   /* op1r tape markers: loaded once (GUI thread, as stock) */
    const int32_t head = tm_head_pos(sel) < 0 ? 0 : tm_head_pos(sel);
    const int rev = tm_tr[sel].speed < 0, mode = T_MODE, armed = UI_STATE == 6 || rec_held;
    rec_red = (uint8_t)(mode == 1 || armed);
    /* boxed track number, dim while the track is muted */
    const uint8_t *cn = tm_tr[sel].mute ? c_grey : c_white;
    ln(r, OP1_Q10(3), OP1_Q10(5), OP1_Q10(23), OP1_Q10(5), 0x500, cn); ln(r, OP1_Q10(23), OP1_Q10(5), OP1_Q10(23), OP1_Q10(25), 0x500, cn);
    ln(r, OP1_Q10(23), OP1_Q10(25), OP1_Q10(3), OP1_Q10(25), 0x500, cn); ln(r, OP1_Q10(3), OP1_Q10(25), OP1_Q10(3), OP1_Q10(5), 0x500, cn);
    char b[16]; b[0] = (char)('1' + sel); b[1] = 0; txt(r, tm_tr[sel].mute ? T_NUMD : T_NUM, b);
    {                                                            /* state, right of the square (y 7..23) */
        const int32_t x = OP1_Q10(28), y = OP1_Q10(15);   /* level with the square (y 5 .. 25) */
        if (mode == 1 || armed) {                                /* recording: a red dot; armed: a red ring */
            static const int8_t hw[7] = { 2, 3, 4, 4, 4, 3, 2 };
            for (int k = 0; k < 7; k++) ln(r, x + OP1_Q10(4) - hw[k] * OP1_Q10(1), y + (k - 3) * OP1_Q10(1), x + OP1_Q10(4) + hw[k] * OP1_Q10(1), y + (k - 3) * OP1_Q10(1), 0x480, c_red);
        } else if ((mode == 2 || mode == 3) && !tm_tr[sel].stop) {   /* playing: a triangle (a stopped track: the square) */
            const int32_t xb = rev ? x + OP1_Q10(8) : x, xt = rev ? x : x + OP1_Q10(8);   /* reverse: flipped */
            ln(r, xb, y - OP1_Q10(5), xb, y + OP1_Q10(5), 0x700, c_green);
            ln(r, xb, y - OP1_Q10(5), xt, y, 0x700, c_green);
            ln(r, xb, y + OP1_Q10(5), xt, y, 0x700, c_green);
        } else {                                                 /* stopped: a square */
            ln(r, x, y - OP1_Q10(4), x + OP1_Q10(8), y - OP1_Q10(4), 0x700, c_white); ln(r, x + OP1_Q10(8), y - OP1_Q10(4), x + OP1_Q10(8), y + OP1_Q10(4), 0x700, c_white);
            ln(r, x + OP1_Q10(8), y + OP1_Q10(4), x, y + OP1_Q10(4), 0x700, c_white); ln(r, x, y + OP1_Q10(4), x, y - OP1_Q10(4), 0x700, c_white);
        }
    }
    const uint32_t hp = head < 0 ? 0 : (uint32_t)head, sec = hp / SR, ms = hp % SR * 1000 / SR;   /* m:ss:ms (per frame) */
    put_u(b, sec / 60, 1); txt(r, T_TIME, b);
    txt(r, T_C1, ":"); txt(r, T_C2, ":");
    const uint32_t dg[5] = { sec % 60 / 10, sec % 10, ms / 100, ms / 10 % 10, ms % 10 };
    for (int i = 0; i < 5; i++) { b[0] = (char)('0' + dg[i]); b[1] = 0; txt(r, T_D0 + i, b); }
    fmt_speed(b, tm_tr[sel].speed); txt(r, T_SPEED, b);
    {                                                            /* right edge, as stock: record level + input meter */
        const int32_t x = OP1_Q10(310), y0 = OP1_Q10(8), y1 = OP1_Q10(100), hgt = y1 - y0;   /* above the tracks to the top */
        ln(r, x, y0, x, y1, 0x500, c_dim);                       /* the scale dark, the signal red, the level a red dot */
        /* as stock: a dB scale (R246's getter: log, clamped to -24 dB); here -24 .. 0 dB over the height */
        int32_t pk = IN_PEAK < 0 ? -IN_PEAK : IN_PEAK, db8 = -24 * 256;           /* dB, Q8 */
        if (pk > 0) {                                            /* 20 log10(pk / 32768) = 6.0206 (log2 pk - 15) */
            int e = 0; while ((pk >> e) > 1) e++;
            const int32_t fr = ((pk << 8) >> e) - 256;            /* the mantissa above 1, Q8 */
            const int32_t l2 = (e - 15) * 256 + (fr * (362 - (fr * 106 >> 8)) >> 8);   /* log2(1 + x) ~ x (1.414 - 0.414 x) */
            db8 = l2 * 1541 >> 8;                                /* x 6.0206 */
            if (db8 < -24 * 256) db8 = -24 * 256;
        }
        const int32_t py = y1 - (int32_t)((int64_t)hgt * (db8 + 24 * 256) / (24 * 256));
        if (py < y1) ln(r, x, py, x, y1, 0xA00, c_red);
        const int32_t ly = y1 - (int32_t)((int64_t)hgt * (REC_LEVEL < 0 ? 0 : REC_LEVEL) >> 15);
        static const int8_t hw[7] = { 2, 3, 4, 4, 4, 3, 2 };     /* the dot: stacked lines */
        for (int k = 0; k < 7; k++) ln(r, x - hw[k] * OP1_Q10(1), ly + (k - 3) * OP1_Q10(1), x + hw[k] * OP1_Q10(1), ly + (k - 3) * OP1_Q10(1), 0x480, c_red);
    }
#ifdef TF_DIAG
    {                                                            /* OP1R_TFDIAG=1: why REC may be refused (docs/82) */
        extern volatile uint8_t pu_mode;
        char d[48], *q = d;
        q = put(q, "s"); q = put_u(q, (uint32_t)UI_STATE, 1);
        q = put(q, " q"); q = put_u(q, *(volatile uint32_t *)0x01AA7330 == *(volatile uint32_t *)0x01AA7334, 1);
        q = put(q, " a"); q = put_u(q, *(volatile uint32_t *)0xFF9002B4, 1);
        q = put(q, " c"); q = put_u(q, (uint32_t)*(volatile int32_t *)(0x01AA8D70 + 0x1A00), 1);
        q = put(q, " p"); q = put_u(q, pu_mode, 1);
        { extern volatile uint32_t tm_miss_t[4]; q = put(q, " m"); put_u(q, tm_miss_t[sel], 1); }   /* misses of the active track */
        txt(r, T_DIAG, d);
    }
#endif
    /* the active track around its head */
    {                                                            /* bar marks (from tape 0, R246's tempo) */
        const int64_t bar_q8 = (int64_t)4 * 60 * SR * 256 * 256 / bpm_q8();   /* samples per bar, Q8 */
        const int64_t a = (int64_t)(head - 8 * SR) << 8, b = (int64_t)(head + 8 * SR) << 8;
        for (int64_t k = a <= 0 ? 0 : (a + bar_q8 - 1) / bar_q8; k * bar_q8 <= b && k < 4096; k++) {
            const int32_t x = OP1_Q10(155) + (int32_t)(((k * bar_q8 >> 8) - head) * OP1_Q10(145) / (8 * SR));
            ln(r, x, OP1_Q10(76), x, OP1_Q10(82), 0x800, c_grey);
        }
    }
    lane(r, sel, OP1_Q10(68), OP1_Q10(10), OP1_Q10(300), head, 8, OP1_Q10(9), 1, 1);
    {                                                            /* op1r tape markers (tape/markers.c): the active track only */
        extern int32_t tm_pos[]; extern int tm_n;
        for (int i = 0; i < tm_n; i++) {
            const int64_t d = (int64_t)(tm_pos[i] - head) * OP1_Q10(145) / (8 * SR);
            if (d < -OP1_Q10(145) || d > OP1_Q10(145)) continue;
            const int32_t x = OP1_Q10(155) + (int32_t)d;         /* as on the stock tape screen (tape/markers.c): a grey
                                                                  * line through the track, a green v above (white at the head) */
            const uint8_t *col = d > -OP1_Q10(2) && d < OP1_Q10(2) ? c_white : c_green;
            ln(r, x, OP1_Q10(58), x, OP1_Q10(78), 0x400, c_grey);
            ln(r, x - OP1_Q10(3), OP1_Q10(50), x, OP1_Q10(55), 0x600, col);
            ln(r, x + OP1_Q10(3), OP1_Q10(50), x, OP1_Q10(55), 0x600, col);
        }
    }
    const uint8_t *hc = rec_red ? c_red : c_white;
    ln(r, OP1_Q10(155), OP1_Q10(48), OP1_Q10(155), OP1_Q10(88), 0x600, hc);
    ln(r, OP1_Q10(155), OP1_Q10(48), OP1_Q10(rev ? 149 : 161), OP1_Q10(48), 0x600, hc);   /* the flag: the play direction */
    /* all four tracks */
    for (int t = 0; t < 4; t++) {
        const int32_t y = OP1_Q10(108 + 8 * t), h = tm_head_pos(t) < 0 ? 0 : tm_head_pos(t);
        lane(r, t, y, OP1_Q10(10), OP1_Q10(300), h, 20, OP1_Q10(3), t == sel, 0);
        ln(r, OP1_Q10(155), y - OP1_Q10(3), OP1_Q10(155), y + OP1_Q10(3), 0x400, t == sel ? c_white : c_grey);
    }
}
static void tfs_init(void *scr) { (void)scr; }

static void tfs_set(int on)                                      /* main loop */
{
    if (on && tfs_id < 0) {                                      /* first time: the tape screen is the current one */
        int id = -1;                                             /* the tape screen's slot, by class (not the current screen) */
        for (int i = 0; i < 0x44; i++) if (SCREEN_TABLE[i] && ((const uint32_t *)SCREEN_TABLE[i])[3] == TAPE_VT) { id = i; break; }
        if (id < 0) return;
        void *saved = SCREEN_TABLE[id];
        SCREEN_CTOR(tfs_scr, id, "usbout.svg");
        SCREEN_TABLE[id] = saved;
        for (int i = 0; i < 10; i++) tfs_vt[i] = ITER_SCREEN_VT[i];
        tfs_vt[3 * 2 + 1] = (uint32_t)tfs_update;
        tfs_vt[4 * 2 + 1] = (uint32_t)tfs_init;
        tfs_scr[3] = (uint32_t)tfs_vt;
        tfs_scr[0] = (uint32_t)SVG_LOAD((void *)tfs_scr[2]);
        if (!tfs_scr[0]) return;
        tape_obj = saved; tfs_id = id;
    }
    if (tfs_id < 0) return;
    if (!on && SCREEN_TABLE[tfs_id] != (void *)tfs_scr) { tfs_in = 0; return; }   /* someone else (MIDI seq view) holds the slot */
    SCREEN_TABLE[tfs_id] = on ? (void *)tfs_scr : tape_obj;
    tfs_in = (uint8_t)on;
}
static int8_t wind;                                              /* -1 / +1 while < / > is held on this screen */
static uint32_t wind_t;
void tfs_poll(void)                                              /* main loop, every pass: setting, scrubbing */
{
    if (tm_on && !tfs_in && CUR_MODE_ID == 2) tfs_set(1);
    else if (!tm_on && tfs_in) tfs_set(0);
    extern volatile int8_t tm_scrub; extern int tm_scrubbing(void);
    extern volatile uint8_t tm_scrub_track; tm_scrub_track = sel;
    tm_scrub = wind;                                             /* the engine moves (and plays) only the active track at up
                                                                  * to 8x; the others keep their speed / position */
    (void)tm_scrubbing;
}
static int shown_now(void) { return tfs_in && CUR_MODE_ID == 2 && tfs_id >= 0 && SCREEN_TABLE[tfs_id] == (void *)tfs_scr; }
int tfs_view_open(void) { return shown_now(); }               /* dispatcher.c tape_page: op1r's tape markers work here too */

int tfs_key(int code, int pressed)                               /* key thread: 1 = consumed */
{
    if (!shown_now()) return 0;
    if (code == 0x21) rec_held = (uint8_t)pressed;               /* REC: shown as armed while held; stock handles it */
    if ((code == 0x22 || code == 0x23) && SHIFT) {               /* Shift + PLAY / STOP: start / stop the active track alone
                                                                  * (its head stands while the transport runs on) */
        if (pressed) tm_tr[sel].stop = (uint8_t)(code == 0x23);  /* saved by tfs_state_flush (flag bit 3) */
        return 1;
    }
    if (code == 0x23 && T_MODE == 0 && pressed) {                /* STOP while stopped (also spinning down): the active track to its loop in
                                                                  * (press only: R246's STOP release turns state 1 into 0, REC arms from 0)
                                                                  * (R246's would jump the transport: all tracks resync) */
        if (pressed) {
            tm_set_head(sel, tm_tr[sel].lset ? tm_tr[sel].lin : 0);
            if (UI_STATE == 1) { UI_STATE = 0; ((void (*)(void))0x01A581F4)(); }   /* R246's second STOP: state 1 -> 0 (only
                                                                  * from 0 does REC arm); its transport jump is left out */
        }
        return 1;
    }
    if (code >= 0x06 && code <= 0x09) {                          /* T: select (R246 records here); Shift + T: mute */
        if (pressed) { if (SHIFT) tm_tr[code - 0x06].mute ^= 1; else { extern volatile uint8_t tm_sel; sel = (uint8_t)(code - 0x06); tm_sel = sel; SEL_TRACK = sel; } }
        return 1;
    }
    if (code == 0x0C && !SHIFT) {                                /* scissors: split the active track's clip at its head */
        if (pressed && T_MODE != 1) {                            /* (R246's split: Clips::update_clip(track, pos, 0)) */
            ((void (*)(int, int32_t, int))0x0195CF9C)(sel, tm_head_pos(sel), 0);
            extern volatile uint8_t tm_reload_after_clip; tm_reload_after_clip = 1;
        }
        return 1;
    }
    if ((code == 0x0A || code == 0x0B) && !SHIFT) {              /* lift / drop: the active track at its head (R246's clipboard) */
        if (pressed) {
            const int32_t h = tm_head_pos(sel);
            if (code == 0x0B) ((void (*)(int, int32_t, int))0x0191BEA8)(sel, h, 0);   /* drop (as the tape key handler) */
            else for (int i = 0; i < NCLIPS; i++)                /* lift: the clip under the head */
                if (CLIP_USED[i] && CLIPS[i * 3] == sel && CLIPS[i * 3 + 1] <= h && h < CLIPS[i * 3 + 2]) {
                    const int32_t a = CLIPS[i * 3 + 1];
                    ((void (*)(int, int32_t, int32_t, int))0x0191BBD0)(sel, a, CLIPS[i * 3 + 2], i);   /* audio_clipboard::copy */
                    tm_set_head(sel, a);                         /* as stock: to the lifted clip's start */
                    break;
                }
            extern volatile uint8_t tm_reload_after_clip; tm_reload_after_clip = 1;
        }
        return 1;
    }
    if (code == 0x3E) {                                          /* red press: a marker at the active head; Shift: remove */
        extern void tm_add_at(int32_t), tm_remove_at(int32_t, int32_t);
        if (pressed) { const int32_t h = tm_head_pos(sel); if (SHIFT) tm_remove_at(h, 6 * 2352); else tm_add_at(h); }   /* ~6 px */
        return 1;
    }
    if (code == 0x11) {                                          /* Shift + s5: reverse the active track */
        static uint8_t sw;
        if (pressed && SHIFT) { tm_tr[sel].speed = -tm_tr[sel].speed; sw = 1; return 1; }
        if (!pressed && sw) { sw = 0; return 1; }
        return 0;
    }
    if (code == 0x24 || code == 0x25) {                          /* < / >: our scrubbing (R246's would call TapeScreen
                                                                  * methods on this object); Shift: previous / next marker */
        if (pressed && SHIFT) {                                  /* Shift: previous / next bar (R246's tempo, from tape 0) */
            const int64_t bar_q8 = (int64_t)4 * 60 * SR * 256 * 256 / bpm_q8(), h = (int64_t)tm_head_pos(sel) << 8, m = 2000 << 8;
            int64_t k = code == 0x25 ? (h + m) / bar_q8 + 1 : (h - m <= 0 ? 0 : (h - m - 1) / bar_q8);
            const int32_t v = (int32_t)(k * bar_q8 >> 8);
            tm_set_head(sel, v < 0 ? 0 : v > TM_MAXPOS ? TM_MAXPOS : v);   /* the active track only */
            return 1;
        }
        wind = pressed && !SHIFT ? (code == 0x24 ? -1 : 1) : 0; wind_t = now();
        return 1;
    }
    if (code < 0x2D || code > 0x2F) return 0;
    if (!pressed) return 1;
    volatile tm_track *tr = &tm_tr[sel];
    const int32_t h = tm_head_pos(sel);
    if (code == 0x2D) { tr->lin = h; tr->lset = 1; if (tr->lout <= tr->lin) tr->lon = 0; }   /* s1: in (marker even if off) */
    else if (code == 0x2E) {                                     /* s2: out, the loop on */
        tr->lout = h;
        if (tr->lout < tr->lin) { const int32_t x = tr->lin; tr->lin = tr->lout; tr->lout = x; }
        tr->lon = tr->lout > tr->lin;
    } else if (SHIFT) {                                          /* Shift + s3: the loop = the clip under the head */
        for (int i = 0; i < NCLIPS; i++)
            if (CLIP_USED[i] && CLIPS[i * 3] == sel && CLIPS[i * 3 + 1] <= h && h < CLIPS[i * 3 + 2]) {
                tr->lin = CLIPS[i * 3 + 1]; tr->lout = CLIPS[i * 3 + 2]; tr->lset = 1; tr->lon = 1;
                break;
            }
    } else tr->lon = (uint8_t)(!tr->lon && tr->lout > tr->lin);   /* s3: on / off */
    return 1;
}

static int32_t acc[8];
static int step_of(int k, int delta, int per)                    /* detents (2048 per detent on the device: 2 events) */
{
    if ((acc[k] > 0 && delta < 0) || (acc[k] < 0 && delta > 0)) acc[k] = 0;
    acc[k] += delta;
    const int st = acc[k] / per; acc[k] -= st * per;
    return st;
}
int tfs_knob(int knob, int delta)                                /* key thread: 1 = consumed (blue, red) */
{
    if (!shown_now() || knob < 0 || knob > 7) return 0;
    const int k = knob & 3, fine = knob > 3 || SHIFT;
    if (k != 2 && k != 3) return 0;                              /* blue / green: R246's */
    volatile tm_track *tr = &tm_tr[sel];
    if (k == 3) {                                                /* red: record level, as stock; Shift + red: marker jump */
        if (fine) {                                              /* the active track only (op1r tape markers, tape/markers.c) */
            const int st = step_of(7, delta, 1024); if (!st) return 1;
            extern int32_t tm_pos[]; extern int tm_n;
            const int32_t h = tm_head_pos(sel), m = 2000;
            int32_t best = -1;
            for (int i = 0; i < tm_n; i++) {
                const int32_t v = tm_pos[i];
                if (st > 0 ? (v > h + m && (best < 0 || v < best)) : (v < h - m && (best < 0 || v > best))) best = v;
            }
            if (best < 0)                                        /* past the last / before the first: wrap to the first / last */
                for (int i = 0; i < tm_n; i++) if (best < 0 || (st > 0 ? tm_pos[i] < best : tm_pos[i] > best)) best = tm_pos[i];
            if (best >= 0) tm_set_head(sel, best);
            return 1;
        }
        const int st = step_of(3, delta, 1024); if (!st) return 1;
        int32_t v = REC_LEVEL + st * 0x400;                      /* 32 clicks over the range */
        REC_LEVEL = (int16_t)(v < 0 ? 0 : v > 0x7FFF ? 0x7FFF : v);
        return 1;
    }
    const int neg = tr->speed < 0;
    int32_t sp = neg ? -tr->speed : tr->speed;
    if (fine) { const int st = step_of(6, delta, 512); if (!st) return 1;
        sp += st * (sp >> 7); if (sp < 0x2000) sp = 0x2000; if (sp > 0x40000) sp = 0x40000; }
    else { const int st = step_of(2, delta, 2048); if (!st) return 1;
        int i = 0; while (i < NR - 1 && ratios[i] < sp) i++;    /* nearest ratio at or above */
        if (ratios[i] != sp && st < 0) i++;
        i += st; i = i < 0 ? 0 : i >= NR ? NR - 1 : i; sp = ratios[i]; }
    tr->speed = neg ? -sp : sp;
    return 1;
}

/* FREE track state across reboots (op1rsettings, docs/82): per track head, loop in / out, speed, flags (loop on | in
 * set << 1 | mute << 2 | stop << 3), and the active track. Saved at key events when changed (as the settings pages, KeyHandler
 * thread), heads only while the tape stands still; loaded at boot. Only changed values are written. */
extern int op1r_setting_get(const char *, int); extern void op1r_setting_put(const char *, int);
static int32_t sv[4][5]; static int32_t sv_sel = -1; static uint8_t sv_have;
static char *sv_key(char *k, int t, char c) { k[0] = 't'; k[1] = 'f'; k[2] = (char)('0' + t); k[3] = c; k[4] = 0; return k; }
void tfs_state_load(void)
{
    extern volatile uint8_t tm_sel, tm_hold_heads;
    char k[5];
    for (int t = 0; t < 4; t++) {
        volatile tm_track *tr = &tm_tr[t];
        const int32_t h = op1r_setting_get(sv_key(k, t, 'h'), -1), li = op1r_setting_get(sv_key(k, t, 'i'), -1),
                      lo = op1r_setting_get(sv_key(k, t, 'o'), -1), sp = op1r_setting_get(sv_key(k, t, 's'), 0),
                      fl = op1r_setting_get(sv_key(k, t, 'f'), -1);
        if (h >= 0 && h <= TM_MAXPOS) tm_set_head(t, h);
        if (li >= 0 && li <= TM_MAXPOS && lo >= 0 && lo <= TM_MAXPOS) { tr->lin = li; tr->lout = lo; }
        if ((sp >= 0x2000 && sp <= 0x40000) || (sp <= -0x2000 && sp >= -0x40000)) tr->speed = sp;   /* 1/8 .. 4x, either way */
        if (fl >= 0 && fl <= 15) { tr->lon = (uint8_t)(fl & 1 && tr->lout > tr->lin); tr->lset = (uint8_t)(fl >> 1 & 1); tr->mute = (uint8_t)(fl >> 2 & 1); tr->stop = (uint8_t)(fl >> 3 & 1); }
        sv[t][0] = h; sv[t][1] = li; sv[t][2] = lo; sv[t][3] = sp; sv[t][4] = fl;
    }
    const int32_t sl = op1r_setting_get("tfsel", -1);
    if (sl >= 0 && sl <= 3) { tm_sel = (uint8_t)sl; SEL_TRACK = sl; }
    sv_sel = sl; sv_have = 1;
    tm_hold_heads = 1;                                           /* tm.c: no resync to the transport before the first play */
}
void tfs_state_flush(void)
{
    extern volatile uint8_t tm_on, tm_sel;
    if (!tm_on || !sv_have) return;
    const int still = T_MODE == 0 && TSPEED_RAW == 0;
    char k[5];
    for (int t = 0; t < 4; t++) {
        const volatile tm_track *tr = &tm_tr[t];
        const int32_t v[5] = { still ? tm_head_pos(t) : sv[t][0], tr->lin, tr->lout, tr->speed, tr->lon | tr->lset << 1 | tr->mute << 2 | tr->stop << 3 };
        static const char key[5] = { 'h', 'i', 'o', 's', 'f' };
        for (int j = 0; j < 5; j++) if (v[j] != sv[t][j]) { op1r_setting_put(sv_key(k, t, key[j]), v[j]); sv[t][j] = v[j]; }
    }
    if (tm_sel != sv_sel) { op1r_setting_put("tfsel", tm_sel); sv_sel = tm_sel; }
}
