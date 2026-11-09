/* Type list groups (docs/73): in the stock FX type list (Shift + T3) and instrument list (Shift + T1), white picks a
 * group and the list shows only its entries (the instrument list's presets stay on green); the group name stands in white under the "1" of the left column. The list opens on "all".
 * The list is the fx_types cache 0x01BAB748 (a std::vector of 0x30-byte rows); a group points the browser controller at
 * our own vector of copies of its rows. The controller picks by row id (+0x1C), not by index (docs/73).
 * shortcut: the groups come from the table below only; plugin.json "group" and content/op1rgroups.json come with the
 * web editor. */
#include "../op1_synth.h"
#include "fxgroups.h"

/* groups per list kind (FG_FX, FG_SYNTH): group 0 is "all", the last "other" (names without a group) */
static const char *const fx_groups[] = { "all", "reverb", "delay", "granular", "modulation", "dynamics", "rhythm",
                                         "character", "other" };
static const char *const synth_groups[] = { "all", "analog", "digital", "keys", "pad", "model", "sample", "weird",
                                            "other" };   /* short: this list's rows are not moved, 7 letters fit */
const char *const *const fg_names[FG_KINDS] = { fx_groups, synth_groups };
const uint8_t fg_ngroups[FG_KINDS] = { sizeof fx_groups / sizeof *fx_groups, sizeof synth_groups / sizeof *synth_groups };
typedef struct { const char *name; uint8_t g; } fg_def_t;
static const fg_def_t fx_def[] = {
    { "spring", 1 }, { "reverb", 1 }, { "nostromo", 1 }, { "father", 1 }, { "sister", 1 }, { "snowglobe", 1 },
    { "palme", 1 }, { "in a room", 1 },
    { "delay", 2 }, { "tape", 2 },
    { "clouds", 3 }, { "hail", 3 },
    { "juni_chorus", 4 }, { "washer", 4 },
    { "ente", 5 }, { "punch", 5 }, { "nitro", 5 },
    { "stutter", 6 }, { "steve", 6 }, { "grid", 6 },
    { "phone", 7 }, { "cwo", 7 }, { "filter", 7 },
};
static const fg_def_t synth_def[] = {
    { "pulse", 1 }, { "voltage", 1 }, { "dsynth", 1 }, { "blot", 1 }, { "monolit", 1 }, { "juni", 1 }, { "elg", 1 },
    { "supersaw", 1 }, { "sid", 1 },
    { "digital", 2 }, { "fm", 2 }, { "phase", 2 }, { "cluster", 2 }, { "drwave", 2 }, { "dna", 2 }, { "wavetable", 2 },
    { "taffy", 2 },
    { "pew", 3 }, { "mixtur", 3 }, { "fmbell", 3 },
    { "eva", 4 }, { "vapora", 4 },
    { "string", 5 }, { "brassophone", 5 }, { "mbira", 5 }, { "hollowsaint", 5 },
    { "sampler", 6 }, { "sampler+", 6 }, { "granular", 6 }, { "amp", 6 },
    { "aether", 7 }, { "arc", 7 }, { "dddd", 7 }, { "iter", 7 },
};
static const fg_def_t *const fg_defs[FG_KINDS] = { fx_def, synth_def };
static const uint8_t fg_ndefs[FG_KINDS] = { sizeof fx_def / sizeof *fx_def, sizeof synth_def / sizeof *synth_def };
static int str_eq_n(const char *a, int n, const char *b)
{
    for (int i = 0; i < n; i++) if (a[i] != b[i] || !b[i]) return 0;
    return !b[n];
}
int fg_group_of(int kind, const char *name, int n)
{
    for (int i = 0; i < fg_ndefs[kind]; i++) if (str_eq_n(name, n, fg_defs[kind][i].name)) return fg_defs[kind][i].g;
    return fg_ngroups[kind] - 1;
}

/* ---- the list (KeyHandler thread; the browser screen draws it on the GUI thread) ----
 * The FX list is the fx_types cache (load_types, built once). It stays untouched: a group shows our own vector whose
 * rows are bitwise copies of the cache's (same names / ids; the copies are never destroyed, a long name's heap buffer
 * stays owned by the cache). */
static uint8_t fg_rows[2][FG_MAX_ROWS * FG_ROW];   /* two in turn: the screen may still draw the previous one */
static uint8_t *fg_vec[2][4];                    /* std::vector: (pad), begin, end, cap */
static int fg_buf;
int fg_cur, fg_kind;                             /* shown group (0 = all ..) and list kind */

static uint8_t *vget(const void *vec, int k) { uint8_t *p; __builtin_memcpy(&p, (const uint8_t *)vec + sizeof p * (k + 1), sizeof p); return p; }
static const char *row_name(const uint8_t *row, int *len)       /* std::string: +4 buffer / pointer, +0x14 size, +0x18 cap */
{
    const char *p;
    *len = *(const int32_t *)(row + 0x14);
    if (*(const uint32_t *)(row + 0x18) <= 15) return (const char *)(row + 4);
    __builtin_memcpy(&p, row + 4, sizeof p);
    return p;
}
int fg_count(int kind, const void *src, int g)   /* rows of group g in the list src */
{
    uint8_t *b = vget(src, 0), *e = vget(src, 1);
    int m = 0, len;
    for (uint8_t *r = b; r < e; r += FG_ROW) { const char *s = row_name(r, &len); m += g == 0 || fg_group_of(kind, s, len) == g; }
    return m;
}
/* the list to show for group g: src itself (all) or our vector of its members; 0 = none (empty / too many) */
void *fg_list(int kind, const void *src, int g)
{
    if (g == 0) return (void *)src;
    uint8_t *b = vget(src, 0), *e = vget(src, 1), *d = fg_rows[fg_buf];
    int m = 0, len;
    for (uint8_t *r = b; r < e; r += FG_ROW) {
        const char *s = row_name(r, &len);
        if (fg_group_of(kind, s, len) != g) continue;
        if (m == FG_MAX_ROWS) return 0;
        for (int j = 0; j < FG_ROW; j++) d[m * FG_ROW + j] = r[j];
        m++;
    }
    if (!m) return 0;
    uint8_t **v = fg_vec[fg_buf];
    v[1] = d; v[2] = d + m * FG_ROW; v[3] = v[2];
    fg_buf ^= 1;
    return v;
}
int fg_next(int kind, const void *src, int g, int dir)   /* the next / previous non-empty group after g */
{
    int n = fg_ngroups[kind];
    for (int i = 0; i < n; i++) {
        g = (g + (dir > 0 ? 1 : n - 1)) % n;
        if (fg_count(kind, src, g)) return g;
    }
    return g;
}

#ifndef OP1_HOST
#define FX_CACHE       ((void *)0x01BAB748)              /* fx_types list (load_types cache, the FX browser's list) */
#define BROWSER_LIST   (*(void *volatile *)0x01DE511C)   /* PresetBrowserController: the shown list */
#define BROWSER_FLAG   (*(volatile uint8_t *)0x01DE5120)
#define CUR_MODE_ID    (*(volatile int *)0xFF907D58)
#define SELECTED_ID    ((int (*)(void))0x01A17560)
#define SELECT_START   ((void (*)(int, int))0x01A172D4)
#define PB_UPDATE      ((void (*)(void *, int))0x01A16C00)

#define SYNTH_LIST     ((void *)0x01DDD668)              /* synth_type_list's static list (rebuilt at every opening) */
static void *const fg_src[FG_KINDS] = { FX_CACHE, SYNTH_LIST };
static int grouped_list_open(void)             /* a grouped list is shown (sets fg_kind); a fresh opening = all */
{
    if (CUR_MODE_ID != 8) return 0;
    void *l = BROWSER_LIST;
    for (int k = 0; k < FG_KINDS; k++) if (l == fg_src[k]) { fg_kind = k; fg_cur = 0; return 1; }
    return l == fg_vec[0] || l == fg_vec[1];
}
static int fx_list_open(void) { return grouped_list_open() && fg_kind == FG_FX; }
int fg_knob(int knob, int delta)               /* knob_event_hook: white in the FX list; 1 = consumed */
{
    static int32_t acc;
    if (knob != 2 || !grouped_list_open()) return 0;
    if ((acc > 0 && delta < 0) || (acc < 0 && delta > 0)) acc = 0;
    acc += delta;
    int st = acc / 1024;                         /* a step per hardware click (delta 1024; the emulator sends 2048) */
    acc -= st * 1024;
    if (!st) return 1;
    void *src = fg_src[fg_kind];
    int g = fg_next(fg_kind, src, fg_cur, st);
    void *l = g == fg_cur ? 0 : fg_list(fg_kind, src, g);
    if (!l) return 1;
    int id = SELECTED_ID();
    BROWSER_LIST = l;
    fg_cur = g;
    SELECT_START(id, BROWSER_FLAG);              /* knobs + rows from the new list; not in it: row 0 */
    return 1;
}

/* layout of the FX list (docs/73): the rows and their cursor start at 1/3 of the screen (cursor x 59.7 -> 106.7), the scroll bar
 * (track + thumb, x 173.8) at the right border (x 306.6, the preset column's bar). Node matrices: 9 Q10 words, tx at
 * [6]. The rows / track keep a matrix (set here); the cursor and thumb get theirs every frame (the two CALL sites of
 * node_set_matrix, hooked to fg_xform). */
#define XFORM          ((void (*)(int32_t *, const int32_t *))0x019EAD58)   /* node_set_matrix(node, m) */
/* cursor x 59.7..167.5 -> 106.7 (1/3 of the screen) .. 300.3 (the stock 6.3 px gap before the bar): scale + shift */
#define SX_CURSOR      OP1_Q10((300.3 - 106.67) / 107.77)
#define TX_CURSOR      OP1_Q10(106.67 - 59.718 * (300.3 - 106.67) / 107.77)
/* row texts: their tx is in panel pixels (0.8 px per unit measured on rev 1, scale 1.25): units x the screen matrix's
 * scale (0x01DE5E68 [3]: 1280 rev 1, 0x4B3 rev 2) */
#define DX_ROWS        ((int32_t)((int64_t)OP1_Q10(46.95) * ((const volatile int32_t *)0x01DE5E68)[3] >> 10))
#define DX_BAR         OP1_Q10(132.85)
static int32_t **pb_obj;                       /* the browser screen object (its update's self) */
static void set_tx(int32_t *node, int32_t tx)
{
    int32_t m[9];
    for (int i = 0; i < 9; i++) m[i] = node[i];
    m[6] = tx;
    XFORM(node, m);
}
void fg_xform(int32_t *node, const int32_t *m)  /* CALL node_set_matrix in the cursor (0x01A16936) / thumb (0x01A16D7C) code */
{
    int cur = 0, bar = 0;
    if (pb_obj && fx_list_open()) { cur = node == pb_obj[0x40 / 4]; bar = node == pb_obj[0x48 / 4]; }
    if (!cur && !bar) { XFORM(node, m); return; }
    int32_t c[9];
    for (int i = 0; i < 9; i++) c[i] = m[i];
    if (cur) { c[0] = SX_CURSOR; c[6] += TX_CURSOR; } else c[6] += DX_BAR;
    XFORM(node, c);
}

/* the type rows' names: the stock code cuts them to 13 characters (snprintf into a 16-byte buffer) before text_set;
 * in the FX list the row gets its full name (text_set keeps any length). CALL text_set at 0x01A16048. */
#define TEXT_SET       ((void (*)(void *, const char *))0x01A61AAC)
#define PB_LIST        (*(uint8_t *volatile *)0x01DE4FB8)            /* the list the browser screen shows */
#define PB_TOP         (*(volatile int *)0x01DE4FC0)                 /* first row shown */
void fg_rowtext(void *text, const char *s)
{
    if (pb_obj && fx_list_open())
        for (int k = 0; k < 6; k++)
            if (text == pb_obj[0x10 / 4 + k]) {
                uint8_t *l = PB_LIST, *row = vget(l, 0) + (k + PB_TOP) * FG_ROW;
                int len;
                if (row < vget(l, 1)) s = row_name(row, &len);      /* the std::string data ends with a 0 */
                break;
            }
    TEXT_SET(text, s);
}

static uint32_t fg_tx[OP1_TEXT_SIZE / 4];
static int fg_shown = -1;
void fg_pb_draw(void *self, int dt)            /* PresetBrowser vtable update (0x01911224 slot 3) */
{
    pb_obj = self;
    int fx = fx_list_open();
    for (int k = 0; k < 6; k++) set_tx(pb_obj[0x10 / 4 + k], fx ? DX_ROWS : 0);   /* the type column's rows */
    set_tx(pb_obj[0x50 / 4], fx ? DX_BAR : 0);                                   /* its scroll track */
    PB_UPDATE(self, dt);
    if (!grouped_list_open()) return;
    static const uint8_t white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    if (fg_shown < 0) op1_text_ctor(fg_tx, OP1_Q10(FG_LABEL_X), FG_LABEL_Y, 12, white);
    int key = fg_kind * 32 + fg_cur;
    if (fg_shown != key) { op1_text_set(fg_tx, fg_names[fg_kind][fg_cur]); fg_shown = key; }
    op1_text_draw(fg_tx, op1_raster(), ident);
}
#endif
