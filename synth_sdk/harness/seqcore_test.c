/* Host test of the sequencers' note output (seq/seq_core.c) and the synth octave:
 *   the note-on adds the live synth octave to a stored synth pitch (op1_seq_key_note stores octave 0), so a running
 *   sequence follows the octave arrows; a held note keeps its number across an octave step (the queued renumbering
 *   is dropped while one of ours is the synth sequencer), so its note-off still finds it; drums keep their pitch.
 *   cc -DOP1_HOST -fsanitize=address,undefined -I.. seqcore_test.c -o build/seqcore_test */
#include "../seq/seq_core.c"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* VoiceCtl stand-in: its held-note list (0x01DFE734: 20-byte entries, number, velocity, flag byte 8) and R246's
 * queued renumbering (0x01A860C0: every flagged held note + 12 a step, off the keyboard: released) */
int host_synth_oct, host_drum_oct;
int32_t host_oct_queue;
static int ours_live;                                  /* one of our sequencers is the synth sequencer */
int seq_ours_live(void) { return ours_live; }
static uint8_t list[64 * 20]; static int count, last_param;
uint8_t *host_vc_list = list; int *host_vc_count = &count;
void host_midi_off(int n) { (void)n; }
void host_midi_on(int n, int v) { (void)n; (void)v; }
static void add(int midi, int vel) { int32_t *e = (int32_t *)(list + count * 20); memset(e, 0, 20); e[0] = midi; e[1] = vel; ((uint8_t *)e)[8] = 1; count++; }
void host_vc_down(int32_t *n, int param) { add(n[0], n[1]); last_param = param; }
void host_vc_up(int32_t *n)                            /* the first held entry with that number */
{
    for (int i = 0; i < count; i++) if (((int32_t *)(list + i * 20))[0] == n[0]) {
        memmove(list + i * 20, list + (i + 1) * 20, (size_t)(count - i - 1) * 20); count--; return;
    }
}
static int held(int m) { int k = 0; for (int i = 0; i < count; i++) k += ((int32_t *)(list + i * 20))[0] == m; return k; }
static int any_held(void) { return count > 0; }
static void step(int steps)                            /* an octave arrow while the sequencer runs (octave set first) */
{
    host_synth_oct += steps;
    eu_fw_octave_queue(steps);
}
static void seq_tick(void)                             /* R246 applies the queue before the next player tick */
{
    const int d = host_oct_queue * 12;
    host_oct_queue = 0;
    for (int i = 0; i < count; ) {
        int32_t *e = (int32_t *)(list + i * 20);
        if (d && ((uint8_t *)e)[8]) {
            if (e[0] + d < 0 || e[0] + d > 127) { int32_t n[4] = { e[0], 0, 0, 0 }; host_vc_up(n); continue; }
            e[0] += d;
        }
        i++;
    }
}
static void kb_down(int key) { add(key + 0x35 + host_synth_oct * 12, 127); }          /* the stock keyboard path */
static void kb_up(int key) { int32_t n[4] = { key + 0x35 + host_synth_oct * 12, 0, 0, 0 }; host_vc_up(n); }   /* stock key_up: at the current octave */

int main(void)
{
    /* a running sequence follows the octave: the stored pitch (octave 0) plays at the current synth octave */
    ours_live = 1;
    int note = op1_seq_key_note(0, 7, 1, 0x019AC100);                    /* key 7 recorded at octave +1 */
    CHECK(note == 0x35 + 7, "stored at octave 0: %d", note);
    host_synth_oct = 1;
    int id = eu_fw_note_on(0, note, 7, 100);
    CHECK(id == 0x35 + 7 + 12 && held(id), "plays as recorded: %d", id);
    eu_fw_note_off(id);
    step(1); seq_tick();
    id = eu_fw_note_on(0, note, 7, 100);
    CHECK(id == 0x35 + 7 + 24 && held(id), "next note an octave up: %d", id);
    eu_fw_note_off(id);
    CHECK(!any_held(), "nothing left");

    /* a USB MIDI note sounds as played at the octave it was recorded at (PR #29) */
    host_synth_oct = 2;
    note = op1_seq_key_note(0, 60 - 0x35, 2, 0x01A85BF4);
    id = eu_fw_note_on(0, note, 0, 100);
    CHECK(id == 60, "MIDI note plays its pitch: %d", id);
    eu_fw_note_off(id);

    /* a held note keeps its number across an octave step, so its note-off finds it (before: stuck in POLY) */
    host_synth_oct = 0;
    id = eu_fw_note_on(0, 0x35 + 7, 7, 100);
    step(1); seq_tick();
    CHECK(held(id) && host_oct_queue == 0, "held note not renumbered");
    eu_fw_note_off(id);
    CHECK(!any_held(), "held note released after the step");
    step(-2); seq_tick();                                                 /* down two: same */
    id = eu_fw_note_on(0, 0x35 + 7, 7, 100);
    CHECK(id == 0x35 + 7 - 12, "octave -1: %d", id);
    step(1); seq_tick();
    eu_fw_note_off(id);
    CHECK(!any_held(), "released after a step up");

    /* a keyboard key held through the step while ours runs: renumbered at once (as R246 without a sequencer), so the
     * stock key_up at the new octave finds it; our held note keeps its number (issue #42's trade-off, closed) */
    host_synth_oct = 0;
    id = eu_fw_note_on(0, 0x35 + 2, 2, 100);
    kb_down(9);
    step(1); seq_tick();
    CHECK(held(0x35 + 9 + 12) == 1 && held(id) == 1, "keyboard key renumbered, ours kept");
    kb_up(9);
    CHECK(held(0x35 + 9 + 12) == 0, "keyboard key released after the step");
    eu_fw_note_off(id);
    CHECK(!any_held(), "nothing left after key + sequencer");
    host_synth_oct = 3; kb_down(23); step(2); seq_tick();                 /* 76 + 36 = 112, + 24 = 136: off the keyboard, released */
    CHECK(!any_held(), "a key past 127 released");
    host_synth_oct = 0;

    /* a stock sequencer: R246 keeps its queue (its players release by key at the current octave) */
    ours_live = 0; host_synth_oct = 0;
    step(1); step(-2);
    CHECK(host_oct_queue == -1, "stock: queued %d", host_oct_queue);
    host_oct_queue = 0;

    /* out of range after the octave add: not played */
    ours_live = 1; host_synth_oct = 4;
    CHECK(eu_fw_note_on(0, 0x35 + 23 + 12, 23, 100) == -1 && !any_held(), "past 127 not played");
    host_synth_oct = 0;

    /* drums: the pad at its own pitch, the synth octave does not matter (PR #28) */
    host_synth_oct = 3; host_drum_oct = 1;
    note = op1_seq_key_note(1, 5, 1, 0);                                  /* pad 5 at drum octave +1 */
    id = eu_fw_note_on(1, note, 5, 100);
    CHECK(id == 0x35 + 5 && last_param == 0, "drum pad: id %d param %d", id, last_param);
    host_drum_oct = 0;
    eu_fw_note_off(eu_fw_note_on(1, note, 5, 100));
    CHECK(last_param == 12, "drum pad one up at drum octave 0: %d", last_param);
    eu_fw_note_off(id);
    CHECK(!any_held(), "drums released");

    if (fails) { printf("seqcore: %d failed\n", fails); return 1; }
    printf("seqcore: ok\n");
    return 0;
}
