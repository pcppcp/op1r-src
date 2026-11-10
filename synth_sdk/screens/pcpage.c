/* Pulse sync page (docs/78): metronome (tempo) + T2. The jack clock's state and settings, after the ideas of
 * Hundekraft's line clock (Pulse Sync by ESC_CODE): the state and the one missing step, the tempo from the pulses with
 * a pulse lamp and beat dots, the pulse level against the trigger, and the four settings under their encoders.
 *   blue  rate   midi / ppq 1, 2, 4, 24, 48 (op1rsettings clksrc)
 *   green start  auto (start / stop with the pulses) / manual
 *   white edge   rise / fall
 *   red   level  auto (adaptive) / 1..6 (fixed trigger); with Shift held the red column is the channel:
 *         both / left / right (Shift + red, clkch)
 * Stock font (op1_text_*), lowercase. Drawing only; the detector and its settings are midi/pclk.c. */
#include "../op1_synth.h"
#include "../screens/op1r_settings.h"

extern volatile uint8_t pc_src, pc_manual, pc_fall, pc_level, pc_chan;
extern volatile int32_t pc_show_jump, pc_show_trig;
extern volatile uint32_t pc_pulses;
extern uint8_t pc_running;
extern uint32_t pc_period, pc_count;
extern const int16_t pc_level_val[7];
int pc_input_ok(void);
void pc_set_src(int v);
#ifdef OP1_HOST
extern int32_t host_sync_mode;
#define SYNC_MODE host_sync_mode
#else
#define SYNC_MODE (*(volatile int32_t *)0x018C1870)
#endif

static const uint8_t pc_ppqs[6] = { 0, 1, 2, 4, 24, 48 };
static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     c_grey[4] = { 0x6F, 0x71, 0x73, 0xFF }, c_dim[4] = { 0x3A, 0x3B, 0x3D, 0xFF };
static const uint8_t *const col4[4] = { c_blue, c_green, c_white, c_red };
#define Q(v) OP1_Q10(v)

enum { PS_MIDI, PS_NOFOLLOW, PS_NOINPUT, PS_WAIT, PS_FOLLOW };
int pcp_state(void)
{
    int m = SYNC_MODE;
    if (!pc_src) return PS_MIDI;
    if (m != 2 && m != 4) return PS_NOFOLLOW;
    if (!pc_input_ok()) return PS_NOINPUT;
    return pc_running ? PS_FOLLOW : PS_WAIT;
}

/* ---- settings (KeyHandler: knob turns on the page, one step per hardware click = 1024; the emulator sends 2048) ---- */
void pcp_turn(int knob, int delta)                          /* knob 0..3, 7 = Shift + red */
{
    static int32_t acc[8];
    if (knob < 0 || knob > 7 || (knob > 3 && knob != 7)) return;
    if ((acc[knob] > 0 && delta < 0) || (acc[knob] < 0 && delta > 0)) acc[knob] = 0;
    acc[knob] += delta;
    int st = acc[knob] / 1024;
    if (!st) return;
    acc[knob] = 0;
    int v;
    switch (knob) {
    case 0: v = pc_src + st; v = v < 0 ? 0 : v > 5 ? 5 : v; pc_set_src(v); break;
    case 1: v = st > 0; if (v != pc_manual) { pc_manual = (uint8_t)v; op1r_setting_put("clkman", v); } break;
    case 2: v = st > 0; if (v != pc_fall) { pc_fall = (uint8_t)v; op1r_setting_put("clkfall", v); } break;
    case 7: v = pc_chan + st; v = v < 0 ? 0 : v > 2 ? 2 : v; if (v != pc_chan) { pc_chan = (uint8_t)v; op1r_setting_put("clkch", v); } break;
    default: v = pc_level + st; v = v < 0 ? 0 : v > 6 ? 6 : v; if (v != pc_level) { pc_level = (uint8_t)v; op1r_setting_put("clklvl", v); } break;
    }
}
void pcp_load(void)                          /* boot: the settings beside clksrc (midi/pclk.c pc_load) */
{
    int v = op1r_setting_get("clkman", 0); pc_manual = v == 1;
    v = op1r_setting_get("clkfall", 0); pc_fall = v == 1;
    v = op1r_setting_get("clklvl", 0); pc_level = (uint8_t)(v >= 0 && v <= 6 ? v : 0);
    v = op1r_setting_get("clkch", 0); pc_chan = (uint8_t)(v >= 0 && v <= 2 ? v : 0);
}

/* ---- drawing ---- */
static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{ op1_pt a = { x0, y0 }, b = { x1, y1 }; op1_raster_line(r, &a, &b, w, c); }
static void box(void *r, int cx, int cy, int h, int fill, const uint8_t *c)   /* a lamp: filled or an outline */
{
    if (fill) { ln(r, Q(cx - h), Q(cy), Q(cx + h), Q(cy), Q(2 * h), c); return; }
    ln(r, Q(cx - h), Q(cy - h), Q(cx + h), Q(cy - h), 0x500, c); ln(r, Q(cx - h), Q(cy + h), Q(cx + h), Q(cy + h), 0x500, c);
    ln(r, Q(cx - h), Q(cy - h), Q(cx - h), Q(cy + h), 0x500, c); ln(r, Q(cx + h), Q(cy - h), Q(cx + h), Q(cy + h), 0x500, c);
}
static int bar_x(int32_t v)                  /* level -> x 8..312: an octave every 33 px from 128 */
{
    if (v < 128) return 8;
    int e = 7;
    while (e < 15 && (v >> (e + 1))) e++;
    int p = (e - 7) * 38 + (int)((((v - (1 << e)) << 5) >> e) * 38 >> 5);
    return 8 + (p > 304 ? 304 : p);
}
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *num(char *p, uint32_t v) { char d[10]; int n = 0; do d[n++] = (char)('0' + v % 10); while (v /= 10); while (n) *p++ = d[--n]; *p = 0; return p; }

enum { T_TITLE, T_STATE, T_BIG, T_STEP, T_BPM, T_HINT, T_PULSE, T_BEAT, T_LAB, T_VAL = T_LAB + 4, T_N = T_VAL + 4 };
static uint32_t tx[T_N][OP1_TEXT_SIZE / 4];
static char shown[T_N][24];
static uint8_t made;
static void txt(void *r, int i, const char *s)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    int same = 1;
    for (int j = 0; j < 24; j++) { if (shown[i][j] != s[j]) { same = 0; break; } if (!s[j]) break; }
    if (!same) { op1_text_set(tx[i], s); int j = 0; for (; j < 23 && s[j]; j++) shown[i][j] = s[j]; shown[i][j] = 0; }
    op1_text_draw(tx[i], r, ident);
}
static void make(void)
{
    op1_text_ctor(tx[T_TITLE], Q(8), 6, 12, c_grey);
    op1_text_ctor(tx[T_STATE], Q(200), 6, 12, c_white);
    op1_text_ctor(tx[T_BIG], Q(8), 24, 30, c_green);
    op1_text_ctor(tx[T_STEP], Q(8), 26, 16, c_white);
    op1_text_ctor(tx[T_BPM], Q(112), 40, 12, c_grey);
    op1_text_ctor(tx[T_HINT], Q(8), 48, 12, c_grey);
    op1_text_ctor(tx[T_PULSE], Q(176), 58, 10, c_grey);
    op1_text_ctor(tx[T_BEAT], Q(234), 58, 10, c_grey);
    for (int k = 0; k < 4; k++) {
        op1_text_ctor(tx[T_LAB + k], Q(8 + 78 * k), 114, 12, col4[k]);
        op1_text_ctor(tx[T_VAL + k], Q(8 + 78 * k), 127, 12, c_white);
    }
    made = 1;
}
static const char *const state_word[5] = { "midi clock", "not following", "no input", "waiting", "following" };

void pcp_draw(void *r)
{
    if (!made) make();
    int s = pcp_state();
    char t[24], *p;
    txt(r, T_TITLE, "pulse sync");
    if (s != PS_MIDI) txt(r, T_STATE, state_word[s]);       /* midi: no state word */
    if (s == PS_WAIT || s == PS_FOLLOW) {                    /* the tempo from the pulses, the pulse lamp, the beat dots */
        uint32_t ppq = pc_ppqs[pc_src], b10 = s == PS_FOLLOW && pc_period ? 44100u * 600u / (pc_period * ppq) : 0;
        if (b10) { p = num(t, b10 / 10); *p++ = '.'; num(p, b10 % 10); } else put(t, "---.-");
        txt(r, T_BIG, t);
        txt(r, T_BPM, "bpm");
        static uint32_t seen; static int flash;
        if (pc_pulses != seen) { seen = pc_pulses; flash = 3; }
        box(r, 190, 42, 6, s == PS_FOLLOW && flash > 0, s == PS_FOLLOW ? c_green : c_grey);
        if (flash > 0) flash--;
        int beat = s == PS_FOLLOW ? (int)((pc_count / ppq) & 3) : -1;
        for (int i = 0; i < 4; i++) box(r, 240 + 18 * i, 42, 4, i == beat, i == beat ? c_white : c_grey);
        txt(r, T_PULSE, "pulse");
        txt(r, T_BEAT, "beat");
    } else {                                                 /* the one missing step */
        static const char *const step[3] = { "uses midi clock", "set sync to follow", "select line in or usb in" };
        static const char *const how[3] = { "", "tempo page: green", "input page" };
        int k = s == PS_MIDI ? 0 : s == PS_NOFOLLOW ? 1 : 2;
        txt(r, T_STEP, step[k]);
        if (k) txt(r, T_HINT, how[k]);                       /* midi: no hint */
        if (s != PS_MIDI) goto bar_and_cols;
        goto cols;
    }
bar_and_cols:
    {                                                        /* pulse level against the trigger */
        int lit = s == PS_WAIT || s == PS_FOLLOW;
        int32_t j = lit ? pc_show_jump : 0, tr = pc_show_trig > 0 ? pc_show_trig : 600;
        int xj = bar_x(j), xt = bar_x(tr);
        ln(r, Q(8), Q(82), Q(312), Q(82), Q(4), c_dim);
        if (xj > 8) ln(r, Q(8), Q(82), Q(xj), Q(82), Q(4), j >= tr ? c_green : c_grey);
        ln(r, Q(xt), Q(75), Q(xt), Q(89), 0x600, c_red);
    }
cols:
    for (int k = 0; k < 4; k++) ln(r, Q(8 + 78 * k), Q(110), Q(78 + 78 * k), Q(110), 0x800, col4[k]);
    static const char *const lab[4] = { "rate", "start", "edge", "level" };
    int sh = op1_shift_held();                                     /* Shift held: the red column is the channel */
    for (int k = 0; k < 4; k++) txt(r, T_LAB + k, k == 3 && sh ? "channel" : lab[k]);
    if (pc_src) { p = put(t, "ppq "); num(p, pc_ppqs[pc_src]); } else put(t, "off");   /* off: the stock midi clock */
    txt(r, T_VAL, t);
    txt(r, T_VAL + 1, pc_manual ? "manual" : "auto");
    txt(r, T_VAL + 2, pc_fall ? "fall" : "rise");
    static const char *const ch_word[3] = { "both", "left", "right" };
    if (sh) put(t, ch_word[pc_chan <= 2 ? pc_chan : 0]);
    else if (pc_level) num(t, pc_level); else put(t, "auto");
    txt(r, T_VAL + 3, t);
}

/* the page's screen object (dispatcher.c: tempo slot 0x22 on T2) */
static void pcp_init(void *scr) { (void)scr; }
static void pcp_update(void *scr, int dt) { op1_screen_draw(scr, dt); pcp_draw(op1_raster()); }
const op1_screen_t SCREEN_pcpage = { pcp_init, pcp_update };
