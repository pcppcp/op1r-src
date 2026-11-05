/* "dddd" - harsh noise, paraphonic (docs/53). DSP in dddd/dd_core.c: one shared wall for all keys.
 *   blue  : source  noise -> crackle -> chaos -> metal         page 2: filter  LP -> BP -> HP
 *           (both in 8 steps per range: plugin.json "coarse", op1r 44 firmware; older: the stock fine steps)
 *   green : feedback (past ~80 % it screams)                   page 2: loop spread (the hands throw the taps)
 *   white : destroy  fold -> clip -> crush                     page 2: rate crush 1 .. 1/32
 *   red   : hands    random movement; stutter at the top
 * Paraphonic: the lowest sounding voice (the leader) renders the wall, the others silence; every sounding key adds a
 * loop tap (the newest four). A voice counts as sounding while it renders (a finished release is not rendered).
 * A released key stops leading while another key is held (note_off), so the wall does not follow a fading release
 * into silence; with no key held the lowest releasing voice leads the tail.
 * Copyright (C) 2026 pcppcp. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"
#include "../../dddd/dd_core.h"

static dd_t dd;
static uint32_t calls, last_call[OP1_VOICES], order[OP1_VOICES], order_n;   /* render calls, note order */
static uint32_t voice_inc[OP1_VOICES], taps_inc[DD_TAPS];
static int taps_n;
static uint8_t inited, alive_seen[OP1_VOICES], held[OP1_VOICES];   /* held: note on, no note off yet */
/* the screen's view of the wall (written by the leader's render, read by the GUI) */
static volatile int32_t level;

static int alive(int v) { return alive_seen[v] && calls - last_call[v] <= OP1_VOICES + 1; }
static int any_held(void) { for (int u = 0; u < OP1_VOICES; u++) if (held[u] && alive(u)) return 1; return 0; }
/* leads: the lowest held voice; with nothing held, the lowest sounding one (the release tail) */
static int leads(int u, int keys) { return alive(u) && (held[u] || !keys); }

static void dd_note_on(void *self, const int16_t *params)
{
    int v = op1_voice_index(self), any = 0;
    (void)params;
    if (!inited) { dd_init(&dd, 0x5EEDu); inited = 1; }
    for (int u = 0; u < OP1_VOICES; u++) if (u != v && alive(u)) any = 1;
    if (!any) { dd_reset_loop(&dd); op1_dbg("dddd reset: voice", v, 0); }   /* nothing sounding: no stale howl */
    order[v] = ++order_n;
    last_call[v] = calls; alive_seen[v] = 1; held[v] = 1;
}

static void dd_note_off(void *self) { held[op1_voice_index(self)] = 0; }

static void dd_render_voice(void *self, int16_t *out, const int16_t *params)
{
    int v = op1_voice_index(self);
    calls++;
    last_call[v] = calls; alive_seen[v] = 1;
    voice_inc[v] = (uint32_t)OP1_PITCH_INC[0];
    int keys = any_held();
    if (!leads(v, keys)) { for (int i = 0; i < OP1_BLOCK; i++) out[i] = 0; return; }   /* a released key while others hold */
    for (int u = 0; u < v; u++)
        if (leads(u, keys)) { for (int i = 0; i < OP1_BLOCK; i++) out[i] = 0; return; }   /* a lower voice leads */
    if (!inited) { dd_init(&dd, 0x5EEDu); inited = 1; }
    /* the sounding keys, oldest first (the newest four become taps) */
    int idx[OP1_VOICES], n = 0;
    for (int u = 0; u < OP1_VOICES; u++) if (alive(u)) idx[n++] = u;
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && order[idx[j]] < order[idx[j - 1]]; j--) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
    uint32_t inc[OP1_VOICES];
    for (int i = 0; i < n; i++) inc[i] = voice_inc[idx[i]];
    int k0 = n > DD_TAPS ? n - DD_TAPS : 0, same = n - k0 == taps_n;
    for (int i = k0; i < n && same; i++) same = taps_inc[i - k0] == inc[i];
    if (!same) {                                               /* new keys: new taps (divisions only here) */
        dd_set_incs(&dd, inc + k0, n - k0);
        taps_n = n - k0;
        for (int i = k0; i < n; i++) taps_inc[i - k0] = inc[i];
    }
    dd_par p = { params[0], params[1], params[2], params[3],
                 params[OP1_ENG_PAGE_WORD(0)], params[OP1_ENG_PAGE_WORD(1)], params[OP1_ENG_PAGE_WORD(2)],
                 op1_voice_vel_gain(self), OP1_VOICE_ENV[OP1_BLOCK - 1] };
    dd_render(&dd, &p, out, OP1_BLOCK);
    int32_t pk = 0;
    for (int i = 0; i < OP1_BLOCK; i++) { int32_t a = out[i] < 0 ? -out[i] : out[i]; if (a > pk) pk = a; }
    level = pk;
}

const op1_engine_t ENGINE_dddd = { dd_note_on, dd_render_voice, dd_note_off,
                                   { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_NONE } };

/* ======================= screen (GUI thread) =======================
 * dddd.svg is empty. Drawn here: a minimalist wasp (line art, top view) that the knobs distort: green stretches its
 * body, white jags and coarsens every line, blue reshapes the wings (smooth .. angular, larger), red speeds the wing
 * beat and makes it jump when the hands jump (torn red for a few frames); the sound makes it buzz. Bottom row in the
 * stock font. */
#include "op1_synth.h"
#define Q(v) ((v) << 10)
#define TS 12
static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     c_grey[4] = { 0x6F, 0x71, 0x73, 0xFF }, c_pale[4] = { 0xAE, 0xB1, 0xDC, 0xFF };
static const uint8_t *const col4[4] = { c_blue, c_green, c_white, c_red };
enum { TX_LAB, TX_VAL = 4, TX_N = 8 };
static uint32_t tx[TX_N][OP1_TEXT_SIZE / 4];
static char shown[TX_N][14];
static uint8_t tx_made;
static uint32_t gui_rng = 1, seen_jumps;
static int tear;

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    if (x0 == x1 && y0 == y1) return;
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
static void txt(void *r, int i, const char *s)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    int same = 1;
    for (int j = 0; j < 14; j++) { if (shown[i][j] != s[j]) { same = 0; break; } if (!s[j]) break; }
    if (!same) { op1_text_set(tx[i], s); int j = 0; for (; j < 13 && s[j]; j++) shown[i][j] = s[j]; shown[i][j] = 0; }
    op1_text_draw(tx[i], r, ident);
}
static uint32_t grnd(void) { gui_rng ^= gui_rng << 13; gui_rng ^= gui_rng >> 17; gui_rng ^= gui_rng << 5; return gui_rng; }
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static void pct(char *p, int32_t v) { int x = (v * 100 + 16383) >> 15; char t[4]; int n = 0; do t[n++] = (char)('0' + x % 10); while ((x /= 10)); while (n) *p++ = t[--n]; put(p, "%"); }

static uint32_t flap;
static int32_t jx, jy, w_jag, w_snap, w_stretch, w_ox, w_oy, w_tear;
static uint32_t w_seed;                                         /* the jag of this frame: the same point, the same jag */
/* a wasp point (x, y: px around the waist, x to the head; abdomen x < 0 is stretched) -> Q10 on screen, distorted */
static op1_pt wp(int32_t x, int32_t y)
{
    if (x < 0) x = (x * w_stretch) >> 10;                       /* the abdomen grows */
    else x = (x * (1024 + ((w_stretch - 1024) >> 2))) >> 10;
    if (w_jag) {                                                /* per point, so the lines stay joined */
        uint32_t q = ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u) ^ w_seed;
        q ^= q >> 13; q *= 0x5bd1e995u; q ^= q >> 15;
        x += (int32_t)(q % (2 * w_jag + 1)) - w_jag; y += (int32_t)((q >> 8) % (2 * w_jag + 1)) - w_jag;
    }
    x += w_ox; y += w_oy;
    if (w_snap > 1) { x -= x % w_snap; y -= y % w_snap; }
    op1_pt p = { Q(x), Q(y) };
    return p;
}
static void seg2(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = wp(x0, y0), b = wp(x1, y1);
    if (w_tear && (grnd() & 7) == 0) c = c_red;                 /* torn */
    if (a.x != b.x || a.y != b.y) op1_raster_line(r, &a, &b, w, c);
}
/* an ellipse of n segments around (cx, cy), radii rx, ry px; a: from .. to (8192 a turn) */
static void ellipse(void *r, int32_t cx, int32_t cy, int32_t rx, int32_t ry, int n, int32_t a0, int32_t a1, const uint8_t *c)
{
    int32_t px = cx + (rx * OP1_SINE_TABLE[(a0 + 2048) & 8191] >> 15), py = cy + (ry * OP1_SINE_TABLE[a0 & 8191] >> 15);
    for (int i = 1; i <= n; i++) {
        int32_t a = a0 + (a1 - a0) * i / n;
        int32_t x = cx + (rx * OP1_SINE_TABLE[(a + 2048) & 8191] >> 15), y = cy + (ry * OP1_SINE_TABLE[a & 8191] >> 15);
        seg2(r, px, py, x, y, 0x600, c);
        px = x; py = y;
    }
}
static void wasp_draw(void *r, int wseg, int32_t open, int32_t src)
{
    /* wings (pale): two pairs, a loop from the thorax backwards, width = the beat */
    for (int s = -1; s <= 1; s += 2) {
        int32_t big = 12 + ((src * 6) >> 15);
        ellipse(r, 6, s * (open + 8), 26 + big / 2, open, wseg, 0, 8192, c_pale);
        ellipse(r, 0, s * (open + 2), 18 + big / 3, open * 2 / 3, wseg, 0, 8192, c_pale);
    }
    ellipse(r, -26, 0, 30, 12, 16, 0, 8192, c_white);           /* abdomen */
    seg2(r, -56, 0, -64, 0, 0x600, c_white);                     /* sting */
    for (int i = 0; i < 3; i++) {                               /* stripes */
        int32_t x = -12 - i * 12, h = 11 - i * 2;
        seg2(r, x, -h, x - 3, 0, 0x400, c_grey); seg2(r, x - 3, 0, x, h, 0x400, c_grey);
    }
    seg2(r, 0, -2, 6, -2, 0x400, c_white); seg2(r, 0, 2, 6, 2, 0x400, c_white);   /* waist */
    ellipse(r, 18, 0, 13, 9, 12, 0, 8192, c_white);             /* thorax */
    ellipse(r, 38, 0, 7, 7, 8, 0, 8192, c_white);               /* head */
    for (int s = -1; s <= 1; s += 2) {
        seg2(r, 43, s * 3, 54, s * 12, 0x400, c_white); seg2(r, 54, s * 12, 60, s * 11, 0x400, c_white);   /* antennae */
        for (int l = 0; l < 3; l++) {                            /* legs */
            int32_t x = 10 + l * 8;
            seg2(r, x, s * 8, x - 6 + l * 3, s * 18, 0x400, c_grey);
            seg2(r, x - 6 + l * 3, s * 18, x - 12 + l * 6, s * 24, 0x400, c_grey);
        }
    }
}

static void dd_screen_init(void *scr) { (void)scr; }

static void dd_screen_update(void *scr, int dt)
{
    int slot = OP1_SCREEN_SLOT & 7;
    int16_t k[4];
    op1_synth_knobs_live(slot, k);
    int16_t wsh[8]; const int16_t *w = op1_knobs_shown(0, 0, op1_synth_knobs(slot), wsh);   /* page 2 as modulated */
    if (!tx_made) {
        for (int i = 0; i < 4; i++) {
            op1_text_ctor(tx[TX_LAB + i], Q(8 + 78 * i), 114, TS, col4[i]);
            op1_text_ctor(tx[TX_VAL + i], Q(8 + 78 * i), 127, TS, c_white);
        }
        tx_made = 1;
    }
    op1_screen_draw(scr, dt);
    void *r = op1_raster();

    /* the wasp: top view, head to the right; green stretches it, white jags it, blue reshapes the wings, red makes
     * it flap faster and jump (with red tears); the sound makes it buzz */
    int32_t lv = level;                                         /* 0 .. 16384 */
    if (dd.jumps != seen_jumps) {                               /* the hands jumped: the wasp jumps too */
        seen_jumps = dd.jumps; tear = 3;
        uint32_t q = grnd();
        jx = (int32_t)(q & 31) - 16; jy = (int32_t)((q >> 5) & 15) - 8;
        jx = jx * k[3] >> 15; jy = jy * k[3] >> 15;
    }
    flap += (uint32_t)(dt > 0 ? dt : 0) * (uint32_t)(40 + (k[3] >> 6) + (lv >> 7));   /* wing beat */
    w_jag = (k[2] * 6) >> 15;                                   /* px of random jag per point */
    w_snap = 1 + ((k[2] * 4) >> 15);                            /* coarse grid at full destroy */
    int32_t buzz = lv >> 12;                                    /* px: the sound shakes the whole wasp */
    w_stretch = 1024 + ((k[1] * 1024) >> 15);                   /* body length x1 .. x2 (Q10) */
    uint32_t q = grnd();
    w_ox = 160 + jx + (buzz ? (int32_t)(q % (2 * buzz + 1)) - buzz : 0);
    w_oy = 54 + jy + (buzz ? (int32_t)((q >> 8) % (2 * buzz + 1)) - buzz : 0);
    w_seed = (q >> 16) & 7;                                     /* the jag flickers between 8 shapes */
    w_tear = tear;
    int seg = 16 - ((k[0] * 12) >> 15);                         /* wings: smooth (16) .. angular (4) */
    int32_t open = 6 + ((OP1_SINE_TABLE[(flap >> 4) & 8191] + 32768) * 6 >> 16);   /* wing width 6 .. 12 px */
    wasp_draw(r, seg, open, k[0]);
    if (tear) tear--;

    /* the bottom row */
    for (int i = 0; i < 4; i++) ln(r, Q(8 + 78 * i), Q(110), Q(78 + 78 * i), Q(110), 0x800, col4[i]);
    static const char *const srcs[4] = { "noise", "crackle", "chaos", "metal" };
    char v[16];
    if (op1_eng_page[0]) {
        int32_t f = w[OP1_ENG_PAGE_WORD(0)] < 0 ? 0 : w[OP1_ENG_PAGE_WORD(0)];
        txt(r, TX_LAB, "filter"); txt(r, TX_VAL, f < 10923 ? "lp" : f < 21845 ? "bp" : "hp");
    } else { txt(r, TX_LAB, "source"); txt(r, TX_VAL, srcs[(k[0] * 4 >> 15) > 3 ? 3 : k[0] * 4 >> 15]); }
    if (op1_eng_page[1]) { txt(r, TX_LAB + 1, "spread"); pct(v, w[OP1_ENG_PAGE_WORD(1)] < 0 ? 0 : w[OP1_ENG_PAGE_WORD(1)]); }
    else { txt(r, TX_LAB + 1, "feedback"); pct(v, k[1]); }
    txt(r, TX_VAL + 1, v);
    if (op1_eng_page[2]) {
        int32_t c = w[OP1_ENG_PAGE_WORD(2)] < 0 ? 0 : w[OP1_ENG_PAGE_WORD(2)];
        int n = 1 + ((c * 31) >> 15);
        char *p = put(v, "1/"); char t[4]; int m = 0; do t[m++] = (char)('0' + n % 10); while ((n /= 10)); while (m) *p++ = t[--m]; *p = 0;
        txt(r, TX_LAB + 2, "rate");
    } else { txt(r, TX_LAB + 2, "destroy"); pct(v, k[2]); }
    txt(r, TX_VAL + 2, v);
    txt(r, TX_LAB + 3, "hands"); pct(v, k[3]); txt(r, TX_VAL + 3, v);
}

const op1_screen_t SCREEN_dddd = { dd_screen_init, dd_screen_update };
