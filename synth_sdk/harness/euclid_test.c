/* Host test of the euclid sequencer (seq/euclid.c).
 *   checks: euclidean patterns, FIFO replacement, start/stop only via red, timing at tempo, gate, rate, octave,
 *   synth/drum banks independent. "render": JSON screen frames for harness/render_euclid.py. */
#include "../seq/euclid.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4];

/* note log */
static int n_on[2], n_off[2], last_on, last_vel, active[2][128], max_active;
static long sample_now, on_at[64], off_at[64]; static int n_log;
void host_eu_note_on(int pos, int n, int v)
{
    n_on[pos]++; last_on = n; last_vel = v; active[pos][n]++;
    int a = 0; for (int i = 0; i < 128; i++) a += active[pos][i]; if (a > max_active) max_active = a;
    if (n_log < 64) on_at[n_log] = sample_now;
}
void host_eu_note_off(int pos, int n) { n_off[pos]++; active[pos][n]--; if (n_log < 64) off_at[n_log++] = sample_now; }

#define MAXL 2048
static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } lines[MAXL]; static int nlines;
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; nlines = 0; }
void *host_raster(void) { return &nlines; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c)
{ (void)r; const op1_pt *p = a, *q = b; if (nlines < MAXL) { lines[nlines].x0 = p->x; lines[nlines].y0 = p->y; lines[nlines].x1 = q->x; lines[nlines].y1 = q->y; lines[nlines].w = w; memcpy(lines[nlines].c, c, 4); nlines++; } }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static void run(int pos, long samples, uint32_t bpm) { for (long s = 0; s < samples; s += 64) { eu_tick(pos, 64, bpm); sample_now += 64; } }
static void turn(int pos, int k, int steps) { eu_turn(pos, k, steps * 1024); }
static void frame(const char *label)
{
    eu_screen_update(0, 16, 0);
    printf("{\"label\": \"%s\", \"nodes\": {}, \"lines\": [", label);
    for (int i = 0; i < nlines; i++) printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", lines[i].x0, lines[i].y0, lines[i].x1, lines[i].y1, lines[i].w, lines[i].c[0], lines[i].c[1], lines[i].c[2], lines[i].c[3]);
    printf("]}\n");
}

int main(int argc, char **argv)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    int render = argc > 1 && !strcmp(argv[1], "render");

    /* key_down's pitch (op1_synth.h op1_seq_key_note): the keyboard adds the octave, R246's USB MIDI note-on
     * (0x01A858D8..0x01A85C16) not in synth mode, drums always */
    CHECK(op1_seq_key_note(0, 7, 1, 0x019AC100) == 0x35 + 7 + 12 && op1_seq_key_note(0, 7, 1, 0x01A85BF2) == 0x35 + 7, "key note: synth");
    CHECK(op1_seq_key_note(1, 7, 1, 0x01A85BF2) == 0x35 + 7 + 12 && op1_seq_key_note(0, 7, -1, 0) == 0x35 + 7 - 12, "key note: drums, octave -1");
    CHECK(op1_seq_key_note(0, 0, 2, 0x01A85C16) == 0x35 + 24 && op1_seq_key_note(0, 0, 2, 0x01A858D7) == 0x35 + 24, "key note: range ends");

    /* patterns: pulse count, even spread, E(3,8) = x..x..x. */
    for (int n = 1; n <= 16; n++) for (int p = 0; p <= n; p++) {
        eu_seq_t q = { .steps = (uint8_t)n, .pulses = (uint8_t)p };
        int c = 0, maxgap = 0, mingap = 99, last = -1, first = -1;
        for (int i = 0; i < n; i++) if (eu_hit(&q, i)) { if (last >= 0) { int g = i - last; if (g > maxgap) maxgap = g; if (g < mingap) mingap = g; } else first = i; last = i; c++; }
        if (c > 1) { int g = first + n - last; if (g > maxgap) maxgap = g; if (g < mingap) mingap = g; }
        CHECK(c == p, "E(%d,%d) has %d pulses", p, n, c);
        CHECK(c < 2 || maxgap - mingap <= 1, "E(%d,%d) uneven gaps %d..%d", p, n, mingap, maxgap);
    }
    { eu_seq_t q = { .steps = 8, .pulses = 1, .rot = 1 };          /* rotation right = clockwise (later steps) */
      CHECK(!eu_hit(&q, 0) && eu_hit(&q, 1), "rot 1: pulse moved to step 2"); }
    { eu_seq_t q = { .steps = 8, .pulses = 3 }; char s[9]; for (int i = 0; i < 8; i++) s[i] = eu_hit(&q, i) ? 'x' : '.'; s[8] = 0;
      CHECK(!strcmp(s, "x..x..x.") || !strcmp(s, "x.x..x.."), "E(3,8) = %s", s); }

    /* slots: Shift + white selects, a key assigns its note to the selected slot, nothing starts by itself */
    turn(0, 3, 1);
    CHECK(!eu[0].s[0].running, "slot without note started");
    eu_key(0, 60);
    CHECK(eu[0].sel == 0 && eu[0].s[0].used && eu[0].s[0].note == 60, "key -> slot 0 note %d", eu[0].s[0].note);
    eu_select(0, 1024); eu_select(0, 1024);
    CHECK(eu[0].sel == 2, "select -> %d", eu[0].sel);
    eu_key(0, 62); eu_key(0, 63);
    CHECK(eu[0].s[2].note == 63 && eu[0].s[0].note == 60 && !eu[0].s[1].used, "assign: slot2 %d slot0 %d", eu[0].s[2].note, eu[0].s[0].note);
    for (int k = 0; k < 3; k++) eu_select(0, 1024);
    CHECK(eu[0].sel == 5, "select -> 6 (%d)", eu[0].sel);
    eu_select(0, 1024);
    CHECK(eu[0].sel == 0, "select wraps 6 -> 1 (%d)", eu[0].sel);
    eu_select(0, -1024);
    CHECK(eu[0].sel == 5, "select wraps 1 -> 6 (%d)", eu[0].sel);
    eu_select(0, 1024);
    run(0, 44100, 12000);
    CHECK(n_on[0] == 0, "sequencers started without red (%d notes)", n_on[0]);
    eu_select(0, 1024); eu_select(0, 1024); eu_select(0, 1024); eu_select(0, 1024);   /* slot 4 */

    /* start with red, timing: 120 bpm, 1/16, 16 steps, 16 pulses -> 8 notes per second */
    eu_key(0, 64);                                      /* slot 4 gets note 64 */
    eu_seq_t *q = &eu[0].s[eu[0].sel];
    turn(0, 1, 12);                                     /* pulses 4 -> 16 */
    CHECK(q->pulses == 16, "pulses %d", q->pulses);
    turn(0, 3, 1);
    CHECK(q->running, "red right did not start");
    n_on[0] = n_off[0] = 0; n_log = 0;
    run(0, 44100 * 2, 12000);
    CHECK(n_on[0] == 16 || n_on[0] == 15, "120 bpm 1/16: %d notes in 2 s (expect 16)", n_on[0]);
    double gap = (on_at[5] - on_at[4]) / 44100.0, gl = (off_at[4] - on_at[4]) / 44100.0;
    CHECK(fabs(gap - 0.125) < 0.003, "step %.4f s (expect 0.125)", gap);
    CHECK(fabs(gl - 0.0625) < 0.003, "gate %.4f s (expect 0.0625 = 50 %%)", gl);
    CHECK(last_on == 64, "note %d", last_on);
    /* page 2: rate 1/8, gate 100 %, octave +1 */
    eu_press(0, 0); turn(0, 0, -2); eu_press(0, 0);    /* 1/16 -> 1/8T -> 1/8 */
    CHECK(q->rate == 1, "rate %d", q->rate);
    eu_press(0, 2); turn(0, 2, 1); eu_press(0, 2);
    n_on[0] = 0; n_log = 0; run(0, 44100 * 2, 12000);
    CHECK(n_on[0] == 8 || n_on[0] == 7, "1/8: %d notes in 2 s (expect 8)", n_on[0]);
    CHECK(last_on == 76, "octave +1: note %d", last_on);
    /* pulses 4 of 16 -> a quarter of the steps */
    turn(0, 1, -12); n_on[0] = 0; run(0, 44100 * 4, 12000);
    CHECK(n_on[0] == 4, "4/16 at 1/8: %d notes in 4 s (expect 4)", n_on[0]);
    /* stop: note off, no more notes */
    turn(0, 3, -1);
    int act = 0; for (int i = 0; i < 128; i++) act += active[0][i];
    n_on[0] = 0; run(0, 44100, 12000);
    CHECK(!q->running && n_on[0] == 0 && act == 0, "stop: running %d, notes %d, hanging %d", q->running, n_on[0], act);
    /* 6 running at 1/32 16/16: never more than 6 notes at once */
    for (int k = 0; k < 6; k++) { eu[0].sel = k; eu_key(0, 60 + k); turn(0, 1, 16); eu_press(0, 0); turn(0, 0, 5); eu_press(0, 0); turn(0, 3, 1); }
    max_active = 0; run(0, 44100, 12000);
    CHECK(max_active <= 6, "max simultaneous notes %d", max_active);
    for (int k = 0; k < 6; k++) { eu[0].sel = k; turn(0, 3, -1); }
    /* Shift + red: all slots with a note start / stop together, empty slots stay off */
    eu_run_all(0, 1024);
    { int nr = 0; for (int k = 0; k < 6; k++) nr += eu[0].s[k].running; CHECK(nr == 6, "run all: %d running", nr); }
    eu_run_all(0, -1024);
    { int nr = 0; for (int k = 0; k < 6; k++) nr += eu[0].s[k].running; CHECK(nr == 0, "stop all: %d running", nr); }
    /* Shift + C#: toggle, starts all at once (first step on the next tick), stops if any runs */
    CHECK(eu_shift_key(0, 20) == 2, "Shift+C# starts");
    { int nr = 0; for (int k = 0; k < 6; k++) nr += eu[0].s[k].running && eu[0].s[k].start == (uint32_t)(eu[0].tick_q16 >> 16);
      CHECK(nr == 6, "C# start now: %d", nr); }
    eu[0].s[2].running = 0;
    CHECK(eu_shift_key(0, 20) == 1, "Shift+C# stops when any runs");
    { int nr = 0; for (int k = 0; k < 6; k++) nr += eu[0].s[k].running; CHECK(nr == 0, "C# stop: %d running", nr); }
    /* mute: slot keeps running (steps advance), plays nothing; Shift-key map; mute-all; persistence round trip */
    {
        eu[0].sel = 0; eu_key(0, 60); eu[0].s[0].pulses = 16; turn(0, 3, 1);
        eu_seq_t *m = &eu[0].s[0];
        run(0, 44100 / 2, 12000);
        int before = n_on[0];
        CHECK(eu_shift_key(0, 14) == 1 && m->muted, "Shift+G mutes slot 1");
        int act = 0; for (int i = 0; i < 128; i++) act += active[0][i];
        int16_t st0 = m->step;
        run(0, 44100, 12000);
        CHECK(n_on[0] == before && m->step > st0, "muted: %d new notes, step %d -> %d", n_on[0] - before, st0, m->step);
        CHECK(eu_shift_key(0, 14) == 1 && !m->muted, "Shift+G unmutes");
        run(0, 44100 / 2, 12000);
        CHECK(n_on[0] > before, "unmuted plays again");
        CHECK(eu_shift_key(0, 15) == 0 && eu_shift_key(0, 3) == 0, "black / low keys ignored");
        CHECK(eu_shift_key(0, 23) == 1 && eu[0].s[5].muted, "Shift+E -> slot 6");
        eu_shift_key(0, 22);
        { int nm = 0; for (int k = 0; k < 6; k++) nm += eu[0].s[k].muted; CHECK(nm == 6, "mute all: %d", nm); }
        eu_shift_key(0, 22);
        { int nm = 0; for (int k = 0; k < 6; k++) nm += eu[0].s[k].muted; CHECK(nm == 0, "unmute all: %d", nm); }
        eu_shift_key(0, 16);                                        /* slot 2 muted for the round trip */
        char buf[1400]; eu_bank_t saved = eu[0];
        int len = eu_serialize(0, buf, sizeof buf);
        memset(&eu[0], 0, sizeof eu[0]);
        CHECK(eu_parse(0, buf, len), "parse failed: %s", buf);
        int same = eu[0].sel == saved.sel;
        for (int k = 0; k < 6; k++) {
            const eu_seq_t *x = &eu[0].s[k], *y = &saved.s[k];
            same &= x->used == y->used && x->note == y->note && x->steps == y->steps && x->pulses == y->pulses &&
                    x->rot == y->rot && x->rate == y->rate && x->gate == y->gate && x->oct == y->oct && x->muted == y->muted &&
                    !x->running;
        }
        CHECK(same, "round trip differs: %s", buf);
        CHECK(!eu_parse(0, "1,2,x", 5) && !eu_parse(0, "1,2,3", 5), "bad text accepted");
        CHECK(host_eu_changed > 0, "changes not reported");
        for (int k = 0; k < 6; k++) eu[0].s[k].running = 0;
    }
    /* patterns: Shift + F G A B, settings per pattern, running slots keep phase, empty slots stop, persistence */
    {
        memset(&eu[0], 0, sizeof eu[0]);
        eu_key(0, 60); turn(0, 1, 3); eu_shift_key(0, 20);              /* P1: slot 1 C4 7/16, running */
        run(0, 44100 / 2, 12000);
        uint32_t st = eu[0].s[0].start;
        CHECK(eu_shift_key(0, 2) == 1 && eu[0].pat == 1, "Shift+G -> pattern 2");
        CHECK(!eu[0].s[0].used && eu[0].s[0].running, "empty slot in P2 keeps running (silent)");
        { int b4 = n_on[0]; run(0, 44100 / 2, 12000); CHECK(n_on[0] == b4, "empty running slot plays %d notes", n_on[0] - b4); }
        eu_key(0, 64); turn(0, 1, -2);                                  /* P2: slot 1 E4 2/16 */
        eu_shift_key(0, 0);
        CHECK(eu[0].pat == 0 && eu[0].s[0].note == 60 && eu[0].s[0].pulses == 7 && eu[0].s[0].running, "back to P1: note %d pulses %d", eu[0].s[0].note, eu[0].s[0].pulses);
        uint32_t st2 = eu[0].s[0].start; (void)st;
        eu_shift_key(0, 2);
        CHECK(eu[0].s[0].note == 64 && eu[0].s[0].pulses == 2 && eu[0].s[0].running && eu[0].s[0].start == st2, "P2 kept: note %d, phase kept", eu[0].s[0].note);
        int act = 0; for (int i = 0; i < 128; i++) act += active[0][i];
        CHECK(act == 0, "recall left %d notes hanging", act);
        char buf[1400]; int len = eu_serialize(0, buf, sizeof buf);
        CHECK(len > 400 && len < 1300, "text length %d", len);
        memset(&eu[0], 0, sizeof eu[0]);
        CHECK(eu_parse(0, buf, len) && eu[0].pat == 1 && eu[0].s[0].note == 64 && eu[0].pats[0][0].note == 60 && eu[0].pats[0][0].pulses == 7,
              "pattern round trip");
        memset(&eu[0], 0, sizeof eu[0]);
        const char *old = "2,1,60,0,16,5,0,3,50,2,0,0,0,0,16,4,0,3,50,2,0,0,0,0,16,4,0,3,50,2,0,0,0,0,16,4,0,3,50,2,0,0,0,0,16,4,0,3,50,2,0,0,0,0,16,4,0,3,50,2,0";
        CHECK(eu_parse(0, old, (int)strlen(old)) && eu[0].pat == 0 && eu[0].sel == 2 && eu[0].s[0].note == 60 && eu[0].pats[1][0].steps == 16 && !eu[0].pats[1][0].used,
              "old one-pattern text");
        for (int k = 0; k < 6; k++) eu[0].s[k].running = 0;
    }
    /* level: red press -> page 2, turn changes velocity (per slot, per pattern), notes carry it; v2 text loads */
    {
        memset(&eu[0], 0, sizeof eu[0]);
        eu_key(0, 60); turn(0, 1, 12); eu_shift_key(0, 20);
        run(0, 44100 / 4, 12000);
        CHECK(last_vel == 100, "default velocity %d", last_vel);
        eu_press(0, 3); turn(0, 3, -10); eu_press(0, 3);
        CHECK(eu[0].s[0].vel == 60 && eu[0].s[0].running, "level 60, still running: %d", eu[0].s[0].vel);
        run(0, 44100 / 4, 12000);
        CHECK(last_vel == 60, "note velocity %d", last_vel);
        eu_press(0, 3); turn(0, 3, -40); turn(0, 3, 80); eu_press(0, 3);
        CHECK(eu[0].s[0].vel == 127, "level clamps 1..127: %d", eu[0].s[0].vel);
        eu_shift_key(0, 2); CHECK(eu[0].s[0].vel == 100, "level per pattern");
        eu_shift_key(0, 0); CHECK(eu[0].s[0].vel == 127, "level back");
        char buf[1400]; int len = eu_serialize(0, buf, sizeof buf);
        memset(&eu[0], 0, sizeof eu[0]);
        CHECK(eu_parse(0, buf, len) && eu[0].s[0].vel == 127 && eu[0].pats[1][0].vel == 100, "level round trip");
        char v2[1400]; int o = sprintf(v2, "0,0");
        for (int k = 0; k < 24; k++) o += sprintf(v2 + o, ",%d,61,8,16,4,0,3,50,2,0", k == 0);
        memset(&eu[0], 0, sizeof eu[0]);
        CHECK(eu_parse(0, v2, o) && eu[0].s[0].note == 61 && eu[0].s[0].vel == 100, "v2 text (no level) loads");
        for (int k = 0; k < 6; k++) eu[0].s[k].running = 0;
    }
    /* lift / drop / scissors: copy / paste / cut the selected slot (settings only, run state kept) */
    {
        memset(&eu[0], 0, sizeof eu[0]);
        eu_key(0, 62); turn(0, 1, 3); turn(0, 2, 2); eu_press(0, 3); turn(0, 3, -5); eu_press(0, 3);
        eu_clip(0, 1, 0);
        CHECK(eu[0].s[0].note == 62, "paste with empty clipboard changes nothing");
        eu_clip(0, 0, 0);
        eu_select(0, 1024); eu_shift_key(0, 20);                          /* slot 2 empty, all running */
        eu_clip(0, 1, 0);
        const eu_seq_t *d = &eu[0].s[1];
        CHECK(d->used && d->note == 62 && d->pulses == 7 && d->rot == 2 && d->vel == 80 && d->running, "paste: note %d pulses %d vel %d", d->note, d->pulses, d->vel);
        eu_clip(0, 2, 0);
        CHECK(!d->used && d->pulses == 4 && d->vel == 100 && d->running, "cut clears, keeps running");
        eu_select(0, 1024); eu_clip(0, 1, 0);
        CHECK(eu[0].s[2].note == 62 && eu[0].s[2].pulses == 7, "paste after cut = cut slot");
        run(0, 44100 / 4, 12000);
        for (int k = 0; k < 6; k++) eu[0].s[k].running = 0;
        /* Shift: whole pattern; slot clipboard untouched */
        eu_clip(0, 0, 1);                                                /* copy P1 (slots 1..3 have C... ) */
        eu_shift_key(0, 2);                                              /* P2 empty */
        eu_clip(0, 1, 1);
        CHECK(eu[0].s[0].note == 62 && eu[0].s[1].used == 0 && eu[0].s[2].note == 62 && eu[0].s[2].pulses == 7, "pattern paste");
        eu_clip(0, 2, 1);
        { int u = 0; for (int k = 0; k < 6; k++) u += eu[0].s[k].used; CHECK(u == 0, "pattern cut: %d slots left", u); }
        eu_select(0, 1024); eu_clip(0, 1, 0);
        CHECK(eu[0].s[eu[0].sel].note == 62 && eu[0].s[eu[0].sel].pulses == 7, "slot clipboard kept");
        eu_clip(0, 1, 1);
        CHECK(eu[0].s[0].note == 62 && eu[0].s[2].note == 62, "paste after pattern cut");
        eu_all_off(0);
    }
    /* drum bank independent */
    n_on[1] = 0; eu_key(1, 53); turn(1, 3, 1); run(1, 44100, 12000);
    CHECK(n_on[1] > 0 && eu[1].s[0].note == 53 && eu[0].s[0].note != 53, "drum bank: %d notes", n_on[1]);
    /* MIDI START: running slots restart at step 0 on the next tick */
    {
        eu_bank_t *b = &eu[0];
        int k = b->sel; eu_seq_t *q = &b->s[k];
        if (!q->used) eu_key(0, 60);
        q->pulses = q->steps; q->muted = 0;
        if (!q->running) eu_turn(0, 3, 1024);
        run(0, 44100 / 3, 12000);
        eu_midi_start();
        eu_advance(0, 1u << 14);
        CHECK(q->step == 0, "MIDI START: euclid step 0 on the next tick (%d)", q->step);
    }
    if (!render) { printf(fails ? "%d FAILED\n" : "OK: all euclid checks pass\n", fails); return fails != 0; }

    /* ---- screen frames ---- */
    memset(eu, 0, sizeof eu);
    frame("start: slot 1 selected, no note yet");
    eu_key(0, 60); turn(0, 1, 1);                        /* E(5,16) */
    frame("C4 pressed: slot 1 plays C4 (stopped, 5/16)");
    turn(0, 3, 1); run(0, 44100 * 6 / 10, 12000);
    frame("slot 1 running, playhead");
    eu_select(0, 1024); eu_key(0, 64); turn(0, 0, -8); turn(0, 1, -1); turn(0, 3, 1);        /* E(3,8) */
    eu_select(0, 1024); eu_key(0, 67); turn(0, 0, -4); turn(0, 1, 1); turn(0, 2, 2); turn(0, 3, 1);
    run(0, 44100 * 3 / 10, 12000);
    frame("shift+white -> slot 3: G4 (12 steps, rotation 2)");
    eu_select(0, 1024); eu_select(0, 1024);
    frame("shift+white -> slot 5: empty (press a key)");
    eu_key(0, 74); eu_press(0, 0); eu_press(0, 1); turn(0, 1, 4); turn(0, 3, 1);
    run(0, 44100 * 4 / 10, 12000);
    frame("slot 5: D5 assigned + started, page 2 (rate, gate)");
    eu_shift_key(0, 16);                                        /* Shift+A: mute slot 2 */
    eu_select(0, -1024); eu_select(0, -1024); eu_select(0, -1024);
    run(0, 44100 * 2 / 10, 12000);
    frame("Shift+A: slot 2 muted (running, dim dots), selected");
    eu_run_all(0, -1024);
    frame("shift+red left: all stopped");
    eu_shift_key(0, 4);
    frame("Shift+A (low): pattern 3, empty");
    eu_key(0, 72); turn(0, 1, 2); eu_shift_key(0, 20); run(0, 44100 * 3 / 10, 12000);
    frame("pattern 3: C5 assigned, Shift+C# started");
    eu_press(0, 0); eu_press(0, 1);
    frame("level 100 (default), page 1");
    eu_press(0, 3); turn(0, 3, -6);
    frame("red press: page 2 = level, turned to 76 (triangle)");
    return 0;
}
