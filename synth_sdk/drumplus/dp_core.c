/* drum+ core part (docs/43): what stays in the firmware when drum+ is a plugin file (plugins/drumplus, kind "drum").
 *  - DrumSampler vtable slots render / note_on / note_off (SLOTS in make_firmware.py): the plugin's, else the stock
 *    functions (a stock drum kit, bit-exact);
 *  - the "op1r" key of drum patches is kept through load / save also without the plugin (a kit's drum+ data comes
 *    back with the plugin), playmode repair (error 12) and the JSON repair (errors 22 / 25) before the stock check;
 *  - drum type browser start row and job (the plugin's for its drum_types row id 2, else stock);
 *  - the boot calls that let the plugin build its page screens (0x10 / 0x17) and the key / knob / sync calls of
 *    dispatcher.c, forwarded to the plugin if one is loaded. */
#include "../op1_synth.h"

/* drum engine plugins (docs/43): drum+ (row 2) is op1_drum, the default for kits no plugin claims (it plays those with
 * the stock code itself, so a drum+-only firmware routes exactly as before); further drum plugins (e.g. perc, row 3)
 * each claim their kits through is_plus. The loader fills a slot before it bumps the count (render may run). */
const op1_drum_t *op1_drum;                                   /* drum+ (set by the plugin loader, plugload.c) */
const op1_drum_t *op1_drums[OP1_DRUMS_MAX];
int op1_drum_rows[OP1_DRUMS_MAX];                             /* drum_types id of each */
volatile int op1_drum_n;
#ifndef DRUM_SLOT                                               /* (host test: harness/drumroute_test.c) */
#define DRUM_SLOT   (*(const volatile int32_t *)0x01AAAC10)   /* the playing drum slot */
#endif
#ifndef VOICE_OBJ
#define VOICE_OBJ   ((void *const *)0x01AAAD30)               /* the 12 drum voice objects */
#endif
#ifndef UI_SLOT_CUR
#define UI_SLOT_CUR (*(volatile int *)0xFF8016D8 & 7)         /* the drum slot the pages show */
#endif

static const op1_drum_t *drum_owner(int slot)                 /* the plugin that claims the slot's kit, or 0 */
{
    int n = op1_drum_n;
    for (int i = 0; i < n; i++) { const op1_drum_t *d = op1_drums[i]; if (d && d->is_plus(slot & 7)) return d; }
    return 0;
}
static const op1_drum_t *drum_for_slot(int slot) { const op1_drum_t *d = drum_owner(slot); return d ? d : op1_drum; }
static const op1_drum_t *drum_by_row(int id)
{
    int n = op1_drum_n;
    for (int i = 0; i < n; i++) if (op1_drum_rows[i] == id) return op1_drums[i];
    return 0;
}
/* a voice stays with the plugin that started it (a slot change mid-sound must not hand it to another engine) */
static const op1_drum_t *voice_owner[12];
static uint8_t voice_set[12];                                 /* voice_owner valid (0 there: stock, without drum+) */
static int voice_of(void *self) { for (int i = 0; i < 12; i++) if (VOICE_OBJ[i] == self) return i; return -1; }
static const op1_drum_t *drum_for_voice(void *self)
{
    int v = voice_of(self);
    if (v >= 0 && voice_set[v]) return voice_owner[v];
    return drum_for_slot(DRUM_SLOT);
}

#define DP_SLOTS      8
#define DRUM_PATCHES  0x01DDD9A8                              /* 8 drum patch objects x 0x38 (JSON at +4) */
#define SYNTH_PATCHES 0x01DDD7E8                              /* 8 synth patch objects x 0x38 (JSON at +4) */
void sp_slot_loaded(int slot, void *op1r);                    /* samplerplus/sp_core.c */
#ifndef STOCK_RENDER
#define STOCK_RENDER   ((void (*)(void *, int16_t *, const void *))0x019789EC)
#endif
#ifndef STOCK_NOTE_ON
#define STOCK_NOTE_ON  ((void (*)(void *, const void *))0x01978926)
#endif
#ifndef STOCK_NOTE_OFF
#define STOCK_NOTE_OFF ((void (*)(void *, const void *))0x01978898)
#endif

/* ---- voice slots (audio interrupt) ---- */
void dp_fw_render(void *self, int16_t *out, const void *pad)
{
    const op1_drum_t *d = drum_for_voice(self);
    uint32_t t0 = op1_cycles();
    if (d) d->render(self, out, pad); else STOCK_RENDER(self, out, pad);
    sm_cy[SMC_DRUM] += op1_cycles() - t0;
}
void dp_fw_note_on(void *self, const void *pad)
{
    const op1_drum_t *d = drum_for_slot(DRUM_SLOT);
    int v = voice_of(self);
    if (v >= 0) { voice_owner[v] = d; voice_set[v] = 1; }
    if (d) d->note_on(self, pad); else STOCK_NOTE_ON(self, pad);
}
void dp_fw_note_off(void *self, const void *pad)
{
    const op1_drum_t *d = drum_for_voice(self);
    if (d) d->note_off(self, pad); else STOCK_NOTE_OFF(self, pad);
}

/* ---- patch JSON: keep "op1r" (parse_patch_json rebuilds the patch from its known keys and assigns the result over
 * the patch at 0x01A044A8: operator=(patch, fresh)); load_drum_patch's parser call (0x01A04BC0) tells the slot ---- */
#define JV_KEY    ((void *(*)(void *, const char *))0x019BFEB4)
#define JV_IDX    ((void *(*)(void *, unsigned))0x019BFAF8)
#define JV_ASSIGN ((void *(*)(void *, const void *))0x019BDDC0)
#define JV_SIZE   ((unsigned (*)(const void *))0x019BE8D8)
#ifndef JV_CGET
#define JV_CGET   ((const void *(*)(const void *, const char *))0x019BEB70)
#endif
#define JV_FROM_INT ((void *(*)(void *, int))0x019BD890)
#define JV_DTOR   ((void (*)(void *, int))0x019BDD18)
#define PARSE_PATCH ((int (*)(void *, int))0x01A037A8)
enum { JV_INT = 1, JV_UINT = 2, JV_ARRAY = 6, JV_OBJECT = 7 };
static uint8_t jv_type(const void *v) { return ((const uint8_t *)v)[8]; }
static int dp_loading_slot = -1;

void *dp_keep_op1r(void *patch, void *fresh)
{
    void *old = JV_KEY(patch, "op1r");                 /* (adds a null member if absent: patch is overwritten next) */
    int keep = jv_type(old) == JV_OBJECT;
    if (keep) JV_ASSIGN(JV_KEY(fresh, "op1r"), old);
    if (dp_loading_slot >= 0)
        for (int i = 0; i < op1_drum_n; i++) op1_drums[i]->slot_loaded(dp_loading_slot, keep ? JV_KEY(fresh, "op1r") : 0);
    uint32_t off = (uint32_t)patch - (SYNTH_PATCHES + 4);              /* a synth slot's patch (sampler+, docs/46) */
    if (off < 8 * 0x38 && off % 0x38 == 0) sp_slot_loaded((int)(off / 0x38), keep ? JV_KEY(fresh, "op1r") : 0);
    return JV_ASSIGN(patch, fresh);
}

/* kits saved by drum+ 2.6 .. 2.7.1 hold gate as playmode 0; the stock check (load_drum_patch, Ranges group 8
 * index 3: 4096..20480) rejects it: "Invalid drum patch ... error 12" at boot. Words below 4096 become 4096 = gate
 * (>> 13 == 0), only where "playmode" is an array (const lookup first: no member is added to other patches). */
#define DP_GATE 4096
static void dp_repair_playmode(void *json)
{
    const void *c = JV_CGET(json, "playmode");
    if (!c || jv_type(c) != JV_ARRAY) return;
    void *pm = JV_KEY(json, "playmode");
    unsigned n = JV_SIZE(pm);
    uint32_t tmp[4];
    for (unsigned i = 0; i < n && i < 64; i++) {
        void *v = JV_IDX(pm, i);
        if ((jv_type(v) == JV_INT || jv_type(v) == JV_UINT) && *(const int32_t *)v < DP_GATE) {
            JV_FROM_INT(tmp, DP_GATE);
            JV_ASSIGN(v, tmp);
            JV_DTOR(tmp, 2);
        }
    }
}
int dp_load_parse(void *json, int mode)
{
    dp_repair_playmode(json);                          /* before the stock range check (error 12) */
    extern void patch_repair(void *json); patch_repair(json);   /* unknown FX / LFO (error 22 / 25) */
    uint32_t off = (uint32_t)json - 4 - DRUM_PATCHES;
    dp_loading_slot = off % 0x38 == 0 && off / 0x38 < DP_SLOTS ? (int)(off / 0x38) : -1;
    int r = PARSE_PATCH(json, mode);
    dp_loading_slot = -1;
    return r;
}

/* ---- drum type browser (Shift + T1) ----
 * Start row (CALL patch_01a01ffe at 0x01977900 in DrumController): the stock id comes from drum_type_id_by_name
 * 0x01A01EF4: drum_types row id, or for "type": "drum" minus the index of the kit with the same name in the kit table
 * 0x01DE20FC, which holds factory AND user kits. The browser list (patch_01A025E8) has only factory kits: a user kit's
 * id is not found and the browser would start on row 0 (DBOX) and load a factory kit on the next turn. So: drum+ slot
 * -> the DRUM+ row (id 2, plugin); an id the list does not have -> 0 (DRUM, nothing loaded until a turn). */
#define DP_TYPE_ID   2
#define DP_DRUM_ID   ((int (*)(int, uint8_t *))0x01A01FFE)
#define DP_TYPE_LIST ((uint8_t *(*)(void))0x01A025E8)
#ifndef TYPE_JOB
#define TYPE_JOB     ((void (*)(void *))0x0193F86C)
#endif
static int dp_list_has(int id)
{
    uint8_t *l = DP_TYPE_LIST();                              /* rows 0x30 (+0x1c id, +0x24/+0x28 items 0x20) */
    for (uint8_t *r = *(uint8_t **)(l + 4); r < *(uint8_t **)(l + 8); r += 0x30) {
        if (*(int *)(r + 0x1c) == id) return 1;
        for (uint8_t *it = *(uint8_t **)(r + 0x24); it < *(uint8_t **)(r + 0x28); it += 0x20)
            if (*(int *)(it + 0x1c) == id) return 1;
    }
    return 0;
}
int dp_browser_id(int slot, uint8_t *found)
{
    int id = DP_DRUM_ID(slot, found);
    const op1_drum_t *own = drum_owner(slot);
    for (int i = 0; own && i < op1_drum_n; i++) if (op1_drums[i] == own) return op1_drum_rows[i];
    if (!dp_list_has(id)) { id = 0; *found = 0; }
    return id;
}
void pj_begin(void), pj_end(void);                                /* patchjob.c: a patch job runs (docs/66) */
void dpu_sync_auto(void);                                         /* dispatcher.c: the drum pages for the shown slot */
/* job +4 slot, +8 selection (<= 0 a kit, 1 dbox, >= 2 a plugin's row). A plugin row goes to that plugin (it claims the
 * kit; the others are told so they let go); any other selection to the slot's plugin (it lets go and runs the stock
 * job), else to drum+ as before; a plugin row whose plugin is not loaded does nothing (the stock job has no such row). */
void dp_type_job(void *job)
{
    pj_begin();
    int slot = *(int32_t *)((uint8_t *)job + 4), sel = *(int32_t *)((uint8_t *)job + 8);
    const op1_drum_t *to = sel >= 2 ? drum_by_row(sel) : 0;
    if (to) {
        to->type_job(job);
        if ((uint32_t)slot < DP_SLOTS) {
            const void *op = JV_CGET((uint8_t *)(DRUM_PATCHES + slot * 0x38) + 4, "op1r");
            for (int i = 0; i < op1_drum_n; i++)
                if (op1_drums[i] != to) op1_drums[i]->slot_loaded(slot, (void *)op);
        }
        dpu_sync_auto();                                      /* the pages again, now that only the new owner claims it */
    } else if (sel < 2) {
        const op1_drum_t *d = (uint32_t)slot < DP_SLOTS ? drum_for_slot(slot) : op1_drum;
        if (d) d->type_job(job); else TYPE_JOB(job);
    }
    pj_end();
}

/* ---- the drum+ pages: the plugin's screens (built at boot from dispatcher.c), the forwarded calls ---- */
void dpu_ui_build(void) { for (int i = 0; i < op1_drum_n; i++) op1_drums[i]->ui_build(); }
void dpu_ui_load(void) { for (int i = 0; i < op1_drum_n; i++) op1_drums[i]->ui_load(); }
static const op1_drum_t *ui_drum(void) { const op1_drum_t *d = drum_for_slot(UI_SLOT_CUR); return d ? d : op1_drum_n ? op1_drums[0] : 0; }
void *dpu_screen_for(int id) { const op1_drum_t *d = ui_drum(); return d ? d->screen_for(id) : 0; }
/* only the shown slot's plugin sets the page table (its page or the stock ones): one writer per key event, as with drum+
 * alone. (Letting every plugin restore the stock pages first flipped the table on every key press while the GUI thread
 * may draw from it.) */
void dpu_sync(int editor_open_on_10) { const op1_drum_t *d = ui_drum(); if (d) d->sync(editor_open_on_10); }
int dpu_knob(int knob, int delta) { const op1_drum_t *d = ui_drum(); return d ? d->knob(knob, delta) : 0; }
int dpu_key(int code, int pressed) { const op1_drum_t *d = ui_drum(); return d ? d->key(code, pressed) : 0; }
