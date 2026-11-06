/* Scale mode, firmware side (docs/55): called from the VoiceCtl key_down / key_up stubs (hook.S) before the MIDI
 * recorder and the stock body. The note record (int[4]: note, velocity, MIDI-send flag, ?) belongs to the caller
 * (a sequencer may keep it), so a changed note goes out in a copy. Mapped: synth mode, callers in the stock code
 * (keyboard 0x019AC0B0 / 0x019AC2F8, stock sequencers, arpeggio, tombola ...), except received MIDI (VoiceCtl note on
 * / off 0x01A858D8..0x01A85C16). Not mapped: our code (euclid, dots, chord, grids, plugins, MIDI sequencer), and
 * nothing while one of our sequencers runs (the stock finger code it is built on previews its keys). */
#include "../op1_synth.h"
#include "sc_core.h"

#define MODE_SYNTH (*(volatile int *)0x01DE4C40 == 0)
extern int seq_ours_live(void);
static int32_t sc_rec[8][4];
/* the key thread and the audio interrupt (stock sequencer ticks) both play notes: the held-note tables are updated
 * with interrupts off, else a count can be lost and a note never released */
static uint32_t irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m)); return m; }
static void irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m)); }
static uint8_t sc_ri;

static int stock_caller(uint32_t ret)
{
    if (ret >= 0x01A858D8 && ret < 0x01A85C16) return 0;   /* received MIDI */
    return (ret >= 0x01900000 && ret < 0x01E00000) || ret >= 0xFFA00000;
}

static int32_t *copy_with(const int32_t *rec, int note)
{
    int32_t *c = sc_rec[sc_ri++ & 7];
    c[0] = note; c[1] = rec[1]; c[2] = rec[2]; c[3] = rec[3];
    return c;
}

/* MIDI recorder destination S (seqdev/ms_fw.c): the recorded note is already mapped and goes back in as a keyboard
 * key, which is mapped again; push the key that maps to it instead (white keys mode; snap maps a scale note to itself) */
int sc_key_note(int note)
{
    return sc_type != SC_OFF && MODE_SYNTH && !seq_ours_live() ? sc_unmap(note) : note;
}

/* keyboard note velocity (screens/settings.c, clock > T3): the OP-1's keys send 127; a key played on the keyboard
 * path (physical_key_down, as the MIDI recorder's input I, seqdev/ms_fw.c) gets this one, synth and drum mode. MIDI
 * out (sent inside VoiceCtl) and the MIDI recorder (after this hook) see it too. */
extern uint8_t kb_vel;
static int kb_caller(uint32_t ret) { return ret >= 0x019AC0B0 && ret < 0x019AC2F2; }

/* returns the record to play, 0 = do not play */
int32_t *sc_vc_down(int32_t *rec, uint32_t ret)
{
    int n = rec[0];
    if (n < 0 || n > 127) return rec;                       /* the stock assert stays the stock's */
    int map = sc_type != SC_OFF && MODE_SYNTH && stock_caller(ret) && !seq_ours_live();
    uint32_t im = irq_off();
    int m = sc_down(n, map);
    irq_on(im);
    if (m < 0) return 0;
    int vel = kb_caller(ret) && kb_vel < 127 ? kb_vel : -1;
    if (m == n && vel < 0) return rec;
    int32_t *c = copy_with(rec, m);
    if (vel >= 0) c[1] = vel;
    return c;
}

/* returns the record to release; note -1 = nothing (VoiceCtl key_up ignores notes outside 0..127) */
int32_t *sc_vc_up(int32_t *rec, uint32_t ret)
{
    (void)ret;
    int n = rec[0];
    if (n < 0 || n > 127) return rec;
    uint32_t im = irq_off();
    int m = sc_up(n);
    irq_on(im);
    return m == n ? rec : copy_with(rec, m);
}
