/* MIDI track: a 5th tape track holding notes instead of audio. TAPE held + T1 selects it (T1..T4 select an audio track
 * again), TAPE held + T1 while it is selected mutes / unmutes it (Shift + TAPE is stock's ERASE TAPE; stock Shift +
 * T-key stays the mute of the audio tracks). While it is selected the track box shows M (grey while muted).
 * Notes are stored at tape positions and played through VoiceCtl (as the keyboard and the sequencers do), so they
 * play the current instrument: change the synth / slot, turn its knobs or go to drum mode while the track plays
 * (drum mode: notes outside the 24 drum keys are folded in by octaves). Track mutes are not saved (as stock).
 *
 * Recording: with the MIDI track selected, hold REC (or Shift + REC: latched until STOP) while the tape plays. Every
 * note that reaches VoiceCtl is recorded at the playhead: keyboard, MIDI in and the sequencers. Overdub: existing
 * notes stay and keep playing. The recorded stretch becomes a MIDI clip (merged with the clips it touches).
 * Playback: once per audio block (64 samples) the notes passed since the last block are played: forward, reverse
 * (a note starts at its end and stops at its start), varispeed and across the loop wrap. Stop, a locate and the loop
 * wrap release the notes the track holds; notes the track did not start are never released by it.
 * Editing (MIDI track selected, tape screen): scissors splits the clip at the playhead, lift takes the clip under the
 * playhead into the MIDI clipboard (removes it), drop pastes the clipboard at the playhead (overdub).
 * Knob automation: while recording, the synth knobs (synth mode: the 4 knobs and the page-2 words of custom engines)
 * and the 4 knobs of the mode's instrument FX are sampled every block; changes are recorded (at most one value per
 * knob every ~20 ms, plus the value it had when the take began) and played back like the notes, on the current
 * synth / FX. FX values are kept as 0..100 % of the FX type's range, so they fit another FX too. A knob that changes
 * by anything else than the track during playback (turned by hand, MIDI CC, a dots lock) is taken over: the track
 * leaves it alone until STOP. Recording a knob replaces its automation from where it was first turned to the end of the
 * take (the whole take once it wrapped around the loop); notes are overdubbed. A knob without automation gets the
 * value it had at the start of the take too, so a loop comes back to it.
 * Changing the slot, the engine, the FX or the mode starts over: nothing taken over, nothing recorded for the switch.
 * Saved in tape.db, table op1rmidi: row 0 the clips, rows 1.. the notes (hex).
 *
 * Threads: mt_block (audio callback, with punch / count-in), mt_vc_down / mt_vc_up (any thread that plays a note:
 * key thread, MIDI, the audio callback for sequencers), mt_key (key thread), mt_service (main loop: load, save),
 * mt_draw (GUI). Changes of the note / clip lists are made with interrupts off, so the audio callback never sees a
 * half-moved list. */
#include "../op1_synth.h"
#ifndef OP1_HOST
#pragma GCC optimize ("Os")                  /* code space (the per-block work is small: a few compares per block) */
#endif

#define MT_MAX      3072                     /* notes (on and off events) */
#define MT_CLIPS    64
#define MT_CLIPMAX  1024                     /* clipboard notes */
#define MT_JUMP     (64 * 16)                /* a position step bigger than this (16x speed) is a jump, not travel */
#define MT_MAXPOS   0xF2BFA0                 /* tape_c::check_pos range (6 min) */
#define MT_MINCLIP  441                      /* 10 ms: shorter pieces are not kept as clips */
#define MT_SPINUP   22050                    /* travel before the tape state says playing (seen: ~4300 samples) */

enum { MT_ON = 1, MT_SEND = 2, MT_AUTO = 4, MT_FRESH = 0x80 };   /* event flags: note on (else off), send MIDI out (as
                                             * recorded), automation (midi = knob, vel | param << 8 = value), this take */
typedef struct { int32_t pos; uint8_t midi, vel, flags; int8_t param; } mt_ev_t;
typedef struct { int32_t start, stop; } mt_clip_t;

#ifdef OP1_HOST
extern int32_t host_tape_state, host_tape_pos, host_tape_speed, host_loop_in, host_loop_out, host_loop_on;
void host_vc_down(int32_t *rec, int param);
void host_vc_up(int32_t *rec);
#define TAPE_STATE  host_tape_state
#define TAPE_POS    host_tape_pos
#define TAPE_SPEED  host_tape_speed
#define LOOP_IN     host_loop_in
#define LOOP_OUT    host_loop_out
#define LOOP_ON     host_loop_on
#define VC_DOWN(r, p) host_vc_down((r), (p))
#define VC_UP(r)    host_vc_up(r)
#define IRQ_OFF()   0
#define IRQ_ON(m)   ((void)(m))
extern int32_t host_sel_track; extern uint8_t host_track_muted[4];
#define SEL_TRACK   host_sel_track
#define TRACK_MUTED host_track_muted
#else
#define SEL_TRACK   (*(volatile int32_t *)0xFF802964)              /* stock selected audio track 0..3 */
#define TRACK_MUTED ((volatile uint8_t *)0xFF8010A0)               /* stock mute byte per track (Shift + T-key) */
#define TAPE_STATE  (*(volatile int32_t *)0x018C17D4)
#define TAPE_POS    (*(volatile int32_t *)0xFF802974)     /* 1/64 samples */
#define TAPE_SPEED  (*(volatile int32_t *)0xFF802970)     /* < 0: reverse */
#define LOOP_IN     (*(volatile int32_t *)0x01B4EF34)
#define LOOP_OUT    (*(volatile int32_t *)0x01B4EF38)
#define LOOP_ON     (*(volatile uint8_t *)0xFF8029C5)
#define VC_DOWN(r, p) (((void (*)(int32_t *, int))0x01A85750)((r), (p)))   /* VoiceCtl key_down (through our hook) */
#define VC_UP(r)    (((void (*)(int32_t *))0x01A85840)(r))
static inline uint32_t mt_irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m)); return m; }
static inline void mt_irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m)); }
#define IRQ_OFF()   mt_irq_off()
#define IRQ_ON(m)   mt_irq_on(m)
#endif

/* automation knobs: 0..7 the synth knob words of the current synth slot (synth mode only; 4 = engine selector: never),
 * 8..11 the knobs of the mode's instrument FX (kept as 0..32767 of the FX type's raw range: read_fx_param asserts on
 * raw words outside it, dots.c) */
#define MT_NT       12
#define MT_THIN     14                       /* blocks (~20 ms) between two recorded values of a knob */
#ifdef OP1_HOST
extern int host_mode, host_fx_type, host_fx_lo[4], host_fx_hi[4];
extern int16_t host_synth_words[8], host_fx_words[4];
#define MODE()          host_mode
#define SLOT()          0
#define SYNTH_WORDS()   host_synth_words
#define FX_WORDS()      host_fx_words
#define FX_TYPE()       host_fx_type
#define FX_RANGE(k, lo, hi) do { (lo) = host_fx_lo[k]; (hi) = host_fx_hi[k]; } while (0)
#else
#define MODE()          (*(volatile int *)0x01DE4C40)                       /* 0 synth, 1 drum */
#define SLOT()          (*(volatile int *)(MODE() == 1 ? 0xFF8016D8 : 0xFF8016D4) & 7)
#define SYNTH_WORDS()   ((volatile int16_t *)(0xFF8013C4 + (*(volatile int *)0xFF8016D4 & 7) * 16))
#define FX_WORDS()      ((volatile int16_t *)((MODE() == 1 ? 0xFF8015C4 : 0xFF801444) + SLOT() * 16))
#define FX_TYPE()       (((volatile int *)(MODE() == 1 ? 0xFF801754 : 0xFF801734))[SLOT()])
#define FX_RANGE(k, lo, hi) do { const int16_t *rg_ = (const int16_t *)(0x01DE5C50 + FX_TYPE() * 0x30 + (k) * 6); \
                                 (lo) = rg_[0]; (hi) = rg_[1]; } while (0)
#endif
static int mt_t_ok(int t)                                     /* knob t can be read / written now */
{
    int m = MODE();
    if (m != 0 && m != 1) return 0;
    if (t < 8) return m == 0 && t != 4;
    int ty = FX_TYPE(), lo, hi;
    if (ty < 0 || ty > 7) return 0;
    FX_RANGE(t - 8, lo, hi);
    return hi > lo;
}
static int32_t mt_t_raw(int t) { return t < 8 ? SYNTH_WORDS()[t] : FX_WORDS()[t - 8]; }
static int32_t mt_t_norm(int t, int32_t raw)                  /* raw word -> stored value */
{
    if (t < 8) return raw;
    int lo, hi;
    FX_RANGE(t - 8, lo, hi);
    int32_t v = (raw - lo) * 32767 / (hi - lo);
    return v < 0 ? 0 : v > 32767 ? 32767 : v;
}
static void mt_t_write(int t, int32_t v)                      /* stored value -> raw word */
{
    if (t < 8) { SYNTH_WORDS()[t] = (int16_t)v; return; }
    int lo, hi;
    FX_RANGE(t - 8, lo, hi);
    if (v < 0) v = 0;
    if (v > 32767) v = 32767;
    FX_WORDS()[t - 8] = (int16_t)(lo + (hi - lo) * v / 32767);
}
static uint32_t mt_ctx(void)                                  /* mode, slot, FX type, engine selector */
{
    int m = MODE();
    return (uint32_t)m | (uint32_t)SLOT() << 2 | (uint32_t)(FX_TYPE() & 15) << 5 |
           (m == 0 ? (uint32_t)(uint16_t)SYNTH_WORDS()[4] << 9 : 0);
}

/* the note list, clipboard and SQL text live on the firmware heap (allocated once at load, ~37 KB: op1r's .bss
 * region is full); until then the track is off (mt_loaded 0) */
#ifdef OP1_HOST
static mt_ev_t mt_ev_buf[MT_MAX], mt_cb_buf[MT_CLIPMAX];
mt_ev_t *mt_ev = mt_ev_buf;
static mt_ev_t *mt_cb = mt_cb_buf;
#else
mt_ev_t *mt_ev;
static mt_ev_t *mt_cb;                       /* clipboard: positions relative to the clip start */
#define MT_NEW ((void *(*)(unsigned))0x01AA370C)                     /* operator new[] (as devupd/du_fw.c) */
#endif
int mt_n;
mt_clip_t mt_clip[MT_CLIPS];
int mt_nclips;
static int mt_cb_n;
static int32_t mt_cb_len = -1;               /* clipboard clip length, -1 = empty */

volatile uint8_t mt_sel;                     /* the MIDI track is the selected track */
volatile uint8_t mt_mute;                    /* muted: plays nothing (still records); not saved (stock mutes aren't) */
static volatile uint8_t mt_rec_held, mt_rec_latch;
volatile uint8_t mt_recording;               /* (read by the drawing) */
static uint8_t mt_in_play;                   /* our own VoiceCtl call: not recorded */
static uint8_t mt_playing;
static int32_t mt_prev = -1;                 /* tape position (samples) at the last block */
static int32_t mt_rest = -1;                 /* where the tape last stood still */
/* recording position: mt_prev, which is -1 for one block when playing starts at the tape start (exclusive bound) */
static int32_t mt_here(void) { return mt_prev < 0 ? 0 : mt_prev; }
static uint32_t mt_sounding[4];              /* notes the track holds (midi bits) */
static uint32_t mt_rec_hold[4];              /* recorded notes still held (on without off) */
volatile int32_t mt_seg_lo, mt_seg_hi;       /* stretch recorded since the last wrap / jump (drawn) */
static volatile uint8_t mt_dirty, mt_loaded;
static int32_t mt_aw[MT_NT];                 /* playback: raw word as the track wrote it */
static uint16_t mt_aw_ok, mt_taken;          /* knobs written by the track / taken over until STOP */
static uint32_t mt_cur_ctx = 0xFFFFFFFF;
static int32_t mt_rl[MT_NT];                 /* recording: last raw value seen */
static uint16_t mt_rl_ok, mt_touched;        /* (baseline known / changed in this take) */
static uint32_t mt_blocks, mt_rt[MT_NT];     /* block counter, block of the last recorded value */
static int32_t mt_take_start, mt_take_lo, mt_take_hi, mt_tpos[MT_NT];   /* take; where each knob was first turned */
static uint8_t mt_take_wrapped;

/* ---- lists (interrupts off) ---- */
static int mt_lower(int32_t pos)             /* first event with ev.pos >= pos */
{
    int lo = 0, hi = mt_n;
    while (lo < hi) { int m = (lo + hi) >> 1; if (mt_ev[m].pos < pos) lo = m + 1; else hi = m; }
    return lo;
}
static int mt_upper(int32_t pos)             /* first event with ev.pos > pos */
{
    int lo = 0, hi = mt_n;
    while (lo < hi) { int m = (lo + hi) >> 1; if (mt_ev[m].pos <= pos) lo = m + 1; else hi = m; }
    return lo;
}
static int mt_insert(const mt_ev_t *e)       /* after the events at the same position (keeps on before off) */
{
    if (mt_n >= MT_MAX || e->pos < 0 || e->pos > MT_MAXPOS) return 0;
    uint32_t m = IRQ_OFF();
    int i = mt_upper(e->pos);
    for (int k = mt_n; k > i; k--) mt_ev[k] = mt_ev[k - 1];
    mt_ev[i] = *e; mt_n++;
    IRQ_ON(m);
    return 1;
}
static void mt_add_clip(int32_t a, int32_t b)                /* merged with the clips it touches or overlaps */
{
    if (a > b) { int32_t t = a; a = b; b = t; }
    if (a < 0) a = 0;
    if (b > MT_MAXPOS) b = MT_MAXPOS;
    if (b - a < MT_MINCLIP) return;
    uint32_t m = IRQ_OFF();
    int o = 0;
    for (int i = 0; i < mt_nclips; i++) {
        mt_clip_t c = mt_clip[i];
        if (c.stop < a || c.start > b) mt_clip[o++] = c;
        else { if (c.start < a) a = c.start; if (c.stop > b) b = c.stop; }
    }
    mt_nclips = o;
    if (mt_nclips < MT_CLIPS) {
        int i = mt_nclips;
        while (i > 0 && mt_clip[i - 1].start > a) { mt_clip[i] = mt_clip[i - 1]; i--; }
        mt_clip[i].start = a; mt_clip[i].stop = b; mt_nclips++;
    }
    IRQ_ON(m);
}
static int mt_clip_at(int32_t pos)
{
    for (int i = 0; i < mt_nclips; i++) if (mt_clip[i].start <= pos && pos <= mt_clip[i].stop) return i;
    return -1;
}

/* ---- playing ---- */
static int mt_bit(const uint32_t *b, int n) { return (b[n >> 5] >> (n & 31)) & 1; }
static void mt_note(int on, int midi, int vel, int param, int send)
{
    if (on && mt_mute) return;
    if (MODE() == 1) {          /* drum mode: 24 keys (midi 0x35..0x4C pick the sound): fold the note in by octaves */
        while (midi < 0x35) midi += 12;
        while (midi > 0x35 + 23) midi -= 12;
    }
    int32_t r[4] = { midi, on ? vel : 0, 0, 0 };
    ((uint8_t *)r)[8] = (uint8_t)(send != 0);
    mt_in_play = 1;
    if (on) {
        if (mt_bit(mt_sounding, midi)) VC_UP(r);             /* retrigger */
        VC_DOWN(r, param);
        mt_sounding[midi >> 5] |= 1u << (midi & 31);
    } else if (mt_bit(mt_sounding, midi)) {
        VC_UP(r);
        mt_sounding[midi >> 5] &= ~(1u << (midi & 31));
    }
    mt_in_play = 0;
}
void mt_all_off(void)
{
    for (int n = 0; n < 128; n++) if (mt_bit(mt_sounding, n)) mt_note(0, n, 0, 0, 1);
}
static int mt_vel_before(int i, int midi)    /* velocity of the note-on that a note-off at i ends (reverse play) */
{
    for (int k = i - 1; k >= 0; k--)
        if (mt_ev[k].midi == midi && !(mt_ev[k].flags & MT_AUTO)) return (mt_ev[k].flags & MT_ON) ? mt_ev[k].vel : 100;
    return 100;
}
static int32_t mt_val(const mt_ev_t *e) { return (int16_t)((uint16_t)(uint8_t)e->param << 8 | e->vel); }
static void mt_auto(int i, int reverse)      /* reverse: the knob gets the value it had before this event */
{
    int t = mt_ev[i].midi;
    if (mt_mute || t >= MT_NT || ((mt_taken >> t) & 1) || !mt_t_ok(t)) return;
    const mt_ev_t *e = &mt_ev[i];
    if (reverse) {
        int k = i - 1;
        while (k >= 0 && !((mt_ev[k].flags & MT_AUTO) && mt_ev[k].midi == t)) k--;
        if (k < 0) return;
        e = &mt_ev[k];
    }
    mt_t_write(t, mt_val(e));
    mt_aw[t] = mt_rl[t] = mt_t_raw(t);
    mt_aw_ok |= (uint16_t)(1u << t); mt_rl_ok |= (uint16_t)(1u << t);
}
/* events passed travelling from a (exclusive) to b (inclusive); reverse: on and off swap */
static void mt_cross(int32_t a, int32_t b)
{
    if (a < b) {
        for (int i = mt_upper(a); i < mt_n && mt_ev[i].pos <= b; i++) {
            const mt_ev_t *e = &mt_ev[i];
            if (e->flags & MT_AUTO) { mt_auto(i, 0); continue; }
            mt_note(e->flags & MT_ON, e->midi, e->vel, e->param, e->flags & MT_SEND);
        }
    } else if (a > b) {
        for (int i = mt_lower(a) - 1; i >= 0 && mt_ev[i].pos >= b; i--) {
            const mt_ev_t *e = &mt_ev[i];
            if (e->flags & MT_AUTO) { mt_auto(i, 1); continue; }
            int on = !(e->flags & MT_ON);
            mt_note(on, e->midi, on ? mt_vel_before(i, e->midi) : 0, e->param, e->flags & MT_SEND);
        }
    }
}

static void mt_seg_close(void)
{
    if (mt_seg_lo >= 0) {
        mt_add_clip(mt_seg_lo, mt_seg_hi);
        if (mt_seg_lo < mt_take_lo) mt_take_lo = mt_seg_lo;
        if (mt_seg_hi > mt_take_hi) mt_take_hi = mt_seg_hi;
    }
    mt_seg_lo = -1;
}
static void mt_auto_ins(int32_t pos, int t, int32_t v)
{
    mt_ev_t e = { pos, (uint8_t)t, (uint8_t)(v & 0xFF), MT_AUTO | MT_FRESH, (int8_t)(v >> 8) };
    mt_insert(&e);
}
/* recording, once per block: knob changes since the last recorded value (thinned) */
static void mt_rec_auto(int32_t pos)
{
    for (int t = 0; t < MT_NT; t++) {
        uint16_t bit = (uint16_t)(1u << t);
        if (!mt_t_ok(t)) continue;
        int32_t raw = mt_t_raw(t);
        if (!(mt_rl_ok & bit)) { mt_rl[t] = raw; mt_rl_ok |= bit; continue; }
        if (raw == mt_rl[t] || mt_blocks - mt_rt[t] < MT_THIN) continue;
        if (!(mt_touched & bit)) {
            mt_touched |= bit; mt_tpos[t] = pos;
            int had = 0;
            for (int k = 0; k < mt_n && !had; k++) had = (mt_ev[k].flags & MT_AUTO) && mt_ev[k].midi == t;
            if (!had) mt_auto_ins(mt_take_start, t, mt_t_norm(t, mt_rl[t]));
        }
        mt_auto_ins(pos, t, mt_t_norm(t, raw));
        mt_rl[t] = raw; mt_rt[t] = mt_blocks;
        mt_taken |= bit; mt_aw_ok &= (uint16_t)~bit;
    }
}
static void mt_rec_end(int32_t pos)          /* notes still held: released at the end of the recording */
{
    for (int n = 0; n < 128; n++) if (mt_bit(mt_rec_hold, n)) {
        mt_ev_t e = { pos, (uint8_t)n, 0, MT_SEND, 0 };
        mt_insert(&e);
    }
    mt_rec_hold[0] = mt_rec_hold[1] = mt_rec_hold[2] = mt_rec_hold[3] = 0;
    for (int t = 0; t < MT_NT; t++)                               /* last values of the knobs turned in this take */
        if (((mt_touched >> t) & 1) && ((mt_rl_ok >> t) & 1) && mt_t_ok(t) && mt_t_raw(t) != mt_rl[t])
            mt_auto_ins(pos, t, mt_t_norm(t, mt_t_raw(t)));
    mt_seg_close();
    uint32_t m = IRQ_OFF();                                       /* their older automation in the take: replaced */
    int o = 0;
    for (int k = 0; k < mt_n; k++) {
        mt_ev_t e = mt_ev[k];
        if ((e.flags & (MT_AUTO | MT_FRESH)) == MT_AUTO && e.midi < MT_NT && ((mt_touched >> e.midi) & 1) &&
            e.pos >= (mt_take_wrapped ? mt_take_lo : mt_tpos[e.midi]) && e.pos <= mt_take_hi) continue;
        e.flags &= (uint8_t)~MT_FRESH;
        mt_ev[o++] = e;
    }
    mt_n = o;
    IRQ_ON(m);
    mt_touched = 0;
    mt_recording = 0;
    mt_dirty = 1;
}

/* audio callback, once per block (tape/countin.c) */
void mt_block(void)
{
    if (!mt_loaded) return;                     /* (the note list is allocated at load) */
    int32_t pos = TAPE_POS >> 6;
    int st = TAPE_STATE;            /* 9 starting, 10 playing, 4 recording (from stop), 5 recording (punch) */
    int playing = st == 10 || st == 9 || st == 4 || st == 5;
    if (!playing) {
        if (mt_playing) {
            mt_all_off();
            if (mt_recording) mt_rec_end(mt_here());
            mt_rec_latch = 0;
        }
        if (pos == mt_prev) mt_rest = pos;                        /* standing still (not spinning up) */
        mt_playing = 0; mt_prev = pos;
        mt_taken = 0; mt_aw_ok = 0;
        return;
    }
    int start = !mt_playing;
    if (start) {               /* the tape moves ~0.1 s before its state says playing: play from where it stood */
        mt_playing = 1;
        int32_t r = mt_rest, fwd = TAPE_SPEED >= 0;
        int near = r >= 0 && (fwd ? pos >= r && pos - r <= MT_SPINUP : pos <= r && r - pos <= MT_SPINUP);
        mt_prev = fwd ? (near ? r : pos) - 1 : (near ? r : pos) + 1;   /* (exclusive: a note at the start plays) */
    }
    int32_t prev = mt_prev, d = pos - prev;
    mt_blocks++;
    uint32_t ctx = mt_ctx();
    if ((ctx ^ mt_cur_ctx) & 0x1F) mt_all_off();              /* other mode / slot: its notes end here */
    if (ctx != mt_cur_ctx) { mt_cur_ctx = ctx; mt_aw_ok = 0; mt_taken = 0; mt_rl_ok = 0; }   /* other instrument */
    for (int t = 0; t < MT_NT; t++)                               /* changed by someone else: taken over */
        if (((mt_aw_ok >> t) & 1) && mt_t_raw(t) != mt_aw[t]) {
            mt_taken |= (uint16_t)(1u << t); mt_aw_ok &= (uint16_t)~(1u << t);
        }
    int want = mt_loaded && mt_sel && (mt_rec_held || mt_rec_latch);
    if (mt_recording && !want) mt_rec_end(mt_here());
    if (start || (d <= MT_JUMP && d >= -MT_JUMP)) {
        mt_cross(prev, pos);
    } else {                                                  /* a jump: the loop wrap, else a locate */
        mt_all_off();
        if (LOOP_ON && LOOP_OUT > LOOP_IN) {
            int fwd = d < 0;                                  /* forward travel wraps back down to the loop in */
            int32_t a_end = fwd ? LOOP_OUT : LOOP_IN, b_start = fwd ? LOOP_IN : LOOP_OUT;
            mt_cross(prev, a_end);
            if (mt_recording) {
                if (fwd) mt_seg_hi = a_end; else mt_seg_lo = a_end;
                mt_seg_close();
            }
            mt_all_off();
            if (mt_recording) mt_take_wrapped = 1;
            mt_cross(fwd ? b_start - 1 : b_start + 1, pos);
            if (mt_recording) mt_seg_lo = mt_seg_hi = b_start;
        } else if (mt_recording) {
            mt_seg_close();
            mt_seg_lo = mt_seg_hi = pos;
        }
    }
    if (want && !mt_recording) {
        mt_recording = 1; mt_seg_lo = mt_seg_hi = mt_here();
        mt_take_start = mt_take_lo = mt_take_hi = start ? mt_here() + (d > 0 && prev >= 0 ? 1 : 0) : pos;   /* (where a playback from here
                                                                                    * starts: plays its values) */
        mt_touched = 0; mt_rl_ok = 0; mt_take_wrapped = 0;
    }
    if (mt_recording) {
        if (pos < mt_seg_lo) mt_seg_lo = pos;
        if (pos > mt_seg_hi) mt_seg_hi = pos;
        mt_rec_auto(mt_here());
    }
    if (!mt_recording && (mt_sounding[0] | mt_sounding[1] | mt_sounding[2] | mt_sounding[3]) && mt_clip_at(pos) < 0)
        mt_all_off();                           /* outside the clips nothing sounds (a note cut by lift / scissors) */
    mt_prev = pos;
}

/* ---- recording: VoiceCtl key_down / key_up entry hooks (hook.S); rec = {midi, vel, send MIDI byte, 0} ---- */
void mt_vc_down(const int32_t *rec, int param)
{
    if (!mt_recording || mt_in_play) return;
    int midi = rec[0], vel = rec[1];
    if (midi < 0 || midi > 127) return;
    if (vel < 0 || vel > 127) vel = 127;
    mt_ev_t e = { mt_here(), (uint8_t)midi, (uint8_t)vel, (uint8_t)(MT_ON | (((const uint8_t *)rec)[8] ? MT_SEND : 0)),
                  (int8_t)(param < -128 ? -128 : param > 127 ? 127 : param) };
    if (mt_insert(&e)) mt_rec_hold[midi >> 5] |= 1u << (midi & 31);
}
void mt_vc_up(const int32_t *rec)
{
    if (!mt_recording || mt_in_play) return;
    int midi = rec[0];
    if (midi < 0 || midi > 127 || !mt_bit(mt_rec_hold, midi)) return;   /* only notes this recording started */
    mt_ev_t e = { mt_here(), (uint8_t)midi, 0, (uint8_t)(((const uint8_t *)rec)[8] ? MT_SEND : 0), 0 };
    mt_insert(&e);
    mt_rec_hold[midi >> 5] &= ~(1u << (midi & 31));
}

/* ---- editing (key thread) ---- */
static int32_t mt_cursor(void) { return TAPE_POS >> 6; }
static void mt_scissors(void)
{
    int32_t c = mt_cursor();
    int i = mt_clip_at(c);
    if (i < 0 || c - mt_clip[i].start < MT_MINCLIP || mt_clip[i].stop - c < MT_MINCLIP || mt_nclips >= MT_CLIPS) return;
    uint32_t m = IRQ_OFF();
    for (int k = mt_nclips; k > i + 1; k--) mt_clip[k] = mt_clip[k - 1];
    mt_clip[i + 1].start = c + 1; mt_clip[i + 1].stop = mt_clip[i].stop;
    mt_clip[i].stop = c;
    mt_nclips++;
    IRQ_ON(m);
    mt_dirty = 1;
}
static void mt_lift(void)
{
    int i = mt_clip_at(mt_cursor());
    if (i < 0) return;
    mt_clip_t c = mt_clip[i];
    uint32_t m = IRQ_OFF();
    int a = mt_lower(c.start), b = mt_upper(c.stop), o = a;
    mt_cb_n = 0;
    for (int k = a; k < b; k++) {
        mt_ev_t e = mt_ev[k];
        if (mt_cb_n < MT_CLIPMAX) { e.pos -= c.start; mt_cb[mt_cb_n++] = e; }
    }
    for (int k = b; k < mt_n; k++) mt_ev[o++] = mt_ev[k];
    mt_n = o;
    for (int k = i; k < mt_nclips - 1; k++) mt_clip[k] = mt_clip[k + 1];
    mt_nclips--;
    mt_cb_len = c.stop - c.start;
    IRQ_ON(m);
    mt_all_off();
    mt_dirty = 1;
}
static void mt_drop(void)
{
    if (mt_cb_len < 0) return;
    int32_t c = mt_cursor();
    if (c + mt_cb_len > MT_MAXPOS) return;
    for (int k = 0; k < mt_cb_n; k++) { mt_ev_t e = mt_cb[k]; e.pos += c; if (!mt_insert(&e)) break; }
    mt_add_clip(c, c + mt_cb_len);
    mt_dirty = 1;
}

/* key thread: tape screen keys while the MIDI track is selected (dispatcher.c). Returns 1 if the key is consumed
 * (its release too). on_tape: the tape screen is the current one. */
static uint8_t mt_swallow[0x40], mt_tape_held;
/* On the tape screen the stock audio callback mixes the instrument in with the selected audio track's mute gain
 * (L1 0xFFA00E50 reads 0xFF802964 and that track's gain 0xFF8018B4[t], which follows its mute byte). With the MIDI
 * track selected that would silence the keyboard and the MIDI track whenever the audio track behind it is muted, so
 * the stock selection is moved to an unmuted track (main loop, and on select). All four muted: left as it is. */
static void mt_monitor(void)
{
    if (!mt_sel) return;
    int s = SEL_TRACK;
    if (s >= 0 && s < 4 && !TRACK_MUTED[s]) return;
    for (int t = 0; t < 4; t++) if (!TRACK_MUTED[t]) { SEL_TRACK = t; return; }
}
int mt_key(int code, int pressed, int shift, int on_tape)
{
    if (code < 0 || code >= 0x40) return 0;
    if (code == 0x04) mt_tape_held = (uint8_t)pressed;               /* (the TAPE key itself stays stock) */
    if (!mt_loaded) return 0;
    if (!pressed && mt_swallow[code]) { mt_swallow[code] = 0; if (code == 0x21) mt_rec_held = 0; return 1; }
    if (code == 0x23 && pressed) mt_rec_latch = 0;                  /* STOP: stock too */
    if (pressed && code == 0x06 && mt_tape_held) {   /* TAPE + T1: select; again while selected: mute / unmute (no
                                                     * Shift: stock Shift + TAPE opens ERASE TAPE) */
        if (mt_sel) { mt_mute ^= 1; if (mt_mute) mt_all_off(); mt_dirty = 1; }
        else { mt_sel = 1; mt_monitor(); }
        mt_swallow[code] = 1; return 1;
    }
    if (!on_tape || !pressed) return 0;
    if (!mt_sel) return 0;
    if (code >= 0x06 && code <= 0x09) {               /* T1..T4: stock track; Shift + T-key (stock mute): M stays */
        if (!shift) { mt_sel = 0; mt_rec_held = mt_rec_latch = 0; }
        return 0;
    }
    if (code == 0x21) {                                              /* REC: never the stock audio recording */
        if (shift) mt_rec_latch ^= 1; else mt_rec_held = 1;
        mt_swallow[code] = 1; return 1;
    }
    if (code >= 0x0A && code <= 0x0C) {                              /* lift / drop / scissors */
        if (code == 0x0A) mt_lift(); else if (code == 0x0B) mt_drop(); else mt_scissors();
        mt_swallow[code] = 1; return 1;
    }
    return 0;
}
int mt_armed(void) { return mt_sel && (mt_rec_held || mt_rec_latch); }

/* ---- persistence: tape.db op1rmidi; row 0 "start,stop,..." (decimal), rows 1.. 14 hex digits per note:
 * pos (6) midi (2) vel (2) flags (2) param (2) ---- */
#define MT_ROW      256                      /* notes per row */
static const char mt_hexd[] = "0123456789abcdef";
static char *mt_hex(char *o, uint32_t v, int digits)
{
    for (int i = digits - 1; i >= 0; i--) *o++ = mt_hexd[(v >> (4 * i)) & 15];
    return o;
}
static int mt_unhex(const char *s, int digits)
{
    int v = 0;
    for (int i = 0; i < digits; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return -1;
        v = v << 4 | d;
    }
    return v;
}
static char *mt_dec(char *o, uint32_t v)
{
    char d[10]; int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) *o++ = d[--k];
    return o;
}
int mt_fmt_clips(char *o)
{
    char *s = o;
    for (int i = 0; i < mt_nclips; i++) {
        if (i) *o++ = ',';
        o = mt_dec(o, (uint32_t)mt_clip[i].start); *o++ = ','; o = mt_dec(o, (uint32_t)mt_clip[i].stop);
    }
    *o = 0;
    return (int)(o - s);
}
int mt_fmt_notes(char *o, int from)          /* up to MT_ROW notes from index `from` */
{
    char *s = o;
    for (int i = from; i < mt_n && i < from + MT_ROW; i++) {
        const mt_ev_t *e = &mt_ev[i];
        o = mt_hex(o, (uint32_t)e->pos, 6); o = mt_hex(o, e->midi, 2); o = mt_hex(o, e->vel, 2);
        o = mt_hex(o, e->flags & (MT_ON | MT_SEND | MT_AUTO), 2); o = mt_hex(o, (uint8_t)e->param, 2);
    }
    *o = 0;
    return (int)(o - s);
}
void mt_parse_clips(const char *p, int len)
{
    int32_t v = 0, a = -1; int have = 0;
    for (int i = 0; i <= len; i++) {
        char c = i < len ? p[i] : ',';
        if (c >= '0' && c <= '9') { if (v <= MT_MAXPOS) v = v * 10 + (c - '0'); have = 1; }
        else if (c == ',') {
            if (have) { if (a < 0) a = v; else { if (v <= MT_MAXPOS) mt_add_clip(a, v); a = -1; } }
            v = 0; have = 0;
        }
    }
}
void mt_parse_notes(const char *p, int len)  /* appended in order (rows are saved sorted) */
{
    for (int i = 0; i + 14 <= len && mt_n < MT_MAX; i += 14) {
        int pos = mt_unhex(p + i, 6), midi = mt_unhex(p + i + 6, 2), vel = mt_unhex(p + i + 8, 2),
            fl = mt_unhex(p + i + 10, 2), par = mt_unhex(p + i + 12, 2);
        if (pos < 0 || midi < 0 || midi > 127 || vel < 0 || fl < 0 || par < 0 || pos > MT_MAXPOS) continue;
        if (!(fl & MT_AUTO) && vel > 127) continue;
        mt_ev_t e = { pos, (uint8_t)midi, (uint8_t)vel, (uint8_t)(fl & (MT_ON | MT_SEND | MT_AUTO)), (int8_t)(uint8_t)par };
        if (mt_n && mt_ev[mt_n - 1].pos > pos) { mt_insert(&e); continue; }
        mt_ev[mt_n++] = e;
    }
}
void mt_clear(void)
{
    uint32_t m = IRQ_OFF();
    mt_n = 0; mt_nclips = 0;
    IRQ_ON(m);
}

#ifdef OP1_HOST
int mt_host_saves;
static void mt_save(void) { mt_host_saves++; }
static int mt_load(void) { return 1; }
#else
#define MT_DB_HANDLE ((void *(*)(void))0x0196DD50)          /* tape.db */
#define MT_DB_EXEC   ((void (*)(void *, const char *))0x0196F950)
#define MT_DB_GET    ((void (*)(void *, void *, const char *))0x0196FBD0)
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } mt_dstr;
#define MT_SQL_LEN (64 + MT_ROW * 14 + MT_CLIPS * 18)
static char *mt_sql;
static const char MT_CREATE[] = "CREATE TABLE IF NOT EXISTS op1rmidi (id INTEGER PRIMARY KEY, data TEXT NOT NULL)";
static char *mt_str(char *o, const char *s) { while (*s) *o++ = *s++; return o; }
/* firmware saver thread (as seq/dots.c): the stock DB writes go through it as well, so ours never run inside a
 * stock transaction (tape.db Clips saves on STOP use BEGIN / COMMIT). No transaction of our own: every row is one
 * INSERT OR REPLACE (atomic), then the rows past the end are deleted; a cut mid-way leaves old and new rows mixed,
 * never an empty track. A failed write (op1_db_last_err, shown on the monitor page) marks the track for another save. */
#define MT_BG_REGISTER ((void (*)(void *))0x0193F010)
#define MT_BG_DIRTY    ((void (*)(void *))0x0193EF3C)
static void mt_bg_nop(void *self) { (void)self; }
static void mt_bg_save(void *self)
{
    (void)self;
    void *db = MT_DB_HANDLE();
    MT_DB_EXEC(db, MT_CREATE);
    int bad = 0, row = 1;
    char *o = mt_str(mt_sql, "INSERT OR REPLACE INTO op1rmidi VALUES (0,'");
    uint32_t mc = IRQ_OFF();                                 /* (the list may change: a take ending) */
    o += mt_fmt_clips(o);
    IRQ_ON(mc);
    mt_str(o, "')")[0] = 0;                                 /* (terminate: older text / heap garbage follows) */
    bad |= op1_db_write(db, mt_sql);
    for (int i = 0; i < mt_n; i += MT_ROW, row++) {
        o = mt_str(mt_sql, "INSERT OR REPLACE INTO op1rmidi VALUES (");
        o = mt_dec(o, (uint32_t)row);
        o = mt_str(o, ",'");
        uint32_t m = IRQ_OFF();                              /* a recording may insert meanwhile */
        o += mt_fmt_notes(o, i);
        IRQ_ON(m);
        mt_str(o, "')")[0] = 0;
        bad |= op1_db_write(db, mt_sql);
    }
    o = mt_str(mt_sql, "DELETE FROM op1rmidi WHERE id>=");
    mt_dec(o, (uint32_t)row)[0] = 0;
    bad |= op1_db_write(db, mt_sql);
    if (bad) mt_dirty = 1;                                   /* try again (mt_service) */
}
static const uint32_t mt_bg_vt[6] = { 0, 0, 0, (uint32_t)mt_bg_nop, 0, (uint32_t)mt_bg_save };
static struct { uint8_t dirty, saving, pad[2]; const uint32_t *vt; uint32_t more[2]; } mt_bg;
static void mt_save(void) { MT_BG_DIRTY(&mt_bg); }
static int mt_load(void)                     /* main loop, once; 0: no memory (the track stays off) */
{
    mt_ev = (mt_ev_t *)MT_NEW(MT_MAX * sizeof(mt_ev_t));
    mt_cb = (mt_ev_t *)MT_NEW(MT_CLIPMAX * sizeof(mt_ev_t));
    mt_sql = (char *)MT_NEW(MT_SQL_LEN);
    if (!mt_ev || !mt_cb || !mt_sql) return 0;
    void *db = MT_DB_HANDLE();
    MT_DB_EXEC(db, MT_CREATE);
    for (int row = 0; row < 1 + MT_MAX / MT_ROW; row++) {
        char *o = mt_str(mt_sql, "SELECT data FROM op1rmidi WHERE id=");
        mt_dec(o, (uint32_t)row)[0] = 0;
        mt_dstr s;                                           /* (no "= { 0 }": -Os makes that a memset call) */
        for (unsigned k = 0; k < sizeof s / 4; k++) ((volatile uint32_t *)&s)[k] = 0;
        MT_DB_GET(&s, db, mt_sql);
        if (!s.size) { if (row) break; continue; }
        const char *p = s.cap > 15 ? s.u.ptr : s.u.buf;       /* (heap copy once per boot, not freed) */
        if (row == 0) mt_parse_clips(p, (int)s.size);       /* (an "m," mute prefix of build 7..11 is skipped: mutes
                                                               * start off at power-up, as the stock track mutes) */
        else mt_parse_notes(p, (int)s.size);
    }
    MT_BG_REGISTER(&mt_bg);
    mt_bg.vt = mt_bg_vt;
    return 1;
}
#endif

/* notes outside every clip (not made by the editing; an older build could lose a clip): clips around them again */
void mt_heal(void)
{
    int32_t a = -1, b = -1;
    for (int i = 0; i < mt_n; i++) {
        int32_t p = mt_ev[i].pos;
        if (mt_clip_at(p) >= 0) continue;
        if (a >= 0 && p - b <= 2 * 44100) { b = p; continue; }  /* gaps up to 2 s: one clip */
        if (a >= 0) mt_add_clip(a, b + MT_MINCLIP);
        a = b = p;
    }
    if (a >= 0) mt_add_clip(a, b + MT_MINCLIP);
}

/* main loop (tape/countin.c ci_service): load once, save after changes (not while recording) */
void mt_service(void)
{
    static uint8_t dead;
    if (dead) return;
    if (!mt_loaded) {
        mt_seg_lo = -1;
        if (!mt_load()) { dead = 1; return; }
        int nc = mt_nclips;
        mt_heal();
        if (mt_nclips != nc) mt_dirty = 1;
        mt_loaded = 1; return;
    }
    if (mt_dirty && !mt_recording) { mt_dirty = 0; mt_save(); }
    mt_monitor();
}

/* ---- drawing, after the stock tape screen and the markers (tape/markers.c tm_screen_draw) ---- */
extern int32_t tm_zoom_q10(void);
#define MT_Y        OP1_Q10(144.5)           /* the 5th strip, on the 4.04 px track pitch (tracks at 128.4 .. 140.5) */
#define MT_CENTRE_X 163973                   /* playhead x 160.13 (as the markers) */
static const uint8_t mt_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF }, mt_blue[4] = { 0x69, 0x8E, 0xFF, 0xFF },
                     mt_offgrey[4] = { 0x35, 0x32, 0x38, 0xFF }, mt_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF },
                     mt_grey[4] = { 0x65, 0x65, 0x79, 0xFF }, mt_black[4] = { 0x00, 0x00, 0x00, 0xFF };   /* (colors.svg) */
#ifndef OP1_HOST
static void mt_line(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
#else
void host_mt_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint8_t *c);
#define mt_line(r, x0, y0, x1, y1, w, c) ((void)(r), host_mt_line((x0), (y0), (x1), (y1), (c)))
#endif
static int32_t mt_x(int32_t pos, int32_t c, int32_t k)
{
    int64_t x = MT_CENTRE_X + (((int64_t)(pos - c) * k) >> 16);
    return x < 0 ? 0 : x > OP1_Q10(319) ? OP1_Q10(319) : (int32_t)x;
}
/* tape screen draw, first sub-drawer (CALL 0x01A5C728 at 0x01A5CFA8 -> here): it sets the screen's recording flag
 * scr+0x93 from the tape state each frame (states 2..8: red theme, rec dot, the clip being recorded from scr+0x98 /
 * scr+0x9C). While the MIDI track is armed or recording, the flag is forced for this frame (no recorded audio clip:
 * range 0); the tape state itself is never touched (the audio interrupt reads it). */
void mt_tape_pre(void *scr)
{
#ifndef OP1_HOST
    ((void (*)(void *))0x01A5C728)(scr);
#endif
    if (scr && mt_loaded && (mt_recording || mt_armed())) {
        ((volatile uint8_t *)scr)[0x93] = 1;
        *(volatile int32_t *)((uint8_t *)scr + 0x98) = 0;
        *(volatile int32_t *)((uint8_t *)scr + 0x9C) = 0;
    }
}
/* the track box (top left, tape.svg "trackno": rect 6,3 28 x 28) shows M while the MIDI track is selected: the stock
 * digit is covered with black, M and the box drawn in white, grey while muted (as a muted audio track) */
static void mt_draw_box(void *r)
{
    const uint8_t *c = mt_mute ? mt_grey : mt_white;
    mt_line(r, OP1_Q10(9.5), OP1_Q10(16.7), OP1_Q10(30.5), OP1_Q10(16.7), OP1_Q10(19), mt_black);   /* digit area */
    mt_line(r, OP1_Q10(6), OP1_Q10(3), OP1_Q10(34.08), OP1_Q10(3), 0x600, c);                        /* box */
    mt_line(r, OP1_Q10(34.08), OP1_Q10(3), OP1_Q10(34.08), OP1_Q10(31.08), 0x600, c);
    mt_line(r, OP1_Q10(34.08), OP1_Q10(31.08), OP1_Q10(6), OP1_Q10(31.08), 0x600, c);
    mt_line(r, OP1_Q10(6), OP1_Q10(31.08), OP1_Q10(6), OP1_Q10(3), 0x600, c);
    mt_line(r, OP1_Q10(13.2), OP1_Q10(24.0), OP1_Q10(13.2), OP1_Q10(9.4), 0x600, c);                  /* M */
    mt_line(r, OP1_Q10(13.2), OP1_Q10(9.4), OP1_Q10(20.3), OP1_Q10(18.2), 0x600, c);
    mt_line(r, OP1_Q10(20.3), OP1_Q10(18.2), OP1_Q10(27.4), OP1_Q10(9.4), 0x600, c);
    mt_line(r, OP1_Q10(27.4), OP1_Q10(9.4), OP1_Q10(27.4), OP1_Q10(24.0), 0x600, c);
}
void mt_draw(void)
{
    if (!mt_loaded) return;
    void *r = op1_raster();
    int32_t c = mt_cursor(), z = tm_zoom_q10();
    int32_t k = (int32_t)(((int64_t)z << 16) / 44100);       /* Q10 px per sample, << 16 */
    for (int i = 0; i < mt_nclips; i++) {   /* stock (0x01A5D420): other tracks offgrey; selected red, blue under the playhead */
        int32_t x0 = mt_x(mt_clip[i].start, c, k), x1 = mt_x(mt_clip[i].stop, c, k);
        const uint8_t *col = !mt_sel ? mt_offgrey : (mt_clip[i].start <= c && c <= mt_clip[i].stop) ? mt_blue : mt_red;
        if (x1 - x0 >= OP1_Q10(1)) mt_line(r, x0, MT_Y, x1, MT_Y, 0x800, col);
    }
    if (mt_recording && mt_seg_lo >= 0) {                     /* the stretch being recorded: blue (stock) */
        int32_t x0 = mt_x(mt_seg_lo, c, k), x1 = mt_x(mt_seg_hi, c, k);
        if (x1 - x0 < OP1_Q10(1)) x1 = x0 + OP1_Q10(1);
        mt_line(r, x0, MT_Y, x1, MT_Y, 0x800, mt_blue);
    }
    if (mt_sel) {                                             /* note starts as ticks under the selected strip */
        int32_t lo = c - (int32_t)(((int64_t)OP1_Q10(161) << 16) / (k ? k : 1));
        int shown = 0;
        uint32_t m = IRQ_OFF();
        int i = mt_lower(lo < 0 ? 0 : lo);
        IRQ_ON(m);
        for (; i < mt_n && shown < 160; i++) {
            mt_ev_t e = mt_ev[i];
            if (!(e.flags & MT_ON)) continue;
            int32_t x = mt_x(e.pos, c, k);
            if (x >= OP1_Q10(319)) break;
            if (x <= 0) continue;
            mt_line(r, x, MT_Y + OP1_Q10(2), x, MT_Y + OP1_Q10(4), 0x400, mt_white);
            shown++;
        }
        mt_draw_box(r);
    }
}
