/* Host test of the dots sequencer (seq/dots.c): cursor / erase / step record / level / start-stop / timing /
 * mutes / patterns. "render": JSON screen frames for harness/render_dots.py. */
#include "../seq/dots.c"
#include <math.h>
#include <stdio.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4];

static int n_on, n_off, last_on, last_vel, active; static long now, last_on_at;
void host_dt_note_on(int pos, int n, int v) { (void)pos; n_on++; last_on = n; last_vel = v; active++; last_on_at = now; }
void host_dt_note_off(int pos, int n) { (void)pos; (void)n; n_off++; active--; }
static int fx_val[2][DT_NLK] = { { -1, -1, -1, -1, -1, -1, -1, -1 }, { -1, -1, -1, -1, -1, -1, -1, -1 } }, n_lock; static long lock_at, restore_at;   /* -1 = base */
void host_dt_fx_lock(int pos, int k, int pc) { fx_val[pos][k] = pc; n_lock++; lock_at = now; }
void host_dt_fx_restore(int pos, int k) { fx_val[pos][k] = -1; restore_at = now; }

#define MAXL 4096
static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } lines[MAXL]; static int nlines;
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_screen_draw(void *s, int d) { (void)s; (void)d; nlines = 0; }
void *host_raster(void) { return &nlines; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c)
{ (void)r; const op1_pt *p = a, *q = b; if (nlines < MAXL) { lines[nlines].x0 = p->x; lines[nlines].y0 = p->y; lines[nlines].x1 = q->x; lines[nlines].y1 = q->y; lines[nlines].w = w; memcpy(lines[nlines].c, c, 4); nlines++; } }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static void run(long samples) { for (long s = 0; s < samples; s += 64) { dt_tick(0, 64, 12000); now += 64; } }   /* 120 bpm */
static void turn(int k, int st, int sh) { dt_turn(0, k, st * 1024, sh); }
static void frame(const char *label)
{
    dt_screen_update(0, 16, 0);
    printf("{\"label\": \"%s\", \"nodes\": {}, \"lines\": [", label);
    for (int i = 0; i < nlines; i++) printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", lines[i].x0, lines[i].y0, lines[i].x1, lines[i].y1, lines[i].w, lines[i].c[0], lines[i].c[1], lines[i].c[2], lines[i].c[3]);
    printf("]}\n");
}

int main(int argc, char **argv)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    int render = argc > 1 && !strcmp(argv[1], "render");
    dt_bank_t *b = &dt[0];
#define STEPREC() (b->rec = b->rec ? 0 : 1)           /* step record on / off (REC key with the slot stopped) */

    if (render) frame("empty slot 1, 16 steps");
    /* keys only record in record mode; recording advances the cursor, no chords (replace) */
    CHECK(!dt_key(0, 60, 7), "key recorded outside record mode");
    STEPREC();
    CHECK(b->rec == 1, "step record on");
    int mel[] = { 60, 63, 67, 70 };
    for (int i = 0; i < 4; i++) { dt_key(0, mel[i], mel[i] - 53); turn(0, 3, 0); }
    CHECK(b->cur[0] == 0 && dt_q(b, 0)->st[0].note == 60 && dt_q(b, 0)->st[4].note == 63 && dt_q(b, 0)->st[12].note == 70 && !dt_q(b, 0)->st[1].vel,
          "step record: cursor %d", b->cur[0]);
    turn(0, -1, 0);                                     /* blue left wraps 0 -> 15 */
    CHECK(b->cur[0] == 15, "cursor wrap left -> %d", b->cur[0]);
    turn(0, 1, 0); turn(0, 2, 0);                       /* -> step 2 */
    dt_key(0, 72, 19); turn(0, -1, 0);                  /* replace? no: step 2 empty -> new trigger, then back */
    dt_key(0, 74, 21);
    CHECK(dt_q(b, 0)->st[2].note == 74 && b->cur[0] == 3, "re-record replaces: %d cur %d", dt_q(b, 0)->st[2].note, b->cur[0]);
    turn(0, -1, 0);
    turn(3, -6, 0);                                     /* record mode: red = level */
    CHECK(dt_q(b, 0)->st[2].vel == 76, "level %d", dt_q(b, 0)->st[2].vel);
    CHECK(!b->rt[0].running, "red started in record mode");
    if (render) frame("record mode: 5 triggers, cursor on step 3, level 76");

    STEPREC();                                     /* perform: red right starts, next step boundary */
    turn(3, 1, 0);
    CHECK(b->rt[0].running, "red right -> running");
    n_on = 0; run(44100 * 2);                           /* 2 s at 120 bpm = 4 beats = 16 steps = 1 loop */
    CHECK(n_on == 5, "one loop -> %d notes", n_on);
    CHECK(active <= 1, "notes overlap (%d)", active);
    run(44100 * 3 / 16);                                /* into the next loop: playhead shown */
    if (render) frame("slot 1 running (playhead), cursor on step 3");

    /* Shift+blue erases the steps the cursor leaves, in record mode only */
    b->cur[0] = 0; turn(0, 3, 1);
    CHECK(dt_q(b, 0)->st[0].vel && b->cur[0] == 3, "perform: Shift+blue must not erase");
    b->cur[0] = 0; STEPREC(); turn(0, 3, 1); STEPREC();
    CHECK(!dt_q(b, 0)->st[0].vel && !dt_q(b, 0)->st[2].vel && dt_q(b, 0)->st[4].vel && b->cur[0] == 3,
          "erase: %d %d %d cur %d", dt_q(b, 0)->st[0].vel, dt_q(b, 0)->st[2].vel, dt_q(b, 0)->st[4].vel, b->cur[0]);

    /* slot 2: 64 steps, all slots play at once, mute */
    dt_select(0, 1024);
    CHECK(b->sel == 1, "select -> %d", b->sel);
    STEPREC();
    dt_turn(0, 1, -1024, 0); CHECK(dt_q(b, 1)->len == 15, "green -1 -> %d", dt_q(b, 1)->len);
    dt_turn(0, 1, 1024, 1);  CHECK(dt_q(b, 1)->len == 16, "Shift+green fills to 16 (%d)", dt_q(b, 1)->len);
    dt_turn(0, 1, 1024, 0);  CHECK(dt_q(b, 1)->len == 17 && !dt_q(b, 1)->st[16].vel, "green +1 -> 17, empty");
    dt_turn(0, 1, -1024, 0);
    b->cur[1] = 0;
    for (int i = 0; i < 16; i++) { if (i % 3 == 0 || i == 14) dt_key(0, 48 + i % 12, i); else turn(0, 1, 0); }
    dt_turn(0, 1, 1024, 1);
    CHECK(dt_q(b, 1)->len == 32 && dt_q(b, 1)->st[16 + 3].vel && dt_q(b, 1)->st[16 + 3].note == dt_q(b, 1)->st[3].note
          && !dt_q(b, 1)->st[16 + 1].vel, "Shift+green: full row -> +16, copies row above");
    dt_turn(0, 1, 2048, 1);
    CHECK(dt_q(b, 1)->len == 64, "Shift+green x2 -> 64 (%d)", dt_q(b, 1)->len);
    dt_turn(0, 1, 1024, 1); CHECK(dt_q(b, 1)->len == 64, "max 64");
    dt_turn(0, 1, -1024, 1); CHECK(dt_q(b, 1)->len == 48, "Shift+green left -> 48 (%d)", dt_q(b, 1)->len);
    dt_turn(0, 1, 1024, 1);
    STEPREC(); dt_turn(0, 1, -1024, 0); CHECK(dt_q(b, 1)->len == 64, "green ignored outside record mode"); STEPREC();
    b->cur[1] = 16;
    for (int i = 16; i < 64; i++) { if (i % 3 == 0 || i % 16 == 14) dt_key(0, 48 + i % 12, i % 24); else turn(0, 1, 0); }
    STEPREC();
    b->cur[1] = 37;
    turn(3, 1, 0);
    run(44100);
    CHECK(b->rt[0].running && b->rt[1].running, "both slots running");
    if (render) frame("slot 2: 64 steps, both slots running");
    dt_mute(0, 1);
    n_on = 0; run(44100 / 2);
    CHECK(n_on == 1 || n_on == 2, "muted slot 2: only slot 1 plays (%d)", n_on);
    if (render) frame("slot 2 muted");
    dt_mute(0, 1);
    turn(3, -1, 1); CHECK(!b->rt[0].running && !b->rt[1].running, "Shift+red left stops all");
    turn(3, 1, 1);  CHECK(b->rt[0].running && b->rt[5].running && b->rt[0].start == b->rt[5].start, "Shift+red right starts all together");
    CHECK(dt_shift_key(0, 20) == 1 && !b->rt[0].running, "Shift+C# stops all");
    CHECK(dt_shift_key(0, 20) == 2 && b->rt[3].running, "Shift+C# starts all");
    for (int k = 2; k < 6; k++) { b->rt[k].running = 0; }
    dt_select(0, 1024 * 5);
    CHECK(b->sel == 0, "select wraps 6 -> 1 (%d)", b->sel);

    /* patterns: separate steps, run state kept */
    dt_recall(0, 2);
    CHECK(b->pat == 2 && !dt_q(b, 0)->st[4].vel && b->rt[0].running, "pattern 3 empty, slot still running");
    if (render) frame("pattern 3: empty, slots keep running");
    dt_recall(0, 0);
    CHECK(dt_q(b, 0)->st[4].vel, "pattern 1 back");
    turn(3, -1, 0);
    CHECK(!b->rt[0].running && !b->rt[0].sounding, "red left stops");

    /* Shift keys, clipboard, persistence round trip */
    CHECK(dt_shift_key(0, 16) && dt_q(b, 1)->muted && !dt_shift_key(0, 1), "Shift+A mutes slot 2, Shift+F# unused");
    if (render) frame("Shift+A: slot 2 muted (grey), others lilac");
    dt_shift_key(0, 22); CHECK(dt_q(b, 0)->muted && dt_q(b, 5)->muted, "Shift+D# mutes all");
    dt_shift_key(0, 22); CHECK(!dt_q(b, 0)->muted && !dt_q(b, 1)->muted, "Shift+D# again unmutes all");
    dt_shift_key(0, 2); CHECK(b->pat == 1, "Shift+G -> pattern 2 (%d)", b->pat);
    dt_shift_key(0, 0);
    dt_clip(0, DT_COPY, 0); b->sel = 3; dt_clip(0, DT_PASTE, 0);
    CHECK(dt_q(b, 3)->st[4].note == 63 && dt_q(b, 3)->len == 16, "paste slot 1 -> slot 4");
    dt_clip(0, DT_CUT, 0);
    CHECK(!dt_q(b, 3)->st[4].vel && dt_q(b, 3)->len == 16, "cut clears triggers, keeps length");
    b->sel = 1;
    { static char txt[2][DT_TEXT_LEN + 1]; dt_bank_t keep = *b;
      CHECK(dt_serialize(0, 0, txt[0]) == DT_TEXT_LEN && dt_serialize(0, 1, txt[1]) == DT_TEXT_LEN, "text length");
      memset(b, 0, sizeof *b);
      CHECK(dt_parse(0, 0, txt[0], DT_TEXT_LEN) && dt_parse(0, 1, txt[1], DT_TEXT_LEN), "parse");
      CHECK(!memcmp(b->pats[0], keep.pats[0], sizeof keep.pats[0]) && !memcmp(b->pats[1], keep.pats[1], sizeof keep.pats[1])
            && b->sel == 1 && b->pat == 0, "round trip");
      CHECK(!dt_parse(0, 0, txt[0], DT_TEXT_LEN - 2), "short text rejected"); }

    /* trigger parameters: white page / select, defaults, re-record keeps them */
    {
        for (int k = 0; k < DT_SLOTS; k++) dt_halt(0, &b->rt[k]);
        dt_recall(0, 3); b->sel = 0; b->cur[0] = 0;
        STEPREC();                                 /* record mode */
        dt_key(0, 60, 7);
        dt_step_t *t = &dt_q(b, 0)->st[0];
        CHECK(t->gate == 50 && t->ratch == 1 && t->chance == 100 && !t->cond && !t->nudge && !t->ramp, "defaults");
        b->cur[0] = 0;
        dt_turn(0, 2, 1024, 0); CHECK(t->gate == 55, "white page 1: gate %d", t->gate);
        dt_turn(0, 2, 10 * 1024, 0); CHECK(t->gate == DT_TIE, "gate -> TIE (%d)", t->gate);
        if (render) frame("rec: GATE TIE (page 1 = value)");
        dt_press(0, 2); dt_turn(0, 2, 1024, 0); CHECK(b->par == DT_P_RATCH && b->page, "page 2 selects RATCH");
        if (render) frame("rec: page 2, RATCH selected");
        dt_press(0, 2); dt_turn(0, 2, 2 * 1024, 0); CHECK(t->ratch == 3, "ratch %d", t->ratch);
        dt_key(0, 62, 9); CHECK(t->note == 62 && t->ratch == 3 && t->gate == DT_TIE, "re-record keeps parameters");
        dt_step_t *t2 = &dt_q(b, 0)->st[1];
        dt_key(0, 64, 11);
        CHECK(t2->vel == 100 && t2->ratch == 1 && t2->gate == 50, "new trigger -> defaults");
        if (render) frame("rec: cursor on empty step 3");
        STEPREC();

        /* playback: ratchet 3 + tie -> 3 notes, no note-off between until the next hit */
        t->gate = 50;
        n_on = 0; dt_start(b, &b->rt[0], dt_next_step(b)); run(44100 * 2);   /* one loop: 16 steps */
        CHECK(n_on == 3 + 1, "ratch 3 + one plain trigger: %d notes", n_on);
        dt_halt(0, &b->rt[0]);
        /* ramp up: levels rise */
        t->ramp = 1; t->vel = 90; t2->vel = 0;
        last_vel = 0; n_on = 0; dt_start(b, &b->rt[0], dt_next_step(b)); run(44100 / 8);
        CHECK(n_on == 3 && last_vel == 90, "ramp up: last hit full level (%d, %d)", n_on, last_vel);
        dt_halt(0, &b->rt[0]); t->ramp = 0; t->ratch = 1;
        /* chance 0: never; cond 1:2: every other loop */
        t->chance = 0; n_on = 0; dt_start(b, &b->rt[0], dt_next_step(b)); run(44100 * 4);
        CHECK(n_on == 0, "chance 0 plays (%d)", n_on);
        dt_halt(0, &b->rt[0]); t->chance = 100; t->cond = 1;
        n_on = 0; dt_start(b, &b->rt[0], dt_next_step(b)); run(44100 * 8);   /* 4 loops */
        CHECK(n_on == 2, "cond 1:2 over 4 loops: %d", n_on);
        dt_halt(0, &b->rt[0]); t->cond = 0;
        /* nudge: +6/96 beat = 1378 samples at 120 bpm later than nudge 0 (firmware tick = 1/96 beat; host 64-sample blocks) */
        long base, late;
        dt_start(b, &b->rt[0], dt_next_step(b)); long st0 = now; run(44100); base = last_on_at - st0; dt_halt(0, &b->rt[0]);
        t->nudge = 6; dt_start(b, &b->rt[0], dt_next_step(b)); st0 = now; run(44100); late = last_on_at - st0; dt_halt(0, &b->rt[0]);
        CHECK(late - base > 1300 && late - base < 1600, "nudge +6: %ld samples later", late - base);
        t->nudge = -12; dt_start(b, &b->rt[0], dt_next_step(b)); st0 = now; run(44100 / 2); dt_halt(0, &b->rt[0]);
        CHECK(n_on > 0, "nudge -12 still plays");
        t->nudge = 0;
        /* old (v1) text still loads with defaults */
        static char v1[DT_TEXT1_LEN + 1]; char *o = dt_hex(dt_hex(v1, 0), 0);
        for (int k = 0; k < DT_SLOTS; k++) { o = dt_hex(dt_hex(o, 16), 0); for (int i = 0; i < DT_STEPS; i++) o = dt_hex(dt_hex(dt_hex(o, 60), 7), i == 0 ? 80 : 0); }
        CHECK(dt_parse(1, 2, v1, DT_TEXT1_LEN) && dt[1].pats[2][0].st[0].vel == 80 && dt[1].pats[2][0].st[0].gate == 50, "v1 text loads");
    }

    /* REC key = recording on / off; red press = type REC / LIVE (any time); LIVE records at the playhead while the
     * slot runs, at the cursor while it is stopped */
    {
        for (int k = 0; k < DT_SLOTS; k++) dt_halt(0, &b->rt[k]);
        b->rec = 0; b->live = 0; b->sel = 2; b->cur[2] = 0;
        dt_rec_key(0); CHECK(b->rec == 1 && !b->live, "REC on, type REC");
        dt_press(0, 3); CHECK(b->live == 1, "red press -> LIVE");
        if (render) frame("LIVE selected, slot stopped: records at the cursor");
        CHECK(!dt_live_down(0, 59, 6), "LIVE + stopped: no live record");
        CHECK(dt_key(0, 59, 6) && dt_q(b, 2)->st[0].note == 59 && b->cur[2] == 1, "LIVE + stopped: records at the cursor");
        dt_start(b, &b->rt[2], dt_next_step(b));
        CHECK(!dt_key(0, 58, 5), "LIVE + running: no step record");
        const long step = 44100 / 8;                    /* 1/16 at 120 bpm = 5512 samples */
        run(step * 5 + step / 2 - 600);
        n_on = 0;
        CHECK(dt_live_down(0, 60, 7) && n_on == 1, "live key sounds at once");
        CHECK(dt_q(b, 2)->st[5].vel && dt_q(b, 2)->st[5].note == 60 && b->cur[2] == 5, "recorded at step 5 (cur %d)", b->cur[2]);
        run(step / 4); dt_live_up(0, 7);
        CHECK(dt_q(b, 2)->st[5].gate >= 20 && dt_q(b, 2)->st[5].gate <= 30, "gate from hold: %d", dt_q(b, 2)->st[5].gate);
        run(step); run(step / 2 + 400);
        { uint64_t L = (uint64_t)DT_RATE << 16, S = (uint64_t)b->rt[2].start << 16;
          double pos = (double)(b->tick_q16 - S) / L;
          CHECK(pos > 6.5 && pos < 7.0 && b->rt[2].planned > 7, "early-press setup: at step %.2f, planned %d", pos, b->rt[2].planned); }
        n_on = 0; dt_live_down(0, 64, 11); int n1 = n_on;
        run(step * 2); dt_live_up(0, 11);
        CHECK(dt_q(b, 2)->st[7].note == 64 && dt_q(b, 2)->st[7].gate == DT_TIE, "early press -> step 7, long hold -> TIE");
        CHECK(n1 == 1 && n_on == 1, "no double note on the recorded step (%d)", n_on);
        n_on = 0; run(step * 16);
        CHECK(n_on == 3, "next loop plays the 3 recorded steps (%d)", n_on);
        if (render) frame("LIVE recording: steps 6 and 8 (no cursor bar)");
        dt_rec_key(0); CHECK(!b->rec && b->live, "REC off, type stays LIVE");
        if (render) frame("recording off: type LIVE shown dim");
        dt_halt(0, &b->rt[2]); b->live = 0;
    }

    /* MIDI START: a running slot restarts at step 0, its first note on the next tick */
    {
        for (int k = 0; k < DT_SLOTS; k++) dt_halt(0, &b->rt[k]);
        dt_recall(0, 3); b->sel = 0;
        dt_q(b, 0)->st[0].vel = 90; dt_q(b, 0)->st[0].ratch = 1; dt_q(b, 0)->st[0].nudge = 0; dt_q(b, 0)->st[0].chance = 100; dt_q(b, 0)->st[0].cond = 0;
        dt_start(b, &b->rt[0], dt_next_step(b)); run(44100 * 3 / 10);    /* somewhere in the pattern */
        n_on = 0; dt_midi_start();
        dt_advance(0, 1u << 14);                         /* first firmware tick after START */
        CHECK(n_on == 1 && b->rt[0].step == 0, "MIDI START: step 0 on the next tick (notes %d, step %d)", n_on, b->rt[0].step);
        /* external clock: 4 ticks per F8 in a burst; a trigger on step 4 (clock 24) must fire on the 1st tick of clock 24 */
        dt_q(b, 0)->st[0].vel = 0; dt_q(b, 0)->st[4] = dt_q(b, 0)->st[0]; dt_q(b, 0)->st[4].vel = 90;
        dt_midi_start();
        int fired_at = -1;
        for (int u = 0; u < 4 * 30 && fired_at < 0; u++) { n_on = 0; dt_advance(0, 1u << 14); if (n_on) fired_at = u; }
        CHECK(fired_at == 96, "step 4 fires on tick %d (expected 96 = 1st tick of clock 24)", fired_at);
        dt_halt(0, &b->rt[0]);
    }

    /* parameter locks: blue press = lock trigger on / off, values per FX knob, revert after the gate / at the next trigger */
    {
        const long step = 44100 / 8;
        for (int k = 0; k < DT_SLOTS; k++) dt_halt(0, &b->rt[k]);
        dt_recall(0, 1); b->sel = 4; b->rec = 1; b->live = 0; b->page = 0; b->cur[4] = 0;
        dt_cfg_clear(dt_q(b, 4), 16);
        dt_press(0, 0);
        dt_step_t *s0 = &dt_q(b, 4)->st[0];
        CHECK(s0->vel && s0->note == DT_NONOTE && !s0->fx[0] && !s0->fx[3], "blue press: lock trigger, locks off");
        dt_press(0, 0); CHECK(!s0->vel, "blue press again: removed");
        dt_press(0, 0);
        b->rt[4].start = 0; dt_start(b, &b->rt[4], dt_next_step(b));
        n_on = n_lock = 0; run(step * 16);
        CHECK(n_on == 0 && n_lock == 0, "lock trigger with all locks off does nothing (%d notes, %d locks)", n_on, n_lock);
        b->par = DT_P_FX1 + 1; turn(2, 1, 0);           /* fx2: off -> 0 % */
        CHECK(s0->fx[1] == 1, "fx2 off -> 0 %% (%d)", s0->fx[1]);
        turn(2, 15, 0); CHECK(s0->fx[1] == 31, "fx2 30 %% (%d)", s0->fx[1]);
        turn(2, -16, 0); CHECK(s0->fx[1] == 0, "fx2 below 0 %% -> off (%d)", s0->fx[1]);
        turn(2, 16, 0);                                 /* 30 % */
        if (render) frame("lock trigger at step 1 (hollow square), FX2 = 30 %");
        n_on = n_lock = 0; long t_loop = now; run(step * 16);
        CHECK(n_on == 0 && n_lock == 1 && fx_val[0][1] == -1, "lock fires once, no note, reverted (%d %d %d)", n_on, n_lock, fx_val[0][1]);
        CHECK(restore_at - lock_at > step * 4 / 10 && restore_at - lock_at < step * 6 / 10, "reverts after the gate (50 %%): %ld", restore_at - lock_at);
        (void)t_loop;
        /* a note at the lock trigger keeps the lock; TIE holds it until the slot's next trigger */
        b->cur[4] = 0; dt_key(0, 62, 9);
        CHECK(s0->note == 62 && s0->fx[1] == 31, "key adds a note, lock kept");
        s0->gate = DT_TIE; dt_q(b, 4)->st[8].vel = 90; dt_q(b, 4)->st[8].note = 64; dt_step_default(&dt_q(b, 4)->st[8]);
        dt_halt(0, &b->rt[4]); dt_start(b, &b->rt[4], dt_next_step(b));
        run(step * 4); CHECK(fx_val[0][1] == 30, "TIE: lock still on at step 4 (%d)", fx_val[0][1]);
        run(step * 5); CHECK(fx_val[0][1] == -1, "TIE: reverted at the next trigger (step 8)");
        b->cur[4] = 0; b->page = 1;
        if (render) frame("note trigger with a lock (tick above): FX2 selected (page 2)");
        b->page = 0; b->par = DT_P_FX1 + 3; b->cur[4] = 3; dt_press(0, 0);
        if (render) frame("lock trigger at step 4 (cursor), FX4 off = does nothing");
        dt_press(0, 0); b->cur[4] = 0;
        /* two slots lock the same knob: the last wins, the knob reverts when the owner's lock ends */
        dt_cfg_clear(dt_q(b, 5), 16); dt_q(b, 5)->st[0] = *s0; dt_q(b, 5)->st[0].gate = 100; dt_q(b, 5)->st[0].fx[1] = 81;
        s0->gate = 25;
        dt_halt(0, &b->rt[4]); dt_start(b, &b->rt[4], dt_next_step(b)); dt_start(b, &b->rt[5], b->rt[4].start);
        run(step / 2); CHECK(fx_val[0][1] == 80 || fx_val[0][1] == 30, "both locked (%d)", fx_val[0][1]);
        run(step * 3 / 10); CHECK(fx_val[0][1] != -1, "first lock ended, the other owner keeps the knob (%d)", fx_val[0][1]);
        run(step); CHECK(fx_val[0][1] == -1, "last owner reverts");
        dt_halt(0, &b->rt[5]); dt_halt(0, &b->rt[4]);
        /* stop reverts; save / load v3 incl. lock triggers; v2 texts load without locks */
        dt_start(b, &b->rt[4], dt_next_step(b)); run(step / 4); dt_halt(0, &b->rt[4]);
        CHECK(fx_val[0][1] == -1, "stop reverts");
        static char t3[DT_TEXT_LEN + 1], tl[DT_LOCK_LEN + 1]; dt_q(b, 4)->st[3].vel = 100; dt_q(b, 4)->st[3].note = DT_NONOTE; dt_q(b, 4)->st[3].fx[3] = 51;
        CHECK(dt_serialize(0, 1, t3) == DT_TEXT_LEN && dt_serialize_locks(0, 1, tl) == DT_LOCK_LEN, "text lengths");
        CHECK(64 + DT_TEXT_LEN < 0x2000 && 64 + DT_LOCK_LEN < 0x2000, "each SQL statement fits the firmware's 8 KB buffer");
        dt_cfg_clear(dt_q(b, 4), 16);
        CHECK(dt_parse(0, 1, t3, DT_TEXT_LEN) && dt_parse_locks(0, 1, tl, DT_LOCK_LEN) && dt_q(b, 4)->st[3].note == DT_NONOTE
              && dt_q(b, 4)->st[3].fx[3] == 51 && dt_q(b, 4)->st[0].fx[1] == 31, "save / load: steps + locks rows");
        dt_parse(0, 1, t3, DT_TEXT_LEN);
        CHECK(!dt_q(b, 4)->st[0].fx[1], "steps row alone: no locks");
        CHECK(!dt_parse_locks(0, 1, tl, DT_LOCK_LEN - 2), "short lock text rejected");
        /* synth engine locks: sy1..4 follow fx1..4 in the parameter list, synth mode only */
        dt_cfg_clear(dt_q(b, 4), 16); b->cur[4] = 0; b->page = 1; b->par = DT_P_FX1 + 3;
        dt_press(0, 0); s0 = &dt_q(b, 4)->st[0];
        turn(2, 1, 0); CHECK(b->par == DT_P_SY1, "white page 2: fx4 -> sy1 (%d)", b->par);
        b->page = 0; turn(2, 3, 0);                     /* sy1: off -> 0 %, then 4 % */
        CHECK(s0->fx[DT_NFX] == 5 && !s0->fx[0], "sy1 sets lock index 4 only (%d)", s0->fx[DT_NFX]);
        b->page = 1; b->par = DT_P_SY1 + DT_NSY - 1; turn(2, 1, 0); CHECK(b->par == DT_P_GATE, "synth list wraps after sy4");
        b->par = DT_P_SY1; b->page = 0;
        if (render) frame("lock trigger with a synth engine lock: SY1 = 4 %");
        for (int k = 0; k < DT_SLOTS; k++) dt_halt(0, &b->rt[k]);
        fx_val[0][DT_NFX] = -1; n_lock = 0; dt_start(b, &b->rt[4], dt_next_step(b)); run(step / 4);
        CHECK(fx_val[0][DT_NFX] == 4 && fx_val[0][0] == -1 && n_lock == 1, "sy1 lock applied to lock index 4 (%d %d)", fx_val[0][DT_NFX], n_lock);
        run(step); CHECK(fx_val[0][DT_NFX] == -1, "sy1 reverts after the gate");
        dt_halt(0, &b->rt[4]);
        static char ts[DT_LOCK_LEN + 1];
        CHECK(dt_serialize_sy(0, 1, ts) == DT_LOCK_LEN && ts[0] == 'S' && 64 + DT_LOCK_LEN < 0x2000, "sy text length");
        CHECK(dt_serialize_locks(0, 1, tl) == DT_LOCK_LEN && !dt_parse_sy(0, 1, tl, DT_LOCK_LEN), "fx text is not an sy text");
        dt_cfg_clear(dt_q(b, 4), 16);
        CHECK(dt_parse_sy(0, 1, ts, DT_LOCK_LEN) && dt_q(b, 4)->st[0].fx[DT_NFX] == 5 && !dt_q(b, 4)->st[0].fx[0], "sy save / load");
        CHECK(!dt_parse_sy(0, 1, ts, DT_LOCK_LEN - 2), "short sy text rejected");
        {                                               /* drum mode: no synth locks (list and playback) */
            dt_bank_t *d = &dt[1]; dt_bank_init(d); d->rec = 1; d->page = 1; d->par = DT_P_SY1 - 1;
            dt_turn(1, 2, 1024, 0); CHECK(d->par == DT_P_GATE, "drum list ends at fx4 (%d)", d->par);
            dt_cfg_clear(dt_q(d, 0), 16); dt_q(d, 0)->st[0].vel = 100; dt_q(d, 0)->st[0].note = DT_NONOTE; dt_q(d, 0)->st[0].fx[DT_NFX] = 51; dt_q(d, 0)->st[0].fx[1] = 21;
            fx_val[1][DT_NFX] = fx_val[1][1] = -1;
            dt_start(d, &d->rt[0], dt_next_step(d)); for (long t = 0; t < step * 5 / 4; t += 64) { dt_tick(1, 64, 12000); now += 64; }
            CHECK(fx_val[1][DT_NFX] == -1 && fx_val[1][1] == 20, "drum ignores sy locks, fx lock plays (%d %d)", fx_val[1][DT_NFX], fx_val[1][1]);
            dt_halt(1, &d->rt[0]); d->rec = 0; d->page = 0; d->par = 0;
        }
        b->rec = 0; b->page = 0; b->par = 0;
    }

    if (render) {                                       /* README screenshot: melody, lock triggers, a locked note, running */
        for (int k = 0; k < DT_SLOTS; k++) dt_halt(0, &b->rt[k]);
        dt_recall(0, 0); b->sel = 1; b->rec = 1; b->live = 0; b->page = 0; b->par = DT_P_FX1 + 1;
        dt_cfg_t *q = dt_q(b, 1); dt_cfg_clear(q, 32);
        static const int mel[32] = { 60, 0, 63, 0, 67, 0, -1, 70, 0, 72, 0, 0, 67, -1, 63, 0,
                                     58, 0, 60, 0, -1, 63, 0, 65, 0, 67, 0, -1, 70, 0, 72, 0 };
        for (int i = 0; i < 32; i++) {
            dt_step_t *t = &q->st[i];
            if (!mel[i]) continue;
            t->vel = 100; dt_step_default(t);
            if (mel[i] < 0) { t->note = DT_NONOTE; t->key = 0; t->fx[1] = (uint8_t)(20 + i * 2); }
            else { t->note = (uint8_t)mel[i]; t->key = (uint8_t)(mel[i] - 53); }
        }
        q->st[9].fx[1] = 61;                            /* a note with a lock */
        dt_q(b, 0)->st[0].vel = 100; dt_q(b, 3)->st[0].vel = 100;
        dt_start(b, &b->rt[1], dt_next_step(b)); dt_start(b, &b->rt[0], b->rt[1].start);
        run(44100 / 8 * 5 + 300);
        b->cur[1] = 9;
        frame("showcase");
    }

    if (!render) printf(fails ? "dots_test: %d FAILED\n" : "dots_test: ok\n", fails);
    return fails != 0;
}
