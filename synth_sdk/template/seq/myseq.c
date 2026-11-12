/* Template sequencer: repeats the last key played on its page, one note per step.
 *   blue  : rate 1/4 1/8 1/16 1/32
 *   green : gate 10..100 %
 *   white : octave -2..+2
 *   red   : turn right = start, left = stop
 * One state per mode (0 synth, 1 drum). Not saved: it starts with the defaults after every boot.
 * The top part is plain C (host-testable, myseq_test.c); the firmware part at the end connects it to the OP-1's
 * player as the shipped sequencers do (seq/euclid.c). Ticks: the player calls tick 96 times per beat.
 * Copyright (C) <year> <you>. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_synth.h"

static const uint8_t my_rate_ticks[4] = { 96, 48, 24, 12 };   /* 1/4 1/8 1/16 1/32 at 96 ticks per beat */

typedef struct {
    uint8_t note, key, has_note;          /* note: absolute pitch; key 0..23 (drums: which sound) */
    uint8_t running, rate, gate, oct;     /* rate 0..3, gate 10..100 %, oct 0..4 = -2..+2 */
    uint8_t sounding;                     /* note id on + 1, 0 = none */
    uint8_t step;                         /* step within the beat (screen) */
    uint16_t t;                           /* ticks since the step started */
    int32_t acc[4];                       /* encoder accumulators: one value per 1024 */
} my_seq_t;
static my_seq_t my[2] = { { .rate = 2, .gate = 50, .oct = 2 }, { .rate = 2, .gate = 50, .oct = 2 } };

#ifdef OP1_HOST
void host_seq_note_on(int mode, int note, int vel);             /* harness/plugin_test.h logs the notes */
void host_seq_note_off(int mode, int note);
#define MY_NOTE_ON(m, n, k, v) (host_seq_note_on((m), (n), (v)), (n))
#define MY_NOTE_OFF(m, id)     host_seq_note_off((m), (id))
static uint32_t my_irq_off(void) { return 0; }
static void my_irq_on(uint32_t m) { (void)m; }
#else
int  eu_fw_note_on(int pos, int note, int key, int vel);        /* core (seq/seq_core.c): notes through VoiceCtl */
void eu_fw_note_off(int midi);
#define MY_NOTE_ON(m, n, k, v) eu_fw_note_on((m), (n), (k), (v))
#define MY_NOTE_OFF(m, id)     eu_fw_note_off(id)
static uint32_t my_irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m) : : "memory"); return m; }
static void my_irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m) : "memory"); }
#endif

/* the key thread and the player tick both end the note: take its id with interrupts off, send the note-off after.
 * (Clearing after the note-off would lose a note the tick starts meanwhile: it would never get its note-off.) */
static void my_silence(int m)
{
    uint32_t im = my_irq_off();
    int id = my[m].sounding;
    my[m].sounding = 0;
    my_irq_on(im);
    if (id) MY_NOTE_OFF(m, id - 1);
}

/* a note key on the sequencer's page: this note from now on */
void myseq_key(int m, int note, int key)
{
    my_seq_t *s = &my[m & 1];
    s->note = (uint8_t)note; s->key = (uint8_t)key; s->has_note = 1;
}

/* encoder turn on its page: delta in 1/1024 detents, as the core sends it */
void myseq_turn(int m, int knob, int delta)
{
    my_seq_t *s = &my[m & 1];
    if (knob < 0 || knob > 3) return;
    if ((s->acc[knob] > 0 && delta < 0) || (s->acc[knob] < 0 && delta > 0)) s->acc[knob] = 0;   /* direction change */
    s->acc[knob] += delta;
    int d = s->acc[knob] / 1024;                                     /* per turn, not per sample: a division is fine */
    if (!d) return;
    s->acc[knob] -= d * 1024;
    if (knob == 0) { int r = s->rate + d; s->rate = (uint8_t)(r < 0 ? 0 : r > 3 ? 3 : r); }
    if (knob == 1) { int g = s->gate + d * 10; s->gate = (uint8_t)(g < 10 ? 10 : g > 100 ? 100 : g); }
    if (knob == 2) { int o = s->oct + d; s->oct = (uint8_t)(o < 0 ? 0 : o > 4 ? 4 : o); }
    if (knob == 3) {
        /* tick (sequencer tick) preempts this (UI thread): set up before running, stop before the note-off */
        if (d > 0 && !s->running) { s->t = 0xFFFF; s->step = 0xFF; s->running = 1; }   /* first step: the next tick */
        if (d < 0) { s->running = 0; my_silence(m & 1); }
    }
}

/* one player tick (96 per beat) */
void myseq_tick(int m)
{
    my_seq_t *s = &my[m & 1];
    if (!s->running) return;
    int len = my_rate_ticks[s->rate];
    s->t = (uint16_t)(s->t + 1 >= len ? 0 : s->t + 1);            /* 0xFFFF + 1 wraps to the first step */
    if (s->t == 0) {
        my_silence(m & 1);
        s->step = (uint8_t)(s->step + 1 >= 96 / len ? 0 : s->step + 1);   /* per step: a division is fine */
        int n = s->note + (s->oct - 2) * 12;
        if (s->has_note && n >= 0 && n <= 127) {
            int id = MY_NOTE_ON(m & 1, n, s->key, 100);
            if (id >= 0) s->sounding = (uint8_t)(id + 1);
        }
    } else if (s->t == len * s->gate / 100) {
        my_silence(m & 1);                                           /* gate over (gate 100: the next step's note-off) */
    }
}

void myseq_all_off(int m) { my_silence(m & 1); }
void myseq_midi_start(void) { for (int m = 0; m < 2; m++) if (my[m].running) { my_silence(m); my[m].t = 0xFFFF; my[m].step = 0xFF; } }

/* ---- screen (GUI thread, every frame): the steps of one beat, the current one lit; myseq.svg the static parts ---- */
void myseq_screen_draw(void *r, int m)
{
    static const uint8_t grey[4] = { 0x6F, 0x71, 0x73, 0xFF }, green[4] = { 0x00, 0xED, 0x95, 0xFF };
    const my_seq_t *s = &my[m & 1];
    int steps = 96 / my_rate_ticks[s->rate], cur = s->running ? s->step : -1;
    int w = 240 / steps;
    for (int i = 0; i < steps; i++) {
        int32_t x0 = OP1_Q10(40 + i * w + 2), x1 = OP1_Q10(40 + (i + 1) * w - 2);
        int32_t top = OP1_Q10(100 - (i == cur ? 40 : 20) * s->gate / 100);
        op1_pt a = { x0, OP1_Q10(100) }, b = { x0, top }, c = { x1, top }, d = { x1, OP1_Q10(100) };
        const uint8_t *col = i == cur ? green : grey;
        op1_raster_line(r, &a, &b, 0x600, col); op1_raster_line(r, &b, &c, 0x600, col); op1_raster_line(r, &c, &d, 0x600, col);
    }
}

#ifndef OP1_HOST
/* ======================= firmware: the player and the plugin interface (as seq/euclid.c, docs/44) =======================
 * Selecting the sequencer runs it as the finger type (3) with myseq_active[mode] = 1; PlayerManager asks player_get for
 * the player: ours while active, else the stock finger player. The player object is a vtable pointer; vtable entries are
 * {adj, fn} pairs: +0x14 key_down(key 0..23) -> 1 = consumed, +0x1c key_up, +0x24 reset, +0x2c tick (96 per beat),
 * +0x3c stop. Mode (synth 0 / drum 1) = 0x01DE4C40. */
#define MY_MODE()   (*(volatile int *)0x01DE4C40 == 1)
#define MY_SCREEN() (*(volatile int *)0x018B65C8)                  /* shown screen: 0x2D = the sequencer page */
#define SYNTH_OCT() (((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7])
#define DRUM_OCT()  (((const int *)0x01DFE500)[*(volatile int *)0xFF8016D8 & 7])
static int my_nop(void *self) { (void)self; return 0; }
static int my_key_down(void *self, int key)
{
    uint32_t caller = OP1_SEQ_CALLER();                              /* keyboard or USB MIDI: read it here (op1_synth.h) */
    (void)self;
    if (key < 0 || key > 23) return 0;
    int m = MY_MODE();
    if (MY_SCREEN() == 0x2D) myseq_key(m, op1_seq_key_note(m, key, m ? DRUM_OCT() : SYNTH_OCT(), caller), key);
    return 0;                                                        /* not consumed: the key also plays */
}
static void my_reset(void *self) { (void)self; myseq_all_off(MY_MODE()); }
static void my_ptick(void *self, int counter) { (void)self; (void)counter; myseq_tick(MY_MODE()); }
static void my_stop(void *self) { (void)self; myseq_all_off(0); myseq_all_off(1); }
static const uint32_t my_vt[16] = {
    0, 0,  0, (uint32_t)my_nop,  0, (uint32_t)my_key_down,  0, (uint32_t)my_nop,
    0, (uint32_t)my_reset,  0, (uint32_t)my_ptick,  0, (uint32_t)my_nop,  0, (uint32_t)my_stop };
static struct { const uint32_t *vt; uint32_t pad[7]; } my_player = { my_vt, { 0 } };
static volatile uint8_t myseq_active[2];                             /* selected for synth / drum: written by the core */

static void *my_player_get(void)
{
    if (!myseq_active[MY_MODE()]) return ((void *(*)(void))0x019ED460)();   /* the stock finger player */
    return &my_player;
}
static int my_pkey(int m, int code, int pressed, int knob, int shift)
{
    (void)m; (void)code; (void)pressed; (void)shift;
    return knob >= 0;                                                /* encoder presses: swallowed (no pages here) */
}
static void my_pturn(int m, int knob, int delta, int shift) { (void)shift; myseq_turn(m, knob, delta); }

static void my_scr_init(void *scr) { (void)scr; }
static void my_scr_update(void *scr, int dt) { op1_screen_draw(scr, dt); myseq_screen_draw(op1_raster(), MY_MODE()); }
const op1_screen_t SCREEN_myseq = { my_scr_init, my_scr_update };

const op1_seq_t SEQ_myseq = { myseq_active, my_player_get, my_pkey, my_pturn, myseq_midi_start, 0 };
#endif
