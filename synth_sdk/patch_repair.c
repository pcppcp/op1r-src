/* Sound JSON repair before the stock parser (patch::parse_patch_json 0x01A037A8), docs/45. The parser refuses some
 * sounds the OP-1 cannot otherwise get out of: the caller asserts and the OP-1 stops at boot.
 *   error 22: "fx_type" missing or empty (fx_types::is_valid 0x019AADF8; unknown names already load as filter,
 *             dispatcher.c fx_name_eq)
 *   error 25: an "lfo_type" the OP-1 does not know (lfo_types is_valid 0x019C9BC4)
 * Such a sound gets the filter FX / the tremolo LFO with their factory defaults, switched off (the rest of the sound
 * is kept). Replaces the CALLs of the parser in load_synth_patch, insert_user_patch and the two preset file
 * importers; load_drum_patch's call goes through drumplus/dp_fw.c dp_load_parse, which calls patch_repair too. */
#include <stdint.h>

#define JV_CGET   ((const void *(*)(const void *, const char *))0x019BEB70)   /* const operator[](key) */
#define JV_KEY    ((void *(*)(void *, const char *))0x019BFEB4)               /* operator[](key) */
#define JV_IDX    ((void *(*)(void *, unsigned))0x019BFAF8)                   /* operator[](index) */
#define JV_ASSIGN ((void *(*)(void *, const void *))0x019BDDC0)               /* operator= */
#define JV_DTOR   ((void (*)(void *, int))0x019BDD18)
#define JV_NULL_C ((void *(*)(void *, int))0x019BFA30)                        /* Value(ValueType) */
#define JV_INT_C  ((void *(*)(void *, int))0x019BD890)                        /* Value(int) */
#define JV_BOOL_C ((void *(*)(void *, int))0x019BD832)                        /* Value(bool) */
#define JV_STR_C  ((void *(*)(void *, const char *))0x019BDC4C)               /* Value(const char *) */
#define PARSE     ((int (*)(void *, int))0x01A037A8)
enum { T_NULL = 0, T_STRING = 4, T_ARRAY = 6 };
static int jtype(const void *v) { return v ? ((const uint8_t *)v)[8] : T_NULL; }
static const char *jstr(const void *v) { return jtype(v) == T_STRING ? *(const char *const *)v : 0; }
static int eq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }

static const char *const lfo_known[] = { "tremolo", "value", "random", "element", "midi", "bend", "crank", "quad", "fox", "pond", 0 };
static const int16_t fx_def[8] = { 7548, 0, 8272, 19572, 8000, 8000, 8000, 8000 };    /* filter (op1_factory.db) */
static const int16_t lfo_def[8] = { 16000, 0, 0, 16000, 0, 0, 0, 0 };                 /* tremolo */

static void set_type(void *json, const char *tkey, const char *type, const char *pkey, const int16_t *p,
                     const char *akey)
{
    uint32_t tmp[4];
    JV_STR_C(tmp, type); JV_ASSIGN(JV_KEY(json, tkey), tmp); JV_DTOR(tmp, 2);
    void *arr = JV_KEY(json, pkey);
    JV_NULL_C(tmp, T_NULL); JV_ASSIGN(arr, tmp); JV_DTOR(tmp, 2);          /* exactly 8 entries afterwards */
    for (unsigned i = 0; i < 8; i++) { JV_INT_C(tmp, p[i]); JV_ASSIGN(JV_IDX(arr, i), tmp); JV_DTOR(tmp, 2); }
    JV_BOOL_C(tmp, 0); JV_ASSIGN(JV_KEY(json, akey), tmp); JV_DTOR(tmp, 2);
}

void patch_repair(void *json)
{
    if (!json) return;
    const char *fx = jstr(JV_CGET(json, "fx_type"));
    if (!fx || !*fx) set_type(json, "fx_type", "filter", "fx_params", fx_def, "fx_active");
    const char *lfo = jstr(JV_CGET(json, "lfo_type"));
    int known = 0;
    for (const char *const *n = lfo_known; lfo && *n; n++) if (eq(lfo, *n)) known = 1;
    if (!known) set_type(json, "lfo_type", "tremolo", "lfo_params", lfo_def, "lfo_active");
}

int patch_parse_safe(void *json, int mode) { patch_repair(json); return PARSE(json, mode); }
