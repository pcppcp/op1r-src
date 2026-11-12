/* drum engine routing (drumplus/dp_core.c) with fake plugins, host only.
 *   cc -DOP1_HOST -Wno-int-to-pointer-cast -Wno-void-pointer-to-int-cast -I. -Iharness harness/drumroute_test.c \
 *      -o harness/build/drumroute_test && harness/build/drumroute_test       (from synth_sdk)
 * 1. drum+ alone: every voice, page, knob and type-job call goes where the one-plugin core sent it.
 * 2. drum+ and a second plugin (perc, row 3): its kits' voices, pages and keys go to it, a voice keeps its plugin
 *    across a slot change, the type job matrix (stock -> perc -> drum+ -> stock, perc row with perc absent), only the
 *    shown slot's plugin syncs the pages (one writer per key event). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int32_t h_drum_slot, h_ui_slot;
static void *h_voices[12];
static char h_log[4096];
static void logs(const char *s) { strcat(h_log, s); strcat(h_log, " "); }
static void stock_render(void *s, int16_t *o, const void *p) { (void)s; (void)o; (void)p; logs("stock.render"); }
static void stock_on(void *s, const void *p) { (void)s; (void)p; logs("stock.on"); }
static void stock_off(void *s, const void *p) { (void)s; (void)p; logs("stock.off"); }
static void stock_job(void *j) { (void)j; logs("stock.job"); }
static const char *h_op1r_engine[8];                       /* per slot: the engine named in its op1r, or 0 */
static const void *h_cget(const void *json, const char *k) { (void)k; int s = (int)(((uintptr_t)json - 4 - 0x01DDD9A8) / 0x38); return h_op1r_engine[s & 7] ? (const void *)h_op1r_engine[s & 7] : 0; }
#define DRUM_SLOT      h_drum_slot
#define VOICE_OBJ      ((void *const *)h_voices)
#define UI_SLOT_CUR    (h_ui_slot & 7)
#define STOCK_RENDER   stock_render
#define STOCK_NOTE_ON  stock_on
#define STOCK_NOTE_OFF stock_off
#define TYPE_JOB       stock_job
#define JV_CGET        h_cget
void sp_slot_loaded(int slot, void *op1r) { (void)slot; (void)op1r; }
void patch_repair(void *json) { (void)json; }
void pj_begin(void) {} void pj_end(void) {}
void dpu_sync(int editor_open_on_10);
void dpu_sync_auto(void) { logs("auto:"); dpu_sync(0); }
#include "../drumplus/dp_core.c"
volatile uint32_t sm_cy[SMC_N];

/* fake plugins: drum+ claims slots whose engine is "drum+"; perc those with "perc" */
typedef struct { const char *name; uint8_t claims[8]; } fake_t;
static fake_t F[2] = { { "drum+", {0} }, { "perc", {0} } };
#define FAKE(n, i) \
static void n##_render(void *s, int16_t *o, const void *p) { (void)s; (void)o; (void)p; logs(#n ".render"); } \
static void n##_on(void *s, const void *p) { (void)s; (void)p; logs(#n ".on"); } \
static void n##_off(void *s, const void *p) { (void)s; (void)p; logs(#n ".off"); } \
static void n##_loaded(int slot, void *op) { F[i].claims[slot] = op && !strcmp((const char *)op, F[i].name); logs(#n ".loaded"); } \
static int n##_is(int slot) { return F[i].claims[slot & 7]; } \
static void n##_job(void *j) { int slot = *(int32_t *)((uint8_t *)j + 4), sel = *(int32_t *)((uint8_t *)j + 8); \
    if (sel == (i ? 3 : 2)) { F[i].claims[slot] = 1; h_op1r_engine[slot] = F[i].name; } \
    else { F[i].claims[slot] = 0; h_op1r_engine[slot] = 0; stock_job(j); } logs(#n ".job"); } \
static void n##_build(void) { logs(#n ".build"); } static void n##_load(void) { logs(#n ".load"); } \
static void *n##_screen(int id) { (void)id; return F[i].claims[UI_SLOT_CUR] ? (void *)#n : (void *)"stock"; } \
static void n##_sync(int e) { (void)e; logs(F[i].claims[UI_SLOT_CUR] ? #n ".sync(own)" : #n ".sync(stock)"); } \
static int n##_knob(int k, int d) { (void)k; (void)d; logs(#n ".knob"); return F[i].claims[UI_SLOT_CUR]; } \
static int n##_key(int c, int p) { (void)c; (void)p; logs(#n ".key"); return F[i].claims[UI_SLOT_CUR]; } \
static const op1_drum_t n = { n##_render, n##_on, n##_off, n##_loaded, n##_is, n##_job, n##_build, n##_load, \
                              n##_screen, n##_sync, n##_knob, n##_key };
FAKE(dplus, 0)
FAKE(perc, 1)

static int checks;
static void expect(const char *what) { if (strcmp(h_log, what)) { printf("FAIL\n  got  [%s]\n  want [%s]\n", h_log, what); assert(0); } h_log[0] = 0; checks++; }
static void job(int slot, int sel) { int32_t j[3] = { 0, slot, sel }; dp_type_job(j); }

static int drum_claim_sync;
int main(void)
{
    static int16_t out[128]; char v[12][4];
    for (int i = 0; i < 12; i++) h_voices[i] = v[i];
    /* ---- 1. drum+ alone ---- */
    op1_drums[0] = &dplus; op1_drum_rows[0] = 2; op1_drum = &dplus; op1_drum_n = 1;
    h_drum_slot = 0;
    dp_fw_note_on(v[0], 0); dp_fw_render(v[0], out, 0); dp_fw_note_off(v[0], 0);
    expect("dplus.on dplus.render dplus.off ");                      /* stock kit: drum+ is called (plays stock) */
    job(0, 2); expect("dplus.job auto: dplus.sync(own) ");                                 /* claim */
    assert(dplus_is(0));
    uint8_t found; (void)found;
    dp_fw_note_on(v[1], 0); dp_fw_render(v[1], out, 0); expect("dplus.on dplus.render ");
    job(0, 0); expect("stock.job dplus.job ");                        /* a kit: drum+ lets go, stock job */
    job(0, 1); expect("stock.job dplus.job ");                        /* dbox */
    job(0, 3); expect("");                                           /* a row nobody has: nothing */
    h_ui_slot = 0; dpu_sync(0); expect("dplus.sync(stock) ");
    assert(dpu_knob(0, 1) == 0); expect("dplus.knob ");
    /* ---- 2. drum+ and perc ---- */
    op1_drums[1] = &perc; op1_drum_rows[1] = 3; op1_drum_n = 2;
    h_ui_slot = 1; drum_claim_sync = 1;
    job(1, 3); expect("perc.job dplus.loaded auto: perc.sync(own) ");   /* perc claims slot 1; drum+ is told; its page (owner only) */
    assert(perc_is(1) && !dplus_is(1));
    h_drum_slot = 1;
    dp_fw_note_on(v[2], 0); dp_fw_render(v[2], out, 0); expect("perc.on perc.render ");
    h_drum_slot = 0;                                                 /* slot change mid-sound: v2 stays perc's */
    dp_fw_render(v[2], out, 0); dp_fw_note_off(v[2], 0); expect("perc.render perc.off ");
    dp_fw_note_on(v[2], 0); expect("dplus.on ");                      /* a new hit on slot 0 (stock): drum+ */
    h_ui_slot = 1; dpu_sync(0); expect("perc.sync(own) ");                     /* only the owner writes the page table */
    assert(!strcmp((const char *)dpu_screen_for(0x10), "perc"));
    assert(dpu_knob(0, 1) == 1); expect("perc.knob ");
    assert(dpu_key(0x3B, 1) == 1); expect("perc.key ");
    h_ui_slot = 0; dpu_sync(0); expect("dplus.sync(stock) ");
    h_ui_slot = 1; job(1, 2); expect("dplus.job perc.loaded auto: dplus.sync(own) ");   /* perc kit -> drum+: drum+'s page */
    assert(dplus_is(1) && !perc_is(1));
    job(1, 3); expect("perc.job dplus.loaded auto: perc.sync(own) ");   /* back to perc */
    job(1, 1); expect("stock.job perc.job ");                         /* perc kit -> dbox: perc lets go */
    assert(!perc_is(1) && !dplus_is(1));
    job(1, 4); expect("");                                           /* row 4: nobody */
    /* ---- 3. perc without drum+: stock kits play stock, its rows still work ---- */
    op1_drums[0] = &perc; op1_drum_rows[0] = 3; op1_drum = 0; op1_drum_n = 1; memset(F[1].claims, 0, 8);
    h_drum_slot = 2;
    dp_fw_note_on(v[3], 0); dp_fw_render(v[3], out, 0); expect("stock.on stock.render ");
    job(2, 2); expect("");                                           /* drum+ row, drum+ absent: nothing */
    h_ui_slot = 2; job(2, 3); expect("perc.job auto: perc.sync(own) ");
    dp_fw_note_on(v[3], 0); expect("perc.on ");
    printf("drum routing: %d checks ok\n", checks);
    return 0;
}
