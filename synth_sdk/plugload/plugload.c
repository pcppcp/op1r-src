/* Plugin loader (docs/37): /yaffs2/plugins/<name>.op1p files become synth engines / FX at boot.
 *
 * Runs from the CALL Clips::load at 0x019F02EE (db open, staged updates applied; before patches, master FX, audio
 * start and screens). Per file: read, op1p_parse (all bounds + CRC), malloc (heap 0), relocate, data-cache FLUSH +
 * IFLUSH, register in the RAM plugin tables (build/engines_table.c). A file named like a built-in replaces it
 * (same number). Other files get a number from the map in op1.db (op1rplugins: never reused): their preferred number
 * (header, = plugin.json order of a former built-in) if free, else >= OP1_FILE_FIRST, and
 * their synth_types / fx_types row (content/op1_factory.db); rows of file plugins no longer present are deleted (only when something changed).
 * Crash marker: plugins/.boot is created before loading and removed by plug_service() once the UI has run for a
 * while; found at boot -> all files are skipped this boot (and it is removed, the next boot tries again). */
#pragma GCC optimize ("Os", "no-tree-loop-distribute-patterns")   /* boot-time code: size; no strlen / memset calls */
#include "op1_synth.h"
#include "op1p_load.h"
#include "op1_input.h"
#include "seq/seq_text.h"

/* plugin tables (RAM, used by dispatcher.c): built-ins copied from build/engines_table.c, then the files */
const op1_engine_t *op1_engines[OP1_SLOTS];
int                 op1_engine_count;
const char         *op1_engine_names[OP1_SLOTS + 1];
const char         *op1_sample_engine_names[OP1_SLOTS + 1];
const op1_screen_t *op1_engine_screens[OP1_SLOTS];
const char         *op1_engine_svgs[OP1_SLOTS];
const op1_fx_t     *op1_fxs[OP1_SLOTS];
uint8_t            op1_fx_stereo[OP1_SLOTS];
const op1_engine_ext_t *op1_engine_exts[OP1_SLOTS];   /* extended engines (file flag OP1P_F_EXT), else 0 */
uint8_t            op1_engine_coarse[OP1_SLOTS];   /* synth: knobs (bit k) turned in 8 steps (file flag) */   /* the FX has render_stereo (file flag OP1P_F_STEREO, or built in) */
int                 op1_fx_count;
const char         *op1_fx_names[OP1_SLOTS + 1];
const op1_screen_t *op1_fx_screens[OP1_SLOTS];
const char         *op1_fx_svgs[OP1_SLOTS];
const op1_seq_t    *op1_seqs[OP1_SEQ_MAX];                     /* sequencer plugins (docs/44), in load order */
const char         *op1_seq_names[OP1_SEQ_MAX];
const op1_screen_t *op1_seq_screens[OP1_SEQ_MAX];
const char         *op1_seq_svgs[OP1_SEQ_MAX];
int                 op1_seq_count;
extern const op1_sampler_t *op1_sampler;                       /* samplerplus/sp_core.c (docs/46) */
int sp_adsr_stock(void *, int16_t *, const int16_t *, const int16_t *);   /* hook.S: the original Adsr_render */
void sp_replay(void);
const op1_tool_t   *op1_tools[OP1_TOOL_MAX];                   /* tool plugins, in load order */
int                 op1_tool_count;
extern const op1_drum_t *op1_drum;                             /* drumplus/dp_core.c: the drum engine plugin (docs/43) */
extern const op1_engine_t *const op1_builtin_engines[];
extern const int                 op1_builtin_engine_count;
extern const char *const         op1_builtin_engine_names[], *const op1_builtin_sample_engine_names[];
extern const op1_screen_t *const op1_builtin_engine_screens[];
extern const char *const         op1_builtin_engine_svgs[];
extern const op1_fx_t *const     op1_builtin_fxs[];
extern const int                 op1_builtin_fx_count;
extern const char *const         op1_builtin_fx_names[];
extern const op1_screen_t *const op1_builtin_fx_screens[];
extern const char *const         op1_builtin_fx_svgs[];
static void tables_init(void)
{
    int ne = op1_builtin_engine_count, nf = op1_builtin_fx_count;
    for (int i = 0; i < ne; i++) {
        op1_engines[i] = op1_builtin_engines[i];
        op1_engine_screens[i] = op1_builtin_engine_screens[i];
        op1_engine_svgs[i] = op1_builtin_engine_svgs[i];
        op1_engine_names[i] = op1_builtin_engine_names[i];             /* ne - 1 names + the 0 end */
    }
    for (int i = 0; op1_builtin_sample_engine_names[i]; i++) op1_sample_engine_names[i] = op1_builtin_sample_engine_names[i];
    for (int i = 0; i < nf; i++) {
        op1_fxs[i] = op1_builtin_fxs[i];
        op1_fx_stereo[i] = op1_fxs[i] && op1_fxs[i]->render_stereo;
        op1_fx_screens[i] = op1_builtin_fx_screens[i];
        op1_fx_svgs[i] = op1_builtin_fx_svgs[i];
        op1_fx_names[i] = op1_builtin_fx_names[i];
    }
    op1_engine_count = ne; op1_fx_count = nf;
}

extern volatile uint8_t op1_eng_page[4];
extern volatile uint8_t op1_fx_page[3][4];
void *read_fx_param_safe(int slot, int pos);
int16_t op1_page2_live(int fx, int pos, int k, int16_t stored);
int op1_patch_wait(void);
int  eu_fw_note_on(int pos, int note, int key, int vel);           /* seq/seq_core.c */
void eu_fw_note_off(int midi);
extern volatile uint8_t eu_rec_play;
void dpu_sync_auto(void);                                          /* dispatcher.c */
void op1_key_played(int midi, int vel, int on);                   /* seqdev/ms_fw.c (docs/50) */

#define Y_OPEN     ((int (*)(const char *, int, int))0x01A997F4)
#define Y_CLOSE    ((int (*)(int))0x01A99908)
#define Y_READ     ((int (*)(int, void *, unsigned))0x01A99C90)
#define Y_LSEEK    ((int (*)(int, int, int))0x01A9A0E4)
#define Y_UNLINK   ((int (*)(const char *))0x01A9A3FC)
#define Y_ACCESS   ((int (*)(const char *, int))0x01A9A6B0)
#define Y_OPENDIR  ((void *(*)(const char *))0x01A9B450)
#define Y_READDIR  ((uint8_t *(*)(void *))0x01A9B648)         /* dirent: d_name at +0xB */
#define Y_CLOSEDIR ((int (*)(void *))0x01A9B74C)
#define O_CREATE   0x241
#define MALLOC     ((void *(*)(unsigned))0x01A9F348)           /* heap 0 */
#define FREE       ((void (*)(void *))0x01AA0040)
#define OP_DELETE  ((void (*)(void *))0x01A5E310)           /* frees TE strings (operator new) */
#define CLIPS_LOAD ((void (*)(void *))0x0195CE88)          /* (clips object) */
#define DB_HANDLE  ((void *(*)(void))0x0196DD44)            /* user/op1.db: the number map */
#define DB_FACTORY ((void *(*)(void))0x0196DD38)            /* content/op1_factory.db: synth_types / fx_types */
#define DB_EXEC    ((void (*)(void *, const char *))0x0196F950)
#define DB_GET     ((void (*)(void *, void *, const char *))0x0196FBD0)
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } pl_dstr;

#define DIR_PATH   "/yaffs2/plugins"
#define MARKER     "/yaffs2/plugins/.boot"
#define MAX_FILES  96                                       /* plugin files read per boot (more: refused, counted) */
#define TOTAL_CAP  (4096u * 1024)                              /* all loaded plugins together (heap 0: ~9 MB free, docs/02) */

void plug_apply_synth(int *patch);
/* debug print (op1_plugin.h): empty; op1emu logs each call by its address (OP1EMU_DBG = _op1_dbg in build/synth.sym) */
__attribute__((noinline, used)) void op1_dbg(const char *text, int a, int b)
{
    __asm__ volatile ("" : : "r"(text), "r"(a), "r"(b) : "memory");
}
extern int se_is_open(void);                                    /* screens/sampedit.c */
static const struct { const char *name; void *addr; } exports[] = {
    { "op1_eng_page", (void *)op1_eng_page }, { "op1_input_tuner_active", (void *)op1_input_tuner_active },
    { "op1_input_gain_db", (void *)op1_input_gain_db }, { "op1_fx_page", (void *)op1_fx_page },
    { "read_fx_param_safe", (void *)read_fx_param_safe },
    { "op1_sc_source", (void *)op1_sc_source },
    { "op1_page2_live", (void *)op1_page2_live },
    { "op1_patch_wait", (void *)op1_patch_wait },
    { "op1_sine_q", (void *)&op1_sine_q },
    { "op1_l1_claim", (void *)op1_l1_claim }, { "op1_l1_claim_state", (void *)op1_l1_claim_state },
    { "op1_l1_sine_quarter", (void *)op1_l1_sine_quarter },
    { "sq_line", (void *)sq_line }, { "sq_text", (void *)sq_text }, { "sq_num", (void *)sq_num },
    { "sq_cat", (void *)sq_cat }, { "sq_node_rotate", (void *)sq_node_rotate },
    { "sq_synth_knobs_live", (void *)sq_synth_knobs_live }, { "sq_fx_knobs_live", (void *)sq_fx_knobs_live },
    /* sequencers (docs/44) */
    { "sq_dot", (void *)sq_dot }, { "sq_join", (void *)sq_join }, { "sq_split", (void *)sq_split },
    { "sq_store_load", (void *)sq_store_load }, { "sq_store_dirty", (void *)sq_store_dirty },
    { "op1_db_last_err", (void *)&op1_db_last_err }, { "eu_fw_note_on", (void *)eu_fw_note_on },
    { "eu_fw_note_off", (void *)eu_fw_note_off }, { "eu_rec_play", (void *)&eu_rec_play },
    /* drum engine (docs/43) */
    { "dpu_sync_auto", (void *)dpu_sync_auto },
    /* chord mode (docs/50): a consumed key for the MIDI recorder input I */
    { "op1_key_played", (void *)op1_key_played }, { "op1_dbg", (void *)op1_dbg },
    /* static / dynamic screen regions (screens/guiskip.c, docs/75) */
    { "op1_screen_regions", (void *)op1_screen_regions }, { "op1_screen_static", (void *)op1_screen_static },
    { "op1_screen_static_done", (void *)op1_screen_static_done }, { "op1_screen_static_dirty", (void *)op1_screen_static_dirty },
    { "op1_screen_draw_cached_fn", (void *)op1_screen_draw_cached_fn },
    /* the sample browser (screens/sbrowse.c): sampler+ T1 */
    { "sb_open", (void *)sb_open }, { "sb_close", (void *)sb_close }, { "sb_is_open", (void *)sb_is_open },
    { "sb_turn", (void *)sb_turn }, { "sb_pick", (void *)sb_pick }, { "sb_page", (void *)sb_page },
    /* sampler engine (docs/46) */
    { "sp_adsr_stock", (void *)sp_adsr_stock },
    /* tools */
    { "op1_overlay", (void *)op1_overlay }, { "op1_inst_fade", (void *)op1_inst_fade }, { "plug_apply_synth", (void *)plug_apply_synth },
    { "se_is_open", (void *)se_is_open }, { "op1_engines", (void *)op1_engines },
    { "op1_engine_count", (void *)&op1_engine_count }, { "op1_fxs", (void *)op1_fxs },
    { "op1_fx_count", (void *)&op1_fx_count },
};

/* boot report (resource monitor later): loaded count, refused count, last error, skipped by the crash marker */
int plug_loaded, plug_refused, plug_last_err, plug_skipped;
static int marker_set;
static uint32_t marker_c2;

/* ---- small string helpers (no libc) ---- */
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
#define scat sq_cat
static char *snum(char *p, int v) { return sq_num(p, (uint32_t)v, 0); }
static void dstr_init(pl_dstr *s) { uint32_t *w = (uint32_t *)s; for (unsigned i = 0; i < sizeof *s / 4; i++) ((volatile uint32_t *)w)[i] = 0; }
static char *sdup(const char *s) { char *d = MALLOC(slen(s) + 1); if (d) scat(d, s); return d; }

/* ---- name -> number map, one text row in op1.db: "s:name=n,f:name=n," (- prefix: row deleted) ---- */
#define MAP_MAX 64
static struct { char kind, present; char name[16]; int n; } map[MAP_MAX];
static int map_n, map_dirty;
static void map_load(void *db)
{
    DB_EXEC(db, "CREATE TABLE IF NOT EXISTS op1rplugins (id INTEGER PRIMARY KEY, data TEXT NOT NULL)");
    pl_dstr s; dstr_init(&s);
    DB_GET(&s, db, "SELECT data FROM op1rplugins WHERE id=0");
    const char *p = s.cap > 15 ? s.u.ptr : s.u.buf, *e = p + s.size;
    while (p < e && map_n < MAP_MAX) {
        int present = 1;
        if (*p == '-') { present = 0; p++; }
        if (e - p < 4 || (p[0] != 's' && p[0] != 'f' && p[0] != 'q' && p[0] != 'd' && p[0] != 'p') || p[1] != ':') break;   /* 'p' sampler+: was missing, so the map was rewritten every boot */
        char k = p[0]; p += 2;
        int i = 0;
        while (p < e && *p != '=' && i < 15) map[map_n].name[i++] = *p++;
        map[map_n].name[i] = 0;
        if (p >= e || *p != '=') break;
        p++;
        int v = 0;
        while (p < e && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        if (p < e && *p == ',') p++;
        if (v < 1 || v > OP1_MAX_ORDER) continue;
        map[map_n].kind = k; map[map_n].present = present; map[map_n].n = v; map_n++;
    }
    if (s.cap > 15 && s.u.ptr) OP_DELETE(s.u.ptr);
}
static void map_save(void *db)
{
    static char sql[64 + MAP_MAX * 24];
    char *o = scat(sql, "INSERT OR REPLACE INTO op1rplugins VALUES (0,'");
    for (int i = 0; i < map_n; i++) {
        if (!map[i].present) *o++ = '-';
        *o++ = map[i].kind; *o++ = ':';
        o = scat(o, map[i].name); *o++ = '=';
        o = snum(o, map[i].n); *o++ = ',';
    }
    scat(o, "')");
    op1_db_write(db, sql);
}

/* ---- the plugin's synth_types / fx_types row ---- */
static const int16_t fx_rng[4][2] = { { 64, 14528 }, { 0, 24576 }, { 0, 22528 }, { 8192, 32767 } };
static void row_params(char *o, const op1p_hdr *h, int n)
{
    int v[8];
    for (int i = 0; i < 4; i++) {
        int d = h->defaults[i] < 0 ? 0 : h->defaults[i];
        v[i] = h->kind == OP1P_FX ? fx_rng[i][0] + d * (fx_rng[i][1] - fx_rng[i][0]) / 32767 : d;
    }
    v[4] = OP1_ENGINE_MAGIC + n;
    for (int i = 5; i < 8; i++)
        v[i] = h->kind == OP1P_FX ? (h->page2_defaults[i - 5] < 0 ? 0 : h->page2_defaults[i - 5]) : 0;
    *o++ = '[';
    for (int i = 0; i < 8; i++) { if (i) { *o++ = ','; *o++ = ' '; } o = snum(o, v[i]); }
    *o++ = ']'; *o = 0;
}
static void row_ensure(void *db, const op1p_hdr *h, int n)
{
    const char *tbl = h->kind == OP1P_FX ? "fx_types" : "synth_types";
    char want[96], sql[256], *o;
    row_params(want, h, n);
    o = scat(scat(scat(scat(sql, "SELECT default_params FROM "), tbl), " WHERE type='"), h->name); scat(o, "'");
    pl_dstr s; dstr_init(&s);
    DB_GET(&s, db, sql);
    const char *have = s.cap > 15 ? s.u.ptr : s.u.buf;
    int same = s.size && s.size == (uint32_t)slen(want) && seq(want, have);
    int exists = s.size != 0;
    if (s.cap > 15 && s.u.ptr) OP_DELETE(s.u.ptr);
    if (same) return;
    if (exists) {
        o = scat(scat(scat(scat(sql, "UPDATE "), tbl), " SET default_params='"), want);
        o = scat(scat(o, "' WHERE type='"), h->name); scat(o, "'");
    } else {
        o = scat(scat(scat(sql, "INSERT INTO "), tbl), " SELECT MAX(id)+1,'");
        o = scat(scat(scat(o, h->name), "','"), want);
        o = scat(scat(o, "' FROM "), tbl);
    }
    op1_db_write(db, sql);
}
static void row_delete(void *db, char kind, const char *name)
{
    char sql[96];
    char *o = scat(scat(sql, kind == 'f' ? "DELETE FROM fx_types" : kind == 'q' ? "DELETE FROM seq_types"
                             : kind == 'd' ? "DELETE FROM drum_types" : "DELETE FROM synth_types"), " WHERE type='");   /* 'p': sampler+ row */
    scat(scat(o, name), "'");
    op1_db_write(db, sql);
}

/* ---- tables ---- */
static uint8_t from_file[2][OP1_SLOTS];                     /* entry registered by a file this boot */
static int find_number(int kind, const char *name)          /* names are indexed by number - 1 */
{
    int cnt = kind == OP1P_SYNTH ? op1_engine_count : op1_fx_count;
    const char **names = kind == OP1P_SYNTH ? op1_engine_names : op1_fx_names;
    for (int n = 1; n < cnt; n++)
        if ((kind == OP1P_SYNTH ? (const void *)op1_engines[n] : (const void *)op1_fxs[n]) && names[n - 1] &&
            seq(names[n - 1], name))
            return n;
    return 0;
}
static int number_for(int kind, const char *name, int pref)
{
    char k = kind == OP1P_FX ? 'f' : 's';
    int me = -1;
    for (int i = 0; i < map_n; i++)
        if (map[i].kind == k && seq(map[i].name, name)) me = i;
    if (me >= 0) {
        if (!map[me].present) { map[me].present = 1; map_dirty = 1; }
        /* a former built-in (preferred number) that got a file number earlier (old file without one): back to its
         * own number if free, so its old sounds work again */
        if (!(pref && map[me].n >= OP1_FILE_FIRST)) return map[me].n;
        int used = pref < (kind == OP1P_FX ? op1_fx_count : op1_engine_count) &&
                   (kind == OP1P_FX ? (const void *)op1_fxs[pref] : (const void *)op1_engines[pref]);
        for (int i = 0; i < map_n; i++) if (i != me && map[i].kind == k && map[i].n == pref) used = 1;
        if (!used) { map[me].n = pref; map_dirty = 1; }
        return map[me].n;
    }
    if (map_n >= MAP_MAX) return 0;
    /* the plugin's own number first (a former built-in keeps its sounds), else 32.., never a used one */
    for (int n = pref ? pref : OP1_FILE_FIRST; n <= OP1_MAX_ORDER; n = n == pref ? OP1_FILE_FIRST : n + 1) {
        int used = n < (kind == OP1P_FX ? op1_fx_count : op1_engine_count) &&
                   (kind == OP1P_FX ? (const void *)op1_fxs[n] : (const void *)op1_engines[n]);
        for (int i = 0; i < map_n; i++) if (map[i].kind == k && map[i].n == n) used = 1;
        if (used) continue;
        map[map_n].kind = k; map[map_n].present = 1; map[map_n].n = n;
        scat(map[map_n].name, name); map_n++; map_dirty = 1;
        return n;
    }
    return 0;
}
static void reg_name(const char **names, int n, int *count, const char *name)   /* names[n-1], "" holes, 0 end */
{
    for (int i = *count - 1; i < n - 1; i++) names[i] = "";
    names[n - 1] = name;
    if (n + 1 > *count) { *count = n + 1; names[n] = 0; }
}
static void reg_list(const char **list, const char *name)                      /* 0-terminated list */
{
    int i = 0;
    while (list[i]) { if (seq(list[i], name)) return; i++; }
    if (i < OP1_SLOTS) { list[i] = name; list[i + 1] = 0; }
}

static void cache_sync(uint8_t *p, uint32_t n)
{
    uint8_t *a = (uint8_t *)((uintptr_t)p & ~31u), *e = p + n;
    for (uint8_t *q = a; q < e; q += 32) __asm__ volatile("FLUSH [%0];" : : "a"(q) : "memory");
    __asm__ volatile("SSYNC;" : : : "memory");
    for (uint8_t *q = a; q < e; q += 32) __asm__ volatile("IFLUSH [%0];" : : "a"(q) : "memory");
    __asm__ volatile("CSYNC;" : : : "memory");
}

/* ---- sequencer plugins (docs/44): registered by name, seq_types row; map entries 'q' (number unused, 1) ---- */
static const char *const stock_seqs[] = { "pattern", "endless", "tombola", "finger", "sketch", "arpeggio" };
static int seq_taken(const char *name)
{
    for (unsigned i = 0; i < sizeof stock_seqs / sizeof stock_seqs[0]; i++) if (seq(stock_seqs[i], name)) return 1;
    for (int i = 0; i < op1_seq_count; i++) if (seq(op1_seq_names[i], name)) return 1;
    return 0;
}
/* a write only when the SELECT finds nothing: an INSERT that changes nothing still costs a write transaction on boot */
static int db_has(void *db, const char *select)
{
    pl_dstr s; dstr_init(&s);
    DB_GET(&s, db, select);
    int has = s.size != 0;
    if (s.cap > 15 && s.u.ptr) OP_DELETE(s.u.ptr);
    return has;
}
static void seq_row(void *db, const char *name)
{
    char sql[160];
    scat(scat(scat(sql, "SELECT type FROM seq_types WHERE type='"), name), "'");
    if (db_has(db, sql)) return;
    char *o = scat(scat(sql, "INSERT INTO seq_types (type) SELECT '"), name);     /* id: INTEGER PRIMARY KEY = next */
    o = scat(scat(o, "' WHERE NOT EXISTS (SELECT 1 FROM seq_types WHERE type='"), name);
    scat(o, "')");
    op1_db_write(db, sql);
}
static void kind_map(char k, const char *name)
{
    for (int i = 0; i < map_n; i++)
        if (map[i].kind == k && seq(map[i].name, name)) { if (!map[i].present) { map[i].present = 1; map_dirty = 1; } return; }
    if (map_n >= MAP_MAX) return;
    map[map_n].kind = k; map[map_n].present = 1; map[map_n].n = 1;
    scat(map[map_n].name, name); map_n++; map_dirty = 1;
}
#define seq_map(name) kind_map('q', (name))

/* loaded plugin images: base, size, file (crash reports: tools/op1crash.py reads _plug_imgs with emu_ctl peek) */
struct plug_img { uint32_t base, size; char file[24]; };
struct plug_img plug_imgs[32];
uint32_t plug_img_n;
static void placed(const uint8_t *mem, uint32_t size, const char *file)
{
    if (plug_img_n >= 32) return;
    struct plug_img *p = &plug_imgs[plug_img_n++];
    p->base = (uint32_t)mem; p->size = size;
    int i = 0;
    for (; i < 23 && file[i]; i++) p->file[i] = file[i];
    p->file[i] = 0;
}

static uint32_t total;
static int load_one(void *db, const char *file)
{
    char path[64];
    if (slen(file) > 40) return OP1P_E_SIZE;
    scat(scat(path, DIR_PATH "/"), file);
    int fd = Y_OPEN(path, 0, 0);
    if (fd < 0) return OP1P_E_SIZE;
    int len = Y_LSEEK(fd, 0, 2);
    Y_LSEEK(fd, 0, 0);
    uint8_t *buf = len > 0 && (uint32_t)len <= 2 * TOTAL_CAP ? MALLOC((len + 3) & ~3) : 0;
    int r = buf && Y_READ(fd, buf, len) == len ? 0 : OP1P_E_SIZE;
    Y_CLOSE(fd);
    op1p_view v;
    if (!r) r = op1p_parse(buf, len, &v);
    uint32_t imp[OP1P_MAX_IMPORTS];
    for (uint32_t i = 0; !r && i < v.hdr->n_imports; i++) {
        imp[i] = 0;
        for (unsigned k = 0; k < sizeof exports / sizeof exports[0]; k++)
            if (seq(exports[k].name, v.import[i])) imp[i] = (uint32_t)exports[k].addr;
        if (!imp[i]) r = OP1P_E_IMPORT;
    }
    if (!r && total + v.mem_size > TOTAL_CAP) r = OP1P_E_SIZE;
    int kind = r ? 0 : (int)v.hdr->kind;
    const char *name = r ? 0 : sdup(v.hdr->name);
    if (!r && kind == OP1P_SAMPLER) {                               /* sampler engine (sampler+): one, row id 100 */
        uint8_t *mem = op1_sampler ? 0 : MALLOC(v.mem_size);
        if (!mem) r = op1_sampler ? OP1P_E_HEADER : OP1P_E_SIZE;
        if (!r) {
            op1p_relocate(&v, mem, (uint32_t)mem, imp);
            cache_sync(mem, v.mem_size); placed(mem, v.mem_size, file);
            total += v.mem_size;
            op1_sampler = (const op1_sampler_t *)(mem + v.hdr->entry_off);
            if (!db_has(DB_FACTORY(), "SELECT id FROM synth_types WHERE id=100"))
                op1_db_write(DB_FACTORY(), "INSERT OR IGNORE INTO synth_types VALUES (100,'sampler+','[0, 0, 0, 0, 0, 0, 0, 0]')");
            kind_map('p', "sampler+");
            sp_replay();                                            /* patches were loaded before the plugins */
        }
        if (buf) FREE(buf);
        return r;
    }
    if (!r && kind == OP1P_DRUM) {                                  /* drum engine (drum+): one, row id 2 */
        uint8_t *mem = op1_drum ? 0 : MALLOC(v.mem_size);
        if (!mem) r = op1_drum ? OP1P_E_HEADER : OP1P_E_SIZE;
        if (!r) {
            op1p_relocate(&v, mem, (uint32_t)mem, imp);
            cache_sync(mem, v.mem_size); placed(mem, v.mem_size, file);
            total += v.mem_size;
            op1_drum = (const op1_drum_t *)(mem + v.hdr->entry_off);
            if (!db_has(DB_FACTORY(), "SELECT id FROM drum_types WHERE id=2"))
                op1_db_write(DB_FACTORY(), "INSERT OR IGNORE INTO drum_types VALUES (2,'drum+','[0, 0, 0, 0, 0, 0, 0, 0]')");
            kind_map('d', "drum+");
        }
        if (buf) FREE(buf);
        return r;
    }
    if (!r && kind == OP1P_SEQ) {                                   /* sequencer: no number, own table */
        if (!name || seq_taken(name) || op1_seq_count >= OP1_SEQ_MAX) r = OP1P_E_HEADER;
        uint8_t *mem = r ? 0 : MALLOC(v.mem_size);
        if (!r && !mem) r = OP1P_E_SIZE;
        if (!r) {
            op1p_relocate(&v, mem, (uint32_t)mem, imp);
            cache_sync(mem, v.mem_size); placed(mem, v.mem_size, file);
            total += v.mem_size;
            const char *svg = 0;
            if (v.svg_len && v.hdr->screen_off != OP1P_NO_SCREEN) {
                char t[64];
                scat(scat(t, "../../plugins/"), file);
                svg = sdup(t);
            }
            int i = op1_seq_count++;
            op1_seqs[i] = (const op1_seq_t *)(mem + v.hdr->entry_off);
            op1_seq_names[i] = name;
            op1_seq_screens[i] = svg ? (const op1_screen_t *)(mem + v.hdr->screen_off) : 0;
            op1_seq_svgs[i] = svg;
            seq_row(DB_FACTORY(), name);
            seq_map(name);
        }
        if (buf) FREE(buf);
        return r;
    }
    if (!r && kind == OP1P_TOOL) {                                  /* tool: keys, main loop, overlay */
        uint8_t *mem = op1_tool_count < OP1_TOOL_MAX ? MALLOC(v.mem_size) : 0;
        if (!mem) r = op1_tool_count < OP1_TOOL_MAX ? OP1P_E_SIZE : OP1P_E_HEADER;
        if (!r) {
            op1p_relocate(&v, mem, (uint32_t)mem, imp);
            cache_sync(mem, v.mem_size); placed(mem, v.mem_size, file);
            total += v.mem_size;
            op1_tools[op1_tool_count++] = (const op1_tool_t *)(mem + v.hdr->entry_off);
        }
        if (buf) FREE(buf);
        return r;
    }
    int builtin = r || !name ? 0 : find_number(kind, name);
    if (!r && (!name || (builtin && from_file[kind][builtin]))) r = OP1P_E_HEADER;   /* same name in two files */
    int n = r ? 0 : builtin ? builtin : number_for(kind, name, OP1P_PREF(v.hdr->flags) < OP1_FILE_FIRST ? OP1P_PREF(v.hdr->flags) : 0);
    if (!r && !n) r = OP1P_E_HEADER;
    uint8_t *mem = r ? 0 : MALLOC(v.mem_size);
    if (!r && !mem) r = OP1P_E_SIZE;
    if (!r) {
        op1p_relocate(&v, mem, (uint32_t)mem, imp);
        cache_sync(mem, v.mem_size); placed(mem, v.mem_size, file);
        total += v.mem_size;
        const char *svg = 0;
        if (v.svg_len && v.hdr->screen_off != OP1P_NO_SCREEN) {
            char t[64];
            scat(scat(t, "../../plugins/"), file);
            svg = sdup(t);
        }
        const op1_screen_t *scr = svg ? (const op1_screen_t *)(mem + v.hdr->screen_off) : 0;
        if (kind == OP1P_SYNTH) {
            op1_engines[n] = (const op1_engine_t *)(mem + v.hdr->entry_off);
            reg_name(op1_engine_names, n, &op1_engine_count, name);
            op1_engine_screens[n] = scr; op1_engine_svgs[n] = svg;
            if (v.hdr->flags & OP1P_F_USES_SAMPLE) reg_list(op1_sample_engine_names, name);
            op1_engine_coarse[n] = (uint8_t)OP1P_COARSE(v.hdr->flags);
            op1_engine_exts[n] = (v.hdr->flags & OP1P_F_EXT) && !(v.hdr->flags & OP1P_F_INPUT)   /* extended engine (op1r 60) */
                                 ? (const op1_engine_ext_t *)op1_engines[n] : 0;
            if (v.hdr->flags & OP1P_F_INPUT) {                       /* input engine (input/in_host.c): the extended entry */
                extern const op1_input_engine_t *op1_input_engines[];
                const op1_input_engine_t *ie = (const op1_input_engine_t *)op1_engines[n];
                op1_input_engines[n] = ie->reset && ie->process ? ie : 0;
            }
        } else {
            op1_fxs[n] = (const op1_fx_t *)(mem + v.hdr->entry_off);
            op1_fx_stereo[n] = (v.hdr->flags & OP1P_F_STEREO) != 0;     /* older files: the struct ends at press */
            reg_name(op1_fx_names, n, &op1_fx_count, name);
            op1_fx_screens[n] = scr; op1_fx_svgs[n] = svg;
        }
        from_file[kind][n] = 1;
        if (!builtin) row_ensure(DB_FACTORY(), v.hdr, n);           /* a replaced built-in keeps its row */
    }
    if (buf) FREE(buf);
    return r;
}

/* preferred number of a file without loading it (trailer -> header flags); 99 if none / unreadable: loads last */
static int file_pref(const char *file)
{
    char path[64];
    scat(scat(path, DIR_PATH "/"), file);
    int fd = Y_OPEN(path, 0, 0), p = 99;
    if (fd < 0) return p;
    uint32_t t[4], h[19];
    int len = Y_LSEEK(fd, 0, 2);
    if (len >= 16 + 76 && Y_LSEEK(fd, len - 16, 0) >= 0 && Y_READ(fd, t, 16) == 16 && t[0] == OP1P_TMAGIC &&
        t[1] <= (uint32_t)len - 16 - 76 && Y_LSEEK(fd, t[1], 0) >= 0 && Y_READ(fd, h, 76) == 76 && h[0] == OP1P_MAGIC &&
        OP1P_PREF(h[16]))
        p = OP1P_PREF(h[16]);
    Y_CLOSE(fd);
    return p;
}

void plug_boot(void)
{
    tables_init();
    void *dir = Y_OPENDIR(DIR_PATH);
    if (!dir) return;
    static char files[MAX_FILES][44];                     /* static: not on the boot thread's stack */
    int nf = 0;
    uint8_t *d;
    while ((d = Y_READDIR(dir))) {
        const char *s = (const char *)d + 0xB;
        int l = slen(s);
        if (!(l > 5 && l < 44 && seq(s + l - 5, ".op1p") && s[0] != '.')) continue;
        if (nf < MAX_FILES) scat(files[nf++], s);
        else { plug_refused++; plug_last_err = OP1P_E_SIZE; }   /* over the limit: shown on the monitor */
    }
    Y_CLOSEDIR(dir);
    if (!nf) return;
    if (Y_ACCESS(MARKER, 0) == 0) {                       /* the last boot with plugins did not reach the UI */
        Y_UNLINK(MARKER);
        plug_skipped = nf;
        return;
    }
    int fd = Y_OPEN(MARKER, O_CREATE, 0x180);
    if (fd >= 0) Y_CLOSE(fd);
    marker_set = 1;
    uint32_t c1;
    __asm__ volatile("%0 = CYCLES; %1 = CYCLES2;" : "=d"(c1), "=d"(marker_c2));
    void *db = DB_HANDLE();
    map_load(db);
    static int pref[MAX_FILES];                           /* load by preferred number: list rows keep the build order */
    for (int i = 0; i < nf; i++) pref[i] = file_pref(files[i]);
    for (int i = 1; i < nf; i++)
        for (int j = i; j > 0 && pref[j] < pref[j - 1]; j--) {
            int t = pref[j]; pref[j] = pref[j - 1]; pref[j - 1] = t;
            char f[44]; scat(f, files[j]); scat(files[j], files[j - 1]); scat(files[j - 1], f);
        }
    for (int i = 0; i < nf; i++) {
        int r = load_one(db, files[i]);
        if (r) { plug_refused++; plug_last_err = r; } else plug_loaded++;
    }
    for (int i = 0; i < map_n; i++) {                      /* file plugins gone since the last boot: drop rows */
        if (!map[i].present) continue;
        int here = map[i].kind == 's' ? map[i].n < op1_engine_count && op1_engines[map[i].n]
                 : map[i].kind == 'f' ? map[i].n < op1_fx_count && op1_fxs[map[i].n]
                 : map[i].kind == 'd' ? op1_drum != 0
                 : map[i].kind == 'p' ? op1_sampler != 0
                 : seq_taken(map[i].name);
        if (!here) { row_delete(DB_FACTORY(), map[i].kind, map[i].name); map[i].present = 0; map_dirty = 1; }
    }
    if (map_dirty) map_save(db);
}

/* ---- name lookup on load (docs/37): a sound's plugin is found by its type name, params[4] = this device's number.
 * Wrappers around the patch-apply methods (synth vtable 0x01910FB0 / FB8, drum vtable 0x01910F80, direct CALLs of
 * apply_synth) and the boot's MasterController::load. The original runs first (it writes the stored params, under its
 * VoiceCtl lock); then the selector word is set if the name is a loaded plugin (else left as stored: stock types,
 * unknown names). One 16-bit store: the audio thread reads it per block. ---- */
#define JSON_GET    ((const void *(*)(const void *, const char *))0x019BEB70)   /* const operator[](key) */
#define JSON_STR    ((void (*)(const void *, pl_dstr *))0x019BE00C)             /* asString -> TE string */
#define APPLY_SYNTH ((void (*)(int *))0x01A010F8)
#define APPLY_SFX   ((void (*)(int *))0x01A00DC4)
#define APPLY_DFX   ((void (*)(int *))0x01A0010C)
#define MASTER_LOAD ((void (*)(void))0x019D79BC)
#define MASTER_FX_TYPE ((void (*)(pl_dstr *))0x0196EF20)                     /* db2::get_master_fx_type */
static int number_of(int kind, pl_dstr *s)                /* 0: not a plugin; frees the string */
{
    char nm[16];
    const char *p = s->cap > 15 ? s->u.ptr : s->u.buf;
    int n = 0;
    if (s->size && s->size < 16) {
        for (uint32_t i = 0; i < s->size; i++) nm[i] = p[i];
        nm[s->size] = 0;
        n = find_number(kind, nm);
    }
    if (s->cap > 15 && s->u.ptr) OP_DELETE(s->u.ptr);
    return n;
}
static void set_selector(int kind, const int *patch, const char *key, uint32_t base)
{
    int slot = patch[0];
    if (slot < 0 || slot > 7) return;
    pl_dstr s; dstr_init(&s);
    JSON_STR(JSON_GET(patch + 1, key), &s);
    int n = number_of(kind, &s);
    if (n) ((volatile int16_t *)(base + slot * 16))[4] = (int16_t)(OP1_ENGINE_MAGIC + n);
}
void plug_apply_synth(int *patch)    { APPLY_SYNTH(patch); set_selector(OP1P_SYNTH, patch, "type", 0xFF8013C4); }
void plug_apply_synth_fx(int *patch) { APPLY_SFX(patch); set_selector(OP1P_FX, patch, "fx_type", 0xFF801444); }
void plug_apply_drum_fx(int *patch)  { APPLY_DFX(patch); set_selector(OP1P_FX, patch, "fx_type", 0xFF8015C4); }
void plug_master_load(void)                               /* CALL @0x019F054A (boot) */
{
    MASTER_LOAD();
    pl_dstr s; dstr_init(&s);
    MASTER_FX_TYPE(&s);
    int n = number_of(OP1P_FX, &s);
    if (n) ((volatile int16_t *)0xFF8016C4)[4] = (int16_t)(OP1_ENGINE_MAGIC + n);
}

/* CALL @0x019F02EE (Clips::load(clips) in R0, return value unused) */
volatile uint32_t op1r_boot_ms[2];          /* boot: the plugin loader, op1r's screen work (ms; resource monitor page 8) */
void plug_boot_hook(void *clips)
{
    { extern volatile uint32_t bt_split[8]; uint32_t c = op1_cycles(); CLIPS_LOAD(clips); bt_split[1] = (op1_cycles() - c) / 400000u; }
    uint32_t t = op1_cycles();
    plug_boot();
    op1r_boot_ms[0] = (op1_cycles() - t) / 400000u;
}

/* main loop (ci_service): remove the crash marker once the UI has run 2 CYCLES2 periods (2^32 cycles each, ~7-10 s) */
void plug_service(void)
{
    if (!marker_set) return;
    uint32_t c1, c2;
    __asm__ volatile("%0 = CYCLES; %1 = CYCLES2;" : "=d"(c1), "=d"(c2));
    (void)c1;
    if (c2 - marker_c2 >= 2) { Y_UNLINK(MARKER); marker_set = 0; }
}
