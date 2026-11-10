/* Pulse clock in (docs/59): follow an analog / modular clock on the input jack, like MIDI clock in.
 * Tempo page Shift + white: CLOCK MIDI (stock) / JACK 1 / 2 / 4 / 24 / 48 PPQ (op1rsettings 'clksrc'); JACK takes
 * effect while the green knob is on a follow mode (2 / 4), as MIDI clock does.
 * - audio (pc_block, per 64-sample block after the input block 0xFF800CB4 is filled): rising-edge detector with
 *   hysteresis (the firmware's input high-pass turns a gate into a positive spike on its rising edge); pulse times
 *   (in samples) into a small queue.
 * - sequencer tick (pc_poll, from du_rx_poll right before the stock MIDI parser): per pulse 96 / ppq ticks into the
 *   stock pending-tick counter 0x018B6070 (an F8 adds 4); between pulses the ticks are spread over the last period and
 *   each pulse completes its share, so pulse k = tick k * N exactly. Every quarter-note pulse feeds the stock tempo
 *   averager as every 24th F8 does. First pulse after silence: FA, no pulse for 2 beats: FC (both through the RX ring,
 *   so the stock start / stop and op1r's start alignment apply). While JACK is on, MIDI F8 / FA / FB / FC are dropped.
 * - GUI: the tempo screen draws the clock source, "midi" or "ppq 4" (white), centred over the sync box while following
 *   (pc_tempo_draw, draw slot 0x01911C08);
 *   pc_tempo_knob takes knob 6 (Shift + white) before the stock on_knob (slot 0x01911BD0). */
#include "../op1_synth.h"
#include "../screens/op1r_settings.h"

#define SR          44100
#define PC_FLOOR    600                     /* lowest trigger level (int16 input block, after the stock high-pass) */
#define PC_START    8192                    /* pulse height assumed before the first pulse */
#define PC_GAP      88                      /* samples: at most one pulse per 2 ms (48 PPQ at 220 bpm: 5.7 ms) */
#define PC_NOPERIOD (3 * SR)                /* no second pulse yet: stop after 3 s */
#define PC_MAXWAIT  (6 * SR)                /* never wait longer than the stock 6 s clock timeout */

static const uint8_t pc_ppq[6] = { 0, 1, 2, 4, 24, 48 };   /* index 0: MIDI (stock) */
volatile uint8_t pc_src;                    /* index into pc_ppq */
/* T2 page settings (screens/pcpage.c): start auto (FA on the first pulse, FC after a gap) / manual (no FA / FC),
 * edge rise / fall (inverted clocks), level 0 auto (adaptive) / 1..6 fixed trigger on the 2-sample jump */
volatile uint8_t pc_manual, pc_fall, pc_level;
/* channel (Shift + red on the page, op1rsettings clkch): 0 both (the stock mono sum L + R, muted while following),
 * 1 left / 2 right: the clock from that channel only, the other channel stays the input's audio */
volatile uint8_t pc_chan;
/* the codec's input of this block (sysmon sm_audio_cb: the audio callback's 2nd / 3rd argument): 64 frames of L, R
 * as 32-bit words (sample in the high half). The stock fill of 0xFF800CB4 (core_block_process 0xFFA01A08): source 3
 * (USB) reads the 3rd argument, else the 2nd; source 0 without the flag 0x01BA15DC scales each channel by
 * sat(x << 2) * 0x65C28F5C / 2^31 (path 0xFFA01A30), else plain (0xFFA030D0); then L + R, high half. */
const int32_t *volatile pc_in_b, *volatile pc_in_c;
const int16_t pc_level_val[7] = { 0, 800, 1600, 3200, 6400, 12800, 25600 };
/* for the page: the largest jump of the last blocks (decaying) and the trigger level now */
volatile int32_t pc_show_jump, pc_show_trig;

#ifdef OP1_HOST
extern int16_t host_pc_in[64];
extern int32_t host_sync_mode, host_ticks, host_ctrl;
extern uint32_t host_ms;
extern void host_bpm_clock(uint32_t lo, uint32_t hi);
extern void host_rx_inject(uint8_t b);
#define IN_BLOCK    host_pc_in
extern int32_t host_pc_raw[128];
#define IN_FLAG     0
#define SYNC_MODE   host_sync_mode
extern int32_t host_input_source, host_line_jack;
#define INPUT_SOURCE host_input_source
#define LINE_JACK   host_line_jack
#define TICKS       host_ticks
#define CTRL_MODE   host_ctrl
#define TIME_MS()   host_ms
#define BPM_CLOCK(lo, hi) host_bpm_clock((lo), (hi))
#define RX_INJECT(b) host_rx_inject(b)
#define CLOCK_SEEN()
static uint32_t irq_off(void) { return 0; }
static void irq_on(uint32_t m) { (void)m; }
#else
#define IN_BLOCK    ((volatile int16_t *)0xFF800CB4)                  /* mono input, 64 x int16 */
#define IN_FLAG     (*(volatile uint8_t *)0x01BA15DC)                 /* stock fill: plain path for source 0 */
#define SYNC_MODE   (*(volatile int32_t *)0x018C1870)                 /* tempo sync mode 0..4 */
#define INPUT_SOURCE (*(volatile uint32_t *)0xFF8010B4)              /* selected input: 0 mic, 1 line, 2 radio, 3 USB */
#define LINE_JACK   (*(volatile uint8_t *)0x018B621C)                 /* a cable in the line jack (kennel kn_input.c) */
#define TICKS       (*(volatile int32_t *)0x018B6070)                 /* pending sequencer ticks (F8: += 4) */
#define CTRL_MODE   (*(volatile uint8_t *)0x018B6074)                 /* COM CTRL mode: the parser ignores clock */
#define TIME_MS()   (((uint32_t (*)(void))0x01A0DCE4)())
#define TEMPO_OBJ() (((void *(*)(void))0x01A5ED44)())                 /* TempoController::instance */
#define BPM_CLOCK(lo, hi) (((void (*)(void *, uint32_t, uint32_t))0x01A5EB30)(TEMPO_OBJ(), (lo), (hi)))
#define CLOCK_SEEN() do { *(volatile uint8_t *)0x018B600E = 1; } while (0)  /* as F8 (clock-in flag) */
#define RX_RING     ((volatile uint8_t *)0x01AB0510)
#define RX_WR       (*(volatile uint32_t *)0x01AB1510)
static uint32_t irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m)); return m; }
static void irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m)); }
static void RX_INJECT(uint8_t b)            /* one USB-MIDI packet (cable 0, CIN F: single byte) into the RX ring */
{
    uint32_t m = irq_off();                 /* the USB interrupt appends to the same ring */
    uint32_t w = RX_WR;
    volatile uint8_t *p = RX_RING + (w & 0x3FF) * 4;
    p[0] = 0x0F; p[1] = b; p[2] = 0; p[3] = 0;
    RX_WR = w + 1;
    irq_on(m);
}
#endif

/* ---- audio side ---- */
static volatile uint32_t pc_samples;        /* sample clock (block starts) */
static volatile uint32_t pc_q[16];          /* pulse times, written by the audio thread only */
static volatile uint32_t pc_w;
static uint8_t armed = 1;
static uint32_t last_edge = 0u - (1u << 30);  /* last accepted edge (start: long ago) */
static int32_t height = PC_START, peak;     /* recent jump height, the current pulse's peak jump */
static uint32_t peak_left;                  /* samples left to measure the current pulse's peak */
static int32_t x1, x2;                      /* the two previous input samples */

static int pc_active(void)
{
    int m = SYNC_MODE;
    return pc_src && (m == 2 || m == 4) && !CTRL_MODE;
}
/* the jack clock listens to line in or USB audio in, never the mic or the radio (line: the source, or a cable in the
 * line jack while the source is not radio / USB, as kennel's line clock decides it) */
int pc_input_ok(void)
{
    uint32_t s = INPUT_SOURCE;
    return s == 1 || s == 3 || (LINE_JACK && s != 2);
}
/* while the jack clock follows, the input is kept out of the dry monitor and the tape (input/in_host.c in_bus clears the
 * block's has-input flag, after kennel's line clock): the clicks are neither heard nor recorded */
int pc_mute(void) { return pc_active() && pc_input_ok() && !pc_chan; }

static const int32_t *raw_block(int *scaled)                /* this block's codec input as the stock fill reads it */
{
#ifdef OP1_HOST
    *scaled = 0; return host_pc_raw;
#else
    uint32_t s = INPUT_SOURCE;
    *scaled = s == 0 && !IN_FLAG;
    return s == 3 ? pc_in_c : pc_in_b;
#endif
}
static inline int32_t raw_sample(const int32_t *raw, int i, int ch, int scaled)   /* one channel, as the mono fill */
{
    int32_t v = raw[2 * i + ch];
    if (scaled) {
        v = v > 0x1FFFFFFF ? 0x7FFFFFFF : v < -0x20000000 ? (int32_t)0x80000000 : v << 2;
        v = (int32_t)(((int64_t)v * 0x65C28F5C) >> 31);
    }
    return v >> 16;
}
/* while following with one channel: the input block becomes the other channel (input/in_host.c in_bus, before the
 * input is used); 1 = done */
int pc_split(void)
{
    if (!pc_chan || !pc_active() || !pc_input_ok()) return 0;
    int scaled; const int32_t *raw = raw_block(&scaled);
    if (!raw) return 0;
    int ch = pc_chan == 1 ? 1 : 0;                          /* the audio: the channel the clock is not on */
#ifdef OP1_HOST
    int16_t *o = host_pc_in;
#else
    volatile int16_t *o = (volatile int16_t *)0xFF800CB4;
#endif
    for (int i = 0; i < 64; i++) o[i] = (int16_t)raw_sample(raw, i, ch, scaled);
    return 1;
}

/* jack input (hardware, docs/59): the analog couplings swing back after each gate edge, so on the level the falling
 * edge of a 5..20 ms gate counted as a second pulse. A rising edge is a near-vertical jump, the swing back is slow:
 * trigger on the 2-sample rise d = x[n] - x[n-2] at half the recent jump height (sinking slowly while pulses are
 * weaker, floor above the noise), re-arm below a quarter, and at most one pulse per half the interval at 300 bpm */
void pc_block(void)
{
    uint32_t t0 = pc_samples;
    pc_samples = t0 + 64;
    if (!pc_src || !pc_input_ok()) { armed = 1; return; }
    int32_t hi = height >> 1, lo = height >> 2;
    if (hi < PC_FLOOR) hi = PC_FLOOR, lo = PC_FLOOR >> 1;
    if (pc_level > 0 && pc_level <= 6) { hi = pc_level_val[pc_level]; lo = hi >> 2; }   /* fixed level */
    int32_t jmax = 0, sgn = pc_fall ? -1 : 1;
    uint32_t gap = (uint32_t)(SR * 60 / 300 / 2) / pc_ppq[pc_src];   /* 1: 100 ms .. 48: 2.1 ms */
    if (gap < PC_GAP) gap = PC_GAP;
    int32_t a = x1, b = x2;
    int scaled = 0; const int32_t *raw = pc_chan ? raw_block(&scaled) : 0;   /* one channel: from the codec block */
    if (pc_chan && !raw) { armed = 1; return; }
    for (int i = 0; i < 64; i++) {
        int32_t x = raw ? raw_sample(raw, i, pc_chan - 1, scaled) : IN_BLOCK[i], d = (x - b) * sgn;
        if (d > jmax) jmax = d;
        b = a; a = x;
        uint32_t t = t0 + (uint32_t)i;
        if (peak_left) {                                     /* height of the jump just accepted (2 ms) */
            if (d > peak) peak = d;
            if (!--peak_left) height = (height * 3 + peak) >> 2;
        }
        if (armed) {
            if (d > hi && t - last_edge >= gap) {
                armed = 0; last_edge = t; peak = d; peak_left = PC_GAP;
                pc_q[pc_w & 15] = t; pc_w++;
            }
        } else if (d < lo) armed = 1;
    }
    x1 = a; x2 = b;
    pc_show_jump = jmax > pc_show_jump ? jmax : pc_show_jump - (pc_show_jump >> 4);
    pc_show_trig = hi;
    height -= height >> 9;                                   /* ~0.7 s: weaker pulses are found again */
    if (height < PC_FLOOR) height = PC_FLOOR;
}

/* ---- tick side (PlayerManager tick context) ---- */
static uint32_t pc_r;
uint8_t pc_running;
#define running pc_running
uint32_t pc_period, pc_count;
#define period pc_period
#define pcount pc_count
static uint32_t last;
static int32_t emitted;                     /* ticks given since the last pulse */
static int run_n;                           /* ticks per pulse they were counted with */
volatile uint32_t pc_pulses;                /* pulses seen (screen flash) */

static void quarter(uint32_t t, uint32_t now)               /* as every 24th F8: one quarter-note timestamp (us) */
{
    uint32_t age_us = (uint32_t)(((uint64_t)(now - t) * 1000000u) / SR);
    BPM_CLOCK(TIME_MS() * 1000u - age_us, 0);               /* same base as the stock 6 s check (high word 0) */
    CLOCK_SEEN();
}

static void pulse(uint32_t t, uint32_t now, int n, int ppq)
{
    pc_pulses++;
    if (!running) {                                          /* first pulse after silence: start, tick 0 */
        running = 1; pcount = 0; emitted = 0; period = 0; last = t; run_n = n;
        if (!pc_manual) RX_INJECT(0xFA);
        quarter(t, now);
        return;
    }
    uint32_t p = t - last;
    period = period ? (period + p) / 2 : p;                  /* light smoothing of the jitter */
    if (n > emitted) TICKS += n - emitted;                   /* this pulse completes its share (emitted <= n - 1) */
    emitted = 0;
    last = t;
    if (++pcount % (uint32_t)ppq == 0) quarter(t, now);
}

void pc_poll(void)
{
    uint32_t now = pc_samples;
    if (!pc_active()) { pc_r = pc_w; running = 0; return; }
    int ppq = pc_ppq[pc_src], n = 96 / ppq;
    /* PPQ changed while running: the ticks already given were a share of the old n; rescale them to the new one, so
     * the pulse completes the same position (hardware: a negative completion made the pending ticks < 0, assert
     * PlayerManager.cpp:366 midi_ext_clock_in >= 0) */
    if (running && n != run_n) { emitted = emitted * n / run_n; run_n = n; }   /* (the pulse period is the source's: kept) */
    while (pc_r != pc_w) { uint32_t t = pc_q[pc_r & 15]; pc_r++; pulse(t, now, n, ppq); }
    if (!running) return;
    uint32_t since = now - last;
    uint32_t wait = period ? 2u * (uint32_t)ppq * period : PC_NOPERIOD;   /* 2 beats */
    if (wait > PC_MAXWAIT) wait = PC_MAXWAIT;
    if (since > wait) { running = 0; if (!pc_manual) RX_INJECT(0xFC); return; }
    if (!period) return;
    int32_t due = (int32_t)((uint64_t)since * (uint32_t)n / period);
    if (due > n - 1) due = n - 1;                            /* the last one comes with the next pulse */
    if (due > emitted) { TICKS += due - emitted; emitted = due; }
}

/* drop MIDI clock / start / stop while JACK is the source (du_rx_poll, before the parser) */
void pc_filter(volatile uint8_t *pk)
{
    if (!pc_active() || (pk[0] & 15) != 0xF) return;
    uint8_t b = pk[1];
    if (b == 0xF8 || b == 0xFA || b == 0xFB || b == 0xFC) pk[0] &= 0xF0;   /* CIN 0: the parser ignores it */
}

/* ---- GUI ---- */
volatile uint32_t pc_show_until;            /* the tempo screen shows the source until this time_ms (after a change) */
/* the stock "clock present" (0x019DE70C): the newest tempo timestamp [0xFF907CDC] in 0x01AADAF8 (us, 64-bit) is younger
 * than [0x018E3FE4] (6 s); EXT on the tempo screen follows it. */
#ifndef OP1_HOST
static int clock_present(void)
{
    int idx = *(volatile int32_t *)0xFF907CDC;
    if (idx < 0 || idx > 15) return 0;
    uint32_t ts = ((volatile uint32_t *)0x01AADAF8)[idx * 2], now = TIME_MS() * 1000u;
    return now - ts < *(volatile uint32_t *)0x018E3FE4;
}
static void forget_clock(void)              /* the jack's timestamps no longer count: EXT goes (a MIDI clock sets it again) */
{
    int idx = *(volatile int32_t *)0xFF907CDC;
    if (idx < 0 || idx > 15) return;
    ((volatile uint32_t *)0x01AADAF8)[idx * 2] = 0; ((volatile uint32_t *)0x01AADAF8)[idx * 2 + 1] = 0;
}
#else
static int clock_present(void) { return 0; }
static void forget_clock(void) {}
#endif
void pc_set_src(int v)                      /* rate setting (Shift + white, the T2 page) */
{
    if (v == pc_src) return;
    if (pc_src && !v && running) forget_clock();             /* followed the jack, now MIDI: reset EXT */
    pc_src = (uint8_t)v; op1r_setting_put("clksrc", v);
    pc_show_until = TIME_MS() + 2000u;
}
static void pc_turn(int delta)              /* KeyHandler: one value per click; MIDI -> right goes to 4 PPQ */
{
    static int32_t acc;
    if ((acc > 0 && delta < 0) || (acc < 0 && delta > 0)) acc = 0;
    acc += delta;
    int st = acc / 1024; if (!st) return;                  /* one value per hardware click (1024; the emulator sends 2048) */
    acc = 0;
    int v = pc_src;
    if (v == 0 && st > 0) v = 3;                             /* default JACK 4 */
    else { v += st; v = v < 0 ? 0 : v > 5 ? 5 : v; }
    if (v != pc_src) pc_set_src(v);
    else pc_show_until = TIME_MS() + 2000u;                  /* (shown again even at an end) */
}

#ifndef OP1_HOST
void pc_tempo_knob(void *self, int knob, int16_t *delta)    /* TempoController vtable +0x2C */
{
    if (knob == 6) { pc_turn(*delta); return; }              /* Shift + white: unused in the stock */
    ((void (*)(void *, int, int16_t *))0x01A5EF8C)(self, knob, delta);
}

static uint32_t txt[1][OP1_TEXT_SIZE / 4];
static uint8_t txt_ok, txt_src = 0xFF;
static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
static const uint8_t white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

void pc_load(void)                          /* GUI thread, boot (screens_load_hook) */
{
    int v = op1r_setting_get("clksrc", 0);
    pc_src = (uint8_t)(v >= 0 && v <= 5 ? v : 0);
}

static char *put_u(char *p, unsigned v) { char d[4]; int n = 0; do d[n++] = (char)('0' + v % 10); while (v /= 10); while (n) *p++ = d[--n]; return p; }
/* line between the middle graphic (EXT, or the PO graphic at 80 %: ends at y 85.3) and the box (top y 109): text rows
 * y 92..102 (size 10: 10.4 px; the y argument sits 1.4 px below the first row, measured) */
#define PC_TXT_Y 93
#define PC_TXT_H 10
#define PC_BOX_X (OP1_Q10(144) + 1229)                      /* centre of the SYNC / PO SYNC box (screenshot 146..213 of 400),
                                                             * + 1.2 px: the centred stock text lands 1.2 px left (measured) */
__attribute__((unused)) static int32_t text_w(const void *t)                        /* drawn width, Q10 logical px: the bounds / display factor (measured) */
{
    int32_t f = *(volatile int32_t *)0x01DFB654;
    if (*(volatile uint8_t *)0xFF906A54 == 1 && !*(volatile uint8_t *)0xFF906A56)
        f = !*(volatile uint8_t *)0xFF906A55 && !*(volatile uint8_t *)0x018BE698 ? 0x500 : 0x4B3;
    const int32_t *m = (const int32_t *)t;
    return f > 0 ? (int32_t)(((int64_t)(m[0x94 / 4] - m[0x90 / 4]) << 10) / f) : 0;
}

void pc_tempo_draw(void *scr, int dt)        /* TempoScreen vtable draw: stock, then the clock source */
{
    ((void (*)(void *, int))0x01A5F64C)(scr, dt);
    int m = SYNC_MODE;
    if (m != 2 && m != 4) return;                            /* not following a clock */
    int detected = pc_src ? running : clock_present();       /* the jack's pulses / a MIDI clock */
    if (!detected && (int32_t)(TIME_MS() - pc_show_until) >= 0) return;   /* else only for 2 s after a change */
    if (!txt_ok || txt_src != pc_src) {                     /* "midi" / "ppq 4", white, centred on the SYNC box (x 144.0) */
        txt_ok = 1; txt_src = pc_src;
        char c[12] = "midi";
        if (pc_src) { char *p = put_u(c + 4, pc_ppq[pc_src]); c[0] = 'p'; c[1] = 'p'; c[2] = 'q'; c[3] = ' '; *p = 0; }
        ((void *(*)(void *, int32_t, int32_t, int32_t, const uint8_t *, int32_t))0x01A61904)(txt[0], PC_BOX_X, PC_TXT_Y, 10, white, 1);
        op1_text_set(txt[0], c);
    }
    op1_text_draw(txt[0], op1_raster(), ident);              /* between the middle graphic and the box */
}
#else
void pc_host_turn(int delta) { pc_turn(delta); }
#endif
