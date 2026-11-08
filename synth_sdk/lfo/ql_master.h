/* Master LFO (after Hundekraft kennel's screens/mlfo.c): an LFO on the master FX knobs that belongs to no sound.
 * Shift + mixer (signal flow, mode 0x0D, screen 0x30): T4 shows its page, T4 again switches it on / off, Shift + T4
 * opens the type list (as the stock LFO list: blue moves, T4 shows the chosen type's page). Types: QUAD, POND.
 * Runs as LFO page 2 of the quad code (0 synth, 1 drum): the quad / pond words, DSP, controls and screens, with the
 * targets limited to the master FX knobs (page 1 / 2) and LFO targets, every LFO shared (FREE).
 * Audio: fr_route (screens/fxroute.c) runs every master FX render; ml_params() hands the FX a copy of its knob words
 * with the modulation, through the firmware's FX knob range function (FX_KNOB_MOD + the master type's range row, as
 * quad's FX targets: values stay inside the range asserts). The live words (saved, shown) keep the set values.
 * Settings (op1rsettings, KeyHandler thread, at the next key event after an edit): mlon, mltype, mlq0..7 / mlp0..7
 * (the quad / pond words, one per key). A set without its marker reads as the defaults (every amount 0).
 * Included at the end of lfo/quadlfo.c. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */

#define ML_NTYPES 2                                          /* 0 quad, 1 pond */
static const char *const ml_type_name[ML_NTYPES] = { "quad", "pond" };   /* stock font: lowercase (renders as caps) */
static const char *const ml_keys[ML_NTYPES][8] = { { "mlq0", "mlq1", "mlq2", "mlq3", "mlq4", "mlq5", "mlq6", "mlq7" },
                                                   { "mlp0", "mlp1", "mlp2", "mlp3", "mlp4", "mlp5", "mlp6", "mlp7" } };
static uint8_t ml_type, ml_saved_on, ml_saved_type, ml_dirty;
static int16_t ml_w[ML_NTYPES][8], ml_saved[ML_NTYPES][8];
volatile uint8_t ml_view;                                    /* 0 not shown, 1 the LFO page, 2 the type list */
static uint8_t ml_cur;                                       /* type list cursor */
static int16_t *ml_words(void) { return ml_w[ml_type < ML_NTYPES ? ml_type : 0]; }

static void ml_defaults(int t, int16_t *w)
{
    for (int k = 0; k < 8; k++) w[k] = 0;
    if (t == 1) {                                            /* (field by field: no memcpy in the firmware) */
        pond_set f; f.rate = 31; f.sync = 0; f.retrig = 0; f.chaos = 24;
        for (int k = 0; k < 4; k++) { f.tgt[k] = (uint8_t)(8 + k); f.amt[k] = 0; }
        pond_put(&f, w); return;
    }
    for (int i = 0; i < 4; i++) {                            /* LFO i -> master FX knob i, amount 0 */
        ql_lfo o = { 0 };
        o.rate = ql_def_rate[i]; o.type = QT_FREE; o.tgt = (uint8_t)(8 + i);
        ql_enc(&o, w, i);
    }
}
static int ml_valid(int t, const int16_t *w) { return t ? ql_is_pond(w) : ((uint16_t)w[7] >> 8) == QL_MARK; }

void ml_load(void)                                           /* boot, KeyHandler / GUI thread (sqlite) */
{
    for (int t = 0; t < ML_NTYPES; t++) {
        int16_t w[8];
        for (int k = 0; k < 8; k++) w[k] = (int16_t)op1r_setting_get(ml_keys[t][k], 0);
        if (!ml_valid(t, w)) ml_defaults(t, w);
        for (int k = 0; k < 8; k++) ml_w[t][k] = ml_saved[t][k] = w[k];
    }
    int ty = op1r_setting_get("mltype", 0);
    ml_type = ml_saved_type = (uint8_t)(ty >= 0 && ty < ML_NTYPES ? ty : 0);
    ml_on = ml_saved_on = op1r_setting_get("mlon", 0) == 1;
}
void ml_flush(void)                                          /* key events: what changed since the last save */
{
    if (!ml_dirty) return;
    ml_dirty = 0;
    if (ml_on != ml_saved_on) op1r_setting_put("mlon", ml_saved_on = ml_on);
    if (ml_type != ml_saved_type) op1r_setting_put("mltype", ml_saved_type = ml_type);
    for (int t = 0; t < ML_NTYPES; t++) {                    /* a changed set: all 8 words (a key never written reads 0) */
        int ch = 0;
        for (int k = 0; k < 8; k++) ch |= ml_w[t][k] != ml_saved[t][k];
        if (ch) for (int k = 0; k < 8; k++) op1r_setting_put(ml_keys[t][k], ml_saved[t][k] = ml_w[t][k]);
    }
}

/* ---- audio thread: the master FX words to render with (params itself while nothing is modulated) ---- */
static int16_t ml_copy[8];
const int16_t *ml_params(const int16_t *params)
{
    if (!ml_on) return params;
    ql_lfo Lb[4];
    const ql_lfo *L = ql_params(ml_words(), 2, -1, Lb);
    ql_advance_pos(2, L);
    int type = MASTER_TYPE(), any = 0;
    for (int k = 0; k < 8; k++) ml_copy[k] = params[k];
    for (int i = 0; i < 4; i++) {
        if (!L[i].a128 || ql_is_lfo(&L[i]) || ql_group(&L[i]) != QL_FX) continue;
        int16_t mod = ql_out(sh_st[2][i].val, L[i].a128);
        int k = ql_dest(&L[i]);
        if (ql_p2(&L[i])) {                                  /* page 2: plugin master FX only */
            if (!FX_PLUGIN(ml_copy[4])) continue;
            ql_p2_mod(&ml_copy[5 + k], mod, 0);
            ql_p2_shown[3][k] = ml_copy[5 + k]; ql_p2_at[3][k] = READ_CYCLES(); ql_p2_ok[3][k] = 1;
        } else {
            if ((uint32_t)type > 7) continue;
            const int16_t *rg = FX_RANGE(type, k);
            if (rg[1] <= rg[0]) continue;                    /* range row not filled */
            FX_KNOB_MOD(&mod, rg, &ml_copy[k]);
        }
        any = 1;
    }
    return any ? ml_copy : params;
}

/* ---- keys (KeyHandler thread, dispatcher.c on the signal flow screen) ---- */
int ml_t4(int shift)                                         /* T4 pressed -> what to show: 1 page, 2 type list */
{
    if (shift) { ml_cur = ml_type; ml_view = 2; }
    else if (ml_view == 2) { if (ml_cur != ml_type) { ml_type = ml_cur; ml_dirty = 1; } ml_view = 1; }
    else if (ml_view == 1) { ml_on = !ml_on; ml_dirty = 1; }
    else ml_view = 1;
    return ml_view;
}
int ml_type_shown(void) { return ml_type; }
void ml_leave(void) { ml_view = 0; }
void ml_press(int knob) { if (ml_view == 1) ql_press(2, knob); }
static int32_t ml_acc;
void ml_turn(int knob, int delta, int shift)
{
    if (ml_view == 2) {                                      /* type list: blue moves, one entry per detent */
        if (knob) return;
        if ((ml_acc > 0 && delta < 0) || (ml_acc < 0 && delta > 0)) ml_acc = 0;
        ml_acc += delta;
        int st = ml_acc / 1024; ml_acc -= st * 1024;
        ml_cur = (uint8_t)clampq(ml_cur + st, 0, ML_NTYPES - 1);
        return;
    }
    if (ml_view != 1) return;
    ql_turn(ml_words(), 2, knob, delta, shift);
    ml_dirty = 1;
}

/* ---- the type list screen (GUI thread): as the stock LFO list (PresetBrowser) - "lfo" top left with a white bar
 * under it, the types in the stock font, the chosen one as a filled bar with dark text, a scroll bar on the right.
 * Positions from the stock screen (emulator capture, 320 x 160). */
#define ML_LIST_X 65                                         /* entry text left */
#define ML_ROW0 9                                            /* first row's text top */
#define ML_ROW_H 23                                           /* stock pitch (28.8 px of the 400 x 200 capture) */
#define ML_BAR_X0 60
#define ML_BAR_X1 170
#define ML_SCROLL_X 173
static const uint8_t ml_blue[4] = { 0x6B, 0x8E, 0xFF, 0xFF }, ml_dark[4] = { 0x10, 0x14, 0x30, 0xFF },
                     ml_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, ml_track[4] = { 0x21, 0x34, 0x5A, 0xFF };
static uint32_t ml_tx[2 + 2 * ML_NTYPES][OP1_TEXT_SIZE / 4];   /* title, (unused), per type: normal + selected */
static uint8_t ml_made;
static void ml_list_init(void *scr) { (void)scr; }
static void ml_list_update(void *scr, int dt)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    if (!ml_made) {
        op1_text_ctor(ml_tx[0], OP1_Q10(17), 7, 14, ml_blue);
        op1_text_set(ml_tx[0], "lfo");
        for (int t = 0; t < ML_NTYPES; t++) {
            op1_text_ctor(ml_tx[2 + 2 * t], OP1_Q10(ML_LIST_X), ML_ROW0 + ML_ROW_H * t, 13, ml_blue);
            op1_text_ctor(ml_tx[3 + 2 * t], OP1_Q10(ML_LIST_X), ML_ROW0 + ML_ROW_H * t, 13, ml_dark);
            op1_text_set(ml_tx[2 + 2 * t], ml_type_name[t]); op1_text_set(ml_tx[3 + 2 * t], ml_type_name[t]);
        }
        ml_made = 1;
    }
    op1_text_draw(ml_tx[0], r, ident);
    qline(r, OP1_Q10(31), OP1_Q10(30), OP1_Q10(31), OP1_Q10(66), OP1_Q10(1), ml_white);
    int cur = ml_cur < ML_NTYPES ? ml_cur : 0;
    for (int t = 0; t < ML_NTYPES; t++) {
        if (t == cur) {
            /* the bar as the stock one: from 2.4 px above the text top, 17.6 px tall (a line this wide draws ~0.75 of it) */
            int32_t yc = OP1_Q10(ML_ROW0 + ML_ROW_H * t) + OP1_Q10(6.4);
            qline(r, OP1_Q10(ML_BAR_X0), yc, OP1_Q10(ML_BAR_X1), yc, OP1_Q10(23.5), ml_blue);
        }
        op1_text_draw(ml_tx[(t == cur ? 3 : 2) + 2 * t], r, ident);
    }
    qline(r, OP1_Q10(ML_SCROLL_X), OP1_Q10(6), OP1_Q10(ML_SCROLL_X), OP1_Q10(136), OP1_Q10(1), ml_track);   /* all in view */
    qline(r, OP1_Q10(ML_SCROLL_X), OP1_Q10(6), OP1_Q10(ML_SCROLL_X), OP1_Q10(136), OP1_Q10(2), ml_blue);
}
const op1_screen_t SCREEN_mlfolist = { ml_list_init, ml_list_update };

/* ---- "LFO OFF" (GUI thread): the master LFO page while it is off, as the stock instrument LFO page: everything
 * dimmed to ~27 %, a black box in the middle, "LFO" in blue and "OFF" in white (stock font, positions from the
 * emulator capture of the stock page). Drawn last over the quad / pond page. */
static uint32_t ml_offtx[2][OP1_TEXT_SIZE / 4];
static uint8_t ml_off_made;
static void ml_off_overlay(void *r, int pos)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    static const uint8_t veil[4] = { 0, 0, 0, 0xBA }, black[4] = { 0, 0, 0, 0xFF };
    if (pos != 2 || ml_on) return;
    if (!ml_off_made) {
        op1_text_ctor(ml_offtx[0], OP1_Q10(120), 64, 16, ml_blue); op1_text_set(ml_offtx[0], "lfo");
        op1_text_ctor(ml_offtx[1], OP1_Q10(162), 62, 20, ml_white); op1_text_set(ml_offtx[1], "off");
        ml_off_made = 1;
    }
    qline(r, 0, OP1_Q10(80), OP1_Q10(320), OP1_Q10(80), OP1_Q10(214), veil);   /* (~0.75 of the width draws) */
    qline(r, OP1_Q10(108), OP1_Q10(72), OP1_Q10(210), OP1_Q10(72), OP1_Q10(51), black);   /* box y 53 .. 91 */
    op1_text_draw(ml_offtx[0], r, ident);
    op1_text_draw(ml_offtx[1], r, ident);
}
