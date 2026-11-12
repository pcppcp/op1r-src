/* drum+ plugin (plugins/drumplus, kind "drum", docs/31 / docs/43): the core (dp_core.c) forwards the DrumSampler
 * vtable slots render / note_on / note_off here. Pads without drum+ extras (and every pad of a stock kit) call the
 * original functions, unchanged; drum+ pads run dp_plus_* with per-voice state indexed like the 12 stock voices. */
#include "../op1_synth.h"
#include "dp_plus.h"

#define DP_SLOTS   8                                          /* drum_slots rows */
#define DP_PADS    24
#define DP_VOICES  12
#define VOICE_OBJ  ((void *const *)0x01AAAD30)                /* voice objects [12] (0xFFB00000 + v * 0x14) */
#define VOICE_PAD  ((const int32_t *)0xFF801334)              /* pad of voice v (note_on_drum) */
#define VOICE_VEL  ((const int16_t *)0xFF8012EC)              /* velocity << 8 of voice v (int16) */
#define DRUM_SLOT  (*(const volatile int32_t *)0x01AAAC10)    /* current drum slot */
#define BUF_INC    ((int32_t *)0xFF801950)                    /* synth_phase_inc_bus [128] (refilled per voice) */
#define BUF_VEL    ((int16_t *)0xFF801D50)                    /* synth_env_buf [128] (velocity << 8, per voice) */
#define FIR        ((const int16_t *)0xFF8021D2)              /* interpolation table centre */
#define BEAT_LEN   (*(const volatile int32_t *)0x01AADAD8)    /* metronome beat length, 0x4000 per sample */

typedef void (*render_fn)(void *, int16_t *, const dp_pad *);
typedef void (*pad_fn)(void *, const dp_pad *);
#define STOCK_RENDER   ((render_fn)0x019789EC)
#define STOCK_NOTE_ON  ((pad_fn)0x01978926)
#define STOCK_NOTE_OFF ((pad_fn)0x01978898)

dp_extra dp_x[DP_SLOTS][DP_PADS];                             /* filled by the drum patch loader (op1r key) */
dp_pre   dp_c[DP_SLOTS][DP_PADS];
uint8_t  dp_isplus[DP_SLOTS];                                 /* slot has an "op1r" object (drum+ pages) */
static dp_xvoice dp_xs[DP_VOICES];

static int voice_index(void *self)
{
    for (int i = 0; i < DP_VOICES; i++) if (VOICE_OBJ[i] == self) return i;
    return -1;
}
static dp_voice *model(void *self) { return (dp_voice *)((uint8_t *)self + 8); }   /* pos, state, blocks */
static int slot_ok(void) { return (uint32_t)DRUM_SLOT < DP_SLOTS; }
static int pad_of(int v) { int p = VOICE_PAD[v]; return (uint32_t)p < DP_PADS ? p : 0; }
static uint32_t samples_per_beat(void) { int32_t l = BEAT_LEN >> 14; return l > 0 ? (uint32_t)l : 22050; }

static void dpx_note_on(void *self, const dp_pad *pad)
{
    int v = voice_index(self);
    if (v < 0 || !slot_ok()) { STOCK_NOTE_ON(self, pad); return; }
    int slot = DRUM_SLOT, p = pad_of(v);
    dp_voice *vp[DP_VOICES]; uint8_t vpad[DP_VOICES];
    for (int i = 0; i < DP_VOICES; i++) { vp[i] = model(VOICE_OBJ[i]); vpad[i] = (uint8_t)pad_of(i); }
    /* (voice v itself may be stolen and get marked: its note-on below clears the fade) */
    dp_plus_choke(vp, dp_xs, vpad, DP_VOICES, p, dp_x[slot]);
    if (!dp_c[slot][p].active) {
        STOCK_NOTE_ON(self, pad);
        dp_xs[v].fading = 0; dp_xs[v].hits_left = 0;
        return;
    }
    dp_plus_note_on(model(self), &dp_xs[v], pad, &dp_x[slot][p], &dp_c[slot][p], VOICE_VEL[v] >> 8,
                    samples_per_beat());
}

static void dpx_note_off(void *self, const dp_pad *pad)
{
    int v = voice_index(self);
    if (v < 0 || !slot_ok() || !dp_c[DRUM_SLOT][pad_of(v)].active) { STOCK_NOTE_OFF(self, pad); return; }
    int slot = DRUM_SLOT, p = pad_of(v);
    dp_plus_note_off(model(self), &dp_xs[v], pad, &dp_x[slot][p], &dp_c[slot][p]);
}

static void dpx_render(void *self, int16_t *out, const dp_pad *pad)
{
    int v = voice_index(self);
    if (v < 0 || !slot_ok()) { STOCK_RENDER(self, out, pad); return; }
    int slot = DRUM_SLOT, p = pad_of(v);
    if (!dp_c[slot][p].active && !dp_xs[v].fading) { STOCK_RENDER(self, out, pad); return; }
    dp_extra *x = &dp_x[slot][p]; const dp_pre *c = &dp_c[slot][p];
    if (dp_plus_fast_pre(model(self), &dp_xs[v], x, c, BUF_INC, BUF_VEL)) {    /* stock render on our buffers */
        STOCK_RENDER(self, out, pad);
        dp_plus_fast_post(model(self), &dp_xs[v], x, c, out);
        return;
    }
    const int16_t *data = **(const int16_t *const *const *)((uint8_t *)self + 4);
    dp_plus_render(model(self), &dp_xs[v], out, pad, &dp_x[slot][p], &dp_c[slot][p], BUF_INC, BUF_VEL, data, FIR,
                   samples_per_beat());
}

/* ---- persistence: "op1r": {"engine": "drum+", "pads": [[f0, f1, ...], ... 24]} in the drum patch JSON ----
 * parse_patch_json (0x01A037A8) rebuilds the patch from its known keys and assigns the result over the patch
 * (0x01A044A8: operator=(patch, fresh)): dp_keep_op1r copies "op1r" across first, so it survives load and is saved
 * again by the stock writer. load_drum_patch's call of the parser (0x01A04BC0) tells which drum slot is loading;
 * then the pads are read into dp_x / dp_c (values clamped; missing key or pad = all off). */
#define JV_KEY    ((void *(*)(void *, const char *))0x019BFEB4)       /* Value::operator[](const char *) */
#define JV_IDX    ((void *(*)(void *, unsigned))0x019BFAF8)           /* Value::operator[](ArrayIndex) */
#define JV_ASSIGN ((void *(*)(void *, const void *))0x019BDDC0)       /* Value::operator= */
#define JV_SIZE   ((unsigned (*)(const void *))0x019BE8D8)            /* Value::size */
#define JV_FROM_INT ((void *(*)(void *, int))0x019BD890)               /* Value(int) */
#define DRUM_PATCHES 0x01DDD9A8                                       /* 8 drum patch objects x 0x38 (JSON at +4) */
enum { JV_NULL = 0, JV_INT = 1, JV_UINT = 2, JV_ARRAY = 6, JV_OBJECT = 7 };
static uint8_t jv_type(const void *v) { return ((const uint8_t *)v)[8]; }
static int32_t jv_int(const void *v)                   /* ints only, never asserts (as asInt, 0x019BE0C0) */
{
    int32_t w = *(const int32_t *)v;                   /* 32-bit Int / UInt in the 8-byte union (high word: junk) */
    return jv_type(v) == JV_INT || (jv_type(v) == JV_UINT && w >= 0) ? w : 0;
}


/* "op1r": {"engine": ...}: drum+'s kit if the engine is "drum+" or absent (kits from before); another drum plugin's
 * engine (e.g. "perc") is not ours. Const lookup: no member is added. */
#define JV_CGET ((const void *(*)(const void *, const char *))0x019BEB70)   /* Value::get(key) or 0, no insert */
static int jv_is_str(void *v, const char *s);
static int dp_engine_ours(void *op1r)
{
    const void *e = JV_CGET(op1r, "engine");
    return !e || jv_type(e) == JV_NULL || jv_is_str((void *)e, "drum+");
}
static void dpx_slot_loaded(int slot, void *op1r)    /* core: a drum patch was loaded (op1r object or 0) */
{
    if ((unsigned)slot >= DP_SLOTS) return;
    dp_isplus[slot] = (uint8_t)(op1r && jv_type(op1r) == JV_OBJECT && dp_engine_ours(op1r));
    for (int p = 0; p < DP_PADS; p++)
        for (int i = 0; i < DP_NFIELDS; i++) dp_extra_set(&dp_x[slot][p], i, 0);
    if (dp_isplus[slot]) {
        void *pads = JV_KEY(op1r, "pads");
        if (jv_type(pads) == JV_ARRAY) {
            unsigned n = JV_SIZE(pads);
            for (unsigned p = 0; p < n && p < DP_PADS; p++) {
                void *a = JV_IDX(pads, p);
                if (jv_type(a) != JV_ARRAY) continue;
                unsigned m = JV_SIZE(a);
                for (unsigned i = 0; i < m && i < DP_NFIELDS; i++) dp_extra_set(&dp_x[slot][p], (int)i, jv_int(JV_IDX(a, i)));
            }
        }
    }
    for (int p = 0; p < DP_PADS; p++) dp_plus_prepare(&dp_x[slot][p], &dp_c[slot][p]);
}


/* ---- drum type browser (Shift + T1): the "drum+" row of drum_types (id DP_TYPE_ID, make_firmware.py) ----
 * The stock type job 0x0193F86C (job +4 slot, +8 selection: <= 0 kit -n, > 0 drum_types id) would strip the kit's
 * regions for a type row. dp_type_job (job vtable slot) handles the drum+ row itself: a sampled kit ("type":
 * "drum") keeps everything and gets "op1r": {"engine": "drum+"}, then UI refresh and slot save as the stock job.
 * Any other selection first drops "op1r" (a new kit / dbox is a stock drum again; the job's reload clears dp_x). */
#define DP_TYPE_ID   2

#define TYPE_JOB     ((void (*)(void *))0x0193F86C)
#define SAVE_SLOT    ((void (*)(void *))0x01940574)              /* job: +4 byte drum, +8 slot: UPDATE ..._slots */
#define UI_SLOT      ((void (*)(int))0x019C2BA0)
#define UI_SIGNAL    ((void (*)(void))0x019C2F04)                /* KeyHandler::signal */
#define JV_FROM_STR  ((void *(*)(void *, const char *))0x019BDC4C) /* Value(const char *) */
#define JV_DTOR      ((void (*)(void *, int))0x019BDD18)
#define JV_REMOVE    ((void (*)(void *, void *, const char *))0x019BEE88)   /* removeMember(key) -> out */
enum { JV_STRING = 4 };

static int jv_is_str(void *v, const char *s)
{
    if (jv_type(v) != JV_STRING) return 0;
    const char *p = *(const char *const *)v;
    if (!p) return 0;
    while (*s && *p == *s) p++, s++;
    return *p == *s;
}

void dpu_sync_auto(void);                                        /* core export (dispatcher.c): screens for 0x10 / 0x17 */

/* op1r.pads[pad] = the pad's fields (UI edits); the caller marks the patch changed (stock saver writes it) */
void dp_fw_store_pad(int slot, int pad)
{
    if ((unsigned)slot >= DP_SLOTS || (unsigned)pad >= DP_PADS) return;
    void *json = (uint8_t *)(DRUM_PATCHES + slot * 0x38) + 4;
    void *op = JV_KEY(json, "op1r");
    if (jv_type(op) != JV_OBJECT) return;                         /* not a drum+ slot */
    void *pads = JV_KEY(op, "pads");
    if (jv_type(pads) != JV_ARRAY && jv_type(pads) != JV_NULL) return;
    void *a = JV_IDX(pads, (unsigned)pad);                        /* (null -> array; grows with null entries) */
    if (jv_type(a) != JV_ARRAY && jv_type(a) != JV_NULL) return;
    uint32_t tmp[4];
    for (int i = 0; i < DP_NFIELDS; i++) {
        JV_FROM_INT(tmp, dp_extra_get(&dp_x[slot][pad], i));
        JV_ASSIGN(JV_IDX(a, (unsigned)i), tmp);
        JV_DTOR(tmp, 2);
    }
}

static void dpx_type_job(void *job)
{
    int slot = *(int32_t *)((uint8_t *)job + 4), sel = *(int32_t *)((uint8_t *)job + 8);
    if ((uint32_t)slot >= DP_SLOTS) { TYPE_JOB(job); return; }
    void *json = (uint8_t *)(DRUM_PATCHES + slot * 0x38) + 4;
    uint32_t tmp[4];                                             /* a Json::Value (16 bytes) */
    if (sel != DP_TYPE_ID) {
        JV_REMOVE(json, tmp, "op1r"); JV_DTOR(tmp, 2);
        dp_isplus[slot] = 0;
        dpu_sync_auto();
        TYPE_JOB(job);
        return;
    }
    if (!jv_is_str(JV_KEY(json, "type"), "drum")) return;     /* dbox: no sample regions to keep */
    void *op = JV_KEY(json, "op1r");                              /* null -> object on the first key */
    JV_FROM_STR(tmp, "drum+");                                    /* also over another plugin's engine (e.g. perc) */
    JV_ASSIGN(JV_KEY(op, "engine"), tmp);
    JV_DTOR(tmp, 2);
    dp_isplus[slot] = 1;
    dpu_sync_auto();
    UI_SLOT(slot);
    UI_SIGNAL();
    struct { uint32_t vt; uint8_t drum, pad[3]; int32_t slot; } save = { 0, 1, { 0, 0, 0 }, slot };
    SAVE_SLOT(&save);
}

/* ---- plugin interface (op1_plugin.h op1_drum_t) ---- */
static int dpx_is_plus(int slot) { return (unsigned)slot < DP_SLOTS && dp_isplus[slot]; }
void dpu_ui_build(void), dpu_ui_load(void);                       /* dp_ui.c */
void *dpu_screen_for(int id);
void dpu_sync(int editor_open_on_10);
int dpu_knob(int knob, int delta), dpu_key(int code, int pressed);
const op1_drum_t DRUMPLUS = {
    (void (*)(void *, int16_t *, const void *))dpx_render, (void (*)(void *, const void *))dpx_note_on,
    (void (*)(void *, const void *))dpx_note_off, dpx_slot_loaded, dpx_is_plus, dpx_type_job,
    dpu_ui_build, dpu_ui_load, dpu_screen_for, dpu_sync, dpu_knob, dpu_key };
