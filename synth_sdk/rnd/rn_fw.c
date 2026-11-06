/* Synth randomiser, a tool plugin (rn_core.c, rn_draw.c): Shift + drop / Shift + lift on the synth pages.
 * A step loads as the synth controller's drop does: voices reset, patch_set, plug_apply_synth, save job, screen
 * request. Stored words come only from the firmware's range tables: parse_patch_json asserts on any other at boot. */
#pragma GCC optimize ("Os", "no-tree-loop-distribute-patterns")
#include "../op1_synth.h"
#include "rn_core.h"
#include "rn_draw.h"

#define SLOT          (*(volatile int *)0xFF8016D4 & 7)         /* current synth slot */
#define SHIFT         (*(volatile uint8_t *)0x01DD7F78)
#define CUR_MODE_ID   (*(volatile int *)0xFF907D58)             /* ModeSwitcher: active mode */
#define RN_SYNTH_MODE 0                                         /* the synth controller's mode */
#define CUR_SCREEN    (*(volatile int *)0x018B65C8)
#define T_PAGE        (*(volatile int *)0x01B4EE88)             /* synth page: 0 T1 .. 3 T4 */
#define RN_CYC_MS     400000u                                   /* CYCLES per ms (400 MHz) */
#define PATCH(s)      ((uint8_t *)0x01DDD7E8 + ((s) & 7) * 0x38)   /* synth patch objects: +0 slot, +4 JSON */
#define ENGINE(s)     (((const volatile int *)0xFF8016E4)[s])
#define FX_TYPE(s)    (((const volatile int *)0xFF801734)[s])
#define LFO_TYPE(s)   (((const volatile int *)0xFF801384)[s])
#define KNOB_W(s)     ((int16_t *)(0xFF8013C4 + (s) * 16))      /* live words: engine knobs */
#define ENV_W(s)      ((int16_t *)(0x01B60EF0 + (s) * 16))      /* envelope */
#define FX_W(s)       ((int16_t *)(0xFF801444 + (s) * 16))
#define LFO_W(s)      ((int16_t *)(0xFF8014C4 + (s) * 16))
#define ENG_RG(e, k)  ((const int16_t *)(0x01DE5890 + (e) * 0x40 + (k) * 8))   /* {min, max, scale, mode} */
#define ENV_RG(k)     ((const int16_t *)(0x01DE5710 + (k) * 6))                /* {min, max, scale} */
#define FX_RG(t, k)   ((const int16_t *)(0x01DE5C50 + (t) * 0x30 + (k) * 6))
#define LFO_RG(t, k)  ((const int16_t *)(0x01DE5740 + (t) * 0x30 + (k) * 6))
#define FX_ON(s)      (((int (*)(int))0x0196727C)(s))
#define LFO_ON(s)     (((int (*)(int))0x0196726C)(s))
#define SET_FX        ((void (*)(void *, int16_t *))0x019FF664) /* set_fx: JSON "fx_params", save */
#define SET_LFO       ((void (*)(void *, int16_t *))0x019FF734) /* set_lfo: JSON "lfo_params", save */
#define VOICES_RESET  ((void (*)(void))0x01A862D4)              /* all voices and held notes */
#define PATCH_SET     ((void (*)(void *, int, const void *))0x01A052DC)
#define PATCH_SAVE    ((void (*)(void *))0x01A00D90)            /* patch vt+0x34: save job */
#define SAVER_JOB     (*(void *const volatile *)0x01BA29D8)    /* the job the saver thread runs (0x01940AC4), 0: none */
#define SAVER_RD      (*(const volatile uint32_t *)0x01BA33E4) /* its job ring: read / write index */
#define SAVER_WR      (*(const volatile uint32_t *)0x01BA33E8)
#define CUR_CTRL      ((uint32_t *(*)(void))0x019E97FC)         /* ModeSwitcher: current controller (vtable at +4) */
#define LAST_MODE     ((int (*)(void))0x019E9528)
#define SCREEN_REQ    ((void (*)(int, int, int))0x019D7C94)     /* (screen id, slot, mode): the GUI's screen queue */
#define JV_CGET       ((const void *(*)(const void *, const char *))0x019BEB70)   /* const operator[](key) */
#define JV_KEY        ((void *(*)(void *, const char *))0x019BFEB4)               /* operator[](key) */
#define JV_IDX        ((void *(*)(void *, unsigned))0x019BFAF8)                   /* operator[](index) */
#define JV_ASSIGN     ((void *(*)(void *, const void *))0x019BDDC0)               /* operator= */
#define JV_DTOR       ((void (*)(void *, int))0x019BDD18)
#define JV_NULL_C     ((void *(*)(void *, int))0x019BFA30)                        /* Value(ValueType) */
#define JV_INT_C      ((void *(*)(void *, int))0x019BD890)                        /* Value(int) */
#define JV_BOOL_C     ((void *(*)(void *, int))0x019BD832)                        /* Value(bool) */
#define JV_STR_C      ((void *(*)(void *, const char *))0x019BDC4C)               /* Value(const char *) */
#define DB_FACTORY    ((void *(*)(void))0x0196DD38)             /* content/op1_factory.db */
#define DB_GET        ((void (*)(void *, void *, const char *))0x0196FBD0)
#define OP_DELETE     ((void (*)(void *))0x01A5E310)            /* frees TE strings (operator new) */
enum { JV_NULL = 0, JV_STRING = 4 };

extern int se_is_open(void);                                    /* screens/sampedit.c */
extern void plug_apply_synth(int *patch);                       /* plugload/plugload.c: patch_apply + selector */
extern const op1_engine_t *op1_engines[];                       /* plugins by number (dispatcher.c) */
extern const op1_fx_t *op1_fxs[];
extern int op1_engine_count, op1_fx_count;
static const int16_t rn_off_screens[] = { 0x2D, -1 };          /* sequencer page */
static rn_t rn;
static volatile uint8_t rn_ready;
static uint32_t cycles(void) { uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c)); return c; }

/* ---- env: the slot's sound, its ranges, applying a step ---- */
static int jtype(const void *v) { return v ? ((const uint8_t *)v)[8] : JV_NULL; }
static void jname(char *o, const void *json, const char *key)  /* a string field, "" if none */
{
    const void *v = JV_CGET(json, key);
    const char *s = jtype(v) == JV_STRING ? *(const char *const *)v : 0;
    int k = 0;
    while (s && s[k] && k < RN_NAME - 1) { o[k] = s[k]; k++; }
    while (k < RN_NAME) o[k++] = 0;
}
static int fw_read(int slot, rn_step *o)                       /* names from the JSON, words live */
{
    const void *json = PATCH(slot) + 4;
    jname(o->type, json, "type"); jname(o->fx, json, "fx_type"); jname(o->lfo, json, "lfo_type");
    for (int i = 0; i < 8; i++) {
        o->knobs[i] = KNOB_W(slot)[i]; o->adsr[i] = ENV_W(slot)[i];
        o->fx_params[i] = FX_W(slot)[i]; o->lfo_params[i] = LFO_W(slot)[i];
    }
    o->fx_on = (uint8_t)(FX_ON(slot) != 0); o->lfo_on = (uint8_t)(LFO_ON(slot) != 0);
    return o->type[0] ? 0 : -1;
}
static void jset_arr(void *json, const char *key, const int16_t *w)
{
    uint32_t tmp[4];                                            /* a Json::Value (16 bytes) */
    void *arr = JV_KEY(json, key);
    JV_NULL_C(tmp, JV_NULL); JV_ASSIGN(arr, tmp); JV_DTOR(tmp, 2);           /* exactly 8 entries afterwards */
    for (unsigned i = 0; i < 8; i++) { JV_INT_C(tmp, w[i]); JV_ASSIGN(JV_IDX(arr, i), tmp); JV_DTOR(tmp, 2); }
}
static void jset_str(void *json, const char *key, const char *s)
{ uint32_t tmp[4]; JV_STR_C(tmp, s); JV_ASSIGN(JV_KEY(json, key), tmp); JV_DTOR(tmp, 2); }
static void jset_bool(void *json, const char *key, int b)
{ uint32_t tmp[4]; JV_BOOL_C(tmp, b); JV_ASSIGN(JV_KEY(json, key), tmp); JV_DTOR(tmp, 2); }
static void page_refresh(int slot)                              /* as the stock key dispatch after a key */
{
    uint32_t *c = CUR_CTRL();
    const uint32_t *vt = (const uint32_t *)c[1];
    int id = ((int (*)(void *))vt[0x44 / 4])((uint8_t *)c + (int16_t)vt[0x40 / 4]);   /* the page's screen id */
    SCREEN_REQ(id, slot, LAST_MODE());
}
static int fw_apply(int slot, const rn_step *s)
{
    uint8_t *patch = PATCH(slot);
    uint32_t json[4];                                           /* a copy of the patch's JSON */
    JV_NULL_C(json, JV_NULL); JV_ASSIGN(json, patch + 4);
    jset_arr(json, "knobs", s->knobs); jset_arr(json, "adsr", s->adsr);
    jset_str(json, "fx_type", s->fx); jset_arr(json, "fx_params", s->fx_params); jset_bool(json, "fx_active", s->fx_on);
    jset_str(json, "lfo_type", s->lfo); jset_arr(json, "lfo_params", s->lfo_params); jset_bool(json, "lfo_active", s->lfo_on);
    VOICES_RESET();
    PATCH_SET(patch, slot, json);
    plug_apply_synth((int *)patch);
    JV_DTOR(json, 2);
    PATCH_SAVE(patch);
    page_refresh(slot);
    return 0;
}
static void page_range(rn_range *g, const int16_t *rg, int lo, int hi)   /* a page-2 word: its table range within lo..hi */
{
    if (rg[0] > lo) lo = rg[0];
    if (rg[1] < hi) hi = rg[1];
    if (hi > lo) { g->lo = (int16_t)lo; g->hi = (int16_t)hi; g->skip = 0; }
}
static int fw_engine_ranges(int slot, rn_range g[8])
{
    int e = ENGINE(slot), filled = 0;
    if (e < 0 || e > 31) return -1;
    for (int k = 0; k < 8; k++) {
        const int16_t *rg = ENG_RG(e, k);
        g[k].lo = rg[0]; g[k].hi = rg[1];
        g[k].skip = (uint8_t)(k > 3 || (rg[3] != 0 && rg[3] != 1) || rg[1] <= rg[0]);   /* linked sample ranges: stock-only */
        filled |= k < 4 && rg[1] > rg[0];
    }
    int n = KNOB_W(slot)[4] - OP1_ENGINE_MAGIC;                   /* plugin engine (iter): its page-2 words */
    const op1_engine_t *p = e == 14 && n > 0 && n < op1_engine_count ? op1_engines[n] : 0;
    for (int k = 0; p && k < 4; k++)
        if (p->press[k] == OP1_PRESS_PAGE || op1_press_steps(p->press[k])) page_range(&g[OP1_ENG_PAGE_WORD(k)], ENG_RG(e, OP1_ENG_PAGE_WORD(k)), 1, 32767);
    return filled ? 0 : -1;                                     /* table not filled yet */
}
static int fw_fx_ranges(int slot, rn_range g[8])
{
    int t = FX_TYPE(slot);
    if (!FX_ON(slot) || t < 0 || t > 15) return -1;
    for (int k = 0; k < 8; k++) {
        const int16_t *rg = FX_RG(t, k);
        if (k < 4 && rg[1] <= rg[0]) return -1;                 /* not filled */
        g[k].lo = rg[0]; g[k].hi = rg[1]; g[k].skip = (uint8_t)(k > 3);
    }
    int n = FX_W(slot)[4] - OP1_ENGINE_MAGIC;                     /* plugin FX (filter): its page-2 words */
    const op1_fx_t *p = t == 5 && n > 0 && n < op1_fx_count ? op1_fxs[n] : 0;
    for (int k = 0; p && k < 3; k++)
        if (p->press[k] == OP1_PRESS_PAGE || p->press[k] == OP1_PRESS_PAGE_SEMI) page_range(&g[5 + k], FX_RG(t, 5 + k), 0, 32767);
    return 0;
}
static int fw_lfo_ranges(int slot, rn_range g[8])
{
    int t = LFO_TYPE(slot);
    if (!LFO_ON(slot) || t < 0 || t > 15 || t == 4) return -1;  /* 4: midi / quad, packed words */
    for (int k = 0; k < 8; k++) {
        const int16_t *rg = LFO_RG(t, k);
        g[k].lo = rg[0]; g[k].hi = rg[1];
        g[k].skip = (uint8_t)(k > 3 || (t == 5 && k == 3) || (t == 6 && k == 0) || rg[1] <= rg[0]);   /* as midi/mx.c */
    }
    return 0;
}
static int fw_fx_set(int slot, const int16_t w[8])
{
    int16_t *live = FX_W(slot);
    for (int k = 0; k < 8; k++) if (k != 4) live[k] = w[k];   /* not the selector */
    SET_FX(PATCH(slot), live);
    return 0;
}
static int fw_lfo_set(int slot, const int16_t w[8])
{
    int16_t *live = LFO_W(slot);
    for (int k = 0; k < 4; k++) live[k] = w[k];
    SET_LFO(PATCH(slot), live);
    return 0;
}
static int fw_busy(void) { return SAVER_JOB != 0 || SAVER_RD != SAVER_WR; }   /* the saver writes sounds without a lock */
static const rn_env env = { fw_read, fw_apply, fw_engine_ranges, fw_fx_ranges, fw_lfo_ranges, fw_fx_set, fw_lfo_set, cycles, fw_busy };

/* ---- the panel, drawn over the page by the core (op1_overlay) ---- */
static volatile uint32_t panel_t0;                              /* CYCLES at the press */
static volatile uint8_t panel_back, panel_page;
static int synth_page(void)
{
    if (CUR_MODE_ID != RN_SYNTH_MODE || se_is_open()) return 0;
    for (const int16_t *i = rn_off_screens; *i >= 0; i++) if (CUR_SCREEN == *i) return 0;
    return 1;
}
static int panel_draw(void *r)                                  /* GUI thread: 0 = done */
{
    if (cycles() - panel_t0 >= RN_PANEL_MS * RN_CYC_MS || !synth_page() || T_PAGE != panel_page) return 0;
    rn_draw_panel(r, panel_back);
    return 1;
}

/* ---- keys (KeyHandler thread): 1 = consumed ---- */
extern const op1_tool_t TOOL_randomiser;
static uint8_t swallow[0x40];
static volatile uint8_t rn_req;                                 /* 1 lift, 2 drop: run by the main loop after the fade */
static int rn_key(int code, int pressed)
{
    if (code < 0 || code >= 0x40) return 0;
    if (!pressed && swallow[code]) { swallow[code] = 0; return 1; }
    if (!pressed || (code != 0x0A && code != 0x0B) || !SHIFT || !synth_page()) return 0;
    if (rn_ready && !rn_req) rn_req = (uint8_t)(code == 0x0A ? 1 : 2);   /* lift: back; drop: forward / random */
    swallow[code] = 1;
    return 1;
}

/* ---- main loop: the type lists once ---- */
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } rn_dstr;   /* as plugload.c */
static const char *db_text(rn_dstr *s, const char *sql)        /* first column of the first row, "" if none */
{
    uint8_t *p = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof *s; i++) p[i] = 0;
    DB_GET(s, DB_FACTORY(), sql);
    char *t = s->cap > 15 ? s->u.ptr : s->u.buf;
    if (!t || s->size > (s->cap > 15 ? s->cap : 15)) return "";
    t[s->size] = 0;
    return t;
}
static void db_free(rn_dstr *s) { if (s->cap > 15 && s->u.ptr) OP_DELETE(s->u.ptr); }
static int env_ranges(void)                                     /* envelope ranges, filled at boot: 1 = there */
{
    for (int k = 0; k < 4; k++) {
        const int16_t *rg = ENV_RG(k);
        if (rg[1] <= rg[0]) return 0;
        rn.adsr[k].lo = rg[0]; rn.adsr[k].hi = rg[1]; rn.adsr[k].skip = 0;
    }
    return 1;
}
static void rn_service(void)
{
    static uint8_t inited, tries;
    if (!inited) { rn_init(&rn, &env); inited = 1; }
    if (!rn_ready && tries < 8 && env_ranges()) {
        tries++;
        rn_dstr a, b;
        int ok = rn_lists(&rn, db_text(&a, "SELECT group_concat(type || '=' || default_params, ';') FROM fx_types"),
                          db_text(&b, "SELECT group_concat(type || '=' || default_params, ';') FROM lfo_types"));
        db_free(&a); db_free(&b);
        __asm__ volatile ("" ::: "memory");                     /* the lists before the flag */
        rn_ready = (uint8_t)ok;
    }
    static uint8_t waited;
    int req = rn_req;
    if (!req) return;
    if (op1_inst_fade(1) && ++waited < 40) return;              /* the voice reset / new synth and FX cut: silent first */
    waited = 0;
    int back = req == 1;
    if (synth_page() && (back ? rn_lift(&rn, SLOT) : rn_drop(&rn, SLOT))) {
        panel_back = (uint8_t)back; panel_page = (uint8_t)T_PAGE; panel_t0 = cycles();
        op1_overlay(&TOOL_randomiser);
    }
    op1_inst_fade(0);
    rn_req = 0;
}

const op1_tool_t TOOL_randomiser = { rn_key, rn_service, panel_draw };
