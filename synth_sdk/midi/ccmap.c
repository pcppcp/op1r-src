/* CC groups (docs/60): keys (physical keys / encoders) and params (midi/mx.c) on their own channels, set on
 * COM -> OP-1 with Shift held (Shift + green KEYS CH, Shift + white PARAM CH, Shift + red FEEDBACK), plus feedback.
 * - RX (cc_rx, from du_rx_poll for every raw USB-MIDI packet, before the stock parser): a CC on the params channel
 *   with a params number -> mx_cc; on the keys channel: CC 5-52 = key code + 5 (value >= 64 press), CC 102-105 =
 *   encoders, relative (1..63 up, 65..127 down). Taken packets get CIN 0 (the stock parser skips them). The rest
 *   (notes, MIDI LFO CC 1-4, ...) stays stock, on the OP-1 channel.
 * - Keys inject like the key-scan ISR (key queue s_key_queue, encoder accumulators 0x01DD5250, interrupts off) and
 *   wake the KeyHandler (docs/11 §9). An update confirmation is never taken from MIDI.
 * - Feedback (cc_fb on): physical key presses / encoder turns -> the same CCs on the keys channel (events that came
 *   from MIDI are counted and not echoed); params: every 16 sequencer ticks (~23 ms) the targets are read
 *   (mx_read) and changed values sent on the params channel (a received value updates "last" first: no echo).
 *   All sending happens in the sequencer tick context (du_rx_poll), through the stock TX queue 0x019E0D18.
 * Settings (op1rsettings): cckeych 0 = same as the OP-1 channel (default), 1..16, 17 off; ccparch 1..16, 17 off
 * (default off); ccfb 0 / 1. */
#include "../op1_synth.h"
#include "../screens/op1r_settings.h"
#include "mx.h"

#define CH_OFF 17
volatile uint8_t cc_keych = 0, cc_parch = CH_OFF, cc_fb = 0;

#ifdef OP1_HOST
extern int host_op_ch, host_prompt;
extern void host_key(int code, int down), host_ticks(int knob, int ticks), host_tx(uint32_t pk);
#define OP_CH          host_op_ch
#define PROMPT_ACTIVE() host_prompt
#define PUSH_KEY(c, d) host_key((c), (d))
#define ADD_TICKS(k, t) host_ticks((k), (t))
#define WAKE()
#define TX(p)          host_tx(p)
#else
#define OP_CH          (*(volatile int *)0x01AADB20)            /* stock RX / TX channel 0..15 */
extern int du_prompt_active(void);
#define PROMPT_ACTIVE() du_prompt_active()
#define WAKE()         ((void (*)(void))0x019C2F04)()            /* KeyHandler::signal */
static uint32_t irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m)); return m; }
static void irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m)); }
static void PUSH_KEY(int code, int down)
{
    volatile uint32_t *rd = (volatile uint32_t *)0x01DD5244, *wr = (volatile uint32_t *)0x01DD5248;
    uint32_t m = irq_off();
    uint32_t w = *wr, n = (w + 1) & 0xFF;
    if (n != *rd && !*(volatile uint8_t *)0x01DD524D)       /* full or ISR mid-push: drop the event */
        ((volatile uint16_t *)0x01DD5044)[w] = (uint16_t)(code | down << 8), *wr = n;
    irq_on(m);
}
static void ADD_TICKS(int k, int t)
{
    uint32_t m = irq_off();
    ((volatile int32_t *)0x01DD5250)[k] += t;
    irq_on(m);
}
#define TX(p)          do { uint32_t _p = (p); ((void (*)(int, const void *, unsigned))0x019E0D18)(0, &_p, 1); } while (0)
#endif

static const uint8_t key_ok[48] = {                  /* key codes reachable over MIDI (no keyboard keys) */
    1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,                 /* help metro synth drum tape mixer T1-T4 lift drop scissors */
    1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,                 /* 1..5, sequencer */
    0,1,1,1,1,1,1,0,0,0,0,1,1,1,1,1};                /* rec play stop left right shift, mic com, 6..8 */
static uint8_t key_down[48];                         /* MIDI key state (only changes are injected) */
static volatile uint8_t inj_key[48];                 /* injected key events not seen by the feedback yet */
static volatile int16_t inj_tick[4];                 /* injected encoder ticks not seen by the feedback yet */

int cc_keys_channel(void) { return cc_keych == 0 ? (OP_CH & 15) : cc_keych <= 16 ? cc_keych - 1 : -1; }
int cc_params_channel(void) { return cc_parch >= 1 && cc_parch <= 16 ? cc_parch - 1 : -1; }

static uint8_t last[MX_N];                           /* params feedback: last value sent / received, 0xFF = none */
static uint8_t last_init;

/* ---- RX ---- */
void cc_rx(volatile uint8_t *pk)
{
    if ((pk[0] & 15) != 0xB) return;
    int ch = pk[1] & 15, cc = pk[2], v = pk[3];
    if (ch == cc_params_channel()) {
        int id = mx_index(cc);
        if (id >= 0) {
            mx_cc(cc, v);
            if (last_init) last[id] = (uint8_t)v;       /* no echo */
            pk[0] &= 0xF0;
            return;
        }
    }
    if (ch != cc_keys_channel()) return;
    int knob = cc - 102, code = cc - 5;
    if (knob >= 0 && knob < 4) {
        pk[0] &= 0xF0;
        if (!(v & 63)) return;                          /* 0 / 64: no movement */
        int t = (v < 64 ? v : v - 128) * 2;             /* 1 detent = 2 ticks */
        ADD_TICKS(knob, t); inj_tick[knob] = (int16_t)(inj_tick[knob] + t);
        WAKE();
        return;
    }
    if (code < 0 || code >= 48 || !key_ok[code]) return;
    pk[0] &= 0xF0;
    int down = v >= 64;
    if (key_down[code] == down || PROMPT_ACTIVE()) return;   /* security: only physical keys confirm an update */
    key_down[code] = (uint8_t)down;
    inj_key[code]++;
    PUSH_KEY(code, down);
    WAKE();
}

/* ---- feedback from the KeyHandler thread: a small ring, sent in the tick context ---- */
static volatile uint32_t fq[32];
static volatile uint32_t fq_w, fq_r;
static void fq_put(int cc, int v) { if (fq_w - fq_r < 32) { fq[fq_w & 31] = (uint32_t)(cc | v << 8); fq_w++; } }

void cc_fb_key(int code, int pressed)                 /* key_event_hook (KeyHandler) */
{
    if (code < 0 || code >= 48 || !key_ok[code]) return;
    if (inj_key[code]) { inj_key[code]--; return; }  /* came from MIDI */
    if (!cc_fb || cc_keys_channel() < 0) return;
    fq_put(code + 5, pressed ? 127 : 0);
}

void cc_fb_knob(int knob, int delta)                  /* knob_event_hook (KeyHandler): delta = ticks << 9 */
{
    static int32_t rem[4];
    if (knob < 0 || knob > 3) return;
    int t = delta / 512, i = inj_tick[knob];
    if (i && (i > 0) == (t > 0)) {                     /* MIDI ticks first */
        int use = (i > 0 ? (t < i ? t : i) : (t > i ? t : i));
        inj_tick[knob] = (int16_t)(i - use); t -= use;
    }
    if (!t || !cc_fb || cc_keys_channel() < 0) return;
    rem[knob] += t;
    int det = rem[knob] / 2;
    if (!det) return;
    rem[knob] -= det * 2;
    if (det > 63) det = 63; else if (det < -63) det = -63;
    fq_put(102 + knob, det > 0 ? det : 128 + det);
}

static void send_cc(int ch, int cc, int v) { TX(0x0Bu | (uint32_t)(0xB0 | ch) << 8 | (uint32_t)cc << 16 | (uint32_t)(v & 127) << 24); }

void cc_tx_poll(void)                                 /* du_rx_poll, every sequencer tick */
{
    int kc = cc_keys_channel();
    while (fq_r != fq_w) {
        uint32_t e = fq[fq_r & 31]; fq_r++;
        if (kc >= 0) send_cc(kc, (int)(e & 127), (int)(e >> 8 & 127));
    }
    static uint32_t tick;
    if (++tick & 15) return;
    int pc = cc_params_channel();
    if (!cc_fb || pc < 0) { last_init = 0; return; }
    if (!last_init) { for (int i = 0; i < MX_N; i++) last[i] = 0xFF; last_init = 1; }   /* (re)enabled: send all */
    int sent = 0;
    for (int id = 0; id < MX_N && sent < 8; id++) {   /* at most 8 messages per 23 ms */
        int v = mx_read(id);
        if (v < 0 || v == last[id]) continue;
        last[id] = (uint8_t)v;
        send_cc(pc, mx_cc_of(id), v);
        sent++;
    }
}

/* ---- COM -> OP-1, Shift held: Shift + green / white / red ---- */
static const uint8_t key_order[18] = { CH_OFF, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static int key_pos(int v) { for (int i = 0; i < 18; i++) if (key_order[i] == v) return i; return 1; }

void cc_turn(int idx, int delta)                      /* KeyHandler: idx 5 / 6 / 7 = Shift + green / white / red */
{
    static int32_t acc[3];
    int i = idx - 5;
    if (i < 0 || i > 2) return;
    if ((acc[i] > 0 && delta < 0) || (acc[i] < 0 && delta > 0)) acc[i] = 0;
    acc[i] += delta;
    int st = acc[i] / 2048; if (!st) return;          /* a detent = 2 events of 1024 */
    acc[i] = 0;
    if (i == 0) {                                     /* KEYS: OFF, OP, 1..16 */
        int p = key_pos(cc_keych) + st; p = p < 0 ? 0 : p > 17 ? 17 : p;
        if (key_order[p] != cc_keych) { cc_keych = key_order[p]; op1r_setting_put("cckeych", cc_keych); }
    } else if (i == 1) {                              /* PARAM: OFF, 1..16 */
        int p = (cc_parch == CH_OFF ? 0 : cc_parch) + st; p = p < 0 ? 0 : p > 16 ? 16 : p;
        int v = p ? p : CH_OFF;
        if (v != cc_parch) { cc_parch = (uint8_t)v; op1r_setting_put("ccparch", v); }
    } else {                                          /* FEEDBACK: right on, left off */
        int v = st > 0;
        if (v != cc_fb) { cc_fb = (uint8_t)v; op1r_setting_put("ccfb", v); }
    }
}

void cc_load(void)                                    /* GUI thread, boot */
{
    int v = op1r_setting_get("cckeych", 0); cc_keych = (uint8_t)(v >= 0 && v <= CH_OFF ? v : 0);
    v = op1r_setting_get("ccparch", CH_OFF); cc_parch = (uint8_t)(v >= 1 && v <= CH_OFF ? v : CH_OFF);
    v = op1r_setting_get("ccfb", 0); cc_fb = (uint8_t)(v == 1);
}

/* value texts, as drawn ("op" = same as the OP-1 channel; the font draws lowercase as capitals) */
static char *put2(char *p, int v) { *p++ = (char)('0' + v / 10); *p++ = (char)('0' + v % 10); return p; }
void cc_text(int row, char *s)
{
    int v = row == 0 ? cc_keych : row == 1 ? cc_parch : cc_fb;
    if (row == 2) { s[0] = 'o'; s[1] = v ? 'n' : 'f'; s[2] = v ? 0 : 'f'; s[3] = 0; return; }
    if (v == CH_OFF) { s[0] = 'o'; s[1] = 'f'; s[2] = 'f'; s[3] = 0; return; }
    if (v == 0) { s[0] = 'o'; s[1] = 'p'; s[2] = 0; return; }
    *put2(s, v) = 0;
}

#ifndef OP1_HOST
void cc_com_knob(void *self, int idx, int16_t *delta)  /* com_controller vtable +0x2C (slot 0x0190F718) */
{
    if (*(volatile int *)0x01AAA784 == 0 && idx >= 5 && idx <= 7) { cc_turn(idx, *delta); return; }   /* page OP-1 */
    ((void (*)(void *, int, int16_t *))0x01962D30)(self, idx, delta);
}

/* the MIDI CH panel (Shift held, page OP-1), widened by make_firmware.py com_panel_edit: border x 194..292, the
 * stock MIDI / CH label moved to x 200.3, its value (opmidich) right edge x 279.8. Our rows follow the stock row's
 * geometry at a 26 px pitch: label two lines (tops 15.8 / 25.7 + 26 r, 7.3 px tall), value right aligned (top
 * 16.1 + 26 r, 17.2 px tall). Text objects: y argument = first pixel row + CC_DY, left x = first column - CC_DX
 * (measured on screenshots). */
#define CC_LX    200.3                                /* label column (the stock label's left edge) */
#define CC_VX    279.8                                /* value right edge (the stock value's) */
#define CC_PITCH 26
#define CC_DY    1                                    /* text y argument below the first pixel row */
#define CC_DX    (-0.8)                               /* labels came out 1 screen px left of the stock label */
#define CC_VDX   (-0.8)                               /* values 1 screen px right of the stock value */
#define QF(x)    ((int32_t)((x) * 1024))
static uint32_t txt[6][OP1_TEXT_SIZE / 4], val[3][OP1_TEXT_SIZE / 4];
static char shown[3][4];
static uint8_t txt_ok;
static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
static const uint8_t lilac[4] = { 0x55, 0x53, 0x80, 0xFF }, vcol[3][4] = { { 0x00, 0xED, 0x95, 0xFF },   /* stock label #555380 */
                                  { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0x3A, 0x5D, 0xFF } };
static const char *const lab[3][2] = { { "keys", "ch" }, { "param", "ch" }, { "feed", "back" } };
#define TEXT_CTOR(o, x, y, sz, c, al) ((void *(*)(void *, int32_t, int32_t, int32_t, const uint8_t *, int32_t))0x01A61904)((o), (x), (y), (sz), (c), (al))

void cc_com_draw(void *scr, int dt)                    /* ComScreen vtable update (slot 0x0190F750) */
{
    ((void (*)(void *, int))0x019655E0)(scr, dt);
    if (*(volatile int *)0x01BAB014 != 0 || !*(volatile uint8_t *)0x01DD7F78) return;   /* page OP-1, Shift held */
    if (!txt_ok) {
        for (int r = 0; r < 3; r++) {
            int y = (int)(15.8 + CC_PITCH * (r + 1) + 0.5) + CC_DY, y2 = (int)(25.7 + CC_PITCH * (r + 1) + 0.5) + CC_DY;
            TEXT_CTOR(txt[2 * r], QF(CC_LX - CC_DX), y, 7, lilac, 0); op1_text_set(txt[2 * r], lab[r][0]);
            TEXT_CTOR(txt[2 * r + 1], QF(CC_LX - CC_DX), y2, 7, lilac, 0); op1_text_set(txt[2 * r + 1], lab[r][1]);
            TEXT_CTOR(val[r], QF(CC_VX + CC_VDX), (int)(16.1 + CC_PITCH * (r + 1) - 0.7), 17, vcol[r], 2);   /* (1.5 screen px up, measured) */
        }
        txt_ok = 1;
    }
    void *r = op1_raster();
    for (int i = 0; i < 3; i++) {
        char s[4]; cc_text(i, s);
        int k = 0; while (k < 4 && s[k] == shown[i][k] && s[k]) k++;
        if (s[k] != shown[i][k]) { op1_text_set(val[i], s); for (k = 0; k < 4; k++) shown[i][k] = s[k]; }
        op1_text_draw(txt[2 * i], r, ident); op1_text_draw(txt[2 * i + 1], r, ident); op1_text_draw(val[i], r, ident);
    }
}
#endif
