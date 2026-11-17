/* FX Shift words, the R246 side (screens/fxshiftx.h has the rules): the live values per FX position and slot, the
 * Shift + turn (dispatcher.c knob_event_hook, KeyHandler), the patch JSON "op1r_fx" (load: dp_keep_op1r on the loading
 * thread; save: written into the live patch, then the stock FX setter marks the patch changed) and the master's
 * op1rsettings keys. */
#include "../op1_synth.h"
#include "op1r_settings.h"
#include "fxshiftx.h"

extern const op1_fx_t *op1_fxs[];
extern const char *op1_fx_names[];
extern int op1_fx_count;
int op1_patch_wait(void);                                         /* patchjob.c */

#define JV_KEY      ((void *(*)(void *, const char *))0x019BFEB4)
#define JV_CGET     ((const void *(*)(const void *, const char *))0x019BEB70)
#define JV_IDX      ((void *(*)(void *, unsigned))0x019BFAF8)
#define JV_SIZE     ((unsigned (*)(const void *))0x019BE8D8)
#define JV_ASSIGN   ((void *(*)(void *, const void *))0x019BDDC0)
#define JV_FROM_INT ((void *(*)(void *, int))0x019BD890)
#define JV_FROM_STR ((void *(*)(void *, const char *))0x019BDC4C)
#define JV_DTOR     ((void (*)(void *, int))0x019BDD18)
#define PATCH_SET_FX ((void (*)(void *, const int16_t *))0x019FF664)
enum { JV_INT = 1, JV_UINT = 2, JV_STRING = 4, JV_ARRAY = 6, JV_OBJECT = 7 };
static uint8_t jv_type(const void *v) { return ((const uint8_t *)v)[8]; }
#define PATCH_OBJ(drum, slot) ((void *)(((drum) ? 0x01DDD9A8 : 0x01DDD7E8) + ((slot) & 7) * 0x38))
#define SLOT(pos)   (*(volatile int *)((pos) ? 0xFF8016D8 : 0xFF8016D4) & 7)
#define WORDS(pos)  ((int16_t *)((pos) == OP1_FX_MASTER ? 0xFF8016C4 : ((pos) ? 0xFF8015C4 : 0xFF801444) + SLOT(pos) * 16))

static fxx_words_t sw[2][8] = { [0 ... 1] = { [0 ... 7] = { { 0 }, -1 } } }, swm = { { 0 }, -1 };
static uint8_t layer[3];                                         /* Shift layer shown per position (0 / 1), not saved */
static int fx_num(const int16_t *w) { int n = w[4] - OP1_ENGINE_MAGIC; return n > 0 && n < op1_fx_count && op1_fxs[n] ? n : 0; }
static fxx_words_t *words_at(int pos) { return pos == OP1_FX_MASTER ? &swm : &sw[pos & 1][SLOT(pos)]; }

void op1_fx_shift_ext(const op1_fx_t *fx, int knob, int n, int dflt) { fxx_register(fx, knob, n, dflt); }
const int16_t *op1_fx_shift_words(int pos)                       /* any thread; the FX at pos (synth / drum: its slot) */
{
    static const int16_t none[FXX_K];
    if (pos < 0 || pos > OP1_FX_MASTER) return none;
    const int e = fx_num(WORDS(pos));
    return e ? fxx_get(words_at(pos), op1_fxs[e], e) : none;
}
int fxx_steps(int e, int k) { return e ? fxx_n(op1_fxs[e], k) : 0; }
int op1_fx_shift_layer(int pos) { return pos >= 0 && pos <= OP1_FX_MASTER ? layer[pos] : 0; }
int fxx_layers_of(int e) { return e ? fxx_layers(op1_fxs[e]) : 1; }
int fxx_press(int e, int pos)                                    /* Shift + an encoder press on a custom FX page: 1 = taken */
{
    if (!e || pos < 0 || pos > OP1_FX_MASTER || fxx_layers(op1_fxs[e]) < 2) return 0;
    layer[pos] ^= 1;
    return 1;
}

static void save_json(int drum, int slot, const fxx_words_t *x)  /* KeyHandler: into the live patch, then marked changed */
{
    op1_patch_wait();                                            /* not while a job serializes this patch */
    void *json = (uint8_t *)PATCH_OBJ(drum, slot) + 4;
    void *o = JV_KEY(json, "op1r_fx");
    uint32_t tmp[4];
    JV_FROM_STR(tmp, op1_fx_names[x->owner]); JV_ASSIGN(JV_KEY(o, "fx"), tmp); JV_DTOR(tmp, 2);
    void *a = JV_KEY(o, "v");
    for (unsigned k = 0; k < FXX_K; k++) { JV_FROM_INT(tmp, x->v[k]); JV_ASSIGN(JV_IDX(a, k), tmp); JV_DTOR(tmp, 2); }
    PATCH_SET_FX(PATCH_OBJ(drum, slot), WORDS(drum));            /* the stock setter: marks the patch changed (saved) */
}
/* the master's values: R246's background saver (as dots / euclid / plk) writes them once after the turns, on its own
 * thread, instead of five DB commits per click on the KeyHandler */
#define BG_REGISTER ((void (*)(void *))0x0193F010)
#define BG_DIRTY    ((void (*)(void *))0x0193EF3C)
static void bg_nop(void *self) { (void)self; }
static void bg_save(void *self)                                  /* saver thread */
{
    (void)self;
    static const char *const key[FXX_K] = { "mfxs0", "mfxs1", "mfxs2", "mfxs3", "mfxs4", "mfxs5", "mfxs6", "mfxs7" };
    const fxx_words_t m = swm;                                   /* (a turn meanwhile: dirty again, saved next pass) */
    for (int k = 0; k < FXX_K; k++) op1r_setting_put(key[k], m.v[k]);
    op1r_setting_put("mfxsn", m.owner);
}
static const uint32_t bg_vt[6] = { 0, 0, 0, (uint32_t)bg_nop, 0, (uint32_t)bg_save };
static struct { uint8_t dirty, saving, pad[2]; const uint32_t *vt; uint32_t more[2]; } bg;
static uint8_t bg_ready;
static void save_master(void) { if (bg_ready) BG_DIRTY(&bg); }
int fxx_knob(int e, int pos, int knob, int delta)                /* Shift + turn on a custom FX page: 1 = taken */
{
    const int k = knob + 4 * op1_fx_shift_layer(pos), n = fxx_steps(e, k);
    if (!n) return 0;
    static int32_t acc[FXX_K];
    fxx_words_t *x = words_at(pos);
    fxx_get(x, op1_fxs[e], e);
    if (!fxx_turn(&acc[k], &x->v[k], delta, n)) return 1;
    if (pos == OP1_FX_MASTER) save_master(); else save_json(pos & 1, SLOT(pos), x);
    return 1;
}
/* a synth / drum patch was parsed into a slot (dp_keep_op1r: fresh = the rebuilt patch, op = its "op1r_fx" or 0) */
void fxx_slot_loaded(int drum, int slot, const void *op)
{
    fxx_words_t *x = &sw[drum & 1][slot & 7];
    x->owner = -1;                                               /* none: the FX's defaults on first use */
    if (!op || jv_type(op) != JV_OBJECT) return;
    const void *nm = JV_CGET(op, "fx"), *a = JV_CGET(op, "v");
    const unsigned na = a && jv_type(a) == JV_ARRAY ? JV_SIZE(a) : 0;
    if (!nm || jv_type(nm) != JV_STRING || (na != 4 && na != FXX_K)) return;   /* (4: before the second layer) */
    const char *s = *(const char *const *)nm;
    int e = 0;
    for (int i = 1; i < op1_fx_count && s && !e; i++) {
        const char *p = op1_fx_names[i], *q = s;
        if (!p) continue;
        while (*p && *p == *q) p++, q++;
        if (!*p && !*q) e = i;
    }
    if (!e) return;                                              /* that FX is not here: defaults */
    fxx_get(x, op1_fxs[e], e);                                   /* defaults first: an older 4-value row keeps 4..7's */
    for (unsigned k = 0; k < na; k++) {
        const void *v = JV_IDX((void *)a, k);
        if (jv_type(v) != JV_INT && jv_type(v) != JV_UINT) return;
        const int32_t i = *(const int32_t *)v;
        x->v[k] = (int16_t)(i < 0 ? 0 : i > 32767 ? 32767 : i);
    }
    x->owner = (int16_t)e;
}
void fxx_load(void)                                              /* boot, GUI thread: the master's */
{
    static const char *const key[FXX_K] = { "mfxs0", "mfxs1", "mfxs2", "mfxs3", "mfxs4", "mfxs5", "mfxs6", "mfxs7" };
    if (!bg_ready && !op1r_safe_boot) { BG_REGISTER(&bg); bg.vt = bg_vt; bg_ready = 1; }   /* (safe mode: nothing saved) */
    const int e = op1r_setting_get("mfxsn", -1);
    if (e <= 0 || e >= op1_fx_count) return;
    for (int k = 0; k < FXX_K; k++) { int v = op1r_setting_get(key[k], 0); swm.v[k] = (int16_t)(v < 0 ? 0 : v > 32767 ? 32767 : v); }
    swm.owner = (int16_t)e;
}
