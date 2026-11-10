/* moai screen (GUI thread; included by moai_ui.c after mm_ui.h; SVG: moai.svg, empty: the dials are drawn here,
 * geometry: moai_geom.h). Everything stays above y = 146: the OP-1 does not show the bottom rows.
 *
 * Every frame: mm_ui_frame (presses -> views, turns -> parameters), then the view:
 *   home      a scene: the moai of Ahu Tongariki filtered as the sound is (the cutoff and emphasis), the filter's
 *             ridge and the contour's island on the horizon, wind (the noise); the number (the stock font, large)
 *             and the four knobs with their names at the top; no dials
 *   advanced  the whole synth as a panel of small knobs (oscillators, mixer, filter, contour, analog); the four
 *             highlighted ones in the knob colours are what the knobs turn (Shift + turn moves a highlight)
 *   osc 1..3  tabs, an oscilloscope of the oscillator, its range / wave / frequency / volume
 *   filter    the response, large; cutoff / emphasis / amount / feedback
 *   contour   attack / decay / sustain / release as one shape, each segment in its knob's colour
 *   analog    three drifting oscillator traces (detune, drift), noise grains, drive clipping
 * and at the bottom of every other view the four knobs: labels and dials (the advanced view adds the values).
 * Integer only (64-bit and divisions are fine here: this is not the audio interrupt). */
#include "seq/seq_text.h"
#include "op1_synth.h"
#include "moai_geom.h"
#pragma GCC push_options
#pragma GCC optimize("Os")                             /* GUI code only: size, not speed */

static const uint8_t MG_COL[4][4] = { { 0x4D, 0x9E, 0xFF, 0xFF }, { 0x00, 0xED, 0x95, 0xFF },
                                      { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0x3A, 0x5D, 0xFF } };
static const uint8_t MG_LIGHT[4] = { 0xDF, 0xD9, 0xFF, 0xFF }, MG_GREY[4] = { 0x6F, 0x71, 0x73, 0xFF },
                     MG_DARK[4] = { 0x3A, 0x3B, 0x3E, 0xFF };
static const char MG_SHORT[P_N][4] = { "SET", "CUT", "EMP", "CON", "DET", "DRV", "DRF",
    "RNG", "WAV", "FRQ", "VOL", "RNG", "WAV", "FRQ", "VOL", "RNG", "WAV", "FRQ", "VOL",
    "NSE", "FBK", "AMT", "ATK", "DEC", "SUS", "REL", "SAT", "MIX", "PIT", "FLT" };
static const char *const MG_RANGE[N_RANGES] = { "OFF", "LO", "32'", "16'", "8'", "4'", "2'" };
static const char *const MG_WAVE[N_WAVES] = { "TRI", "SHARK", "SAW", "R.SAW", "SQUARE", "WIDE", "NARROW" };
static uint32_t mg_t;                                   /* frames: animations */

static inline int32_t mg_sin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }     /* table[k] = sin(2 pi k / 8192) */
static inline int32_t mg_cos(int32_t a) { return mg_sin(a + 2048); }
#define LN(x0, y0, x1, y1, w, c) sq_line(r, (x0), (y0), (x1), (y1), (w), (c))

static void mg_shade(uint8_t *o, const uint8_t *c, int32_t l)                  /* l = 0..256 */
{
    for (int i = 0; i < 3; i++) o[i] = (uint8_t)((c[i] * l) >> 8);
    o[3] = 0xFF;
}
static void mg_box(void *r, int x0, int y0, int x1, int y1, const uint8_t *c)                     /* px */
{
    LN(OP1_Q10(x0), OP1_Q10(y0), OP1_Q10(x1), OP1_Q10(y0), 0x400, c); LN(OP1_Q10(x1), OP1_Q10(y0), OP1_Q10(x1), OP1_Q10(y1), 0x400, c);
    LN(OP1_Q10(x1), OP1_Q10(y1), OP1_Q10(x0), OP1_Q10(y1), 0x400, c); LN(OP1_Q10(x0), OP1_Q10(y1), OP1_Q10(x0), OP1_Q10(y0), 0x400, c);
}
static int32_t mg_strw(const char *t)                                          /* stroke-font width in grid units */
{
    int32_t u = 0;
    for (; *t; t++) u += *t == '1' || *t == '.' ? 4 : 6;
    return u > 2 ? u - 2 : u;
}
/* text with the feet mark: ' is not in the stroke font (it leaves a gap), drawn here by hand */
static int32_t mg_text_at(void *r, int32_t x, int32_t y, int32_t s, const char *t, const uint8_t *c)   /* Q10 */
{
    int32_t xe = sq_text(r, x, y, s, t, 0x400, c), n = 0;
    while (t[n]) n++;
    if (n && t[n - 1] == '\'') LN(xe - s * 5, y, xe - s * 5, y + s * 2, 0x400, c);
    return xe;
}
static void mg_ctext(void *r, int32_t cx, int32_t y, int32_t s, const char *t, const uint8_t *c)   /* centred, px */
{
    mg_text_at(r, OP1_Q10(cx) - mg_strw(t) * s / 2, OP1_Q10(y), s, t, c);
}
static void mg_text(void *r, int32_t x, int32_t y, int32_t s, const char *t, const uint8_t *c)     /* px */
{
    mg_text_at(r, OP1_Q10(x), OP1_Q10(y), s, t, c);
}
static void mg_circle(void *r, int32_t cx, int32_t cy, int32_t rad, int32_t w, const uint8_t *c, int seg)  /* Q10 */
{
    int32_t px = cx + rad, py = cy;
    for (int i = 1; i <= seg; i++) {
        int32_t a = 8192 * i / seg;
        int32_t qx = cx + ((rad * mg_cos(a)) >> 15), qy = cy + ((rad * mg_sin(a)) >> 15);
        LN(px, py, qx, qy, w, c);
        px = qx; py = qy;
    }
}

/* ---- numbers ---------------------------------------------------------------------------------------------- */

static int32_t mg_exp2_q12(int32_t e)                    /* 2^(e / 1024) in Q12, e >= 0 */
{
    int32_t n = e >> 10, i = (e & 1023) >> 5, f = e & 31;
    int32_t t = mg_exp2[i] + (((int32_t)mg_exp2[i + 1] - (int32_t)mg_exp2[i]) * f >> 5);
    return n >= 19 ? 0x7FFFFFFF : t << n;
}
static int32_t mg_hz(int32_t cut) { return (int32_t)((20LL * mg_exp2_q12((clampk(cut) * 10 * 1024) >> 15)) >> 12); }
static int32_t mg_ms(int32_t t) { return (int32_t)((4LL * mg_exp2_q12(time_index(t) * 90)) >> 12); }   /* 4 ms x 1.0625^i */

/* the value the knobs of words 5..7 stand for (0 = never set: their default) */
static int32_t mg_pv(int id) { return id >= P_DETUNE && id <= P_DRIFT ? page_start(id, P[id]) : P[id]; }
/* what the sound gets: the value plus the OP-1 LFO's offset on the knob that shows it (as the stock engines' needles
 * move). The offsets come from the audio side (mm_lfo); without a note for a few frames there are none. On home the
 * offsets belong to the four home parameters even while Shift shows other ones. */
static int16_t mg_off[4];
static uint8_t mg_seq, mg_idle;
static void mg_lfo_frame(void)
{
    if (mm_lfo_seq != mg_seq) { mg_seq = mm_lfo_seq; mg_idle = 0; } else if (mg_idle < 255) mg_idle++;
    for (int k = 0; k < 4; k++) mg_off[k] = mg_idle > 6 ? 0 : mm_lfo[k];
}
static int32_t mg_lv(int id)
{
    int32_t v = mg_pv(id);
    for (int k = 0; k < 4; k++)
        if (mg_off[k] && (mm_view == V_HOME ? k : mm_map[k]) == id) return pclamp(id, v + poffset(id, mg_off[k]));
    return v;
}

/* value of parameter id as text (short: fits a value column) */
static void mg_value(char *s, int id, int32_t v)
{
    char *e = s;
    *e = 0;
    switch (id) {
    case P_SETUP: sq_num(e, MM_POS[entry_of(v)], 3); break;         /* its number, 000 .. (no names) */
    case P_CUTOFF: {
        /* on a note (within 5 cents, A4 = 440 Hz): its name. The knob is 20 Hz x 2^(10 v / 32768), i.e. 1548.68
         * cents above MIDI note 0 at 0, 12000 cents over the range (in hundredths of a cent here) */
        static const char NOTE[12][3] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        int32_t ct = 154868 + (int32_t)((int64_t)clampk(v) * 1200000 / 32768), n = (ct + 5000) / 10000;
        if (ct - n * 10000 >= -500 && ct - n * 10000 <= 500) { e = sq_cat(e, NOTE[n % 12]); sq_num(e, (uint32_t)(n / 12 - 1), 1); break; }
        int32_t hz = mg_hz(v);
        if (hz >= 1000) {
            e = sq_num(e, (uint32_t)(hz / 1000), 1);
            if (hz < 10000) { *e++ = '.'; e = sq_num(e, (uint32_t)(hz / 100 % 10), 1); }
            sq_cat(e, "K");
        } else { e = sq_num(e, (uint32_t)hz, 1); sq_cat(e, "HZ"); }
        break;
    }
    case P_ATTACK: case P_DECAY: case P_RELEASE: {
        int j = time_slot(id);
        if (ui_sync[j] >= 0 && v == P[id]) { sq_cat(e, DIVS[ui_sync[j]].t); break; }   /* on the tempo: the note value */
        int32_t ms = id == P_ATTACK && v < 400 ? 2 : mg_ms(v);
        e = sq_num(e, (uint32_t)ms, 1); sq_cat(e, "MS");                       /* (an S reads as a 5) */
        break;
    }
    case P_AMOUNT: {                                                              /* octaves, one decimal */
        int32_t t = (clampk(v) >> 2) * 10 / 1024;
        e = sq_num(e, (uint32_t)(t / 10), 1); *e++ = '.'; e = sq_num(e, (uint32_t)(t % 10), 1); sq_cat(e, "OCT");
        break;
    }
    case P_DETUNE: e = sq_num(e, (uint32_t)(v > 0 ? (40 * ((v * v) >> 15)) >> 15 : 5), 1); sq_cat(e, "CT"); break;
    case P_MOD_MIX:                                                              /* the source: osc 3 .. noise */
        if (v < 328) sq_cat(e, "OSC3");
        else if (v > 32439) sq_cat(e, "NOISE");
        else { e = sq_num(e, (uint32_t)((clampk(v) * 100 + 16383) / 32767), 1); sq_cat(e, "%N"); }
        break;
    case P_MOD_PITCH: {                                                          /* depth: cents, then semitones */
        int32_t c = mod_depth(v, MOD_PITCH_MAX);
        if (!c) sq_cat(e, "OFF");
        else if (c < 100) { e = sq_num(e, (uint32_t)c, 1); sq_cat(e, "CT"); }
        else { e = sq_num(e, (uint32_t)(c / 100), 1); *e++ = '.'; e = sq_num(e, (uint32_t)(c / 10 % 10), 1); sq_cat(e, "ST"); }
        break;
    }
    case P_MOD_FILT: {                                                           /* depth: octaves, one decimal */
        int32_t t = mod_depth(v, MOD_FILT_MAX) * 10 / 1024;
        if (!mod_depth(v, MOD_FILT_MAX)) { sq_cat(e, "OFF"); break; }
        e = sq_num(e, (uint32_t)(t / 10), 1); *e++ = '.'; e = sq_num(e, (uint32_t)(t % 10), 1); sq_cat(e, "OCT");
        break;
    }
    case P_DRIVE: case P_SAT: {                                                  /* the gain: 1.0X .. 4.0X / 8.0X */
        int32_t d = id == P_DRIVE ? drive_q12(v) : sat_gain(v);
        e = sq_num(e, (uint32_t)(d >> 12), 1); *e++ = '.'; e = sq_num(e, (uint32_t)(((d & 4095) * 10) >> 12), 1); sq_cat(e, "X");
        break;
    }
    default:
        if (id >= P_O1_RANGE && id < P_NOISE && ((id - P_O1_RANGE) & 3) < 3) {
            int f = (id - P_O1_RANGE) & 3;
            if (f == 0) sq_cat(e, MG_RANGE[pclamp(id, v)]);
            else if (f == 1) sq_cat(e, MG_WAVE[pclamp(id, v)]);
            else if (P[id - 2] == R_LO) {                                        /* LO: the rate, at the last note */
                const mm_voice_t *lv = mm_last < OP1_VOICES ? &V[mm_last] : 0;
                uint32_t ki = lv && lv->key_inc ? lv->key_inc : 12740185u;       /* (C4 at 88.2 kHz before any note) */
                int32_t ex = pclamp(id, v) * LO_FREQ_X * 1024 / 1200 + 3072;    /* octaves + 3 (>= 0), Q10 */
                int32_t ch = (int32_t)(((((int64_t)ki * 8820000) >> 32) * mg_exp2_q12(ex)) >> 21);   /* centi-Hz (ki: 88.2 kHz steps) */
                if (ch < 995) { ch = (ch + 5) / 10; e = sq_num(e, (uint32_t)(ch / 10), 1); *e++ = '.'; e = sq_num(e, (uint32_t)(ch % 10), 1); }
                else e = sq_num(e, (uint32_t)((ch + 50) / 100), 1);
                sq_cat(e, "HZ");
            } else {                                                             /* semitones: +7, -3.25 */
                int32_t c = pclamp(id, v);
                *e++ = c < 0 ? '-' : '+'; if (c < 0) c = -c;
                e = sq_num(e, (uint32_t)(c / 100), 1);
                if (c % 100) { *e++ = '.'; sq_num(e, (uint32_t)(c % 100), 2); }
                else *e = 0;
            }
        } else { e = sq_num(e, (uint32_t)((clampk(v) * 100 + 16383) / 32767), 1); sq_cat(e, "%"); }
        break;
    }
}
static void mg_vtext(void *r, int32_t cx, int32_t y, int32_t s, int id, int32_t v, const uint8_t *c)   /* centred value */
{
    char t[16]; mg_value(t, id, v);
    mg_ctext(r, cx, y, s, t, c);
}
/* the short name, with the oscillator's number for its parameters (the advanced view shows all three) */
static void mg_label(char *t, int id)
{
    char *e = sq_cat(t, MG_SHORT[id]);
    if (id >= P_O1_RANGE && id < P_NOISE) { *e++ = (char)('1' + (id - P_O1_RANGE) / 4); *e = 0; }
}
/* 0..32767 for a dial or a knob drawing */
static int32_t mg_norm(int id, int32_t v)
{
    if (id == P_SETUP) return MM_POS[entry_of(v)] * 32767 / (N_ENTRIES - 1);   /* the dial follows the numbers */
    switch (PD[id].kind) {
    case K_STEP: return PD[id].n > 1 ? pclamp(id, v) * 32767 / (PD[id].n - 1) : 0;
    case K_CENTS: return (pclamp(id, v) + FREQ_MAX) * 32767 / (2 * FREQ_MAX);
    default: return clampk(v);
    }
}

/* ---- the filter response (home and filter views) ------------------------------------------------------------- */

static int32_t mg_log2(uint64_t v)                       /* log2(v >= 1), Q10 */
{
    int m = 63;
    while (m > 0 && !(v >> m)) m--;
    uint32_t f = (uint32_t)(m >= 31 ? v >> (m - 31) : v << (31 - m)) & 0x7FFFFFFFu;
    int32_t i = (int32_t)(f >> 26), fr = (int32_t)((f >> 11) & 0x7FFF);
    return m * 1024 + MM_LOG2[i] + (((int32_t)MM_LOG2[i + 1] - (int32_t)MM_LOG2[i]) * fr >> 15);
}
/* ladder response in dB (Q10) at o octaves (Q10) from the cutoff: gin / |(1 + jx)^4 + k|, x = 2^o (k, gin Q12);
 * peaks clamped at +30 dB (self-oscillation); far above the cutoff -24 dB per octave */
static int32_t mg_resp(int32_t o, int32_t k, int32_t gin)
{
    int32_t extra = 0;
    if (o > 4096) { extra = o - 4096; o = 4096; } else if (o < -4096) o = -4096;
    int32_t n = o >> 10, i = (o & 1023) >> 5, fr = o & 31;
    int64_t t = mg_exp2[i] + (((int32_t)mg_exp2[i + 1] - (int32_t)mg_exp2[i]) * fr >> 5);
    int64_t x = n >= 0 ? t << n : t >> -n;                                       /* Q12 */
    int64_t x2 = (x * x) >> 12, x3 = (x2 * x) >> 12, x4 = (x2 * x2) >> 12;
    int64_t a = 4096 - 6 * x2 + x4 + k, b = 4 * x - 4 * x3;
    int64_t den = a * a + b * b;                                                 /* Q24 */
    if (den < 16384) den = 16384;
    int32_t lp = 2 * (mg_log2((uint64_t)gin) - 12 * 1024) - (mg_log2((uint64_t)den) - 24 * 1024);
    return ((lp * 3083) >> 10) - extra * 24;                                     /* x 3.0103 = dB */
}
typedef struct { int16_t x0, x1, y0, y1, yzero; int32_t ppo; int16_t dbn, dbd; } plot_t;   /* px; dbn / dbd px per dB */
static void mg_curve(void *r, const plot_t *pl, int32_t pos, int32_t k, int32_t gin, const uint8_t *c, int32_t w)
{
    int32_t px = 0, py = 0;
    for (int x = pl->x0; x <= pl->x1; x += 4) {
        int32_t o = ((x - pl->x0) << 20) / pl->ppo - pos;
        int32_t y = OP1_Q10(pl->yzero) - mg_resp(o, k, gin) * pl->dbn / pl->dbd;
        if (y < OP1_Q10(pl->y0 + 1)) y = OP1_Q10(pl->y0 + 1);
        if (y > OP1_Q10(pl->y1 - 1)) y = OP1_Q10(pl->y1 - 1);
        if (x > pl->x0) LN(px, py, OP1_Q10(x), y, w, c);
        px = OP1_Q10(x); py = y;
    }
}
static int32_t mg_pos_x(const plot_t *pl, int32_t pos) { return OP1_Q10(pl->x0) + (int32_t)(((int64_t)pos * pl->ppo) >> 10); }
static void mg_filter_plot(void *r, const plot_t *pl)
{
    uint8_t c[4];
    mg_box(r, pl->x0, pl->y0, pl->x1, pl->y1, MG_DARK);
    LN(OP1_Q10(pl->x0), OP1_Q10(pl->yzero), OP1_Q10(pl->x1), OP1_Q10(pl->yzero), 0x400, MG_DARK);
    static const int16_t DEC[3] = { 100, 1000, 10000 };
    static const char *const DT[3] = { "100", "1K", "10K" };
    mg_shade(c, MG_COL[1], 70);
    for (int i = 0; i < 3; i++) {                                                /* decades */
        int32_t x = mg_pos_x(pl, mg_log2((uint64_t)DEC[i] * 1024 / 20) - 10 * 1024);
        LN(x, OP1_Q10(pl->y0), x, OP1_Q10(pl->y1 + 2), 0x400, MG_DARK);
        sq_text(r, x - mg_strw(DT[i]) * 512, OP1_Q10(pl->y1 + 2), 1024, DT[i], 0x400, c);
    }
    int32_t kq = emph_k(mg_lv(P_EMPH)), gin = make_up(kq);
    int32_t base = (mg_lv(P_CUTOFF) * 10 * 1024) >> 15, depth = mg_lv(P_AMOUNT) >> 2;
    int32_t peak = base + depth; if (peak > MAX_POS) peak = MAX_POS;
    int live = 0, lpos = base;
    if (mm_last < OP1_VOICES) {
        const mm_voice_t *v = &V[mm_last];
        if (v->stage != 2 || v->env > (1 << 16)) { live = 1; lpos = v->pos; }
    }
    mg_shade(c, MG_COL[0], live ? 80 : 256);                                   /* the knob: dim while a note sounds */
    mg_curve(r, pl, base, kq, gin, c, live ? 0x400 : 0x700);
    if (live) { mg_shade(c, MG_COL[0], 256); mg_curve(r, pl, lpos, kq, gin, c, 0x700); }
    int32_t y = OP1_Q10(pl->y1 - 3), x0 = mg_pos_x(pl, base), x1 = mg_pos_x(pl, peak);   /* the contour's sweep */
    mg_shade(c, MG_COL[2], depth > 100 ? 190 : 60);
    LN(x0, y, x1, y, 0x600, c);
    LN(x0, y - OP1_Q10(2), x0, y + OP1_Q10(2), 0x600, c);
    if (x1 > x0 + OP1_Q10(4)) { LN(x1, y, x1 - OP1_Q10(3), y - OP1_Q10(2), 0x600, c); LN(x1, y, x1 - OP1_Q10(3), y + OP1_Q10(2), 0x600, c); }
}

/* ---- small drawings ----------------------------------------------------------------------------------------- */

/* cycles of a wave in x0 .. x0 + w (px), +-h px around y, from phase ph, n segments; noise: a scribble */
static void mg_glyph(void *r, int wave, int32_t x0, int32_t y, int32_t w, int32_t h, int cycles, uint32_t ph, int n, const uint8_t *c)
{
    int32_t s[65], pk = 1;
    if (n > 64) n = 64;
    if (wave == W_NOISE) {
        uint32_t z = 12345 + ph;
        for (int i = 0; i <= n; i++) { z = z * 1664525u + 1013904223u; s[i] = (int32_t)(z >> 16) - 32768; }
        pk = 32768;
    } else {
        wave_t wv; wave_of(&wv, wave);
        uint32_t step = (uint32_t)((0x100000000ull * (uint32_t)cycles) / (uint32_t)n);
        for (int i = 0; i <= n; i++) {
            s[i] = wave_raw(&wv, ph + (uint32_t)i * step);
            int32_t a = s[i] < 0 ? -s[i] : s[i];
            if (a > pk) pk = a;
        }
    }
    int32_t px = 0, py = 0;
    for (int i = 0; i <= n; i++) {
        int32_t x = OP1_Q10(x0) + OP1_Q10(w) * i / n, yy = OP1_Q10(y) - (int32_t)((int64_t)s[i] * OP1_Q10(h) / pk);
        if (i) LN(px, py, x, yy, 0x500, c);
        px = x; py = yy;
    }
}

static int32_t mg_exp_neg(int32_t e8)                    /* 2^(-e8/256), Q15 */
{
    int32_t n = e8 >> 8;
    return n >= 15 ? 0 : mg_pow2neg[(e8 & 255) >> 3] >> n;
}

/* a small panel knob: circle and pointer around a Q10 centre */
static void mg_knob(void *r, int32_t cx, int32_t cy, int32_t rad, int32_t v, const uint8_t *c, int hl)
{
    int32_t a = ((clampk(v) * 6144) >> 15) - 3072 - 2048;                       /* -135 .. +135 degrees from up */
    mg_circle(r, cx, cy, rad, hl ? 0x600 : 0x400, c, hl ? 16 : 10);            /* fewer lines for the grey ones */
    LN(cx, cy, cx + ((rad * mg_cos(a)) >> 15), cy + ((rad * mg_sin(a)) >> 15), hl ? 0x700 : 0x500, c);
}

/* the four values of a view, each above its dial */
static void mg_values(void *r, int y, int32_t s)
{
    uint8_t c[4];
    mg_shade(c, MG_LIGHT, 220);
    for (int k = 0; k < 4; k++) if (mm_map[k] >= 0) mg_vtext(r, mg_dial_x[k], y, s, mm_map[k], mg_lv(mm_map[k]), c);
}

/* ---- home: the scene ---------------------------------------------------------------------------------------- */

/* The moai of Ahu Tongariki at dusk (moai_scene.h, from the drawing scene/moai_v3c.svg), filtered as the sound is:
 * each stroke is a frequency, the long outlines low and the short details high, and the filter's response there, at
 * the cutoff the sound has now (on a note: the note's, swept by the contour), is its brightness. Closed, the outlines
 * stay; open, the whole drawing; the emphasis lights the strokes at the cutoff white (the resonant peak). On the
 * horizon, left, a ridge is the filter's response (its cliff at the cutoff) with a summit behind it as high as the
 * emphasis, right, the island is the contour (its shape, as high as its amount); the wind in the sky is the noise; detune makes the lines shimmer, drift sways the
 * moai. Oscillators are not shown (their view has them). Top: the number, big, and the four knobs with their names, each
 * shown while it changes and then fading out; what a turning knob changes lights in its colour. */
#include "moai_scene.h"

static void mg_mix(uint8_t *o, const uint8_t *a, const uint8_t *b, int32_t m)   /* m = 0 (a) .. 256 (b) */
{
    for (int i = 0; i < 3; i++) o[i] = (uint8_t)(a[i] + (((b[i] - a[i]) * m) >> 8));
    o[3] = 0xFF;
}
/* the island: the contour as the home view drew it up to v3.19: its depth on the filter (the amount: h px at the most,
 * none without) over a quadratic time axis (0 .. 2.4 s, the key released at 70 %: short plucks get most of the width),
 * x0 .. x0 + w on the horizon */
static void sc_island(void *r, int32_t x0, int32_t w, int32_t h, const uint8_t *c)
{
    h = OP1_Q10(h) * clampk(mg_lv(P_AMOUNT)) / 27853;                          /* the contour knob's most: 27853 */
    if (h < 300) return;
    envc_t ec; contour_coefs(&ec, P);
    const int32_t TREL = 1150, T = 2400, N = 24;
    int32_t sus15 = ec.sus >> 7, ta = P[P_ATTACK] < 400 ? 2 : mg_ms(P[P_ATTACK]), env_r = 0;
    int32_t px = OP1_Q10(x0), py = OP1_Q10(MO_HZ);
    for (int col = 1; col <= N; col++) {
        int32_t t = T * col * col / (N * N), env;
        if (t < ta) env = 32767 * t / ta;
        else if (t < TREL) {
            int32_t e8 = (int32_t)(((int64_t)(t - ta) * ec.dcoef * 509) >> 16);
            env = sus15 + (int32_t)(((int64_t)(32768 - sus15) * mg_exp_neg(e8)) >> 15);
        } else {
            if (!env_r) {                                                        /* the level at the release */
                int32_t d8 = (int32_t)(((int64_t)(TREL - ta) * ec.dcoef * 509) >> 16);
                env_r = TREL < ta ? 32767 * TREL / ta : sus15 + (int32_t)(((int64_t)(32768 - sus15) * mg_exp_neg(d8)) >> 15);
                if (!env_r) env_r = 1;
            }
            int32_t e8 = (int32_t)(((int64_t)(t - TREL) * ec.rcoef * 509) >> 16);
            env = (int32_t)(((int64_t)env_r * mg_exp_neg(e8)) >> 15);
        }
        int32_t x = OP1_Q10(x0) + OP1_Q10(w) * col / N, y = OP1_Q10(MO_HZ) - (int32_t)(((int64_t)h * env) >> 15);
        LN(px, py, x, y, 0x500, c);
        px = x; py = y;
    }
    if (py < OP1_Q10(MO_HZ - 1)) LN(px, py, px + OP1_Q10(3), OP1_Q10(MO_HZ), 0x500, c);   /* still sounding: down to the sea */
}
/* the ridges on the horizon, x0 (20 Hz) .. x0 + w (11 octaves up: the drop stays inside at the highest cutoff), the
 * filter's response as the filter view draws it. In front, low: without the emphasis, at the cutoff the sound has now
 * (on a note: the note's), a plateau and a cliff (6 px at 0 dB, 0.2 px per dB). Behind it, higher: the response the
 * knobs set (P: no LFO, no note), with the emphasis: the plateau runs out to the cutoff and a bump rises there
 * (10 px at 0 dB, 0.3 px per dB); drawn where it rises above the front. Behind that, dim: the same where the contour
 * takes the cutoff (its amount up; drawn where it shows past the knobs' ridge). cf / ce / cg: their colours */
static void sc_ridge(void *r, int32_t x0, int32_t w, int32_t pos, const uint8_t *cf, const uint8_t *ce, const uint8_t *cg)
{
    enum { STEP = 3, NS = 19 };
    int32_t kq = emph_k(P[P_EMPH]), gin = make_up(kq), g0 = make_up(0);
    int32_t db0 = mg_resp(-4 * 1024, 0, g0), dbk = mg_resp(-4 * 1024, kq, gin);
    int32_t set = (P[P_CUTOFF] * 10 * 1024) >> 15, far = set + (P[P_AMOUNT] >> 2);
    if (far > MAX_POS) far = MAX_POS;
    int32_t fh[NS], bh[NS], gh[NS];
    for (int i = 0; i < NS; i++) {                                               /* every STEP px */
        int32_t x = i * STEP, o = x * 11 * 1024 / w;
        int32_t f = OP1_Q10(6) + (mg_resp(o - pos, 0, g0) - db0) / 5;
        f += (mg_sin(x * 700) + mg_sin(x * 1900 + 900) / 2) >> 7;               /* rocks */
        int32_t b = OP1_Q10(10) + (mg_resp(o - set, kq, gin) - dbk) * 3 / 10;
        int32_t g = OP1_Q10(10) + (mg_resp(o - far, kq, gin) - dbk) * 3 / 10;
        if (x < 9) { f = f * x / 9; b = b * x / 9; g = g * x / 9; }             /* the left flanks */
        fh[i] = f < 0 ? 0 : f; bh[i] = b < 0 ? 0 : b; gh[i] = g < 0 ? 0 : g;
    }
    for (int i = 1; i < NS; i++) {
        int32_t xa = OP1_Q10(x0 + (i - 1) * STEP), xb = OP1_Q10(x0 + i * STEP), y = OP1_Q10(MO_HZ);
        if ((gh[i - 1] > bh[i - 1] + 256 || gh[i] > bh[i] + 256) && (gh[i - 1] > fh[i - 1] || gh[i] > fh[i]))
            LN(xa, y - gh[i - 1], xb, y - gh[i], 0x500, cg);
        if (bh[i - 1] > fh[i - 1] || bh[i] > fh[i]) LN(xa, y - bh[i - 1], xb, y - bh[i], 0x500, ce);
        LN(xa, y - fh[i - 1], xb, y - fh[i], 0x500, cf);
    }
}
/* the wind: streaks across the sky, more and faster with the noise, passing behind the moai */
static void sc_wind(void *r, int32_t nz, const uint8_t *c)
{
    int n = 1 + nz * 5 / 32767;
    for (int j = 0; j < n; j++) {
        int32_t len = 16 + 7 * (j % 3), y = 36 + 8 * j + (j & 1) * 3, a = -1;      /* under the number and knobs */
        int32_t x0 = (int32_t)((mg_t * (uint32_t)(2 + (j & 1) + nz / 12000) + (uint32_t)j * 113) % 380) - 40;
        for (int32_t x = x0; ; x += 2) {                                         /* the runs of it in the open sky */
            int end = x > x0 + len, clear = !end && x >= 0 && x < 320 && MO_SKY[x >> 2] > y + 2;
            if (clear) { if (a < 0) a = x; }
            else {
                if (a >= 0 && x - 2 > a) LN(OP1_Q10(a), OP1_Q10(y), OP1_Q10(x - 2), OP1_Q10(y), 0x400, c);
                a = -1;
            }
            if (end) break;
        }
    }
}
/* the moai: stroke s at the frequency 1 + 9 s / (MO_N - 1) octaves, against the cutoff at 3 + 0.8 x its position
 * (octaves above 20 Hz): closed, the outlines stay. Below the cutoff the response is spread twice as wide, so the
 * emphasis lights more strokes. db0: the pass band's level; fuzz: Q10 px of shimmer per point; sway: Q10 px at 128 px
 * up. hc, ha: the last knob turned, the strokes at the cutoff in its colour (ha 0 .. 256: as long as it shows) */
static void sc_moai(void *r, int32_t pos, int32_t kq, int32_t gin, int32_t db0, int32_t fuzz, int32_t sway,
                    const uint8_t *hc, int32_t ha)
{
    int32_t thr = 3 * 1024 + pos * 4 / 5;
    uint32_t z = (mg_t >> 1) * 2654435761u;                                      /* the shimmer: new every 2nd frame */
    uint8_t c[4];
    for (int s = 0; s < MO_N; s++) {
        int32_t o = 1024 + s * 9 * 1024 / (MO_N - 1) - thr;
        if (o > 1536) break;                                                     /* the rest: under -30 dB */
        int32_t db = mg_resp(o < 0 ? o / 2 : o, kq, gin) - db0, l = 256 + db * 256 / (30 * 1024);   /* Q10 dB; -30 dB: dark */
        if (l < 12) continue;
        const uint8_t *rgb = MO_RGB[MO_COL[s]];
        if (db > 512) mg_mix(c, rgb, MG_LIGHT, db > 6 * 1024 ? 256 : (db - 512) / 22);         /* the peak: towards white */
        else mg_shade(c, rgb, l > 256 ? 256 : l);
        int32_t w = db > 3 * 1024 ? 0x600 : 0x500, px = 0, py = 0;
        if (ha && o > -1640 && o < 820) { mg_mix(c, c, hc, ha); w += (0x780 - w) * ha >> 8; }   /* thicker: red on pink */
        const int16_t *p = MO_PTS + 2 * MO_FIRST[s];
        for (int i = MO_FIRST[s]; i < MO_FIRST[s + 1]; i++, p += 2) {
            int32_t X = p[0] * 256, Y = p[1] * 256;                              /* quarter px -> Q10 */
            X += (int32_t)(((int64_t)(OP1_Q10(140) - Y) * sway) >> 17);
            if (fuzz) {
                z = z * 1664525u + 1013904223u; X += (int32_t)(((int64_t)((int32_t)(z >> 16) - 32768) * fuzz) >> 15);
                z = z * 1664525u + 1013904223u; Y += (int32_t)(((int64_t)((int32_t)(z >> 16) - 32768) * fuzz) >> 15);
            }
            if (i > MO_FIRST[s]) LN(px, py, X, Y, w, c);
            px = X; py = Y;
        }
    }
}
/* the sound number, large, from x, y (top left, px), h px high, at level l (0 .. 256: it fades out), white or blue (the
 * knob turned it): on the OP-1 in the stock font (the text object the mixer's big volume digits use, built once per
 * colour: three levels of white and of blue for the fade); in host builds (the previews) in our own line digits
 * (moai_scene.h) */
#ifndef OP1_HOST
static uint32_t sc_num_tx[6][OP1_TEXT_SIZE / 4];
static char sc_num_s[6][8];
static uint8_t sc_num_col[6][4], sc_num_made;
#endif
static void sc_number(void *r, int32_t x, int32_t y, int32_t h, const char *t, int32_t l, int blue)
{
#ifndef OP1_HOST
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    static const int16_t LV[3] = { 256, 160, 80 };
    if (!sc_num_made) {
        for (int i = 0; i < 6; i++) {
            mg_shade(sc_num_col[i], i < 3 ? MG_LIGHT : MG_COL[0], LV[i % 3]);
            op1_text_ctor(sc_num_tx[i], OP1_Q10(x), y, h * 5 / 4, sc_num_col[i]);   /* size: about 1.25 x the digits */
        }
        sc_num_made = 1;
    }
    int i = l >= 200 ? 0 : l >= 120 ? 1 : l >= 40 ? 2 : -1;
    if (i < 0) return;
    i += blue ? 3 : 0;
    char *was = sc_num_s[i];
    int same = 1;
    for (int j = 0; j < 8; j++) { if (was[j] != t[j]) { same = 0; break; } if (!t[j]) break; }
    if (!same) { op1_text_set(sc_num_tx[i], t); for (int j = 0; j < 7 && (was[j] = t[j]); j++) ; was[7] = 0; }
    op1_text_draw(sc_num_tx[i], r, ident);
#else
    uint8_t c[4];
    if (l < 40) return;
    mg_shade(c, blue ? MG_COL[0] : MG_LIGHT, l);
    for (; *t; t++, x += h * 19 / 22) {
        if (*t < '0' || *t > '9') continue;
        int d = *t - '0', pen = 1;
        int32_t px = 0, py = 0;
        for (int i = MO_DIGF[d]; i < MO_DIGF[d + 1]; i++) {
            const int16_t *p = MO_DIG + 2 * i;
            if (p[0] < 0) { pen = 1; continue; }
            int32_t X = OP1_Q10(x) + p[0] * h * 32 / 5, Y = OP1_Q10(y) + p[1] * h * 32 / 5;   /* tenths of h / 16 */
            if (!pen) LN(px, py, X, Y, 0x800, c);
            px = X; py = Y; pen = 0;
        }
    }
#endif
}
/* a home knob: thicker than the panel's */
static void sc_knob(void *r, int32_t cx, int32_t cy, int32_t rad, int32_t v, const uint8_t *c)
{
    int32_t a = ((clampk(v) * 6144) >> 15) - 3072 - 2048;                       /* -135 .. +135 degrees from up */
    mg_circle(r, cx, cy, rad, 0x900, c, 16);
    LN(cx, cy, cx + ((rad * mg_cos(a)) >> 15), cy + ((rad * mg_sin(a)) >> 15), 0xB00, c);
}
/* in frames (about 60 a second). Turning: a knob turned in the last SC_TURN frames (its colour in the scene, fading
 * out over as many again). The number (when it changes, or home comes back) and the knobs (while one turns, or Shift
 * is held): unchanged for SC_HOLD frames after, then one fade over SC_FALL */
enum { SC_TURN = 20, SC_HOLD = 50, SC_FALL = 20, SC_SHIFT_HOLD = 20 };
static uint16_t sc_kf[4] = { 999, 999, 999, 999 }, sc_shf = 999, sc_nf;
static uint8_t sc_nblue;                                                         /* the blue knob turned the number */
static int32_t sc_num_was = -1;
static uint32_t sc_home_t;                                                       /* the last frame home was drawn */
static void sc_age(uint16_t *f) { if (*f < 999) ++*f; }
static int32_t sc_fade(int32_t f, int32_t hold) { return f <= hold ? 256 : f >= hold + SC_FALL ? 0 : 256 - (f - hold) * 256 / SC_FALL; }

static void view_home(void *r)
{
    uint8_t c[4];
    int lit[4];                                                                  /* a home knob turning: what it moves lights */
    for (int k = 0; k < 4; k++) lit[k] = !ui_layer && ui_idle[k] < SC_TURN;
    /* the cutoff the sound has now (on a note: the note's) */
    int32_t kq = emph_k(mg_lv(P_EMPH)), gin = make_up(kq), pos = (mg_lv(P_CUTOFF) * 10 * 1024) >> 15;
    if (mm_last < OP1_VOICES) {
        const mm_voice_t *v = &V[mm_last];
        if (v->stage != 2 || v->env > (1 << 16)) pos = v->pos;
    }
    int32_t db0 = mg_resp(-4 * 1024, kq, gin);                                   /* the pass band: 0 dB */
    /* the sky: the wind; the horizon: the filter's ridges (left), the contour's island (right) */
    int32_t nz = clampk(P[P_NOISE]);
    mg_shade(c, MG_LIGHT, 130);
    if (nz > 300) sc_wind(r, nz, c);
    uint8_t ce[4], cg[4];
    if (lit[1]) mg_shade(c, MG_COL[1], 256); else mg_shade(c, MO_RGB[3], 230);
    if (lit[1] || lit[2]) mg_shade(ce, MG_COL[lit[2] ? 2 : 1], 256); else mg_shade(ce, MO_RGB[2], 190);
    mg_shade(cg, lit[3] ? MG_COL[3] : MO_RGB[2], lit[3] ? 150 : 80);
    sc_ridge(r, 17, 56, pos, c, ce, cg);
    mg_shade(c, lit[3] ? MG_COL[3] : MO_RGB[2], lit[3] ? 256 : 220);
    sc_island(r, 249, 54, 15, c);
    /* the moai; the last knob turned (either layer): its colour on the strokes at the cutoff while it turns */
    int32_t det = detune_q16(mg_lv(P_DETUNE)) * 1800 / 1514, dr = drift_amount(mg_lv(P_DRIFT));   /* Q10 px */
    int32_t sway = ((int32_t)(((int64_t)mg_sin((int32_t)(mg_t * 23)) * dr) >> 15) * 3) >> 5;
    int hk = 0;
    for (int k = 1; k < 4; k++) if (ui_idle[k] < ui_idle[hk]) hk = k;
    int32_t hi = ui_idle[hk], ha = hi <= SC_TURN ? 256 : hi >= 2 * SC_TURN ? 0 : (2 * SC_TURN - hi) * 256 / SC_TURN;
    sc_moai(r, pos, kq, gin, db0, det, sway, MG_COL[hk], ha);
    /* top: the number and the knobs with their names (Shift: the second layer's) */
    if (sc_home_t + 1 != mg_t) { sc_nf = 0; sc_nblue = 0; }
    sc_home_t = mg_t;
    char t[24];
    int32_t num = P[P_SETUP];                                                    /* (not mg_lv: no LFO here) */
    if (lit[0]) { sc_nf = 0; sc_nblue = 1; }
    else if (num != sc_num_was) { sc_nf = 0; sc_nblue = 0; }
    else sc_age(&sc_nf);
    sc_num_was = num;
    mg_value(t, P_SETUP, num);
    sc_number(r, 7, 5, 22, t, sc_fade(sc_nf, SC_HOLD), sc_nblue);
    if (ui_layer) sc_shf = 0; else sc_age(&sc_shf);
    for (int k = 0; k < 4; k++) {
        int id = mm_map[k];
        if (ui_idle[k] < SC_TURN) sc_kf[k] = 0; else sc_age(&sc_kf[k]);
        int32_t a = sc_fade(sc_kf[k], SC_HOLD), b = sc_fade(sc_shf, SC_SHIFT_HOLD);
        if (b > a) a = b;
        if (id < 0 || a < 16) continue;
        int x = 217 + 27 * k;
        mg_shade(c, MG_COL[k], a);
        sc_knob(r, OP1_Q10(x), OP1_Q10(14), OP1_Q10(10), mg_norm(id, mg_lv(id)), c);
        mg_ctext(r, x, 28, 850, MG_SHORT[id], c);
    }
}

/* ---- views ------------------------------------------------------------------------------------------------- */

static void view_advanced(void *r)
{
    uint8_t c[4];
    /* the Minimoog's section names, centred over their columns; thin lines between the sections */
    static const struct { int8_t c0, c1; char t[12]; } SEC[] = {
        { 0, 2, "OSCILLATORS" }, { 3, 4, "MIXER" }, { 5, 8, "MODIFIERS" }, { 9, 10, "ANALOG" } };
    for (int i = 0; i < 4; i++) {
        mg_ctext(r, (ADV_C(SEC[i].c0) + ADV_C(SEC[i].c1)) / 2, 2, 800, SEC[i].t, MG_GREY);
        if (i) {
            int32_t x = OP1_Q10(ADV_C(SEC[i].c0) + ADV_C(SEC[i - 1].c1)) / 2;
            LN(x, OP1_Q10(4), x, OP1_Q10(ADV_R(2) + 16), 0x400, MG_DARK);
        }
    }
    int32_t xh0 = OP1_Q10(ADV_C(9) - 10), xh1 = OP1_Q10(ADV_C(10) + 10), yh = OP1_Q10(ADV_R(2) - 12);
    LN(xh0, yh, xh1, yh, 0x400, MG_DARK);                                        /* the home knobs apart */
    for (int i = 0; i < 3; i++) { char n[2] = { (char)('1' + i), 0 }; mg_ctext(r, 4, ADV_R(i) - 4, 800, n, MG_GREY); }
    for (int i = 0; i < N_ADV; i++) {
        int id = ADV[i].id, hl = -1;
        for (int k = 0; k < 4; k++) if (ui_hl[k] == id) hl = k;
        if (hl >= 0) mg_shade(c, MG_COL[hl], 256);
        else if (id == P_O3_VOL && mm_osc3_off) mg_shade(c, MG_COL[3], 120);
        else mg_shade(c, MG_GREY, 256);
        mg_knob(r, OP1_Q10(ADV[i].x), OP1_Q10(ADV[i].y), OP1_Q10(hl >= 0 ? 8 : 7), mg_norm(id, mg_lv(id)), c, hl >= 0);
        if (hl < 0) mg_shade(c, MG_GREY, 170);
        mg_ctext(r, ADV[i].x, ADV[i].y + 10, 700, MG_SHORT[id], c);              /* (the row is the oscillator) */
    }
}

/* the scope: the latest note's real output (mm_scope, a ring the engine fills: 64 word copies per block of the newest
 * voice, about half a cycle per sample). Everything else happens here, in the GUI thread, on at most 65 points:
 * - the window is two cycles of the lowest oscillator sounding (its range: a 32' bass has a cycle 4 times the key's);
 *   for long cycles the engine keeps every 2nd .. 16th sample, so two cycles always fit in the ring (down to 11 Hz);
 * - triggers are rising zero crossings armed by a dip below -1/8 of the peak (hysteresis: noise and ripples near zero
 *   do not trigger), placed between samples (no one-sample jitter);
 * - of the triggers in one cycle, the one whose trace looks most like the last frame's wins (the others are the same
 *   cycle's other crossings: resonance, pulses and detuned oscillators have several);
 * - the trace is averaged with the last one and the peak scale falls slowly, so detune beating morphs instead of
 *   flickering. A new key starts afresh.
 * Without a note (or once the ring stops moving: the firmware no longer renders the voice): the selected oscillator's
 * ideal shape at its volume, dimmer. */
#define SC_PTS 64
static int16_t sc_tr[SC_PTS + 1];       /* the last trace (samples) */
static int32_t sc_per, sc_pk = 2048;     /* its period (0: none) and peak scale */
static uint16_t sc_from;                /* where the ring's samples at a new decimation start */
static uint8_t sc_filling;              /* 1: until enough of them */
static uint16_t sc_last_w;              /* the ring's write position at the last frame */
static uint8_t sc_still;                /* frames it has not moved (the voice no longer renders) */

static int32_t sc_at(int32_t base, int32_t q8)                      /* the ring at base + q8 / 256, interpolated */
{
    int32_t i = base + (q8 >> 8), f = q8 & 255;
    int32_t a = mm_scope[i & (SCOPE_N - 1)], b = mm_scope[(i + 1) & (SCOPE_N - 1)];
    return a + (((b - a) * f) >> 8);
}

static void mg_scope(void *r, int x0, int y0, int x1, int y1, int osc)
{
    uint8_t c[4];
    mg_box(r, x0, y0, x1, y1, MG_DARK);
    int32_t ym = (y0 + y1) / 2, h = (y1 - y0) / 2 - 4;
    if (osc == 2 && mm_osc3_off) {                                               /* osc 3 rests: why */
        mg_shade(c, MG_COL[3], 230);
        mg_ctext(r, (x0 + x1) / 2, ym - 17, 1024, "OSC 3 RESTS", c);
        mg_ctext(r, (x0 + x1) / 2, ym - 4, 1024, mm_osc3_why == 2 ? "IN UNISON" : "4+ NOTES PLAY", c);
        mg_shade(c, MG_LIGHT, 160);
        mg_ctext(r, (x0 + x1) / 2, ym + 12, 800, "BACK WITH 2 OR LESS", c);
        return;
    }
    LN(OP1_Q10(x0), OP1_Q10(ym), OP1_Q10(x1), OP1_Q10(ym), 0x400, MG_DARK);
    const mm_voice_t *v = mm_last < OP1_VOICES ? &V[mm_last] : 0;
    sc_still = mm_scope_w != sc_last_w ? 0 : (sc_still < 255 ? sc_still + 1 : 255);
    sc_last_w = mm_scope_w;
    if (!v || !v->key_inc || (v->stage == 2 && v->env < (1 << 14)) || sc_still > 6) {   /* no note: the ideal shape */
        sc_per = 0;
        int32_t lv = clampk(osc == 3 ? P[P_NOISE] : P[P_O(osc, 3)]);
        if (osc == 3) {                                                          /* the noise row: grains */
            if (lv <= 300) return;
            uint32_t z = mg_t * 2654435761u;
            int32_t sp = (int32_t)(((int64_t)OP1_Q10(h) * 5 / 6 * lv) >> 15);
            mg_shade(c, MG_COL[0], 160);
            for (int i = 0; i < 24; i++) {
                z = z * 1664525u + 1013904223u;
                int32_t x = OP1_Q10(x0 + 4) + (int32_t)((z >> 8) % ((uint32_t)(x1 - x0 - 8) << 10));
                int32_t y = OP1_Q10(ym) + (int32_t)((int32_t)(z >> 16) % (sp > 0 ? sp : 1));
                LN(x, y, x + OP1_Q10(1), y, 0x600, c);
            }
            return;
        }
        if (P[P_O(osc, 0)] == R_OFF) { mg_ctext(r, (x0 + x1) / 2, ym - 4, 1024, "OFF", MG_GREY); return; }
        mg_shade(c, MG_COL[0], 160);
        mg_glyph(r, P[P_O(osc, 1)], x0 + 4, ym, x1 - x0 - 8, (h * 5 * lv) / (6 * 32767), 2, 0, 48, c);
        return;
    }
    int lo = 9;                                                                 /* the lowest range sounding */
    for (int i = 0; i < 3; i++) {
        int rg = P[P_O(i, 0)];
        if (rg > R_LO && P[P_O(i, 3)] && !(i == 2 && mm_osc3_off) && RANGE_OCT[rg] < lo) lo = RANGE_OCT[rg];   /* (LO: ticks, no pitch) */
    }
    if (lo == 9) lo = 0;
    uint32_t pf = 0xFFFFFFFFu / v->key_inc;                                     /* samples per cycle at 8' */
    pf = lo < 0 ? pf << -lo : pf >> lo;
    int sh = 0;
    while ((pf >> sh) > 256 && sh < 4) sh++;                                    /* the ring's decimation */
    int32_t per = (int32_t)(pf >> sh);
    if (per < 4) per = 4;
    if (per > 256) per = 256;
    if (sh != mm_scope_sh) { mm_scope_sh = (uint8_t)sh; sc_from = mm_scope_w; sc_filling = 1; }
    int32_t n = 2 * per;                                                        /* the window */
    if (sc_filling && ((mm_scope_w - sc_from) & (SCOPE_N - 1)) < n + per + 2) {   /* not enough at this rate yet */
        sc_per = 0;
        return;
    }
    sc_filling = 0;
    int fresh = per != sc_per;
    sc_per = per;
    int32_t s0 = mm_scope_w - n - per - 2;              /* oldest read <= 770 back: the writer stays ahead */
    /* triggers in one cycle, best kept */
    int32_t th = sc_pk >> 3, best = per << 8, best_score = 0x7FFFFFFF, armed = 0;
    for (int32_t i = 1; i <= per; i++) {
        int32_t a = mm_scope[(s0 + i - 1) & (SCOPE_N - 1)], b = mm_scope[(s0 + i) & (SCOPE_N - 1)];
        if (a < -th) armed = 1;
        if (!armed || a >= 0 || b < 0) continue;
        armed = 0;
        int32_t q8 = ((i - 1) << 8) + (-a << 8) / (b - a), score = 0;
        if (fresh) score = a - b;                                                /* no last frame: the steepest */
        else for (int j = 0; j <= SC_PTS; j += 2) {
            int32_t d = sc_at(s0, q8 + j * n * 4) - sc_tr[j];
            score += d < 0 ? -d : d;
        }
        if (score < best_score) { best_score = score; best = q8; }
    }
    int32_t pk = 2048;
    for (int j = 0; j <= SC_PTS; j++) {
        int32_t s = sc_at(s0, best + j * n * 4);
        if (!fresh) s = (s + sc_tr[j]) >> 1;
        sc_tr[j] = (int16_t)s;
        if (s < 0) s = -s;
        if (s > pk) pk = s;
    }
    sc_pk = fresh || pk > sc_pk ? pk : sc_pk - ((sc_pk - pk) >> 4);
    mg_shade(c, MG_COL[0], 256);
    int32_t px = 0, py = 0;
    for (int j = 0; j <= SC_PTS; j++) {
        int32_t x = OP1_Q10(x0 + 3) + OP1_Q10(x1 - x0 - 6) * j / SC_PTS, y = OP1_Q10(ym) - (int32_t)((int64_t)sc_tr[j] * OP1_Q10(h) / sc_pk);
        if (j) LN(px, py, x, y, 0x500, c);
        px = x; py = y;
    }
}

/* the Minimoog's mixer as rows: oscillators 1..3 (range, wave, frequency, volume) and the noise (its level only);
 * the selected row (cur: 0..2, 3 = noise) in colour, the scope on the right */
static void view_osc(void *r, int cur)
{
    static const int16_t KX[4] = { 36, 74, 112, 150 };
    uint8_t c[4], c2[4];
    for (int i = 0; i < 3; i++) {
        int y = 14 + i * 25, on = i == cur, rest = i == 2 && mm_osc3_off;
        char n[2] = { (char)('1' + i), 0 };
        mg_shade(c, on ? MG_COL[1] : MG_GREY, on ? 256 : 170);
        mg_text(r, 6, y - 6, 1536, n, c);
        for (int k = 0; k < 4; k++) {
            int id = P_O(i, k);
            if (on) mg_shade(c, MG_COL[k], 256); else mg_shade(c, MG_GREY, 256);
            if (rest && k == 3) mg_shade(c, MG_COL[3], on ? 200 : 110);
            mg_knob(r, OP1_Q10(KX[k]), OP1_Q10(y), OP1_Q10(on ? 8 : 7), mg_norm(id, mg_lv(id)), c, on);
            mg_shade(c2, on ? MG_LIGHT : MG_GREY, on ? 230 : 170);
            mg_vtext(r, KX[k], y + 10, 700, id, mg_lv(id), c2);
        }
        if (rest) { mg_shade(c, MG_COL[3], 230); mg_text(r, KX[3] + 12, y - 3, 800, mm_osc3_why == 2 ? "UNI" : "POLY", c); }
    }
    {                                                                            /* modulation (mix, osc, filter) and noise */
        static const int8_t ID[4] = { P_MOD_MIX, P_MOD_PITCH, P_MOD_FILT, P_NOISE };
        int y = 89, on = cur == 3;
        mg_shade(c, on ? MG_COL[1] : MG_GREY, on ? 256 : 170);
        mg_text(r, 6, y - 6, 1536, "M", c);
        for (int k = 0; k < 4; k++) {
            if (on) mg_shade(c, MG_COL[k], 256); else mg_shade(c, MG_GREY, 256);
            mg_knob(r, OP1_Q10(KX[k]), OP1_Q10(y), OP1_Q10(on ? 8 : 7), mg_lv(ID[k]), c, on);
            mg_shade(c2, on ? MG_LIGHT : MG_GREY, on ? 230 : 170);
            mg_vtext(r, KX[k], y + 10, 700, ID[k], mg_lv(ID[k]), c2);
        }
    }
    mg_scope(r, 182, 6, 314, 102, cur);                                          /* as high as the rows */
    if (cur == 3 && (mod_depth(mg_lv(P_MOD_PITCH), MOD_PITCH_MAX) || mod_depth(mg_lv(P_MOD_FILT), MOD_FILT_MAX))
        && P[P_O3_RANGE] != R_LO && mg_lv(P_MOD_MIX) <= 64) {                   /* modulation, but no source */
        mg_shade(c, MG_COL[3], 230);
        mg_ctext(r, 248, 12, 800, "SET OSC 3 TO LO", c);
    }
}

static void view_filter(void *r)
{
    static const plot_t pl = { 14, 306, 4, 80, 30, 29901, 3, 4 };              /* 3 px per 4 dB */
    mg_filter_plot(r, &pl);
    mg_values(r, 92, 1024);
}

static void view_contour(void *r)
{
    uint8_t c[4];
    mg_box(r, 14, 4, 306, 84, MG_DARK);
    /* OP-1 style: attack rises, decay falls to the sustain, it holds, release falls; widths follow the times */
    int32_t wa = 10 + time_index(mg_lv(P_ATTACK)) / 2, wd = 10 + time_index(mg_lv(P_DECAY)) / 2, wr = 10 + time_index(mg_lv(P_RELEASE)) / 2;
    int32_t ws = 60, tot = wa + wd + ws + wr, W = 280;
    int32_t x0 = 20, xa = x0 + wa * W / tot, xd = xa + wd * W / tot, xs = xd + ws * W / tot, xr = xs + wr * W / tot;
    int32_t yb = 78, yt = 10, ysus = yb - (int32_t)(((int64_t)(yb - yt) * clampk(mg_lv(P_SUSTAIN))) >> 15);
    const int32_t X[5] = { x0, xa, xd, xs, xr }, Y[5] = { yb, yt, ysus, ysus, yb };
    for (int s = 0; s < 4; s++) {
        mg_shade(c, MG_COL[s], 256);
        if (s == 1 || s == 3) {                                                  /* exponential segments */
            int32_t px = OP1_Q10(X[s]), py = OP1_Q10(Y[s]);
            for (int i = 1; i <= 12; i++) {
                int32_t e = mg_exp_neg(i * 256 * 4 / 12);                       /* 2^(-4 i / 12) */
                int32_t x = OP1_Q10(X[s]) + OP1_Q10(X[s + 1] - X[s]) * i / 12;
                int32_t y = i == 12 ? OP1_Q10(Y[s + 1]) : OP1_Q10(Y[s + 1]) + (int32_t)(((int64_t)OP1_Q10(Y[s] - Y[s + 1]) * e) >> 15);
                LN(px, py, x, y, 0x700, c); px = x; py = y;
            }
        } else LN(OP1_Q10(X[s]), OP1_Q10(Y[s]), OP1_Q10(X[s + 1]), OP1_Q10(Y[s + 1]), 0x700, c);
    }
    mg_shade(c, MG_GREY, 200);
    LN(OP1_Q10(xs), OP1_Q10(yt), OP1_Q10(xs), OP1_Q10(yb), 0x400, c);            /* key up */
    mg_values(r, 88, 1024);
    for (int k = 0; k < 4; k++) {                                                /* on the tempo: the time in ms too */
        int j = time_slot(mm_map[k]);
        if (j < 0 || ui_sync[j] < 0) continue;
        char t[12], *e = sq_num(t, (uint32_t)(mm_map[k] == P_ATTACK && mg_lv(P_ATTACK) < 400 ? 2 : mg_ms(mg_lv(mm_map[k]))), 1);
        sq_cat(e, "MS");
        mg_ctext(r, mg_dial_x[k], 96, 700, t, MG_GREY);
    }
}

static int32_t mg_clip3(int32_t x)                       /* the engine's cubic soft clip, L = 32768: out to +-2L/3 */
{
    if (x >= 32768) return 21845;
    if (x <= -32768) return -21845;
    return x - (int32_t)((int64_t)x * x / 32768 * x / (3 * 32768));
}

/* the sound's oscillators (each at its volume) and the noise on one scope, triggered on osc 1: detune slides osc 2
 * (sharp) and osc 3 (flat, half as much) along it and drift wobbles them (both exaggerated to be seen); drive pushes
 * them into the filter input's soft clip (dashed: its limits), the saturation into the output stage's (off centre: one
 * side flattens first); a little bandwidth, as an analog stage has, rounds the edges */
static void view_analog(void *r)
{
    enum { X0 = 18, X1 = 302, YM = 44, A = 30, STEP = 3 };                    /* px; the clip's limit at YM +- A */
    uint8_t c[4];
    mg_box(r, 14, 4, 306, 84, MG_DARK);
    LN(OP1_Q10(14), OP1_Q10(YM), OP1_Q10(306), OP1_Q10(YM), 0x400, MG_DARK);
    for (int x = X0; x < X1; x += 8) for (int s = -A; s <= A; s += 2 * A)
        LN(OP1_Q10(x), OP1_Q10(YM + s), OP1_Q10(x + 3), OP1_Q10(YM + s), 0x400, MG_DARK);
    int32_t det = detune_q16(mg_lv(P_DETUNE)), dr = drift_amount(mg_lv(P_DRIFT)), drv = drive_q12(mg_lv(P_DRIVE));
    int32_t sat = clampk(mg_lv(P_SAT)), sg = sat_gain(sat), bias = (sat * 1229) >> 13, dc = mg_clip3(bias);
    int lo = 9;                                                                  /* the lowest range sounding */
    for (int i = 0; i < 3; i++) { int rg = P[P_O(i, 0)]; if (rg > R_LO && RANGE_OCT[rg] < lo) lo = RANGE_OCT[rg]; }
    for (int i = 3; i >= 0; i--) {                                               /* the noise below, osc 1 on top */
        int32_t lv = clampk(i == 3 ? P[P_NOISE] : P[P_O(i, 3)]);
        if (lv <= 300) continue;
        uint32_t z = (mg_t >> 2) * 2654435761u;                                  /* the noise: new every 4th frame */
        int rg = i == 3 ? R8 : P[P_O(i, 0)];
        if (i < 3 && (rg <= R_LO || (i == 2 && mm_osc3_off))) continue;
        /* 2 cycles across for the lowest range, x 2 per octave above it, x the interval */
        int32_t e = i == 3 ? 4096 : (RANGE_OCT[rg] - lo) * 1024 + pclamp(P_O(i, 2), P[P_O(i, 2)]) * 1024 / 1200 + 4096;
        uint32_t cyc = (uint32_t)mg_exp2_q12(e < 0 ? 0 : e) >> 3;               /* Q12 */
        if (cyc > 12 * 4096) cyc = 12 * 4096;
        int32_t d = i == 1 ? det : (i == 2 ? -det / 2 : 0);
        uint32_t inc = (uint32_t)(((((uint64_t)cyc << 20) / (X1 - X0)) * (uint64_t)(65536 + 8 * d)) >> 16) * STEP;
        uint32_t p = ((mg_t * (uint32_t)d) << 14) + (i ? (uint32_t)((mg_sin((int32_t)(mg_t * (uint32_t)(6 + 5 * i))) * dr) >> 15) << 14 : 0);
        wave_t wv; wave_of(&wv, i == 3 ? W_SAW : P[P_O(i, 1)]);
        p -= 4 * inc;                                                            /* the bandwidth's warm-up */
        int32_t lp = 0, px = 0, py = 0;
        for (int x = X0 - 4 * STEP; x <= X1; x += STEP, p += inc) {
            int32_t w = i == 3 ? (int32_t)((z = z * 1664525u + 1013904223u) >> 16) - 32768 : wave_raw(&wv, p);
            if (i == 3) w = (w * 23170) >> 15;                                   /* (the engine's noise level) */
            int32_t y = mg_clip3((int32_t)(((((int64_t)w * lv) >> 15) * drv * 19661) >> 27));   /* x volume x 0.6 x drive */
            if (sat > 64) y = mg_clip3((int32_t)(((int64_t)y * sg) >> 12) + bias) - dc;
            lp += (y - lp) >> 1;
            if (x < X0) { if (x == X0 - 4 * STEP) lp = y; continue; }
            int32_t yy = OP1_Q10(YM) - lp * OP1_Q10(A) / 21845;
            if (i == 3) mg_shade(c, MG_GREY, 256);
            else mg_shade(c, i == 0 ? MG_LIGHT : MG_COL[i == 1 ? 0 : 1], i == 0 ? 230 : 200);
            if (x > X0) LN(px, py, OP1_Q10(x), yy, 0x500, c);
            px = OP1_Q10(x); py = yy;
        }
    }
    mg_values(r, 90, 1024);
}

/* ---- frame ------------------------------------------------------------------------------------------------- */

static void mg_screen_init(void *scr) { (void)scr; }

/* the four dials under the views, as moai.svg drew them until 2026-11-03 (a parsed SVG node costs R246's drawing
 * heap about 235 bytes): grey ticks at -135, 0 and +135 degrees (radius 11 .. 13, 2 px, square caps), a dot of
 * radius 2.8 in the knob's colour and a 9 px pointer from the centre (2 px, square caps) at the view's parameter */
static void mg_dials(void *r)
{
    static const int16_t HW[11] = { 1291, 2007, 2421, 2678, 2821, 2867, 2821, 2678, 2421, 2007, 1291 };   /* the dot: */
                                                                 /* rows every 0.5 px, half widths sqrt(2.8^2 - y^2), Q10 */
    for (int i = 0; i < 4; i++) {
        int32_t cx = OP1_Q10(mg_dial_x[i]), cy = OP1_Q10(MG_DIAL_Y);
        for (int k = -1; k <= 1; k++) {
            int32_t s = mg_sin(k * 3072), c = mg_cos(k * 3072);
            LN(cx + ((OP1_Q10(10) * s) >> 15), cy - ((OP1_Q10(10) * c) >> 15),
               cx + ((OP1_Q10(14) * s) >> 15), cy - ((OP1_Q10(14) * c) >> 15), 0x800, MG_GREY);
        }
        int id = mm_map[i];
        int32_t a = ((id < 0 ? 0 : mg_norm(id, mg_lv(id))) * 6144 >> 15) - 3072;
        int32_t s = mg_sin(a), c = mg_cos(a);
        LN(cx - ((OP1_Q10(1) * s) >> 15), cy + ((OP1_Q10(1) * c) >> 15),
           cx + ((OP1_Q10(10) * s) >> 15), cy - ((OP1_Q10(10) * c) >> 15), 0x800, MG_COL[i]);
        for (int k = 0; k < 11; k++) LN(cx - HW[k], cy + (k - 5) * 512, cx + HW[k], cy + (k - 5) * 512, 0x500, MG_COL[i]);
    }
}

static void mg_screen_update(void *scr, int dt)
{
    mm_ui_frame();
    mg_lfo_frame();
    mg_t++;
    int view = mm_view;
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    if (view != V_HOME) mg_dials(r);                                             /* home: its own small knobs */
    switch (view) {
    case V_HOME: view_home(r); break;
    case V_ADV: view_advanced(r); break;
    case V_OSC1: case V_OSC2: case V_OSC3: view_osc(r, view - V_OSC1); break;
    case V_NOISE: view_osc(r, 3); break;
    case V_FILTER: view_filter(r); break;
    case V_CONTOUR: view_contour(r); break;
    default: view_analog(r); break;
    }
    uint8_t c[4];                                                                /* labels above the dials */
    for (int k = 0; k < 4 && view != V_HOME; k++) {
        int id = mm_map[k];
        if (id < 0) continue;
        mg_shade(c, MG_COL[k], 230);
        if (view == V_ADV) {                                                     /* + the value */
            char t[24]; mg_label(t, id);
            char *e = t; while (*e) e++;
            *e++ = ' '; mg_value(e, id, mg_lv(id));
            mg_ctext(r, mg_dial_x[k], MG_LABEL_Y, 850, t, c);
        } else mg_ctext(r, mg_dial_x[k], MG_LABEL_Y, 1024, MG_SHORT[id], c);
    }
}

#pragma GCC pop_options
const op1_screen_t SCREEN_moai = { mg_screen_init, mg_screen_update };
