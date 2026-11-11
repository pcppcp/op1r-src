/* "euclid" sequencer: 6 euclidean sequencer slots per mode (synth / drum), each playing one note.
 *
 * Slots: Shift + white encoder selects slot 1..6 (wraps). A note key pressed while the euclid screen is shown assigns that
 * note to the selected slot (settings kept, a playing note is released). A slot without a note can be edited but
 * not started. Started / stopped only by turning the red encoder (right = start, left = stop).
 *
 * Encoders, page 1:  blue steps 1..16 · green pulses 0..steps · white rotation 0..steps-1 · red run (start/stop)
 * Encoder press toggles that knob to page 2:
 *                    blue rate (1/4 1/8 1/8T 1/16 1/16T 1/32) · green gate 10..100 % · white octave -2..+2 ·
 *                    red level 1..127 (note velocity; the OP-1 engines scale the volume with it)
 *
 * Mute: Shift + the last six white keys G A B C D E (keys 14 16 18 19 21 23) toggles slot 1..6, Shift + D#
 * (key 22) mutes all (or unmutes all if all are muted). Works from any screen while the sequencer is on. A muted
 * slot keeps running (playhead moves) but plays no notes; its pulse dots are dimmed.
 * Shift + C# (key 20): stop all if any slot runs, else start all at once. With REC held (tape armed) the start also
 * presses PLAY, so the tape records from the first step.
 * Patterns: 4 per mode, each = the settings of all 6 slots (note, steps, pulses, rotation, rate, gate, octave, mute).
 * Shift + F G A B (keys 0 2 4 6) recalls pattern 1..4 (any screen). Running slots keep running and keep their phase
 * with the new settings. Running is global per slot: Shift + red / C# start all 6 slots, one without a note in the
 * current pattern runs silently. Edits go to the current pattern.
 * Saved (bg_saver, DB table euclidseq): all patterns + selected slot + current pattern per mode; slots come back stopped.
 *
 * Clock: one free-running clock per mode at the OP-1 tempo, 24 ticks per beat; all sequencers lock to it. A
 * started sequencer begins at step 0 on the next step boundary of its rate. Each hit is a note-on of the
 * sequencer's note (+ octave) and a note-off after `gate` percent of the step. Not saved (live only).
 *
 * Pattern: step i is a pulse if ((i - rot) * pulses) % steps < pulses (Bresenham form of the euclidean rhythm,
 * pulses spread as evenly as possible over the steps; rot turns it clockwise).
 *
 * This file is platform independent; the firmware glue (keys, tempo, note output, screen slot) provides
 * EU_NOTE_ON / EU_NOTE_OFF and calls eu_key / eu_turn / eu_press / eu_tick / eu_screen_update. */
#include "../op1_synth.h"

#define EU_MAX      6                                   /* = OP-1 voices */
#define EU_PATS     4                                   /* patterns per mode (Shift + F G A B) */
#define EU_PPQN     24
enum { EU_RATES = 6 };
static const uint8_t eu_rate_ticks[EU_RATES] = { 24, 12, 8, 6, 4, 3 };   /* 1/4 1/8 1/8T 1/16 1/16T 1/32 */

typedef struct {
    uint8_t  used, running, note, steps, pulses, rot, rate, gate, oct;   /* used: has a note; oct: 0..4 = -2..+2 */
    uint8_t  key;                                       /* key 0..23 pressed (drums: which drum sound) */
    uint8_t  muted;                                     /* runs but plays no notes */
    uint8_t  vel;                                       /* level = note velocity 1..127 */
    uint8_t  sounding;                                  /* note id currently on (+1), 0 = none */
    uint32_t start;                                     /* tick of step 0 */
    int32_t  step;                                      /* current step (-1 = not started yet) */
} eu_seq_t;

typedef struct {
    eu_seq_t s[EU_MAX];
    int      sel;                                       /* selected slot 0..5 */
    uint8_t  init;
    uint64_t tick_q16;                                  /* clock, ticks Q16 */
    uint8_t  page[4];                                   /* encoder page per knob (0/1) */
    int32_t  acc[4];                                    /* encoder accumulators (one value per 1024) */
    uint8_t  pat;                                       /* current pattern; its live copy is s[] */
    eu_seq_t pats[EU_PATS][EU_MAX];                     /* stored settings (pats[pat] is stale, s[] is current) */
} eu_bank_t;

static eu_bank_t eu[2];                                 /* 0 synth, 1 drum */
#ifdef OP1_HOST
int host_eu_changed;
#define EU_CHANGED() (host_eu_changed++)
#else
static void eu_changed(void);
#define EU_CHANGED() eu_changed()
#endif

#ifdef OP1_HOST
void host_eu_note_on(int pos, int note, int vel);
void host_eu_note_off(int pos, int note);
#define EU_NOTE_ON(pos, n, k, v)  (host_eu_note_on((pos), (n), (v)), (n))   /* -> note id for the note-off */
#define EU_NOTE_OFF(pos, n)    host_eu_note_off((pos), (n))
#else
/* firmware: notes go through VoiceCtl like the stock sequencers (ok_player). A slot stores the absolute pitch taken
 * when its key was pressed (key + 0x35 + octave * 12), so later octave changes do not move it.
 *  synth: record {midi, vel, send midi = 1, 0} -> key_down 0x01A85750 -> note_on_synth (uses midi as is).
 *  drums: note_on_drum takes the SOUND from record midi - 0x35 (key 0..23) and the PITCH from the param, relative
 *         to the pad's own pitch; key_down adds the current drum octave * 12 -> param = note - (key + 0x35) - current
 *         drum octave * 12 (seq/seq_core.c). */
#define SYNTH_OCT() (((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7])
#define DRUM_OCT()  (((const int *)0x01DFE500)[*(volatile int *)0xFF8016D8 & 7])
int  eu_fw_note_on(int pos, int note, int key, int vel);    /* seq/seq_core.c (core, shared with the other sequencers) */
void eu_fw_note_off(int midi);
#define EU_NOTE_ON(pos, n, k, v)  eu_fw_note_on((pos), (n), (k), (v))
#define EU_NOTE_OFF(pos, n)    eu_fw_note_off(n)
#endif

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* the key thread and the tick both release a slot's note: the id is taken with interrupts off, the note-off goes
 * out after (as scale/sc_fw.c), so a tick inside the key thread's note-off can't lose the note it starts */
#ifdef OP1_HOST
static uint32_t eu_irq_off(void) { return 0; }
static void eu_irq_on(uint32_t m) { (void)m; }
#else
static uint32_t eu_irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m) : : "memory"); return m; }
static void eu_irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m) : "memory"); }
#endif

static inline int eu_hit(const eu_seq_t *q, int i)
{
    if (!q->pulses) return 0;
    int j = (i + q->steps - q->rot) % q->steps;           /* rot > 0 turns the pattern clockwise */
    return ((j * q->pulses) % q->steps) < q->pulses;
}

static void eu_silence(int pos, eu_seq_t *q)
{
    uint32_t m = eu_irq_off();
    int id = q->sounding;
    q->sounding = 0;
    eu_irq_on(m);
    if (id) EU_NOTE_OFF(pos, id - 1);
}

/* ---- keys / encoders (UI thread) ---- */
static void eu_bank_init(eu_bank_t *b)
{
    if (b->init) return;
    b->init = 1; b->sel = 0; b->pat = 0;
    for (int k = 0; k < EU_MAX; k++) {
        b->s[k] = (eu_seq_t){ .steps = 16, .pulses = 4, .rot = 0, .rate = 3, .gate = 50, .oct = 2, .vel = 100, .step = -1 };
        for (int p = 0; p < EU_PATS; p++) b->pats[p][k] = b->s[k];
    }
}

/* settings only; running / start / step / sounding stay with the live slot */
static void eu_cfg_copy(eu_seq_t *d, const eu_seq_t *c)
{
    d->used = c->used; d->note = c->note; d->key = c->key; d->steps = c->steps; d->pulses = c->pulses;
    d->rot = c->rot; d->rate = c->rate; d->gate = c->gate; d->oct = c->oct; d->muted = c->muted; d->vel = c->vel;
}
static const eu_seq_t *eu_cfg(const eu_bank_t *b, int p, int k) { return p == b->pat ? &b->s[k] : &b->pats[p][k]; }

/* Shift + F G A B: switch to pattern p. Running is per slot, not per pattern: slots keep running (and their phase);
 * a running slot without a note in this pattern just plays nothing */
void eu_recall(int pos, int p)
{
    eu_bank_t *b = &eu[pos & 1];
    eu_bank_init(b);
    if (p < 0 || p >= EU_PATS || p == b->pat) return;
    for (int k = 0; k < EU_MAX; k++) {
        eu_seq_t *q = &b->s[k];
        eu_silence(pos & 1, q);
        eu_cfg_copy(&b->pats[b->pat][k], q);
        eu_cfg_copy(q, &b->pats[p][k]);
    }
    b->pat = (uint8_t)p;
    EU_CHANGED();
}

/* note key on the euclid screen: the selected slot plays that note */
int eu_key_ex(int pos, int note, int key)
{
    eu_bank_t *b = &eu[pos & 1];
    eu_bank_init(b);
    eu_seq_t *q = &b->s[b->sel];
    if (q->used && q->note == note && q->key == key) return b->sel;
    eu_silence(pos & 1, q);
    q->used = 1; q->note = (uint8_t)note; q->key = (uint8_t)key;
    EU_CHANGED();
    return b->sel;
}
int eu_key(int pos, int note) { return eu_key_ex(pos, note, note); }

/* ---- mute: slot keeps running, plays no notes ---- */
void eu_mute(int pos, int k)
{
    eu_bank_t *b = &eu[pos & 1];
    eu_bank_init(b);
    if (k < 0 || k >= EU_MAX) return;
    eu_seq_t *q = &b->s[k];
    q->muted ^= 1;
    if (q->muted) eu_silence(pos & 1, q);
    EU_CHANGED();
}
void eu_mute_all(int pos)                               /* all muted -> unmute all, else mute all */
{
    eu_bank_t *b = &eu[pos & 1];
    eu_bank_init(b);
    int all = 1;
    for (int k = 0; k < EU_MAX; k++) if (!b->s[k].muted) all = 0;
    for (int k = 0; k < EU_MAX; k++) { b->s[k].muted = (uint8_t)!all; if (!all) eu_silence(pos & 1, &b->s[k]); }
    EU_CHANGED();
}
int eu_toggle_all(int pos);
/* Shift + key: F G A B (0 2 4 6) -> pattern 1..4, G A B C D E (14 16 18 19 21 23) -> mute slot 1..6,
 * D# (22) -> mute all, C# (20) -> start / stop all. Returns 0 if not handled, 2 if C# started the slots, else 1. */
int eu_shift_key(int pos, int key)
{
    static const int8_t slot_of[24] = { 10, -1, 11, -1, 12, -1, 13, -1, -1, -1, -1, -1, -1, -1,
                                        0, -1, 1, -1, 2, 3, 8, 4, 9, 5 };
    if (key < 0 || key > 23 || slot_of[key] < 0) return 0;
    if (slot_of[key] >= 10) { eu_recall(pos, slot_of[key] - 10); return 1; }
    if (slot_of[key] == 8) return eu_toggle_all(pos) ? 2 : 1;
    if (slot_of[key] == 9) eu_mute_all(pos); else eu_mute(pos, slot_of[key]);
    return 1;
}

/* ---- persistence text: "sel,pat,((used,note,key,steps,pulses,rot,rate,gate,oct,muted,vel) x 6) x 4"
 * older texts still load: "sel,pat,((10 fields, no vel) x 6) x 4" and "sel,(10 fields) x 6" (one pattern) ---- */
#define EU_FIELDS 11
#define EU_TEXT_MAX (2 + EU_PATS * EU_MAX * EU_FIELDS)
int eu_serialize(int pos, char *out, int n)
{
    eu_bank_t *b = &eu[pos & 1];
    eu_bank_init(b);
    int v[EU_TEXT_MAX], c = 0, o = 0;
    v[c++] = b->sel; v[c++] = b->pat;
    for (int k = 0; k < EU_PATS * EU_MAX; k++) {
        const eu_seq_t *q = eu_cfg(b, k / EU_MAX, k % EU_MAX);
        int f[EU_FIELDS] = { q->used, q->note, q->key, q->steps, q->pulses, q->rot, q->rate, q->gate, q->oct, q->muted, q->vel };
        for (int i = 0; i < EU_FIELDS; i++) v[c++] = f[i];
    }
    for (int i = 0; i < c; i++) {
        char d[4]; int m = 0, x = v[i];
        do { d[m++] = (char)('0' + x % 10); x /= 10; } while (x && m < 4);
        if (o + m + 2 >= n) break;
        if (i) out[o++] = ',';
        while (m) out[o++] = d[--m];
    }
    out[o] = 0;
    return o;
}
int eu_parse(int pos, const char *s, int len)
{
    int v[EU_TEXT_MAX], c = 0, x = -1;
    for (int i = 0; i <= len && c < (int)(sizeof v / sizeof v[0]); i++) {
        char ch = i < len ? s[i] : ',';
        if (ch >= '0' && ch <= '9') { x = (x < 0 ? 0 : x * 10) + (ch - '0'); if (x > 9999) return 0; }   /* as sq_split */
        else if (ch == ',') { if (x < 0) return 0; v[c++] = x; x = -1; }
        else return 0;
    }
    int old = c == 1 + EU_MAX * 10, nf = c == 2 + EU_PATS * EU_MAX * 10 || old ? 10 : EU_FIELDS;
    if (!old && nf == EU_FIELDS && c != EU_TEXT_MAX) return 0;
    eu_bank_t *b = &eu[pos & 1];
    eu_bank_init(b);
    b->sel = clampi(v[0], 0, EU_MAX - 1);
    b->pat = (uint8_t)(old ? 0 : clampi(v[1], 0, EU_PATS - 1));
    for (int k = 0; k < (old ? 1 : EU_PATS) * EU_MAX; k++) {
        const int *f = &v[(old ? 1 : 2) + k * nf];
        eu_seq_t *q = &b->pats[k / EU_MAX][k % EU_MAX];
        q->used = (uint8_t)(f[0] & 1); q->note = (uint8_t)clampi(f[1], 0, 127); q->key = (uint8_t)clampi(f[2], 0, 23);
        q->steps = (uint8_t)clampi(f[3], 1, 16); q->pulses = (uint8_t)clampi(f[4], 0, q->steps);
        q->rot = (uint8_t)(f[5] % q->steps); q->rate = (uint8_t)clampi(f[6], 0, EU_RATES - 1);
        q->gate = (uint8_t)clampi(f[7], 10, 100); q->oct = (uint8_t)clampi(f[8], 0, 4); q->muted = (uint8_t)(f[9] & 1);
        q->vel = (uint8_t)(nf > 10 ? clampi(f[10], 1, 127) : 100);
    }
    for (int k = 0; k < EU_MAX; k++) {
        eu_seq_t *q = &b->s[k];
        eu_cfg_copy(q, &b->pats[b->pat][k]);
        q->running = 0; q->step = -1; q->sounding = 0;
    }
    return 1;
}

/* start (now = 1: on the next tick, else at the next step boundary) / stop every slot. Slots without a note run
 * silently, so they play when a pattern with a note there is recalled (running state is global) */
static void eu_set_all(int pos, int on, int now_start)
{
    eu_bank_t *b = &eu[pos & 1];
    uint32_t now = (uint32_t)(b->tick_q16 >> 16);
    for (int k = 0; k < EU_MAX; k++) {
        eu_seq_t *q = &b->s[k];
        if (on && !q->running) {
            uint32_t r = eu_rate_ticks[q->rate];
            q->start = now_start ? now : (now / r + 1) * r; q->step = -1; q->running = 1;
        } else if (!on && q->running) { q->running = 0; eu_silence(pos & 1, q); q->step = -1; }
    }
}

/* Shift + red encoder: start (right) / stop (left) every slot that has a note */
void eu_run_all(int pos, int delta)
{
    eu_bank_t *b = &eu[pos & 1];
    eu_bank_init(b);
    if ((b->acc[3] > 0 && delta < 0) || (b->acc[3] < 0 && delta > 0)) b->acc[3] = 0;
    b->acc[3] += delta;
    int st = b->acc[3] / 1024;
    if (!st) return;
    b->acc[3] -= st * 1024;
    eu_set_all(pos, st > 0, 0);
}

/* Shift + C#: any slot running -> stop all, else start all at once. Returns 1 if it started. */
int eu_toggle_all(int pos)
{
    eu_bank_t *b = &eu[pos & 1];
    eu_bank_init(b);
    int any = 0;
    for (int k = 0; k < EU_MAX; k++) if (b->s[k].running) any = 1;
    eu_set_all(pos, !any, 1);
    return !any;
}

/* Shift + white encoder: select slot, wraps */
void eu_select(int pos, int delta)
{
    eu_bank_t *b = &eu[pos & 1];
    eu_bank_init(b);
    if ((b->acc[2] > 0 && delta < 0) || (b->acc[2] < 0 && delta > 0)) b->acc[2] = 0;   /* direction change */
    b->acc[2] += delta;
    int st = b->acc[2] / 1024;
    if (!st) return;
    b->acc[2] -= st * 1024;
    b->sel = ((b->sel + st) % EU_MAX + EU_MAX) % EU_MAX;          /* wraps 6 -> 1 */
    EU_CHANGED();
}

/* lift / drop / scissors on the euclid page: copy / paste / cut the selected slot's settings (note, steps, pulses,
 * rotation, rate, gate, octave, mute, level); with Shift (all = 1) all 6 slots = the whole current pattern.
 * Slot and pattern clipboards are separate, one of each per mode. Running state stays with the slots. */
enum { EU_COPY, EU_PASTE, EU_CUT };
static eu_seq_t eu_clipboard[2][EU_MAX];               /* pattern clipboard per mode */
static eu_seq_t eu_clip_slot[2];
static uint8_t eu_clip_full[2][2];                      /* [mode][all] */
void eu_clip(int pos, int op, int all)
{
    eu_bank_t *b = &eu[pos & 1];
    eu_bank_init(b);
    int m = pos & 1, k0 = all ? 0 : b->sel, n = all ? EU_MAX : 1;
    eu_seq_t *clip = all ? eu_clipboard[m] : &eu_clip_slot[m];
    if (op != EU_PASTE) { for (int i = 0; i < n; i++) eu_cfg_copy(&clip[i], &b->s[k0 + i]); eu_clip_full[m][all != 0] = 1; }
    if (op == EU_COPY || (op == EU_PASTE && !eu_clip_full[m][all != 0])) return;
    const eu_seq_t d = { .steps = 16, .pulses = 4, .rate = 3, .gate = 50, .oct = 2, .vel = 100 };
    for (int i = 0; i < n; i++) {
        eu_seq_t *q = &b->s[k0 + i];
        eu_silence(m, q);
        eu_cfg_copy(q, op == EU_PASTE ? &clip[i] : &d);
        q->rot = (uint8_t)(q->rot % q->steps);
    }
    EU_CHANGED();
}

void eu_press(int pos, int knob)
{
    if (knob >= 0 && knob < 4) eu[pos & 1].page[knob] ^= 1;
}


void eu_turn(int pos, int knob, int delta)
{
    eu_bank_t *b = &eu[pos & 1];
    eu_bank_init(b);
    if (knob < 0 || knob > 3) return;
    eu_seq_t *q = &b->s[b->sel];
    if ((b->acc[knob] > 0 && delta < 0) || (b->acc[knob] < 0 && delta > 0)) b->acc[knob] = 0;   /* direction change */
    b->acc[knob] += delta;
    int st = b->acc[knob] / 1024;
    if (!st) return;
    b->acc[knob] -= st * 1024;
    if (knob == 3 && b->page[3]) {                      /* red page 2: level, 4 per detent */
        q->vel = (uint8_t)clampi(q->vel + st * 4, 1, 127);
        EU_CHANGED();
        return;
    }
    if (knob == 3) {                                    /* red: start / stop */
        if (st > 0 && !q->running && q->used) {                     /* (run state is not saved) */
            uint32_t now = (uint32_t)(b->tick_q16 >> 16), r = eu_rate_ticks[q->rate];
            q->start = (now / r + 1) * r;               /* next step boundary */
            q->step = -1; q->running = 1;
        } else if (st < 0 && q->running) { q->running = 0; eu_silence(pos & 1, q); q->step = -1; }
        return;
    }
    if (!b->page[knob]) {
        if (knob == 0) { q->steps = (uint8_t)clampi(q->steps + st, 1, 16);
                         q->pulses = (uint8_t)clampi(q->pulses, 0, q->steps); q->rot = (uint8_t)(q->rot % q->steps); }
        else if (knob == 1) q->pulses = (uint8_t)clampi(q->pulses + st, 0, q->steps);
        else q->rot = (uint8_t)((q->rot + st % q->steps + q->steps) % q->steps);
    } else {
        if (knob == 0) {                                /* rate: keep the step grid aligned */
            q->rate = (uint8_t)clampi(q->rate + st, 0, EU_RATES - 1);
            if (q->running) { uint32_t now = (uint32_t)(b->tick_q16 >> 16), r = eu_rate_ticks[q->rate];
                              q->start = (now / r + 1) * r; q->step = -1; }
        }
        else if (knob == 1) q->gate = (uint8_t)clampi(q->gate + st * 5, 10, 100);
        else q->oct = (uint8_t)clampi(q->oct + st, 0, 4);
    }
    EU_CHANGED();
}

/* ---- clock: advance by inc ticks (Q16, 24 per beat) ---- */
static void eu_advance(int pos, uint64_t inc)
{
    eu_bank_t *b = &eu[pos & 1];
    uint64_t t1 = b->tick_q16;                          /* this tick's own position (see dots.c dt_advance) */
    b->tick_q16 = t1 + inc;
    for (int k = 0; k < EU_MAX; k++) {
        eu_seq_t *q = &b->s[k];
        if (!q->running) continue;                      /* no note (used = 0): runs silently */
        uint64_t r = (uint64_t)eu_rate_ticks[q->rate] << 16, st0 = (uint64_t)q->start << 16;
        if (t1 < st0) continue;
        int32_t step = (int32_t)((t1 - st0) / r);
        uint64_t in = (t1 - st0) % r;                   /* position inside the step, Q16 ticks */
        if (q->sounding && in >= r * q->gate / 100) eu_silence(pos & 1, q);
        if (step != q->step) {
            q->step = step;
            eu_silence(pos & 1, q);
            int i = step % q->steps;
            if (q->used && eu_hit(q, i) && !q->muted) {
                int key = q->note + (pos & 1 ? 0 : (q->oct - 2) * 12);   /* stored midi note; drums: no slot octave */
                int id = EU_NOTE_ON(pos & 1, key, q->key, q->vel);
                if (id >= 0) q->sounding = (uint8_t)(id + 1);
            }
        }
    }
}

/* host: advance by n samples at bpm_x100 (tempo * 100) */
void eu_tick(int pos, int n, uint32_t bpm_x100)
{
    eu_advance(pos, ((uint64_t)bpm_x100 * EU_PPQN << 16) / (6000ull * 44100ull) * (uint64_t)n);
}

void eu_all_off(int pos)
{
    for (int k = 0; k < EU_MAX; k++) eu_silence(pos & 1, &eu[pos & 1].s[k]);
}

/* MIDI START (dispatcher.c midi_start_hook): running slots restart at step 0 on the next clock tick, so they line up
 * with the master's downbeat (the first F8 after FA) */
void eu_midi_start(void)
{
    for (int pos = 0; pos < 2; pos++) {
        eu_bank_t *b = &eu[pos];
        b->tick_q16 = 0;
        for (int k = 0; k < EU_MAX; k++) {
            eu_seq_t *q = &b->s[k];
            if (!q->running) continue;
            eu_silence(pos, q);
            q->start = 0; q->step = -1;
        }
    }
}

/* ======================= screen (GUI thread) =======================
 * Mini row (top): one small ring per sequencer slot with its pulses (slots without a note: outline only);
 * dim if stopped; the selected slot underlined.
 * Big ring (left): the selected sequencer, steps as dots (pulses in green, rests grey), the current step white
 * and larger. Right: note name, then the four encoder rows (dot in encoder colour + value; page-2 rows show the
 * page-2 value). Static parts are in display/euclid.svg; everything else is a few raster lines. */
#include "euclid_geom.h"

static void eline(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, p = { x1, y1 };
    op1_raster_line(r, &a, &p, w, c);
}
/* dot: the hardware rasteriser drops zero-length lines and draws flat (butt) ends, so a short wide line is a
 * thin bar. Two crossing segments, each as long as the dot is wide (width 0.7 w) -> octagon-like blob. */
static void edot(void *r, int32_t x, int32_t y, int32_t w, const uint8_t *c)
{
    int32_t h = w / 2, t = w * 7 / 10;
    eline(r, x - h, y, x + h, y, t, c);
    eline(r, x, y - h, x, y + h, t, c);
}

#include "../screens/stroke_font.h"
#define etext sf_text

static char *eu_num(char *p, int v)
{
    if (v >= 100) *p++ = (char)('0' + v / 100);
    if (v >= 10) *p++ = (char)('0' + v / 10 % 10);
    *p++ = (char)('0' + v % 10); *p = 0; return p;
}
static void eu_note_name(char *p, int n)
{
    static const char nm[12][3] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const char *s = nm[n % 12];
    while (*s) *p++ = *s++;
    int o = n / 12 - 1; if (o < 0) { *p++ = '-'; o = -o; }
    eu_num(p, o);
}

/* point i of n on a ring of radius rad (Q10) around (cx, cy), step 0 at the top, clockwise */
static void eu_ring_pt(int i, int n, int32_t cx, int32_t cy, int32_t rad, int32_t *x, int32_t *y)
{
    int32_t a = (i * 8192 / n) & 8191;                  /* angle, 8192 = full turn */
    int32_t s = op1_sin(a), c = op1_sin((a + 2048) & 8191);
    *x = cx + (int32_t)(((int64_t)s * rad) >> 15);
    *y = cy - (int32_t)(((int64_t)c * rad) >> 15);
}

static void eu_screen_draw(void *r, int pos)
{
    eu_bank_t *b = &eu[pos & 1];
    /* mini rings */
    eu_bank_init(b);
    for (int k = 0; k < EU_MAX; k++) {
        const eu_seq_t *q = &b->s[k];
        int32_t cx = OP1_Q10(EU_MINI_X0 + k * EU_MINI_DX), cy = OP1_Q10(EU_MINI_Y);
        if (k == b->sel) eline(r, cx - OP1_Q10(8), cy + OP1_Q10(EU_MINI_R + 6), cx + OP1_Q10(8), cy + OP1_Q10(EU_MINI_R + 6), 0x500, eu_white);
        if (q->running) edot(r, cx, cy, 0xC00, eu_col[3]);         /* playing: red centre dot (also without a note) */
        if (!q->used) continue;
        for (int i = 0; i < q->steps; i++) if (eu_hit(q, i)) {        /* static: pattern only, no playhead */
            int32_t x, y; eu_ring_pt(i, q->steps, cx, cy, OP1_Q10(EU_MINI_R), &x, &y);
            edot(r, x, y, 0xE00, q->muted ? eu_dimgreen : eu_green);   /* muted: dimmed */
        }
    }
    const eu_seq_t *q = &b->s[b->sel];
    /* big ring */
    int32_t cx = OP1_Q10(EU_RING_X), cy = OP1_Q10(EU_RING_Y);
    for (int i = 0; i < q->steps; i++) {
        int32_t x, y; eu_ring_pt(i, q->steps, cx, cy, OP1_Q10(EU_RING_R), &x, &y);
        int cur = q->running && q->step >= 0 && (q->step % q->steps) == i;
        int hit = eu_hit(q, i);
        if (cur) edot(r, x, y, 0x2400, hit && !q->muted ? eu_white : eu_grey);
        else edot(r, x, y, hit ? 0x1C00 : 0xA00, hit ? (q->muted ? eu_dimgreen : eu_green) : eu_grey);
        if (i == 0) {                                   /* step 1 marker (rotation reference): tick outside */
            int32_t x2, y2; eu_ring_pt(0, q->steps, cx, cy, OP1_Q10(EU_RING_R + 11), &x2, &y2);
            eline(r, x2, y2 + OP1_Q10(3), x2, y2 - OP1_Q10(1), 0x400, eu_white);
        }
    }
    if (q->running) edot(r, cx, cy, 0x1C00, eu_col[3]);          /* playing: red centre dot, size of a pulse dot */
    /* level (velocity) of the selected slot: red bar at the right border with a dot, like the tape screen's input
     * level (tape.svg mixervol / reclevel); red page 2 active: small triangle left of the dot, pointing at it */
    {
        int32_t lx = OP1_Q10(EU_LVL_X), ly = OP1_Q10(EU_LVL_Y1) - (q->vel - 1) * OP1_Q10(EU_LVL_Y1 - EU_LVL_Y0) / 126;
        eline(r, lx, OP1_Q10(EU_LVL_Y0), lx, ly, 0x600, eu_dimred);           /* above the level: dim */
        eline(r, lx, ly, lx, OP1_Q10(EU_LVL_Y1), 0x600, eu_col[3]);
        edot(r, lx, ly, 0x1B00, eu_col[3]);
        if (b->page[3])
            for (int i = 0; i < 5; i++)                 /* filled triangle, tip 6 px left of the bar: 1 px strokes */
                eline(r, lx - OP1_Q10(10.5 - i), ly - OP1_Q10(5 - i), lx - OP1_Q10(10.5 - i), ly + OP1_Q10(5 - i), 0x480, eu_col[3]);
    }
    char pt[3] = { 'P', (char)('1' + b->pat), 0 };                  /* pattern, bottom left */
    etext(r, OP1_Q10(EU_PAT_X), OP1_Q10(EU_PAT_Y), OP1_Q10(2), pt, 0x400, eu_white);
    /* note name */
    char t[12];
    int n = q->note + (pos & 1 ? 0 : (q->oct - 2) * 12);
    if (!q->used) { t[0] = '-'; t[1] = '-'; t[2] = 0; }             /* no note yet: press a key */
    else if (n >= 0 && n <= 127) eu_note_name(t, n);
    else { t[0] = '?'; t[1] = 0; }                                  /* below / above MIDI: plays nothing */
    etext(r, OP1_Q10(EU_TEXT_X), OP1_Q10(EU_NOTE_Y), OP1_Q10(3), t, 0x600, eu_white);
    /* encoder rows */
    static const char *rates[EU_RATES] = { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" };
    for (int k = 0; k < 3; k++) {                       /* blue, green, white (red: centre dot + level bar) */
        int32_t y = OP1_Q10(EU_ROW_Y0 + k * EU_ROW_DY);
        int pg = b->page[k];
        /* page indicator: page 1 filled dot, page 2 dot with a gap ring */
        edot(r, OP1_Q10(EU_TEXT_X + 3), y, 0x1C00, eu_col[k]);
        if (pg) eline(r, OP1_Q10(EU_TEXT_X + 11), y - OP1_Q10(5), OP1_Q10(EU_TEXT_X + 11), y + OP1_Q10(5), 0x400, eu_col[k]);   /* page 2 */
        char *p = t; t[0] = 0;
        if (k == 0) { if (pg) { const char *s = rates[q->rate]; while ((*p++ = *s++)) ; } else eu_num(p, q->steps); }
        else if (k == 1) { if (pg) { p = eu_num(p, q->gate); p[0] = '%'; p[1] = 0; } else eu_num(p, q->pulses); }
        else if (k == 2) { if (pg) { int o = q->oct - 2; *p++ = o < 0 ? '-' : '+'; eu_num(p, o < 0 ? -o : o); } else eu_num(p, q->rot); }
        int32_t tx = OP1_Q10(EU_TEXT_X + 18), ty = y - OP1_Q10(6);
        etext(r, tx, ty, OP1_Q10(2), t, 0x400, eu_col[k]);
    }
}

static void eu_screen_update(void *scr, int dt, int pos)
{
    op1_screen_draw(scr, dt);
    eu_screen_draw(op1_raster(), pos);
}

#ifndef OP1_HOST
/* ======================= firmware player (extra sequencer "euclid", runs as type 3 + flag) =======================
 * DB row seq_types "euclid"; SeqIds::from_name "euclid" -> type 3 with eu_active[mode] = 1 (dispatcher.c).
 * PlayerManager::create_player case 3 (CALL 0x019ED460 @0x01A0D150) -> eu_player_get: euclid player if
 * eu_active[mode], else the original finger player (0x019ED460). The player object is a
 * vtable pointer; vtable entries are {adj, fn} pairs (call: fn(self + adj, ...)), offsets as ok_player
 * (vtable 0x01910E9C): +0x14 key_down(key 0..23) -> 1 = consumed, +0x1c key_up(key) (consumed keys only),
 * +0x24 reset, +0x2c tick (96 per beat, only for the current mode's player while the sequencer is on),
 * +0x3c stop. Mode (synth 0 / drum 1) = 0x01DE4C40. */
#define EU_MODE() (*(volatile int *)0x01DE4C40 == 1)
extern volatile uint8_t eu_rec_play;                    /* seq/seq_core.c: Shift+C# with REC held -> PLAY */
static int eu_nop(void *self) { (void)self; return 0; }
static int eu_key_down(void *self, int key)
{
    uint32_t caller = OP1_SEQ_CALLER();                                     /* keyboard or USB MIDI */
    (void)self;
    if (key < 0 || key > 23) return 0;
    if (*(volatile uint8_t *)0x01DD7F78) {                              /* Shift + key: mute / run (any screen) */
        int h = eu_shift_key(EU_MODE(), key);
        int tape = *(volatile int *)0x018C17D4;                          /* tape state: 3 / 6 = REC held (armed) */
        if (h == 2 && (tape == 3 || tape == 6)) eu_rec_play = 1;         /* dispatcher presses PLAY afterwards */
        if (h) return 1;                                                 /* consumed: no note, no assignment */
    }
    if (*(volatile int *)0x018B65C8 == 0x2D)                        /* euclid screen shown: assign note */
        eu_key_ex(EU_MODE(), op1_seq_key_note(EU_MODE(), key, EU_MODE() ? DRUM_OCT() : SYNTH_OCT(), caller), key);   /* pitch: absolute, as it sounds */
    return 0;                                           /* not consumed: the key also plays its note */
}
static void eu_reset(void *self) { (void)self; eu_all_off(EU_MODE()); }
static void eu_ptick(void *self, int counter) { (void)self; (void)counter; eu_advance(EU_MODE(), 1u << 14); }   /* 1/96 beat */
static void eu_stop(void *self) { (void)self; eu_all_off(0); eu_all_off(1); }
static const uint32_t eu_vt[16] = {
    0, 0,  0, (uint32_t)eu_nop,  0, (uint32_t)eu_key_down,  0, (uint32_t)eu_nop,
    0, (uint32_t)eu_reset,  0, (uint32_t)eu_ptick,  0, (uint32_t)eu_nop,  0, (uint32_t)eu_stop };
static struct { const uint32_t *vt; uint32_t pad[7]; } eu_player = { eu_vt, { 0 } };
volatile uint8_t eu_active[2];                          /* euclid selected (synth, drum) */

/* ---- persistence via the firmware's background saver (bg_saver, as finger's okseq) ----
 * object {u8 dirty, u8 saving, .., vtable @+4}; register 0x0193F010(obj) (sets the base vtable -> ours after),
 * mark dirty 0x0193EF3C(obj) (wakes the saver thread), the saver calls vt+0x14 save(obj) in its own thread.
 * DB: 0x0196DD44() (as okseq); exec 0x0196F950(db, sql); get_string 0x0196FBD0(&out_string, db, sql). */
#define BG_REGISTER ((void (*)(void *))0x0193F010)
#define BG_DIRTY    ((void (*)(void *))0x0193EF3C)
#define DB_HANDLE   ((void *(*)(void))0x0196DD44)
#define DB_EXEC     ((void (*)(void *, const char *))0x0196F950)
#define DB_GET      ((void (*)(void *, void *, const char *))0x0196FBD0)
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } eu_dstr;
static const char EU_CREATE[] = "CREATE TABLE IF NOT EXISTS euclidseq (is_synth INTEGER PRIMARY KEY, data TEXT NOT NULL)";
static void eu_bg_nop(void *self) { (void)self; }
static void eu_bg_save(void *self)
{
    (void)self;
    static char sql[1400];                              /* 4 patterns: <= ~1250 chars per row */
    void *db = DB_HANDLE();
    DB_EXEC(db, EU_CREATE);
    for (int pos = 0; pos < 2; pos++) {
        static const char head[] = "INSERT OR REPLACE INTO euclidseq VALUES (";
        int o = 0;
        for (const char *h = head; *h; h++) sql[o++] = *h;
        sql[o++] = pos ? '0' : '1'; sql[o++] = ','; sql[o++] = '\'';
        o += eu_serialize(pos, sql + o, (int)sizeof sql - o - 4);
        sql[o++] = '\''; sql[o++] = ')'; sql[o] = 0;
        op1_db_write(db, sql);                          /* (no assert: long statement, docs/17) */
    }
}
static const uint32_t eu_bg_vt[6] = { 0, 0, 0, (uint32_t)eu_bg_nop, 0, (uint32_t)eu_bg_save };
static struct { uint8_t dirty, saving, pad[2]; const uint32_t *vt; uint32_t more[2]; } eu_bg;
static uint8_t eu_bg_ready;
static void eu_changed(void) { if (eu_bg_ready) BG_DIRTY(&eu_bg); }
static void eu_load(void)
{
    void *db = DB_HANDLE();
    DB_EXEC(db, EU_CREATE);
    for (int pos = 0; pos < 2; pos++) {
        eu_dstr s = { 0 };
        DB_GET(&s, db, pos ? "SELECT data FROM euclidseq WHERE is_synth=0" : "SELECT data FROM euclidseq WHERE is_synth=1");
        const char *p = s.cap > 15 ? s.u.ptr : s.u.buf;
        if (s.size && s.size < 1300) eu_parse(pos, p, (int)s.size);  /* (a heap copy, once per boot, is not freed) */
    }
    BG_REGISTER(&eu_bg);
    eu_bg.vt = eu_bg_vt;
    eu_bg_ready = 1;
}

void *eu_player_get(void)
{
    if (!eu_bg_ready) eu_load();
    if (!eu_active[EU_MODE()]) return ((void *(*)(void))0x019ED460)();   /* finger */
    return &eu_player;
}

/* screen: own object swapped into the finger screen slot 0x2D (dispatcher.c) */
static void eu_scr_init(void *scr) { (void)scr; }
static void eu_scr_update(void *scr, int dt) { eu_screen_update(scr, dt, EU_MODE()); }
const op1_screen_t SCREEN_euclid = { eu_scr_init, eu_scr_update };

/* plugin interface (docs/44): page keys (lift / drop / scissors = copy / paste / cut, Shift: whole pattern; press
 * toggles page 2) and turns (Shift + white: slot, Shift + red: start / stop all) */
static int eu_pkey(int m, int code, int pressed, int knob, int shift)
{
    if (code >= 0x0A && code <= 0x0C) { if (pressed) eu_clip(m, code == 0x0A ? 0 : code == 0x0B ? 1 : 2, shift); return 1; }
    if (knob >= 0) { if (pressed) eu_press(m, knob); return 1; }
    return 0;
}
static void eu_pturn(int m, int knob, int delta, int shift)
{
    if (knob == 2 && shift) eu_select(m, delta);
    else if (knob == 3 && shift) eu_run_all(m, delta);
    else eu_turn(m, knob, delta);
}
const op1_seq_t SEQ_euclid = { eu_active, eu_player_get, eu_pkey, eu_pturn, eu_midi_start, 0 };
#endif
