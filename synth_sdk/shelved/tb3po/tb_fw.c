/* "tb3po" sequencer plugin: TB-3PO (O_C Phazerville applet by Logarhythm / djphazer, MIT; tb3po/tb_core.c) on the
 * OP-1. A TB-303 style line from a seed: gates, accents, slides, octave jumps, pitches from a scale above a root.
 *   blue  : density -7..+7            Shift: transpose -7..+7 scale degrees
 *   green : length 1..32 steps         Shift: slides on / off
 *   white : scale                      Shift: rate 1/8, 1/16, 1/32
 *   red   : seed: cw a new one, ccw the previous ones (8 kept)     press: play / stop
 *   keyboard on the page: the root (synth: the key's note; drum: the base pad), consumed
 * Clock: the player tick (96 per beat). Gates: half a step, as on a 303; an accent plays at velocity 127, others 90.
 * A slide holds the note into the next step (its note on comes first, then the old note's off): a legato / mono
 * synth glides there (the OP-1's portamento), as the 303's slide. Drum mode: the pitch picks a pad (base pad +
 * semitones, wrapped into 0..23). Stopped by default (not saved); MIDI START restarts at step 0.
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE); the generator after TB-3PO (MIT). */
#include "../op1_synth.h"
#include "../seq/seq_text.h"
#include "tb_core.h"
#include "tb_fw.h"

#define VEL_ACC 127
#define VEL_NORM 90
static const uint8_t rate_ticks[3] = { 48, 24, 12 };         /* 1/8, 1/16, 1/32 of a 96-tick beat */
static const char *const rate_name[3] = { "1/8", "1/16", "1/32" };

tb_bank tb_b[2];
typedef struct { int16_t on_note, held_note; uint8_t off_at, init; uint32_t tick; uint8_t restart; } tb_run;
static tb_run tb_r[2];

#ifdef OP1_HOST
void host_tb_note(int m, int note, int vel, int on);
#define TB_NOTE_ON(m, note, key, vel) ((void)(key), host_tb_note((m), (note), (vel), 1))
#define TB_NOTE_OFF(m, note)          host_tb_note((m), (note), 0, 0)
#define TB_CHANGED()                  ((void)0)
int tb_host_oct, tb_host_on_page = 1;
static int tb_oct(int m) { (void)m; return tb_host_oct; }
#define ON_PAGE() (tb_host_on_page)
#else
int  eu_fw_note_on(int pos, int note, int key, int vel);   /* seq/seq_core.c: VoiceCtl note on / off */
void eu_fw_note_off(int midi);
#define TB_NOTE_ON(m, note, key, vel) eu_fw_note_on((m), (note), (key), (vel))
#define TB_NOTE_OFF(m, note)          eu_fw_note_off(note)
static void tb_changed(void);
#define TB_CHANGED()                  tb_changed()
static int tb_oct(int m)
{
    return m ? ((const int *)0x01DFE500)[*(volatile int *)0xFF8016D8 & 7] : ((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7];
}
#define ON_PAGE() (*(volatile int *)0x018B65C8 == 0x2D)
#endif

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static tb_run *run(int m)
{
    tb_run *r = &tb_r[m & 1];
    if (!r->init) {
        r->init = 1; r->on_note = r->held_note = -1;
        tb_bank *b = &tb_b[m & 1];
        uint8_t *p = (uint8_t *)b;
        for (unsigned i = 0; i < sizeof *b; i++) p[i] = 0;
        b->pat.seed = (uint16_t)(0x303 + m); b->pat.density = 10; b->pat.scale = 0;
        b->length = 16; b->slides = 1; b->rate = 1; b->root = m ? 0 : 36;
        tb_generate(&b->pat);
    }
    return r;
}
static void note_off(int m)
{
    tb_run *r = &tb_r[m & 1];
    if (r->on_note >= 0) { TB_NOTE_OFF(m, r->on_note); r->on_note = -1; }
}
void tb_all_off(int m) { run(m); note_off(m); if (tb_r[m & 1].held_note >= 0) { TB_NOTE_OFF(m, tb_r[m & 1].held_note); tb_r[m & 1].held_note = -1; } }

static int step_note(int m, int s, int *key)                 /* the midi note (synth) / pad key (drum) of step s */
{
    tb_bank *b = &tb_b[m & 1];
    int st = tb_semitones(&b->pat, s, b->transpose);
    if (m) { int k = ((b->root + st) % 24 + 24) % 24; *key = k; return k + 0x35 + tb_oct(1) * 12; }
    *key = 0;
    return b->root + st;
}

void tb_tick(int m)                                          /* 1/96 beat */
{
    tb_run *r = run(m);
    tb_bank *b = &tb_b[m & 1];
    if (r->on_note >= 0 && r->off_at && !--r->off_at) note_off(m);
    if (!b->playing) return;
    int per = rate_ticks[b->rate % 3];
    if (r->restart) { r->restart = 0; r->tick = 0; b->step = 0; }
    if (r->tick % (uint32_t)per == 0) {
        int s = b->step, prev = (s + b->length - 1) % b->length, slid = b->slides && r->on_note >= 0 && tb_slide(&b->pat, prev);
        if (tb_gate(&b->pat, s) || slid) {
            int key, note = step_note(m, s, &key);
            if (note >= 0 && note <= 127) {
                int old = r->on_note;
                if (!slid || old != note) {                  /* a slide: the new note first, then the old one off */
                    TB_NOTE_ON(m, note, key, tb_accent(&b->pat, s) ? VEL_ACC : VEL_NORM);
                    if (old >= 0) TB_NOTE_OFF(m, old);
                }
                r->on_note = (int16_t)note;
                r->off_at = (uint8_t)(b->slides && tb_slide(&b->pat, s) ? 0 : per / 2);   /* slide: held into the next */
            }
        } else note_off(m);
        b->step = (uint8_t)((s + 1) % b->length);
    }
    r->tick++;
}
void tb_restart(int m) { run(m)->restart = 1; }

/* ---- controls ---- */
static void reseed(tb_bank *b, int dir)
{
    if (dir > 0) {                                            /* new: the current one into the history */
        for (int i = TB_HIST - 1; i > 0; i--) b->hist[i] = b->hist[i - 1];
        b->hist[0] = b->pat.seed;
        if (b->nhist < TB_HIST) b->nhist++;
        static uint32_t x = 0x2545F491u;
#ifndef OP1_HOST
        { uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c)); x ^= c; }
#endif
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        b->pat.seed = (uint16_t)(x >> 8);
    } else if (b->nhist) {                                    /* back */
        b->pat.seed = b->hist[0];
        for (int i = 0; i + 1 < TB_HIST; i++) b->hist[i] = b->hist[i + 1];
        b->nhist--;
    }
}
static int32_t tb_acc[2][4];
void tb_turn(int m, int knob, int delta, int shift)
{
    run(m);
    if (knob < 0 || knob > 3) return;
    int32_t *a = &tb_acc[m & 1][knob];
    if ((*a > 0 && delta < 0) || (*a < 0 && delta > 0)) *a = 0;
    *a += delta;
    int st = *a / 1024;
    *a -= st * 1024;
    if (!st) return;
    tb_bank *b = &tb_b[m & 1];
    int regen = 0;
    if (!shift) switch (knob) {
        case 0: b->pat.density = (uint8_t)clampi(b->pat.density + st, 0, 14); regen = 1; break;
        case 1: b->length = (uint8_t)clampi(b->length + st, 1, TB_STEPS); if (b->step >= b->length) b->step = 0; break;
        case 2: b->pat.scale = (uint8_t)clampi(b->pat.scale + st, 0, TB_SCALES - 1); regen = 1; break;
        default: for (int i = st < 0 ? -st : st; i--; ) reseed(b, st); regen = 1; break;
    } else switch (knob) {
        case 0: b->transpose = (int8_t)clampi(b->transpose + st, -7, 7); break;
        case 1: b->slides = st > 0; break;
        case 2: b->rate = (uint8_t)clampi(b->rate + st, 0, 2); break;
        default: break;
    }
    if (regen) tb_generate(&b->pat);
    TB_CHANGED();
}
void tb_press(int m, int knob)
{
    run(m);
    if (knob != 3) return;
    tb_bank *b = &tb_b[m & 1];
    b->playing = !b->playing;
    if (b->playing) tb_restart(m); else tb_all_off(m);
}
/* the keyboard on the page sets the root (consumed); elsewhere keys play as usual */
int tb_key_down_from(int m, int key, uint32_t caller)
{
    run(m);
    if (key < 0 || key > 23 || !ON_PAGE()) return 0;
    tb_bank *b = &tb_b[m & 1];
    b->root = (uint8_t)(m ? key : clampi(op1_seq_key_note(0, key, tb_oct(0), caller), 0, 127));
    TB_CHANGED();
    return 1;
}

/* ---- persistence: "seed hi,seed lo,density,scale,length,transpose + 7,slides,rate,root" (sq_join: 0..9999) ---- */
int tb_serialize(int m, char *out, int n)
{
    run(m);
    const tb_bank *b = &tb_b[m & 1];
    int v[9] = { b->pat.seed >> 8, b->pat.seed & 255, b->pat.density, b->pat.scale, b->length, b->transpose + 7, b->slides, b->rate, b->root };
    return sq_join(out, n, v, 9);
}
int tb_parse(int m, const char *str, int len)
{
    int v[9];
    if (sq_split(str, len, v, 9) != 9) return 0;
    run(m);
    tb_bank *b = &tb_b[m & 1];
    b->pat.seed = (uint16_t)((v[0] & 255) << 8 | (v[1] & 255)); b->pat.density = (uint8_t)clampi(v[2], 0, 14);
    b->pat.scale = (uint8_t)clampi(v[3], 0, TB_SCALES - 1); b->length = (uint8_t)clampi(v[4], 1, TB_STEPS);
    b->transpose = (int8_t)clampi(v[5] - 7, -7, 7); b->slides = v[6] != 0;
    b->rate = (uint8_t)clampi(v[7], 0, 2); b->root = (uint8_t)clampi(v[8], 0, m ? 23 : 127);
    if (b->step >= b->length) b->step = 0;
    tb_generate(&b->pat);
    return 1;
}

/* ---- the page (GUI thread) ----
 * The pattern as a 303 step row: 16 steps a row (two rows from 17 steps), a box per step: filled = gate (red: accent),
 * a bar over the gap to the next = slide, a tick above / below = octave up / down, the playing step framed white.
 * Above: a one-octave keyboard with the playing note, the root's name and the seed. Bottom: the four values. */
static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     c_grey[4] = { 0x6F, 0x71, 0x73, 0xFF }, c_dim[4] = { 0x2C, 0x2D, 0x30, 0xFF },
                     c_lilac[4] = { 0xAE, 0xB1, 0xDC, 0xFF };
static const uint8_t *const col4[4] = { c_blue, c_green, c_white, c_red };
#define Q(v) OP1_Q10(v)
static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{ op1_pt a = { x0, y0 }, b = { x1, y1 }; op1_raster_line(r, &a, &b, w, c); }
enum { TX_LAB, TX_VAL = 4, TX_ROOT = 8, TX_SEED, TX_N };
static uint32_t tx[TX_N][OP1_TEXT_SIZE / 4];
static char shown[TX_N][14];
static uint8_t made;
static void txt(void *r, int i, const char *s)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    int same = 1;
    for (int j = 0; j < 14; j++) { if (shown[i][j] != s[j]) { same = 0; break; } if (!s[j]) break; }
    if (!same) { op1_text_set(tx[i], s); int j = 0; for (; j < 13 && s[j]; j++) shown[i][j] = s[j]; shown[i][j] = 0; }
    op1_text_draw(tx[i], r, ident);
}
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *num(char *p, int v, int sign)
{
    if (v < 0) { *p++ = '-'; v = -v; } else if (sign && v) *p++ = '+';
    char d[6]; int n = 0; do d[n++] = (char)('0' + v % 10); while ((v /= 10) && n < 5);
    while (n) *p++ = d[--n];
    *p = 0; return p;
}
static const char *const nn[12] = { "c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b" };
static const int8_t kb_black[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
#define SX0 16                                               /* step row: 16 boxes from x 16, 18 px apart */
#define SDX 18
#define SY0 54                                               /* first row's box centre */
#define SDY 26
#define KX0 112                                              /* keyboard: 7 white keys 14 px wide */
#define KY0 10
void tb_draw(void *r, int m, int shift)
{
    tb_bank *b = &tb_b[m & 1]; tb_run *rr = run(m);
    if (!made) {
        for (int k = 0; k < 4; k++) {
            op1_text_ctor(tx[TX_LAB + k], Q(8 + 78 * k), 114, 12, col4[k]);
            op1_text_ctor(tx[TX_VAL + k], Q(8 + 78 * k), 127, 12, c_white);
        }
        op1_text_ctor(tx[TX_ROOT], Q(16), 12, 14, c_white);
        op1_text_ctor(tx[TX_SEED], Q(232), 12, 12, c_lilac);
        made = 1;
    }
    int playing = b->playing, cur = playing ? (b->step + b->length - 1) % b->length : -1;   /* the step that sounded */
    int rows = b->length > 16 ? 2 : 1;
    for (int s = 0; s < (rows == 2 ? TB_STEPS : 16); s++) {
        int x = SX0 + SDX * (s & 15), y = SY0 + SDY * (s >> 4), in = s < b->length;
        int g = tb_gate(&b->pat, s), a = tb_accent(&b->pat, s);
        const uint8_t *c = !in ? c_dim : g ? (a ? c_red : c_blue) : c_grey;
        if (g && in) ln(r, Q(x - 6), Q(y), Q(x + 6), Q(y), Q(12), c);                  /* filled box */
        else {                                                                         /* outline */
            ln(r, Q(x - 6), Q(y - 6), Q(x + 6), Q(y - 6), 0x400, c); ln(r, Q(x - 6), Q(y + 6), Q(x + 6), Q(y + 6), 0x400, c);
            ln(r, Q(x - 6), Q(y - 6), Q(x - 6), Q(y + 6), 0x400, c); ln(r, Q(x + 6), Q(y - 6), Q(x + 6), Q(y + 6), 0x400, c);
        }
        if (!in) continue;
        if (b->slides && tb_slide(&b->pat, s) && (s & 15) != 15 && s + 1 < b->length)   /* slide: across the gap */
            ln(r, Q(x + 6), Q(y), Q(x + SDX - 6), Q(y), 0x600, c_green);
        int up = b->pat.oct_up >> s & 1, dn = b->pat.oct_down >> s & 1;
        if (up) ln(r, Q(x - 3), Q(y - 9), Q(x + 3), Q(y - 9), 0x500, c_white);
        if (dn) ln(r, Q(x - 3), Q(y + 9), Q(x + 3), Q(y + 9), 0x500, c_white);
        if (s == cur) {                                                                /* playing: a white frame */
            ln(r, Q(x - 8), Q(y - 8), Q(x + 8), Q(y - 8), 0x500, c_white); ln(r, Q(x - 8), Q(y + 8), Q(x + 8), Q(y + 8), 0x500, c_white);
            ln(r, Q(x - 8), Q(y - 8), Q(x - 8), Q(y + 8), 0x500, c_white); ln(r, Q(x + 8), Q(y - 8), Q(x + 8), Q(y + 8), 0x500, c_white);
        }
    }
    /* keyboard: the playing note's pitch class lit (blue, red on an accent) */
    int pc = -1, acc = 0;
    if (cur >= 0 && rr->on_note >= 0) { int k; pc = ((step_note(m, cur, &k) - (m ? 0x35 : 0)) % 12 + 12) % 12; acc = tb_accent(&b->pat, cur); }
    for (int i = 0, w = 0; i < 12; i++) {
        if (kb_black[i]) continue;
        int x = KX0 + 14 * w++;
        ln(r, Q(x + 1), Q(KY0 + 13), Q(x + 13), Q(KY0 + 13), Q(26), i == pc ? (acc ? c_red : c_blue) : c_grey);
    }
    for (int i = 0, w = 0; i < 12; i++) {
        if (!kb_black[i]) { w++; continue; }
        int x = KX0 + 14 * w;
        ln(r, Q(x - 4), Q(KY0 + 8), Q(x + 4), Q(KY0 + 8), Q(16), i == pc ? (acc ? c_red : c_blue) : c_dim);
    }
    char t[16], *p;
    if (m) { p = put(t, "pad "); num(p, b->root + 1, 0); }
    else { p = put(t, nn[b->root % 12]); num(p, b->root / 12 - 1, 0); }
    txt(r, TX_ROOT, t);
    { static const char hx[] = "0123456789abcdef"; p = put(t, "seed ");
      for (int i = 3; i >= 0; i--) *p++ = hx[b->pat.seed >> (4 * i) & 15];
      *p = 0; txt(r, TX_SEED, t); }
    for (int i = 0; i < 4; i++) ln(r, Q(8 + 78 * i), Q(110), Q(78 + 78 * i), Q(110), 0x800, col4[i]);
    if (!shift) {
        txt(r, TX_LAB, "density"); num(t, (int)b->pat.density - 7, 1); txt(r, TX_VAL, t);
        txt(r, TX_LAB + 1, "length"); num(t, b->length, 0); txt(r, TX_VAL + 1, t);
        txt(r, TX_LAB + 2, "scale"); txt(r, TX_VAL + 2, tb_scale_name[b->pat.scale % TB_SCALES]);
        txt(r, TX_LAB + 3, playing ? "playing" : "stopped"); txt(r, TX_VAL + 3, "new seed");
    } else {
        txt(r, TX_LAB, "transpose"); num(t, b->transpose, 1); txt(r, TX_VAL, t);
        txt(r, TX_LAB + 1, "slides"); txt(r, TX_VAL + 1, b->slides ? "on" : "off");
        txt(r, TX_LAB + 2, "rate"); txt(r, TX_VAL + 2, rate_name[b->rate % 3]);
        txt(r, TX_LAB + 3, ""); txt(r, TX_VAL + 3, "");
    }
}

#ifndef OP1_HOST
/* ======================= firmware player (type 3 + tb_active[mode], as grids / peal) ======================= */
#define TB_MODE()  (*(volatile int *)0x01DE4C40 == 1)
#define SHIFT_HELD (*(volatile uint8_t *)0x01DD7F78)
static int tb_nop(void *self) { (void)self; return 0; }
static int tb_pkey_down(void *self, int key) { (void)self; return tb_key_down_from(TB_MODE(), key, OP1_SEQ_CALLER()); }
static void tb_pkey_up(void *self, int key) { (void)self; (void)key; }
static void tb_reset(void *self) { (void)self; tb_all_off(TB_MODE()); }
static void tb_ptick(void *self, int counter) { (void)self; (void)counter; tb_tick(TB_MODE()); }
static void tb_pstop(void *self) { (void)self; tb_all_off(0); tb_all_off(1); }
static const uint32_t tb_vt[16] = {
    0, 0,  0, (uint32_t)tb_nop,  0, (uint32_t)tb_pkey_down,  0, (uint32_t)tb_pkey_up,
    0, (uint32_t)tb_reset,  0, (uint32_t)tb_ptick,  0, (uint32_t)tb_nop,  0, (uint32_t)tb_pstop };
static struct { const uint32_t *vt; uint32_t pad[7]; } tb_player = { tb_vt, { 0 } };
volatile uint8_t tb_active[2];

static char tb_sql[200];
static sq_store_t tb_store = { .name = "tb3po", .ser = tb_serialize, .parse = tb_parse,
                               .sql = tb_sql, .cap = sizeof tb_sql, .maxlen = 80 };
static void tb_changed(void) { sq_store_dirty(&tb_store); }
static void *tb_player_get(void) { run(0); run(1); sq_store_load(&tb_store); return &tb_player; }

static void tb_scr_init(void *scr) { (void)scr; }
static void tb_scr_update(void *scr, int dt) { op1_screen_draw(scr, dt); tb_draw(op1_raster(), TB_MODE(), SHIFT_HELD); }
const op1_screen_t SCREEN_tb3po = { tb_scr_init, tb_scr_update };

static int tb_pkey(int m, int code, int pressed, int knob, int shift)
{
    (void)shift;
    if (code >= 0x0A && code <= 0x0C) return 1;              /* lift / drop / scissors: nothing */
    if (knob >= 0) { if (pressed) { tb_press(m, knob); TB_CHANGED(); } return 1; }
    return 0;
}
static void tb_midi_start(void) { tb_restart(0); tb_restart(1); }
const op1_seq_t SEQ_tb3po = { tb_active, tb_player_get, tb_pkey, tb_turn, tb_midi_start, 0 };
#endif
