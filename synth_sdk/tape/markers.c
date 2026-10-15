/* Tape markers (docs/22): red press adds a marker at the cursor, Shift + red press removes the one at the cursor,
 * Shift + red turn jumps to the next / previous marker (wraps). One sorted set for the whole tape, sample positions,
 * saved in the DB table op1rmarkers (one text row). Drawn on the stock tape screen as ticks scrolling with the tape. */
#include "../op1_synth.h"
extern volatile uint8_t pu_mode;                            /* tape/punch.c: 1 armed, 2 punched in */

#define TM_MAX      32
#define TM_DUP      2205                      /* 50 ms: no second marker this close */
#define TM_HIT_PX   3                         /* remove: marker within 3 px of the cursor (at the current zoom) */
#define TM_STEP     441                       /* jump: skip markers closer than 10 ms (the one you are on) */
#define TM_MAXPOS   0xF2BFA0                  /* tape_c::check_pos range (6 min) */
#define TM_CENTRE_X 163973                    /* centerline x 160.13, Q10 (hardware: 160.88 sat right of it, 159.38 left) */

int32_t tm_pos[TM_MAX];                      /* (shared with tape/punch.c) */
int tm_n;

#ifdef OP1_HOST
int32_t tm_host_cursor; float tm_host_zoom = 40.0f; int32_t tm_host_located = -1; char tm_host_saved[400];
#define TM_CURSOR()  tm_host_cursor
static uint32_t tm_host_bits(void) { uint32_t b; memcpy(&b, &tm_host_zoom, 4); return b; }
#define TM_ZOOM_BITS() tm_host_bits()
#define TM_LOCATE(s) (tm_host_located = (s))
#else
#define TM_CURSOR()  (*(volatile int32_t *)0xFF802974 >> 6)
#define TM_ZOOM_BITS() (*(volatile uint32_t *)0x018C183C)      /* float px/s, read as bits */
#define TM_LOCATE(s) (((void (*)(int32_t))0x01A55024)(s))
#endif

/* ---- persistence: "p1,p2,..." decimal ---- */
static int tm_fmt(char *o)
{
    char *s = o;
    for (int i = 0; i < tm_n; i++) {
        if (i) *o++ = ',';
        char d[10]; int k = 0; uint32_t v = (uint32_t)tm_pos[i];
        do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (k) *o++ = d[--k];
    }
    *o = 0;
    return (int)(o - s);
}
static void tm_parse(const char *p, int len)
{
    tm_n = 0;
    int32_t v = 0; int have = 0;
    for (int i = 0; i <= len; i++) {
        char c = i < len ? p[i] : ',';
        if (c >= '0' && c <= '9') { if (v < TM_MAXPOS + 1) v = v * 10 + (c - '0'); have = 1; }
        else if (c == ',') {
            if (have && v <= TM_MAXPOS && tm_n < TM_MAX && (!tm_n || v > tm_pos[tm_n - 1])) tm_pos[tm_n++] = v;
            v = 0; have = 0;
        }
    }
}
#ifdef OP1_HOST
static void tm_save(void) { tm_fmt(tm_host_saved); }
static void tm_load(void) { int n = 0; while (tm_host_saved[n]) n++; tm_parse(tm_host_saved, n); }
#else
#define TM_DB_HANDLE ((void *(*)(void))0x0196DD44)
#define TM_DB_EXEC   ((void (*)(void *, const char *))0x0196F950)
#define TM_DB_GET    ((void (*)(void *, void *, const char *))0x0196FBD0)
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } tm_dstr;
static char tm_sql[64 + TM_MAX * 9];
static const char TM_CREATE[] = "CREATE TABLE IF NOT EXISTS op1rmarkers (id INTEGER PRIMARY KEY, data TEXT NOT NULL)";
static void tm_save(void)                                   /* KeyHandler thread, on change */
{
    static const char head[] = "INSERT OR REPLACE INTO op1rmarkers VALUES (0,'";
    char *o = tm_sql;
    for (const char *h = head; *h; h++) *o++ = *h;
    o += tm_fmt(o);
    *o++ = '\''; *o++ = ')'; *o = 0;
    void *db = TM_DB_HANDLE();
    TM_DB_EXEC(db, TM_CREATE);
    op1_db_write(db, tm_sql);                         /* (no assert: long statement) */
}
static void tm_load(void)
{
    void *db = TM_DB_HANDLE();
    TM_DB_EXEC(db, TM_CREATE);
    tm_dstr s = { 0 };
    TM_DB_GET(&s, db, "SELECT data FROM op1rmarkers WHERE id=0");
    if (s.size) tm_parse(s.cap > 15 ? s.u.ptr : s.u.buf, (int)s.size);   /* (heap copy once per boot, not freed) */
}
#endif
static uint8_t tm_loaded;
static void tm_ready(void) { if (!tm_loaded) { tm_loaded = 1; tm_load(); } }

/* ---- edits ---- */
/* zoom (IEEE float px/s from the firmware) -> Q10 integer, clamped to 1..4096 px/s; no soft-float */
int32_t tm_zoom_q10(void)
{
    uint32_t b = TM_ZOOM_BITS();
    int e = (int)((b >> 23) & 0xFF) - 127;
    if ((b >> 31) || e < 0) return 1 << 10;                        /* negative, < 1 px/s, 0, denormal */
    if (e >= 12) return 4096 << 10;                                /* >= 4096 px/s, inf, nan */
    uint32_t m = (b & 0x7FFFFF) | 0x800000;                        /* 1.m in Q23 */
    return (int32_t)(m >> (13 - e));                               /* Q23 * 2^e -> Q10 */
}
static int32_t tm_hit_samples(void)                       /* TM_HIT_PX at the current zoom, in samples */
{
    return (int32_t)((TM_HIT_PX * 1024 * 44100) / tm_zoom_q10());
}
void tm_add(void)
{
    tm_ready();
    int32_t c = TM_CURSOR();
    if (c < 0 || c > TM_MAXPOS || tm_n >= TM_MAX) return;
    int i = 0;
    while (i < tm_n && tm_pos[i] < c) i++;
    if ((i < tm_n && tm_pos[i] - c < TM_DUP) || (i > 0 && c - tm_pos[i - 1] < TM_DUP)) return;
    for (int k = tm_n; k > i; k--) tm_pos[k] = tm_pos[k - 1];
    tm_pos[i] = c; tm_n++;
    tm_save();
}
void tm_remove(void)
{
    tm_ready();
    int32_t c = TM_CURSOR(), tol = tm_hit_samples(), best = -1, bd = 0;
    for (int i = 0; i < tm_n; i++) {
        int32_t d = tm_pos[i] > c ? tm_pos[i] - c : c - tm_pos[i];
        if (d <= tol && (best < 0 || d < bd)) { best = i; bd = d; }
    }
    if (best < 0) return;
    for (int k = best; k < tm_n - 1; k++) tm_pos[k] = tm_pos[k + 1];
    tm_n--;
    tm_save();
}
static int32_t tm_acc;
void tm_turn(int delta)                                    /* Shift + red: one jump per detent, wraps */
{
    tm_ready();
    if ((tm_acc > 0 && delta < 0) || (tm_acc < 0 && delta > 0)) tm_acc = 0;
    tm_acc += delta;
    int st = tm_acc / 1024;
    if (!st || !tm_n) { if (st) tm_acc = 0; return; }
    tm_acc = 0;
    int32_t c = TM_CURSOR(), t = -1;
    if (st > 0) {
        for (int i = 0; i < tm_n; i++) if (tm_pos[i] > c + TM_STEP) { t = tm_pos[i]; break; }
        if (t < 0) t = tm_pos[0];
    } else {
        for (int i = tm_n - 1; i >= 0; i--) if (tm_pos[i] < c - TM_STEP) { t = tm_pos[i]; break; }
        if (t < 0) t = tm_pos[tm_n - 1];
    }
    TM_LOCATE(t);
}

/* ---- drawing, after the stock tape screen (vtable slot 0x01911B78) ---- */
#define TM_Y0 OP1_Q10(115)                                  /* caret above the ruler (y 122), below the heads */
static const uint8_t tm_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF };
static const uint8_t tm_green[4] = { 0x00, 0xED, 0x95, 0xFF }, tm_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF },
                     tm_grey[4] = { 0x6F, 0x71, 0x73, 0xFF };
static void tm_line(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
void tm_draw(void)
{
    tm_ready();
    int punch = pu_mode;
    void *r = op1_raster();
    if (punch) {                                                /* [o] while punch mode is on (armed or punched in) */
        const int32_t cx = 163686, cy = 56882, dx = OP1_Q10(20), h = OP1_Q10(13), arm = OP1_Q10(4);  /* circle 159.85, 55.55 */
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            int32_t x = cx + sgn * dx, xa = x - sgn * arm;
            tm_line(r, x, cy - h, x, cy + h, 0x600, tm_red);
            tm_line(r, x + sgn * 0x300, cy - h, xa, cy - h, 0x600, tm_red);   /* (+0.75 px: closed corner) */
            tm_line(r, x + sgn * 0x300, cy + h, xa, cy + h, 0x600, tm_red);
        }
    }
    if (!tm_n) return;
    int32_t c = TM_CURSOR(), z = tm_zoom_q10(), tol = (TM_HIT_PX * 1024 * 44100) / z;
    int32_t k = (int32_t)(((int64_t)z << 16) / 44100);              /* Q10 px per sample, << 16: once per frame */
    for (int i = 0; i < tm_n; i++) {
        int64_t x = TM_CENTRE_X + (((int64_t)(tm_pos[i] - c) * k) >> 16);
        if (x < OP1_Q10(3) || x > OP1_Q10(317)) continue;
        int32_t d = tm_pos[i] > c ? tm_pos[i] - c : c - tm_pos[i];
        const uint8_t *col = d <= tol ? tm_white : punch ? tm_red : tm_green;
        tm_line(r, (int32_t)x, OP1_Q10(124), x, OP1_Q10(146), 0x400, tm_grey);               /* through the tracks */
        int32_t xi = (int32_t)x;
        tm_line(r, xi - OP1_Q10(3), TM_Y0, xi, TM_Y0 + OP1_Q10(5), 0x600, col);       /* caret v */
        tm_line(r, xi + OP1_Q10(3), TM_Y0, xi, TM_Y0 + OP1_Q10(5), 0x600, col);
    }
}

/* tape screen vtable draw slot 0x01911B78: stock draw (0x01A5CF94), then the markers. Remembers the screen object so
 * the keys only act while it is the current screen. */
void *tm_scr;
extern void tl_draw(void);
void tm_screen_draw(void *scr, int dt)
{
    ((void (*)(void *, int))0x01A5CF94)(scr, dt);
    tm_scr = scr;
    tm_draw();
    tl_draw();                                              /* tape/tapeloop.c */
}
