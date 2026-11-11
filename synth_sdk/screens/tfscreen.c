/* Free tape machine screen (docs/82, approved preview display/tapemachine_preview.py): replaces R246's tape screen
 * while the free machine is on (op1r settings, white: TAPE STOCK / FREE). Four lanes: the dark line is R246's
 * global loop (or, without a loop, a 20 s window around the transport), the bright segment the track's own loop,
 * a tick its head (flag = direction). The selected track's values below, in encoder colours:
 *   blue speed (ratios; Shift: fine), green loop (fraction of the global loop), white offset (Shift: fine),
 *   red direction (fwd / rev). Track keys T1..T4 select; every other key stays R246's (transport, record, lift ..).
 * Swap like the MIDI sequencer view (seqdev/ms_fw.c): an iter-class screen object (empty usbout.svg) put into the
 * tape screen's slot of the screen table; R246's object back when the machine is off. */
#include "../op1_synth.h"
#include "stroke_font.h"

typedef struct { int32_t speed; uint16_t loop, off; } tm_track;
extern volatile tm_track tm_tr[4];
extern volatile uint8_t tm_on;
int32_t tm_head_pos(int t);
int tm_span(int t, int32_t *lo, int32_t *hi);

#define SCREEN_TABLE   ((void **)0x01DF4E54)
#define CUR_MODE_ID    (*(volatile int *)0xFF907D58)              /* tape = 2 */
#define CUR_SCREEN     (*(volatile int *)0x018B65C8)
#define SCREEN_CTOR    ((void *(*)(void *, int, const char *))0x01A3BAB0)
#define ITER_SCREEN_VT ((const uint32_t *)0x019103FC)
#define SVG_LOAD       ((void *(*)(void *))0x01A503D8)
#define SHIFT          (*(volatile uint8_t *)0x01DD7F78)
#define TPOS           (*(volatile int32_t *)0xFF802974)          /* samples << 6 */
#define LOOP_S         (*(volatile int32_t *)0x01B4EF34)
#define LOOP_E         (*(volatile int32_t *)0x01B4EF38)
#define LOOP_ON        (*(volatile uint8_t *)0xFF8029C5)

static uint32_t tfs_scr[32], tfs_vt[10];
static void *tape_obj;
static int tfs_id = -1;
static uint8_t tfs_in, sel;

static const int32_t ratios[] = { 0x4000, 0x5555, 0x8000, 0xAAAB, 0xC000, 0x10000, 0x15555, 0x18000, 0x20000, 0x30000, 0x40000 };
static const char *const ratio_txt[] = { "1/4", "1/3", "1/2", "2/3", "3/4", "1", "4/3", "3/2", "2", "3", "4" };
#define NR ((int)(sizeof ratios / sizeof ratios[0]))
static const uint16_t loops[] = { 0x2000, 0x2AAB, 0x4000, 0x5555, 0x8000, 0xAAAB, 0xC000, 0xFFFF };
static const char *const loop_txt[] = { "1/8", "1/6", "1/4", "1/3", "1/2", "2/3", "3/4", "1" };
#define NL ((int)(sizeof loops / sizeof loops[0]))

static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     c_grey[4] = { 0x6F, 0x71, 0x73, 0xFF }, c_dim[4] = { 0x40, 0x42, 0x44, 0xFF };

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c) { if (x0 != x1 || y0 != y1) sf_line(r, x0, y0, x1, y1, w, c); }
static char *put_u(char *q, uint32_t v) { char b[10]; int n = 0; do b[n++] = (char)('0' + v % 10); while (v /= 10); while (n) *q++ = b[--n]; return q; }
static void fmt_speed(char *q, int32_t sp)                       /* "1/2X", "1.37X" */
{
    if (sp < 0) sp = -sp;
    for (int i = 0; i < NR; i++) if (ratios[i] == sp) { const char *s = ratio_txt[i]; while (*s) *q++ = *s++; *q++ = 'X'; *q = 0; return; }
    const uint32_t c = (uint32_t)(((int64_t)sp * 100 + 0x8000) >> 16);
    q = put_u(q, c / 100); *q++ = '.'; *q++ = (char)('0' + c / 10 % 10); *q++ = (char)('0' + c % 10); *q++ = 'X'; *q = 0;
}
static void fmt_frac(char *q, uint16_t v, int loop)
{
    if (loop) for (int i = 0; i < NL; i++) if (loops[i] == v) { const char *s = loop_txt[i]; while (*s) *q++ = *s++; *q = 0; return; }
    if (!v) { *q++ = '0'; *q = 0; return; }
    const uint32_t c = ((uint32_t)v * 100 + 0x8000) >> 16;       /* .xx of the loop */
    *q++ = '.'; *q++ = (char)('0' + c / 10 % 10); *q++ = (char)('0' + c % 10); *q = 0;
}

#define X0  OP1_Q10(44)
#define X1  OP1_Q10(268)
#define LY(i) OP1_Q10(18 + 22 * (i))
#define TS  (OP1_Q10(5) / 2)
static void tfs_update(void *scr, int dt)
{
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    const int looped = LOOP_ON && LOOP_E > LOOP_S;
    const int32_t pos = TPOS >> 6, gs = looped ? LOOP_S : pos - 441000, gl = looped ? LOOP_E - LOOP_S : 882000;
    for (int t = 0; t < 4; t++) {
        const int on = t == sel; const int32_t y = LY(t);
        char b[12]; b[0] = (char)('1' + t); b[1] = 0;
        sf_text(r, OP1_Q10(14), y - OP1_Q10(7), TS, b, 0x500, on ? c_white : c_grey);
        ln(r, X0, y, X1, y, 0x400, c_dim);
        int32_t lo, hi;
        if (tm_span(t, &lo, &hi)) {                              /* own loop */
            const int32_t a = X0 + (int32_t)((int64_t)(X1 - X0) * (lo - gs) / gl), e = X0 + (int32_t)((int64_t)(X1 - X0) * (hi - gs) / gl);
            ln(r, a, y, e, y, 0x800, on ? c_white : c_grey);
            ln(r, a, y - OP1_Q10(3), a, y + OP1_Q10(3), 0x400, on ? c_white : c_grey);
            ln(r, e, y - OP1_Q10(3), e, y + OP1_Q10(3), 0x400, on ? c_white : c_grey);
        }
        const int32_t h = tm_head_pos(t);
        int64_t px = X0 + (int64_t)(X1 - X0) * (h - gs) / gl;
        if (px >= X0 && px <= X1) {
            const uint8_t *c = on ? c_red : c_white; const int32_t p = (int32_t)px, d = tm_tr[t].speed < 0 ? -OP1_Q10(5) : OP1_Q10(5);
            ln(r, p, y - OP1_Q10(7), p, y + OP1_Q10(7), 0x800, c);
            ln(r, p, y - OP1_Q10(7), p + d, y - OP1_Q10(7), 0x800, c);
        }
        fmt_speed(b, tm_tr[t].speed);
        int32_t w = 0; for (const char *s = b; *s; s++) w += OP1_Q10(5) * 5 / 2 * 5 / 4;   /* about one cell per char */
        sf_text(r, OP1_Q10(306) - w, y - OP1_Q10(7), TS, b, 0x500, on ? c_blue : c_grey);
    }
    char v[4][12];
    fmt_speed(v[0], tm_tr[sel].speed); fmt_frac(v[1], tm_tr[sel].loop, 1); fmt_frac(v[2], tm_tr[sel].off, 0);
    v[3][0] = 0; { const char *s = tm_tr[sel].speed < 0 ? "REV" : "FWD"; char *q = v[3]; while (*s) *q++ = *s++; *q = 0; }
    static const char *const nm[4] = { "SPEED", "LOOP", "OFFSET", "DIR" };
    const uint8_t *col[4] = { c_blue, c_green, c_white, c_red };
    for (int k = 0; k < 4; k++) {
        sf_text(r, OP1_Q10(18 + 76 * k), OP1_Q10(102), OP1_Q10(2), nm[k], 0x400, col[k]);   /* 8 px caps: OFFSET fits the column */
        sf_text(r, OP1_Q10(18 + 76 * k), OP1_Q10(118), TS, v[k], 0x500, col[k]);
    }
}
static void tfs_init(void *scr) { (void)scr; }

static void tfs_set(int on)                                      /* main loop / key thread */
{
    if (on && tfs_id < 0) {                                      /* first time: the tape screen is the current one */
        const int id = CUR_SCREEN;
        if (id < 0 || id >= 0x44 || !SCREEN_TABLE[id]) return;
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
void tfs_poll(void)                                              /* main loop: follow the setting */
{
    if (tm_on && !tfs_in && CUR_MODE_ID == 2) tfs_set(1);
    else if (!tm_on && tfs_in) tfs_set(0);
}
static int shown(void) { return tfs_in && CUR_MODE_ID == 2 && tfs_id >= 0 && SCREEN_TABLE[tfs_id] == (void *)tfs_scr; }

int tfs_key(int code, int pressed)                               /* key thread: 1 = consumed */
{
    if (!shown() || code < 0x06 || code > 0x09) return 0;
    if (pressed) sel = (uint8_t)(code - 0x06);
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
int tfs_knob(int knob, int delta)                                /* key thread: 1 = consumed */
{
    if (!shown() || knob < 0 || knob > 7) return 0;
    const int k = knob & 3, fine = knob > 3 || SHIFT;
    volatile tm_track *tr = &tm_tr[sel];
    const int neg = tr->speed < 0;
    int32_t sp = neg ? -tr->speed : tr->speed;
    if (k == 0) {
        if (fine) { const int st = step_of(4, delta, 512); if (!st) return 1;
            sp += st * (sp >> 7); if (sp < 0x2000) sp = 0x2000; if (sp > 0x40000) sp = 0x40000; }
        else { const int st = step_of(0, delta, 2048); if (!st) return 1;
            int i = 0; while (i < NR - 1 && ratios[i] < sp) i++;      /* nearest ratio at or above */
            if (ratios[i] != sp && st < 0) i++;
            i += st; i = i < 0 ? 0 : i >= NR ? NR - 1 : i; sp = ratios[i]; }
        tr->speed = neg ? -sp : sp;
    } else if (k == 1) {
        const int st = step_of(1, delta, 2048); if (!st) return 1;
        int i = 0; while (i < NL - 1 && loops[i] < tr->loop) i++;
        i += st; i = i < 0 ? 0 : i >= NL ? NL - 1 : i; tr->loop = loops[i];
    } else if (k == 2) {
        const int st = step_of(fine ? 6 : 2, delta, fine ? 512 : 2048); if (!st) return 1;
        int32_t o = tr->off + st * (fine ? 0x100 : 0x1000);
        tr->off = (uint16_t)(o < 0 ? 0 : o > 0xFFFF ? 0xFFFF : o);
    } else {
        const int st = step_of(3, delta, 2048); if (!st) return 1;
        tr->speed = -tr->speed;                                  /* any detent flips the direction */
    }
    return 1;
}
