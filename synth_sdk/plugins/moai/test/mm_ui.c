/* Host test of the views and knobs (mm_ui.h) against an emulation of what the firmware does:
 *   a turn: on the stock path the knob word moves (clamped 0..32767); while the knob's page flag is set the core
 *           moves page word 5..7 by delta / 4 (clamped 1..32767), as dispatcher.c's knob_event_hook
 *   a press: toggles the page flag (op1_eng_page), as dispatcher.c's key hook
 *   a frame: the screen update (mm_ui_frame + drawing)
 * Scenarios with checks; prints PASS / FAIL lines and exits 1 on a failure. */
#include "../../../op1_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24] __attribute__((aligned(4)));
int16_t host_voice_vel[6]; int16_t host_accel[3]; int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];
int host_shift; int host_set_knobs_calls;
int16_t host_saved[8][8];                                     /* what the patch setter put into each slot's sound */
void host_set_knobs(int slot, const int16_t *w) { memcpy(host_saved[slot & 7], w, sizeof host_saved[0]); host_set_knobs_calls++; }
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
void *host_raster(void) { return 0; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c) { (void)r; (void)a; (void)b; (void)w; (void)c; }
void *read_fx_param_safe(int slot, int pos) { (void)slot; (void)pos; return 0; }
/* a knob turn landing between the Shift loop and the word refresh (the core's thread racing the screen's) */
static int race_k = -1, race_d;
#define MM_RACE_HOOK() do { if (race_k >= 0) { host_knobs[host_slot][race_k] += race_d; race_k = -1; } } while (0)
#define MM_HOST_UI
#include "../moai.c"
#include "../moai_ui.c"

static int fails;
#define CHECK(cond, ...) do { if (cond) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)
static int16_t *W(void) { return host_knobs[host_slot]; }
static void fw_turn(int k, int d)
{
    int16_t *w = W();
    if (op1_eng_page[k]) { int i = OP1_ENG_PAGE_WORD(k); int v = (w[i] > 0 ? w[i] : 8192) + d / 4; w[i] = (int16_t)(v < 1 ? 1 : (v > 32767 ? 32767 : v)); }
    else { int v = w[k] + d; w[k] = (int16_t)(v < 0 ? 0 : (v > 32767 ? 32767 : v)); }
}
static void fw_press(int k) { op1_eng_page[k] ^= 1; }
#define CLICK   256                     /* one encoder click in this emulation (Shift: CLICK_S) */
#define CLICK_S 16
static void frame(void) { mg_screen_update(0, 16); }
static void to_entry(int e)                                     /* blue on home, click by click, the short way round */
{
    for (int i = 0; i < N_ENTRIES && entry_of(W()[0]) != e; i++) {
        int d = (MM_POS[e] - MM_POS[entry_of(W()[0])] + N_ENTRIES) % N_ENTRIES;
        fw_turn(0, d <= N_ENTRIES / 2 ? CLICK : -CLICK); frame();
    }
}
static void to_entry_keep(int e)       /* ... and the other words back as they were (presets on the way write theirs) */
{
    int16_t w[8]; memcpy(w, W(), sizeof w);
    to_entry(e);
    w[0] = W()[0]; w[6] = W()[6]; memcpy(W(), w, sizeof w);
    for (int k = 1; k < 4; k++) P[k] = ui_prev[k] = w[k];                    /* quietly: not a load (white box) */
    for (int j = 0; j < 3; j++) { ui_page[j] = w[5 + j]; P[P_DETUNE + j] = w[5 + j]; }
    frame();
}
static void play_block(void)                                    /* the engine renders with the live words */
{
    int16_t out[OP1_BLOCK];
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = 25480370;
    ENGINE_moai.render(host_voice_mem, out, W());
}

int main(void)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    int16_t start[8] = { 1 * 1024 + 512, 15000, 9000, 12000, 0x5301, 0, 0, 0 };   /* FAT BASS */
    memcpy(W(), start, sizeof start);
    host_voice_vel[0] = 100 << 8;
    ENGINE_moai.note_on(host_voice_mem, W());
    frame(); play_block();
    CHECK(mm_view == V_HOME && P[P_CUTOFF] == 15000 && P[P_O1_RANGE] == R16, "home view: P follows the words, FAT BASS derived");

    fw_turn(1, 1000); frame();
    CHECK(P[P_CUTOFF] == 16000 && W()[1] == 16000, "home: green turns the cutoff word (stock path)");
    CHECK(ui_idle[1] == 0 && ui_idle[2] > 0, "home: the green turn is seen (its knob and the mountain light)");
    fw_turn(2, 500); fw_turn(3, 500); frame();
    CHECK(ui_idle[2] == 0 && ui_idle[3] == 0 && ui_idle[1] == 1, "home: white and red turns are seen too");
    fw_turn(2, -500); fw_turn(3, -500); frame();

    int sk = host_set_knobs_calls;
    fw_press(1); frame();
    CHECK(mm_view == V_OSC1 && op1_eng_page[1] == 0, "green press -> osc 1, page flag cleared");
    CHECK(host_set_knobs_calls == sk && W()[1] == 16000 && W()[0] == clampcap(1536), "words borrowed (at the home value, kept off the ends), not saved yet");
    for (int f = 0; f < SH_SETTLE; f++) frame();
    fw_turn(3, -CLICK); frame();
    CHECK(ui_click[0] == 2048, "one single click is not the click yet (seen once)");
    fw_turn(3, CLICK); frame();                                              /* single clicks: measured (volume */
    host_shift = 1; for (int f = 0; f < SH_SETTLE; f++) frame();             /* is at the top) */
    fw_turn(3, -CLICK_S); frame(); fw_turn(3, CLICK_S); frame(); host_shift = 0;
    CHECK(ui_click[0] == CLICK && ui_click[1] == CLICK_S, "a click's movement measured (seen twice), with and without Shift");
    /* Shift released mid-turn: the core's last Shift-sized turn arrives in a frame without Shift (twice) */
    int vol = P[P_O1_VOL];
    host_shift = 1; for (int f = 0; f < SH_SETTLE; f++) frame();
    fw_turn(3, CLICK_S); host_shift = 0; frame(); frame();
    host_shift = 1; fw_turn(3, -CLICK_S); host_shift = 0; frame(); frame();
    CHECK(ui_click[0] == CLICK, "Shift released mid-turn: the Shift-sized turns are not taken for a plain click");
    int wv = P[P_O1_WAVE];
    fw_turn(1, 2 * CLICK); frame();                                          /* 2 of the 3 clicks of a wave step */
    host_shift = 1; fw_turn(1, CLICK_S); frame(); host_shift = 0; frame();
    CHECK(P[P_O1_WAVE] == wv, "plain clicks waiting in the accumulator do not count as Shift clicks (no jump)");
    for (int f = 0; f < SH_SETTLE; f++) frame();
    ui_click[0] = CLICK_S;                                                   /* measured wrong somehow */
    for (int i = 0; i < RECOVER; i++) { fw_turn(3, i & 1 ? CLICK : -CLICK); frame(); frame(); frame(); }
    CHECK(ui_click[0] == CLICK, "a click measured too small comes back after %d single clicks", RECOVER);
    P[P_O1_WAVE] = (int16_t)wv; P[P_O1_VOL] = (int16_t)vol;
    fw_turn(0, STEP_CLICKS * CLICK); frame();
    CHECK(P[P_O1_RANGE] == R8 && W()[0] == ui_cap[0], "osc 1: blue = range one step up per 3 clicks (16' -> 8'), word set back");
    fw_turn(2, 2 * STEP_CLICKS * CLICK); frame();
    CHECK(P[P_O1_FREQ] == 200, "osc 1: white = +2 semitones");
    host_shift = 1; fw_turn(2, 10 * CLICK_S); frame(); host_shift = 0;
    CHECK(P[P_O1_FREQ] == 210, "osc 1: Shift + white = fine (+10 cents, one per click)");
    fw_turn(3, -8192); frame();
    CHECK(P[P_O1_VOL] == 32764 - 8192, "osc 1: red = volume");
    play_block();
    CHECK(P[P_CUTOFF] == 16000 && P[P_O1_RANGE] == R8, "rendering on another view keeps the home values and the edits");

    fw_press(1); frame();
    CHECK(mm_view == V_OSC2, "green again -> osc 2");
    host_shift = 1; fw_press(1); frame(); host_shift = 0;
    CHECK(mm_view == V_OSC1, "Shift + green -> back to osc 1");

    fw_press(0); frame();
    CHECK(mm_view == V_HOME && W()[0] == 1536 && W()[1] == 16000 && W()[2] == 9000 && W()[3] == 12000, "blue -> home, home words restored exactly");
    CHECK(P[P_O1_RANGE] == R8 && P[P_O1_FREQ] == 210, "oscillator edits survive the view change");

    to_entry_keep(2); play_block();                                              /* SUB BASS */
    CHECK(P[P_O1_RANGE] == R32 + 0 || P[P_O1_RANGE] == SETUPS[2].o[0].r, "turning the setup knob sets the oscillators from the new setup");
    CHECK(P[P_O1_FREQ] == 0, "... and drops the edits");
    CHECK(W()[6] == 1 && P[P_SAT] == 0, "SUB BASS's character: clean (drive 1x into the word, no saturation)");

    fw_press(3); frame();
    CHECK(mm_view == V_ANALOG, "red -> analog");
    fw_turn(0, 3000); frame();
    CHECK(W()[5] == 11600 + 3000 && P[P_DETUNE] == 14600, "analog: blue = detune, from its default (5 cents), stored in word 5");
    fw_turn(2, 8000); frame();
    CHECK(W()[6] == 8001, "analog: white = drive, stored in word 6");
    fw_turn(3, 6000); frame();
    CHECK(P[P_SAT] == 6000 && W()[6] == 8001, "analog: red = saturation (session)");
    fw_press(3); frame();
    CHECK(mm_view == V_ANALOG, "red again: stays on analog");
    sk = host_set_knobs_calls;
    for (int f = 0; f < SAVE_WAIT + 2; f++) frame();
    CHECK(host_set_knobs_calls == sk + 1, "the words go into the sound once, after the knobs rest");
    {
        const int16_t *sv = host_saved[host_slot];
        CHECK(sv[0] == P[P_SETUP] && sv[1] == P[P_CUTOFF] && sv[2] == P[P_EMPH] && sv[3] == P[P_CONTOUR] && sv[0] != W()[0],
              "... the home words (%d %d %d %d), not the ones the view borrows", sv[0], sv[1], sv[2], sv[3]);
        CHECK(sv[5] == 14600 && sv[6] == 8001, "... with the analog view's detune and drive");
    }
    fw_press(0); frame();
    CHECK(mm_view == V_HOME && W()[5] == 14600 && W()[6] == 8001, "blue -> home, analog words kept");
    to_entry_keep(11);
    CHECK(W()[6] == pct(70) && P[P_SAT] == pct(45) && P[P_FEEDBACK] == pct(25), "turned to SCREAMER: its drive (in the word), saturation, feedback");
    to_entry_keep(9);
    CHECK(W()[6] == pct(SETUPS[9].drv) && P[P_SAT] == pct(SETUPS[9].sat) && P[P_FEEDBACK] == 0, "on to SQ LEAD: its own drive and saturation, not SCREAMER's");
    to_entry_keep(2);

    fw_press(2); frame();
    CHECK(mm_view == V_FILTER && mm_map[0] == P_CUTOFF, "white -> filter (blue = cutoff)");
    fw_turn(0, -3000); frame();
    int cut = P[P_CUTOFF];
    host_shift = 1; fw_turn(0, 3 * CLICK_S); frame(); host_shift = 0; frame();
    CHECK(P[P_CUTOFF] == cut + 3 * CUT_FINE, "filter: Shift + cutoff = 1/2048 of the range per click");
    P[P_CUTOFF] = (int16_t)cut;
    {
        char t[16];
        mg_value(t, P_CUTOFF, 14613); CHECK(!strcmp(t, "A4"), "cutoff on 440 Hz shows A4 (%s)", t);
        mg_value(t, P_CUTOFF, 14613 + 273); CHECK(!strcmp(t, "A#4"), "... A#4 one semitone up (%s)", t);
        mg_value(t, P_CUTOFF, 14613 + 136); CHECK(t[strlen(t) - 1] == 'Z', "... between notes: Hz (%s)", t);
        mg_value(t, P_CUTOFF, 0); CHECK(!strcmp(t, "20HZ"), "... 20 Hz is not on a note (%s)", t);
    }
    fw_press(2); frame();
    CHECK(mm_view == V_CONTOUR, "white again -> contour");
    int k8 = knob_of_ms(div_ms(4));                                          /* 1/8 at 120 BPM */
    ui_sync[1] = -1; P[P_DECAY] = (int16_t)(k8 - 2 * CLICK - 50);
    fw_turn(1, CLICK); frame();
    CHECK(ui_sync[1] < 0 && P[P_DECAY] == k8 - CLICK - 50, "contour: decay turns freely, as a continuous knob");
    fw_turn(1, CLICK); frame();
    CHECK(ui_sync[1] == 4 && P[P_DECAY] == k8, "a click within half a click of a note value lands on it (%s at 120 BPM)", ui_sync[1] >= 0 ? DIVS[ui_sync[1]].t : "-");
    fw_turn(1, CLICK); frame();
    CHECK(ui_sync[1] == 4 && P[P_DECAY] == k8, "... and holds it %d more click", SYNC_HOLD);
    fw_turn(1, CLICK); frame();
    CHECK(ui_sync[1] < 0 && P[P_DECAY] == k8 + CLICK, "... then turns on freely");
    fw_turn(1, -CLICK); frame();
    CHECK(ui_sync[1] == 4 && P[P_DECAY] == k8, "back onto it from above");
    P[P_DECAY] = (int16_t)(k8 - FINE_UNITS / 2); ui_sync[1] = -1;
    int dec = P[P_DECAY];
    host_shift = 1; fw_turn(1, 4 * CLICK_S); frame(); host_shift = 0;
    CHECK(ui_sync[1] < 0 && P[P_DECAY] == dec + 4 * FINE_UNITS, "Shift: free and fine (4 clicks = 4 x 1/256), right over a note value");
    fw_press(0); frame();
    CHECK(W()[1] == 13000, "the cutoff edited on the filter view is the stored home knob");

    fw_press(0); frame();
    CHECK(mm_view == V_ADV, "blue on home -> advanced");
    int8_t before = ui_hl[0];
    host_shift = 1; fw_turn(0, CLICK_S); frame(); host_shift = 0;
    CHECK(ui_hl[0] != before && ui_hl[0] != ui_hl[1] && ui_hl[0] != ui_hl[2] && ui_hl[0] != ui_hl[3], "Shift + blue: one click moves blue's highlight one control (never onto another)");
    int id = ui_hl[0], v0 = P[id];
    fw_turn(0, 2048); frame();
    CHECK(P[id] != v0 || PD[id].kind == K_STEP, "blue turns the highlighted control");

    /* a turn racing a press: the core sends it to the page word before the next frame */
    fw_press(0); frame();                                        /* home */
    fw_press(1); fw_turn(1, 4096); frame();
    CHECK(mm_view == V_OSC1 && W()[6] == 1, "a turn right after a press: page word put back");
    CHECK(P[P_O1_WAVE] != SETUPS[2].o[0].w, "... and the turn went to the view's knob");
    fw_press(1); frame(); fw_press(1); frame(); fw_press(1); frame();
    CHECK(mm_view == V_NOISE && mm_map[3] == P_NOISE && mm_map[0] == P_MOD_MIX && mm_map[1] == P_MOD_PITCH && mm_map[2] == P_MOD_FILT,
          "green: osc 1 -> 2 -> 3 -> modulation row (mix, osc, filter, noise)");
    int nz = P[P_NOISE]; fw_turn(3, 5000); fw_turn(0, 5000); frame();
    CHECK(P[P_NOISE] == nz + 5000 && P[P_MOD_MIX] == 5000, "modulation row: red = noise level, blue = mod mix");
    P[P_MOD_MIX] = 0;
    fw_press(1); frame();
    CHECK(mm_view == V_OSC1, "green again: back to osc 1");
    host_shift = 1; fw_press(1); frame(); host_shift = 0;
    CHECK(mm_view == V_NOISE, "Shift + green from osc 1: the noise row");
    for (int f = 0; f < 10; f++) frame();

    /* a sound loaded while on another view */
    int16_t other[8] = { 27 * 1024 + 512, 30000, 20000, 25000, 0x5301, 100, 200, 300 };
    memcpy(W(), other, sizeof other); frame();
    CHECK(mm_view == V_HOME && P[P_CUTOFF] == 30000 && W()[1] == 30000 && P[P_O2_FREQ] == 300, "a loaded sound: home view, its own words, setup MINOR derived");
    CHECK(W()[6] == 200, "... and its own drive (a setup never overrides a loaded sound's)");
    for (int f = 0; f < 10; f++) frame();
    int16_t other2[8] = { 11 * 1024 + 512, 20000, 9000, 25000, 0x5301, 100, 200, 300 };   /* page words the same */
    memcpy(W(), other2, sizeof other2); frame();
    CHECK(W()[6] == 200, "a sound loaded on home (SCREAMER setup, same page words): still its own drive");

    /* extremes: a home knob at the end still turns both ways on another view */
    W()[1] = 32767; frame(); fw_press(2); frame();
    fw_turn(0, 2000); frame();
    CHECK(P[P_CUTOFF] == 32767, "cutoff at the top: turning up on the filter view stays at the top");
    fw_turn(0, -2000); frame();
    CHECK(P[P_CUTOFF] == 30767, "... and turns down");
    fw_press(0); frame();
    CHECK(W()[1] == 30767, "home: the edited cutoff in its word");

    /* another slot while on another view: the old slot gets its home words back */
    fw_press(1); frame();
    int16_t keep = W()[1];
    host_slot = 1; memcpy(W(), start, sizeof start); frame();
    CHECK(host_knobs[0][1] == 30767 && keep == clampcap(30767) && mm_view == V_HOME, "slot change: old slot's home words restored, new slot on home");

    /* LFO on another view: the knob word's offset goes to that view's parameter */
    fw_press(1); frame();                                        /* osc 1, setup FAT BASS: 16' */
    int16_t p[8]; memcpy(p, W(), sizeof p); p[0] = (int16_t)(W()[0] + 12000);
    int16_t out[OP1_BLOCK]; for (int i = 0; i < 128; i++) host_pitch_inc[i] = 25480370;
    ENGINE_moai.render(host_voice_mem, out, p);
    CHECK(P[P_O1_RANGE] == R16, "an LFO offset on blue modulates osc 1's range for that block only (P unchanged)");
    frame();
    CHECK(mg_lv(P_O1_RANGE) == pclamp(P_O1_RANGE, R16 + poffset(P_O1_RANGE, 12000 * LFO_GAIN)), "... and the screen shows the range it plays (the needle follows the LFO)");
    for (int f = 0; f < 10; f++) frame();
    CHECK(mg_lv(P_O1_RANGE) == R16, "no note for a few frames: the needle back on the knob's value");

    /* home + Shift: the second layer */
    fw_press(0); frame();
    for (int f = 0; f < 10; f++) frame();
    int16_t emp = W()[2], amt = P[P_AMOUNT], det = W()[5];
    host_shift = 1; frame();
    CHECK(mm_map[2] == P_AMOUNT && mm_map[0] == P_DETUNE, "home + Shift: the knobs show DET, DRV, AMT, SAT");
    fw_turn(2, 3 * CLICK_S); frame();
    CHECK(W()[2] == emp && P[P_AMOUNT] == amt + 3 * CLICK, "Shift + white on home: contour amount, 3 clicks = 3 plain clicks, emphasis word put back");
    fw_turn(0, CLICK_S); frame();
    CHECK(W()[5] == page_start(P_DETUNE, det) + CLICK, "Shift + blue on home: detune (stored)");
    host_shift = 0; frame();
    CHECK(mm_map[2] == P_EMPH && W()[2] == emp, "Shift released: home knobs again");
    {   /* user report 2026-11-08: on a preset, Shift + blue sometimes moved AMT and left DET (a race, MM_RACE_HOOK) */
        int e0 = entry_of(W()[0]), pe = 0;
        while (pe < N_ENTRIES && !preset_of(pe)) pe++;
        to_entry_keep(pe);
        for (int f = 0; f < 10; f++) frame();
        int16_t amt0 = P[P_AMOUNT], det0 = W()[5];
        host_shift = 1; frame();
        for (int i = 0; i < 6; i++) { race_k = 0; race_d = i & 1 ? -CLICK_S : CLICK_S; frame(); frame(); }
        race_k = 0; race_d = CLICK_S; frame(); frame(); frame();
        CHECK(entry_of(W()[0]) == pe && P[P_AMOUNT] == amt0 && W()[5] != det0,
              "Shift + blue on a preset, the turn landing late: still the preset, AMT kept, DET moved (%d %d)", W()[5], det0);
        /* ... and the audio thread in the gap: the core has added a Shift click to word 0 (on a preset it crosses into
           the next entry) and the screen has not taken it back yet. The sound and AMT stay (user report 2026-11-08:
           DET did nothing and AMT moved) */
        fw_turn(2, 3 * CLICK_S); frame();                            /* AMT edited (a session value) */
        int16_t amt1 = P[P_AMOUNT], cut1 = P[P_O1_RANGE];
        for (int i = 0; i < 4; i++) {
            fw_turn(0, i & 1 ? -CLICK_S : CLICK_S);
            play_block(); play_block();                              /* audio blocks before the next frame */
            CHECK(mm_derived_setup == pe && P[P_AMOUNT] == amt1 && P[P_O1_RANGE] == cut1,
                  "Shift + blue on a preset, audio before the frame: same sound, AMT kept (%d %d, AMT %d %d)",
                  mm_derived_setup, pe, P[P_AMOUNT], amt1);
            frame();
        }
        CHECK(P[P_AMOUNT] == amt1 && entry_of(W()[0]) == pe, "... and after the frames: AMT %d (was %d), still the preset", P[P_AMOUNT], amt1);
        host_shift = 0; frame();
        to_entry_keep(e0);
    }

    /* LO and modulation (Model D: osc 3 in LO as the source) */
    host_shift = 0; frame();
    fw_press(1); frame();                                        /* osc 1 */
    P[P_O1_RANGE] = R32; ui_acc[0] = 0;
    fw_turn(0, -STEP_CLICKS * CLICK); frame();
    CHECK(P[P_O1_RANGE] == R_LO, "osc 1: range one step down from 32' is LO");
    {
        char t[16]; mg_value(t, P_O1_FREQ, 0);
        CHECK(!strcmp(t, "4.1HZ"), "LO: the frequency shows the rate (%s at C4)", t);
        mg_value(t, P_O1_FREQ, 300); CHECK(!strcmp(t, "8.2HZ"), "... +3 on the knob: 4x 3 semitones, an octave (%s)", t);
        mg_value(t, P_O1_FREQ, 800); CHECK(!strcmp(t, "26HZ"), "... the top: 26 Hz (%s)", t);
    }
    fw_press(0); frame();                                        /* home */
    for (int i = 0; i < 3; i++) { P[P_O(i, 0)] = R8; P[P_O(i, 1)] = W_SAW; P[P_O(i, 2)] = 0; P[P_O(i, 3)] = 32767; }
    P[P_O3_RANGE] = R_LO; P[P_O3_WAVE] = W_SQUARE; P[P_O3_VOL] = 0; P[P_AMOUNT] = 0; P[P_NOISE] = 0;
    P[P_MOD_MIX] = 0; P[P_MOD_PITCH] = 0; P[P_MOD_FILT] = 32767;
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = 25480370;
    ENGINE_moai.note_on(host_voice_mem, W());
    int32_t pmin = 1 << 30, pmax = -(1 << 30), flips = 0, last = 0;
    for (int b = 0; b < 400; b++) {
        int16_t o[OP1_BLOCK]; ENGINE_moai.render(host_voice_mem, o, W());
        if (b > 40) { if (V[0].pos < pmin) pmin = V[0].pos; if (V[0].pos > pmax) pmax = V[0].pos; }
        int up = V[0].pos > (pmin + pmax) / 2;
        if (b > 41 && up != last) flips++;
        last = up;
    }
    CHECK(pmax - pmin > 5800 && pmax - pmin < 6300, "filter mod, osc 3 LO square (silent): the cutoff jumps +-3 octaves (%d Q10)", pmax - pmin);
    CHECK(flips >= 7 && flips <= 11, "... at osc 3's rate: %d half cycles in 1 s (4.1 Hz)", flips);
    P[P_MOD_FILT] = 0; P[P_MOD_PITCH] = 32767;
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = 25480370 / 4;   /* C2: a block's phase fits 32 bits 2 octaves up */
    uint32_t dmin = 0xFFFFFFFFu, dmax = 0;
    for (int b = 0; b < 400; b++) {
        int16_t o[OP1_BLOCK]; uint32_t p0 = V[0].ph[0];
        ENGINE_moai.render(host_voice_mem, o, W());
        uint32_t d = V[0].ph[0] - p0;
        dmin = d < dmin ? d : dmin; dmax = d > dmax ? d : dmax;
    }
    CHECK(dmax / (dmin / 100) > 380 && dmax / (dmin / 100) < 420, "osc mod, full: osc 1 trills two octaves apart (x%d.%02d)", dmax / dmin, dmax / (dmin / 100) % 100);
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = 25480370;
    P[P_MOD_PITCH] = 0; P[P_MOD_FILT] = 32767; P[P_MOD_MIX] = 32767; P[P_O3_RANGE] = R8;
    pmin = 1 << 30; pmax = -(1 << 30);
    for (int b = 0; b < 100; b++) {
        int16_t o[OP1_BLOCK]; ENGINE_moai.render(host_voice_mem, o, W());
        if (b > 10) { if (V[0].pos < pmin) pmin = V[0].pos; if (V[0].pos > pmax) pmax = V[0].pos; }
    }
    CHECK(pmax - pmin > 3000, "mod mix at noise (osc 3 not in LO): the cutoff jitters (%d Q10)", pmax - pmin);
    P[P_MOD_MIX] = 0;
    pmin = 1 << 30; pmax = -(1 << 30);
    for (int b = 0; b < 100; b++) {
        int16_t o[OP1_BLOCK]; ENGINE_moai.render(host_voice_mem, o, W());
        if (b > 10) { if (V[0].pos < pmin) pmin = V[0].pos; if (V[0].pos > pmax) pmax = V[0].pos; }
    }
    CHECK(pmax - pmin == 0, "osc 3 in an audio range: no modulation (no FM here)");
    P[P_MOD_FILT] = 0;

    /* one numbered list on blue (000 ..): setups and presets shuffled together, the initial sound at 000, wrapping */
    int rt = 1, perm = 1;
    for (int e = 0; e < N_ENTRIES; e++) rt &= entry_of(entry_word(e)) == e && entry_word(e) > 0 && entry_word(e) < 32767;
    for (int n = 0; n < N_ENTRIES; n++) perm &= MM_POS[MM_ORDER[n]] == n;
    CHECK(rt, "every setup and preset has its own word value (%d entries), never at the ends", N_ENTRIES);
    CHECK(perm && MM_ORDER[0] == MM_INITIAL, "the numbers are a permutation of the entries, the initial sound at 000");
    CHECK(MM_ORDER[1] == MM_INITIAL + 1, "001: the Chase bass, right after it");
    CHECK(entry_of(1536) == 1 && entry_of(1100) == 1 && entry_of(0) == 0 && entry_of(32767) == 31 && entry_of(31 * 1024 + 1013) == 31 && entry_of(30 * 1024 + 1019) == 30,
          "a setup saved before (anywhere in its slot) is still that setup");
    CHECK(entry_step(MM_ORDER[N_ENTRIES - 1], 1) == MM_ORDER[0] && entry_step(MM_ORDER[0], -1) == MM_ORDER[N_ENTRIES - 1]
          && entry_step(MM_ORDER[5], -7) == MM_ORDER[N_ENTRIES - 2], "stepping wraps around both ends");
    {
        char t[16], x[8];
        mg_value(t, P_SETUP, entry_word(MM_ORDER[0])); CHECK(!strcmp(t, "000"), "numbers only, three digits (%s", t);
        mg_value(t, P_SETUP, entry_word(4)); snprintf(x, sizeof x, "%03d", MM_POS[4]); CHECK(!strcmp(t, x), "... a setup: %s", t);
        mg_value(t, P_SETUP, entry_word(MM_ORDER[N_ENTRIES - 1])); snprintf(x, sizeof x, "%03d", N_ENTRIES - 1);
        CHECK(!strcmp(t, x), "... the last: %s)", t);
    }
    for (int f = 0; f < 10; f++) frame();
    CHECK(mm_view == V_HOME, "home again");
    to_entry(N_SETUPS); play_block();
    const preset_t *pr = &PRESETS[0];
    CHECK(entry_of(W()[0]) == N_SETUPS && W()[1] == pct(pr->cut) && W()[2] == pct(pr->emp) && W()[3] == pct(pr->con)
          && W()[5] == pct(pr->det) && W()[6] == pct(pr->drv), "turned to a preset: its cutoff, emphasis, contour, detune and drive into the words");
    CHECK(P[P_SAT] == pct(pr->sat) && P[P_O2_RANGE] == pr->o[1].r && P[P_O2_FREQ] == pr->o[1].semi * 100 && P[P_O3_RANGE] == pr->o[2].r,
          "... its saturation and oscillators");
    int am = P[P_AMOUNT];
    CHECK(am >= pct(pr->amt) - 2 && am <= pct(pr->amt) + 2, "... and its contour amount (%d for %d)", am, pct(pr->amt));
    int16_t saved[8]; memcpy(saved, W(), sizeof saved);
    memcpy(W(), start, sizeof start); frame(); frame();
    memcpy(W(), saved, sizeof saved); frame(); frame(); play_block();
    CHECK(P[P_SAT] == pct(pr->sat) && P[P_AMOUNT] == am && P[P_O2_FREQ] == pr->o[1].semi * 100 && W()[6] == pct(pr->drv),
          "a preset saved and loaded again: all of it back from the seven words");
    int e0 = entry_of(W()[0]);
    fw_turn(0, CLICK); frame();
    CHECK(entry_of(W()[0]) == entry_step(e0, 1), "one click on: the next number (%03d -> %03d)", MM_POS[e0], MM_POS[entry_of(W()[0])]);
    to_entry(31);
    CHECK(W()[1] == pct(SETUPS[31].cut) && W()[2] == pct(SETUPS[31].emp) && W()[3] == pct(SETUPS[31].con)
          && W()[6] == (SETUPS[31].drv ? pct(SETUPS[31].drv) : 1) && W()[7] == DRIFT_DEFAULT,
          "a setup after a preset: its own cutoff, emphasis, contour, drive; drift back to its default");
    {   /* every number sounds the same from either side (v3.19: a setup kept the sound before it's filter) */
        int bad = 0, first = -1;
        for (int n = 0; n < N_ENTRIES; n++) {
            int e = MM_ORDER[n], nb[2] = { MM_ORDER[(n + N_ENTRIES - 1) % N_ENTRIES], MM_ORDER[(n + 1) % N_ENTRIES] };
            int16_t got[2][P_N];
            for (int s = 0; s < 2; s++) {
                to_entry(nb[s]);
                W()[7] = ui_page[2] = (int16_t)(9000 + 4000 * s);                 /* your edits on the way (white box) */
                P[P_SAT] = P[P_FEEDBACK] = (int16_t)(5000 * s); P[P_O1_FREQ] = (int16_t)(100 * s); P[P_ATTACK] = (int16_t)(3000 * s);
                to_entry(e);
                for (int i = 0; i < P_N; i++) got[s][i] = P[i];
                for (int j = 0; j < 3; j++) got[s][P_DETUNE + j] = W()[5 + j];
            }
            if (memcmp(got[0], got[1], sizeof got[0])) { bad++; if (first < 0) first = n; }
        }
        CHECK(!bad, "every number: the same values whichever side it is reached from (%d differ, first %03d)", bad, first);
    }
    to_entry(MM_ORDER[N_ENTRIES - 1]);
    fw_turn(0, CLICK); frame();
    CHECK(entry_of(W()[0]) == MM_ORDER[0], "past the last: back to 000 (%03d)", MM_POS[entry_of(W()[0])]);
    fw_turn(0, -CLICK); frame();
    CHECK(entry_of(W()[0]) == MM_ORDER[N_ENTRIES - 1], "and back down to the last");

    /* the engine chosen on a slot: plugin.json's defaults (000, the Jestofunk bass), words 5..7 never set */
    {
        int16_t fresh[8] = { 1013, 3932, 26213, 21626, 0x5301, 0, 0, 0 };   /* plugin.json "defaults" */
        pr = &PRESETS[MM_INITIAL - N_SETUPS];
        CHECK(entry_of(fresh[0]) == MM_INITIAL && fresh[1] == pct(pr->cut) && fresh[2] == pct(pr->emp) && fresh[3] == pct(pr->con),
              "plugin.json's defaults are the initial preset's knobs");
        host_slot = 2; memcpy(W(), fresh, sizeof fresh); frame(); frame(); play_block();
        CHECK(mm_view == V_HOME && W()[5] == pct(pr->det) && W()[6] == pct(pr->drv) && W()[7] == DRIFT_DEFAULT,
              "a fresh sound on a preset gets its detune and drive, drift its default (%d %d)", W()[5], W()[6]);
        CHECK(P[P_O3_RANGE] == R32 && P[P_O3_WAVE] == W_SQUARE && P[P_SAT] == 0, "... and its oscillators (32' square sub)");
        int16_t own[8] = { 1013, 3932, 26213, 21626, 0x5301, 9000, 5000, 0 };
        host_slot = 3; memcpy(W(), own, sizeof own); frame(); frame();
        CHECK(W()[5] == 9000 && W()[6] == 5000, "a saved sound on that preset keeps its own detune and drive");
        /* session edits (not in the 7 words) do not follow into another sound with the same setup and contour */
        int16_t o1f = P[P_O1_FREQ], att = P[P_ATTACK];
        P[P_O1_FREQ] = 700; P[P_ATTACK] = 20000; ui_sync[1] = 4;
        host_slot = 4; memcpy(W(), own, sizeof own); frame(); frame();
        CHECK(P[P_O1_FREQ] == o1f && P[P_ATTACK] == att && ui_sync[1] < 0,
              "another slot with the same sound: its own oscillators and contour, not the session edits (%d %d)", P[P_O1_FREQ], P[P_ATTACK]);
        P[P_O1_FREQ] = 700;
        memcpy(W(), own, sizeof own); W()[1] = 12000; W()[2] = 3000; frame(); frame();
        CHECK(P[P_O1_FREQ] == o1f, "a sound loaded on the same slot, same setup and contour: rebuilt too (%d)", P[P_O1_FREQ]);
        host_slot = 0;
    }
    printf(fails ? "%d FAILED\n" : "ui ok\n", fails);
    return fails != 0;
}
