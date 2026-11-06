/* "dots" sequencer: 6 step-sequencer slots per mode (synth / drum), up to 64 steps each, all slots play at once.
 *
 * Each step is empty or a trigger {note, key, level}. One note per step (no chords).
 * Controls:  blue        cursor (wraps within the length)
 *            Shift+white select slot 1..6 (wraps)
 *            red         right = start, left = stop the selected slot
 *            Shift+red   right = start all, left = stop all (also in record mode); Shift + C#: toggle all
 *            REC key     record on / off
 *            red press   record type REC (step, default) / LIVE, any time
 * Step record: a note key puts a trigger with that note at the cursor (replacing what was there), then the cursor
 * moves one step on (step recorder). The key also sounds. Red turns the level of the trigger under the cursor.
 * Shift+blue moves the cursor erasing every step it leaves. Green: length +-1; Shift+green: fill to the next 16
 * (full last row: +16) copying the row above into the new steps, left: back to the previous 16.
 * The cursor never follows the playhead.
 * Live record (type LIVE, recording on, selected slot running; while it is stopped LIVE records at the cursor
 * like REC, and switches to live recording as soon as the slot runs): a note key is snapped to the nearest step at the playhead (hard
 * quantised), plays at once (the key is consumed and sounded by the sequencer) and its hold time sets the gate
 * (5..100 % of a step, longer = TIE). The recorded step does not play again in the same pass. New triggers get
 * the defaults, an existing trigger keeps its parameters (note, key and gate change). The cursor jumps to the
 * recorded step (no cursor bar while recording live).
 * Mutes / patterns / clipboard as euclid: Shift + G A B C D E mute slot 1..6, Shift + D# all; Shift + F G A B
 * pattern 1..4; lift / drop / scissors copy / paste / cut the selected slot (Shift: whole pattern).
 * Trigger parameters (record mode, white encoder): press white to switch page 1 (white turns the value of the
 * selected parameter for the trigger at the cursor) / page 2 (white selects the parameter):
 *   gate    5..100 % of the (ratchet) step, or TIE: held until the slot's next note
 *   ratch   1..8 repeats inside the step (each with the gate)
 *   chance  0..100 %: random skip
 *   cond    ALL or a:b = plays only in loop a of every b loops of the slot (1:2 .. 4:4)
 *   nudge   -12..+12 / 96 beat (up to half a step early / late)
 *   ramp    OFF, UP (repeats fade in), DN (fade out)
 *   fx1..4  parameter locks: -- (off, default) or 0..100 % for knob 1..4 (blue..red) of the mode's instrument FX.
 *           When the step plays (chance / cond / nudge apply) the knob jumps to the value; it reverts to the knob's own
 *           setting after the gate (% of a step; TIE: until the slot's next trigger). Last lock wins a shared knob.
 *   sy1..4  the same for knob 1..4 of the synth engine of the instrument (synth mode only; not in drum mode). Knobs of
 *           an engine without a plain range (sample start / end) are not locked.
 * Lock triggers (no note): blue press in record mode on an empty step creates one (all locks off = does nothing
 * until a lock is set), on a lock trigger removes it. A key on a lock trigger adds a note and keeps the locks.
 * New triggers get the defaults (50 %, 1, 100 %, ALL, 0, OFF); recording over a trigger only changes its note.
 * Length 1..64 (default 16). Step rate fixed 1/16.
 *
 * Platform independent like euclid.c; the firmware glue provides DT_NOTE_ON / DT_NOTE_OFF. */
#include "../op1_synth.h"

#define DT_SLOTS    6
#define DT_STEPS    64
#define DT_PATS     4
#define DT_PPQN     24
#define DT_RATE     6                                   /* ticks per step: 1/16 */
#define DT_TIE      101                                 /* gate value: hold until the next note */
#define DT_NONOTE   0xFF                                /* note of a lock trigger (no note) */
#define DT_NFX      4                                   /* locks: instrument FX knobs 1..4 */
#define DT_NSY      4                                   /* locks: synth engine knobs 1..4 (synth mode) */
#define DT_NLK      (DT_NFX + DT_NSY)                   /* lock index: 0..3 FX, 4..7 synth engine */
enum { DT_P_GATE, DT_P_RATCH, DT_P_CHANCE, DT_P_COND, DT_P_NUDGE, DT_P_RAMP, DT_P_FX1, DT_P_SY1 = DT_P_FX1 + DT_NFX,
       DT_NPARAMS = DT_P_SY1 + DT_NSY };
#define DT_NPAR(pos) ((pos) ? DT_P_SY1 : DT_NPARAMS)    /* drum mode has no synth engine */
static const uint8_t dt_cond_ab[10][2] = { {0,0}, {1,2}, {2,2}, {1,3}, {2,3}, {3,3}, {1,4}, {2,4}, {3,4}, {4,4} };

typedef struct {                                        /* vel 0 = no trigger */
    uint8_t note, key, vel, gate, ratch, chance, cond;
    int8_t  nudge, ramp;
    uint8_t fx[DT_NLK];                                 /* 0 = off, 1..101 = 0..100 % */
} dt_step_t;
#define DT_STEP_BYTES 9                                 /* saved fields of the step row (locks: own row) */
typedef struct {
    uint8_t   len, muted;
    dt_step_t st[DT_STEPS];
} dt_cfg_t;                                             /* saved, per pattern */
typedef struct {                                        /* a decided step: its hits, in time order */
    uint8_t  on, ratch, j, note, key, vel, gate;        /* j: next hit */
    int8_t   ramp;
    uint8_t  fx[DT_NLK];
    int32_t  s;                                         /* step (counted from start) */
    uint64_t t0, sub;                                   /* first hit, hit spacing (Q16 ticks) */
} dt_plan_t;
typedef struct {
    uint8_t  running, sounding;                         /* sounding: note id + 1 */
    int16_t  step;                                      /* playhead (display), -1 = not started yet */
    uint32_t start;                                     /* tick of step 0 */
    int32_t  planned;                                   /* next step (counted from start) to decide */
    int32_t  skip;                                      /* live record: do not decide this step once (-1 none) */
    uint64_t off_at;                                    /* note-off time of the sounding note (~0: tie) */
    uint8_t  locks;                                     /* knobs this slot has locked (bit k) */
    uint64_t lock_off;                                  /* revert time (~0: until the next trigger) */
    dt_plan_t p[2];                                     /* a late step can overlap an early next one */
} dt_run_t;                                             /* live, global per slot (as euclid) */

typedef struct {
    dt_cfg_t pats[DT_PATS][DT_SLOTS];
    dt_run_t rt[DT_SLOTS];
    uint8_t  init, pat, sel, rec, cur[DT_SLOTS];        /* cursor per slot */
    uint8_t  live;                                      /* record type: 0 REC (step), 1 LIVE */
    uint8_t  page, par;                                 /* white: 0 value / 1 parameter select; parameter */
    uint8_t  live_on[24], live_i[24];                   /* live record: key held, its step; */
    int16_t  live_id[24];                               /* sounding note id */
    uint64_t live_t[24];                                /* press time (Q16 ticks) */
    uint64_t tick_q16;
    int32_t  acc[4];
} dt_bank_t;

static dt_bank_t dt[2];                                 /* 0 synth, 1 drum */

#ifdef OP1_HOST
int host_dt_changed;
#define DT_CHANGED() (host_dt_changed++)
void host_dt_note_on(int pos, int note, int vel);
void host_dt_note_off(int pos, int note);
#define DT_NOTE_ON(pos, n, k, v) (host_dt_note_on((pos), (n), (v)), (n))
#define DT_NOTE_OFF(pos, n)      host_dt_note_off((pos), (n))
void host_dt_fx_lock(int pos, int knob, int pc);       /* knob: lock index 0..7 */
void host_dt_fx_restore(int pos, int knob);
#define DT_FX_LOCK(pos, k, pc)   host_dt_fx_lock((pos), (k), (pc))
#define DT_FX_RESTORE(pos, k)    host_dt_fx_restore((pos), (k))
#else
static void dt_changed(void);
#define DT_CHANGED() dt_changed()
int  eu_fw_note_on(int pos, int note, int key, int vel);   /* seq/euclid.c: VoiceCtl note on / off */
void eu_fw_note_off(int midi);
#define DT_NOTE_ON(pos, n, k, v) eu_fw_note_on((pos), (n), (k), (v))
#define DT_NOTE_OFF(pos, n)      eu_fw_note_off(n)
static void dt_fw_fx_lock(int pos, int knob, int pc);
static void dt_fw_fx_restore(int pos, int knob);
#define DT_FX_LOCK(pos, k, pc)   dt_fw_fx_lock((pos), (k), (pc))
#define DT_FX_RESTORE(pos, k)    dt_fw_fx_restore((pos), (k))
#endif

static int dt_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static dt_cfg_t *dt_q(dt_bank_t *b, int k) { return &b->pats[b->pat][k]; }
static void dt_step_default(dt_step_t *s)
{
    s->gate = 50; s->ratch = 1; s->chance = 100; s->cond = 0; s->nudge = 0; s->ramp = 0;
    for (int i = 0; i < DT_NLK; i++) s->fx[i] = 0;
}
/* byte loops instead of struct copies: the firmware build has no memcpy / memset */
static void dt_cfg_copy(dt_cfg_t *d, const dt_cfg_t *c)
{
    uint8_t *o = (uint8_t *)d; const uint8_t *i = (const uint8_t *)c;
    for (unsigned n = 0; n < sizeof *d; n++) o[n] = i[n];
}
static void dt_cfg_clear(dt_cfg_t *d, int len)
{
    uint8_t *o = (uint8_t *)d;
    for (unsigned n = 0; n < sizeof *d; n++) o[n] = 0;
    d->len = (uint8_t)len;
    for (int i = 0; i < DT_STEPS; i++) dt_step_default(&d->st[i]);   /* empty steps keep valid parameters */
}

static void dt_bank_init(dt_bank_t *b)
{
    if (b->init) return;
    b->init = 1;
    for (int p = 0; p < DT_PATS; p++) for (int k = 0; k < DT_SLOTS; k++) dt_cfg_clear(&b->pats[p][k], 16);
    for (int k = 0; k < DT_SLOTS; k++) b->rt[k].step = -1;
}

/* locks: the knob owner per mode (slot + 1); the base value is kept by the glue until the owner reverts */
static uint8_t dt_lk_owner[2][DT_NLK];
static void dt_unlock(int pos, int slot, dt_run_t *r)
{
    for (int i = 0; i < DT_NLK; i++)
        if (r->locks >> i & 1 && dt_lk_owner[pos][i] == slot + 1) { DT_FX_RESTORE(pos, i); dt_lk_owner[pos][i] = 0; }
    r->locks = 0;
}
static void dt_lock(int pos, int slot, dt_run_t *r, const uint8_t *fx)
{
    for (int i = 0; i < DT_NLK; i++)
        if (fx[i] && !(pos && i >= DT_NFX)) { DT_FX_LOCK(pos, i, fx[i] - 1); dt_lk_owner[pos][i] = (uint8_t)(slot + 1); r->locks |= (uint8_t)(1 << i); }
}
static int dt_slot_of(int pos, const dt_run_t *r) { return (int)(r - dt[pos & 1].rt); }

static void dt_silence(int pos, dt_run_t *r)
{
    if (r->sounding) { DT_NOTE_OFF(pos, r->sounding - 1); r->sounding = 0; }
    r->off_at = 0;
    if (r->locks) dt_unlock(pos, dt_slot_of(pos, r), r);
}

/* ---- keys / encoders (UI thread) ---- */
void dt_select(int pos, int delta)
{
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    if ((b->acc[2] > 0 && delta < 0) || (b->acc[2] < 0 && delta > 0)) b->acc[2] = 0;
    b->acc[2] += delta;
    int st = b->acc[2] / 1024; if (!st) return;
    b->acc[2] -= st * 1024;
    b->sel = (uint8_t)(((b->sel + st) % DT_SLOTS + DT_SLOTS) % DT_SLOTS);
    DT_CHANGED();
}

void dt_press(int pos, int knob)
{
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    if (knob == 3) b->live ^= 1;                        /* red press: record type REC / LIVE */
    if (knob == 0 && b->rec) {                          /* blue press (record mode): lock trigger on / off */
        dt_step_t *s = &dt_q(b, b->sel)->st[b->cur[b->sel]];
        if (!s->vel) { s->vel = 100; dt_step_default(s); s->note = DT_NONOTE; s->key = 0; }
        else if (s->note == DT_NONOTE) s->vel = 0;
        else return;                                    /* note trigger: locks via white */
        DT_CHANGED();
    }
    if (knob == 2 && b->rec) b->page ^= 1;              /* white press (record mode): value / parameter page */
}

void dt_set_len(int pos, int k, int len)                /* control to be decided; used by tests */
{
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    dt_cfg_t *q = dt_q(b, k);
    q->len = (uint8_t)dt_clampi(len, 1, DT_STEPS);
    if (b->cur[k] >= q->len) b->cur[k] = 0;
    DT_CHANGED();
}

/* knob 0 blue, 3 red; shift: Shift held (blue erases) */
static void dt_start(dt_bank_t *b, dt_run_t *r, uint32_t at)
{
    (void)b; r->start = at; r->step = -1; r->planned = 0; r->skip = -1; r->p[0].on = r->p[1].on = 0; r->running = 1;
}
static void dt_halt(int pos, dt_run_t *r) { r->running = 0; dt_silence(pos, r); r->step = -1; r->p[0].on = r->p[1].on = 0; }

/* white page 1: parameter value of a trigger, +-st detents */
static void dt_param_turn(dt_step_t *s, int par, int st)
{
    switch (par) {
    case DT_P_GATE:   s->gate = (uint8_t)(s->gate == DT_TIE ? (st < 0 ? 100 : DT_TIE)
                                          : s->gate + st * 5 > 100 ? DT_TIE : dt_clampi(s->gate + st * 5, 5, 100)); break;
    case DT_P_RATCH:  s->ratch = (uint8_t)dt_clampi(s->ratch + st, 1, 8); break;
    case DT_P_CHANCE: s->chance = (uint8_t)dt_clampi(s->chance + st * 5, 0, 100); break;
    case DT_P_COND:   s->cond = (uint8_t)dt_clampi(s->cond + st, 0, 9); break;
    case DT_P_NUDGE:  s->nudge = (int8_t)dt_clampi(s->nudge + st, -12, 12); break;
    case DT_P_RAMP:   s->ramp = (int8_t)dt_clampi(s->ramp + st, -1, 1); break;
    default: {                                          /* fx / synth lock: off below 0 %, 2 % per detent */
        uint8_t *v = &s->fx[par - DT_P_FX1];
        int pc = *v ? *v - 1 : -1;
        pc = pc < 0 ? (st > 0 ? (st - 1) * 2 : -1) : pc + st * 2;
        *v = (uint8_t)(pc < 0 ? 0 : dt_clampi(pc, 0, 100) + 1);
    } }
}
static uint32_t dt_next_step(const dt_bank_t *b) { return ((uint32_t)(b->tick_q16 >> 16) / DT_RATE + 1) * DT_RATE; }

/* run all (Shift + red: on = 1 start / 0 stop; Shift + C#: on = -1 toggle = stop all if any runs, else start all).
 * All start together on the next step boundary. Returns 1 if it started the slots. */
int dt_run_all(int pos, int on)
{
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    if (on < 0) { on = 1; for (int k = 0; k < DT_SLOTS; k++) if (b->rt[k].running) on = 0; }
    uint32_t at = dt_next_step(b);
    for (int k = 0; k < DT_SLOTS; k++) {
        dt_run_t *r = &b->rt[k];
        if (on && !r->running) dt_start(b, r, at);
        else if (!on && r->running) dt_halt(pos & 1, r);
    }
    return on;
}

/* length change (record mode). Green: +-1 step, an added step starts empty. Shift + green: right = fill up to the
 * next multiple of 16 (a full last row: add 16), left = back to the previous multiple of 16 (min 16); steps added
 * this way copy the trigger of the step one row above. */
static void dt_len_step(dt_cfg_t *q, int st, int shift)
{
    for (int n = st < 0 ? -st : st; n--; ) {
        int len = q->len, to;
        if (!shift) to = len + (st > 0 ? 1 : -1);
        else if (st > 0) to = (len / 16 + 1) * 16;
        else to = len > 16 ? (len - 1) / 16 * 16 : len;
        to = dt_clampi(to, 1, DT_STEPS);
        for (int i = len; i < to; i++) {
            if (shift && i >= 16) { uint8_t *d = (uint8_t *)&q->st[i]; const uint8_t *c = (const uint8_t *)&q->st[i - 16];
                                    for (unsigned n = 0; n < sizeof q->st[i]; n++) d[n] = c[n]; }
            else q->st[i].vel = 0;
        }
        q->len = (uint8_t)to;
    }
}

/* knob 0 blue, 1 green, 3 red; shift: Shift held */
void dt_turn(int pos, int knob, int delta, int shift)
{
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    if (knob < 0 || knob > 3) return;
    if ((b->acc[knob] > 0 && delta < 0) || (b->acc[knob] < 0 && delta > 0)) b->acc[knob] = 0;
    b->acc[knob] += delta;
    int st = b->acc[knob] / 1024; if (!st) return;
    b->acc[knob] -= st * 1024;
    int k = b->sel; dt_cfg_t *q = dt_q(b, k); dt_run_t *r = &b->rt[k];
    if (knob == 0) {                                    /* cursor; record mode + Shift: erase the steps it leaves */
        int erase = shift && b->rec;
        for (int n = st < 0 ? -st : st; n--; ) {
            if (erase) q->st[b->cur[k]].vel = 0;
            b->cur[k] = (uint8_t)((b->cur[k] + (st > 0 ? 1 : q->len - 1)) % q->len);
        }
        if (erase) DT_CHANGED();
        return;
    }
    if (knob == 2) {                                    /* white, record mode: parameter value / selection */
        if (!b->rec) return;
        if (b->page) { int n = DT_NPAR(pos & 1); b->par = (uint8_t)((b->par + st % n + n) % n); return; }
        dt_step_t *s = &q->st[b->cur[k]];
        if (s->vel) { dt_param_turn(s, b->par, st); DT_CHANGED(); }
        return;
    }
    if (knob == 1) {                                    /* green: length, record mode only */
        if (!b->rec) return;
        dt_len_step(q, st, shift);
        if (b->cur[k] >= q->len) b->cur[k] = (uint8_t)(q->len - 1);
        DT_CHANGED();
        return;
    }
    if (shift) { dt_run_all(pos, st > 0); return; }     /* Shift + red: start / stop all */
    if (b->rec) {                                       /* red, record mode: level of the trigger at the cursor */
        dt_step_t *s = &q->st[b->cur[k]];
        if (s->vel) { s->vel = (uint8_t)dt_clampi(s->vel + st * 4, 1, 127); DT_CHANGED(); }
        return;
    }
    if (st > 0 && !r->running) dt_start(b, r, dt_next_step(b));   /* red, perform: start on the next step */
    else if (st < 0 && r->running) dt_halt(pos & 1, r);
}

/* note key (record mode only): note = absolute pitch, key 0..23. Returns 1 if recorded. The key still sounds. */
int dt_key(int pos, int note, int key)
{
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    if (!b->rec || (b->live && b->rt[b->sel].running)) return 0;   /* step record (live: dt_live_down) */
    int k = b->sel; dt_cfg_t *q = dt_q(b, k);
    dt_step_t *s = &q->st[b->cur[k]];
    if (!s->vel) { s->vel = 100; dt_step_default(s); }  /* new trigger: defaults; else only the note changes */
    s->note = (uint8_t)note; s->key = (uint8_t)key;
    b->cur[k] = (uint8_t)((b->cur[k] + 1) % q->len);
    DT_CHANGED();
    return 1;
}

/* REC key on the dots screen: recording on / off (type: red press) */
void dt_rec_key(int pos)
{
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    b->rec ^= 1;
}

/* LIVE: key pressed. Returns 1 if recorded (the key is then consumed and sounded here). */
int dt_live_down(int pos, int note, int key)
{
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    int k = b->sel; dt_run_t *r = &b->rt[k]; dt_cfg_t *q = dt_q(b, k);
    if (!b->rec || !b->live || !r->running || key < 0 || key > 23) return 0;
    const uint64_t L = (uint64_t)DT_RATE << 16, S = (uint64_t)r->start << 16, t = b->tick_q16;
    int32_t sn = t + L / 2 >= S ? (int32_t)((t + L / 2 - S) / L) : 0;     /* nearest step */
    int i = sn % q->len;
    dt_step_t *st = &q->st[i];
    if (!st->vel) { st->vel = 100; dt_step_default(st); }
    st->note = (uint8_t)note; st->key = (uint8_t)key;
    for (int j = 0; j < 2; j++) if (r->p[j].on && r->p[j].s == sn) r->p[j].on = 0;   /* already decided: drop */
    if (sn >= r->planned) r->skip = sn;                 /* not decided yet: skip it once */
    if (b->live_on[key]) DT_NOTE_OFF(pos & 1, b->live_id[key]);
    int id = DT_NOTE_ON(pos & 1, note, key, st->vel);
    b->live_on[key] = 1; b->live_id[key] = (int16_t)id; b->live_i[key] = (uint8_t)i; b->live_t[key] = t;
    b->cur[k] = (uint8_t)i;
    DT_CHANGED();
    return 1;
}

/* LIVE: key released -> note off, gate from the hold time */
void dt_live_up(int pos, int key)
{
    dt_bank_t *b = &dt[pos & 1];
    if (key < 0 || key > 23 || !b->live_on[key]) return;
    b->live_on[key] = 0;
    if (b->live_id[key] >= 0) DT_NOTE_OFF(pos & 1, b->live_id[key]);
    const uint64_t L = (uint64_t)DT_RATE << 16;
    uint64_t held = b->tick_q16 - b->live_t[key];
    int g = (int)(held * 100 / L);
    dt_step_t *st = &dt_q(b, b->sel)->st[b->live_i[key]];
    if (st->vel) st->gate = (uint8_t)(g > 100 ? DT_TIE : dt_clampi((g + 2) / 5 * 5, 5, 100));
    DT_CHANGED();
}

void dt_mute(int pos, int k)
{
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    dt_cfg_t *q = dt_q(b, k); q->muted ^= 1;
    if (q->muted) dt_silence(pos & 1, &b->rt[k]);
    DT_CHANGED();
}

void dt_recall(int pos, int p)
{
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    if (p < 0 || p >= DT_PATS || p == b->pat) return;
    for (int k = 0; k < DT_SLOTS; k++) dt_silence(pos & 1, &b->rt[k]);
    b->pat = (uint8_t)p;
    for (int k = 0; k < DT_SLOTS; k++) if (b->cur[k] >= dt_q(b, k)->len) b->cur[k] = 0;
    DT_CHANGED();
}

void dt_mute_all(int pos)                               /* all muted -> unmute all, else mute all */
{
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    int all = 1;
    for (int k = 0; k < DT_SLOTS; k++) if (!dt_q(b, k)->muted) all = 0;
    for (int k = 0; k < DT_SLOTS; k++) { dt_q(b, k)->muted = (uint8_t)!all; if (!all) dt_silence(pos & 1, &b->rt[k]); }
    DT_CHANGED();
}

/* Shift + key (as euclid): F G A B (0 2 4 6) -> pattern 1..4, G A B C D E (14 16 18 19 21 23) -> mute slot 1..6,
 * D# (22) -> mute all, C# (20) -> start / stop all. Returns 0 if not handled, 2 if C# started the slots, else 1. */
int dt_shift_key(int pos, int key)
{
    static const int8_t act[24] = { 10, -1, 11, -1, 12, -1, 13, -1, -1, -1, -1, -1, -1, -1,
                                    0, -1, 1, -1, 2, 3, 8, 4, 9, 5 };
    if (key < 0 || key > 23 || act[key] < 0) return 0;
    if (act[key] >= 10) dt_recall(pos, act[key] - 10);
    else if (act[key] == 8) return dt_run_all(pos, -1) ? 2 : 1;
    else if (act[key] == 9) dt_mute_all(pos);
    else dt_mute(pos, act[key]);
    return 1;
}

/* lift / drop / scissors = copy / paste / cut: the selected slot, or (all) every slot of the pattern.
 * Separate clipboards for slot and pattern, per mode. Cut clears the triggers and keeps the length. */
enum { DT_COPY, DT_PASTE, DT_CUT };
static dt_cfg_t dt_clip_pat[2][DT_SLOTS], dt_clip_slot[2];
static uint8_t dt_clip_full[2][2];
void dt_clip(int pos, int op, int all)
{
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    int m = pos & 1, k0 = all ? 0 : b->sel, n = all ? DT_SLOTS : 1;
    dt_cfg_t *clip = all ? dt_clip_pat[m] : &dt_clip_slot[m];
    if (op != DT_PASTE) { for (int i = 0; i < n; i++) dt_cfg_copy(&clip[i], dt_q(b, k0 + i)); dt_clip_full[m][all != 0] = 1; }
    if (op == DT_COPY || (op == DT_PASTE && !dt_clip_full[m][all != 0])) return;
    for (int i = 0; i < n; i++) {
        dt_cfg_t *q = dt_q(b, k0 + i);
        dt_silence(m, &b->rt[k0 + i]);
        if (op == DT_PASTE) dt_cfg_copy(q, &clip[i]);
        else dt_cfg_clear(q, q->len);
        if (b->cur[k0 + i] >= q->len) b->cur[k0 + i] = 0;
    }
    DT_CHANGED();
}

/* ---- persistence: one hex text per (mode, pattern): "v2" "pp ss" (current pattern, selected slot), then per slot
 * "len muted" + 64 steps x 9 fields (note key vel gate ratch chance cond nudge ramp, 2 hex digits each; "v2").
 * Old texts (no prefix, 3 fields: note key vel) still load with default parameters.
 * Parameter locks are a second text per (mode, pattern): "L1" + 6 slots x 64 steps x fx1..fx4 (3074 chars), the synth
 * locks a third: "S1" + the same x sy1..sy4. The firmware formats every SQL statement into an 8 KB buffer (db2.cpp,
 * te_vsnprintf 0x2000), one text with all of them is too long. ---- */
#define DT_TEXT1_LEN (4 + DT_SLOTS * (4 + DT_STEPS * 6))
#define DT_TEXT_LEN  (2 + 4 + DT_SLOTS * (4 + DT_STEPS * DT_STEP_BYTES * 2))
#define DT_LOCK_LEN  (2 + DT_SLOTS * DT_STEPS * DT_NFX * 2)
static const char dt_hexd[] = "0123456789abcdef";
static char *dt_hex(char *o, int v) { *o++ = dt_hexd[(v >> 4) & 15]; *o++ = dt_hexd[v & 15]; return o; }
static int dt_unhex(const char *s)
{
    int v = 0;
    for (int i = 0; i < 2; i++) {
        char c = s[i]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    return v;
}
int dt_serialize(int pos, int p, char *out)             /* out: DT_TEXT_LEN + 1 */
{
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    char *o = out; *o++ = 'v'; *o++ = '2';
    o = dt_hex(dt_hex(o, b->pat), b->sel);
    for (int k = 0; k < DT_SLOTS; k++) {
        const dt_cfg_t *q = &b->pats[p][k];
        o = dt_hex(dt_hex(o, q->len), q->muted);
        for (int i = 0; i < DT_STEPS; i++) {
            const dt_step_t *t = &q->st[i];
            o = dt_hex(dt_hex(dt_hex(o, t->note), t->key), t->vel);
            o = dt_hex(dt_hex(dt_hex(dt_hex(o, t->gate), t->ratch), t->chance), t->cond);
            o = dt_hex(dt_hex(o, (uint8_t)t->nudge), (uint8_t)t->ramp);
        }
    }
    *o = 0;
    return (int)(o - out);
}
int dt_parse(int pos, int p, const char *s, int len)    /* 0 = rejected (wrong size / bad digits), state kept */
{
    int v2 = len == DT_TEXT_LEN && s[0] == 'v' && s[1] == '2', nf = v2 ? DT_STEP_BYTES : 3;
    if ((!v2 && len != DT_TEXT1_LEN) || p < 0 || p >= DT_PATS) return 0;
    if (v2) { s += 2; len -= 2; }
    for (int i = 0; i < len; i += 2) if (dt_unhex(s + i) < 0) return 0;
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    if (p == 0) { b->pat = (uint8_t)(dt_unhex(s) % DT_PATS); b->sel = (uint8_t)(dt_unhex(s + 2) % DT_SLOTS); }
    s += 4;
    for (int k = 0; k < DT_SLOTS; k++) {
        dt_cfg_t *q = &b->pats[p][k];
        q->len = (uint8_t)dt_clampi(dt_unhex(s), 1, DT_STEPS); q->muted = dt_unhex(s + 2) != 0; s += 4;
        for (int i = 0; i < DT_STEPS; i++, s += nf * 2) {
            dt_step_t *t = &q->st[i];
            int nt = dt_unhex(s);
            t->note = (uint8_t)(nt == DT_NONOTE ? DT_NONOTE : nt & 127); t->key = (uint8_t)(dt_unhex(s + 2) % 24); t->vel = (uint8_t)(dt_unhex(s + 4) & 127);
            dt_step_default(t);
            if (!v2) continue;
            t->gate = (uint8_t)dt_clampi(dt_unhex(s + 6), 5, DT_TIE); t->ratch = (uint8_t)dt_clampi(dt_unhex(s + 8), 1, 8);
            t->chance = (uint8_t)dt_clampi(dt_unhex(s + 10), 0, 100); t->cond = (uint8_t)dt_clampi(dt_unhex(s + 12), 0, 9);
            t->nudge = (int8_t)dt_clampi((int8_t)dt_unhex(s + 14), -12, 12); t->ramp = (int8_t)dt_clampi((int8_t)dt_unhex(s + 16), -1, 1);
        }
    }
    for (int k = 0; k < DT_SLOTS; k++) if (b->cur[k] >= dt_q(b, k)->len) b->cur[k] = 0;
    return 1;
}

static int dt_ser_locks(int pos, int p, char *out, int f0, char tag)
{
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    char *o = out; *o++ = tag; *o++ = '1';
    for (int k = 0; k < DT_SLOTS; k++)
        for (int i = 0; i < DT_STEPS; i++)
            for (int f = f0; f < f0 + DT_NFX; f++) o = dt_hex(o, b->pats[p][k].st[i].fx[f]);
    *o = 0;
    return (int)(o - out);
}
static int dt_parse_lk(int pos, int p, const char *s, int len, int f0, char tag)   /* after dt_parse of the same pattern */
{
    if (len != DT_LOCK_LEN || s[0] != tag || s[1] != '1' || p < 0 || p >= DT_PATS) return 0;
    for (int i = 2; i < len; i += 2) if (dt_unhex(s + i) < 0) return 0;
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    s += 2;
    for (int k = 0; k < DT_SLOTS; k++)
        for (int i = 0; i < DT_STEPS; i++)
            for (int f = f0; f < f0 + DT_NFX; f++, s += 2) b->pats[p][k].st[i].fx[f] = (uint8_t)dt_clampi(dt_unhex(s), 0, 101);
    return 1;
}
int dt_serialize_locks(int pos, int p, char *out) { return dt_ser_locks(pos, p, out, 0, 'L'); }      /* out: DT_LOCK_LEN + 1 */
int dt_parse_locks(int pos, int p, const char *s, int len) { return dt_parse_lk(pos, p, s, len, 0, 'L'); }
int dt_serialize_sy(int pos, int p, char *out) { return dt_ser_locks(pos, p, out, DT_NFX, 'S'); }
int dt_parse_sy(int pos, int p, const char *s, int len) { return dt_parse_lk(pos, p, s, len, DT_NFX, 'S'); }

/* ---- clock: advance by inc ticks (Q16, 24 per beat) ----
 * Each step is decided half a step before its grid time (nudge reaches half a step early): cond (loop of the slot),
 * chance, then a plan of `ratch` hits from grid + nudge, spaced step / ratch. Hits fire in time order; a hit
 * releases the slot's sounding note, its note-off comes after gate % of the hit spacing (tie: at the next hit).
 * Muted slots decide and advance but play nothing. */
static uint32_t dt_rng = 0x2545F491u;
static int dt_rand100(void) { dt_rng = dt_rng * 1664525u + 1013904223u; return (int)((dt_rng >> 16) % 100); }

static void dt_decide(dt_run_t *r, const dt_cfg_t *q, int32_t s, uint64_t grid)
{
    const dt_step_t *t = &q->st[s % q->len];
    if (s == r->skip) { r->skip = -1; return; }          /* just live-recorded (already sounded) */
    if (!t->vel) return;
    if (t->cond) { int a = dt_cond_ab[t->cond][0], m = dt_cond_ab[t->cond][1]; if ((s / q->len) % m != a - 1) return; }
    if (t->chance < 100 && dt_rand100() >= t->chance) return;
    dt_plan_t *p = !r->p[0].on ? &r->p[0] : !r->p[1].on ? &r->p[1] : r->p[0].t0 < r->p[1].t0 ? &r->p[0] : &r->p[1];
    uint64_t L = (uint64_t)DT_RATE << 16;
    int64_t t0 = (int64_t)grid + (int64_t)t->nudge * (1 << 14);       /* nudge: 1/96 beat = 1/4 tick */
    p->t0 = t0 < 0 ? 0 : (uint64_t)t0; p->sub = L / t->ratch;
    p->ratch = t->ratch; p->j = 0; p->note = t->note; p->key = t->key; p->vel = t->vel; p->gate = t->gate;
    p->ramp = t->ramp; p->s = s; p->on = 1;
    for (int i = 0; i < DT_NLK; i++) p->fx[i] = t->fx[i];
}

static void dt_advance(int pos, uint64_t inc)
{
    dt_bank_t *b = &dt[pos & 1];
    uint64_t t1 = b->tick_q16;                         /* this tick's own position: events due now fire now */
    b->tick_q16 = t1 + inc;                            /* (an external clock delivers its 4 ticks in a burst; judging */
                                                       /*  at the end of the tick fired steps on the previous clock) */
    if (!b->init) return;
    const uint64_t L = (uint64_t)DT_RATE << 16;
    for (int k = 0; k < DT_SLOTS; k++) {
        dt_run_t *r = &b->rt[k];
        if (!r->running) continue;
        const dt_cfg_t *q = dt_q(b, k);
        uint64_t S = (uint64_t)r->start << 16;
        while (S + (uint64_t)r->planned * L <= t1 + L / 2) { dt_decide(r, q, r->planned, S + (uint64_t)r->planned * L); r->planned++; }
        r->step = (int16_t)(t1 >= S ? (int32_t)((t1 - S) / L) : -1);
        for (;;) {                                      /* due hits, earliest first */
            dt_plan_t *p = 0; uint64_t th = 0;
            for (int i = 0; i < 2; i++) if (r->p[i].on) {
                uint64_t h = r->p[i].t0 + r->p[i].j * r->p[i].sub;
                if (h <= t1 && (!p || h < th)) { p = &r->p[i]; th = h; }
            }
            if (!p) break;
            if (p->j == 0 && !q->muted) {               /* trigger start: last locks revert, new ones apply */
                if (r->locks) dt_unlock(pos & 1, k, r);
                dt_lock(pos & 1, k, r, p->fx);
                if (r->locks) r->lock_off = p->gate == DT_TIE ? ~0ull : th + L * p->gate / 100;
            }
            if (p->note == DT_NONOTE) { p->on = 0; continue; }   /* lock trigger: no note, no repeats */
            if (r->sounding) { DT_NOTE_OFF(pos & 1, r->sounding - 1); r->sounding = 0; }   /* a hit releases the last note */
            if (!q->muted) {
                int v = p->vel;
                if (p->ramp > 0) v = v * (p->j + 1) / p->ratch;
                else if (p->ramp < 0) v = v * (p->ratch - p->j) / p->ratch;
                int id = DT_NOTE_ON(pos & 1, p->note, p->key, v < 1 ? 1 : v);
                if (id >= 0) { r->sounding = (uint8_t)(id + 1); r->off_at = p->gate == DT_TIE ? ~0ull : th + p->sub * p->gate / 100; }
            }
            if (++p->j >= p->ratch) p->on = 0;
        }
        if (r->sounding && r->off_at <= t1) { DT_NOTE_OFF(pos & 1, r->sounding - 1); r->sounding = 0; }
        if (r->locks && r->lock_off <= t1) dt_unlock(pos & 1, k, r);
    }
}

void dt_tick(int pos, int n, uint32_t bpm_x100)       /* host: n samples at tempo bpm_x100 */
{
    dt_advance(pos, ((uint64_t)bpm_x100 * DT_PPQN << 16) / (6000ull * 44100ull) * (uint64_t)n);
}

/* ======================= screen (GUI thread) =======================
 * Top: slot numbers 1..6 (selected white + underline, others grey, muted dim; red dot = running, green dot = has
 * triggers; other slots lilac, muted grey). Grid: 4 rows of 16 dots, only the first `len` steps; empty grey, trigger green (muted slot: dim
 * green), playhead larger and white; cursor = short bar under the dot, blue (red in record mode).
 * Bottom: pattern P1..P4, note + level at the cursor; record mode: REC top right and the selected parameter of
 * the trigger at the cursor (name bright on page 2 = select, value bright on page 1 = edit).
 * The level of the trigger at the cursor follows its note (red in record mode, where red changes it). */
#include "dots_geom.h"
#include "../screens/stroke_font.h"

static void dline(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, p = { x1, y1 };
    op1_raster_line(r, &a, &p, w, c);
}
static void ddot(void *r, int32_t x, int32_t y, int32_t w, const uint8_t *c)   /* as euclid: two crossing bars */
{
    int32_t h = w / 2, t = w * 7 / 10;
    dline(r, x - h, y, x + h, y, t, c);
    dline(r, x, y - h, x, y + h, t, c);
}
static char *dnum(char *p, int v)
{
    if (v >= 100) *p++ = (char)('0' + v / 100);
    if (v >= 10) *p++ = (char)('0' + v / 10 % 10);
    *p++ = (char)('0' + v % 10); *p = 0; return p;
}
static void dnote(char *p, int n)
{
    static const char nm[12][3] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    for (const char *s = nm[n % 12]; *s; ) *p++ = *s++;
    int o = n / 12 - 1; if (o < 0) { *p++ = '-'; o = -o; }
    dnum(p, o);
}
static int32_t dt_x(int i) { return OP1_Q10(DT_GRID_X0 + i % 16 * DT_GRID_DX + (i % 16 / 4) * DT_GRID_GAP); }
static int32_t dt_y(int i) { return OP1_Q10(DT_GRID_Y0 + i / 16 * DT_GRID_DY); }

static void dt_screen_draw(void *r, int pos)
{
    dt_bank_t *b = &dt[pos & 1]; dt_bank_init(b);
    for (int k = 0; k < DT_SLOTS; k++) {                /* slot row */
        const dt_cfg_t *q = dt_q(b, k);
        int32_t x = OP1_Q10(DT_SLOT_X0 + k * DT_SLOT_DX), y = OP1_Q10(DT_SLOT_Y);
        char t[2] = { (char)('1' + k), 0 };
        sf_text(r, x, y, OP1_Q10(2), t, 0x400, k == b->sel ? dt_white : q->muted ? dt_grey : dt_lilac);
        if (k == b->sel) dline(r, x - OP1_Q10(1), y + OP1_Q10(15), x + OP1_Q10(9), y + OP1_Q10(15), 0x400, dt_white);
        int any = 0; for (int i = 0; i < q->len; i++) any |= q->st[i].vel;
        if (any) ddot(r, x + OP1_Q10(13), y + OP1_Q10(9), 0x800, q->muted ? dt_dimgreen : dt_green);
        if (b->rt[k].running) ddot(r, x + OP1_Q10(13), y + OP1_Q10(3), 0x800, dt_col[3]);
    }
    int k = b->sel; const dt_cfg_t *q = dt_q(b, k); const dt_run_t *rt = &b->rt[k];
    int play = rt->running && rt->step >= 0 ? rt->step % q->len : -1;
    for (int i = 0; i < q->len; i++) {                  /* grid */
        const dt_step_t *s = &q->st[i];
        int32_t x = dt_x(i), y = dt_y(i);
        int lk = s->vel && s->note == DT_NONOTE, any = 0;
        for (int f = 0; f < DT_NLK; f++) any |= s->fx[f];
        const uint8_t *c = q->muted ? dt_dimgreen : dt_green;
        if (i == play) c = s->vel && !q->muted ? dt_white : dt_grey;
        if (lk) {                                       /* lock trigger: hollow square */
            int32_t h = i == play ? OP1_Q10(4) : OP1_Q10(3), w = 0x500;
            dline(r, x - h, y - h, x + h, y - h, w, c); dline(r, x - h, y + h, x + h, y + h, w, c);
            dline(r, x - h, y - h + w / 2, x - h, y + h - w / 2, w, c); dline(r, x + h, y - h + w / 2, x + h, y + h - w / 2, w, c);
        } else if (i == play) ddot(r, x, y, 0x1C00, c);
        else if (s->vel) ddot(r, x, y, 0x1400, c);
        else ddot(r, x, y, 0x900, dt_grey);
        if (s->vel && !lk && any)                       /* note trigger with locks: tick above */
            dline(r, x - OP1_Q10(2), y - OP1_Q10(6), x + OP1_Q10(2), y - OP1_Q10(6), 0x500, dt_white);
        if (i == b->cur[k] && !(b->rec && b->live && rt->running))
            dline(r, x - OP1_Q10(5), y + OP1_Q10(9), x + OP1_Q10(5), y + OP1_Q10(9), 0x600, b->rec ? dt_col[3] : dt_col[0]);
    }
    char t[12];                                         /* info row */
    t[0] = 'P'; t[1] = (char)('1' + b->pat); t[2] = 0;
    sf_text(r, OP1_Q10(DT_PAT_X), OP1_Q10(DT_INFO_Y), OP1_Q10(2), t, 0x400, dt_white);
    const dt_step_t *s = &q->st[b->cur[k]];
    int lks = s->vel && s->note == DT_NONOTE;
    if (lks) { t[0] = 'L'; t[1] = 'K'; t[2] = 0; }
    else if (s->vel) dnote(t, s->note); else { t[0] = '-'; t[1] = '-'; t[2] = 0; }
    int32_t nx = sf_text(r, OP1_Q10(DT_NOTE_X), OP1_Q10(DT_INFO_Y), OP1_Q10(2), t, 0x400, s->vel ? dt_green : dt_grey);
    if (s->vel && !lks) { dnum(t, s->vel); sf_text(r, nx + OP1_Q10(10), OP1_Q10(DT_INFO_Y), OP1_Q10(2), t, 0x400, b->rec ? dt_col[3] : dt_grey); }
    if (!b->rec)                                        /* recording off: the selected type, dim */
        sf_text(r, OP1_Q10(b->live ? DT_REC_X - 12 : DT_REC_X), OP1_Q10(DT_SLOT_Y), OP1_Q10(2),
                b->live ? "LIVE" : "REC", 0x400, dt_grey);
    if (b->rec) {                                       /* REC top right; parameter of the trigger at the cursor */
        sf_text(r, OP1_Q10(b->live ? DT_REC_X - 12 : DT_REC_X), OP1_Q10(DT_SLOT_Y), OP1_Q10(2),
                b->live ? "LIVE" : "REC", 0x400, dt_col[3]);
        static const char *const names[DT_NPARAMS] = { "GATE", "RATCH", "CHANCE", "COND", "NUDGE", "RAMP",
                                                       "FX1", "FX2", "FX3", "FX4", "SY1", "SY2", "SY3", "SY4" };
        int fxp = b->par >= DT_P_FX1;                   /* lock names in their knob colour on page 2 */
        int32_t px = sf_text(r, OP1_Q10(DT_PAR_X), OP1_Q10(DT_INFO_Y), OP1_Q10(2), names[b->par], 0x400,
                             b->page ? (fxp ? dt_col[(b->par - DT_P_FX1) & 3] : dt_white) : dt_grey);
        char *e = t; t[0] = '-'; t[1] = '-'; t[2] = 0;
        if (s->vel) switch (b->par) {
            case DT_P_GATE:   if (s->gate == DT_TIE) { t[0] = 'T'; t[1] = 'I'; t[2] = 'E'; t[3] = 0; }
                              else { e = dnum(t, s->gate); e[0] = '%'; e[1] = 0; } break;
            case DT_P_RATCH:  dnum(t, s->ratch); break;
            case DT_P_CHANCE: e = dnum(t, s->chance); e[0] = '%'; e[1] = 0; break;
            case DT_P_COND:   if (!s->cond) { t[0] = 'A'; t[1] = 'L'; t[2] = 'L'; t[3] = 0; }
                              else { t[0] = (char)('0' + dt_cond_ab[s->cond][0]); t[1] = ':';
                                     t[2] = (char)('0' + dt_cond_ab[s->cond][1]); t[3] = 0; } break;
            case DT_P_NUDGE:  t[0] = s->nudge < 0 ? '-' : '+'; dnum(t + 1, s->nudge < 0 ? -s->nudge : s->nudge);
                              if (!s->nudge) { t[0] = '0'; t[1] = 0; } break;
            case DT_P_RAMP:   t[0] = s->ramp > 0 ? 'U' : s->ramp < 0 ? 'D' : 'O'; t[1] = s->ramp > 0 ? 'P' : s->ramp < 0 ? 'N' : 'F';
                              t[2] = s->ramp ? 0 : 'F'; t[3] = 0; break;
            default: { int v = s->fx[b->par - DT_P_FX1];
                       if (v) { e = dnum(t, v - 1); e[0] = '%'; e[1] = 0; } } break;
        }
        sf_text(r, px + OP1_Q10(8), OP1_Q10(DT_INFO_Y), OP1_Q10(2), t, 0x400, b->page ? dt_grey : dt_white);
    }
}

static void dt_screen_update(void *scr, int dt_ms, int pos)
{
    op1_screen_draw(scr, dt_ms);
    dt_screen_draw(op1_raster(), pos);
}

void dt_all_off(int pos)
{
    for (int k = 0; k < DT_SLOTS; k++) dt_silence(pos & 1, &dt[pos & 1].rt[k]);
}

/* MIDI START (dispatcher.c midi_start_hook): running slots restart at step 0 on the next clock tick */
void dt_midi_start(void)
{
    for (int pos = 0; pos < 2; pos++) {
        dt_bank_t *b = &dt[pos];
        b->tick_q16 = 0;
        for (int k = 0; k < DT_SLOTS; k++) {
            dt_run_t *r = &b->rt[k];
            if (!r->running) continue;
            dt_silence(pos, r);
            dt_start(b, r, 0);
        }
    }
}

#ifndef OP1_HOST
/* ======================= firmware player (extra sequencer "dots", runs as type 3 + flag, like euclid) ========
 * DB row seq_types "dots"; SeqIds::from_name "dots" -> type 3 with dt_active[mode] = 1 (dispatcher.c); create_player
 * case 3 -> seq_player_get (dispatcher.c) -> dt_player_get. Player vtable as euclid's (ok_player layout). */
#define DT_MODE() (*(volatile int *)0x01DE4C40 == 1)
extern volatile uint8_t eu_rec_play;                    /* seq/euclid.c, handled in key_event_hook */
#define DT_SYNTH_OCT() (((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7])
#define DT_DRUM_OCT()  (((const int *)0x01DFE500)[*(volatile int *)0xFF8016D8 & 7])
/* parameter locks on the instrument FX of the mode (docs/17): knob words synth 0xFF801444 / drum 0xFF8015C4 + slot*16,
 * FX type synth 0xFF801734[slot] / drum 0xFF801754[slot], raw range per type/knob in the table 0x01DE5C50 + type*0x30 +
 * knob*6 {min, max, ?} (filled at boot; read_fx_param asserts outside it, so values always stay in [min, max]).
 * Synth engine locks (lock index 4..7, synth mode): knob words 0xFF8013C4 + slot*16, engine 0xFF8016E4[slot], range
 * 0x01DE5890 + engine*0x40 + knob*8 {min, max, scale, mode} (as midi/mx.c; mode 2 / 3 = linked sample start / end:
 * not locked). Only the live word is written, not the patch (set_knobs), so a lock is normally not saved with the
 * sound (as the FX locks: a patch save while a lock holds, e.g. after a hand turn, may store it; docs/17); the base is
 * restored the same way. The words are reloaded from the patch on an instrument switch. */
static int16_t *dt_lk_ptr[2][DT_NLK];
static int16_t  dt_lk_base[2][DT_NLK];
static int      dt_lk_eng[DT_NSY], dt_lk_slot[DT_NSY];  /* engine / slot of the synth word when its base was taken */
static void dt_fw_fx_lock(int pos, int knob, int pc)
{
    int slot = *(volatile int *)(pos ? 0xFF8016D8 : 0xFF8016D4) & 7;
    int16_t *w; int lo, hi, eng = 0;
    if (knob >= DT_NFX) {                               /* synth engine knob */
        if (pos) return;
        eng = ((volatile int *)0xFF8016E4)[slot];
        if (eng < 0 || eng > 31) return;
        const int16_t *rg = (const int16_t *)(0x01DE5890 + eng * 0x40 + (knob - DT_NFX) * 8);
        if (rg[3] != 0 && rg[3] != 1) return;
        lo = rg[0]; hi = rg[1];
        w = (int16_t *)(0xFF8013C4 + slot * 16) + (knob - DT_NFX);
    } else {
        int type = ((volatile int *)(pos ? 0xFF801754 : 0xFF801734))[slot];
        if (type < 0 || type > 7) return;
        const int16_t *rg = (const int16_t *)(0x01DE5C50 + type * 0x30 + knob * 6);
        lo = rg[0]; hi = rg[1];
        w = (int16_t *)((pos ? 0xFF8015C4 : 0xFF801444) + slot * 16) + knob;
    }
    if (hi <= lo) return;                               /* table not filled */
    if (dt_lk_ptr[pos][knob] && (dt_lk_ptr[pos][knob] != w || (knob >= DT_NFX && dt_lk_eng[knob - DT_NFX] != eng)))
        dt_lk_ptr[pos][knob] = 0;                       /* another slot / engine since: the old base is stale */
    if (!dt_lk_ptr[pos][knob]) {                        /* first lock: keep base */
        dt_lk_ptr[pos][knob] = w; dt_lk_base[pos][knob] = *w;
        if (knob >= DT_NFX) { dt_lk_eng[knob - DT_NFX] = eng; dt_lk_slot[knob - DT_NFX] = slot; }
    }
    *w = (int16_t)(lo + (hi - lo) * pc / 100);
}
static void dt_fw_fx_restore(int pos, int knob)
{
    if (!dt_lk_ptr[pos][knob]) return;
    if (knob < DT_NFX || ((volatile int *)0xFF8016E4)[dt_lk_slot[knob - DT_NFX]] == dt_lk_eng[knob - DT_NFX])
        *dt_lk_ptr[pos][knob] = dt_lk_base[pos][knob];  /* (the engine of that slot changed meanwhile: base no longer valid) */
    dt_lk_ptr[pos][knob] = 0;
}

static int dt_nop(void *self) { (void)self; return 0; }
static int dt_key_down(void *self, int key)
{
    (void)self;
    if (key < 0 || key > 23) return 0;
    if (*(volatile uint8_t *)0x01DD7F78) {                              /* Shift + key (any screen) */
        int h = dt_shift_key(DT_MODE(), key);
        int tape = *(volatile int *)0x018C17D4;                          /* 3 / 6 = REC held (armed) */
        if (h == 2 && (tape == 3 || tape == 6)) eu_rec_play = 1;         /* dispatcher presses PLAY (as euclid) */
        if (h) return 1;
    }
    if (*(volatile int *)0x018B65C8 == 0x2D) {                          /* dots screen shown: record */
        int note = key + 0x35 + (DT_MODE() ? DT_DRUM_OCT() : DT_SYNTH_OCT()) * 12;
        if (dt_live_down(DT_MODE(), note, key)) return 1;               /* LIVE: consumed, sounded by us */
        dt_key(DT_MODE(), note, key);
    }
    return 0;                                           /* not consumed: the key also plays its note */
}
static void dt_key_up(void *self, int key) { (void)self; dt_live_up(DT_MODE(), key); }   /* consumed keys only */
static void dt_reset(void *self) { (void)self; dt_all_off(DT_MODE()); }
static void dt_ptick(void *self, int counter) { (void)self; (void)counter; dt_advance(DT_MODE(), 1u << 14); }   /* 1/96 beat */
static void dt_stop(void *self) { (void)self; dt_all_off(0); dt_all_off(1); }
static const uint32_t dt_vt[16] = {
    0, 0,  0, (uint32_t)dt_nop,  0, (uint32_t)dt_key_down,  0, (uint32_t)dt_key_up,
    0, (uint32_t)dt_reset,  0, (uint32_t)dt_ptick,  0, (uint32_t)dt_nop,  0, (uint32_t)dt_stop };
static struct { const uint32_t *vt; uint32_t pad[7]; } dt_player = { dt_vt, { 0 } };
volatile uint8_t dt_active[2];                          /* dots selected (synth, drum) */

/* persistence: background saver + DB as euclid (seq/euclid.c), table dotsseq, one row per (mode, pattern) */
#define DT_BG_REGISTER ((void (*)(void *))0x0193F010)
#define DT_BG_DIRTY    ((void (*)(void *))0x0193EF3C)
#define DT_DB_HANDLE   ((void *(*)(void))0x0196DD44)
#define DT_DB_EXEC     ((void (*)(void *, const char *))0x0196F950)
#define DT_DB_GET      ((void (*)(void *, void *, const char *))0x0196FBD0)
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } dt_dstr;
static const char DT_CREATE[] = "CREATE TABLE IF NOT EXISTS dotsseq (id INTEGER PRIMARY KEY, data TEXT NOT NULL)";
static char dt_sql[64 + DT_TEXT_LEN];                    /* < 8 KB: the firmware's SQL format buffer */
typedef char dt_sql_fits[sizeof dt_sql < 0x2000 - 64 ? 1 : -1];
static void dt_bg_nop(void *self) { (void)self; }
static void dt_bg_save(void *self)                      /* saver thread */
{
    (void)self;
    void *db = DT_DB_HANDLE();
    DT_DB_EXEC(db, DT_CREATE);
    for (int id = 0; id < 6 * DT_PATS; id++) {          /* id = mode * 4 + pattern (steps), + 8 (fx locks), + 16 (synth locks) */
        static const char head[] = "INSERT OR REPLACE INTO dotsseq VALUES (";
        char *o = dt_sql;
        for (const char *h = head; *h; h++) *o++ = *h;
        if (id >= 10) *o++ = (char)('0' + id / 10);
        *o++ = (char)('0' + id % 10); *o++ = ','; *o++ = '\'';
        int m = id % (2 * DT_PATS);
        o += id < 2 * DT_PATS ? dt_serialize(m / DT_PATS, m % DT_PATS, o)
           : id < 4 * DT_PATS ? dt_serialize_locks(m / DT_PATS, m % DT_PATS, o) : dt_serialize_sy(m / DT_PATS, m % DT_PATS, o);
        *o++ = '\''; *o++ = ')'; *o = 0;
        op1_db_write(db, dt_sql);                       /* (no assert: long statement, docs/17) */
    }
}
static const uint32_t dt_bg_vt[6] = { 0, 0, 0, (uint32_t)dt_bg_nop, 0, (uint32_t)dt_bg_save };
static struct { uint8_t dirty, saving, pad[2]; const uint32_t *vt; uint32_t more[2]; } dt_bg;
static uint8_t dt_bg_ready;
static void dt_changed(void) { if (dt_bg_ready) DT_BG_DIRTY(&dt_bg); }
static void dt_load(void)
{
    void *db = DT_DB_HANDLE();
    DT_DB_EXEC(db, DT_CREATE);
    for (int id = 0; id < 6 * DT_PATS; id++) {          /* steps first (ids 0..7), then their locks (8..15, 16..23) */
        static char q[] = "SELECT data FROM dotsseq WHERE id=00";
        q[sizeof q - 3] = (char)('0' + id / 10); q[sizeof q - 2] = (char)('0' + id % 10);
        dt_dstr s = { 0 };
        DT_DB_GET(&s, db, q);
        const char *p = s.cap > 15 ? s.u.ptr : s.u.buf;
        int m = id % (2 * DT_PATS);
        if (s.size) {                                   /* (heap copy, once per boot, not freed) */
            if (id < 2 * DT_PATS) dt_parse(m / DT_PATS, m % DT_PATS, p, (int)s.size);
            else if (id < 4 * DT_PATS) dt_parse_locks(m / DT_PATS, m % DT_PATS, p, (int)s.size);
            else dt_parse_sy(m / DT_PATS, m % DT_PATS, p, (int)s.size);
        }
    }
    DT_BG_REGISTER(&dt_bg);
    dt_bg.vt = dt_bg_vt;
    dt_bg_ready = 1;
}
void *dt_player_get(void)
{
    if (!dt_bg_ready) dt_load();
    return &dt_player;
}

static void dt_scr_init(void *scr) { (void)scr; }
static void dt_scr_update(void *scr, int dt_ms) { dt_screen_update(scr, dt_ms, DT_MODE()); }
const op1_screen_t SCREEN_dots = { dt_scr_init, dt_scr_update };

/* plugin interface (docs/44): clipboard (Shift: whole pattern), presses, REC = step / live record; Shift + white
 * selects the slot */
static int dt_pkey(int m, int code, int pressed, int knob, int shift)
{
    if (code >= 0x0A && code <= 0x0C) { if (pressed) dt_clip(m, code == 0x0A ? 0 : code == 0x0B ? 1 : 2, shift); return 1; }
    if (knob >= 0) { if (pressed) dt_press(m, knob); return 1; }
    if (code == 0x21) { if (pressed) dt_rec_key(m); return 1; }
    return 0;
}
static void dt_pturn(int m, int knob, int delta, int shift)
{
    if (knob == 2 && shift) dt_select(m, delta);
    else dt_turn(m, knob, delta, shift);
}
const op1_seq_t SEQ_dots = { dt_active, dt_player_get, dt_pkey, dt_pturn, dt_midi_start, 0 };
#endif
