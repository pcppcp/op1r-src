/* drum+ pages (docs/31, mock-up: display/drumplus_preview.py): T1 play (screen 0x10), T2 envelope and tone (0x17),
 * drawn in code over the pad's region. Turns / presses edit the pad's dp_extra (and the stock reverse / playmode
 * words); T1 blue turn (pitch) and Shift + red (volume) stay with the stock drum controller.
 * Host: compiled into harness/dp_ui_test.c with OP1_HOST (logic + draw recorded); firmware glue at the end. */
#include "../op1_synth.h"
#include "dp_plus.h"
#include "dp_tables.h"

enum { DPU_T1 = 0, DPU_T2 = 1 };
#define DPU_X0   20                          /* waveform area */
#define DPU_X1   300
#define DPU_COLS ((DPU_X1 - DPU_X0) / 2)     /* one line every 2 px */
#define DPU_N    529200                      /* drum buffer: 12 s (sampedit's SE_CAP_DRUM) */
#define DPU_MID  66
#define DPU_AMP  32
#define DPU_TS   12                          /* text size (as the sample editor) */

/* ---- edits (shared with the host test) ---- */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* turn: field and step size per (page, knob, shift); -1 = the stock controller's (pitch, volume) */
static const int8_t dpu_turn_field[2][2][4] = {
    { { -1, 6, 9, 14 }, { 8, 7, 10, -1 } },                 /* T1: pitch | p env | retrig | drive; fine | p time | rate | volume */
    { { 1, 3, 4, 12 }, { 17, 20, 13, 18 } },                /* T2: attack | decay | release | cutoff; vel>pitch | human | reso | vel>cut */
};
int dpu_turn_target(int page, int knob, int shift) { return dpu_turn_field[page & 1][shift ? 1 : 0][knob & 3]; }

/* steps: whole detents (signed). Returns 1 if the extras changed. */
int dpu_turn(dp_extra *x, int page, int knob, int shift, int steps)
{
    int f = dpu_turn_target(page, knob, shift);
    if (f < 0 || !steps) return 0;
    int v = dp_extra_get(x, f), nv;
    (void)clampi;
    if (f == 9) {                                           /* retrig: off, 2..8 */
        nv = v + steps;
        if (v == 0 && steps > 0) nv = 2 + steps - 1;
        if (nv < 2) nv = 0;
    } else nv = v + steps;
    int changed = dp_extra_set(x, f, nv) != v;
    /* shaping the envelope / filter switches it on (else the turn would do nothing audible) */
    if ((f == 1 || f == 3 || f == 4) && !x->env_on) { x->env_on = 1; changed = 1; }
    if ((f == 12 || f == 13) && !x->filter) { x->filter = 1; changed = 1; }
    return changed;
}

/* press: returns 1 extras changed, 2 pad words changed */
int dpu_press(dp_extra *x, int16_t *padw, int page, int knob)
{
    static const uint8_t vs[4] = { 0, 40, 80, 127 };
    if (page == DPU_T1) {
        switch (knob & 3) {
        case 0: padw[4] = (int16_t)(padw[4] >> 14 ? 8192 : 16384); return 2;          /* reverse: fwd 8192, rev 16384 */
        case 1: {                                                                    /* playmode: gate -> one-shot -> loop */
            int m = padw[3] >> 13;
            padw[3] = (int16_t)(m == 0 ? 8192 : m == 1 ? 16384 : 4096);   /* gate 4096: the stock range is 4096..20480 */
            return 2;
        }
        case 2: x->choke = (uint8_t)((x->choke + 1) % 5); return 1;
        default: x->mono ^= 1; return 1;
        }
    }
    switch (knob & 3) {
    case 0: x->env_on ^= 1; return 1;
    case 1: x->curve_exp ^= 1; return 1;
    case 2: x->filter = (uint8_t)((x->filter + 1) & 3); return 1;
    default: {
        int i = 0;
        while (i < 3 && vs[i] < x->vel_start) i++;
        x->vel_start = vs[(i + (vs[i] == x->vel_start)) & 3];
        return 1;
    }
    }
}

/* ---- labels ---- */
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *num(char *p, int v, int sign)
{
    char t[12]; int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; } else if (sign && v) *p++ = '+';
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    *p = 0; return p;
}
static void fmt_ms(char *p, uint32_t samples)
{
    uint32_t ms = samples * 10 / 441;
    if (ms < 1000) { p = num(p, (int)ms, 0); put(p, "ms"); return; }
    p = num(p, (int)(ms / 1000), 0); *p++ = '.'; p = num(p, (int)(ms % 1000 / 100), 0); put(p, "s");
}
static void fmt_hz(char *p, int hz)
{
    if (hz < 1000) { p = num(p, hz, 0); put(p, "hz"); return; }
    p = num(p, hz / 1000, 0); *p++ = '.'; p = num(p, hz % 1000 / 100, 0); put(p, "k");
}

/* label, value and press-state texts of encoder k on (page, shift) */
void dpu_texts(const dp_extra *x, const int16_t *padw, int page, int shift, int k, char *lab, char *val, char *prs)
{
    static const char *const l[2][2][4] = {
        { { "pitch", "p env", "retrig", "drive" }, { "fine", "p time", "rate", "volume" } },
        { { "attack", "decay", "release", "cutoff" }, { "vel>pitch", "human", "reso", "vel>cut" } },
    };
    static const char *const rate[6] = { "1/8", "1/8t", "1/16", "1/16t", "1/32", "1/64" };
    static const char *const flt[4] = { "off", "lp", "hp", "bp" };
    put(lab, l[page & 1][shift ? 1 : 0][k & 3]);
    val[0] = prs[0] = 0;
    if (page == DPU_T1) {
        if (!shift) switch (k) {
        case 0: num(val, padw[0] >> 9, 1); put(prs, padw[4] >> 14 ? "rev" : ""); break;
        case 1: num(val, x->pitch_env, 1); put(prs, (padw[3] >> 13) == 0 ? "gate" : (padw[3] >> 13) == 1 ? "shot" : "loop"); break;
        case 2: if (x->retrig) num(val, x->retrig, 0); else put(val, "off");
                if (x->choke) { prs[0] = 'c'; num(prs + 1, x->choke, 0); } break;
        default: num(val, x->drive, 0); put(prs, x->mono ? "mono" : ""); break;
        } else switch (k) {
        case 0: put(num(val, x->fine, 1), "c"); break;
        case 1: fmt_ms(val, dp_time_samples(x->pitch_time)); break;
        case 2: put(val, rate[x->retrig_div % 6]); break;
        default: num(val, (int)padw[7] * 100 / 32768, 0); break;
        }
        return;
    }
    if (!shift) switch (k) {
    case 0: fmt_ms(val, dp_time_samples(x->attack)); put(prs, x->env_on ? "env" : "off"); break;
    case 1: if (x->decay) fmt_ms(val, dp_time_samples(x->decay)); else put(val, "hold");
            put(prs, x->curve_exp ? "exp" : "lin"); break;
    case 2: fmt_ms(val, dp_time_samples(x->release)); put(prs, flt[x->filter & 3]); break;
    default: fmt_hz(val, dp_cutoff_hz[x->cutoff & 127]);
             put(prs, x->vel_start == 0 ? "" : x->vel_start <= 40 ? "v>st1" : x->vel_start <= 80 ? "v>st2" : "v>st3"); break;
    } else switch (k) {
    case 0: num(val, x->vel_pitch, 1); break;
    case 1: num(val, x->human, 0); break;
    case 2: num(val, x->resonance, 0); break;
    default: num(val, x->vel_cutoff, 1); break;
    }
}

/* ---- drawing ---- */
static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_grey[4] = { 0x6F, 0x71, 0x73, 0xFF },
                     c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF }, c_lilac[4] = { 0xDF, 0xD9, 0xFF, 0xFF },
                     c_pale[4] = { 0xAE, 0xB1, 0xDC, 0xFF }, c_dark[4] = { 0x3A, 0x22, 0x28, 0xFF };
static const uint8_t *const dpu_col[4] = { c_blue, c_green, c_white, c_red };
static void ln(void *r, int x0, int y0, int x1, int y1, int w, const uint8_t *c)   /* px * 1024 (Q10) */
{
    if (x0 == x1 && y0 == y1) return;                    /* zero-length lines are dropped by the rasteriser */
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
#define Q(v) ((v) << 10)

typedef struct {
    int16_t lo[DPU_COLS], hi[DPU_COLS];
    int32_t key0, key1;                                  /* region the columns belong to */
    int32_t len;                                         /* region length in samples */
} dpu_wave;

/* columns of the region [a, b) (sample indices of data), at most 64 reads per column */
void dpu_wave_update(dpu_wave *w, const int16_t *data, int32_t a, int32_t b)
{
    a = a < 0 ? 0 : a > DPU_N ? DPU_N : a;               /* a kit's pad words may be anything (sampedit clamps too) */
    b = b < 0 ? 0 : b > DPU_N ? DPU_N : b;
    if (w->key0 == a && w->key1 == b) return;
    w->key0 = a; w->key1 = b; w->len = b > a ? b - a : 0;
    int32_t per = w->len / DPU_COLS;                     /* (on a region change only) */
    int32_t step = per > 64 ? per / 64 : 1;
    for (int c = 0; c < DPU_COLS; c++) {
        int16_t lo = 0, hi = 0;
        int32_t s0 = a + per * c;
        for (int32_t i = 0; i < per; i += step) { int16_t v = data[s0 + i]; if (v < lo) lo = v; if (v > hi) hi = v; }
        w->lo[c] = lo; w->hi[c] = hi;
    }
}

static void draw_wave(void *r, const dpu_wave *w)
{
    for (int c = 0; c < DPU_COLS; c++) {
        int32_t y0 = Q(DPU_MID) - w->hi[c] * DPU_AMP / 32, y1 = Q(DPU_MID) - w->lo[c] * DPU_AMP / 32;
        if (y1 - y0 < 0x400) { y0 -= 0x200; y1 += 0x200; }
        int32_t x = Q(DPU_X0 + 2 * c) + 0x200;
        ln(r, x, y0, x, y1, 0x400, c_pale);   /* waveform: pale (grey was too dark) */
    }
}

static int32_t t2x(const dpu_wave *w, uint32_t t)        /* time (samples from the hit) -> x Q10, clamped */
{
    int32_t len = w->len > 4410 ? w->len : 4410;
    if (t >= (uint32_t)len) return Q(DPU_X1);
    return Q(DPU_X0) + (int32_t)(((int64_t)t * Q(DPU_X1 - DPU_X0)) / len);
}

static void draw_t1(void *r, const dpu_wave *w, const dp_extra *x, uint32_t spb)
{
    if (x->pitch_env) {                                  /* pitch envelope: offset gliding to 0 */
        int32_t y = Q(DPU_MID) - x->pitch_env * Q(DPU_AMP - 4) / 48, xe = t2x(w, dp_time_samples(x->pitch_time));
        if (xe < Q(DPU_X0) + 0x800) xe = Q(DPU_X0) + 0x800;
        ln(r, Q(DPU_X0), y, xe, Q(DPU_MID), 0x600, c_green);
        ln(r, xe, Q(DPU_MID), Q(DPU_X1), Q(DPU_MID), 0x600, c_green);
    }
    if (x->retrig >= 2) {                                /* retrig hits: ticks, falling */
        static const uint8_t per_beat[6] = { 2, 3, 4, 6, 8, 16 };
        uint32_t iv = spb / per_beat[x->retrig_div % 6];
        int32_t h = Q(10);
        for (int k = 0; k < x->retrig; k++) {
            int32_t xx = t2x(w, iv * (uint32_t)k);
            if (xx >= Q(DPU_X1)) break;
            ln(r, xx + 0x400, Q(104) - h, xx + 0x400, Q(104), 0x800, c_white);
            h = h * 26 / 33;
        }
    }
    if (x->drive) {                                      /* drive meter at the right edge */
        ln(r, Q(306), Q(98), Q(306), Q(58), 0xC00, c_dark);
        ln(r, Q(306), Q(98), Q(306), Q(98) - x->drive * Q(40) / 127, 0xC00, c_red);
    }
}

static void draw_t2(void *r, const dpu_wave *w, const dp_extra *x)
{
    if (x->env_on) {
        uint32_t ta = dp_time_samples(x->attack), th = dp_time_samples(x->hold), td = dp_time_samples(x->decay);
        int32_t xa = t2x(w, ta), xh = t2x(w, ta + th), top = Q(DPU_MID - DPU_AMP), base = Q(DPU_MID + DPU_AMP);
        if (xa == Q(DPU_X0)) xa += 0x400;                /* attack 0: a vertical edge */
        ln(r, Q(DPU_X0), base, xa, top, 0x600, c_blue);
        ln(r, xa, top, xh, top, 0x600, c_lilac);
        int32_t px = xh, py = top, xend;
        if (!td) { ln(r, xh, top, Q(DPU_X1), top, 0x600, c_green); xend = Q(DPU_X1); py = top; }
        else {
            for (int i = 1; i <= 16; i++) {              /* decay: 16 segments (exp: e^-6.9 t ~ 2^-10 t) */
                int32_t nx = t2x(w, ta + th + td * (uint32_t)i / 16);
                static const uint16_t ex[17] = { 1024, 662, 431, 279, 181, 117, 76, 49, 32, 21, 13, 9, 6, 4, 2, 1, 1 };
                int32_t lvl = x->curve_exp ? ex[i] : Q(1) - Q(1) * i / 16;              /* Q10 level (exp: 2^(-10 i/16)) */
                int32_t ny = base - (int32_t)(((int64_t)(base - top) * lvl) >> 10);
                ln(r, px, py, nx, ny, 0x600, c_green);
                px = nx; py = ny;
            }
            xend = px;
        }
        if (x->release && xend < Q(DPU_X1)) {            /* release: from the sustain level after the decay */
            int32_t xr = xend + t2x(w, dp_time_samples(x->release)) - Q(DPU_X0);
            ln(r, xend, py, xr < Q(DPU_X1) ? xr : Q(DPU_X1), base, 0x600, c_white);
        }
    }
    if (x->filter) {                                     /* filter response sketch, right of the area */
        int32_t x0 = Q(236), x1 = Q(300), y = Q(98), fc = x0 + (x1 - x0) * x->cutoff / 127;
        int32_t bump = x->resonance * Q(12) / 127, fall = Q(16);
        int32_t y0 = y - Q(6), pk = y0 - bump, lo = y0 + fall;
        if (x->filter == 1) {                            /* low pass: flat, bump at the cutoff, falling */
            int32_t a = fc - Q(6) > x0 ? fc - Q(6) : x0, e = fc + Q(14) < x1 ? fc + Q(14) : x1;
            ln(r, x0, y0, a, y0, 0x600, c_red); ln(r, a, y0, fc, pk, 0x600, c_red); ln(r, fc, pk, e, lo, 0x600, c_red);
        } else if (x->filter == 2) {                     /* high pass: mirrored */
            int32_t a = fc - Q(14) > x0 ? fc - Q(14) : x0, e = fc + Q(6) < x1 ? fc + Q(6) : x1;
            ln(r, a, lo, fc, pk, 0x600, c_red); ln(r, fc, pk, e, y0, 0x600, c_red); ln(r, e, y0, x1, y0, 0x600, c_red);
        } else {                                         /* band pass */
            int32_t a = fc - Q(16) > x0 ? fc - Q(16) : x0, e = fc + Q(16) < x1 ? fc + Q(16) : x1;
            ln(r, a, lo, fc, pk, 0x600, c_red); ln(r, fc, pk, e, lo, 0x600, c_red);
        }
    }
}

/* texts: header (pad, drum+, shift), choke / mono, 4 x (label, value, press) */
enum { DT_PAD, DT_NAME, DT_SHIFT, DT_LAB = 3, DT_VAL = 7, DT_PRS = 11, DT_N = 15 };
static uint32_t dpu_txt[DT_N][OP1_TEXT_SIZE / 4];
static char dpu_shown[DT_N][12];
static const int32_t dpu_ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
static uint8_t dpu_texts_made;
static void dpu_texts_init(void)
{
    if (dpu_texts_made) return;
    op1_text_ctor(dpu_txt[DT_PAD], Q(8), 4, DPU_TS, c_white);
    op1_text_ctor(dpu_txt[DT_NAME], Q(262), 4, DPU_TS, c_pale);
    op1_text_ctor(dpu_txt[DT_SHIFT], Q(140), 4, DPU_TS, c_grey);
    for (int k = 0; k < 4; k++) {
        op1_text_ctor(dpu_txt[DT_LAB + k], Q(8 + 78 * k), 112, DPU_TS, dpu_col[k]);
        op1_text_ctor(dpu_txt[DT_VAL + k], Q(8 + 78 * k), 126, DPU_TS, c_white);
        op1_text_ctor(dpu_txt[DT_PRS + k], Q(8 + 78 * k), 19, DPU_TS, dpu_col[k]);   /* press states: status row */
    }
    op1_text_set(dpu_txt[DT_NAME], "drum+");
    op1_text_set(dpu_txt[DT_SHIFT], "shift");
    dpu_texts_made = 1;
}
static void txt(void *r, int i, const char *s)
{
    if (!s[0]) return;
    int same = 1;
    for (int j = 0; j < 12; j++) { if (dpu_shown[i][j] != s[j]) { same = 0; break; } if (!s[j]) break; }
    if (!same) { op1_text_set(dpu_txt[i], s); int j = 0; for (; j < 11 && s[j]; j++) dpu_shown[i][j] = s[j]; dpu_shown[i][j] = 0; }
    op1_text_draw(dpu_txt[i], r, dpu_ident);
}

void dpu_draw(void *r, int page, int shift, int pad, const dp_extra *x, const int16_t *padw, const dpu_wave *w, uint32_t spb)
{
    dpu_texts_init();
    draw_wave(r, w);
    if (page == DPU_T1) draw_t1(r, w, x, spb); else draw_t2(r, w, x);
    for (int k = 0; k < 4; k++)
        ln(r, Q(8 + 78 * k), Q(108), Q(78 + 78 * k), Q(108), 0x800, dpu_col[k]);
    char a[16], b[16], c[16];
    put(num(put(a, "pad "), pad + 1, 0), "");
    txt(r, DT_PAD, a); txt(r, DT_NAME, "drum+");
    if (shift) txt(r, DT_SHIFT, "shift");
    for (int k = 0; k < 4; k++) {
        dpu_texts(x, padw, page, shift, k, a, b, c);
        txt(r, DT_LAB + k, a); txt(r, DT_VAL + k, b); txt(r, DT_PRS + k, c);
    }
}

#ifndef OP1_HOST
/* ---- firmware glue ---- */
#define SCREEN_TABLE    ((void **)0x01DF4E54)
#define CURRENT_SCREEN  (*(volatile int *)0x018B65C8)
#define DRUM_MODE       (*(volatile int *)0x01DE4C40 == 1)
#define DRUM_SLOT_CUR   (*(volatile int *)0xFF8016D8 & 7)
#define LAST_PAD        (*(volatile int *)0xFF801044)
#define SHIFT_HELD      (*(volatile uint8_t *)0x01DD7F78)
#define PAD_TABLE(s)    ((int16_t *)(0x01AAADB4 + (s) * 0x180))
#define DRUM_TO_PATCH   ((void (*)(void *))0x019FFC50)            /* live pad table -> patch JSON (+ changed) */
#define DRUM_PATCH(s)   ((void *)(0x01DDD9A8 + (s) * 0x38))
#define BEAT_LEN        (*(const volatile int32_t *)0x01AADAD8)
extern dp_extra dp_x[8][24];
extern dp_pre dp_c[8][24];
extern uint8_t dp_isplus[8];
void dp_fw_store_pad(int slot, int pad);                          /* dp_fw.c: op1r.pads[pad] into the patch JSON */

static uint32_t *dpu_scr[2];                                      /* page objects (core, dp_core.c) */
static void *dpu_stock[2];
static uint8_t dpu_ready;
static dpu_wave dpu_w;
static int32_t dpu_acc[4];

static int dpu_slot_plus(void) { return DRUM_MODE && dp_isplus[DRUM_SLOT_CUR]; }
static int dpu_pad(void) { int p = LAST_PAD; return p >= 0 && p < 24 ? p : 0; }

/* the object for slot 0x10 / 0x17 when no editor is open; stock = the boot-time objects */
void *dpu_screen_for(int id)
{
    int i = id == 0x17;
    if (!dpu_ready || (id != 0x10 && id != 0x17)) return 0;
    return dpu_slot_plus() ? (void *)dpu_scr[i] : dpu_stock[i];
}
void dpu_sync(int editor_open_on_10)
{
    if (!dpu_ready) return;
    if (!editor_open_on_10) SCREEN_TABLE[0x10] = dpu_screen_for(0x10);
    SCREEN_TABLE[0x17] = dpu_screen_for(0x17);
}
int dpu_page(void)                                               /* 0 / 1 if a drum+ page is shown, else -1 */
{
    if (!dpu_ready || !dpu_slot_plus()) return -1;
    int id = CURRENT_SCREEN;
    if (id == 0x10 && SCREEN_TABLE[0x10] == (void *)dpu_scr[0]) return DPU_T1;
    if (id == 0x17 && SCREEN_TABLE[0x17] == (void *)dpu_scr[1]) return DPU_T2;
    return -1;
}

static void dpu_changed(int what, int slot, int pad)
{
    if (what & 1) dp_plus_prepare(&dp_x[slot][pad], &dp_c[slot][pad]);
    op1_patch_wait();                                             /* not while a save / type job serializes the patch */
    if (what & 1) dp_fw_store_pad(slot, pad);                    /* JSON: op1r */
    DRUM_TO_PATCH(DRUM_PATCH(slot));                             /* pad words -> JSON, marks the patch changed */
}

/* returns 1 if consumed (else the stock controller gets the turn) */
int dpu_knob(int knob, int delta)
{
    int page = dpu_page();
    if (page < 0 || knob < 0 || knob > 3) return 0;
    int shift = SHIFT_HELD;
    if (dpu_turn_target(page, knob, shift) < 0) return 0;        /* pitch / volume: stock */
    int f = dpu_turn_target(page, knob, shift);
    int unit = (f == 9 || f == 10) ? 2048 : 512;                 /* retrig count / rate: 1 per detent */
    int32_t *acc = &dpu_acc[knob];
    if ((*acc > 0 && delta < 0) || (*acc < 0 && delta > 0)) *acc = 0;
    *acc += delta;
    int st = *acc / unit;
    if (!st) return 1;
    *acc -= st * unit;
    int slot = DRUM_SLOT_CUR, pad = dpu_pad();
    if (dpu_turn(&dp_x[slot][pad], page, knob, shift, st)) dpu_changed(1, slot, pad);
    return 1;
}
int dpu_key(int code, int pressed)                               /* encoder presses 0x3B..0x3E */
{
    int page = dpu_page();
    if (page < 0 || code < 0x3B || code > 0x3E) return 0;
    if (pressed) {
        int slot = DRUM_SLOT_CUR, pad = dpu_pad();
        int what = dpu_press(&dp_x[slot][pad], PAD_TABLE(slot) + pad * 8, page, code - 0x3B);
        dpu_changed(what, slot, pad);
    }
    return 1;
}

static void dpu_init(void *scr) { (void)scr; dpu_texts_init(); }
static void dpu_update(void *scr, int dt)
{
    op1_screen_draw(scr, dt);
    int page = scr == (void *)dpu_scr[1] ? DPU_T2 : DPU_T1, slot = DRUM_SLOT_CUR, pad = dpu_pad();
    const int16_t *pw = PAD_TABLE(slot) + pad * 8;
    uint32_t *obj = ((uint32_t **)0x01CC10CC)[slot];              /* DrumSampler: obj[0] = SampleBuffer */
    if (obj && obj[0]) {
        const int16_t *data = *(const int16_t **)obj[0];
        int32_t a = dp_bound(pw[1], pw[5]) >> 11, b = dp_bound(pw[2], pw[6]) >> 11;
        dpu_wave_update(&dpu_w, data, a, b);
    }
    int32_t l = BEAT_LEN >> 14;
    dpu_draw(op1_raster(), page, SHIFT_HELD, pad, &dp_x[slot][pad], pw, &dpu_w, l > 0 ? (uint32_t)l : 22050);
}
const op1_screen_t SCREEN_drumplus = { dpu_init, dpu_update };

/* boot: the core builds the two page objects (dispatcher.c) and hands them over once the stock ones exist */
/* boot, called by the core at the points where the firmware builds / loads its screens (dispatcher.c): two screen
 * objects of the iter screen class (empty SVG, drawn in code) for ids 0x10 / 0x17, kept out of the table until a
 * drum+ slot is shown (dpu_sync) */
#define SCREEN_CTOR     ((void *(*)(void *, int, const char *))0x01A3BAB0)
#define SVG_LOAD        ((void *(*)(void *))0x01A503D8)
#define ITER_SCREEN_VT  ((const uint32_t *)0x019103FC)
static uint32_t dpu_obj[2][32], dpu_vt[10];
void dpu_ui_build(void)
{
    for (int i = 0; i < 10; i++) dpu_vt[i] = ITER_SCREEN_VT[i];       /* is_visible: id == current */
    dpu_vt[3 * 2 + 1] = (uint32_t)SCREEN_drumplus.update;
    dpu_vt[4 * 2 + 1] = (uint32_t)SCREEN_drumplus.init;
    for (int i = 0; i < 2; i++) {
        int id = i ? 0x17 : 0x10;
        void *saved = SCREEN_TABLE[id];
        SCREEN_CTOR(dpu_obj[i], id, "usbout.svg");
        SCREEN_TABLE[id] = saved;
        dpu_obj[i][3] = (uint32_t)dpu_vt;
        dpu_scr[i] = dpu_obj[i];
    }
}
void dpu_ui_load(void)
{
    for (int i = 0; i < 2; i++) dpu_obj[i][0] = (uint32_t)SVG_LOAD((void *)dpu_obj[i][2]);
    if (!dpu_obj[0][0] || !dpu_obj[1][0]) return;
    SCREEN_drumplus.init(dpu_obj[0]);
    dpu_stock[0] = SCREEN_TABLE[0x10]; dpu_stock[1] = SCREEN_TABLE[0x17];
    dpu_ready = dpu_stock[0] && dpu_stock[1]; dpu_w.key0 = dpu_w.key1 = -1;
}
#endif
