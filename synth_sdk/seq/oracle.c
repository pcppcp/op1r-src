/* "oracle" sequencer: a geomantic Shield Chart as a 16-step, 4-lane chord / drum pattern (synth and drum mode).
 *
 * Lore: geomancy (Arabic 'ilm al-raml, "science of the sand"; Latin Europe from the 12th c.; Agrippa, Robert Fludd).
 * A figure has four rows Fire, Air, Water, Earth, each one dot (odd) or two dots (even) = 4 bits, 16 figures.
 * Four Mothers (cast at random) generate the chart (checked against digitalambler.com, "How to Construct the Shield
 * Chart of Geomancy"): Daughter k = row k of Mothers 1..4 read across (D1 = the Fire rows of M1 M2 M3 M4);
 * Nieces N1 = M1+M2, N2 = M3+M4, N3 = D1+D2, N4 = D3+D4; Right Witness = N1+N2, Left Witness = N3+N4;
 * Judge = RW+LW; Reconciler (Sentence) = Judge+M1. "+" adds the dots row by row and keeps the parity:
 * 1+1 = 2 (even), 1+2 = 3 (odd), 2+2 = 4 (even) -> odd rows add as XOR. The Judge is always "even" (an even number
 * of odd rows: it is the XOR of all 16 mother bits taken twice), so only 8 of the 16 figures can judge.
 * Figure numbering (= the encoder order): bit set = even row, Fire is the high bit:
 *   0 Via 1 Cauda Draconis 2 Puer 3 Fortuna Minor 4 Puella 5 Amissio 6 Carcer 7 Laetitia 8 Caput Draconis
 *   9 Conjunctio 10 Acquisitio 11 Rubeus 12 Fortuna Major 13 Albus 14 Tristitia 15 Populus
 * (e.g. Laetitia = 1,2,2,2 dots, Fortuna Major = 2,2,1,1, Puer = 1,1,2,1, Puella = 1,2,1,1).
 *
 * Pattern: step i (16 per bar) = chart figure i in the order M1 M2 M3 M4 D1 D2 D3 D4 N1 N2 N3 N4 RW LW Judge Reconciler.
 * Lane r (0 Fire .. 3 Earth) hits on step i when row r of figure i is odd (one dot). Lanes sound together (synth mode:
 * chords; drum mode: up to 4 drum sounds per step). The Judge's step is accented (velocity 120, else 100).
 *
 * Playing (like the stock pattern / endless sequencers; any screen while oracle is the mode's sequencer):
 *   note key        starts the pattern at once (step 1); it runs while a key is held. Synth mode: transposed to the
 *                   key - every lane is an interval above it. Drum mode: any key starts it (the lanes keep their
 *                   drum sounds). Another key while it runs re-transposes it, in phase. Releasing the last key stops
 *                   it at the next step (the notes of the last step end with their gate: nothing hangs).
 *   Shift + key     latches: it keeps running after the key is released. Shift + F G A B / G A B C C# D# are the
 *                   shortcuts below, so for those roots press the key first, then hold Shift and let the key go
 *                   (a key released while Shift is held always latches). Latched, a key with Shift re-transposes
 *                   and stays latched; a key without Shift takes over: it plays while held and stops on release
 *                   (the stock way to unlatch). Shift + C# / Shift + red left also stop it.
 * Lanes, synth mode: interval above the key per lane (-24..+36 semitones), Earth the lowest by default.
 *   voicings (Shift + red press cycles, Earth Water Air Fire): OPEN 0 7 12 16 (root fifth octave tenth, default) ·
 *   MAJOR 0 4 7 12 · MINOR 0 3 7 12 · SUS4 0 5 7 12 · MAJ7 0 4 7 11 · MIN7 0 3 7 10 · DOM7 0 4 7 10 ·
 *   QUARTAL 0 5 10 15 · POWER 0 7 12 19 · OCTAVES -12 0 12 24.
 * Lanes, drum mode: one drum sound (key) per lane; default Earth F, Water G, Air A, Fire B (keys 0 2 4 6).
 * Lane edit: Shift + white press toggles EDIT (oracle screen; "EDIT" shown, lanes show intervals). While EDIT is on, a
 *   note key on the oracle screen does not start the pattern: it sounds normally and sets the selected lane (Shift +
 *   white turn): synth = its interval above the current root (the last key played; latch a root, then play the
 *   interval you want against it), drum = that drum sound. On other screens keys still play the pattern.
 *
 * Controls (oracle screen shown; Shift = the shift key held):
 *   blue / green / white / red turn   figure of Mother 1 / 2 / 3 / 4 (16 figures, wraps)
 *   blue / green / white / red press  re-cast that Mother at random
 *   Shift + blue    rate 1/4 1/8 1/8T 1/16 1/16T 1/32 (per mode)
 *   Shift + green   gate 10..100 % of a step
 *   Shift + white   select lane Fire / Air / Water / Earth (wraps, as euclid's slot select); press: EDIT on / off
 *   Shift + red     right = start (latched, last root), left = stop; press: next voicing (synth mode)
 *   left / right    keyboard octave (as euclid)
 *   lift / drop     copy / paste the current pattern (Mothers + lanes); scissors: clear the selected lane (silent),
 *                   Shift + scissors: clear the whole pattern (default Mothers and lanes)
 * Any screen while oracle is the mode's sequencer (Shift + key, like euclid):
 *   Shift + F G A B (keys 0 2 4 6)    pattern 1..4 (each = 4 Mothers + 4 lanes + mutes); running keeps phase
 *   Shift + G A B C (keys 14 16 18 19) mute lane Fire / Air / Water / Earth, Shift + D# (22) mute / unmute all
 *   Shift + C# (key 20)               start (latched, last root) / stop; with REC held (tape armed) the start also
 *                                     presses PLAY
 * Saved (bg_saver, DB table oracleseq, one row per mode): patterns, current pattern, lane, rate, gate, root, voicing
 * (the root is written with the next other change: a key press alone does not write the DB). Comes back stopped.
 * Clock as euclid: 24 ticks per beat at the OP-1 tempo; a key or Shift + C# starts at once, Shift + red on the next
 * step boundary.
 *
 * Platform independent (host test harness/oracle_test.c); firmware glue at the end and in dispatcher.c.
 * Copyright (C) 2026 ultralazr. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "../op1_synth.h"
#include "seq_text.h"

#define ORA_LANES 4
#define ORA_STEPS 16
#define ORA_PATS  4
#define ORA_PPQN  24
#define ORA_IVL_MIN (-24)
#define ORA_IVL_MAX 36
enum { ORA_RATES = 6, ORA_VOICINGS = 10 };
static const uint8_t ora_rate_ticks[ORA_RATES] = { 24, 12, 8, 6, 4, 3 };   /* 1/4 1/8 1/8T 1/16 1/16T 1/32 */
static const char *const ora_names[16] = { "VIA", "CAUDA DRACONIS", "PUER", "FORTUNA MINOR", "PUELLA", "AMISSIO",
    "CARCER", "LAETITIA", "CAPUT DRACONIS", "CONJUNCTIO", "ACQUISITIO", "RUBEUS", "FORTUNA MAJOR", "ALBUS",
    "TRISTITIA", "POPULUS" };
/* voicings: intervals of lanes Fire, Air, Water, Earth (Earth = the root, Fire the top: the elements' order) */
static const char *const ora_vnames[ORA_VOICINGS] = { "OPEN", "MAJOR", "MINOR", "SUS4", "MAJ7", "MIN7", "DOM7",
    "QUARTAL", "POWER", "OCTAVES" };
static const int8_t ora_voicing[ORA_VOICINGS][ORA_LANES] = {
    { 16, 12, 7, 0 }, { 12, 7, 4, 0 }, { 12, 7, 3, 0 }, { 12, 7, 5, 0 }, { 11, 7, 4, 0 }, { 10, 7, 3, 0 },
    { 10, 7, 4, 0 }, { 15, 10, 5, 0 }, { 19, 12, 7, 0 }, { 24, 12, 0, -12 } };
static const uint8_t ora_drum_keys[ORA_LANES] = { 6, 4, 2, 0 };   /* Fire B, Air A, Water G, Earth F */
static const uint8_t ora_m0[4] = { 12, 2, 4, 9 };       /* Fortuna Major, Puer, Puella, Conjunctio */

typedef struct {
    uint8_t m[4];                                       /* Mothers: figure 0..15 */
    uint8_t used[ORA_LANES], note[ORA_LANES], key[ORA_LANES], muted[ORA_LANES];   /* note / key: drum mode */
    int8_t  ivl[ORA_LANES];                             /* synth mode: semitones above the root */
} ora_pat_t;

typedef struct {
    uint8_t  init, pat, sel, rate, gate, running;
    uint8_t  root, root_key, voicing;                   /* transposition: last key's note (synth), its key */
    uint8_t  latched, stop_pend, edit;                  /* runs without a key / stop at the next step / lane edit */
    uint32_t held;                                      /* keys down that play the pattern (bit = key 0..23) */
    int16_t  step;                                      /* playhead 0..15 (-1 = not started) */
    uint32_t start;                                     /* tick of step 0 */
    int32_t  abs;                                       /* last step counted from start (-1: none played yet) */
    uint8_t  sounding[ORA_LANES];                       /* note id + 1 per lane */
    int32_t  acc[4];
    uint64_t tick_q16;
    uint32_t rng;
    int16_t  show_ms;                                   /* screen: show the last touched Mother's / voicing name */
    uint8_t  touched;                                   /* 0..3 Mother, 4 voicing */
    ora_pat_t pats[ORA_PATS];                           /* pats[pat] is the live one */
} ora_bank_t;

static ora_bank_t ora[2];                               /* 0 synth, 1 drum */
static ora_pat_t  ora_clipboard[2];
static uint8_t    ora_clip_full[2];

#ifdef OP1_HOST
int host_ora_changed;
#define ORA_CHANGED() (host_ora_changed++)
void host_ora_note_on(int pos, int note, int vel);
void host_ora_note_off(int pos, int note);
#define ORA_NOTE_ON(pos, n, k, v) (host_ora_note_on((pos), (n), (v)), (n))
#define ORA_NOTE_OFF(pos, n)      host_ora_note_off((pos), (n))
static uint32_t ora_irq_off(void) { return 0; }
static void ora_irq_on(uint32_t m) { (void)m; }
#else
static void ora_changed(void);
#define ORA_CHANGED() ora_changed()
int  eu_fw_note_on(int pos, int note, int key, int vel);   /* seq/euclid.c: VoiceCtl note on / off */
void eu_fw_note_off(int midi);
#define ORA_NOTE_ON(pos, n, k, v) eu_fw_note_on((pos), (n), (k), (v))
#define ORA_NOTE_OFF(pos, n)      eu_fw_note_off(n)
static uint32_t ora_irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m) : : "memory"); return m; }
static void ora_irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m) : "memory"); }
#endif

static int ora_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---- geomancy ---- */
/* figure number -> odd-row mask (bit r = row r has one dot; r 0 Fire .. 3 Earth) and back */
static int ora_odd(int fig) { int o = 0; for (int r = 0; r < 4; r++) if (!((fig >> (3 - r)) & 1)) o |= 1 << r; return o; }
static int ora_fig(int odd) { int f = 0; for (int r = 0; r < 4; r++) if (!((odd >> r) & 1)) f |= 1 << (3 - r); return f; }

/* the 16 figures of the chart as odd masks, in step order M1..M4 D1..D4 N1..N4 RW LW J R */
static void ora_chart(const uint8_t m[4], uint8_t c[ORA_STEPS])
{
    for (int k = 0; k < 4; k++) c[k] = (uint8_t)ora_odd(m[k]);
    for (int k = 0; k < 4; k++) {                       /* Daughter k: row k of Mothers 1..4 -> rows Fire..Earth */
        int d = 0;
        for (int j = 0; j < 4; j++) d |= ((c[j] >> k) & 1) << j;
        c[4 + k] = (uint8_t)d;
    }
    for (int k = 0; k < 4; k++) c[8 + k] = c[2 * k] ^ c[2 * k + 1];   /* Nieces: M1+M2, M3+M4, D1+D2, D3+D4 */
    c[12] = c[8] ^ c[9]; c[13] = c[10] ^ c[11];         /* Right, Left Witness */
    c[14] = c[12] ^ c[13];                              /* Judge */
    c[15] = c[14] ^ c[0];                               /* Reconciler = Judge + First Mother */
}

static uint32_t ora_rand(ora_bank_t *b) { b->rng = b->rng * 1664525u + 1013904223u; return b->rng >> 16; }

/* default pattern: the start Mothers; lanes = OPEN voicing (synth) / keys F G A B (drum) */
static void ora_pat_default(ora_pat_t *q, int drum)
{
    for (int k = 0; k < 4; k++) {
        q->m[k] = ora_m0[k]; q->used[k] = 1; q->muted[k] = 0; q->ivl[k] = ora_voicing[0][k];
        q->key[k] = drum ? ora_drum_keys[k] : 0; q->note[k] = (uint8_t)(drum ? ora_drum_keys[k] + 0x35 : 0);
    }
}
static void ora_bank_init(ora_bank_t *b)
{
    if (b->init) return;
    b->init = 1; b->pat = 0; b->sel = 0; b->rate = 3; b->gate = 50; b->step = -1; b->abs = -1; b->rng = 0x5EED6E0u;
    b->root = 60; b->root_key = 7; b->voicing = 0;      /* C4 (key C with octave 0) */
    for (int p = 0; p < ORA_PATS; p++) ora_pat_default(&b->pats[p], b == &ora[1]);
}
static ora_pat_t *ora_cur(ora_bank_t *b) { return &b->pats[b->pat]; }
static void ora_pat_copy(ora_pat_t *d, const ora_pat_t *c)   /* byte loop: the firmware build has no memcpy */
{
    for (unsigned i = 0; i < sizeof *d; i++) ((uint8_t *)d)[i] = ((const uint8_t *)c)[i];
}

/* the key thread and the tick both release a lane's note: the id is taken with interrupts off, the note-off goes
 * out after (as scale/sc_fw.c), so a tick inside the key thread's note-off can't lose the note it starts */
static void ora_silence_lane(int pos, ora_bank_t *b, int r)
{
    (void)pos;                                          /* (firmware note-off needs no mode) */
    uint32_t m = ora_irq_off();
    int id = b->sounding[r];
    b->sounding[r] = 0;
    ora_irq_on(m);
    if (id) ORA_NOTE_OFF(pos, id - 1);
}
static void ora_silence(int pos, ora_bank_t *b)
{
    for (int r = 0; r < ORA_LANES; r++) ora_silence_lane(pos, b, r);
}

static void ora_start(ora_bank_t *b, int now_start)
{
    b->stop_pend = 0;
    if (b->running) return;
    uint32_t now = (uint32_t)(b->tick_q16 >> 16), r = ora_rate_ticks[b->rate];
    b->start = now_start ? now : (now / r + 1) * r;
    b->step = -1; b->abs = -1; b->running = 1;
}
static void ora_stop(int pos, ora_bank_t *b)
{
    b->latched = 0; b->stop_pend = 0;
    if (!b->running) return;
    b->running = 0; b->step = -1; ora_silence(pos & 1, b);
}

/* UI code (keys, encoders, persistence, screen) is compiled for size: code space is short. The clock (ora_advance
 * and what it calls, the player tick / reset / stop) stays -O2: it runs in the audio callback. */
#pragma GCC push_options
#pragma GCC optimize("Os")
/* ---- keys (UI thread) ---- */
/* note key (not a Shift shortcut): play the pattern from this key. shift: latch. Returns 1 (consumed). */
int ora_key_down(int pos, int note, int key, int shift)
{
    ora_bank_t *b = &ora[pos & 1];
    ora_bank_init(b);
    if (key < 0 || key > 23) return 0;
    b->held |= 1u << key;
    b->root = (uint8_t)ora_clampi(note, 0, 127); b->root_key = (uint8_t)key;   /* in phase: from the next step */
    b->latched = (uint8_t)(shift ? 1 : 0);              /* a key without Shift takes a latched pattern over */
    ora_start(b, 1);
    return 1;
}
/* key released (only keys this sequencer took); shift held now: latch. The last key up stops at the next step. */
void ora_key_up(int pos, int key, int shift)
{
    ora_bank_t *b = &ora[pos & 1];
    if (key < 0 || key > 23 || !(b->held & (1u << key))) return;
    b->held &= ~(1u << key);
    if (b->held) return;
    if (shift) b->latched = 1;
    if (b->running && !b->latched) b->stop_pend = 1;
}

/* EDIT on: note key on the oracle screen -> selected lane (synth: interval above the root; drum: that sound) */
int ora_assign(int pos, int note, int key)
{
    ora_bank_t *b = &ora[pos & 1];
    ora_bank_init(b);
    ora_pat_t *q = ora_cur(b);
    int r = b->sel;
    ora_silence_lane(pos & 1, b, r);
    q->used[r] = 1;
    if (pos & 1) { q->note[r] = (uint8_t)ora_clampi(note, 0, 127); q->key[r] = (uint8_t)ora_clampi(key, 0, 23); }
    else q->ivl[r] = (int8_t)ora_clampi(note - b->root, ORA_IVL_MIN, ORA_IVL_MAX);
    ORA_CHANGED();
    return r;
}
int ora_editing(int pos) { return ora[pos & 1].edit; }

/* ---- encoders ---- */
static int ora_detents(ora_bank_t *b, int k, int delta)   /* accumulate encoder ticks: 1024 per detent */
{
    if ((b->acc[k] > 0 && delta < 0) || (b->acc[k] < 0 && delta > 0)) b->acc[k] = 0;   /* direction change */
    b->acc[k] += delta;
    int st = b->acc[k] / 1024;
    b->acc[k] -= st * 1024;
    return st;
}

void ora_turn(int pos, int knob, int delta)             /* turn: Mother 1..4 figure (wraps) */
{
    ora_bank_t *b = &ora[pos & 1];
    ora_bank_init(b);
    if (knob < 0 || knob > 3) return;
    int st = ora_detents(b, knob, delta);
    if (!st) return;
    ora_pat_t *q = ora_cur(b);
    q->m[knob] = (uint8_t)((q->m[knob] + st) & 15);
    b->touched = (uint8_t)knob; b->show_ms = 1500;
    ORA_CHANGED();
}

/* press: re-cast Mother at random; Shift + white: EDIT on / off; Shift + red: next voicing (synth mode) */
void ora_press(int pos, int knob, int shift)
{
    ora_bank_t *b = &ora[pos & 1];
    ora_bank_init(b);
    if (knob < 0 || knob > 3) return;
    if (shift) {
        if (knob == 2) b->edit ^= 1;
        else if (knob == 3 && !(pos & 1)) {
            b->voicing = (uint8_t)(b->voicing + 1 >= ORA_VOICINGS ? 0 : b->voicing + 1);
            ora_pat_t *q = ora_cur(b);
            for (int l = 0; l < ORA_LANES; l++) { q->ivl[l] = ora_voicing[b->voicing][l]; q->used[l] = 1; }
            b->touched = 4; b->show_ms = 1500;
            ORA_CHANGED();
        }
        return;
    }
    b->rng ^= (uint32_t)b->tick_q16;                    /* the moment of the press seeds the cast */
    ora_pat_t *q = ora_cur(b);
    int f;
    do f = (int)(ora_rand(b) & 15); while (f == q->m[knob] && (ora_rand(b) & 3));   /* mostly a new figure */
    q->m[knob] = (uint8_t)f;
    b->touched = (uint8_t)knob; b->show_ms = 1500;
    ORA_CHANGED();
}

void ora_shift_turn(int pos, int knob, int delta)       /* Shift + turn: rate, gate, lane, run */
{
    ora_bank_t *b = &ora[pos & 1];
    ora_bank_init(b);
    if (knob < 0 || knob > 3) return;
    int st = ora_detents(b, knob, delta);
    if (!st) return;
    if (knob == 0) {                                    /* rate: keep the step grid aligned */
        int r = ora_clampi(b->rate + st, 0, ORA_RATES - 1);
        if (r == b->rate) return;
        b->rate = (uint8_t)r;
        if (b->running) { uint32_t now = (uint32_t)(b->tick_q16 >> 16), t = ora_rate_ticks[r];
                          b->start = (now / t + 1) * t; b->step = -1; b->abs = -1; }
    } else if (knob == 1) b->gate = (uint8_t)ora_clampi(b->gate + st * 5, 10, 100);
    else if (knob == 2) b->sel = (uint8_t)(((b->sel + st) % ORA_LANES + ORA_LANES) % ORA_LANES);
    else { if (st > 0) { ora_start(b, 0); b->latched = 1; } else ora_stop(pos, b); return; }
    ORA_CHANGED();
}

void ora_recall(int pos, int p)
{
    ora_bank_t *b = &ora[pos & 1];
    ora_bank_init(b);
    if (p < 0 || p >= ORA_PATS || p == b->pat) return;
    ora_silence(pos & 1, b);
    b->pat = (uint8_t)p; b->show_ms = 0;
    ORA_CHANGED();
}

int ora_toggle(int pos)                                 /* Shift + C#: returns 1 if it started (latched) */
{
    ora_bank_t *b = &ora[pos & 1];
    ora_bank_init(b);
    if (b->running) { ora_stop(pos, b); return 0; }
    ora_start(b, 1);
    b->latched = 1;
    return 1;
}

/* Shift + key: F G A B -> pattern 1..4, G A B C -> mute lane 1..4, D# -> mute all, C# -> start / stop.
 * Returns 0 if not handled (then the key latches the pattern), 2 if C# started, else 1 (as eu_shift_key). */
int ora_shift_key(int pos, int key)
{
    static const int8_t map[24] = { 10, -1, 11, -1, 12, -1, 13, -1, -1, -1, -1, -1, -1, -1,
                                    0, -1, 1, -1, 2, 3, 8, -1, 9, -1 };
    if (key < 0 || key > 23 || map[key] < 0) return 0;
    ora_bank_t *b = &ora[pos & 1];
    ora_bank_init(b);
    int a = map[key];
    if (a >= 10) { ora_recall(pos, a - 10); return 1; }
    if (a == 8) return ora_toggle(pos) ? 2 : 1;
    ora_pat_t *q = ora_cur(b);
    if (a == 9) {
        int all = 1;
        for (int r = 0; r < ORA_LANES; r++) if (!q->muted[r]) all = 0;
        for (int r = 0; r < ORA_LANES; r++) q->muted[r] = (uint8_t)!all;
        if (!all) ora_silence(pos & 1, b);
    } else {
        q->muted[a] ^= 1;
        if (q->muted[a]) ora_silence_lane(pos & 1, b, a);
    }
    ORA_CHANGED();
    return 1;
}

/* lift / drop / scissors: 0 copy pattern, 1 paste pattern, 2 clear the selected lane (all: whole pattern) */
void ora_clip(int pos, int op, int all)
{
    ora_bank_t *b = &ora[pos & 1];
    ora_bank_init(b);
    int m = pos & 1;
    ora_pat_t *q = ora_cur(b);
    if (op == 0) { ora_pat_copy(&ora_clipboard[m], q); ora_clip_full[m] = 1; return; }
    ora_silence(m, b);
    if (op == 1) { if (!ora_clip_full[m]) return; ora_pat_copy(q, &ora_clipboard[m]); }
    else if (!all) { q->used[b->sel] = 0; q->muted[b->sel] = 0; }
    else ora_pat_default(q, m);
    ORA_CHANGED();
}

#pragma GCC pop_options
/* ---- clock: advance by inc ticks (Q16, 24 per beat) ---- */
static void ora_advance(int pos, uint64_t inc)
{
    ora_bank_t *b = &ora[pos & 1];
    uint64_t t1 = b->tick_q16;
    b->tick_q16 = t1 + inc;
    if (!b->running) return;
    uint64_t r = (uint64_t)ora_rate_ticks[b->rate] << 16, st0 = (uint64_t)b->start << 16;
    if (t1 < st0) return;
    int32_t step = (int32_t)((t1 - st0) / r);
    uint64_t in = (t1 - st0) % r;
    if (in >= r * b->gate / 100) ora_silence(pos & 1, b);
    if (step == b->abs) return;
    if (b->stop_pend && b->abs >= 0) { ora_stop(pos, b); return; }   /* released: stop on this step boundary */
    b->abs = step;
    b->step = (int16_t)(step & (ORA_STEPS - 1));
    ora_silence(pos & 1, b);
    const ora_pat_t *q = ora_cur(b);
    uint8_t c[ORA_STEPS], nt[ORA_LANES];
    ora_chart(q->m, c);
    int odd = c[b->step], vel = b->step == 14 ? 120 : 100;   /* the Judge's step accented */
    for (int l = 0; l < ORA_LANES; l++) {
        int n = pos & 1 ? q->note[l] : b->root + q->ivl[l], k = pos & 1 ? q->key[l] : b->root_key;
        nt[l] = (uint8_t)ora_clampi(n, 0, 127);
        (void)k;                                        /* (drum: the sound; the host build ignores it) */
        if (!((odd >> l) & 1) || !q->used[l] || q->muted[l] || n < 0 || n > 127) continue;
        int dup = 0;                                    /* two lanes on one note: one note-on (one note-off) */
        for (int j = 0; j < l; j++) if (b->sounding[j] && nt[j] == nt[l] && (!(pos & 1) || q->key[j] == q->key[l])) dup = 1;
        if (dup) continue;
        int id = ORA_NOTE_ON(pos & 1, n, k, vel);
        if (id >= 0) b->sounding[l] = (uint8_t)(id + 1);
    }
}

void ora_tick(int pos, int n, uint32_t bpm_x100)        /* host: advance by n samples at bpm_x100 */
{
    ora_advance(pos, ((uint64_t)bpm_x100 * ORA_PPQN << 16) / (6000ull * 44100ull) * (uint64_t)n);
}
void ora_all_off(int pos) { ora_silence(pos & 1, &ora[pos & 1]); }
void ora_midi_start(void)                               /* MIDI START: restart at step 0 on the next tick */
{
    for (int pos = 0; pos < 2; pos++) {
        ora_bank_t *b = &ora[pos];
        b->tick_q16 = 0;
        if (!b->running) continue;
        ora_silence(pos, b);
        b->start = 0; b->step = -1; b->abs = -1;
    }
}

#pragma GCC push_options
#pragma GCC optimize("Os")
/* ---- persistence text: "pat,sel,rate,gate,root,voicing,(m0..m3,(used,note,key,muted,ivl+48) x 4) x 4" ---- */
#define ORA_HEAD    6
#define ORA_PFIELDS (4 + 5 * ORA_LANES)
#define ORA_TEXT_N  (ORA_HEAD + ORA_PATS * ORA_PFIELDS)
int ora_serialize(int pos, char *out, int n)
{
    ora_bank_t *b = &ora[pos & 1];
    ora_bank_init(b);
    int v[ORA_TEXT_N], c = 0;
    v[c++] = b->pat; v[c++] = b->sel; v[c++] = b->rate; v[c++] = b->gate; v[c++] = b->root; v[c++] = b->voicing;
    for (int p = 0; p < ORA_PATS; p++) {
        const ora_pat_t *q = &b->pats[p];
        for (int k = 0; k < 4; k++) v[c++] = q->m[k];
        for (int l = 0; l < ORA_LANES; l++) {
            v[c++] = q->used[l]; v[c++] = q->note[l]; v[c++] = q->key[l]; v[c++] = q->muted[l]; v[c++] = q->ivl[l] + 48;
        }
    }
    return sq_join(out, n, v, c);
}
int ora_parse(int pos, const char *s, int len)
{
    int v[ORA_TEXT_N];
    if (sq_split(s, len, v, ORA_TEXT_N) != ORA_TEXT_N) return 0;
    ora_bank_t *b = &ora[pos & 1];
    ora_bank_init(b);
    ora_silence(pos & 1, b);
    b->pat = (uint8_t)ora_clampi(v[0], 0, ORA_PATS - 1); b->sel = (uint8_t)ora_clampi(v[1], 0, ORA_LANES - 1);
    b->rate = (uint8_t)ora_clampi(v[2], 0, ORA_RATES - 1); b->gate = (uint8_t)ora_clampi(v[3], 10, 100);
    b->root = (uint8_t)ora_clampi(v[4], 0, 127); b->voicing = (uint8_t)ora_clampi(v[5], 0, ORA_VOICINGS - 1);
    b->root_key = (uint8_t)((b->root + 7) % 12);        /* (synth: the key does not matter, only the note) */
    const int *f = &v[ORA_HEAD];
    for (int p = 0; p < ORA_PATS; p++, f += ORA_PFIELDS) {
        ora_pat_t *q = &b->pats[p];
        for (int k = 0; k < 4; k++) q->m[k] = (uint8_t)(f[k] & 15);
        for (int l = 0; l < ORA_LANES; l++) {
            const int *g = &f[4 + l * 5];
            q->used[l] = (uint8_t)(g[0] & 1); q->note[l] = (uint8_t)ora_clampi(g[1], 0, 127);
            q->key[l] = (uint8_t)ora_clampi(g[2], 0, 23); q->muted[l] = (uint8_t)(g[3] & 1);
            q->ivl[l] = (int8_t)ora_clampi(g[4] - 48, ORA_IVL_MIN, ORA_IVL_MAX);
        }
    }
    b->running = 0; b->step = -1; b->latched = 0; b->stop_pend = 0; b->held = 0; b->edit = 0;
    return 1;
}

/* ======================= screen (GUI thread) =======================
 * The chart as in the manuscripts: top tier Daughters 4..1 | Mothers 4..1 (right to left), then Nieces, Witnesses,
 * Judge (centre bottom) and Reconciler (small cell right of it). Each figure is drawn as dots (one dot = odd row).
 * Mothers in their encoder colours, the derived figures grey, the Judge white; the playing figure is white and
 * underlined in green. Bottom left: the Judge's Latin name (or, for 1.5 s after a change, that Mother's / the voicing's
 * name) and the play state (HOLD / LATCH, or EDIT in green); bottom right: the lanes (F A W E = Fire Air Water Earth;
 * synth: the notes they play from the current root, in EDIT their intervals; drum: the drum key; selected lane white,
 * muted lanes dim), pattern, rate, gate. */
#include "oracle_geom.h"
#define ora_line sq_line
#define ora_dot sq_dot
/* figure with odd mask o, centre x cx, first row y (px); dx: half distance of a row's two dots; w: dot size Q10 */
static void ora_glyph(void *r, int o, int32_t cx, int32_t y0, int32_t dx, int32_t dy, int32_t w, const uint8_t *c)
{
    for (int k = 0; k < 4; k++) {
        int32_t y = y0 + k * dy;
        if ((o >> k) & 1) ora_dot(r, cx, y, w, c);
        else { ora_dot(r, cx - dx, y, w, c); ora_dot(r, cx + dx, y, w, c); }
    }
}
/* cell centre (px) of chart figure i */
static void ora_cell(int i, int32_t *x, int32_t *y, int32_t *wid)
{
    if (i < 8) { *x = 300 - i * 40; *y = 0; *wid = 40; }               /* M1 rightmost .. D4 leftmost */
    else if (i < 12) { *x = 280 - (i - 8) * 80; *y = ORA_TIER_H; *wid = 80; }
    else if (i < 14) { *x = i == 12 ? 240 : 80; *y = 2 * ORA_TIER_H; *wid = 160; }
    else if (i == 14) { *x = (ORA_JX0 + ORA_JX1) / 2; *y = 3 * ORA_TIER_H; *wid = ORA_JX1 - ORA_JX0; }
    else { *x = (ORA_JX1 + ORA_RX1) / 2; *y = 3 * ORA_TIER_H; *wid = ORA_RX1 - ORA_JX1; }
}
static void ora_note_name(char *p, int n)
{
    static const char nm[12][3] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const char *s = nm[n % 12];
    while (*s) *p++ = *s++;
    int o = n / 12 - 1;
    if (o < 0) { *p++ = '-'; o = -o; }
    *p++ = (char)('0' + o % 10); *p = 0;
}
static void ora_ivl_text(char *p, int v)                /* +7, -12, +0 */
{
    *p++ = v < 0 ? '-' : '+';
    if (v < 0) v = -v;
    if (v >= 10) *p++ = (char)('0' + v / 10);
    *p++ = (char)('0' + v % 10); *p = 0;
}

static void ora_screen_draw(void *r, int pos, int dt)
{
    ora_bank_t *b = &ora[pos & 1];
    ora_bank_init(b);
    const ora_pat_t *q = ora_cur(b);
    uint8_t c[ORA_STEPS];
    ora_chart(q->m, c);
    int cur = b->running && b->step >= 0 ? b->step : -1;
    for (int i = 0; i < ORA_STEPS; i++) {
        int32_t x, y, wd; ora_cell(i, &x, &y, &wd);
        int small = i == 15;
        const uint8_t *col = i < 4 ? ora_col[i] : i == 14 ? ora_white : ora_grey;
        if (i == cur) col = ora_white;
        int32_t dx = small ? 3 : 4, dy = small ? 5 : ORA_ROW_DY, y0 = y + (small ? ORA_ROW0 + 3 : ORA_ROW0);
        ora_glyph(r, c[i], OP1_Q10(x), OP1_Q10(y0), OP1_Q10(dx), OP1_Q10(dy), i == cur ? 0x1200 : (small ? 0xA00 : 0xD00), col);
        if (i == cur) {                                 /* playhead: green bar under the figure */
            int32_t yb = OP1_Q10(y0 + 3 * dy + 5), hw = OP1_Q10(small ? 8 : 12);
            ora_line(r, OP1_Q10(x) - hw, yb, OP1_Q10(x) + hw, yb, 0x400, ora_green);
        }
    }
    if (b->running) ora_dot(r, OP1_Q10(ORA_JX0 - 8), OP1_Q10(3 * ORA_TIER_H + 8), 0x1400, ora_col[3]);   /* playing */
    /* name: the Judge, or the Mother / voicing just changed */
    if (dt < 0) dt = 0; else if (dt > 200) dt = 200;   /* frame time, ms */
    if (b->show_ms > 0) b->show_ms = (int16_t)(b->show_ms > dt ? b->show_ms - dt : 0);
    char lab[4];
    const uint8_t *lc; const char *name, *label = "JUDGE";
    if (b->show_ms > 0 && b->touched == 4) {
        label = "VOICING"; lc = ora_white; name = ora_vnames[b->voicing % ORA_VOICINGS];
    } else if (b->show_ms > 0) {
        lab[0] = 'M'; lab[1] = (char)('1' + (b->touched & 3)); lab[2] = 0; label = lab;
        lc = ora_col[b->touched & 3]; name = ora_names[q->m[b->touched & 3] & 15];
    } else {
        lc = ora_white; name = ora_names[ora_fig(c[14])];
    }
    int32_t ty = OP1_Q10(3 * ORA_TIER_H + 6);
    sq_text(r, OP1_Q10(ORA_NAME_X), ty, OP1_Q10(1), label, 0x300, label == lab ? lc : ora_grey);
    /* play state, right of the label: EDIT (lane edit on), else LATCH / HOLD while running */
    const char *state = b->edit ? "EDIT" : !b->running ? 0 : b->latched ? "LATCH" : "HOLD";
    if (state) sq_text(r, OP1_Q10(ORA_STATE_X), ty, OP1_Q10(1), state, 0x300, b->edit ? ora_green : ora_grey);
    /* long names on two lines ("FORTUNA" / "MAJOR") */
    char l1[16], l2[16]; int a = 0, z = 0;
    const char *s = name;
    while (*s && *s != ' ' && a < 15) l1[a++] = *s++;
    l1[a] = 0;
    if (*s == ' ') s++;
    while (*s && z < 15) l2[z++] = *s++;
    l2[z] = 0;
    sq_text(r, OP1_Q10(ORA_NAME_X), ty + OP1_Q10(9), OP1_Q10(1.5), l1, 0x480, lc);
    if (z) sq_text(r, OP1_Q10(ORA_NAME_X), ty + OP1_Q10(21), OP1_Q10(1.5), l2, 0x480, lc);
    /* lanes: 2 x 2, then pattern / rate / gate */
    static const char el[4] = { 'F', 'A', 'W', 'E' };
    for (int l = 0; l < ORA_LANES; l++) {
        int32_t x = OP1_Q10(ORA_LANE_X + (l & 1) * 41), y = OP1_Q10(3 * ORA_TIER_H + 4 + (l >> 1) * 11);
        char t[8]; t[0] = el[l]; t[1] = ' ';
        int n = pos & 1 ? q->note[l] : b->root + q->ivl[l];
        if (!q->used[l]) { t[2] = '-'; t[3] = '-'; t[4] = 0; }
        else if (!(pos & 1) && b->edit) ora_ivl_text(t + 2, q->ivl[l]);
        else if (n >= 0 && n <= 127) ora_note_name(t + 2, n);
        else { t[2] = '?'; t[3] = 0; }
        const uint8_t *col = q->muted[l] ? ora_dim : l == b->sel ? (b->edit ? ora_green : ora_white) : ora_grey;
        sq_text(r, x, y, OP1_Q10(1.2), t, 0x380, col);
        if (l == b->sel) ora_line(r, x, y + OP1_Q10(9), x + OP1_Q10(36), y + OP1_Q10(9), 0x300, b->edit ? ora_green : ora_white);
    }
    static const char *const rates[ORA_RATES] = { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" };
    char t[24], *p = t; const char *u;
    *p++ = 'P'; *p++ = (char)('1' + b->pat); *p++ = ' ';
    for (u = rates[b->rate]; *u; ) *p++ = *u++;
    *p++ = ' ';
    if (b->gate >= 100) *p++ = '1';
    if (b->gate >= 10) *p++ = (char)('0' + b->gate / 10 % 10);
    *p++ = (char)('0' + b->gate % 10); *p++ = '%'; *p = 0;
    sq_text(r, OP1_Q10(ORA_LANE_X), OP1_Q10(3 * ORA_TIER_H + 27), OP1_Q10(1), t, 0x300, ora_grey);
}

static void ora_screen_update(void *scr, int dt, int pos)
{
    op1_screen_draw(scr, dt);
    ora_screen_draw(op1_raster(), pos, dt);
}

#ifndef OP1_HOST
/* ======================= firmware player (extra sequencer "oracle", type 3 + ora_active[mode], like euclid) ======
 * DB row seq_types "oracle"; dispatcher.c: SeqIds::from_name sets ora_active, create_player case 3 -> ora_player_get,
 * screen object in the finger slot 0x2D. Player vtable as euclid's (ok_player layout): key_down returns 1 for the
 * keys that play the pattern (consumed: the firmware does not sound them), so key_up (vt +0x1c, consumed keys only,
 * as dots' live record) tells us when they are released. */
#define ORA_MODE() (*(volatile int *)0x01DE4C40 == 1)
#define ORA_SHIFT() (*(volatile uint8_t *)0x01DD7F78)
#define ORA_SYNTH_OCT() (((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7])
#define ORA_DRUM_OCT()  (((const int *)0x01DFE500)[*(volatile int *)0xFF8016D8 & 7])
extern volatile uint8_t eu_rec_play;                    /* seq/euclid.c, handled in key_event_hook */
static int ora_nop(void *self) { (void)self; return 0; }
static int ora_pkey_down(void *self, int key)
{
    uint32_t caller = OP1_SEQ_CALLER();                                     /* keyboard or USB MIDI */
    (void)self;
    if (key < 0 || key > 23) return 0;
    int m = ORA_MODE(), sh = ORA_SHIFT();
    if (sh) {                                                           /* Shift + key shortcuts (any screen) */
        int h = ora_shift_key(m, key);
        int tape = *(volatile int *)0x018C17D4;                          /* 3 / 6 = REC held (armed) */
        if (h == 2 && (tape == 3 || tape == 6)) eu_rec_play = 1;         /* dispatcher presses PLAY afterwards */
        if (h) return 1;
    }
    int note = op1_seq_key_note(m, key, m ? ORA_DRUM_OCT() : ORA_SYNTH_OCT(), caller);
    if (ora_editing(m) && *(volatile int *)0x018B65C8 == 0x2D) {        /* EDIT on the oracle screen: set lane */
        ora_assign(m, note, key);
        return 0;                                       /* not consumed: the key sounds as usual */
    }
    return ora_key_down(m, note, key, sh);              /* plays the pattern (consumed) */
}
static void ora_pkey_up(void *self, int key)            /* consumed keys only; both modes (a mode switch while held) */
{
    (void)self;
    ora_key_up(0, key, ORA_SHIFT()); ora_key_up(1, key, ORA_SHIFT());
}
#pragma GCC pop_options
static void ora_reset(void *self) { (void)self; ora_all_off(ORA_MODE()); }
static void ora_ptick(void *self, int counter) { (void)self; (void)counter; ora_advance(ORA_MODE(), 1u << 14); }   /* 1/96 beat */
static void ora_pstop(void *self) { (void)self; ora_all_off(0); ora_all_off(1); }
#pragma GCC push_options
#pragma GCC optimize("Os")
static const uint32_t ora_vt[16] = {
    0, 0,  0, (uint32_t)ora_nop,  0, (uint32_t)ora_pkey_down,  0, (uint32_t)ora_pkey_up,
    0, (uint32_t)ora_reset,  0, (uint32_t)ora_ptick,  0, (uint32_t)ora_nop,  0, (uint32_t)ora_pstop };
static struct { const uint32_t *vt; uint32_t pad[7]; } ora_player = { ora_vt, { 0 } };
volatile uint8_t ora_active[2];                         /* oracle selected (synth, drum) */

/* persistence: bg_saver + DB as euclid, table oracleseq (is_synth, data); seq_text.h: sq_store_* */
static char ora_sql[560];                               /* text <= 102 fields * 3 chars + commas ~ 400 */
static sq_store_t ora_store = { .name = "oracleseq", .ser = ora_serialize, .parse = ora_parse,
                                .sql = ora_sql, .cap = sizeof ora_sql, .maxlen = 500 };
static void ora_changed(void) { sq_store_dirty(&ora_store); }
void *ora_player_get(void)
{
    sq_store_load(&ora_store);
    return &ora_player;
}

static void ora_scr_init(void *scr) { (void)scr; }
static void ora_scr_update(void *scr, int dt) { ora_screen_update(scr, dt, ORA_MODE()); }
const op1_screen_t SCREEN_oracle = { ora_scr_init, ora_scr_update };

/* plugin interface (docs/44): clipboard (Shift: whole pattern), presses re-cast (Shift: white EDIT, red voicing);
 * turns the Mothers, Shift + turn rate gate lane run */
static int ora_pkey(int m, int code, int pressed, int knob, int shift)
{
    if (code >= 0x0A && code <= 0x0C) { if (pressed) ora_clip(m, code == 0x0A ? 0 : code == 0x0B ? 1 : 2, shift); return 1; }
    if (knob >= 0) { if (pressed) ora_press(m, knob, shift); return 1; }
    return 0;
}
static void ora_pturn(int m, int knob, int delta, int shift)
{
    if (shift) ora_shift_turn(m, knob, delta); else ora_turn(m, knob, delta);
}
const op1_seq_t SEQ_oracle = { ora_active, ora_player_get, ora_pkey, ora_pturn, ora_midi_start, "shield" };
#endif
#pragma GCC pop_options
