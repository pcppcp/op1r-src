/* sampler+ pages (docs/46, mock-up: display/samplerplus_preview.py): T1 play (screen 6, replaces the stock sampler
 * page), T2 envelope and filter (screen 0x16, replaces the ADSR page), the sample browser (T1 white press: the core's,
 * screens/sbrowse.c). Drawn in code on two screen objects of the iter class (empty SVG), swapped into the screen table
 * while the current synth slot is sampler+ (spu_sync, from the core's se_swap). The region / loop points are edited in
 * the sample editor (synth + scissors), shown read-only here. */
#include "../op1_synth.h"
#include "sp_plus.h"
#include "../drumplus/dp_tables.h"

#define SCREEN_TABLE    ((void **)0x01DF4E54)
#define CURRENT_SCREEN  (*(volatile int *)0x018B65C8)
#define SYNTH_MODE      (*(volatile int *)0x01DE4C40 == 0)
#define SLOT_CUR        (*(volatile int *)0xFF8016D4 & 7)
#define SHIFT_HELD      (*(volatile uint8_t *)0x01DD7F78)
#define SLOT_TYPE       ((const volatile int32_t *)0xFF8016E4)
#define VOICE_ON        ((const volatile uint8_t *)0xFF80105C)
#define BEAT_LEN        (*(const volatile int32_t *)0x01AADAD8)
#define SAMPLER_OBJ(s)  (((uint32_t *const *)0x01DF6F78)[(s) * 2])   /* {SampleBuffer *sb, rate} per slot: obj */
#define PATCH_OBJ(s)    ((void *)(0x01DDD7E8 + (s) * 0x38))
#define PATCH_SET_KNOBS ((void (*)(void *, const int16_t *))0x01A00D14)   /* knob words -> JSON, patch changed */
#define QUEUE_TYPE_JOB  ((void (*)(int, int))0x01940FD0)                /* (selection, slot): synth type job */
#define JV_CGET   ((const void *(*)(const void *, const char *))0x019BEB70)
#define ADSR_W(s)       ((const volatile int16_t *)(0x01B60EF0 + ((s) & 7) * 16))   /* the slot's 8 ADSR words */

extern sp_extra sp_x[8];
extern sp_pre sp_c[8];
extern uint8_t sp_isplus[8], sp_keep_next[8];
void sp_fw_store(int slot);

enum { T1 = 0, T2 = 1 };
#define TS 12
#define Q(v) ((v) << 10)
static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_grey[4] = { 0x6F, 0x71, 0x73, 0xFF },
                     c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF }, c_lilac[4] = { 0xDF, 0xD9, 0xFF, 0xFF },
                     c_pale[4] = { 0xAE, 0xB1, 0xDC, 0xFF }, c_dark[4] = { 0x3A, 0x22, 0x28, 0xFF },
                     c_dim[4] = { 0x3A, 0x3B, 0x3D, 0xFF };
static const uint8_t *const col4[4] = { c_blue, c_green, c_white, c_red };
static void ln(void *r, int x0, int y0, int x1, int y1, int w, const uint8_t *c)
{
    if (x0 == x1 && y0 == y1) return;
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}

/* ---- edits ---- */
/* turn targets per (page, shift, knob): sp_extra field, or -2 = the stock gain word (knob 7) */
static const int8_t turn_field[2][2][4] = {
    { { 16, 12, 20, 19 }, { 18, 13, 17, -2 } },       /* T1: xfade | p env | sync | drive; rnd st | p time | vel>st | gain */
    { { 0, 2, 3, 4 }, { -3, -4, 8, 9 } },             /* T2: attack | decay | sustain | release; play mode | portamento
                                                         (stock words, the playmode controller) | cutoff | reso */
};
static const uint8_t hold_steps[4] = { 0, 40, 64, 90 }, vel_steps[3] = { 0, 64, 127 };

static int32_t acc[4];
static int16_t gain_w;                                 /* gain word being edited (knob 7) */
static void changed(int slot, int extras)
{
    op1_patch_wait();                                             /* not while a save / type job serializes it */
    if (extras) { sp_plus_prepare(&sp_x[slot], &sp_c[slot]); sp_fw_store(slot); }
    PATCH_SET_KNOBS(PATCH_OBJ(slot), op1_synth_knobs(slot));   /* marks the patch changed (JSON saved) */
}

/* ---- browser: the core's (screens/sbrowse.c, sb_*: the sampler presets) ---- */
#define br_open sb_is_open(SB_SAMPLERPLUS)
static const char *jv_str(const void *json, const char *key)       /* a string member, else 0 */
{
    const void *v = JV_CGET(json, key);
    return v && ((const uint8_t *)v)[8] == 4 ? *(const char *const *)v : 0;
}

/* ---- screens ---- */
static uint32_t obj[3][32], vt[10], *scr[3];          /* T1 (6), T2 (0x16), T2 with Shift (playmode slot 0x28) */
static void *stock[3];                                 /* 6, 0x16, 0x28 (playmode: Shift on the ADSR page) */
static uint8_t ready;
static int slot_plus(void) { int s = SLOT_CUR; return SYNTH_MODE && sp_isplus[s] && SLOT_TYPE[s] == 6; }
void *spu_screen_for(int id)
{
    int i = id == 0x16;
    if (!ready || (id != 6 && id != 0x16)) return 0;
    return slot_plus() ? (void *)scr[i] : stock[i];
}
void spu_sync(int editor_open_on_6)
{
    if (!ready) return;
    if (!editor_open_on_6) SCREEN_TABLE[6] = spu_screen_for(6);
    SCREEN_TABLE[0x16] = spu_screen_for(0x16);
    SCREEN_TABLE[0x28] = slot_plus() ? (void *)scr[2] : stock[2];   /* Shift: our T2 page with the play mode */
    if (!slot_plus() && br_open) sb_close();
}
static int page(void)
{
    if (!ready || !slot_plus()) return -1;
    int id = CURRENT_SCREEN;
    if (id == 6 && SCREEN_TABLE[6] == (void *)scr[0]) return T1;
    if (id == 0x16 && SCREEN_TABLE[0x16] == (void *)scr[1]) return T2;
    if (id == 0x28 && SCREEN_TABLE[0x28] == (void *)scr[2]) return T2;   /* Shift held on T2: the playmode slot */
    return -1;
}

int spu_knob(int knob, int delta)
{
    int pg = page();
    if (pg < 0 || knob < 0 || knob > 3) return 0;
    if (br_open && pg != T1) sb_close();                             /* the browser lives on T1 only */
    if (br_open) {                                                   /* browser: white scrolls */
        if (knob == 2) sb_turn(delta);
        return 1;
    }
    int shift = SHIFT_HELD, f = turn_field[pg][shift ? 1 : 0][knob], slot = SLOT_CUR;
    if (f == -3 || f == -4) return 0;                                /* play mode / portamento: the stock controller */
    int unit = f == 20 ? 2048 : 512;                                 /* sync: one step per detent */
    if ((acc[knob] > 0 && delta < 0) || (acc[knob] < 0 && delta > 0)) acc[knob] = 0;
    acc[knob] += delta;
    int st = acc[knob] / unit;
    if (!st) return 1;
    acc[knob] -= st * unit;
    if (f == -2) {                                                   /* stock gain word */
        int16_t *w = (int16_t *)op1_synth_knobs(slot);
        int32_t v = w[7] + st * 256;
        w[7] = (int16_t)(v < 0 ? 0 : v > 32767 ? 32767 : v);
        gain_w = w[7];
        changed(slot, 0);
        return 1;
    }
    int before = sp_extra_get(&sp_x[slot], f);
    if (sp_extra_set(&sp_x[slot], f, before + st) != before) changed(slot, 1);
    return 1;
}

int spu_key(int code, int pressed)
{
    int pg = page();
    if (pg < 0 || (pg != T1 && br_open)) { if (br_open) sb_close(); if (pg < 0) return 0; }
    if (code == 0x06 && pressed && br_open) { sb_close(); return 1; }   /* T1 closes the browser */
    if (code >= 0x07 && code <= 0x09 && pressed && br_open) sb_close();  /* T2..T4 leave it (the key acts as usual) */
    if (code < 0x3B || code > 0x3E) return 0;
    if (!pressed) return 1;
    int k = code - 0x3B, slot = SLOT_CUR;
    sp_extra *x = &sp_x[slot];
    if (br_open) {
        if (k == 2) {                                                 /* white press: load (the stock job, sampler+ kept) */
            int id = sb_pick();
            if (id <= 0) { sp_keep_next[slot] = 1; QUEUE_TYPE_JOB(id, slot); }
        } else if (k == 3) sb_close();
        return 1;
    }
    if (pg == T1) {
        if (k == 0) sp_extra_set(x, 14, !x->loop_mode);
        else if (k == 1) sp_extra_set(x, 15, !x->loop_rel);
        else if (k == 2) { sb_open(SB_SAMPLERPLUS); return 1; }
        else {                                                        /* reverse: knob word 4 bit 14 */
            int16_t *w = (int16_t *)op1_synth_knobs(slot);
            w[4] = (int16_t)(w[4] >= 0x4000 ? 12000 : 20000);
            changed(slot, 0);
            return 1;
        }
    } else {
        static const int8_t mod_steps[5] = { -127, -64, 0, 64, 127 };   /* Shift + white / red: env > cut, vel > cut */
        if (SHIFT_HELD && k >= 2) {
            int f = k == 2 ? 10 : 11, v = sp_extra_get(x, f), i = 0;
            while (i < 4 && mod_steps[i] <= v) i++;                  /* the next step up, wrapping */
            sp_extra_set(x, f, mod_steps[i == 4 && mod_steps[4] <= v ? 0 : i]);
        }
        else if (k == 0) sp_extra_set(x, 5, !x->curve_exp);
        else if (k == 1) { int i = 0; while (i < 3 && hold_steps[i] < x->hold) i++; sp_extra_set(x, 1, hold_steps[(i + 1) & 3]); }
        else if (k == 2) sp_extra_set(x, 7, (x->filter + 1) & 3);
        else { int i = 0; while (i < 2 && vel_steps[i] < x->vel_amt) i++; sp_extra_set(x, 6, vel_steps[i == 2 ? 0 : i + 1]); }
    }
    changed(slot, 1);
    return 1;
}

/* ---- text ---- */
enum { TX_TITLE, TX_SHIFT, TX_LAB, TX_VAL = TX_LAB + 4, TX_PRS = TX_VAL + 4, TX_N = TX_PRS + 4 };
static uint32_t tx[TX_N][OP1_TEXT_SIZE / 4];
static char shown[TX_N][14];
static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
static uint8_t tx_made;
static void tx_init(void)
{
    if (tx_made) return;
    op1_text_ctor(tx[TX_TITLE], Q(8), 4, TS, c_white);
    op1_text_ctor(tx[TX_SHIFT], Q(120), 4, TS, c_grey);
    for (int k = 0; k < 4; k++) {
        op1_text_ctor(tx[TX_LAB + k], Q(8 + 78 * k), 114, TS, col4[k]);
        op1_text_ctor(tx[TX_VAL + k], Q(8 + 78 * k), 127, TS, c_white);
        op1_text_ctor(tx[TX_PRS + k], Q(8 + 78 * k), 94, TS, col4[k]);   /* press states: row above the bars */
    }
    tx_made = 1;
}
static void txt(void *r, int i, const char *s)
{
    if (!s || !s[0]) return;
    int same = 1;
    for (int j = 0; j < 14; j++) { if (shown[i][j] != s[j]) { same = 0; break; } if (!s[j]) break; }
    if (!same) { op1_text_set(tx[i], s); int j = 0; for (; j < 13 && s[j]; j++) shown[i][j] = s[j]; shown[i][j] = 0; }
    op1_text_draw(tx[i], r, ident);
}
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *num(char *p, int v, int sign)
{
    char b[12]; int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; } else if (sign && v) *p++ = '+';
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = b[--n];
    *p = 0; return p;
}
static void ms(char *p, uint32_t samples)
{
    uint32_t m = samples * 93 >> 12;                                 /* / 44.1 */
    if (m >= 1000) { p = num(p, (int)(m / 1000), 0); *p++ = '.'; p = num(p, (int)(m % 1000 / 100), 0); put(p, "s"); }
    else put(num(p, (int)m, 0), "ms");
}
static const char *const sync_names[7] = { "off", "1/4", "1/2", "1 bar", "2 bars", "4 bars", "8 bars" };
static const char *const ftype[4] = { "off", "lp", "hp", "bp" };

static void texts(int pg, int shift, int k, int slot, char *lab, char *val, char *prs)
{
    static const char *const lab_t1[2][4] = { { "xfade", "p env", "sync", "drive" }, { "rnd st", "p time", "vel>st", "gain" } };
    static const char *const lab_t2[2][4] = { { "attack", "decay", "sustain", "release" }, { "play", "porta", "cutoff", "reso" } };
    const sp_extra *x = &sp_x[slot];
    put(lab, (pg == T1 ? lab_t1 : lab_t2)[shift ? 1 : 0][k]);
    int f = turn_field[pg][shift ? 1 : 0][k];
    val[0] = prs[0] = 0;
    if (f == -2) put(num(val, op1_synth_knobs(slot)[7] * 100 / 32767, 0), "%");
    else if (f == -3) {                                               /* as the playmode screen (0x01A0DAD8) */
        static const char *const pm[4] = { "poly", "mono", "legato", "unison" };
        int32_t i = ((ADSR_W(slot)[4] - 2048) * 3 + 4092) / 8184;
        put(val, pm[i < 0 ? 0 : i > 3 ? 3 : i]);
    } else if (f == -4) {
        int32_t v = (ADSR_W(slot)[5] - 64 + 64) / 128;               /* 0 off .. 128 max */
        if (v <= 0) put(val, "off"); else if (v >= 128) put(val, "max"); else num(val, v, 0);
    }
    else if (f == 16) ms(val, (uint32_t)x->xfade * 32);
    else if (f == 20) put(val, sync_names[x->sync]);
    else if (f == 12 || f == 10 || f == 11) num(val, sp_extra_get(x, f), 1);
    else if (f == 13 || f == 0 || f == 2 || f == 4) ms(val, sp_time_samples((uint8_t)sp_extra_get(x, f)));
    else if (f == 8) { int hz = dp_cutoff_hz[x->cutoff]; if (hz >= 1000) { char *p = num(val, hz / 1000, 0); *p++ = '.'; put(num(p, hz % 1000 / 100, 0), "k"); } else num(val, hz, 0); }
    else if (f == 3) put(num(val, x->sustain * 100 / 127, 0), "%");
    else num(val, sp_extra_get(x, f), 0);
    if (shift) {                                                      /* Shift + white / red press: the filter's mod */
        if (pg == T2 && k == 2) put(num(put(prs, "env "), x->fenv, 1), "");
        if (pg == T2 && k == 3) put(num(put(prs, "vel "), x->vel_cutoff, 1), "");
        return;
    }
    if (pg == T1) {
        const int16_t *w = op1_synth_knobs(slot);
        put(prs, k == 0 ? (x->loop_mode ? "pong" : "fwd") : k == 1 ? (x->loop_rel ? "rel" : "") : k == 2 ? "file" : (w[4] >= 0x4000 ? "rev" : ""));
    } else {
        int h = 0; while (h < 3 && hold_steps[h] < x->hold) h++;
        static const char *const hs[4] = { "", "hold", "hold2", "hold3" };
        put(prs, k == 0 ? (x->curve_exp ? "exp" : "lin") : k == 1 ? hs[h] : k == 2 ? ftype[x->filter & 3] : x->vel_amt >= 127 ? "vel" : x->vel_amt ? "vel/2" : "");
    }
}

/* ---- drawing ---- */
#define WX0 14
#define WCOLS 146                                     /* 2 px each: x 14 .. 306 */
#define WMID 54
static int32_t vlen = 264600;                         /* samples shown: from 0 to past the region / loop end */
static int32_t pos_x(int32_t pos)                     /* buffer position (Q12, incl. guard) -> x Q10 */
{
    int32_t smp = pos >> 12;
    if (smp < 0) smp = 0; else if (smp > vlen) smp = vlen;
    return Q(WX0) + ((smp * 1168 / vlen) << 8);        /* 292 px over vlen samples */
}
static void draw_t1(void *r, int slot)
{
    const int16_t *w = op1_synth_knobs(slot);
    const sp_extra *x = &sp_x[slot];
    uint32_t *o = SAMPLER_OBJ(slot);
    int32_t st = sp_bound(w[0], 0), en = sp_bound(w[3], 0), ls = sp_bound(w[1], w[5]), le = sp_bound(w[2], w[6]);
    int32_t far = (en > le ? en : le) >> 12;
    vlen = far + far / 8 + 1024; if (vlen > 264600) vlen = 264600;
    if (o && o[0]) {                                                  /* peaks: max per 256 samples (SampleBuffer +4) */
        const int16_t *pk = (const int16_t *)(o[0] + 4);
        for (int c = 0; c < WCOLS; c++) {
            int m = 0;
            int32_t s0 = vlen / WCOLS * c, s1 = s0 + vlen / WCOLS;
            for (int i = s0 >> 8, e = s1 >> 8; i <= e && i < 0x813; i++) if (pk[i] > m) m = pk[i];   /* max per 256 samples */
            int32_t a = m * 28 >> 15, xq = Q(WX0 + 2 * c) + 0x200;
            if (a < 1) a = 1;
            int32_t smp = s0;
            ln(r, xq, Q(WMID - a), xq, Q(WMID + a), 0x400, smp >= (st >> 12) && smp <= (en >> 12) ? c_grey : c_dim);
        }
    }
    int32_t xs = pos_x(st), xe = pos_x(en), xl = pos_x(ls), xr = pos_x(le);
    ln(r, xs, Q(WMID - 32), xs, Q(WMID + 32), 0x400, c_white);
    ln(r, xe, Q(WMID - 32), xe, Q(WMID + 32), 0x400, c_white);
    if (w[0] < w[2] && le > ls) {                                     /* loop (lilac), crossfade, ping-pong */
        ln(r, xl, Q(WMID - 32), xl, Q(WMID + 32), 0x400, c_lilac);
        ln(r, xr, Q(WMID - 32), xr, Q(WMID + 32), 0x400, c_lilac);
        ln(r, xl, Q(WMID + 32), xr, Q(WMID + 32), 0x800, c_lilac);
        if (x->xfade && !x->loop_mode) {
            int32_t xf = pos_x(le - ((int32_t)x->xfade * 32 << 12)) ; if (xf < xl) xf = xl;
            if (xr - xf < 0x400) xf = xr - 0x400;
            ln(r, xf, Q(WMID + 28), xr, Q(WMID + 28), 0xC00, c_blue);
        }
        if (x->loop_mode && xr - xl > Q(12)) {
            int32_t y = Q(WMID + 38);
            ln(r, xl + Q(2), y, xr - Q(2), y, 0x400, c_blue);
            ln(r, xl + Q(2), y, xl + Q(6), y - Q(3), 0x400, c_blue); ln(r, xl + Q(2), y, xl + Q(6), y + Q(3), 0x400, c_blue);
            ln(r, xr - Q(2), y, xr - Q(6), y - Q(3), 0x400, c_blue); ln(r, xr - Q(2), y, xr - Q(6), y + Q(3), 0x400, c_blue);
        }
    }
    for (int v = 0; v < 6; v++)                                       /* playing voices */
        if (VOICE_ON[v]) {
            int32_t p = *(const volatile int32_t *)(0xFFB00008 + v * 0x10), xv = pos_x(p);
            ln(r, xv, Q(WMID - 28), xv, Q(WMID + 28), 0x400, c_red);
        }
    if (x->pitch_env) {                                               /* pitch envelope from the start (green) */
        int32_t len = 4 + x->pitch_time / 3, h = x->pitch_env * 26 / 48;
        ln(r, xs, Q(WMID) - Q(h), xs + Q(len), Q(WMID), 0x600, c_green);
    }
    ln(r, Q(308), Q(86), Q(308), Q(46), 0xC00, c_dark);              /* drive meter */
    if (x->drive) ln(r, Q(308), Q(86), Q(308), Q(86) - Q(x->drive * 40 / 127), 0xC00, c_red);
}
static int32_t tw(int t, int span) { return 2 + t * span / 127; }   /* parameter (log time) -> px */
static void draw_t2(void *r, int slot)
{
    const sp_extra *x = &sp_x[slot];
    int base = 88, top = 26, ys = base - (base - top) * x->sustain / 127;
    int xa = 20 + tw(x->attack, 46), xh = xa + (x->hold ? tw(x->hold, 30) : 0), xd = xh + tw(x->decay, 50);
    int xu = xd + 60, xr = xu + tw(x->release, 50);
    ln(r, Q(20), Q(base), Q(xa), Q(top), 0x600, c_blue);
    if (xh > xa) ln(r, Q(xa), Q(top), Q(xh), Q(top), 0x600, c_lilac);
    static const uint8_t ex[9] = { 0, 115, 185, 223, 243, 252, 254, 255, 255 };   /* 1 - e^-4t, /255 */
    for (int i = 0; i < 8; i++) {                                     /* decay to sustain, release to 0 */
        int f0 = x->curve_exp ? ex[i] : i * 32, f1 = x->curve_exp ? ex[i + 1] : (i + 1) * 32;
        ln(r, Q(xh + (xd - xh) * i / 8), Q(top + (ys - top) * f0 / 256), Q(xh + (xd - xh) * (i + 1) / 8), Q(top + (ys - top) * f1 / 256), 0x600, c_green);
        ln(r, Q(xu + (xr - xu) * i / 8), Q(ys + (base - ys) * f0 / 256), Q(xu + (xr - xu) * (i + 1) / 8), Q(ys + (base - ys) * f1 / 256), 0x600, c_red);
    }
    ln(r, Q(xd), Q(ys), Q(xu), Q(ys), 0x600, c_white);
    ln(r, Q(xu), Q(ys - 6), Q(xu), Q(ys + 6), 0x400, c_grey);
    if (x->filter) {                                                  /* filter response (right): base / envelope peak */
        for (int pass = 0; pass < 2; pass++) {
            int ci = x->cutoff + (pass ? x->fenv : 0); ci = ci < 0 ? 0 : ci > 127 ? 127 : ci;
            int fc = 236 + ci * 70 / 127, bump = x->resonance * 10 / 127, prevx = 236, prevy = 0;
            for (int i = 0; i <= 14; i++) {
                int px = 236 + i * 5, d = px - fc, y;
                if (x->filter == 1) y = d < 0 ? 0 : -d / 2;
                else if (x->filter == 2) y = d > 0 ? 0 : d / 2;
                else y = -(d < 0 ? -d : d) / 2;
                if (d > -6 && d < 6) y += bump;
                y = 50 - (y < -20 ? -20 : y);
                if (i) ln(r, Q(prevx), Q(prevy), Q(px), Q(y), pass ? 0x600 : 0x400, pass ? c_pale : c_grey);
                prevx = px; prevy = y;
            }
        }
    }
}
static void update(void *s, int dt)
{
    op1_screen_draw(s, dt);
    tx_init();
    void *r = op1_raster();
    int pg = s == (void *)scr[0] ? T1 : T2, slot = SLOT_CUR, shift = SHIFT_HELD;
    if (pg == T1 && br_open) { sb_page(); return; }                 /* the core's browser page */
    for (int k = 0; k < 4; k++) ln(r, Q(8 + 78 * k), Q(110), Q(78 + 78 * k), Q(110), 0x800, col4[k]);
    const char *name = jv_str((const uint8_t *)PATCH_OBJ(slot) + 4, "name");
    txt(r, TX_TITLE, name && name[0] ? name : "sampler");
    if (shift) txt(r, TX_SHIFT, "shift");
    if (pg == T1) draw_t1(r, slot); else draw_t2(r, slot);
    char a[16], b[16], c[16];
    for (int k = 0; k < 4; k++) {
        texts(pg, shift, k, slot, a, b, c);
        txt(r, TX_LAB + k, a); txt(r, TX_VAL + k, b); txt(r, TX_PRS + k, c);
    }
}
static void init(void *s) { (void)s; tx_init(); }

/* boot (core calls at the screen build / load points, as drum+) */
#define SCREEN_CTOR     ((void *(*)(void *, int, const char *))0x01A3BAB0)
#define SVG_LOAD        ((void *(*)(void *))0x01A503D8)
#define ITER_SCREEN_VT  ((const uint32_t *)0x019103FC)
void spu_ui_build(void)
{
    for (int i = 0; i < 10; i++) vt[i] = ITER_SCREEN_VT[i];
    vt[3 * 2 + 1] = (uint32_t)update;
    vt[4 * 2 + 1] = (uint32_t)init;
    for (int i = 0; i < 3; i++) {                                    /* (a screen shows under its own id only) */
        int id = i == 2 ? 0x28 : i ? 0x16 : 6;
        void *saved = SCREEN_TABLE[id];
        SCREEN_CTOR(obj[i], id, "usbout.svg");
        SCREEN_TABLE[id] = saved;
        obj[i][3] = (uint32_t)vt;
        scr[i] = obj[i];
    }
}
void spu_ui_load(void)
{
    for (int i = 0; i < 3; i++) obj[i][0] = (uint32_t)SVG_LOAD((void *)obj[i][2]);
    if (!obj[0][0] || !obj[1][0] || !obj[2][0]) return;
    stock[0] = SCREEN_TABLE[6]; stock[1] = SCREEN_TABLE[0x16]; stock[2] = SCREEN_TABLE[0x28];
    ready = stock[0] && stock[1] && stock[2];
}
