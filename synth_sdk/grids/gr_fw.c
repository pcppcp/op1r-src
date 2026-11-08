/* "grids" sequencer plugin (docs/52): the player and the firmware glue around gr_ui.c / gr_core.c.
 * Clock: the player tick (96 per beat); a Grids step (1/32 note) = 12 ticks, a bar = 32 steps. Swing delays the
 * second 16th of each 8th (Grids: step & 2) by up to 1/3 of a 16th. Gates: half a step (drums: notes end at once).
 * Drum mode: each part plays its pad (key 0..23). Synth mode: the held keys (lowest -> BD, middle -> SD, highest ->
 * HH; fewer keys repeat), none held: the stored notes; keys held on the grids page are consumed (they set notes).
 * Stopped by default (not saved); red press: play / stop (starts on the next tick at step 0); MIDI START restarts at
 * step 0. */
#include "../op1_synth.h"
#include "../seq/seq_text.h"
#include "gr_ui.h"

#define TICKS_STEP 12
#define VEL_MAX    127

typedef struct {
    uint32_t tick;                            /* ticks since step 0 of the bar */
    uint8_t  held_n, held[GR_PARTS * 4], held_key[GR_PARTS * 4];   /* synth: held keys' midi notes + key 0..23, press order */
    int16_t  on_note[GR_PARTS];               /* sounding note per part (-1 none) */
    uint8_t  off_at[GR_PARTS];                /* ticks left until its note off */
    uint8_t  swing_hits, swing_left;          /* a swung step's hits / accents, ticks until they fire */
    uint8_t  restart, init;
} gr_run;
static gr_run gr_r[2];

#ifdef OP1_HOST
void host_gr_note(int m, int note, int vel, int on);
#define GR_NOTE_ON(m, note, key, vel) ((void)(key), host_gr_note((m), (note), (vel), 1))
#define GR_NOTE_OFF(m, note)          host_gr_note((m), (note), 0, 0)
#define GR_CHANGED()                  ((void)0)
int gr_host_oct;                                             /* host tests: keyboard octave */
int gr_host_held_n(void);
static int gr_oct(int m) { (void)m; return gr_host_oct; }
#else
int  eu_fw_note_on(int pos, int note, int key, int vel);   /* seq/seq_core.c: VoiceCtl note on / off */
void eu_fw_note_off(int midi);
#define GR_NOTE_ON(m, note, key, vel) eu_fw_note_on((m), (note), (key), (vel))
#define GR_NOTE_OFF(m, note)          eu_fw_note_off(note)
static void gr_changed(void);
#define GR_CHANGED()                  gr_changed()
static int gr_oct(int m)                                    /* keyboard octave of the current slot */
{
    return m ? ((const int *)0x01DFE500)[*(volatile int *)0xFF8016D8 & 7] : ((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7];
}
#endif

static gr_run *run(int m)
{
    gr_run *r = &gr_r[m & 1];
    if (!r->init) {
        r->init = 1;
        for (int i = 0; i < GR_PARTS; i++) r->on_note[i] = -1;
        gr_ui_init(m & 1);
    }
    return r;
}

static void note_off(int m, int part)
{
    gr_run *r = &gr_r[m & 1];
    if (r->on_note[part] < 0) return;
    GR_NOTE_OFF(m, r->on_note[part]);
    r->on_note[part] = -1;
}
void gr_all_off(int m) { run(m); for (int i = 0; i < GR_PARTS; i++) note_off(m, i); }

static int part_note(int m, int part)                       /* the midi note a part plays now */
{
    gr_run *r = &gr_r[m & 1];
    gr_bank *b = &gr_b[m & 1];
    if (m) return b->note[part] + 0x35 + gr_oct(1) * 12;      /* drum: the pad's key at the drum octave */
    if (!r->held_n) return b->note[part];
    uint8_t s[GR_PARTS * 4];                                  /* held keys sorted by pitch */
    int n = r->held_n;
    for (int i = 0; i < n; i++) s[i] = r->held[i];
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && s[j] < s[j - 1]; j--) { uint8_t t = s[j]; s[j] = s[j - 1]; s[j - 1] = t; }
    int k = n >= GR_PARTS ? part * (n - 1) / (GR_PARTS - 1) : part % n;   /* lowest .. highest */
    return s[k];
}

static void fire(int m, int e)                               /* hits (bits 0..2) and accents (3..5) */
{
    gr_bank *b = &gr_b[m & 1];
    gr_run *r = &gr_r[m & 1];
    int played[GR_PARTS], np = 0;
    for (int i = 0; i < GR_PARTS; i++) {
        if (!(e >> i & 1)) continue;
        note_off(m, i);
        int note = part_note(m, i), vel = e >> (3 + i) & 1 ? VEL_MAX : VEL_MAX - b->accent, dup = 0;
        if (note < 0 || note > 127) continue;
        for (int j = 0; j < np; j++) dup |= played[j] == note;   /* fewer keys than parts: one note-on per note */
        if (dup) continue;
        played[np++] = note;
        GR_NOTE_ON(m, note, m ? b->note[i] : 0, vel < 1 ? 1 : vel);
        r->on_note[i] = (int16_t)note;
        r->off_at[i] = (uint8_t)(b->res32 ? TICKS_STEP / 2 : TICKS_STEP);   /* half a step */
    }
}

void gr_tick(int m)                                          /* 1/96 beat */
{
    gr_run *r = run(m);
    gr_bank *b = &gr_b[m & 1];
    for (int i = 0; i < GR_PARTS; i++)
        if (r->on_note[i] >= 0 && r->off_at[i] && !--r->off_at[i]) note_off(m, i);
    if (r->swing_left && !--r->swing_left) { fire(m, r->swing_hits); r->swing_hits = 0; }
    if (!b->playing) return;
    if (r->restart) { r->restart = 0; r->tick = 0; }
    if (r->tick % TICKS_STEP == 0) {
        int step = (int)(r->tick / TICKS_STEP);
        int e = gr_ui_step(m, step);
        int delay = (step & 2) ? b->swing * TICKS_STEP * 2 / 3 / 42 : 0;   /* up to 1/3 of a 16th */
        if (e && delay) { if (r->swing_hits) fire(m, r->swing_hits); r->swing_hits = (uint8_t)e; r->swing_left = (uint8_t)delay; }
        else if (e) fire(m, e);
    }
    if (++r->tick >= (uint32_t)TICKS_STEP * GR_STEPS) r->tick = 0;
}

#ifdef OP1_HOST
int gr_host_held_n(void) { return run(0)->held_n; }
#endif
void gr_restart(int m) { run(m)->restart = 1; }

/* synth mode: keys on the grids page set the notes (consumed); elsewhere and in drum mode keys play as usual */
#ifdef OP1_HOST
int gr_host_on_page = 1;                                             /* host tests: the grids page is shown */
#endif
int gr_key_down_from(int m, int key, uint32_t caller)        /* caller: OP1_SEQ_CALLER() of the player's key_down */
{
    gr_run *r = run(m);
    if (m || key < 0 || key > 23) return 0;
#ifndef OP1_HOST
    if (*(volatile int *)0x018B65C8 != 0x2D) return 0;                 /* not on the grids page: the key plays as usual */
#else
    if (!gr_host_on_page) return 0;
#endif
    int midi = op1_seq_key_note(0, key, gr_oct(0), caller);
    if (r->held_n < sizeof r->held) { r->held_key[r->held_n] = (uint8_t)key; r->held[r->held_n++] = (uint8_t)midi; }
    return 1;
}
int gr_key_down(int m, int key) { return gr_key_down_from(m, key, 0); }   /* host tests: the keyboard */
void gr_key_up(int m, int key)                               /* by key, not note: the octave may have changed meanwhile */
{
    gr_run *r = run(m);
    if (m) return;
    for (int i = 0; i < r->held_n; i++)
        if (r->held_key[i] == key) {
            for (int j = i; j + 1 < r->held_n; j++) { r->held[j] = r->held[j + 1]; r->held_key[j] = r->held_key[j + 1]; }
            r->held_n--; break;
        }
}

/* ---- persistence: "x,y,chaos,f0,f1,f2,sel,n0,n1,n2,swing,res32,accent" ---- */
int gr_serialize(int m, char *out, int n)
{
    run(m);
    const gr_bank *b = &gr_b[m & 1];
    const gr_set *s = &b->g.s;
    int v[13] = { s->x, s->y, s->chaos, s->density[0], s->density[1], s->density[2], b->sel, b->note[0], b->note[1],
                  b->note[2], b->swing, b->res32, b->accent };
    return sq_join(out, n, v, 13);
}
int gr_parse(int m, const char *str, int len)
{
    int v[13];
    if (sq_split(str, len, v, 13) != 13) return 0;
    run(m);
    gr_bank *b = &gr_b[m & 1];
    gr_set *s = &b->g.s;
    for (int i = 0; i < 13; i++) v[i] = v[i] < 0 ? 0 : v[i] > 255 ? 255 : v[i];
    s->x = (uint8_t)v[0]; s->y = (uint8_t)v[1]; s->chaos = (uint8_t)v[2];
    for (int i = 0; i < GR_PARTS; i++) { s->density[i] = (uint8_t)v[3 + i]; b->note[i] = (uint8_t)(m ? (v[7 + i] > 23 ? 23 : v[7 + i]) : (v[7 + i] > 127 ? 127 : v[7 + i])); }
    b->sel = (uint8_t)(v[6] % GR_PARTS); b->swing = (uint8_t)(v[10] > 42 ? 42 : v[10]); b->res32 = (uint8_t)(v[11] & 1);
    b->accent = (uint8_t)(v[12] > 127 ? 127 : v[12]);
    return 1;
}

/* page controls: encoder deltas (1024 a detent) to whole detents */
static int32_t gr_acc[2][4];
void gr_turn(int m, int knob, int delta, int shift)
{
    run(m);
    if (knob < 0 || knob > 3) return;
    int32_t *a = &gr_acc[m & 1][knob];
    if ((*a > 0 && delta < 0) || (*a < 0 && delta > 0)) *a = 0;
    *a += delta;
    int st = *a / 1024;
    *a -= st * 1024;
    if (!st) return;
    gr_ui_turn(m, knob, st, shift);
    GR_CHANGED();
}
void gr_press(int m, int knob)
{
    run(m);
    int was = gr_b[m & 1].playing;
    gr_ui_press(m, knob);
    if (knob == 3) { if (gr_b[m & 1].playing && !was) gr_restart(m); else if (!gr_b[m & 1].playing) gr_all_off(m); }
    GR_CHANGED();
}

#ifndef OP1_HOST
/* ======================= firmware player (type 3 + gr_active[mode], as peal / chord) ======================= */
#define GR_MODE()  (*(volatile int *)0x01DE4C40 == 1)
#define SHIFT_HELD (*(volatile uint8_t *)0x01DD7F78)
static int gr_nop(void *self) { (void)self; return 0; }
static int gr_pkey_down(void *self, int key) { (void)self; return gr_key_down_from(GR_MODE(), key, OP1_SEQ_CALLER()); }
static void gr_pkey_up(void *self, int key) { (void)self; gr_key_up(0, key); gr_key_up(1, key); }
static void gr_reset(void *self) { (void)self; gr_all_off(GR_MODE()); }
static void gr_ptick(void *self, int counter) { (void)self; (void)counter; gr_tick(GR_MODE()); }
static void gr_pstop(void *self) { (void)self; gr_all_off(0); gr_all_off(1); }
static const uint32_t gr_vt[16] = {
    0, 0,  0, (uint32_t)gr_nop,  0, (uint32_t)gr_pkey_down,  0, (uint32_t)gr_pkey_up,
    0, (uint32_t)gr_reset,  0, (uint32_t)gr_ptick,  0, (uint32_t)gr_nop,  0, (uint32_t)gr_pstop };
static struct { const uint32_t *vt; uint32_t pad[7]; } gr_player = { gr_vt, { 0 } };
volatile uint8_t gr_active[2];

static char gr_sql[200];
static sq_store_t gr_store = { .name = "gridsseq", .ser = gr_serialize, .parse = gr_parse,
                               .sql = gr_sql, .cap = sizeof gr_sql, .maxlen = 100 };
static void gr_changed(void) { sq_store_dirty(&gr_store); }
static void *gr_player_get(void) { run(0); run(1); sq_store_load(&gr_store); return &gr_player; }

static void gr_scr_init(void *scr) { (void)scr; }
static void gr_scr_update(void *scr, int dt) { op1_screen_draw(scr, dt); gr_ui_draw(op1_raster(), GR_MODE(), SHIFT_HELD); }
const op1_screen_t SCREEN_grids = { gr_scr_init, gr_scr_update };

static int gr_pkey(int m, int code, int pressed, int knob, int shift)
{
    (void)shift;
    if (code >= 0x0A && code <= 0x0C) return 1;              /* lift / drop / scissors: nothing */
    if (knob >= 0) { if (pressed) gr_press(m, knob); return 1; }
    return 0;
}
static void gr_midi_start(void) { gr_restart(0); gr_restart(1); }
const op1_seq_t SEQ_grids = { gr_active, gr_player_get, gr_pkey, gr_turn, gr_midi_start, 0 };
#endif
