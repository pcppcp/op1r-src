/* "peal" sequencer: English change ringing (synth and drum mode).
 *
 * Lore: change ringing grew in 17th-century England (Fabian Stedman, Tintinnalogia 1668 / Campanalogia 1677). The
 * bells ring in rows, each bell once per row; a "method" swaps adjacent bells from row to row ("changes"), never
 * repeating a row, until the bells come back to rounds (1 2 3 ... n). A ringer learns the path of a bell through
 * the rows (its "blue line"). Rows alternate handstroke / backstroke; before each handstroke row there is a beat of
 * silence, the handstroke gap.
 *
 * Playing (like the stock sequencers; any screen while peal is the mode's sequencer): hold 3..8 keys (a chord) and
 * they ring as the bells while held. Sorted by pitch, the highest is the treble (bell 1), the lowest the tenor, as in a
 * tower. Ringing starts a moment after the third key (PL_CHORD_TICKS: the rest of a chord still makes the first row);
 * keys added while held join (up to 8). Releasing keys does not drop bells; when the last key is up the bells finish
 * the row they are ringing and stop. One or two keys alone do nothing (silent). While ringing, a changed chord takes
 * effect at the next row (bells change only between rows, as in a tower; it also lets a chord gather): the same number
 * of bells -> just the new pitches, the method goes on (like a transposition); another number -> rounds.
 *   Shift + keys    latch: the peal goes on after the keys are released (Shift held while pressing a chord key, or
 *                   while letting the last key go). Latched, a new chord with Shift changes the bells and stays
 *                   latched; a chord without Shift takes over (rings while held, stops after release: the stock way
 *                   to unlatch). Shift + C# / Shift + red left stop it.
 * The last chord stays the bell set (saved) for Shift + C# / Shift + red.
 * Each row plays all bells in its order as successive steps; then the method permutes the row. Handstroke gap: one
 * step rest after every backstroke row (toggle). Every row is generated from place notation (one change = the set
 * of places where a bell lies still, all other bells swap with their neighbour):
 *   Plain Hunt   n even: x.1n ...         n odd: n.1 ...                       (2n rows, no calls)
 *   Plain Bob    n even: (x.1n) x n, last 12; bob 14, single 1234           (Minimus 24, Minor 60, Major 112 rows)
 *                n odd:  (n.1) x n, last 12n; bob 14n, single 123 / 12347   (Doubles 40, Triples 84)
 *   Grandsire    3.1.n.1.n.1 ... n.1 (odd n); bob 3.1, single 3.123 for the last two changes of the lead
 *                (complib: 3,1.5.1.5.1 / 3,1.7.1.7.1.7.1; Doubles 30, Triples 70 rows)
 *   Stedman      3.1.n.3.1.3.1.3.n.1.3.1 (odd n, sixes); Triples: bob n-2, single (n-2)(n-1)n at the six ends;
 *                Doubles: no bob, the single 345 in the middle of a six (Doubles 60, Triples 84 rows)
 * Grandsire and Stedman are odd-bell methods: with an even number of bells the tenor rings behind as a cover (always
 * last), as in practice. With 3-4 bells they ring plain hunt on three (+ cover); Plain Bob on 3 = plain hunt.
 * Calls are made at the lead end (Stedman: the six end). The extent counter shows rows rung of the w! possible rows of
 * the w working bells; "ROUNDS" when the bells come round (the counter starts again; ringing goes on).
 *
 * Controls (peal screen shown; Shift = shift key held):
 *   blue turn    method: Plain Hunt / Plain Bob / Grandsire / Stedman (restarts from rounds at the next row)
 *   blue press   handstroke gap on / off
 *   green turn   rate 1/4 1/8 1/8T 1/16 1/16T 1/32 (one bell per step)
 *   green press  "That's all": stop when the bells next come round
 *   white turn   automatic call probability 0..100 % per call position
 *   white press  call "Bob!" for the next call position; Shift + white press: "Single!"
 *   red turn     rough striking 0..100 %: random lateness (up to half a step) and loudness of each blow
 *   red press    "Look to": back to rounds at the next row
 *   Shift + blue octave -2..+2 (synth) · Shift + green gate 10..100 % · Shift + white level 1..127 ·
 *   Shift + red  right = start (latched, last bells), left = stop
 *   left / right keyboard octave (as euclid)
 * Any screen while peal is the mode's sequencer: Shift + C# (key 20) start (latched, last bells) / stop (with REC held
 * also PLAY, as euclid).
 * Saved (bg_saver, DB table pealseq, one row per mode): bells, method, rate, gate, gap, call %, rough, octave, level.
 * Clock as euclid (24 ticks per beat; Shift + C# starts at once, Shift + red on the next step boundary, keys after
 * PL_CHORD_TICKS; comes back stopped).
 *
 * Platform independent (host test harness/peal_test.c); firmware glue at the end and in dispatcher.c.
 * Copyright (C) 2026 ultralazr. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "../op1_synth.h"
#include "seq_text.h"

#define PL_MAXB  8
#define PL_HIST  9                                      /* rows shown */
#define PL_PPQN  24
#define PL_CHORD_TICKS 2                                /* key start delay (1/12 beat): the chord gathers */
enum { PL_HUNT, PL_BOB, PL_GRANDSIRE, PL_STEDMAN, PL_METHODS };
enum { PL_RATES = 6 };
static const uint8_t pl_rate_ticks[PL_RATES] = { 24, 12, 8, 6, 4, 3 };   /* 1/4 1/8 1/8T 1/16 1/16T 1/32 */
enum { PL_NONE, PL_CALL_BOB, PL_CALL_SINGLE, PL_ROUNDS };

typedef struct {                                        /* saved */
    uint8_t n;                                          /* bells: 0 (none yet) or 3..8 */
    uint8_t note[PL_MAXB], key[PL_MAXB];                /* bell 1 (treble, highest) first */
    uint8_t method, rate, gate, gap, callp, rough, oct, vel;
} pl_cfg_t;

typedef struct {                                        /* a method at a stage, as place masks (bit i = place i+1) */
    uint8_t  w, len, ncall, has_bob, has_single;
    int8_t   cp[2];                                     /* call positions: first change index replaced (-1: none) */
    uint16_t pn[16];
    uint16_t bob[2], single[2];
} pl_method_t;

typedef struct {
    uint8_t  init, running, run_n, pos, gap_pending, reset_pending, stand;
    uint8_t  row[PL_MAXB];
    uint8_t  ci;                                        /* next change of the lead */
    uint8_t  call_req, call_now, call_left;             /* requested call, call in progress, changes left */
    uint8_t  flash, flash_rows;                         /* screen: BOB / SINGLE / ROUNDS */
    uint32_t rows, last_course, rowabs;                 /* rows since rounds, rows of the last course, rows rung */
    uint32_t start; int32_t abs;
    uint64_t tick_q16, strike_at, off_at;
    uint8_t  strike, sounding;                          /* pending bell (0 none), note id + 1 */
    uint8_t  strike_vel;
    int32_t  acc[4];
    uint32_t rng;
    uint32_t held;                                      /* keys down that ring the peal */
    uint8_t  pend_n, pend_note[PL_MAXB], pend_key[PL_MAXB];   /* the chord being held */
    uint8_t  latched, rel_pend, chord_pend;             /* rings without keys / keys up: stop at the row end /
                                                           a new chord for the next row */
    uint8_t  hist[PL_HIST][PL_MAXB], hist_lh[PL_HIST], hist_n;   /* hist[0] = newest */
    pl_method_t m;
    pl_cfg_t c;
} pl_bank_t;

static pl_bank_t pl[2];                                 /* 0 synth, 1 drum */

#ifdef OP1_HOST
int host_pl_changed;
#define PL_CHANGED() (host_pl_changed++)
void host_pl_note_on(int pos, int note, int vel);
void host_pl_note_off(int pos, int note);
#define PL_NOTE_ON(pos, n, k, v) (host_pl_note_on((pos), (n), (v)), (n))
#define PL_NOTE_OFF(pos, n)      host_pl_note_off((pos), (n))
#else
static void pl_changed(void);
#define PL_CHANGED() pl_changed()
int  eu_fw_note_on(int pos, int note, int key, int vel);   /* seq/euclid.c: VoiceCtl note on / off */
void eu_fw_note_off(int midi);
#define PL_NOTE_ON(pos, n, k, v) eu_fw_note_on((pos), (n), (k), (v))
#define PL_NOTE_OFF(pos, n)      eu_fw_note_off(n)
#endif

static int pl_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static uint32_t pl_rand(pl_bank_t *b) { b->rng = b->rng * 1664525u + 1013904223u; return b->rng >> 16; }

/* ---- methods ---- */
#define P(k) ((uint16_t)(1u << ((k) - 1)))              /* place k (1-based) */
static void pl_build(pl_method_t *m, int method, int n)
{
    for (int i = 0; i < 16; i++) m->pn[i] = 0;
    m->cp[0] = m->cp[1] = -1; m->ncall = 1; m->has_bob = m->has_single = 0;
    m->bob[0] = m->bob[1] = m->single[0] = m->single[1] = 0;
    int w = n;
    if (method == PL_GRANDSIRE || method == PL_STEDMAN) { if (!(w & 1)) w--; if (w < 5) { w = 3; method = PL_HUNT; } }
    if (method == PL_BOB && w < 4) method = PL_HUNT;
    if (w < 2) w = 2;
    m->w = (uint8_t)w;
    uint16_t N = P(w);
    if (method == PL_HUNT || method == PL_BOB) {
        m->len = (uint8_t)(2 * w);
        for (int i = 0; i < w; i++) {
            m->pn[2 * i] = w & 1 ? N : 0;               /* even: x, odd: n */
            m->pn[2 * i + 1] = w & 1 ? P(1) : (uint16_t)(P(1) | N);
        }
        if (method == PL_BOB) {
            int le = 2 * w - 1;
            m->pn[le] = w & 1 ? (uint16_t)(P(1) | P(2) | N) : (uint16_t)(P(1) | P(2));
            m->cp[0] = (int8_t)le; m->has_bob = m->has_single = 1;
            m->bob[0] = w & 1 ? (uint16_t)(P(1) | P(4) | N) : (uint16_t)(P(1) | P(4));
            m->single[0] = w & 1 ? (uint16_t)(P(1) | P(2) | P(3) | (w >= 7 ? P(4) | N : 0)) : (uint16_t)(P(1) | P(2) | P(3) | P(4));
        }
    } else if (method == PL_GRANDSIRE) {
        m->len = (uint8_t)(2 * w);
        m->pn[0] = P(3);
        for (int i = 1; i < 2 * w; i++) m->pn[i] = i & 1 ? P(1) : N;
        m->cp[0] = (int8_t)(2 * w - 2); m->ncall = 2; m->has_bob = m->has_single = 1;
        m->bob[0] = P(3); m->bob[1] = P(1);
        m->single[0] = P(3); m->single[1] = (uint16_t)(P(1) | P(2) | P(3));
    } else {                                            /* Stedman */
        static const uint8_t st[12] = { 3, 1, 0, 3, 1, 3, 1, 3, 0, 1, 3, 1 };   /* 0 = n */
        m->len = 12;
        for (int i = 0; i < 12; i++) m->pn[i] = st[i] ? P(st[i]) : N;
        m->has_single = 1;
        if (w >= 7) {                                   /* calls at the six ends */
            m->cp[0] = 2; m->cp[1] = 8; m->has_bob = 1;
            m->bob[0] = P(w - 2); m->single[0] = (uint16_t)(P(w - 2) | P(w - 1) | N);
        } else {                                        /* Doubles: single 345 in the middle of a six (for 3) */
            m->cp[0] = 5; m->single[0] = (uint16_t)(P(3) | P(4) | P(5));
        }
    }
}

/* one change on the working bells: places in pn lie still, other adjacent pairs swap */
static void pl_apply(uint8_t *row, int w, uint16_t pn)
{
    for (int i = 0; i < w; ) {
        if ((pn >> i) & 1) { i++; continue; }
        if (i + 1 < w && !((pn >> (i + 1)) & 1)) { uint8_t t = row[i]; row[i] = row[i + 1]; row[i + 1] = t; i += 2; }
        else i++;                                       /* (malformed notation: lie still) */
    }
}

static int pl_is_rounds(const uint8_t *row, int n) { for (int i = 0; i < n; i++) if (row[i] != i + 1) return 0; return 1; }

static void pl_hist_push(pl_bank_t *b, int leadhead)
{
    for (int k = PL_HIST - 1; k > 0; k--) {
        for (int i = 0; i < PL_MAXB; i++) b->hist[k][i] = b->hist[k - 1][i];
        b->hist_lh[k] = b->hist_lh[k - 1];
    }
    for (int i = 0; i < PL_MAXB; i++) b->hist[0][i] = b->row[i];
    b->hist_lh[0] = (uint8_t)leadhead;
    if (b->hist_n < PL_HIST) b->hist_n++;
}

/* back to rounds with the current bells / method */
static void pl_rounds(pl_bank_t *b)
{
    b->run_n = b->c.n;
    pl_build(&b->m, b->c.method, b->run_n ? b->run_n : 3);
    for (int i = 0; i < PL_MAXB; i++) b->row[i] = (uint8_t)(i + 1);
    b->ci = 0; b->pos = 0; b->gap_pending = 0; b->reset_pending = 0;
    b->call_now = b->call_left = 0;
    b->rows = 0; b->rowabs = 0; b->hist_n = 0;
    pl_hist_push(b, 0);
}

/* the next row: decide a call at a call position, apply the change */
static void pl_next_row(pl_bank_t *b)
{
    const pl_method_t *m = &b->m;
    uint16_t pn = m->pn[b->ci];
    if (!b->call_left && (b->ci == m->cp[0] || b->ci == m->cp[1])) {
        int c = b->call_req;
        if (!c && b->c.callp && (int)(pl_rand(b) % 100) < b->c.callp)
            c = m->has_single && (!m->has_bob || !(pl_rand(b) & 3)) ? PL_CALL_SINGLE : PL_CALL_BOB;
        if (c == PL_CALL_BOB && !m->has_bob) c = m->has_single ? PL_CALL_SINGLE : 0;
        if (c == PL_CALL_SINGLE && !m->has_single) c = m->has_bob ? PL_CALL_BOB : 0;
        b->call_req = 0;
        if (c) { b->call_now = (uint8_t)c; b->call_left = m->ncall; b->flash = (uint8_t)c; b->flash_rows = 3; }
    }
    if (b->call_left) {
        const uint16_t *cv = b->call_now == PL_CALL_BOB ? m->bob : m->single;
        pn = cv[m->ncall - b->call_left];
        b->call_left--;
    }
    pl_apply(b->row, m->w, pn);
    b->ci = (uint8_t)(b->ci + 1 >= m->len ? 0 : b->ci + 1);
    b->rows++; b->rowabs++;
    if (b->flash_rows && !--b->flash_rows) b->flash = 0;
    if (pl_is_rounds(b->row, b->run_n)) { b->last_course = b->rows; b->rows = 0; b->flash = PL_ROUNDS; b->flash_rows = 4; }
    pl_hist_push(b, b->ci == 0);
}

static void pl_commit(pl_bank_t *b);
/* one step: returns the bell to strike (1..n) or 0 (rest: handstroke gap, no bells) */
static int pl_step(pl_bank_t *b)
{
    if (b->gap_pending) { b->gap_pending = 0; return 0; }
    if (b->pos >= b->run_n) {                           /* row done */
        if (b->chord_pend) pl_commit(b);                /* a changed chord: from this row (count: rounds) */
        if (b->rel_pend) { b->stand = 2; return 0; }    /* keys released: the row is rung, stop */
        if (b->stand == 3) { b->stand = 2; return 0; }  /* "That's all": rounds rung, stand */
        if (b->reset_pending || b->run_n != b->c.n) pl_rounds(b);
        else {
            pl_next_row(b);
            if (b->stand == 1 && b->flash == PL_ROUNDS && b->flash_rows == 4) b->stand = 3;   /* ring this rounds */
        }
        b->pos = 0;
    }
    if (!b->run_n) return 0;
    int bell = b->row[b->pos++];
    if (b->pos == b->run_n && b->c.gap && (b->rowabs & 1)) b->gap_pending = 1;   /* after a backstroke */
    return bell;
}

static void pl_bank_init(pl_bank_t *b)
{
    if (b->init) return;
    b->init = 1; b->rng = 0xBE11u;
    b->c.n = 0; b->c.method = PL_BOB; b->c.rate = 3; b->c.gate = 60; b->c.gap = 1; b->c.callp = 0;
    b->c.rough = 0; b->c.oct = 2; b->c.vel = 100;
    pl_rounds(b);
}

static void pl_silence(int pos, pl_bank_t *b)
{
    (void)pos;
    if (b->sounding) { PL_NOTE_OFF(pos, b->sounding - 1); b->sounding = 0; }
}
/* when: 0 next step boundary, 1 now, 2 after PL_CHORD_TICKS (keys) */
static void pl_start(pl_bank_t *b, int when)
{
    b->rel_pend = 0;
    if (b->running) return;
    pl_rounds(b);
    uint32_t now = (uint32_t)(b->tick_q16 >> 16), r = pl_rate_ticks[b->c.rate];
    b->start = when == 1 ? now : when == 2 ? now + PL_CHORD_TICKS : (now / r + 1) * r;
    b->abs = -1; b->strike = 0; b->stand = 0; b->running = 1;
}
static void pl_stop(int pos, pl_bank_t *b)
{
    b->latched = 0; b->rel_pend = 0;
    if (!b->running) return;
    b->running = 0; b->strike = 0; b->stand = 0;
    pl_silence(pos, b);
    if (b->chord_pend) pl_commit(b);                    /* (stopped: the chord is the bell set for the next start) */
}
/* bells / method changed: stopped -> at once, running -> at the next row */
static void pl_restart(pl_bank_t *b) { if (b->running) b->reset_pending = 1; else pl_rounds(b); }

/* ---- keys (UI thread): hold a chord = the bells ring ---- */
/* the held chord (3..8 keys) becomes the bell set. Stopped or nothing rung yet: rounds now; ringing: called at a row
 * end (pl_step), same number -> new pitches, else rounds */
static void pl_commit(pl_bank_t *b)
{
    b->chord_pend = 0;
    uint8_t nt[PL_MAXB], ky[PL_MAXB];
    int n = b->pend_n;
    for (int i = 0; i < n; i++) { nt[i] = b->pend_note[i]; ky[i] = b->pend_key[i]; }
    for (int i = 1; i < n; i++)                         /* insertion sort, highest first (treble = bell 1) */
        for (int j = i; j > 0 && nt[j] > nt[j - 1]; j--) {
            uint8_t t = nt[j]; nt[j] = nt[j - 1]; nt[j - 1] = t;
            t = ky[j]; ky[j] = ky[j - 1]; ky[j - 1] = t;
        }
    int same = n == b->c.n;
    for (int i = 0; i < n && same; i++) same = nt[i] == b->c.note[i] && ky[i] == b->c.key[i];
    if (same) return;
    int count = n != b->c.n;
    b->c.n = (uint8_t)n;
    for (int i = 0; i < PL_MAXB; i++) { b->c.note[i] = i < n ? nt[i] : 0; b->c.key[i] = i < n ? ky[i] : 0; }
    if (!b->running || b->abs < 0) pl_rounds(b);       /* nothing rung yet: the whole chord rings from rounds */
    else if (count) b->reset_pending = 1;               /* (same number: the method goes on) */
    PL_CHANGED();
}
/* UI code (keys, encoders, persistence, screen) is compiled for size: code space is short. The clock (pl_advance and
 * what it calls, pl_commit, the player tick / reset / stop) stays -O2: it runs in the audio callback. */
#pragma GCC push_options
#pragma GCC optimize("Os")
/* key down (not a Shift shortcut): add it to the held chord; 3+ keys ring. shift: latch. Returns 1 (consumed). */
int pl_key_down(int pos, int note, int key, int shift)
{
    pl_bank_t *b = &pl[pos & 1];
    pl_bank_init(b);
    if (key < 0 || key > 23) return 0;
    if (!b->held) { b->pend_n = 0; b->latched = (uint8_t)(shift ? 1 : 0); }   /* a new chord (no Shift: takes over) */
    else if (shift) b->latched = 1;
    b->held |= 1u << key;
    int dup = 0;
    for (int i = 0; i < b->pend_n; i++) if (b->pend_key[i] == key) dup = 1;
    if (!dup && b->pend_n < PL_MAXB) { b->pend_note[b->pend_n] = (uint8_t)note; b->pend_key[b->pend_n] = (uint8_t)key; b->pend_n++; }
    if (b->pend_n >= 3) {
        if (!b->running || b->abs < 0) pl_commit(b);   /* first row not begun: the chord gathers into it */
        else b->chord_pend = 1;                         /* ringing: at the next row */
        pl_start(b, 2);                                 /* (running: just cancels a pending stop) */
    } else if (b->running) b->rel_pend = 0;             /* keys back on a ringing peal: it goes on */
    return 1;
}
/* key up (only keys this sequencer took); shift held now: latch. The last key up: stop at the end of the row. */
void pl_key_up(int pos, int key, int shift)
{
    pl_bank_t *b = &pl[pos & 1];
    if (key < 0 || key > 23 || !(b->held & (1u << key))) return;
    b->held &= ~(1u << key);
    if (b->held) return;
    if (shift) b->latched = 1;
    if (b->running && !b->latched) b->rel_pend = 1;
}

/* ---- encoders ---- */
static int pl_detents(pl_bank_t *b, int k, int delta)
{
    if ((b->acc[k] > 0 && delta < 0) || (b->acc[k] < 0 && delta > 0)) b->acc[k] = 0;
    b->acc[k] += delta;
    int st = b->acc[k] / 1024;
    b->acc[k] -= st * 1024;
    return st;
}
void pl_turn(int pos, int knob, int delta, int shift)
{
    pl_bank_t *b = &pl[pos & 1];
    pl_bank_init(b);
    if (knob < 0 || knob > 3) return;
    int st = pl_detents(b, knob, delta);
    if (!st) return;
    pl_cfg_t *c = &b->c;
    if (shift) {
        if (knob == 0) c->oct = (uint8_t)pl_clampi(c->oct + st, 0, 4);
        else if (knob == 1) c->gate = (uint8_t)pl_clampi(c->gate + st * 5, 10, 100);
        else if (knob == 2) c->vel = (uint8_t)pl_clampi(c->vel + st * 4, 1, 127);
        else { if (st > 0) { pl_start(b, 0); b->latched = 1; } else pl_stop(pos & 1, b); return; }
    } else if (knob == 0) {
        int mth = pl_clampi(c->method + st, 0, PL_METHODS - 1);
        if (mth == c->method) return;
        c->method = (uint8_t)mth;
        pl_restart(b);
    } else if (knob == 1) {
        int r = pl_clampi(c->rate + st, 0, PL_RATES - 1);
        if (r == c->rate) return;
        c->rate = (uint8_t)r;
        if (b->running) {                               /* keep the step grid aligned; the row goes on */
            uint32_t now = (uint32_t)(b->tick_q16 >> 16), t = pl_rate_ticks[r];
            b->start = (now / t + 1) * t; b->abs = -1;
        }
    } else if (knob == 2) c->callp = (uint8_t)pl_clampi(c->callp + st * 5, 0, 100);
    else c->rough = (uint8_t)pl_clampi(c->rough + st * 5, 0, 100);
    PL_CHANGED();
}
void pl_press(int pos, int knob, int shift)
{
    pl_bank_t *b = &pl[pos & 1];
    pl_bank_init(b);
    if (knob == 0) { b->c.gap ^= 1; b->gap_pending = 0; PL_CHANGED(); }
    else if (knob == 1) b->stand = b->running ? (uint8_t)!b->stand : 0;   /* "That's all" (again: cancel) */
    else if (knob == 2) b->call_req = shift ? PL_CALL_SINGLE : PL_CALL_BOB;
    else if (knob == 3) { b->call_req = 0; b->stand = 0; pl_restart(b); }
}
int pl_toggle(int pos)                                  /* Shift + C#: returns 1 if it started (latched) */
{
    pl_bank_t *b = &pl[pos & 1];
    pl_bank_init(b);
    if (b->running) { pl_stop(pos & 1, b); return 0; }
    pl_start(b, 1);
    b->latched = 1;
    return 1;
}
int pl_shift_key(int pos, int key) { return key == 20 ? (pl_toggle(pos) ? 2 : 1) : 0; }

#pragma GCC pop_options
/* ---- clock: advance by inc ticks (Q16, 24 per beat) ---- */
static void pl_advance(int pos, uint64_t inc)
{
    pl_bank_t *b = &pl[pos & 1];
    uint64_t t1 = b->tick_q16;
    b->tick_q16 = t1 + inc;
    if (!b->running) return;
    uint64_t r = (uint64_t)pl_rate_ticks[b->c.rate] << 16, st0 = (uint64_t)b->start << 16;
    if (t1 < st0) return;
    if (b->sounding && t1 >= b->off_at) pl_silence(pos, b);
    int32_t step = (int32_t)((t1 - st0) / r);
    if (step != b->abs) {
        b->abs = step;
        pl_silence(pos, b);
        b->strike = 0;
        int bell = pl_step(b);
        if (b->stand == 2) { pl_stop(pos & 1, b); return; }
        if (bell) {
            uint64_t at = st0 + (uint64_t)step * r;
            int v = b->c.vel;
            if (b->c.rough) {                           /* rough striking: late by up to rough % of half a step */
                uint32_t span = (uint32_t)((r >> 8) * b->c.rough / 200);   /* (in 1/256 ticks) */
                at += (uint64_t)(pl_rand(b) % (span + 1)) << 8;
                v += ((int)(pl_rand(b) % 61) - 30) * b->c.rough / 100;
            }
            b->strike = (uint8_t)bell; b->strike_at = at; b->strike_vel = (uint8_t)pl_clampi(v, 1, 127);
        }
    }
    if (b->strike && t1 >= b->strike_at) {
        int k = b->strike - 1, note = b->c.note[k] + (pos & 1 ? 0 : (b->c.oct - 2) * 12);
        b->strike = 0;
        if (note >= 0 && note <= 127) {
            int id = PL_NOTE_ON(pos & 1, note, b->c.key[k], b->strike_vel);
            if (id >= 0) { b->sounding = (uint8_t)(id + 1); b->off_at = b->strike_at + r * b->c.gate / 100; }
        }
    }
}
void pl_tick(int pos, int n, uint32_t bpm_x100)         /* host: advance by n samples at bpm_x100 */
{
    pl_advance(pos, ((uint64_t)bpm_x100 * PL_PPQN << 16) / (6000ull * 44100ull) * (uint64_t)n);
}
void pl_all_off(int pos)
{
    pl_bank_t *b = &pl[pos & 1];
    pl_silence(pos & 1, b);
    b->strike = 0;
}
void pl_midi_start(void)                                /* MIDI START: rounds again, first blow on the next tick */
{
    for (int pos = 0; pos < 2; pos++) {
        pl_bank_t *b = &pl[pos];
        b->tick_q16 = 0;
        if (!b->running) continue;
        pl_all_off(pos);
        pl_rounds(b);
        b->start = 0; b->abs = -1;
    }
}

#pragma GCC push_options
#pragma GCC optimize("Os")
/* ---- persistence text: "n,(note,key) x 8,method,rate,gate,gap,callp,rough,oct,vel" ---- */
#define PL_TEXT_N (1 + 2 * PL_MAXB + 8)
int pl_serialize(int pos, char *out, int n)
{
    pl_bank_t *b = &pl[pos & 1];
    pl_bank_init(b);
    const pl_cfg_t *c = &b->c;
    int v[PL_TEXT_N], k = 0;
    v[k++] = c->n;
    for (int i = 0; i < PL_MAXB; i++) { v[k++] = c->note[i]; v[k++] = c->key[i]; }
    v[k++] = c->method; v[k++] = c->rate; v[k++] = c->gate; v[k++] = c->gap;
    v[k++] = c->callp; v[k++] = c->rough; v[k++] = c->oct; v[k++] = c->vel;
    return sq_join(out, n, v, k);
}
int pl_parse(int pos, const char *s, int len)
{
    int v[PL_TEXT_N];
    if (sq_split(s, len, v, PL_TEXT_N) != PL_TEXT_N) return 0;
    pl_bank_t *b = &pl[pos & 1];
    pl_bank_init(b);
    pl_all_off(pos);
    pl_cfg_t *c = &b->c;
    c->n = (uint8_t)(v[0] >= 3 && v[0] <= PL_MAXB ? v[0] : 0);
    for (int i = 0; i < PL_MAXB; i++) { c->note[i] = (uint8_t)pl_clampi(v[1 + 2 * i], 0, 127); c->key[i] = (uint8_t)pl_clampi(v[2 + 2 * i], 0, 23); }
    const int *f = &v[1 + 2 * PL_MAXB];
    c->method = (uint8_t)pl_clampi(f[0], 0, PL_METHODS - 1); c->rate = (uint8_t)pl_clampi(f[1], 0, PL_RATES - 1);
    c->gate = (uint8_t)pl_clampi(f[2], 10, 100); c->gap = (uint8_t)(f[3] & 1);
    c->callp = (uint8_t)pl_clampi(f[4], 0, 100); c->rough = (uint8_t)pl_clampi(f[5], 0, 100);
    c->oct = (uint8_t)pl_clampi(f[6], 0, 4); c->vel = (uint8_t)pl_clampi(f[7], 1, 127);
    b->running = 0; b->latched = 0; b->rel_pend = 0; b->chord_pend = 0; b->held = 0;
    pl_rounds(b);
    return 1;
}

/* ======================= screen (GUI thread) =======================
 * Left: the last rows as bell numbers, newest at the bottom, the bell just struck white. The treble's path is drawn
 * as a blue line through its places (its numbers are left out, as on a method diagram); a thin grey line above
 * each lead head. Right: method, stage (+ covers), rate (green), call % (white), rough % (red), rows rung / extent,
 * the call flash BOB / SINGLE (a requested call shows dim until it is made) or ROUNDS, gap and run state. */
#include "peal_geom.h"
#define pl_line sq_line
#define pl_dot sq_dot
#define pl_num(p, v) sq_num((p), (v), 1)
#define pl_cat sq_cat

static void pl_screen_draw(void *r, int pos)
{
    pl_bank_t *b = &pl[pos & 1];
    pl_bank_init(b);
    const pl_cfg_t *c = &b->c;
    int n = b->run_n;
    char t[24];
    if (!n) {
        sq_text(r, OP1_Q10(PL_COL_X0), OP1_Q10(56), OP1_Q10(1.5), "HOLD 3-8", 0x480, pl_white);
        sq_text(r, OP1_Q10(PL_COL_X0), OP1_Q10(72), OP1_Q10(1.5), "KEYS", 0x480, pl_white);
    } else {
        int hn = b->hist_n;
        int32_t px = 0, py = 0;
        for (int k = hn - 1; k >= 0; k--) {             /* oldest at the top */
            int line = PL_HIST - 1 - k;
            int32_t y = OP1_Q10(PL_ROW_Y0 + line * PL_ROW_DY);
            const uint8_t *row = b->hist[k];
            if (b->hist_lh[k] && k != hn - 1)           /* lead head: line above */
                pl_line(r, OP1_Q10(PL_COL_X0 - 4), y - OP1_Q10(3), OP1_Q10(PL_COL_X0 + (n - 1) * PL_COL_DX + 10), y - OP1_Q10(3), 0x200, pl_grey);
            for (int i = 0; i < n; i++) {
                int32_t x = OP1_Q10(PL_COL_X0 + i * PL_COL_DX);
                int32_t cx = x + OP1_Q10(3), cy = y + OP1_Q10(4.5);
                if (row[i] == 1) {                      /* treble: blue line */
                    if (k != hn - 1) pl_line(r, px, py, cx, cy, 0x600, pl_blue);
                    pl_dot(r, cx, cy, 0x900, pl_blue);
                    px = cx; py = cy;
                    continue;
                }
                const uint8_t *col = k ? pl_dim : (b->running && i == b->pos - 1 ? pl_white : pl_grey);
                t[0] = (char)('0' + row[i]); t[1] = 0;
                sq_text(r, x, y, OP1_Q10(1.5), t, k ? 0x300 : 0x400, col);
            }
        }
        if (b->running && b->pos > 0 && b->pos <= n) {  /* bell struck: bar under it */
            int32_t x = OP1_Q10(PL_COL_X0 + (b->pos - 1) * PL_COL_DX), y = OP1_Q10(PL_ROW_Y0 + (PL_HIST - 1) * PL_ROW_DY + 12);
            pl_line(r, x - OP1_Q10(1), y, x + OP1_Q10(7), y, 0x300, pl_white);
        }
    }
    /* right panel */
    static const char *const mnames[PL_METHODS] = { "PLAIN HUNT", "PLAIN BOB", "GRANDSIRE", "STEDMAN" };
    static const char *const stages[9] = { "", "", "", "SINGLES", "MINIMUS", "DOUBLES", "MINOR", "TRIPLES", "MAJOR" };
    int32_t x = OP1_Q10(PL_PANEL_X);
    sq_text(r, x, OP1_Q10(4), OP1_Q10(1.4), mnames[c->method], 0x400, pl_blue);
    {
        char *p = t; t[0] = 0;
        if (n) {
            int w = b->m.w;
            p = pl_cat(p, stages[w]);
            if (n > w) { p = pl_cat(p, " +"); p = pl_num(p, (uint32_t)(n - w)); }
        } else pl_cat(p, "NO BELLS");
        sq_text(r, x, OP1_Q10(17), OP1_Q10(1.1), t, 0x300, pl_grey);
    }
    static const char *const rates[PL_RATES] = { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" };
    sq_text(r, x, OP1_Q10(32), OP1_Q10(1.4), rates[c->rate], 0x400, pl_green);
    if (c->gap) sq_text(r, x + OP1_Q10(60), OP1_Q10(33), OP1_Q10(1.1), "GAP", 0x300, pl_green);
    { char *p = pl_cat(t, "CALL "); p = pl_num(p, c->callp); pl_cat(p, "%"); }
    sq_text(r, x, OP1_Q10(48), OP1_Q10(1.2), t, 0x380, pl_white);
    { char *p = pl_cat(t, "ROUGH "); p = pl_num(p, c->rough); pl_cat(p, "%"); }
    sq_text(r, x, OP1_Q10(62), OP1_Q10(1.2), t, 0x380, pl_red);
    if (n) {                                            /* rows rung / extent of the working bells */
        static const uint16_t fact[9] = { 1, 1, 2, 6, 24, 120, 720, 5040, 40320 };
        char *p = pl_num(t, b->flash == PL_ROUNDS ? b->last_course : b->rows);
        p = pl_cat(p, "/"); pl_num(p, fact[b->m.w]);
        sq_text(r, x, OP1_Q10(80), OP1_Q10(1.4), t, 0x400, pl_white);
    }
    const char *fl = 0; const uint8_t *fc = pl_white;
    if (b->flash == PL_CALL_BOB) { fl = "BOB"; fc = pl_red; }
    else if (b->flash == PL_CALL_SINGLE) { fl = "SINGLE"; fc = pl_red; }
    else if (b->flash == PL_ROUNDS) { fl = "ROUNDS"; fc = pl_green; }
    else if (b->call_req) { fl = b->call_req == PL_CALL_BOB ? "BOB" : "SINGLE"; fc = pl_dimred; }   /* called, not yet made */
    if (fl) sq_text(r, x, OP1_Q10(98), OP1_Q10(2.2), fl, 0x600, fc);
    if (b->running) pl_dot(r, x + OP1_Q10(4), OP1_Q10(131), 0x1400, pl_red);
    if (b->stand) sq_text(r, x + OP1_Q10(14), OP1_Q10(127), OP1_Q10(1.1), "THATS ALL", 0x300, pl_white);
    else if (b->running)                                /* keys held / latched / keys up: the last row */
        sq_text(r, x + OP1_Q10(14), OP1_Q10(127), OP1_Q10(1.1), b->rel_pend ? "LAST ROW" : b->latched ? "LATCH" : "HOLD", 0x300, pl_grey);
}

static void pl_screen_update(void *scr, int dt, int pos)
{
    op1_screen_draw(scr, dt);
    pl_screen_draw(op1_raster(), pos);
}

#ifndef OP1_HOST
/* ======================= firmware player (extra sequencer "peal", type 3 + pl_active[mode], like euclid) =======
 * DB row seq_types "peal"; dispatcher.c: SeqIds::from_name sets pl_active, create_player case 3 -> pl_player_get,
 * screen object in the finger slot 0x2D. Note keys (any screen) are consumed (they ring the peal; the firmware does not
 * sound them), so the player also gets their key_up (vt +0x1c, consumed keys only, as dots' live record). */
#define PL_MODE() (*(volatile int *)0x01DE4C40 == 1)
#define PL_SYNTH_OCT() (((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7])
#define PL_DRUM_OCT()  (((const int *)0x01DFE500)[*(volatile int *)0xFF8016D8 & 7])
extern volatile uint8_t eu_rec_play;                    /* seq/euclid.c, handled in key_event_hook */
static int pl_nop(void *self) { (void)self; return 0; }
static int pl_pkey_down(void *self, int key)
{
    uint32_t caller = OP1_SEQ_CALLER();                                     /* keyboard or USB MIDI */
    (void)self;
    if (key < 0 || key > 23) return 0;
    int m = PL_MODE(), sh = *(volatile uint8_t *)0x01DD7F78;
    if (sh) {                                                           /* Shift + C# (any screen) */
        int h = pl_shift_key(m, key);
        int tape = *(volatile int *)0x018C17D4;
        if (h == 2 && (tape == 3 || tape == 6)) eu_rec_play = 1;
        if (h) return 1;
    }
    return pl_key_down(m, op1_seq_key_note(m, key, m ? PL_DRUM_OCT() : PL_SYNTH_OCT(), caller), key, sh);   /* a bell (consumed) */
}
static void pl_pkey_up(void *self, int key)            /* consumed keys only; both modes (a mode switch while held) */
{
    (void)self;
    int sh = *(volatile uint8_t *)0x01DD7F78;
    pl_key_up(0, key, sh); pl_key_up(1, key, sh);
}
#pragma GCC pop_options
static void pl_reset(void *self) { (void)self; pl_all_off(PL_MODE()); }
static void pl_ptick(void *self, int counter) { (void)self; (void)counter; pl_advance(PL_MODE(), 1u << 14); }   /* 1/96 beat */
static void pl_pstop(void *self) { (void)self; pl_all_off(0); pl_all_off(1); }
#pragma GCC push_options
#pragma GCC optimize("Os")
static const uint32_t pl_vt[16] = {
    0, 0,  0, (uint32_t)pl_nop,  0, (uint32_t)pl_pkey_down,  0, (uint32_t)pl_pkey_up,
    0, (uint32_t)pl_reset,  0, (uint32_t)pl_ptick,  0, (uint32_t)pl_nop,  0, (uint32_t)pl_pstop };
static struct { const uint32_t *vt; uint32_t pad[7]; } pl_player = { pl_vt, { 0 } };
volatile uint8_t pl_active[2];                          /* peal selected (synth, drum) */

/* persistence: bg_saver + DB as euclid, table pealseq (is_synth, data); seq_text.h: sq_store_* */
static char pl_sql[200];                                /* text <= 25 fields * 4 chars = 100 */
static sq_store_t pl_store = { .name = "pealseq", .ser = pl_serialize, .parse = pl_parse,
                               .sql = pl_sql, .cap = sizeof pl_sql, .maxlen = 200 };
static void pl_changed(void) { sq_store_dirty(&pl_store); }
void *pl_player_get(void)
{
    sq_store_load(&pl_store);
    return &pl_player;
}

static void pl_scr_init(void *scr) { (void)scr; }
static void pl_scr_update(void *scr, int dt) { pl_screen_update(scr, dt, PL_MODE()); }
const op1_screen_t SCREEN_peal = { pl_scr_init, pl_scr_update };

/* plugin interface (docs/44): lift / drop / scissors do nothing (finger is not running); presses gap, that's all,
 * Bob (Shift: Single), look to */
static int pl_pkey(int m, int code, int pressed, int knob, int shift)
{
    if (code >= 0x0A && code <= 0x0C) return 1;
    if (knob >= 0) { if (pressed) pl_press(m, knob, shift); return 1; }
    return 0;
}
const op1_seq_t SEQ_peal = { pl_active, pl_player_get, pl_pkey, pl_turn, pl_midi_start, 0 };
#endif
#pragma GCC pop_options
