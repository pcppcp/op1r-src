/* moai views and knobs (GUI thread: mm_ui_frame runs at the start of every screen update; included by
 * moai_ui.c).
 *
 * The plugin API gives an engine no key or encoder events, only the knob words and a page flag per encoder press
 * (op1_eng_page, toggled by the core). So:
 *   - a press = the page flag set: it is cleared at once (turns then go to the knob words again) and switches the
 *     view; Shift is read from the firmware (0x01DD7F78), as the core does.
 *       blue   home <-> advanced (from any other view: home)
 *       green  oscillator 1 -> 2 -> 3 -> 1 (Shift: backwards; from another view: osc 1, Shift: osc 3)
 *       white  filter <-> contour
 *       red    analog
 *   - on the home view the knobs are the home knobs (stored, as any engine's knobs);
 *     on any other view the four knob words are borrowed: each frame their movement (the word minus what it was
 *     set to) edits the parameter of that knob in the view, and the word is set back. The home values wait in P[]
 *     and go back into the words when the home view returns. The borrowed words sit at the home value, kept 6144
 *     away from both ends (so a knob can always turn both ways), and are written into the sound with the patch
 *     setter as the core's page-2 words are: a sound saved on another view keeps its home knobs within that margin.
 *   - the analog view's detune / drive / drift are stored in words 5..7 (written with the patch setter).
 *   - a sound loaded on another view (its words jump) or another slot: back to the home view.
 * Turn sizes: continuous parameters move as the stock knobs do (Shift: fine, FINE_UNITS per click, the cutoff
 * CUT_FINE); steps (range, wave) and semitones every STEP_CLICKS clicks, setups and presets every SETUP_CLICKS;
 * contour times as continuous ones, held on a note value they pass by for SYNC_HOLD more clicks (Shift: free, fine, no note values); Shift on a
 * frequency = one cent per click. On the advanced view Shift + turn moves that knob's highlight to the next control
 * per click (highlights never share a control). */

#ifndef OP1_HOST
#define MM_SHIFT (*(volatile uint8_t *)0x01DD7F78)                    /* Shift held (the core reads it there too) */
#define MM_SET_KNOBS(slot, w) ((void (*)(void *, const int16_t *))0x01A00D14)((void *)(0x01DDD7E8 + ((slot) & 7) * 0x38), (w))
#else
#ifndef MM_HOST_UI                                                    /* host programs that do not drive the UI */
__attribute__((weak)) int host_shift;                          /* op1r: int, as harness/op1_host.h */
__attribute__((weak)) int host_set_knobs_calls;
__attribute__((weak)) void host_set_knobs(int slot, const int16_t *w) { (void)slot; (void)w; host_set_knobs_calls++; }
#else                                                                 /* test/mm_ui.c: its own */
extern int host_shift;
void host_set_knobs(int slot, const int16_t *w);
#endif
#define MM_SHIFT host_shift
#define MM_SET_KNOBS(slot, w) host_set_knobs((slot), (w))
#endif

/* Turn sizes are counted in clicks of the encoder. The knob movement of one click is not known (and Shift makes
 * the stock turns much smaller): it is measured, as the smallest movement seen in a frame, with and without Shift
 * (ui_click; a fast turn only adds clicks up, it never gives less than one). */
#define STEP_CLICKS 3                   /* a step (range, wave) or a semitone */
#define SETUP_CLICKS 1                  /* a setup or preset (000 .. 126 on the blue knob) */
#define SYNC_HOLD   1                   /* clicks a contour time stays on a note value it reached */
#define HL_CLICKS   1                   /* a highlight move (Shift, advanced view) */
#define FINE_UNITS  128                 /* Shift: a continuous value moves 1/256 of its range per click */
#define CUT_FINE    16                  /* Shift: the cutoff moves 1/2048 of its range per click (~6 cents) */
#define SAVE_WAIT   30                  /* frames without a turn before the words go into the sound (~0.5 s) */
#define CAP_MARGIN  6144                /* borrowed words stay this far from the ends */
#define LOAD_JUMP   8000                /* a word jumping more than this in one frame (on 2+ knobs): a loaded sound */

/* the advanced view: every parameter at a place on the panel, in reading order (the highlights move along it).
 * One grid: 3 rows, 11 columns in 4 sections (oscillators, mixer, modifiers, analog), 24 px apart within a section */
#define ADV_C(i) (21 + (i) * 24 + ((i) >= 3) * 14 + ((i) >= 5) * 14 + ((i) >= 9) * 14)   /* column centres, px */
#define ADV_R(j) (22 + (j) * 30)                                                         /* row centres, px */
typedef struct { int8_t id; int16_t x, y; } adv_t;
static const adv_t ADV[] = {
    /* row 1: oscillator 1 (range, wave, frequency: as on its own view), its volume, the filter, detune and drift */
    { P_O1_RANGE, ADV_C(0), ADV_R(0) }, { P_O1_WAVE, ADV_C(1), ADV_R(0) }, { P_O1_FREQ, ADV_C(2), ADV_R(0) },
    { P_O1_VOL, ADV_C(3), ADV_R(0) },
    { P_CUTOFF, ADV_C(5), ADV_R(0) }, { P_EMPH, ADV_C(6), ADV_R(0) }, { P_AMOUNT, ADV_C(7), ADV_R(0) }, { P_FEEDBACK, ADV_C(8), ADV_R(0) },
    { P_DETUNE, ADV_C(9), ADV_R(0) }, { P_DRIFT, ADV_C(10), ADV_R(0) },
    /* row 2: oscillator 2, its volume, the contour, drive and saturation */
    { P_O2_RANGE, ADV_C(0), ADV_R(1) }, { P_O2_WAVE, ADV_C(1), ADV_R(1) }, { P_O2_FREQ, ADV_C(2), ADV_R(1) },
    { P_O2_VOL, ADV_C(3), ADV_R(1) },
    { P_ATTACK, ADV_C(5), ADV_R(1) }, { P_DECAY, ADV_C(6), ADV_R(1) }, { P_SUSTAIN, ADV_C(7), ADV_R(1) }, { P_RELEASE, ADV_C(8), ADV_R(1) },
    { P_DRIVE, ADV_C(9), ADV_R(1) }, { P_SAT, ADV_C(10), ADV_R(1) },
    /* row 3: oscillator 3, its volume and the noise, the modulation, the home knobs that set many at once */
    { P_O3_RANGE, ADV_C(0), ADV_R(2) }, { P_O3_WAVE, ADV_C(1), ADV_R(2) }, { P_O3_FREQ, ADV_C(2), ADV_R(2) },
    { P_O3_VOL, ADV_C(3), ADV_R(2) }, { P_NOISE, ADV_C(4), ADV_R(2) },
    { P_MOD_MIX, ADV_C(5), ADV_R(2) }, { P_MOD_PITCH, ADV_C(6), ADV_R(2) }, { P_MOD_FILT, ADV_C(7), ADV_R(2) },
    { P_SETUP, ADV_C(9), ADV_R(2) }, { P_CONTOUR, ADV_C(10), ADV_R(2) },
};
#define N_ADV ((int)(sizeof ADV / sizeof ADV[0]))

static uint8_t ui_slot = 0xFF;          /* the slot this state belongs to */
static int16_t ui_cap[4];               /* what the borrowed knob words are set back to */
static int16_t ui_page[3];              /* words 5..7 as last written / seen */
static int32_t ui_acc[4];               /* step accumulators per knob */
static int8_t  ui_hl[4] = { P_CUTOFF, P_EMPH, P_AMOUNT, P_DECAY };   /* advanced view highlights */
static uint8_t ui_click_age = 255;      /* frames since the last press */
static uint8_t ui_save;                 /* frames left before the words go into the sound (0: nothing to save) */
static int16_t ui_setup_seen = -1;      /* the setup (its character) or preset (its words) in place */
/* home + Shift: a second layer on the same four knobs (the core turns the home words: they are put back) */
static const int8_t HOME_SHIFT[4] = { P_DETUNE, P_DRIVE, P_AMOUNT, P_SAT };
#ifndef MM_RACE_HOOK
#define MM_RACE_HOOK() ((void)0)
#endif
static uint8_t ui_layer;                /* 1 while Shift shows HOME_SHIFT on home */
static int16_t ui_prev[4];              /* home: last frame's knob words (a load moves several at once) */
/* knob movement of one click, without / with Shift: the smallest turn seen. It starts high on purpose: a guess too
   high is corrected by the first single clicks. A smaller turn counts once it is seen twice (one odd turn does not),
   and not in the frames right after Shift changed (a turn the core made with Shift can reach us in the first frame
   without it: counted as a plain click, it made every plain click count as many) nor when the word hit an end (a
   clipped turn). And if RECOVER turns in a row from rest are all 2+ clicks and the same size (single clicks: fast
   flicks differ), the click was measured too small: that size is the click again. */
#define SH_SETTLE 4                     /* frames after a Shift change without measuring */
#define RECOVER   6
static int32_t ui_click[2] = { 2048, 256 };
static int32_t ui_cand[2], ui_big[2];   /* a smaller click seen once; the smallest of the big turns from rest */
static uint8_t ui_nbig[2];              /* big turns from rest in a row */
static uint8_t ui_idle[4] = { 255, 255, 255, 255 };   /* frames since each knob turned */
static uint8_t ui_sh = 0xFF, ui_sh_age; /* Shift as last seen, frames since it changed */
static void measure_click(int k, int32_t d, int sh, int clipped)
{
    int32_t a = d < 0 ? -d : d;
    int rest = ui_idle[k] >= 2;
    ui_idle[k] = 0;
    if (!a || clipped || ui_sh_age < SH_SETTLE) return;
    int32_t c = ui_click[sh];
    if (a < c) {
        int32_t m = ui_cand[sh];
        if (m && a * 8 >= m * 7 && a * 7 <= m * 8) { ui_click[sh] = a > m ? a : m; ui_cand[sh] = 0; }
        else ui_cand[sh] = a;
        ui_nbig[sh] = 0;
    } else if (rest && a >= 2 * c) {
        int32_t m = ui_big[sh];
        if (ui_nbig[sh] && a * 8 >= m * 7 && a * 7 <= m * 8) ui_nbig[sh]++;
        else ui_nbig[sh] = 1;
        if (ui_nbig[sh] == 1 || a < m) ui_big[sh] = a;
        if (ui_nbig[sh] >= RECOVER) { ui_click[sh] = ui_big[sh]; ui_nbig[sh] = 0; ui_cand[sh] = 0; }
    } else if (rest) ui_nbig[sh] = 0;
}
/* whole steps of n clicks in this turn (the rest waits in the knob's accumulator) */
static int steps(int k, int32_t d, int sh, int n)
{
    int32_t per = ui_click[sh] * n;
    int s = 0;
    ui_acc[k] += d;
    while (ui_acc[k] >= per) { s++; ui_acc[k] -= per; }
    while (ui_acc[k] <= -per) { s--; ui_acc[k] += per; }
    return s;
}
static int8_t  ui_click_knob = -1;

static void view_map(int view, volatile int8_t *m)
{
    static const int8_t M[N_VIEWS][4] = {
        { P_SETUP, P_CUTOFF, P_EMPH, P_CONTOUR },                   /* home */
        { 0, 0, 0, 0 },                                             /* advanced: the highlights */
        { P_O1_RANGE, P_O1_WAVE, P_O1_FREQ, P_O1_VOL },
        { P_O2_RANGE, P_O2_WAVE, P_O2_FREQ, P_O2_VOL },
        { P_O3_RANGE, P_O3_WAVE, P_O3_FREQ, P_O3_VOL },
        { P_CUTOFF, P_EMPH, P_AMOUNT, P_FEEDBACK },                 /* filter */
        { P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE },                /* contour */
        { P_DETUNE, P_DRIFT, P_DRIVE, P_SAT },                      /* analog: the chain, in signal order */
        { P_MOD_MIX, P_MOD_PITCH, P_MOD_FILT, P_NOISE },            /* the oscillator view's 4th row: modulation, noise */
    };
    for (int k = 0; k < 4; k++) m[k] = view == V_ADV ? ui_hl[k] : M[view][k];
}

static int32_t clampcap(int32_t v) { return v < CAP_MARGIN ? CAP_MARGIN : (v > 32767 - CAP_MARGIN ? 32767 - CAP_MARGIN : v); }

static void set_view(int v)
{
    if (v == mm_view) return;
    int16_t *w = live_words();
    if (mm_view == V_HOME) {                                         /* borrow the knob words */
        for (int k = 0; k < 4; k++) { P[k] = w[k]; ui_cap[k] = (int16_t)clampcap(w[k]); w[k] = ui_cap[k]; }
    } else if (v == V_HOME) {                                        /* give them back, into the sound too */
        for (int k = 0; k < 4; k++) w[k] = P[k];
        MM_SET_KNOBS(ui_slot, w);
        ui_save = 0;
    }
    for (int k = 0; k < 4; k++) ui_acc[k] = 0;
    view_map(v, mm_map);
    mm_view = (uint8_t)v;
    ui_layer = 0;
}

static void click(int k, int shift)
{
    int v = mm_view;
    switch (k) {
    case 0: set_view(v == V_HOME ? V_ADV : V_HOME); break;
    case 1: {                                                        /* rows 1, 2, 3, noise */
        int row = v == V_NOISE ? 3 : (v >= V_OSC1 && v <= V_OSC3 ? v - V_OSC1 : -1);
        row = row < 0 ? (shift ? 3 : 0) : (row + (shift ? 3 : 1)) % 4;
        set_view(row == 3 ? V_NOISE : V_OSC1 + row);
        break;
    }
    case 2: set_view(v == V_FILTER ? V_CONTOUR : (v == V_CONTOUR ? V_FILTER : (shift ? V_CONTOUR : V_FILTER))); break;
    default: set_view(V_ANALOG); break;
    }
}

/* ---- contour times on the tempo ----
 * A contour time (attack, decay, release) turns freely, but a turn that ends within half a click of a note value at
 * the OP-1 tempo (the metronome's beat clock: the BPM setting or MIDI clock; 120 BPM without one) lands on it, and it
 * stays there SYNC_HOLD more clicks (a light detent); with Shift there are none. While on a note value it follows the
 * tempo (ui_sync: the note value, -1 = free). Session only, like the times. */
static const struct { uint8_t num, den; char t[6]; } DIVS[] = {      /* in beats (a quarter note = 1) */
    { 1, 8, "1/32" }, { 1, 6, "1/16T" }, { 1, 4, "1/16" }, { 1, 3, "1/8T" }, { 1, 2, "1/8" }, { 2, 3, "1/4T" },
    { 1, 1, "1/4" }, { 2, 1, "1/2" }, { 4, 1, "1/1" }, { 8, 1, "2/1" }, { 16, 1, "4/1" } };
#define N_DIVS ((int)(sizeof DIVS / sizeof DIVS[0]))
static int8_t ui_sync[3] = { -1, -1, -1 };                            /* attack, decay, release */
static int32_t ui_contour_seen = -1;                                  /* the contour knob value they belong to */
static inline int time_slot(int id) { return id == P_ATTACK ? 0 : (id == P_DECAY ? 1 : (id == P_RELEASE ? 2 : -1)); }
static int32_t beat_ms(void)
{
    op1_beat_t b; op1_beat_clock(&b);
    if (b.len <= 0) return 500;
    int32_t ms = (int32_t)((int64_t)b.len * 10 / (16384 * 441));      /* 1/16384 samples at 44.1 kHz */
    return ms < 50 ? 50 : (ms > 4000 ? 4000 : ms);
}
static int32_t div_ms(int i) { return beat_ms() * DIVS[i].num / DIVS[i].den; }
/* the knob value whose time (4 ms x 1.0625^index, see time_index) is closest to ms */
static int32_t knob_of_ms(int32_t ms)
{
    if (ms < 4) ms = 4;
    int32_t idx = ((ulog2_q10((uint32_t)ms) - 2048) * 100 + 4478) / 8956;   /* log2(ms / 4) / log2(1.0625), rounded */
    idx = idx < 0 ? 0 : (idx > 114 ? 114 : idx);
    return (idx * 32768 + 114) / 115;
}
static int near_div(int32_t t, int32_t r)                           /* the note value within r of knob value t */
{
    int best = -1;
    for (int i = 0; i < N_DIVS; i++) {
        int32_t e = knob_of_ms(div_ms(i)) - t;
        if (e < 0) e = -e;
        if (e < r) { best = i; r = e; }
    }
    return best;
}
static void sync_follow(void)                                         /* every frame: synced times follow the tempo */
{
    static const int8_t ID[3] = { P_ATTACK, P_DECAY, P_RELEASE };
    if (P[P_CONTOUR] != ui_contour_seen) { ui_contour_seen = P[P_CONTOUR]; for (int j = 0; j < 3; j++) ui_sync[j] = -1; }
    for (int j = 0; j < 3; j++) if (ui_sync[j] >= 0) P[ID[j]] = (int16_t)knob_of_ms(div_ms(ui_sync[j]));
}

/* the starting point of a page word that was never set (0): the value its default stands for */
static int32_t page_start(int id, int32_t w)
{
    if (w > 0) return w;
    return id == P_DETUNE ? 11600 : (id == P_DRIFT ? 8192 : 0);      /* 5 cents, v2's drift, drive 1x */
}

static int edit(int k, int id, int32_t d, int shift)                 /* returns 1 if a word changed */
{
    int32_t v = P[id];
    if (id >= P_DETUNE && id <= P_DRIFT) v = page_start(id, live_words()[5 + id - P_DETUNE]);
    int sh = shift ? 1 : 0;
    switch (PD[id].kind) {
    case K_STEP: v += steps(k, d, sh, STEP_CLICKS); break;
    case K_CENTS:
        if (shift) { v += steps(k, d, 1, 1); break; }                /* fine: a cent per click */
        for (int n = steps(k, d, 0, STEP_CLICKS); n; n += n > 0 ? -1 : 1)      /* semitones */
            v = n > 0 ? (v >= 0 ? v / 100 : -((-v + 99) / 100)) * 100 + 100 : (v > 0 ? (v + 99) / 100 : -(-v / 100)) * 100 - 100;
        break;
    default:
        if (time_slot(id) >= 0) {
            int j = time_slot(id);
            if (shift) { ui_sync[j] = -1; v += steps(k, d, 1, 1) * FINE_UNITS; break; }   /* Shift: free, fine */
            int32_t c = ui_click[0];
            if (ui_sync[j] >= 0) {                                   /* on a note value: held SYNC_HOLD clicks */
                ui_acc[k] += d;
                int32_t a = ui_acc[k] < 0 ? -ui_acc[k] : ui_acc[k], hold = SYNC_HOLD * c + c / 2;
                if (a < hold) break;
                v += ui_acc[k] > 0 ? a - SYNC_HOLD * c : SYNC_HOLD * c - a;
                ui_sync[j] = -1; ui_acc[k] = 0;
            } else v += d;
            int i = near_div(v, c / 2);
            if (i >= 0) { ui_sync[j] = (int8_t)i; v = knob_of_ms(div_ms(i)); ui_acc[k] = 0; }
        } else if (id == P_SETUP && !shift) {                        /* setups and presets: an entry per click */
            int n = steps(k, d, 0, SETUP_CLICKS);
            if (n) v = entry_word(entry_step(entry_of(v), n));
        } else v += shift ? steps(k, d, 1, 1) * (id == P_CUTOFF ? CUT_FINE : FINE_UNITS) : d;
        break;
    }
    P[id] = (int16_t)pclamp(id, v);
    if (id >= P_DETUNE && id <= P_DRIFT) {                           /* stored: words 5..7 (never 0 again: 0 = unset) */
        int16_t *w = live_words();
        int j = id - P_DETUNE;
        w[5 + j] = P[id] > 0 ? P[id] : 1;
        ui_page[j] = w[5 + j];
        return 1;
    }
    return 0;
}

static void move_highlight(int k, int dir)
{
    int at = 0;
    for (int i = 0; i < N_ADV; i++) if (ADV[i].id == ui_hl[k]) at = i;
    for (int n = 0; n < N_ADV; n++) {
        at = (at + dir + N_ADV) % N_ADV;
        int used = 0;
        for (int j = 0; j < 4; j++) if (j != k && ui_hl[j] == ADV[at].id) used = 1;
        if (!used) break;
    }
    ui_hl[k] = ADV[at].id;
    view_map(V_ADV, mm_map);
}

static int turn(int k, int32_t d, int shift)                         /* returns 1 if a word changed */
{
    if (mm_view == V_ADV && shift) {
        for (int n = steps(k, d, 1, HL_CLICKS); n; n += n > 0 ? -1 : 1) move_highlight(k, n > 0 ? 1 : -1);
        return 0;
    }
    int id = mm_map[k];
    return id >= 0 ? edit(k, id, d, shift) : 0;
}


static void ui_home_from_words(const int16_t *w)                     /* a (new) sound: home view, its values */
{
    for (int j = 0; j < 3; j++) ui_page[j] = w[5 + j];
    for (int k = 0; k < 4; k++) { P[k] = w[k]; ui_acc[k] = 0; ui_prev[k] = w[k]; }
    ui_setup_seen = (int16_t)entry_of(w[0]);                          /* its own words: never a setup's or preset's */
    if (!w[5] && !w[6] && !w[7] && preset_of(ui_setup_seen))         /* a fresh sound (the engine just chosen: */
        ui_setup_seen = -1;                                          /* plugin.json defaults) gets its preset's words */
    mm_derived_setup = -1; mm_derived_contour = -1;                  /* its oscillators and contour rebuilt from its */
    for (int j = 0; j < 3; j++) ui_sync[j] = -1;                     /* words: no session edits of another sound */
    mm_view = V_HOME;
    view_map(V_HOME, mm_map);
    ui_layer = 0;
}

static void mm_ui_frame(void)
{
    int slot = MM_SLOT;
    int16_t *w = live_words();
    if (slot != ui_slot) {                                           /* first frame, or another slot */
        if (ui_slot != 0xFF && (mm_view != V_HOME || ui_save)) {     /* give the old slot its home knobs back */
            int16_t *ow = (int16_t *)op1_synth_knobs(ui_slot);
            if (mm_view != V_HOME) for (int k = 0; k < 4; k++) ow[k] = P[k];
            MM_SET_KNOBS(ui_slot, ow);
        }
        ui_slot = (uint8_t)slot; ui_save = 0;
        for (int k = 0; k < 4; k++) op1_eng_page[k] = 0;
        ui_home_from_words(w);
    }
    int sh = MM_SHIFT, dirty = 0;
    if (ui_click_age < 255) ui_click_age++;
    for (int k = 0; k < 4; k++) if (ui_idle[k] < 255) ui_idle[k]++;
    if (sh != ui_sh) {                       /* Shift changed: what waits in the accumulators was in the other size */
        ui_sh = (uint8_t)sh; ui_sh_age = 0;
        for (int k = 0; k < 4; k++) ui_acc[k] = 0;
    } else if (ui_sh_age < 255) ui_sh_age++;
    for (int k = 0; k < 4; k++)
        if (op1_eng_page[k]) { op1_eng_page[k] = 0; click(k, sh); ui_click_age = 0; ui_click_knob = (int8_t)k; }

    /* words 5..7 changed by someone else: a turn that raced a press (the core sent it there, at 1/4), or a sound
     * that was loaded */
    int load = 0;
    for (int j = 0; j < 3; j++)
        if (w[5 + j] != ui_page[j]) {
            if (ui_click_age < 8) {
                int k = j == 2 && ui_click_knob == 3 ? 3 : j;        /* white and red share word 7 */
                int32_t d = (w[5 + j] - ui_page[j]) * 4;
                w[5 + j] = ui_page[j];
                dirty |= turn(k, d, sh) | 1;
            } else load = 1;
        }
    {
        int jumps = 0;
        for (int k = 0; k < 4; k++) {
            int32_t d = w[k] - (mm_view != V_HOME ? ui_cap[k] : ui_prev[k]);
            if (d > LOAD_JUMP || d < -LOAD_JUMP) jumps++;
        }
        if (jumps >= 2) load = 1;
    }
    if (load) {                                                      /* another sound: its words are its own */
        ui_home_from_words(w);
        sync_derived(w[0], w[3]);
        return;
    }
    if (mm_view == V_HOME) {
        /* The core turns the words on the KeyHandler thread while this runs on the Gfx thread: each word is read once
           here, and with Shift held the home words are never taken from w (a turn that lands after this loop stays in
           w and is next frame's Shift click). Before 2026-11-08 a blue click landing there became the new entry word,
           which could step to another preset and load its words: DET back, AMT changed (a user report). */
        if (sh) for (int k = 0; k < 4; k++) {                        /* the Shift layer: a Shift click = a click */
            const int32_t cur = w[k], d = cur - ui_prev[k];
            if (!d) continue;
            measure_click(k, d, 1, cur <= 0 || cur >= 32767);
            w[k] = ui_prev[k];
            int n = steps(k, d, 1, 1);
            if (n) dirty |= edit(k, HOME_SHIFT[k], n * ui_click[0], 0) | 1;
        }
        int32_t d0 = w[0] - ui_prev[0];
        if (!sh && d0 && d0 <= LOAD_JUMP && d0 >= -LOAD_JUMP) {     /* blue: the next number per click (wraps) */
            measure_click(0, d0, 0, w[0] <= 0 || w[0] >= 32767);
            w[0] = (int16_t)entry_word(entry_step(entry_of(ui_prev[0]), steps(0, d0, 0, SETUP_CLICKS)));
            dirty = 1;
        }
        if (!sh) for (int k = 1; k < 4; k++)                         /* the core's knobs: turning (for the view) */
            if (w[k] != ui_prev[k]) ui_idle[k] = 0;
        MM_RACE_HOOK();                                              /* host test: a turn landing here */
        for (int k = 0; k < 4; k++) P[k] = ui_prev[k] = sh ? ui_prev[k] : w[k];
        mm_home_hold = (uint8_t)sh;                                  /* the audio thread: P's home words (moai.h) */
        if (sh != ui_layer) {
            ui_layer = (uint8_t)sh;
            for (int k = 0; k < 4; k++) { mm_map[k] = sh ? HOME_SHIFT[k] : (int8_t)k; ui_acc[k] = 0; }
        }
    }
    else
        for (int k = 0; k < 4; k++) {
            int32_t d = w[k] - ui_cap[k];
            if (d) { measure_click(k, d, sh, w[k] <= 0 || w[k] >= 32767); turn(k, d, sh); w[k] = ui_cap[k]; dirty = 1; }
        }
    int ent = entry_of(mm_view == V_HOME ? ui_prev[0] : P[P_SETUP]);
    if (ent != ui_setup_seen) {      /* turned to another setup or preset: its words, so a number always sounds the same */
        const preset_t *pr = preset_of(ent);
        const setup_t *st = &SETUPS[ent & (N_SETUPS - 1)];
        const uint8_t hv[5] = { pr ? pr->cut : st->cut, pr ? pr->emp : st->emp, pr ? pr->con : st->con,
                                pr ? pr->det : st->det, pr ? pr->drv : st->drv };
        for (int k = 1; k < 4; k++) {                                /* cutoff, emphasis, contour */
            P[k] = (int16_t)pct(hv[k - 1]);
            if (mm_view == V_HOME) w[k] = ui_prev[k] = P[k];         /* (on another view: the words are borrowed) */
        }
        for (int j = 0; j < 3; j++) {                                /* detune, drive; drift back to its default */
            int32_t t = j < 2 ? pct(hv[3 + j]) : DRIFT_DEFAULT;
            w[5 + j] = (int16_t)(t < 1 ? 1 : t); ui_page[j] = w[5 + j];   /* (0 = never set) */
        }
        dirty = 1;
        ui_setup_seen = (int16_t)ent;
    }
    for (int j = 0; j < 3; j++) P[P_DETUNE + j] = w[5 + j];
    /* into the sound once the knobs rest (the patch setter marks the sound changed: not on every frame) */
    if (dirty) ui_save = SAVE_WAIT;
    else if (ui_save && !--ui_save) {               /* the sound's words: on another view, home's (not the borrowed) */
        int16_t s[8];
        for (int i = 0; i < 8; i++) s[i] = i < 4 ? (mm_view != V_HOME ? P[i] : ui_prev[i]) : w[i];   /* not a click in flight */
        MM_SET_KNOBS(slot, s);
    }
    sync_derived(mm_view == V_HOME ? ui_prev[0] : P[P_SETUP], mm_view == V_HOME ? ui_prev[3] : P[P_CONTOUR]);
    mm_ui_beat++;
    sync_follow();
}
