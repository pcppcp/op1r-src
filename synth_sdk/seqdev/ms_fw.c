/* MIDI sequencer device, firmware glue (docs/39): the device view on the tape screen (SEQ + TAPE), keys, recording
 * inputs (VoiceCtl entries, MIDI CCs), destinations, file I/O, drawing. The core (ms_core.c) is host-tested.
 *
 * Opt-in: clock + T2, green (screens/usbout.c ms_enabled, saved); off = SEQ + TAPE stock, no allocation, no work
 * (switching off closes the view, silences, saves pending slots, then idles).
 * Synced to the tape: positions are tape samples; PLAY / STOP stay the tape's. While the view is open: T1..T4 select
 * the track (Shift: mute), REC held = record the selected track (from stop: the first note starts the tape, then latched) (Shift + REC: latched, STOP ends it; from stop: armed,
 * the first note on the track's input starts the tape and the recording), REC held + metronome from stop: count-in
 * (beats as the tape count-in), then PLAY and latched recording; Shift + blue turn: quantize grid (off, 1/4 .. 1/32,
 * 1/8T, 1/16T; a take's notes snap when it ends), Shift + blue press: quantize the selection; scissors marks
 * the selection start / end, LIFT cuts it from the selected track (no selection: the 1.5 s page under the playhead), Shift + LIFT from all
 * tracks, then the playhead jumps to the first lifted note; DROP pastes with the first note at the playhead, white press switches between the track's input and output, white turn scrolls the
 * targets (I instrument, S sequencer, M1..M16 MIDI); red press / Shift + red / Shift + red turn: tape markers as on the tape screen. TAPE alone or another mode key leaves the view (the device keeps playing with the tape). */
#pragma GCC optimize ("Os", "no-tree-loop-distribute-patterns")
#include "../op1_synth.h"
#include "../input/debounce.h"
#include "../seq/seq_text.h"
#include "ms_core.h"
#include "ms_draw.h"

#define TAPE_STATE  (*(volatile int32_t *)0x018C17D4)
#define TAPE_POS    (*(volatile int32_t *)0xFF802974)            /* 1/64 samples */
#define LOOP_IN     (*(volatile int32_t *)0x01B4EF34)
#define LOOP_OUT    (*(volatile int32_t *)0x01B4EF38)
#define LOOP_ON     (*(volatile uint8_t *)0xFF8029C5)
#define SHIFT       (*(volatile uint8_t *)0x01DD7F78)
#define VC_DOWN(r, p) (((void (*)(int32_t *, int))0x01A85750)((r), (p)))   /* VoiceCtl key_down (through our hook) */
#define VC_UP(r)    (((void (*)(int32_t *))0x01A85840)(r))
#define VC_MIDI_MSG ((void (*)(void *))0x01A84F8C)                         /* VoiceCtl::midi_message (CC table) */
#define TX_ENQ      ((void (*)(int, const void *, unsigned))0x019E0D18)    /* usbmidi_tx_enqueue(_, packets, n) */
#define KEY_DISPATCH ((void (*)(int, int))0x019B6C74)
#define NEW_ARR     ((void *(*)(unsigned))0x01AA370C)
#define Y_OPEN      ((int (*)(const char *, int, int))0x01A997F4)
#define Y_CLOSE     ((int (*)(int))0x01A99908)
#define Y_READ      ((int (*)(int, void *, unsigned))0x01A99C90)
#define Y_PWRITE    ((int (*)(int, const void *, unsigned, int))0x01A99F9C)
#define Y_LSEEK     ((int (*)(int, int, int))0x01A9A0E4)
#define O_RDWR_CREAT 0x42
#define MS_FILE     "/yaffs2/user/op1r_seq.mseq"
#define MS_HEAD_SZ  4096

extern volatile uint8_t ms_enabled;                    /* screens/usbout.c: clock + T2, green (opt-in) */
extern volatile uint8_t ms_quant;                       /* screens/usbout.c: grid, saved */
void ms_quant_save(void);
static int32_t bpm_q8(void);
static ms_t *ms;                            /* heap (~60 KB), allocated by the main loop */
/* the view is its own screen object (iter screen class, empty SVG, drawing in code) swapped into the tape screen's
 * slot while open; made on first use with the tape screen's id (the current screen when the view opens) */
#define SCREEN_TABLE  ((void **)0x01DF4E54)
#define CUR_MODE_ID   (*(volatile int *)0xFF907D58)              /* ModeSwitcher: active mode, tape = 2 */
#define CUR_SCREEN    (*(volatile int *)0x018B65C8)
#define SCREEN_CTOR   ((void *(*)(void *, int, const char *))0x01A3BAB0)
#define ITER_SCREEN_VT ((const uint32_t *)0x019103FC)
#define SVG_LOAD      ((void *(*)(void *))0x01A503D8)
static uint32_t ms_scr[32], ms_vt[10];
static int ms_scr_id = -1;
static void *tape_obj;
static volatile uint8_t view, seq_open_req, rec_held, rec_latch, seq_down, seq_chord, sel_set, own_play;
static volatile uint8_t rec_arm, arm_go, ci_latch, take_done;     /* armed (first note starts), its PLAY wanted, count-in */
int ci_start_ext(void), ci_pending(void), ci_counting(void);   /* tape/countin.c */
void ci_cancel(void);
/* the view is on screen (tape mode, its screen current): keys / encoders / recording only then. The view flag itself
 * stays while another mode is shown (back in tape mode the recorder is still there). */
static int shown(void) { return view && CUR_MODE_ID == 2 && ms_scr_id >= 0 && CUR_SCREEN == ms_scr_id; }
static int32_t sel_a = -1, sel_b = -1;
static uint8_t io_sel;                                   /* white knob edits: 0 output, 1 input */
static int io_acc;
/* white turn: one step per detent through I, S, M1..M16 (wraps) for the selected track's input or output */
static int q_acc;
int ms_knob(int knob, int delta)
{
    if (knob == 0 && SHIFT && shown() && ms) {            /* Shift + blue: quantize grid off / 1/4 .. 1/32 / 1/8T / 1/16T */
        if ((q_acc > 0 && delta < 0) || (q_acc < 0 && delta > 0)) q_acc = 0;
        q_acc += delta;
        int st = q_acc / 1024;
        if (st) { q_acc = 0; ms_quant = (uint8_t)(((ms_quant + st) % MS_Q_N + MS_Q_N) % MS_Q_N); ms_quant_save(); }
        return 1;
    }
    if (!shown() || !ms || knob != 2) return 0;
    if ((io_acc > 0 && delta < 0) || (io_acc < 0 && delta > 0)) io_acc = 0;
    io_acc += delta;
    int st = io_acc / 1024;
    if (!st) return 1;
    io_acc = 0;
    int t = ms->sel & 3, k, ch;
    if (io_sel) { k = ms->h.in[t] & 15; ch = ms->h.in[t] >> 4; } else { k = ms->h.dest[t] % 3; ch = ms->h.ch[t] & 15; }
    int i = k == MS_DEST_MIDI ? 2 + ch : k == MS_DEST_SEQ ? 1 : 0;   /* list index */
    i = ((i + st) % 18 + 18) % 18;
    k = i >= 2 ? MS_DEST_MIDI : i == 1 ? MS_DEST_SEQ : MS_DEST_INSTR; ch = i >= 2 ? i - 2 : ch;
    if (io_sel) ms->h.in[t] = (uint8_t)(k | ch << 4); else { ms->h.dest[t] = (uint8_t)k; ms->h.ch[t] = (uint8_t)ch; }
    ms->head_dirty = 1;
    return 1;
}
static uint8_t swallow[0x40];

/* ---- env ---- */
static uint32_t irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m)); return m; }
static void irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m)); }
static int32_t e_pos(void) { return TAPE_POS >> 6; }
static int e_playing(void) { int st = TAPE_STATE; return st == 10 || st == 9 || st == 4 || st == 5; }
static int e_loop(int32_t *a, int32_t *b) { *a = LOOP_IN; *b = LOOP_OUT; return LOOP_ON; }
/* built-in sequencer: the note as a keyboard key (key i plays 53 + i at octave 0), through the KeyHandler queue like a
 * physical key, so the active stock sequencer gets it in order with the real keys. KEY_SYNTHETIC bypasses debounce
 * so short sequenced gates and repeated notes retain their timing. */
static const uint8_t kb_keys[24] = { 39, 13, 40, 14, 41, 15, 42, 28, 32, 29, 22, 30, 31, 23, 48, 24, 49, 25, 50, 51, 26, 52, 27, 53 };
static uint8_t kb_down[24];
static void kb_push(int code, int down)
{
    volatile uint32_t *rd = (volatile uint32_t *)0x01DD5244, *wr = (volatile uint32_t *)0x01DD5248;
    uint32_t m = irq_off(), w = *wr, n = (w + 1) & 0xFF;
    if (n != *rd && !*(volatile uint8_t *)0x01DD524D) ((volatile uint16_t *)0x01DD5044)[w] = (uint16_t)(code | KEY_SYNTHETIC | down << 8), *wr = n;
    irq_on(m);
    ((void (*)(void))0x019C2F04)();                               /* wake KeyHandler */
}
static void e_play(int t, int dest, int ch, int kind, int d1, int d2, int on)
{
    (void)t;
    if (dest == MS_DEST_MIDI) {                                   /* USB MIDI out, the track's channel */
        uint32_t st = kind == MS_CC ? 0xB0 : on ? 0x90 : 0x80, p = (st >> 4) | (st | (ch & 15)) << 8 | (uint32_t)d1 << 16 |
                      (uint32_t)(kind == MS_CC ? d2 : on ? d2 : 64) << 24;
        TX_ENQ(0, &p, 1);
        return;
    }
    if (kind == MS_CC) {                                          /* output I only: automation, else as a received CC */
        if (dest != MS_DEST_INSTR) return;                        /* (output S: the stock sequencers take no CCs) */
        extern int mx_cc(int cc, int v);
        if (mx_cc(d1, d2)) return;                                /* synth automation CC (midi/mx.c) */
        static uint8_t msg[3];
        static struct { uint32_t a; uint8_t *b, *e; uint32_t c; } vec;
        msg[0] = (uint8_t)(0xB0 | (ch & 15)); msg[1] = (uint8_t)d1; msg[2] = (uint8_t)d2;
        vec.b = msg; vec.e = msg + 3;
        VC_MIDI_MSG(&vec);
        return;
    }
    if (dest == MS_DEST_SEQ) {
        int i = d1 - 53;
        while (i < 0) i += 12;
        while (i > 23) i -= 12;
        if (!on == !kb_down[i]) return;                           /* never a second down of a held key (stock asserts) */
        kb_down[i] = (uint8_t)on;
        kb_push(kb_keys[i], on);
        return;
    }
    int32_t r[4] = { d1, on ? d2 : 0, 0, 0 };
    own_play = 1;
    if (on) VC_DOWN(r, 0); else VC_UP(r);
    own_play = 0;
}
static int open_rw(void) { return Y_OPEN(MS_FILE, O_RDWR_CREAT, 0x180); }
static int e_read(int off, void *p, int n)
{
    int fd = Y_OPEN(MS_FILE, 0, 0), r = -1;
    if (fd < 0) return -1;
    if (Y_LSEEK(fd, off, 0) == off && Y_READ(fd, p, (unsigned)n) == n) r = 0;
    Y_CLOSE(fd);
    return r;
}
static int e_write(int off, const void *p, int n)
{
    int fd = open_rw(), r = -1;
    if (fd < 0) return -1;
    if (Y_PWRITE(fd, p, (unsigned)n, off) == n) r = 0;
    Y_CLOSE(fd);
    return r;
}
static int e_rs(int sl, ms_slot *o) { return e_read(MS_HEAD_SZ + sl * (int)sizeof(ms_slot), o, (int)sizeof *o); }
static int e_ws(int sl, const ms_slot *i) { return e_write(MS_HEAD_SZ + sl * (int)sizeof(ms_slot), i, (int)sizeof *i); }
static int e_rh(void *h, int n) { return e_read(0, h, n); }
static int e_wh(const void *h, int n) { return e_write(0, h, n); }
static const ms_env env = { e_pos, e_playing, e_loop, e_play, e_rs, e_ws, e_rh, e_wh, irq_off, irq_on };

static void view_set(int on);

/* ---- recording inputs: queued from any thread, inserted by the audio thread ---- */
#define QN 64
static struct { uint8_t cc, d1, d2, ch; } q[QN];
static volatile uint32_t qw, qr;
static void q_put(int cc, int d1, int d2, int ch)
{
    uint32_t m = irq_off();
    if (qw - qr < QN) { q[qw % QN].cc = (uint8_t)cc; q[qw % QN].d1 = (uint8_t)d1; q[qw % QN].d2 = (uint8_t)d2; q[qw % QN].ch = (uint8_t)ch; qw++; }
    irq_on(m);
}
static int recording(void) { return ms && shown() && (rec_held || rec_latch || rec_arm); }
static int e_playing(void);
/* tape stopped and armed (Shift + REC) or REC held (not counting in): the first note starts the tape */
static int waiting(void) { return (rec_arm || (rec_held && !ci_latch)) && !e_playing(); }
/* input source of a VoiceCtl note, from its caller: the keyboard (global_controller key down / up with no sequencer
 * taking the key), received MIDI (VoiceCtl note on / off), else a player (the stock sequencers, arpeggio, ...) */
static int src_of(uint32_t ret)
{
    if ((ret >= 0x019AC0B0 && ret < 0x019AC2F2) || (ret >= 0x019AC2F8 && ret < 0x019AC3C6)) return MS_DEST_INSTR;
    if ((ret >= 0x01A858D8 && ret < 0x01A85C16)) return -1;            /* MIDI: taken from the packets (ms_midi_rx) */
    return MS_DEST_SEQ;
}
static int in_kind(void) { return ms->h.in[ms->sel & 3] & 15; }
void ms_vc_down(const int32_t *rec, int param, uint32_t ret)      /* VoiceCtl key_down entry (hook.S): every note played */
{
    (void)param;
    if (own_play || !recording() || rec[0] < 0 || rec[0] > 127 || src_of(ret) != in_kind()) return;
    q_put(0, rec[0], rec[1] < 1 ? 100 : rec[1] > 127 ? 127 : rec[1], 1);
    if (waiting()) { arm_go = 1; rec_arm = 1; }                      /* armed / REC held from stop: the note starts the tape,
                                                                         recording then latched (REC can be released) */
}
void ms_vc_up(const int32_t *rec, uint32_t ret)
{
    if (own_play || !recording() || rec[0] < 0 || rec[0] > 127 || src_of(ret) != in_kind()) return;
    q_put(0, rec[0], 0, 0);
}
/* a key as played, from a sequencer plugin that consumed it (chord mode, docs/50): input I records it as a keyboard
 * note, the plugin's own output (VoiceCtl notes from the plugin) counts as input S */
void op1_key_played(int midi, int vel, int on)
{
    if (own_play || !recording() || midi < 0 || midi > 127 || in_kind() != MS_DEST_INSTR) return;
    q_put(0, midi, on ? (vel < 1 ? 100 : vel > 127 ? 127 : vel) : 0, on);
    if (on && waiting()) { arm_go = 1; rec_arm = 1; }
}
/* received USB-MIDI packet (devupd/du_fw.c du_rx_poll, before the stock parser, every channel): input M<ch> takes
 * notes and CCs on its channel; inputs I / S take CCs on the OP-1's MIDI channel (what stock receives) */
void ms_midi_rx(const volatile uint8_t *pk)
{
    int cin = pk[0] & 15, ch = pk[1] & 15, d1 = pk[2] & 127, d2 = pk[3] & 127;
    if (!recording() || (cin != 8 && cin != 9 && cin != 11)) return;
    int in = ms->h.in[ms->sel & 3], mine = (in & 15) == MS_DEST_MIDI ? ch == in >> 4 : cin == 11 && ch == (*(volatile int *)0x01AADB20 & 15);
    if (!mine) return;
    if (cin == 11) q_put(1, d1, d2, ch);
    else q_put(0, d1, cin == 9 ? d2 : 0, cin == 9 && d2);           /* note on with velocity 0 = off */
    if (waiting() && cin == 9 && d2) { arm_go = 1; rec_arm = 1; }
}

/* ---- audio thread (tape/countin.c, once per block) ---- */
void ms_fw_block(void)
{
    if (!ms || !ms_enabled) return;
    ms->sel = ms->sel & 3;
    if (e_playing()) {                                    /* started by the count-in / the first note: record, latched */
        if (ci_latch) { ci_latch = 0; rec_latch = 1; }
        if (rec_arm) { rec_arm = 0; arm_go = 0; rec_latch = 1; }
    } else if (ci_latch && !ci_pending()) ci_latch = 0;   /* count-in cancelled */
    int was = ms->rec;
    ms->rec = recording();
    if (ms->rec && !was) ms_take_reset(ms);               /* a take starts */
    if (!ms->rec && was) take_done = 1;                   /* ...and ends: quantized in the main loop */
    ms_block(ms);
    if (waiting()) return;                                /* the notes wait for the tape (recorded at its start) */
    while (qr != qw) {
        int i = qr % QN;
        if (q[i].cc) ms_cc_in(ms, q[i].d1, q[i].d2, q[i].ch);
        else ms_note_in(ms, q[i].d1, q[i].d2, 0, q[i].ch);   /* (notes: ch 1 = note-on, 0 = note-off) */
        qr++;
    }
    if (!e_playing()) rec_latch = 0;
}

/* ---- main loop (tape/countin.c ci_service) ---- */
void ms_fw_service(void)
{
    static uint8_t dead, was_on;
    if (dead) return;
    if (!ms_enabled) {                                    /* off (or switched off): close, silence, save, then idle */
        if (!ms) return;
        if (was_on) {
            was_on = 0; rec_held = rec_latch = 0; seq_down = 0;
            if (view) view_set(0);
            uint32_t m = irq_off(); ms_all_off(ms); irq_on(m);
        }
        int dirty = ms->head_dirty;
        for (int b = 0; b < MS_RAM && !dirty; b++) dirty = ms->ram_slot[b] != MS_NONE && ms->ram_dirty[b];
        if (dirty) ms_service(ms, 4);
        return;
    }
    was_on = 1;
    if (!ms) {
        ms_t *s = (ms_t *)NEW_ARR(sizeof(ms_t));
        if (!s) { dead = 1; return; }
        ms_init(s, &env);
        ms_load(s);
        ms = s;
        return;
    }
    ms_service(ms, 4);
    if (take_done) {                                      /* recording ended: the take's notes to the grid */
        take_done = 0;
        int32_t step = ms_grid_step(ms_quant, bpm_q8());
        if (step) ms_quantize_take(ms, step); else ms_take_reset(ms);
    }
    if (arm_go) {                                         /* armed + first note: PLAY as the user would */
        arm_go = 0;
        if (waiting()) { KEY_DISPATCH(0x22, 1); KEY_DISPATCH(0x22, 0); }
    }
    if (seq_open_req && CUR_MODE_ID == 2) { seq_open_req = 0; view_set(1); }   /* SEQ + TAPE from another mode */
}

/* ---- keys (key thread, dispatcher.c key_event_hook): 1 = consumed ---- */
static void ms_scr_update(void *self, int dt);
static void ms_scr_init(void *self) { (void)self; }
static void view_set(int on)                          /* key thread */
{
    if (on && ms_scr_id < 0) {                         /* first open: the tape screen is the current one */
        int id = CUR_SCREEN;
        if (id < 0 || id >= 0x44 || !SCREEN_TABLE[id]) return;
        void *saved = SCREEN_TABLE[id];
        SCREEN_CTOR(ms_scr, id, "usbout.svg");         /* (empty SVG, shipped for the USB out page) */
        SCREEN_TABLE[id] = saved;
        for (int i = 0; i < 10; i++) ms_vt[i] = ITER_SCREEN_VT[i];   /* is_visible: id == ours */
        ms_vt[3 * 2 + 1] = (uint32_t)ms_scr_update;
        ms_vt[4 * 2 + 1] = (uint32_t)ms_scr_init;
        ms_scr[3] = (uint32_t)ms_vt;
        ms_scr[0] = (uint32_t)SVG_LOAD((void *)ms_scr[2]);
        if (!ms_scr[0]) return;
        tape_obj = saved; ms_scr_id = id;
    }
    view = (uint8_t)on;
    if (ms_scr_id >= 0) SCREEN_TABLE[ms_scr_id] = on ? (void *)ms_scr : tape_obj;
}
int ms_key(int code, int pressed, int on_tape)
{
    (void)on_tape;
    if (!ms_enabled || !ms) return 0;                     /* off: SEQ / TAPE stay stock */
    if (code < 0 || code >= 0x40) return 0;
    if (!pressed && swallow[code]) { swallow[code] = 0; if (code == 0x21) rec_held = 0; return 1; }
    if (code == 0x15) {                                   /* SEQ: held for the SEQ + TAPE chord */
        if (pressed) { seq_down = 1; seq_chord = 0; return 1; }
        seq_down = 0;
        if (!seq_chord) { KEY_DISPATCH(0x15, 1); KEY_DISPATCH(0x15, 0); }   /* plain SEQ: stock, late */
        return 1;
    }
    if (code == 0x04 && pressed) {                        /* TAPE: with SEQ held opens the view, alone closes it */
        if (seq_down && CUR_MODE_ID == 2) { seq_chord = 1; if (!view) view_set(1); swallow[code] = 1; return 1; }
        if (seq_down) { seq_chord = 1; seq_open_req = 1; return 0; }   /* not in tape mode yet: stock switches, then open */
        if (view) view_set(0);
        return 0;                                         /* (the tape mode itself is stock's) */
    }
    if (code == 0x01 && pressed && rec_held && shown() && ms) {   /* REC held + metronome: count-in, then play + record */
        if (ci_pending()) { ci_cancel(); ci_latch = 0; }
        else if (!e_playing() && ci_start_ext()) { ci_latch = 1; rec_arm = 0; }
        swallow[code] = 1; return 1;
    }
    if (pressed && view && (code == 0x02 || code == 0x03 || code == 0x05 || code == 0x00 || code == 0x01 || code == 0x2C)) view_set(0);
    if (!shown() || !ms || !pressed) return 0;
    int sh = SHIFT;
    if (code >= 0x06 && code <= 0x09) {
        int t = code - 0x06;
        if (sh) { ms->h.mute[t] ^= 1; ms->head_dirty = 1; if (ms->h.mute[t]) { uint32_t m = irq_off(); ms_all_off(ms); irq_on(m); } }
        else { ms->sel = t; rec_latch = 0; }
        swallow[code] = 1; return 1;
    }
    if (code == 0x21) {                                   /* REC held: record; Shift: latched (stopped: armed) */
        if (!sh) rec_held = 1;
        else if (e_playing()) rec_latch ^= 1;
        else { rec_arm ^= 1; arm_go = 0; }
        swallow[code] = 1; return 1;
    }
    int32_t ph = TAPE_POS >> 6;
    if (code == 0x0C) {                                   /* scissors: selection start, then end */
        if (!sel_set || sel_b >= 0) { sel_a = ph; sel_b = -1; sel_set = 1; }
        else { sel_b = ph; if (sel_b < sel_a) { int32_t x = sel_a; sel_a = sel_b; sel_b = x; } }
        swallow[code] = 1; return 1;
    }
    if (code == 0x0A) {                                   /* lift: the selected track, Shift: all tracks */
        int32_t a = sel_b >= 0 ? sel_a : ph & ~0xFFFF, b = sel_b >= 0 ? sel_b : (ph | 0xFFFF) + 1;
        if (!ms_cut(ms, a, b, sh ? 15 : 1 << (ms->sel & 3)) && ms->clip_first >= 0)
            ((void (*)(int32_t))0x01A55024)(ms->clip_first);   /* tape locate: DROP here puts them back in place */
        sel_set = 0; sel_a = sel_b = -1;
        swallow[code] = 1; return 1;
    }
    if (code == 0x0B) { ms_paste(ms, ph); swallow[code] = 1; return 1; }
    if (code == 0x3B && sh) {                             /* Shift + blue press: quantize the selection (selected track) */
        int32_t step = ms_grid_step(ms_quant, bpm_q8());
        if (sel_b >= 0 && step) ms_quantize(ms, ms->sel & 3, sel_a, sel_b, step);
        swallow[code] = 1; return 1;
    }
    if (code == 0x3D) { io_sel ^= 1; swallow[code] = 1; return 1; }
    return 0;
}
/* ---- drawing: the view screen's update (GUI thread, every frame): seqdev/ms_draw.c ---- */
extern int32_t tm_zoom_q10(void);
extern void tm_ready(void);
extern int tm_n;
extern int32_t tm_pos[];
extern volatile uint8_t pu_mode;                       /* tape/punch.c: punch mode on (markers red) */
#define TE_BPM_BITS (*(volatile uint32_t *)0x018C1878)                 /* tempo, float 40..220 (read as bits) */
static int32_t bpm_q8(void)                       /* float bits -> bpm in Q8, no soft-float; 120 if out of range */
{
    uint32_t b = TE_BPM_BITS;
    int e = (int)((b >> 23) & 0xFF) - 127;
    if ((b >> 31) || e < 5 || e > 7) return 120 << 8;                /* 32..255 */
    uint32_t m = (b & 0x7FFFFF) | 0x800000;                            /* 1.m in Q23 */
    return (int32_t)(m >> (15 - e));                                   /* Q23 * 2^e -> Q8 */
}
int ms_view_open(void) { return view; }
static void ms_scr_update(void *self, int dt)
{
    (void)self; (void)dt;
    if (!ms) return;
    tm_ready();
    ms_view v;
    v.ph = TAPE_POS >> 6; v.zoom_q10 = tm_zoom_q10(); v.bpm_q8 = bpm_q8();
    v.loop_on = LOOP_ON; v.loop_in = LOOP_IN; v.loop_out = LOOP_OUT;
    v.sel_a = sel_a; v.sel_b = sel_b; v.sel_track = ms->sel & 3; v.rec = recording(); v.punch = pu_mode; v.io = io_sel; v.shift = SHIFT; v.quant = ms_quant;
    v.tm_n = tm_n; v.tm = tm_pos;
    ms_draw(op1_raster(), &v, ms);
}
