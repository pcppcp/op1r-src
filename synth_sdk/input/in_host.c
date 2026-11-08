/* Input engines (after Hundekraft kennel 3.2.2 kennel/kn_input.c): a synth plugin with plugin.json "input_engine" plays
 * the selected input (mic / line / radio / USB) through its own DSP instead of voices, e.g. Amp.
 *   Audio: R246's synth-bus high-pass entry 0x019674F8 (synth_bus_dc_block) is the hook (hook.S hook_inputbus, 6-byte
 *   prologue): its three callers inside core_block_process pass the block's has-input flag at SP+0x34 (synth 0xFFA02896,
 *   drum 0xFFA02464, game 0xFFA01EC2). For the synth caller with an input engine on the current synth slot (synth mode,
 *   type 14 = our engines), the engine's process() writes 64 samples into the bus 0xFF800BB4 from the input
 *   0xFF800CB4 (silence while resampling the master), and the flag is cleared (no dry monitor of the input). Everything
 *   else runs the stock high-pass.
 *   Rate: 22050 / 44100 from the audio callback cadence (sm_audio_cb: in_clock / in_end).
 *   T2: the engine's optional tuner screen replaces the synth T2 page (slot 0x16) while the engine is current;
 *   on it blue turns INPUT GAIN (-12..+24 dB per input source, op1rsettings ingain0..3), other knobs / presses do
 *   nothing. service() (pitch analysis) runs from the tuner screen's frame (GUI thread).
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "../op1_synth.h"
#include "../op1_input.h"
#include "../screens/op1r_settings.h"
#include "in_gain.h"

const op1_input_engine_t *op1_input_engines[OP1_SLOTS];
static const op1_input_engine_t *active;
static int active_slot = -1;
static uint32_t active_rate, active_source, active_mode;
static uint32_t rate = 44100, previous_clock, clock_sum, clock_count;
static int block_touched;
static volatile int8_t gain_db[4];
static volatile uint8_t gain_dirty, gain_loaded;
static int gain_acc;
#define PLAY_MODE    (*(volatile uint32_t *)0xFF801038)
#define SLOT         (*(volatile uint32_t *)0xFF8016D4 & 7)
#define TYPE         (*(volatile uint32_t *)(0xFF8016E4 + 4 * SLOT))
#define INPUT_MODE   (*(volatile uint32_t *)0xFF8010B0)
#define INPUT_SOURCE (*(volatile uint32_t *)0xFF8010B4)
#define SCREEN       (*(volatile int *)0x018B65C8)
#define TABLE        ((void **)0x01DF4E54)
#define T2_ID        0x16

static int current(void)
{
    if (PLAY_MODE != 0 || TYPE != 14) return 0;
    int n = op1_synth_knobs(SLOT)[4] - OP1_ENGINE_MAGIC;
    return n > 0 && n < OP1_SLOTS && op1_input_engines[n] ? n : 0;
}
void in_clock(uint32_t now)                                  /* audio callback start (sysmon) */
{
    uint32_t d = now - previous_clock; previous_clock = now; block_touched = 0;
    if (d < 400000u || d > 1600000u) { clock_sum = clock_count = 0; return; }   /* start-up pauses */
    clock_sum += d;
    if (++clock_count == 16) { rate = clock_sum > 13931968u ? 22050 : 44100; clock_sum = clock_count = 0; }
}
extern void in_highpass_stock(void);                         /* hook.S: the stock prologue, then 0x019674FE */
void in_bus(uint32_t *monitor, uint32_t caller)              /* hook.S in_highpass_entry (audio thread) */
{
    int n = caller == 0xFFA02896u ? current() : 0;
    if (!n) { in_highpass_stock(); return; }
    const op1_input_engine_t *d = op1_input_engines[n];
    int slot = (int)SLOT;
    if (d != active || slot != active_slot || active_rate != rate || active_source != INPUT_SOURCE || active_mode != INPUT_MODE) {
        if (active) active->reset(-1, rate);
        active = d; active_slot = slot; active_rate = rate; active_source = INPUT_SOURCE; active_mode = INPUT_MODE;
        d->reset(slot, rate);
    }
    block_touched = 1;
    int16_t words[8];
    for (int k = 0; k < 8; k++) words[k] = op1_synth_knobs(slot)[k];
    static const int16_t silence[64];
    const int16_t *input = *monitor && INPUT_MODE == 0 ? (const int16_t *)0xFF800CB4 : silence;   /* resampling: silence */
    static int16_t gained[64];
    const int db = gain_db[INPUT_SOURCE & 3];
    if (db && input != silence) { in_gain_apply(input, gained, 64, db); input = gained; }
    uint32_t t = op1_cycles();
    d->process(input, (int16_t *)0xFF800BB4, words, rate);
    sm_cy[SMC_SYNTH] += op1_cycles() - t;
    *monitor = 0;                                            /* no dry monitor of the input */
}
void in_end(void)                                            /* audio callback end: deactivate when not run */
{
    if (!block_touched && active) { active->reset(-1, rate); active = 0; active_slot = -1; active_rate = 0; }
}

/* ---- INPUT GAIN, the T2 tuner page (GUI / KeyHandler threads) ---- */
static const char *const gain_key[4] = { "ingain0", "ingain1", "ingain2", "ingain3" };   /* mic, line, radio, USB */
#ifndef OP1_HOST
int op1_input_gain_db(void) { return gain_db[INPUT_SOURCE & 3]; }
#endif
void in_flush(void)                                          /* key events / tuner frames: load once, save turns */
{
    if (!gain_loaded) { for (int i = 0; i < 4; i++) gain_db[i] = (int8_t)in_gain_clamp(op1r_setting_get(gain_key[i], 0)); gain_loaded = 1; }
    if (gain_dirty) { uint8_t d = gain_dirty; gain_dirty = 0; for (int i = 0; i < 4; i++) if (d >> i & 1) op1r_setting_put(gain_key[i], gain_db[i]); }
}
#define IN_MAX 4                                             /* input engines with a tuner page */
static uint32_t in_scr[IN_MAX][32], in_vt[IN_MAX][10];
static int in_n[IN_MAX], in_count;
static void *prior, *shown;
static void tuner_update(void *self, int dt)                 /* GUI thread: the engine's page, then its service */
{
    for (int i = 0; i < in_count; i++)
        if (self == (void *)in_scr[i]) {
            const op1_input_engine_t *d = op1_input_engines[in_n[i]];
            d->tuner->update(self, dt);
            if (d->service) d->service();
        }
}
void in_ctor(void)                                           /* screen ctor (after the plugins loaded) */
{
    for (int n = 1; n < OP1_SLOTS && in_count < IN_MAX; n++) {
        if (!op1_input_engines[n] || !op1_input_engines[n]->tuner) continue;
        uint32_t *s = in_scr[in_count], *v = in_vt[in_count];
        void *save = TABLE[T2_ID];
        ((void *(*)(void *, int, const char *))0x01A3BAB0)(s, T2_ID, "usbout.svg");   /* empty svg: drawn in code */
        TABLE[T2_ID] = save;
        for (int i = 0; i < 10; i++) v[i] = ((const uint32_t *)0x019103FC)[i];
        v[7] = (uint32_t)tuner_update; v[9] = (uint32_t)op1_input_engines[n]->tuner->init;
        s[3] = (uint32_t)v;
        in_n[in_count++] = n;
    }
}
void in_load(void)                                           /* screens load */
{
    for (int i = 0; i < in_count; i++) {
        uint32_t *s = in_scr[i];
        s[0] = (uint32_t)((void *(*)(void *))0x01A503D8)((void *)s[2]);
        if (s[0]) op1_input_engines[in_n[i]]->tuner->init(s); else in_n[i] = 0;
    }
}
static void *tuner_of(int n) { for (int i = 0; i < in_count; i++) if (n && in_n[i] == n && in_scr[i][0]) return in_scr[i]; return 0; }
void in_screen_sync(void)                                    /* key / knob events: T2 = the tuner while current */
{
    void *target = tuner_of(current());
    if (shown && TABLE[T2_ID] != shown) shown = prior = 0;   /* another page owner (sampler+) restored stock */
    if (!target) { if (shown && TABLE[T2_ID] == shown) TABLE[T2_ID] = prior; shown = prior = 0; return; }
    if (TABLE[T2_ID] != target) { if (!shown) prior = TABLE[T2_ID]; TABLE[T2_ID] = target; shown = target; }
}
static int tuner_visible(void) { void *t = tuner_of(current()); return t && SCREEN == T2_ID && TABLE[T2_ID] == t; }
int op1_input_tuner_active(void) { return tuner_visible(); }
int in_knob(int knob, int delta)                             /* 1 = taken (the tuner page) */
{
    if (!tuner_visible()) return 0;
    if (knob == 0 && gain_loaded) {                          /* blue: 1 dB a detent */
        gain_acc += delta;
        int st = gain_acc / 1024;
        if (st) {
            gain_acc -= st * 1024;
            uint32_t so = INPUT_SOURCE & 3;
            int g = in_gain_clamp(gain_db[so] + st);
            if (g != gain_db[so]) { gain_db[so] = (int8_t)g; gain_dirty |= (uint8_t)(1u << so); }
        }
    }
    return 1;
}
int in_press(int code) { return tuner_visible() && code >= 0x3B && code <= 0x3E; }
