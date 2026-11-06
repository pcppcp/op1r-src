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
 * - GUI: the tempo screen draws "ppq 4" centred over the sync box, a flash before it per pulse (pc_tempo_draw, draw slot 0x01911C08);
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

#ifdef OP1_HOST
extern int16_t host_pc_in[64];
extern int32_t host_sync_mode, host_ticks, host_ctrl;
extern uint32_t host_ms;
extern void host_bpm_clock(uint32_t lo, uint32_t hi);
extern void host_rx_inject(uint8_t b);
#define IN_BLOCK    host_pc_in
#define SYNC_MODE   host_sync_mode
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
#define SYNC_MODE   (*(volatile int32_t *)0x018C1870)                 /* tempo sync mode 0..4 */
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

/* jack input (hardware, docs/59): the analog couplings swing back after each gate edge, so on the level the falling
 * edge of a 5..20 ms gate counted as a second pulse. A rising edge is a near-vertical jump, the swing back is slow:
 * trigger on the 2-sample rise d = x[n] - x[n-2] at half the recent jump height (sinking slowly while pulses are
 * weaker, floor above the noise), re-arm below a quarter, and at most one pulse per half the interval at 300 bpm */
void pc_block(void)
{
    uint32_t t0 = pc_samples;
    pc_samples = t0 + 64;
    if (!pc_src) return;
    int32_t hi = height >> 1, lo = height >> 2;
    if (hi < PC_FLOOR) hi = PC_FLOOR, lo = PC_FLOOR >> 1;
    uint32_t gap = (uint32_t)(SR * 60 / 300 / 2) / pc_ppq[pc_src];   /* 1: 100 ms .. 48: 2.1 ms */
    if (gap < PC_GAP) gap = PC_GAP;
    int32_t a = x1, b = x2;
    for (int i = 0; i < 64; i++) {
        int32_t x = IN_BLOCK[i], d = x - b;
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
    height -= height >> 9;                                   /* ~0.7 s: weaker pulses are found again */
    if (height < PC_FLOOR) height = PC_FLOOR;
}

/* ---- tick side (PlayerManager tick context) ---- */
static uint32_t pc_r;
static uint8_t running;
static uint32_t last, period, pcount;
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
        RX_INJECT(0xFA);
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
    if (since > wait) { running = 0; RX_INJECT(0xFC); return; }
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
static void pc_turn(int delta)              /* KeyHandler: one value per detent; MIDI -> right goes to 4 PPQ */
{
    static int32_t acc;
    if ((acc > 0 && delta < 0) || (acc < 0 && delta > 0)) acc = 0;
    acc += delta;
    int st = acc / 2048; if (!st) return;                  /* a detent = 2 events of 1024 */
    acc = 0;
    int v = pc_src;
    if (v == 0 && st > 0) v = 3;                             /* default JACK 4 */
    else { v += st; v = v < 0 ? 0 : v > 5 ? 5 : v; }
    if (v != pc_src) { pc_src = (uint8_t)v; op1r_setting_put("clksrc", v); }
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
static const uint8_t green[4] = { 0x00, 0xED, 0x95, 0xFF };

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
static int32_t text_w(const void *t)                        /* drawn width, Q10 logical px: the bounds / display factor (measured) */
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
    if (!pc_src || (m != 2 && m != 4)) return;
    unsigned ppq = pc_ppq[pc_src];
    static int32_t half_w;                                   /* half the text width, Q10 (for the dot) */
    if (!txt_ok || txt_src != pc_src) {                     /* "ppq 4", centred on the SYNC box (x 144.0) */
        txt_ok = 1; txt_src = pc_src;
        char c[12] = "ppq ", *p = put_u(c + 4, ppq);
        *p = 0;
        ((void *(*)(void *, int32_t, int32_t, int32_t, const uint8_t *, int32_t))0x01A61904)(txt[0], PC_BOX_X, PC_TXT_Y, 10, green, 1);
        op1_text_set(txt[0], c);
        half_w = text_w(txt[0]) / 2;
    }
    void *r = op1_raster();
    op1_text_draw(txt[0], r, ident);                         /* between the middle graphic and the box */
    static uint32_t seen; static int flash;
    if (pc_pulses != seen) { seen = pc_pulses; flash = 4; }  /* a few frames per pulse */
    if (flash > 0) {
        flash--;
        int32_t x = PC_BOX_X - half_w - OP1_Q10(5) - 512, y = OP1_Q10(PC_TXT_Y) + OP1_Q10(PC_TXT_H) / 2;   /* text's centre line (measured) */
        op1_pt a0 = { x - OP1_Q10(2), y }, a1 = { x + OP1_Q10(2), y }, b0 = { x, y - OP1_Q10(2) }, b1 = { x, y + OP1_Q10(2) };
        op1_raster_line(r, &a0, &a1, 0x600, green);            /* a dot as two crossing segments (rasteriser) */
        op1_raster_line(r, &b0, &b1, 0x600, green);
    }
}
#else
void pc_host_turn(int delta) { pc_turn(delta); }
#endif
