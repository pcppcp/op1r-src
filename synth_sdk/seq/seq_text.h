/* stroke text for our screens (oracle, peal, mixtur, arc, palme, in a room): the glyphs of screens/stroke_font.h
 * plus J, Q, W, packed, and ONE copy of the table and the drawing code in the firmware image (code space is short):
 * seq/oracle.c defines OCC_TEXT_IMPL before including this header and holds the definitions, everyone else links to
 * them. Host builds (-DOP1_HOST: tests, previews) compile a private static copy into each program instead.
 * sq_text(r, x, y, s, text, width, rgba): (x, y) top-left, scale s px per grid unit (Q10); returns the x after the
 * text. Same lines in the same order as stroke_font.h's sf_text (unknown chars leave a gap; '1' and '.' are narrow).
 * sq_line(r, x0, y0, x1, y1, width, rgba): one raster line (Q10); sq_dot(r, x, y, size, rgba): a dot (a short cross).
 * Also shared the same way (firmware only): some SDK inline helpers and the oracle / peal DB persistence.
 * Copyright (C) 2026 ultralazr. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef SEQ_TEXT_H
#define SEQ_TEXT_H
#pragma GCC push_options
#pragma GCC optimize("Os")                             /* GUI / saver code only: size, not speed */
#ifdef OP1_HOST
#define SQ_DEF static __attribute__((unused))
#else
#define SQ_DEF
#endif
#if defined(OP1_HOST) || defined(OCC_TEXT_IMPL)
/* glyph k = char sq_fc[k]: sq_fn[k] segments from segment sq_fo[k] (identical glyphs share them: O = 0, S = 5);
 * a segment is 2 bytes (x0 << 4 | y0, x1 << 4 | y1) on the 4 x 6 grid, in stroke_font.h's order */
static const char sq_fc[] = "0123456789ABCDEFG#/T+-%P.SYNRHILMOUVXKZ:JQW";
static const uint8_t sq_fo[] = { 0, 4, 6, 11, 15, 18, 23, 28, 30, 35, 40, 44, 23, 50, 55, 59, 62, 67, 71, 72, 74, 16,
    76, 79, 83, 18, 84, 87, 90, 95, 98, 24, 101, 0, 105, 108, 110, 112, 115, 118, 120, 123, 128 };
static const uint8_t sq_fn[] = { 4, 2, 5, 4, 3, 5, 5, 2, 5, 5, 4, 6, 3, 5, 4, 3, 5, 4, 1, 2, 2, 1, 3, 4, 1, 5, 3, 3, 5,
    3, 3, 2, 4, 4, 3, 2, 2, 3, 3, 2, 3, 5, 4 };
static const uint8_t sq_seg[] = {
    0x00, 0x40, 0x40, 0x46, 0x46, 0x06, 0x06, 0x00, 0x11, 0x20, 0x20, 0x26, 0x00, 0x40, 0x40, 0x43, 0x43, 0x03, 0x03,
    0x06, 0x06, 0x46, 0x00, 0x40, 0x40, 0x46, 0x46, 0x06, 0x13, 0x43, 0x00, 0x03, 0x03, 0x43, 0x40, 0x46, 0x40, 0x00,
    0x00, 0x03, 0x03, 0x43, 0x43, 0x46, 0x46, 0x06, 0x40, 0x00, 0x00, 0x06, 0x06, 0x46, 0x46, 0x43, 0x43, 0x03, 0x00,
    0x40, 0x40, 0x16, 0x00, 0x40, 0x40, 0x46, 0x46, 0x06, 0x06, 0x00, 0x03, 0x43, 0x43, 0x03, 0x03, 0x00, 0x00, 0x40,
    0x40, 0x46, 0x46, 0x06, 0x06, 0x00, 0x00, 0x40, 0x40, 0x46, 0x03, 0x43, 0x00, 0x06, 0x00, 0x30, 0x30, 0x33, 0x03,
    0x43, 0x43, 0x46, 0x46, 0x06, 0x00, 0x06, 0x00, 0x30, 0x30, 0x42, 0x42, 0x46, 0x46, 0x06, 0x40, 0x00, 0x00, 0x06,
    0x06, 0x46, 0x03, 0x33, 0x40, 0x00, 0x00, 0x06, 0x03, 0x33, 0x40, 0x00, 0x00, 0x06, 0x06, 0x46, 0x46, 0x43, 0x43,
    0x23, 0x10, 0x16, 0x30, 0x36, 0x02, 0x42, 0x04, 0x44, 0x40, 0x06, 0x00, 0x40, 0x20, 0x26, 0x03, 0x43, 0x21, 0x25,
    0x40, 0x06, 0x00, 0x01, 0x45, 0x46, 0x06, 0x00, 0x00, 0x40, 0x40, 0x43, 0x43, 0x03, 0x15, 0x16, 0x00, 0x23, 0x40,
    0x23, 0x23, 0x26, 0x06, 0x00, 0x00, 0x46, 0x46, 0x40, 0x06, 0x00, 0x00, 0x40, 0x40, 0x43, 0x43, 0x03, 0x13, 0x46,
    0x00, 0x06, 0x40, 0x46, 0x03, 0x43, 0x20, 0x26, 0x10, 0x30, 0x16, 0x36, 0x06, 0x00, 0x00, 0x23, 0x23, 0x40, 0x40,
    0x46, 0x00, 0x06, 0x06, 0x46, 0x46, 0x40, 0x00, 0x26, 0x26, 0x40, 0x00, 0x46, 0x40, 0x06, 0x00, 0x06, 0x40, 0x03,
    0x03, 0x46, 0x00, 0x40, 0x40, 0x06, 0x06, 0x46, 0x21, 0x22, 0x24, 0x25, 0x40, 0x46, 0x46, 0x06, 0x06, 0x04, 0x00,
    0x40, 0x40, 0x46, 0x46, 0x06, 0x06, 0x00, 0x24, 0x47, 0x00, 0x16, 0x16, 0x23, 0x23, 0x36, 0x36, 0x40 };
SQ_DEF void sq_line(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
SQ_DEF int32_t sq_text(void *r, int32_t x, int32_t y, int32_t s, const char *t, int32_t w, const uint8_t *c)
{
    for (; *t; t++) {
        for (unsigned k = 0; k < sizeof sq_fc - 1; k++) if (sq_fc[k] == *t) {
            const uint8_t *g = sq_seg + 2 * sq_fo[k];
            for (int j = 0; j < sq_fn[k]; j++, g += 2)
                sq_line(r, x + (g[0] >> 4) * s, y + (g[0] & 15) * s, x + (g[1] >> 4) * s, y + (g[1] & 15) * s, w, c);
            break;
        }
        x += (*t == '1' || *t == '.' ? 4 : 6) * s;
    }
    return x;
}
SQ_DEF void sq_dot(void *r, int32_t x, int32_t y, int32_t w, const uint8_t *c)   /* as euclid's edot */
{
    int32_t h = w / 2, t = w * 7 / 10;
    sq_line(r, x - h, y, x + h, y, t, c);
    sq_line(r, x, y - h, x, y + h, t, c);
}
/* text building: sq_cat appends s, sq_num appends v in decimal with at least `digits` digits (zero padded, <= 10);
 * both terminate the text and return its end */
SQ_DEF char *sq_cat(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
SQ_DEF char *sq_num(char *p, uint32_t v, int digits)
{
    char t[10]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n < digits) ((volatile char *)t)[n++] = '0';   /* volatile: gcc -O2 would make this a memset (none in the firmware) */
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}
/* persistence text of oracle / peal: k numbers 0..9999 joined with ',' into out (n bytes; stops before a number that
 * would not fit), returns the length; and back: returns the count, or -1 (bad char, empty field, > 9999, > max) */
SQ_DEF int sq_join(char *out, int n, const int *v, int k)
{
    int o = 0;
    for (int i = 0; i < k; i++) {
        char d[4]; int m = 0, x = v[i];
        do { d[m++] = (char)('0' + x % 10); x /= 10; } while (x && m < 4);
        if (o + m + 2 >= n) break;
        if (i) out[o++] = ',';
        while (m) out[o++] = d[--m];
    }
    out[o] = 0;
    return o;
}
SQ_DEF int sq_split(const char *s, int len, int *v, int max)
{
    int k = 0, x = -1;
    for (int i = 0; i <= len; i++) {
        char ch = i < len ? s[i] : ',';
        if (ch >= '0' && ch <= '9') { x = (x < 0 ? 0 : x * 10) + (ch - '0'); if (x > 9999) return -1; }
        else if (ch == ',') { if (x < 0 || k >= max) return -1; v[k++] = x; x = -1; }
        else return -1;
    }
    return k;
}
#else
void sq_line(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c);
int32_t sq_text(void *r, int32_t x, int32_t y, int32_t s, const char *t, int32_t w, const uint8_t *c);
void sq_dot(void *r, int32_t x, int32_t y, int32_t w, const uint8_t *c);
char *sq_cat(char *p, const char *s);
char *sq_num(char *p, uint32_t v, int digits);
int sq_join(char *out, int n, const int *v, int k);
int sq_split(const char *s, int len, int *v, int max);
#endif

/* SDK inline helpers that several of our screens use: one out-of-line firmware copy (the same code, called) */
#ifdef OP1_HOST
#define sq_fx_knobs_live    op1_fx_knobs_live
#define sq_synth_knobs_live op1_synth_knobs_live
#define sq_node_rotate      op1_node_rotate
#else
#ifdef OCC_TEXT_IMPL
void sq_fx_knobs_live(int16_t out[4]) { op1_fx_knobs_live(out); }
void sq_synth_knobs_live(int slot, int16_t out[4]) { op1_synth_knobs_live(slot, out); }
void sq_node_rotate(void *node, int32_t cx, int32_t cy, int32_t s15, int32_t c15) { op1_node_rotate(node, cx, cy, s15, c15); }
#else
void sq_fx_knobs_live(int16_t out[4]);
void sq_synth_knobs_live(int slot, int16_t out[4]);
void sq_node_rotate(void *node, int32_t cx, int32_t cy, int32_t s15, int32_t c15);
#endif

/* oracle / peal persistence (as euclid): a bg_saver object + DB table <name> (is_synth INTEGER PRIMARY KEY, data
 * TEXT), one row per mode. The firmware's bg_saver part comes first (the saver calls vt[5](self) with self = the
 * store), our fields follow. sq_store_load: once (player_get), loads both rows and registers the saver;
 * sq_store_dirty: after a change (once registered). */
typedef struct {
    uint8_t dirty, saving, pad[2]; const uint32_t *vt; uint32_t more[2];   /* firmware bg_saver */
    const char *name;                                   /* table */
    int (*ser)(int pos, char *out, int n);              /* row text of mode pos */
    int (*parse)(int pos, const char *s, int len);
    char *sql; int16_t cap, maxlen;                     /* statement buffer, longest row text accepted */
    uint8_t ready;                                      /* 1: loaded + saver, 2: no table, not saved */
} sq_store_t;
#ifdef OCC_TEXT_IMPL
#define SQ_BG_REGISTER ((void (*)(void *))0x0193F010)
#define SQ_BG_DIRTY    ((void (*)(void *))0x0193EF3C)
#define SQ_DB_HANDLE   ((void *(*)(void))0x0196DD44)
#define SQ_DB_GET      ((void (*)(void *, void *, const char *))0x0196FBD0)
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } sq_dstr;
static int sq_create(sq_store_t *st, void *db)          /* no db2::exec assert (euclid.c eu_load); 0 = table there */
{
    sq_cat(sq_cat(sq_cat(st->sql, "CREATE TABLE IF NOT EXISTS "), st->name), " (is_synth INTEGER PRIMARY KEY, data TEXT NOT NULL)");
    return op1_db_write(db, st->sql);
}
static void sq_bg_nop(void *self) { (void)self; }
static void sq_bg_save(void *self)                      /* saver thread */
{
    sq_store_t *st = (sq_store_t *)self;
    void *db = SQ_DB_HANDLE();
    sq_create(st, db);
    for (int pos = 0; pos < 2; pos++) {
        char *p = sq_cat(sq_cat(sq_cat(st->sql, "INSERT OR REPLACE INTO "), st->name), " VALUES (");
        *p++ = pos ? '0' : '1'; *p++ = ','; *p++ = '\'';
        int o = (int)(p - st->sql);
        o += st->ser(pos, st->sql + o, st->cap - o - 4);
        st->sql[o++] = '\''; st->sql[o++] = ')'; st->sql[o] = 0;
        op1_db_write(db, st->sql);
    }
}
static const uint32_t sq_bg_vt[6] = { 0, 0, 0, (uint32_t)sq_bg_nop, 0, (uint32_t)sq_bg_save };
void sq_store_dirty(sq_store_t *st) { if (st->ready == 1) SQ_BG_DIRTY(st); }
#pragma GCC push_options
#pragma GCC optimize("O2")                              /* (-Os clears the string with a memset call: none here) */
void sq_store_load(sq_store_t *st)
{
    if (st->ready) return;
    void *db = SQ_DB_HANDLE();
    if (sq_create(st, db)) { st->ready = 2; return; }
    for (int pos = 0; pos < 2; pos++) {
        sq_dstr s = { 0 };
        sq_cat(sq_cat(sq_cat(st->sql, "SELECT data FROM "), st->name), pos ? " WHERE is_synth=0" : " WHERE is_synth=1");
        SQ_DB_GET(&s, db, st->sql);
        const char *p = s.cap > 15 ? s.u.ptr : s.u.buf;
        if (s.size && s.size < (uint32_t)st->maxlen) st->parse(pos, p, (int)s.size);   /* (heap copy, once per boot) */
    }
    SQ_BG_REGISTER(st);
    st->vt = sq_bg_vt;
    st->ready = 1;
}
#pragma GCC pop_options
#else
void sq_store_dirty(sq_store_t *st);
void sq_store_load(sq_store_t *st);
#endif
#endif
#pragma GCC pop_options
/* width of a text in px * 1024 at scale s (as sq_text advances) */
__attribute__((unused)) static int32_t sq_width(int32_t s, const char *t)
{
    int32_t x = 0;
    for (; *t; t++) x += (*t == '1' || *t == '.' ? 4 : 6) * s;
    return x;
}
#endif
