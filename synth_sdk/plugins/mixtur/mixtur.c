/* "mixtur" - subharmonic organ after Oskar Sala's Mixtur-Trautonium (1952; The Birds, 1963).
 * The played key is the TOP note. Four sawtooth dividers sound its undertones f/n (n = 1..24), all phase-locked
 * to one master phase like the Trautonium's frequency dividers, and the mixture goes through two resonant
 * formant band-passes in parallel (vowel-like "Formantfilter").
 *   knob 0 (blue)  : divider A undertone n   (off, 1..24 - 25 steps)
 *   knob 1 (green) : divider B undertone n
 *   knob 2 (white) : divider C undertone n
 *   knob 3 (red)   : divider D undertone n
 *   blue press     : page 2 = formant 1 frequency  180 .. 4070 Hz   (word 5; 0 = unset = 8192 = 392 Hz)
 *   green press    : page 2 = formant 2 frequency  500 .. 5660 Hz   (word 6; 0 = unset = 8192 = 917 Hz)
 *   white press    : page 2 = resonance Q 0.7 .. 22                 (word 7; 0 = unset = 8192 = Q 3.2)
 * Unset page-2 words read as 8192 because that is where the firmware starts turning an unset word.
 *
 * Dividers, exact and division-free per sample: master phase p (u32, 2^32 = one cycle of the key's pitch).
 * Each divider k counts master wraps in c_k = 0..n_k-1; its own phase is
 *     q_k = c_k * floor(2^32/n) + (p >> 16) * floor(2^16/n)          (tables, gen_tables.py)
 * which never exceeds 2^32 - 65536/n, so it stays locked forever with no drift and only a 2^-16 step where c_k
 * advances. Every saw reset falls on a master wrap, so one 2-sample polyBLEP at the wrap (lookahead for the sample
 * before) band-limits all four dividers at once. Changing n while a note sounds fades that divider out (~12 ms),
 * switches, and fades it back in: no clicks. Level is normalised for 1..4 distinct undertones.
 * Formants: Chamberlin SVFs (Q13 coefficients, int32 state), input scaled by the damping q so the band-pass has
 * unity peak gain, makeup gain over Q, and a soft knee at the output: linear up to 0.7 of full scale, rounded above.
 * Undertones below the audible range fade out (40 Hz full, 25 Hz silent): a 13 Hz "undertone" is not a tone but
 * 13 clicks a second, each ringing the formants - it crackles.
 * Cost: two passes per block (mixture -> buffer, then filters). On the Blackfin they run as the kernels in
 * mixtur_dsp.S (about 50 packets per sample), bit for bit the C loops in mixtur_dsp.h, which the host builds use, as
 * does a Blackfin build with MIXTUR_C_REF. No library division per block for a held pitch. See README.
 * Copyright (C) 2026 ultralazr. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include "op1_plugin.h"
#include "mixtur_tables.h"
#include "mixtur_dsp.h"

typedef struct {
    uint32_t vtable;            /* +0x00 firmware-owned, never written */
    uint32_t p;                 /* +0x04 master phase (the top note) */
    int32_t  lp1, bp1, lp2, bp2;/* +0x08 formant SVF states */
    uint8_t  c[4];              /* +0x18 divider wrap counters 0..n-1 */
    uint8_t  n[4];              /* +0x1c undertone playing (0 = off); n[0] bit 7 = key held (screen) */
    uint8_t  g[4];              /* +0x20 divider gain 0..255 (fades when n changes) */
} mx_voice_t;
typedef char mx_size_check[sizeof(mx_voice_t) <= OP1_VOICE_SIZE ? 1 : -1];

#define GATE     0x80
#define FADE     64                                 /* gain step per block: 0 -> 255 in 4 blocks (11.6 ms) */
#define SMAX     131071                             /* SVF state range: bp >> 2 fits 16 bits */
#ifndef TUNE_LEVEL
#define TUNE_LEVEL 5793                              /* +3 dB: peaks just reach the knee */
#endif
#define LEVEL    TUNE_LEVEL                         /* output level, Q12 (matched to op1r's engines) */
#define INC_25HZ 2434896u                           /* phase increment of 25 Hz / 40 Hz (2^32 f / 44100) */
#define INC_40HZ 3895834u
static const uint8_t NORM[5] = { 0, 196, 132, 108, 94 };  /* per-divider gain for 0..4 distinct undertones (equal RMS) */

/* voice memory is shared with the other engines: a signature of our state, written after every render, tells a
 * voice we left (retrigger: keep phase + filters, continuous) from one another engine scribbled over (start clean) */
static uint32_t own[OP1_VOICES];
static mx_p1_t mx_b1;                                  /* the render's pass blocks and mixture (audio thread, */
static mx_p2_t mx_b2;                                  /* one voice at a time), off the stack */
static int16_t mx_xbuf[OP1_BLOCK];
static inline uint32_t sig(const void *v_)
{
    const uint32_t *w = (const uint32_t *)v_;
    return (w[1] ^ 0x6D697874u) + w[3] * 0x9E3779B1u + w[5] * 0x85EBCA6Bu + w[6];
}

static inline int32_t clamp(int32_t x, int32_t m) { return x > m ? m : (x < -m ? -m : x); }
static inline int knob_n(int32_t k) { return k <= 0 ? 0 : (int)((k * 25) >> 15); }   /* 0 = off, 1..24 */
static inline int32_t page_word(const int16_t *params, int i) { return params[i] > 0 ? params[i] : 8192; }
static inline int32_t curve(const int16_t *t, int32_t w)               /* 33-point table, w = 0..32767 */
{
    int i = w >> 10;
    return t[i] + (((t[i + 1] - t[i]) * (w & 1023)) >> 10);
}

/* undertone n per knob and gain targets: a divider repeating an earlier one's n is muted (identical saws would only
 * add up coherently, +6 dB), an undertone below the audible range fades out (inc = the key's phase increment, 0 =
 * unknown: all audible; one division per divider and block), the level is normalised for the undertones heard */
static void targets(const int16_t *params, uint32_t inc, int nt[4], int32_t gt[4])
{
    int act = 0;
    for (int k = 0; k < 4; k++) {
        nt[k] = knob_n(params[k]);
        gt[k] = nt[k] != 0 ? 256 : 0;
        for (int j = 0; j < k; j++) if (nt[j] == nt[k]) gt[k] = 0;
        if (gt[k] && inc) {                                            /* the undertone's increment s = inc / n: */
            uint32_t nk = (uint32_t)nt[k], s;                          /* s >= T <=> inc >= T n, s <= T <=> inc < */
            if (inc >= INC_40HZ * nk) gt[k] = 256;                     /* (T + 1) n (T n < 2^27: no overflow), so */
            else if (inc < (INC_25HZ + 1u) * nk) gt[k] = 0;            /* only the fade band divides */
            else { s = inc / nk; gt[k] = (int32_t)(((s - INC_25HZ) >> 4) * 256 / ((INC_40HZ - INC_25HZ) >> 4)); }
        }
        act += gt[k] >= 128;
    }
    if (!act) act = 1;
    for (int k = 0; k < 4; k++) gt[k] = (NORM[act] * gt[k]) >> 8;
}

/* state left by another engine (signature mismatch): start silent and clean */
static int adopt(mx_voice_t *v)
{
    int vi = op1_voice_index(v);
    if (vi < 0 || vi >= OP1_VOICES) return 0;
    if (own[vi] != sig(v)) {
        v->p = 0; v->lp1 = v->bp1 = v->lp2 = v->bp2 = 0;
        for (int k = 0; k < 4; k++) v->c[k] = v->n[k] = v->g[k] = 0;
    }
    return vi;
}

static void mx_note_on(void *self, const int16_t *params)
{
    mx_voice_t *v = (mx_voice_t *)self;
    int vi = adopt(v), n[4];
    int32_t gt[4];
    targets(params, 0, n, gt);                  /* pitch unknown here: the key's saw starts at once, undertones */
    for (int k = 0; k < 4; k++) {               /* fade in over ~12 ms (render knows which ones are audible) */
        if (v->c[k] >= n[k]) v->c[k] = 0;
        v->n[k] = (uint8_t)n[k];
        if (n[k] == 1) v->g[k] = (uint8_t)gt[k];
    }
    v->n[0] |= GATE;
    own[vi] = sig(v);
}

static void mx_note_off(void *self) { ((mx_voice_t *)self)->n[0] &= (uint8_t)~GATE; }

static void mx_render(void *self, int16_t *out, const int16_t *params)
{
    mx_voice_t *v = (mx_voice_t *)self;
    const int32_t *inc = OP1_PITCH_INC;

    /* ---- per block: divider targets, fades, formant coefficients ---- */
    int vi = adopt(v), nt[4];
    int32_t gt[4];
    targets(params, (uint32_t)inc[0], nt, gt);
    mx_p1_t *b1 = &mx_b1;
    uint32_t c[4], n[4], gq[4], dg[4], R[4], bh[4];
    for (int k = 0; k < 4; k++) {
        uint32_t nk = v->n[k] & 31u, ck = v->c[k];
        int32_t g = v->g[k];
        if (nk > 24) nk = 0;
        if (nk != (uint32_t)nt[k] && g == 0) {  /* silent: switch to the new undertone */
            nk = (uint32_t)nt[k];
            if (ck >= 24) ck = 0;
            while (nk && ck >= nk) ck -= nk;    /* <= 23 steps */
        }
        if (!nk || ck >= nk) ck = 0;
        int32_t tgt = nk == (uint32_t)nt[k] ? gt[k] : 0, gn = g;
        gn += clamp(tgt - g, FADE);
        n[k] = nk; c[k] = ck; R[k] = DIV_R[nk]; b1->d[k] = DIV_D[nk]; bh[k] = (ck * DIV_D[nk]) >> 17;
        gq[k] = (uint32_t)g << 7;               /* the gain at sample 0, then 7 steps of 16 samples: */
        dg[k] = (uint32_t)(gn - g) * 16u;       /* 8 dg = (gn - g) << 7 */
        v->g[k] = (uint8_t)gn;
    }
#define MX_PAIR(a, k) ((a)[k + 1] << 16 | ((a)[k] & 0xFFFFu))
    b1->c01 = MX_PAIR(c, 0); b1->c23 = MX_PAIR(c, 2); b1->n01 = MX_PAIR(n, 0); b1->n23 = MX_PAIR(n, 2);
    b1->gq01 = MX_PAIR(gq, 0); b1->gq23 = MX_PAIR(gq, 2); b1->dg01 = MX_PAIR(dg, 0); b1->dg23 = MX_PAIR(dg, 2);
    b1->bh01 = MX_PAIR(bh, 0); b1->bh23 = MX_PAIR(bh, 2); b1->r01 = MX_PAIR(R, 0); b1->r23 = MX_PAIR(R, 2);
    b1->fade = (dg[0] | dg[1] | dg[2] | dg[3]) != 0;
    int32_t f1 = curve(F1_COEF, page_word(params, 5)), f2 = curve(F2_COEF, page_word(params, 6));
    int32_t wr = page_word(params, 7), q = curve(RES_Q, wr);
    int32_t gain = (((curve(RES_MAKEUP, wr) * op1_voice_vel_gain(self)) >> 15) * LEVEL) >> 12;   /* Q12 makeup x vel */
    if (gain > 32767) gain = 32767;
    /* polyBLEP fraction d = p / inc in Q15 without a per-sample division: rinv from this block's first inc */
    uint32_t inc0 = (uint32_t)inc[0] | 1u; int sh = 0;
    while ((inc0 >> sh) >= 32768u) sh++;                                 /* <= 17 steps */
    uint32_t dd = inc0 >> sh;                                            /* 16384 .. 32767 once sh > 0 */
    uint32_t rinv = dd > 16384u ? recip29(dd) : dd == 16384u ? 32768u : (1u << 29) / dd;   /* per block */

    b1->p = v->p; b1->rinv = rinv; b1->nsh = -sh; b1->inc = inc; b1->one2 = 0x00010001u;

    /* pass 1: master phase, dividers, polyBLEP -> xbuf. Between two master wraps every saw is linear in p, so the
     * whole mixture is ONE ramp: sum_k gq_k * saw_k = A + (p >> 16) * B, with A = sum gq_k * (base_k >> 17) and
     * B = sum (gq_k * R_k) >> 17 (truncation error <= 8 / 32768). A changes at wraps, A and B when the gains step.
     * pass 2: two formant SVFs in parallel, makeup x velocity, soft clip. No clamps needed in the loop: the
     * worst-case L1 norms of the SVF (Q 0.7, 5.6 kHz: lp 1.54, bp 2.24, hp 5.06, see README) times |in / q| <= 50176
     * keep bp >> 2 and hp >> 3 inside 16 bits. Both in mixtur_dsp.h (C) and mixtur_dsp.S (Blackfin). */
    mx_p2_t *b2 = &mx_b2;
    b2->f = (uint32_t)f2 << 16 | (uint32_t)f1; b2->qg = (uint32_t)gain << 16 | (uint32_t)q;
    b2->f2l = MX_F2_LEVEL; b2->cm = MX_KNEE + MX_KNEE_T; b2->kn = MX_KNEE; b2->zero = 0; b2->ka = MX_KNEE_A;
    b2->lp1 = clamp(v->lp1, SMAX); b2->bp1 = clamp(v->bp1, SMAX); b2->lp2 = clamp(v->lp2, SMAX); b2->bp2 = clamp(v->bp2, SMAX);
#if defined(__bfin__) && !defined(MIXTUR_C_REF)
    mx_pass1(b1, mx_xbuf);
    mx_pass2(b2, mx_xbuf, out);
#else
    mx_pass1_c(b1, mx_xbuf);
    mx_pass2_c(b2, mx_xbuf, out);
#endif
    v->p = b1->p;
    v->lp1 = b2->lp1; v->bp1 = b2->bp1; v->lp2 = b2->lp2; v->bp2 = b2->bp2;
    c[0] = mx_lo(b1->c01); c[1] = mx_hi(b1->c01); c[2] = mx_lo(b1->c23); c[3] = mx_hi(b1->c23);
    for (int k = 0; k < 4; k++) v->c[k] = (uint8_t)c[k];
    v->n[0] = (uint8_t)(n[0] | (v->n[0] & GATE));
    v->n[1] = (uint8_t)n[1]; v->n[2] = (uint8_t)n[2]; v->n[3] = (uint8_t)n[3];
    own[vi] = sig(v);
}

const op1_engine_t ENGINE_mixtur = { mx_note_on, mx_render, mx_note_off,
                                     { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_NONE } };

/* ======================= screen (GUI thread) =======================
 * mixtur.svg (gen_svg.py, geometry in mixtur_geom.h): the undertone ladder - 24 strings from a bridge, string n is
 * 1/n long - and a log-frequency axis under its hyperbola. Drawn here: the four selected strings in blue / green /
 * white / red; while keys are held a visual master clock ticks (TICK_MS) and every divider steps a bead one notch
 * down its string per tick (n notches = one divider cycle), flashing the string when it wraps - the counters of the
 * real dividers, slowed down to watchable speed. Formant curves on the axis (grey, or blue / green on page 2).
 * Bottom: undertone n per knob (OFF, or dim when it repeats another divider's n), F1 / F2 / RES on page 2. */
#include "seq/seq_text.h"
#include "mixtur_geom.h"
#pragma GCC push_options
#pragma GCC optimize("Os")                             /* GUI code: size, not speed (code space is short) */

#define TICK_MS 140
static const uint8_t MX_COL[4][3] = { { 0x4D, 0x9E, 0xFF }, { 0x00, 0xED, 0x95 }, { 0xFF, 0xFF, 0xFF }, { 0xFF, 0x3A, 0x5D } };
static const uint8_t MX_GREY[3] = { 0x6F, 0x71, 0x73 };
static int32_t mx_ms, mx_glow, mx_flash[4];       /* ms into the tick, held glow 0..256, string flash 0..256 */
static uint32_t mx_tick;

static void mx_shade(uint8_t *o, const uint8_t *c, int32_t l)          /* l = 0..256 */
{
    for (int i = 0; i < 3; i++) o[i] = (uint8_t)((c[i] * l) >> 8);
    o[3] = 0xFF;
}
#define mx_line sq_line                               /* seq/seq_text.h: one raster line */
static int32_t mx_strw(const char *t)                                 /* stroke-font width in grid units */
{
    int32_t u = 0;
    for (; *t; t++) u += *t == '1' || *t == '.' ? 4 : 6;
    return u > 2 ? u - 2 : u;
}
static void mx_ctext(void *r, int cx, int y, int32_t s, int32_t w, const char *t, const uint8_t *c)   /* centred */
{
    sq_text(r, OP1_Q10(cx) - mx_strw(t) * s / 2, OP1_Q10(y), s, t, w, c);
}
#define mx_itoa(p, v) sq_num((p), (uint32_t)(v), 1)     /* v >= 0, returns the end (seq/seq_text.h) */

static void mx_screen_init(void *scr) { (void)scr; }

static void mx_screen_update(void *scr, int dt)
{
    int slot = OP1_SCREEN_SLOT & 7;
    int16_t k[4];
    sq_synth_knobs_live(slot, k);                                     /* incl. LFO, like the stock screens */
    const int16_t *wd = op1_synth_knobs(slot);                         /* page-2 words 5..7 */
    int nt[4], pg[4] = { op1_eng_page[0], op1_eng_page[1], op1_eng_page[2], 0 };
    int32_t gt[4];
    targets(k, 0, nt, gt);
    int held = 0;
    for (int i = 0; i < OP1_VOICES; i++) if (((const mx_voice_t *)OP1_VOICE(i))->n[0] & GATE) held = 1;

    /* visual master clock: ticks while keys are held; divider k flashes when the tick count is a multiple of n */
    if (dt < 0) dt = 0; else if (dt > 100) dt = 100;
    if (held) {
        mx_ms += dt;
        if (mx_ms >= TICK_MS) {
            mx_ms -= TICK_MS; if (mx_ms >= TICK_MS) mx_ms = 0;
            mx_tick++;
            for (int i = 0; i < 4; i++) if (nt[i] && mx_tick % (uint32_t)nt[i] == 0) mx_flash[i] = 256;
        }
        mx_glow += (256 - mx_glow) >> 2;
    } else mx_glow -= (mx_glow >> 3) + 1;
    if (mx_glow < 0) mx_glow = 0;
    for (int i = 0; i < 4; i++) { mx_flash[i] -= (mx_flash[i] * dt) >> 7; if (mx_flash[i] < 4) mx_flash[i] = 0; }

    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    uint8_t c[4];

    /* formant panel: band-pass power curves 1 / (1 + z^2), z ~ 1.39 Q * octaves from the peak */
    int32_t w5 = page_word(wd, 5), w6 = page_word(wd, 6), w7 = page_word(wd, 7), q10 = curve(RES_Q10, w7);
    int32_t pk[2] = { MX_F1_X0_Q10 + (((MX_F1_SPAN_Q10 >> 5) * w5) >> 10),        /* peak x, Q10 */
                      MX_F2_X0_Q10 + (((MX_F2_SPAN_Q10 >> 5) * w6) >> 10) };
    int pg2 = pg[0] | pg[1] | pg[2];
    for (int f = 1; f >= 0; f--) {
        if (pg2) mx_shade(c, MX_COL[f], pg[f] | pg[2] ? 256 : 140); else mx_shade(c, MX_GREY, 120);
        int32_t hmax = OP1_Q10(MX_FH) * (f ? 7 : 10) / 10, px = 0, py = 0;
        for (int x = MX_FX0; x <= MX_FX1; x += 3) {
            int32_t dx = OP1_Q10(x) - pk[f];                               /* Q10 px */
            int32_t z8 = (((q10 * dx) >> 10) * 1211) >> 10;                /* z in Q8: Q * dx / 30 px * 1.386 */
            if (z8 < 0) z8 = -z8;
            if (z8 > 8192) z8 = 8192;
            int32_t h = (hmax << 8) / (256 + ((z8 * z8) >> 8));             /* GUI thread: divisions are fine */
            int32_t yy = OP1_Q10(MX_FY) - h;
            if (x > MX_FX0) mx_line(r, px, py, OP1_Q10(x), yy, 0x500, c);
            px = OP1_Q10(x); py = yy;
        }
        if (pg2 && pg[f]) mx_line(r, pk[f], OP1_Q10(MX_FY - 3), pk[f], OP1_Q10(MX_FY + 5), 0x800, c);
    }

    /* ladder: selected strings, beads, flashes */
    for (int i = 3; i >= 0; i--) {
        int n = nt[i];
        if (!n || !gt[i]) continue;                                    /* off, or repeats another divider */
        int32_t y = MX_LY0_Q10 + (n - 1) * MX_LDY_Q10, len = OP1_Q10(MX_LMAX) / n;
        int32_t x0 = OP1_Q10(MX_LX0), x1 = x0 + len;
        mx_shade(c, MX_COL[i], 150 + ((mx_glow * 50) >> 8) + ((mx_flash[i] * 56) >> 8));
        mx_line(r, x0, y, x1, y, 0x780 + (mx_flash[i] << 2), c);
        mx_line(r, OP1_Q10(13), y, OP1_Q10(19), y, 0x780, c);          /* tab left of the bridge */
        if (mx_glow > 8) {                                             /* bead: steps one notch per tick */
            int32_t ph = (int32_t)(mx_tick % (uint32_t)n), e = mx_ms * 1024 / (TICK_MS / 3);
            if (e > 1024) e = 1024;
            int32_t pos = ph ? (ph - 1) * 1024 + e : 0;                   /* notches * 1024, slides into place */
            int32_t bx = x1 - (len >> 4) * pos / (n * 64);
            mx_shade(c, MX_COL[i], mx_glow);
            mx_line(r, bx, y - OP1_Q10(2.5), bx, y + OP1_Q10(2.5), 0x600, c);
            mx_line(r, bx - OP1_Q10(1.5), y, bx + OP1_Q10(1.5), y, 0x400, c);
        }
    }

    /* readouts: caption + value per knob */
    static const char *cap1[4] = { "A", "B", "C", "D" }, *cap2[3] = { "F1", "F2", "RES" };
    for (int i = 0; i < 4; i++) {
        char t[8], *e;
        int cx = mx_col_x[i];
        if (pg[i]) {
            int32_t v = i == 0 ? curve(F1_HZ, w5) : i == 1 ? curve(F2_HZ, w6) : q10;
            if (i == 2 || v >= 1000) {                                  /* one decimal: 3.2 / 1.9K */
                int32_t d = i == 2 ? v : (v + 50) / 100;
                e = mx_itoa(t, d / 10); *e++ = '.'; e = mx_itoa(e, d % 10);
                if (i < 2) { *e++ = 'K'; *e = 0; }
            } else mx_itoa(t, v);
            mx_shade(c, MX_COL[i], 256);
            mx_ctext(r, cx, MX_CAP_Y, 1024, 0x480, cap2[i], c);
        } else {
            mx_shade(c, MX_GREY, 256);
            mx_ctext(r, cx, MX_CAP_Y, 1024, 0x480, cap1[i], c);
            if (nt[i]) { mx_itoa(t, nt[i]); mx_shade(c, MX_COL[i], gt[i] ? 256 : 90); }
            else { mx_shade(c, MX_GREY, 170); mx_ctext(r, cx, MX_NUM_Y + 3, 1536, 0x600, "OFF", c); continue; }
        }
        mx_ctext(r, cx, MX_NUM_Y, 2304, 0x700, t, c);
    }
}

#pragma GCC pop_options
const op1_screen_t SCREEN_mixtur = { mx_screen_init, mx_screen_update };
