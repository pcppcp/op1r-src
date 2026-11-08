/* "chord" sequencer plugin (docs/50): one key plays a chord, on any screen while chord is the mode's sequencer.
 * Logic in ch_core.c (host-tested); here: the player (key_down / key_up / tick), strum, the MIDI recorder report,
 * persistence (DB table chordseq) and the page.
 *   blue turn    chord: fixed types (maj .. oct) or, in scale mode, the stacked kind (triad, 7th, sus2, sus4, add9)
 *   blue press   fixed / scale
 *   green turn   voicing: close, inv 1-3, drop 2, spread     green press  bass note an octave below on / off
 *   white turn   strum: off .. 1/8 beat between the notes    white press  up / down / random
 *   red turn     key (scale root C .. B)                     red press    scale: major / minor / dorian / mixolydian
 * Drum mode: the chord's notes go out as keys, so each triggers its pad (ch_drum_fold).
 * MIDI recorder: the key as played goes to input I (op1_key_played), the chord notes (VoiceCtl from here) to input S.
 * Strum runs on the player tick (96 per beat, tempo-relative); the first note sounds at once. */
#include "../op1_synth.h"
#include "../seq/seq_text.h"
#include "ch_core.h"

#define CH_VEL 100
static const uint8_t ch_strum_ticks[8] = { 0, 1, 2, 3, 4, 6, 8, 12 };   /* 1/96 beat */
enum { CH_UP, CH_DOWN, CH_RAND };

typedef struct {
    ch_cfg c;
    uint8_t strum, dir;
} ch_set;                                              /* saved */

typedef struct {
    ch_set s;
    ch_held h;
    uint8_t raw[CH_KEYS];                              /* midi + 1 of the key as played (recorder note-off) */
    uint8_t pend[CH_KEYS][CH_MAXN], npend[CH_KEYS], ipend[CH_KEYS];
    uint32_t due[CH_KEYS], tick, rng;
    int16_t last;                                      /* midi of the last chord played (screen), -1 none */
    uint8_t lastn, lastnotes[CH_MAXN];
    int32_t acc[4];
    uint8_t init;
} ch_bank;
static ch_bank chb[2];                                 /* 0 synth, 1 drum */

#ifdef OP1_HOST
void host_ch_note(int pos, int note, int vel, int on);
#define CH_NOTE_ON(pos, note, key, vel) ((void)(key), host_ch_note((pos), (note), (vel), 1))
#define CH_NOTE_OFF(pos, note)          host_ch_note((pos), (note), 0, 0)
#define CH_KEY_PLAYED(m, v, on)         ((void)0)
#define CH_CHANGED()                    ((void)0)
static int ch_oct(int m) { (void)m; return 0; }
#else
int  eu_fw_note_on(int pos, int note, int key, int vel);   /* seq/seq_core.c: VoiceCtl note on / off */
void eu_fw_note_off(int midi);
void op1_key_played(int midi, int vel, int on);             /* seqdev/ms_fw.c: recorder input I */
#define CH_NOTE_ON(pos, note, key, vel) eu_fw_note_on((pos), (note), (key), (vel))
#define CH_NOTE_OFF(pos, note)          eu_fw_note_off(note)
#define CH_KEY_PLAYED(m, v, on)         op1_key_played((m), (v), (on))
static void ch_changed(void);
#define CH_CHANGED()                    ch_changed()
static int ch_oct(int m)                                    /* keyboard octave of the current slot */
{
    return m ? ((const int *)0x01DFE500)[*(volatile int *)0xFF8016D8 & 7] : ((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7];
}
#endif

static void ch_init_bank(ch_bank *b)
{
    if (b->init) return;
    b->init = 1; b->last = -1; b->rng = 0x9E3779B9u;
    b->s.c.mode = CH_FIXED; b->s.c.type = CT_MAJ; b->s.c.scale = SC_MAJOR;
}

/* ch_core output: synth notes are MIDI notes; drum "notes" are pad + 0x35 (the key's midi at drum octave 0) */
static void ch_out(int note, int vel, int on, void *ctx)
{
    int m = (int)(intptr_t)ctx;
    if (!on) { CH_NOTE_OFF(m, note); return; }
    if (m) { int pad = note - 0x35; CH_NOTE_ON(1, note + ch_oct(1) * 12, pad, vel); }   /* pad at its own pitch */
    else CH_NOTE_ON(0, note, 0, vel);
}

static void ch_cancel(ch_bank *b, int key) { b->npend[key] = b->ipend[key] = 0; }

int ch_key_down_from(int m, int key, uint32_t caller)        /* caller: OP1_SEQ_CALLER() of the player's key_down */
{
    ch_bank *b = &chb[m & 1];
    ch_init_bank(b);
    if (key < 0 || key >= CH_KEYS) return 0;
    int midi = op1_seq_key_note(m, key, ch_oct(m), caller);
    if (midi < 0 || midi > 127) return 1;
    if (b->raw[key]) { CH_KEY_PLAYED(b->raw[key] - 1, 0, 0); b->raw[key] = 0; }
    CH_KEY_PLAYED(midi, CH_VEL, 1);
    b->raw[key] = (uint8_t)(midi + 1);
    uint8_t n[CH_MAXN], o[CH_MAXN];
    int k = ch_notes(&b->s.c, midi, n);
    b->last = (int16_t)midi; b->lastn = (uint8_t)k;
    for (int i = 0; i < k; i++) b->lastnotes[i] = n[i];
    if (m) {                                                    /* drum: pads, as notes at drum octave 0 */
        uint8_t p[CH_MAXN];
        k = ch_drum_fold(key, midi, n, k, p);
        for (int i = 0; i < k; i++) n[i] = (uint8_t)(p[i] + 0x35);
    }
    for (int i = 0; i < k; i++) o[i] = b->s.dir == CH_DOWN ? n[k - 1 - i] : n[i];
    if (b->s.dir == CH_RAND)                                    /* shuffle */
        for (int i = k - 1; i > 0; i--) {
            b->rng = b->rng * 1664525u + 1013904223u;
            int j = (int)((b->rng >> 16) % (uint32_t)(i + 1));
            uint8_t t = o[i]; o[i] = o[j]; o[j] = t;
        }
    void *ctx = (void *)(intptr_t)(m & 1);
    ch_cancel(b, key);
    int st = ch_strum_ticks[b->s.strum & 7];
    if (!st) { ch_down(&b->h, key, o, k, CH_VEL, ch_out, ctx); return 1; }
    ch_up(&b->h, key, ch_out, ctx);                             /* (a second down of a held key) */
    if (k) ch_add(&b->h, key, o[0], CH_VEL, ch_out, ctx);       /* first note now, the rest on the ticks */
    for (int i = 1; i < k; i++) b->pend[key][i - 1] = o[i];
    b->npend[key] = (uint8_t)(k ? k - 1 : 0); b->ipend[key] = 0;
    b->due[key] = b->tick + (uint32_t)st;
    return 1;
}
int ch_key_down(int m, int key) { return ch_key_down_from(m, key, 0); }   /* host tests: the keyboard */

void ch_key_up(int m, int key)
{
    ch_bank *b = &chb[m & 1];
    if (key < 0 || key >= CH_KEYS) return;
    if (b->raw[key]) { CH_KEY_PLAYED(b->raw[key] - 1, 0, 0); b->raw[key] = 0; }
    ch_cancel(b, key);
    ch_up(&b->h, key, ch_out, (void *)(intptr_t)(m & 1));
}

void ch_tick(int m)                                             /* 1/96 beat */
{
    ch_bank *b = &chb[m & 1];
    b->tick++;
    int st = ch_strum_ticks[b->s.strum & 7];
    for (int key = 0; key < CH_KEYS; key++)
        while (b->ipend[key] < b->npend[key] && (int32_t)(b->tick - b->due[key]) >= 0) {
            ch_add(&b->h, key, b->pend[key][b->ipend[key]++], CH_VEL, ch_out, (void *)(intptr_t)(m & 1));
            b->due[key] += (uint32_t)(st ? st : 1);
        }
}

void ch_all_notes_off(int m)
{
    ch_bank *b = &chb[m & 1];
    for (int key = 0; key < CH_KEYS; key++) { ch_cancel(b, key); if (b->raw[key]) { CH_KEY_PLAYED(b->raw[key] - 1, 0, 0); b->raw[key] = 0; } }
    ch_all_off(&b->h, ch_out, (void *)(intptr_t)(m & 1));
}

/* ---- page: turns and presses ---- */
static int ch_detents(ch_bank *b, int k, int delta)
{
    if ((b->acc[k] > 0 && delta < 0) || (b->acc[k] < 0 && delta > 0)) b->acc[k] = 0;
    b->acc[k] += delta;
    int st = b->acc[k] / 1024;
    b->acc[k] -= st * 1024;
    return st;
}
static int ch_clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void ch_turn(int m, int knob, int delta, int shift)
{
    (void)shift;
    ch_bank *b = &chb[m & 1];
    ch_init_bank(b);
    if (knob < 0 || knob > 3) return;
    int st = ch_detents(b, knob, delta);
    if (!st) return;
    ch_set *s = &b->s;
    if (knob == 0) s->c.type = (uint8_t)ch_clampi(s->c.type + st, 0, (s->c.mode == CH_FIXED ? CT_N : SK_N) - 1);
    else if (knob == 1) s->c.voicing = (uint8_t)ch_clampi(s->c.voicing + st, 0, VO_N - 1);
    else if (knob == 2) s->strum = (uint8_t)ch_clampi(s->strum + st, 0, 7);
    else s->c.root = (uint8_t)((s->c.root + st % 12 + 12) % 12);
    CH_CHANGED();
}

void ch_press(int m, int knob)
{
    ch_bank *b = &chb[m & 1];
    ch_init_bank(b);
    ch_set *s = &b->s;
    if (knob == 0) { s->c.mode ^= 1; s->c.type = 0; }
    else if (knob == 1) s->c.bass ^= 1;
    else if (knob == 2) s->dir = (uint8_t)((s->dir + 1) % 3);
    else if (knob == 3) s->c.scale = (uint8_t)((s->c.scale + 1) % SC_N);
    CH_CHANGED();
}

/* ---- persistence text: "mode,type,root,scale,voicing,bass,strum,dir" ---- */
int ch_serialize(int pos, char *out, int n)
{
    ch_bank *b = &chb[pos & 1];
    ch_init_bank(b);
    const ch_set *s = &b->s;
    int v[8] = { s->c.mode, s->c.type, s->c.root, s->c.scale, s->c.voicing, s->c.bass, s->strum, s->dir };
    return sq_join(out, n, v, 8);
}
int ch_parse(int pos, const char *str, int len)
{
    int v[8];
    if (sq_split(str, len, v, 8) != 8) return 0;
    ch_bank *b = &chb[pos & 1];
    ch_init_bank(b);
    ch_set *s = &b->s;
    s->c.mode = (uint8_t)(v[0] & 1);
    s->c.type = (uint8_t)ch_clampi(v[1], 0, (s->c.mode == CH_FIXED ? CT_N : SK_N) - 1);
    s->c.root = (uint8_t)ch_clampi(v[2], 0, 11); s->c.scale = (uint8_t)ch_clampi(v[3], 0, SC_N - 1);
    s->c.voicing = (uint8_t)ch_clampi(v[4], 0, VO_N - 1); s->c.bass = (uint8_t)(v[5] & 1);
    s->strum = (uint8_t)ch_clampi(v[6], 0, 7); s->dir = (uint8_t)ch_clampi(v[7], 0, 2);
    return 1;
}

/* ======================= screen (GUI thread) =======================
 * Top: the chord name (stock font) and a two-octave keyboard with the last chord's notes lit (blue; the played key
 * white). Bottom row as sampler+: coloured bars, label, value, press state above. */
#define KB_X0 20
#define KB_Y0 34
#define KB_WK 20                                        /* white key width (14 white keys: 280 px) */
#define KB_H  52
static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     c_grey[4] = { 0x6F, 0x71, 0x73, 0xFF }, c_dim[4] = { 0x3A, 0x3B, 0x3D, 0xFF };
static const uint8_t *const ch_col[4] = { c_blue, c_green, c_white, c_red };
enum { TX_NAME, TX_LAB, TX_VAL = TX_LAB + 4, TX_PRS = TX_VAL + 4, TX_N = TX_PRS + 4 };
static uint32_t ch_tx[TX_N][OP1_TEXT_SIZE / 4];
static char ch_shown[TX_N][16];
static uint8_t ch_tx_made;

static void ch_txt(void *r, int i, const char *s)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    char lc[16]; int k = 0;
    for (; s[k] && k < 15; k++) lc[k] = (char)(s[k] >= 'A' && s[k] <= 'Z' ? s[k] + 32 : s[k]);   /* stock font: lower case */
    lc[k] = 0;
    if (!lc[0]) return;
    int same = 1;
    for (int j = 0; j < 16; j++) { if (ch_shown[i][j] != lc[j]) { same = 0; break; } if (!lc[j]) break; }
    if (!same) { op1_text_set(ch_tx[i], lc); for (int j = 0; j < 16; j++) { ch_shown[i][j] = lc[j]; if (!lc[j]) break; } }
    op1_text_draw(ch_tx[i], r, ident);
}
static void ch_ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}

/* key 0 = F (the OP-1's first key, midi 53 at octave 0): x of a key's centre and whether it is black */
static int ch_black(int k) { int pc = (k + 5) % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }
static int32_t ch_key_x(int k)
{
    int w = 0;
    for (int i = 0; i < k; i++) if (!ch_black(i)) w++;
    return ch_black(k) ? OP1_Q10(KB_X0 + w * KB_WK) : OP1_Q10(KB_X0 + w * KB_WK + KB_WK / 2);
}

static void ch_screen_draw(void *r, int m)
{
    ch_bank *b = &chb[m & 1];
    ch_init_bank(b);
    const ch_set *s = &b->s;
    if (!ch_tx_made) {
        op1_text_ctor(ch_tx[TX_NAME], OP1_Q10(KB_X0), 8, 12, c_white);
        for (int k = 0; k < 4; k++) {
            op1_text_ctor(ch_tx[TX_LAB + k], OP1_Q10(8 + 78 * k), 114, 12, ch_col[k]);
            op1_text_ctor(ch_tx[TX_VAL + k], OP1_Q10(8 + 78 * k), 127, 12, c_white);
            op1_text_ctor(ch_tx[TX_PRS + k], OP1_Q10(8 + 78 * k), 94, 12, ch_col[k]);
        }
        ch_tx_made = 1;
    }
    /* chord name: the last chord played, else what C (key 7) would give */
    int base = 0x35 + ch_oct(m) * 12, midi = b->last >= 0 ? b->last : base + 7;
    char name[16];
    ch_name(&s->c, midi, name);
    ch_txt(r, TX_NAME, name);
    /* keyboard: white keys as outlines, black keys as bars; notes of the shown chord lit */
    uint8_t lit[CH_KEYS] = { 0 };
    uint8_t n[CH_MAXN];
    int k = b->last >= 0 ? b->lastn : ch_notes(&s->c, midi, n);
    const uint8_t *nn = b->last >= 0 ? b->lastnotes : n;
    for (int i = 0; i < k; i++) { int key = nn[i] - base; while (key < 0) key += 12; while (key >= CH_KEYS) key -= 12; lit[key] = 1; }
    int played = midi - base;
    for (int i = 0, w = 0; i < CH_KEYS; i++) {
        if (ch_black(i)) continue;
        int32_t x = OP1_Q10(KB_X0 + w * KB_WK), y0 = OP1_Q10(KB_Y0), y1 = OP1_Q10(KB_Y0 + KB_H);
        ch_ln(r, x, y0, x, y1, 0x300, c_grey);                          /* left edge */
        if (lit[i]) ch_ln(r, x + OP1_Q10(KB_WK / 2), y1 - OP1_Q10(18), x + OP1_Q10(KB_WK / 2), y1 - OP1_Q10(4), OP1_Q10(10), i == played ? c_white : c_blue);
        w++;
    }
    ch_ln(r, OP1_Q10(KB_X0 + 14 * KB_WK), OP1_Q10(KB_Y0), OP1_Q10(KB_X0 + 14 * KB_WK), OP1_Q10(KB_Y0 + KB_H), 0x300, c_grey);
    ch_ln(r, OP1_Q10(KB_X0), OP1_Q10(KB_Y0 + KB_H), OP1_Q10(KB_X0 + 14 * KB_WK), OP1_Q10(KB_Y0 + KB_H), 0x300, c_grey);
    for (int i = 0; i < CH_KEYS; i++) {
        if (!ch_black(i)) continue;
        int32_t x = ch_key_x(i);
        ch_ln(r, x, OP1_Q10(KB_Y0), x, OP1_Q10(KB_Y0 + 30), OP1_Q10(10), lit[i] ? (i == played ? c_white : c_blue) : c_dim);
    }
    /* bottom row */
    static const char *const types[CT_N] = { "maj", "min", "sus2", "sus4", "7", "maj7", "min7", "dim", "aug", "6", "add9", "5", "oct" };
    static const char *const kinds[SK_N] = { "triad", "7th", "sus2", "sus4", "add9" };
    static const char *const voic[VO_N] = { "close", "inv 1", "inv 2", "inv 3", "drop 2", "spread" };
    static const char *const strum[8] = { "off", "1/96", "1/48", "1/32", "1/24", "1/16", "1/12", "1/8" };
    static const char *const dirs[3] = { "up", "down", "rand" };
    static const char *const roots[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char *const scales[SC_N] = { "major", "minor", "dorian", "mixo" };
    static const char *const labs[4] = { "chord", "voicing", "strum", "key" };
    for (int i = 0; i < 4; i++) {
        ch_ln(r, OP1_Q10(8 + 78 * i), OP1_Q10(110), OP1_Q10(78 + 78 * i), OP1_Q10(110), 0x800, ch_col[i]);
        ch_txt(r, TX_LAB + i, labs[i]);
    }
    ch_txt(r, TX_VAL + 0, s->c.mode == CH_FIXED ? types[s->c.type % CT_N] : kinds[s->c.type % SK_N]);
    ch_txt(r, TX_VAL + 1, voic[s->c.voicing % VO_N]);
    ch_txt(r, TX_VAL + 2, strum[s->strum & 7]);
    ch_txt(r, TX_VAL + 3, s->c.mode == CH_SCALE ? roots[s->c.root % 12] : "-");
    ch_txt(r, TX_PRS + 0, s->c.mode == CH_FIXED ? "fixed" : "scale");
    if (s->c.bass) ch_txt(r, TX_PRS + 1, "bass");
    if (s->strum) ch_txt(r, TX_PRS + 2, dirs[s->dir % 3]);
    if (s->c.mode == CH_SCALE) ch_txt(r, TX_PRS + 3, scales[s->c.scale % SC_N]);
}

#ifdef OP1_HOST
void ch_host_draw(void *r, int m) { ch_screen_draw(r, m); }
#else
/* ======================= firmware player (type 3 + ch_active[mode], as peal) ======================= */
#define CH_MODE() (*(volatile int *)0x01DE4C40 == 1)
static int ch_nop(void *self) { (void)self; return 0; }
static int ch_pkey_down(void *self, int key) { (void)self; return ch_key_down_from(CH_MODE(), key, OP1_SEQ_CALLER()); }   /* consumed */
static void ch_pkey_up(void *self, int key) { (void)self; ch_key_up(0, key); ch_key_up(1, key); }   /* (mode switch while held) */
static void ch_reset(void *self) { (void)self; ch_all_notes_off(CH_MODE()); }
static void ch_ptick(void *self, int counter) { (void)self; (void)counter; ch_tick(CH_MODE()); }
static void ch_pstop(void *self) { (void)self; ch_all_notes_off(0); ch_all_notes_off(1); }
static const uint32_t ch_vt[16] = {
    0, 0,  0, (uint32_t)ch_nop,  0, (uint32_t)ch_pkey_down,  0, (uint32_t)ch_pkey_up,
    0, (uint32_t)ch_reset,  0, (uint32_t)ch_ptick,  0, (uint32_t)ch_nop,  0, (uint32_t)ch_pstop };
static struct { const uint32_t *vt; uint32_t pad[7]; } ch_player = { ch_vt, { 0 } };
volatile uint8_t ch_active[2];

static char ch_sql[160];
static sq_store_t ch_store = { .name = "chordseq", .ser = ch_serialize, .parse = ch_parse,
                               .sql = ch_sql, .cap = sizeof ch_sql, .maxlen = 80 };
static void ch_changed(void) { sq_store_dirty(&ch_store); }
static void *ch_player_get(void) { sq_store_load(&ch_store); return &ch_player; }

static void ch_scr_init(void *scr) { (void)scr; }
static void ch_scr_update(void *scr, int dt) { op1_screen_draw(scr, dt); ch_screen_draw(op1_raster(), CH_MODE()); }
const op1_screen_t SCREEN_chord = { ch_scr_init, ch_scr_update };

/* page keys: presses (knob 0..3) toggle; lift / drop / scissors do nothing */
static int ch_pkey(int m, int code, int pressed, int knob, int shift)
{
    (void)shift;
    if (code >= 0x0A && code <= 0x0C) return 1;
    if (knob >= 0) { if (pressed) ch_press(m, knob); return 1; }
    return 0;
}
static void ch_pturn(int m, int knob, int delta, int shift) { ch_turn(m, knob, delta, shift); }
static void ch_midi_start(void) { }
const op1_seq_t SEQ_chord = { ch_active, ch_player_get, ch_pkey, ch_pturn, ch_midi_start, 0 };
#endif
