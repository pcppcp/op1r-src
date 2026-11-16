/* Core part of the sequencers (the sequencers themselves are plugin files, docs/44): what they and other core code
 * share. The one firmware copy of seq_text.h (stroke text, lines, oracle / peal DB persistence; exported to plugins),
 * the VoiceCtl note output of euclid / dots / oracle / peal, and the "start with REC held" flag dispatcher.c turns
 * into a PLAY press. */
#include "../op1_synth.h"
#define OCC_TEXT_IMPL
#include "seq_text.h"

volatile uint8_t eu_rec_play;                           /* Shift + C# started a sequencer with REC held */

/* notes go through VoiceCtl like the stock sequencers (ok_player). The synth octave is a live setting, as on the
 * stock sequencers: a slot stores a synth pitch at octave 0 (op1_seq_key_note) and the note-on adds the current synth
 * octave, so the octave arrows (and a slot change) move what a sequencer plays from its next note on. Drum slots store
 * the absolute pitch (key + 0x35 + drum octave * 12).
 *  synth: record {midi = note + synth octave * 12, vel, send midi = 1, 0} -> key_down 0x01A85750 -> note_on_synth
 *         (uses midi as is). The returned id is that midi: the note-off releases the number the note-on started.
 *  drums: note_on_drum takes the SOUND from record midi - 0x35 (key 0..23) and the PITCH from the param: the pad
 *         plays 2^(param / 12) times its own pitch, and key_down adds the current drum octave * 12 to the param. A
 *         pad key sends param 0 (the stock players send octave offsets only), so key + 0x35 + octave * 12 is the
 *         pad's unshifted pitch -> param = note - (key + 0x35) - current drum octave * 12 (0 at the octave the note
 *         was recorded at). */
#ifdef OP1_HOST                                                    /* harness/seqcore_test.c */
void host_vc_down(int32_t *n, int param);
void host_vc_up(int32_t *n);
extern int host_synth_oct, host_drum_oct;
extern int32_t host_oct_queue;
#define VC_DOWN   host_vc_down
#define VC_UP     host_vc_up
#define DRUM_OCT()  host_drum_oct
#define SYNTH_OCT() host_synth_oct
#define OCT_QUEUE host_oct_queue
#else
#define VC_DOWN   ((void (*)(int32_t *, int))0x01A85750)
#define VC_UP     ((void (*)(int32_t *))0x01A85840)
#define DRUM_OCT()  (((const int *)0x01DFE500)[*(volatile int *)0xFF8016D8 & 7])
#define SYNTH_OCT() (((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7])
#define OCT_QUEUE (*(volatile int32_t *)0x018B6208)                /* PlayerManager: queued synth octave steps */
#endif
static uint8_t ours[128];                                          /* our sounding notes per number (note on - off) */
int eu_fw_note_on(int pos, int note, int key, int vel)
{
    int midi = pos ? key + 0x35 : note + SYNTH_OCT() * 12, param = pos ? note - (key + 0x35) - DRUM_OCT() * 12 : 0;
    if (midi < 0 || midi > 127) return -1;
    int32_t n[4] = { midi, vel, 0, 0 };
    ((uint8_t *)n)[8] = 1;
    VC_DOWN(n, param);
    if (ours[midi] < 255) ours[midi]++;                            /* (renumber_others leaves it) */
    return midi;                                                   /* note-off uses this */
}
void eu_fw_note_off(int midi)
{
    if (midi >= 0 && midi < 128 && ours[midi]) ours[midi]--;
    int32_t n[4] = { midi, 0, 1, 0 };
    ((uint8_t *)n)[8] = 1;
    VC_UP(n);
}

/* Synth octave and slot steps while the sequencer runs (synth_octave_step 0x01A86158 and slot select, each a CALL
 * 0x01A0CEFC at 0x01A86236 / 0x01A86578; hook.S hook_seqoct0 / 1). synth_octave_step has already set the new octave;
 * with no sequencer running it renumbers every held VoiceCtl note (flag byte 8 set) by 12 a step at once, while one
 * runs it queues the steps in 0x018B6208 and the sequencer tick renumbers them before the players (0x01A860C0). The
 * stock players release by key at the current octave, so they find the new number; ours release by the number they
 * started (eu_fw_note_off), so a renumbered note of ours was never released (issue #42: stuck in POLY).
 * With one of ours as the synth sequencer the step is not queued: our held notes keep their numbers until their
 * note-off, and the next ones get the new octave (eu_fw_note_on). Every other held note (a keyboard key held through
 * the step) is renumbered here at once, as R246 does without a sequencer, so the stock key_up still finds it.
 * shortcut: a keyboard key on the same number as a note of ours counts as ours (not renumbered); upgrade if seen. */
int seq_ours_live(void);                                           /* dispatcher.c */
#ifdef OP1_HOST
extern uint8_t *host_vc_list; extern int *host_vc_count;
void host_midi_off(int n); void host_midi_on(int n, int v);
#define VC_LIST   host_vc_list
#define VC_COUNT  (*host_vc_count)
#define MIDI_OFF  host_midi_off
#define MIDI_ON   host_midi_on
#else
#define VC_LIST   ((uint8_t *)0x01DFE734)                          /* held notes: 20 bytes each (midi, vel, flag byte, ..) */
#define VC_COUNT  (*(volatile int32_t *)(0x01DFE734 + 0xA00))
#define MIDI_OFF  ((void (*)(int))0x019E2DC0)                     /* midi_send_note_off */
#define MIDI_ON   ((void (*)(int, int))0x019E2E64)                /* midi_send_note */
#endif
static void renumber_others(int steps)                             /* as 0x01A860C0, but not our notes */
{
    for (int i = 0; i < VC_COUNT; ) {
        int32_t *e = (int32_t *)(VC_LIST + i * 20);
        const int m = e[0];
        if (!((uint8_t *)e)[8] || (m >= 0 && m < 128 && ours[m])) { i++; continue; }
        const int nm = m + steps * 12;
        if (nm < 0 || nm > 127) {                                  /* off the keyboard: released (the list shifts) */
            int32_t n[4] = { e[0], e[1], e[2], e[3] };
            VC_UP(n);
            continue;
        }
        MIDI_OFF(m); e[0] = nm; MIDI_ON(nm, e[1]);
        i++;
    }
}
void eu_fw_octave_queue(int steps)
{
    if (!seq_ours_live()) { OCT_QUEUE += steps; return; }
    renumber_others(steps);
}
