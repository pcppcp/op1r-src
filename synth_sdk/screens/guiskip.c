/* Exact GUI frame skip (docs/74): dev builds with OP1R_GUISKIP=1 only (hooks GUISKIP_HOOKS in make_firmware.py).
 * Every frame runs the screens' update() as usual (animations keep their timing). In a *record* frame nothing is
 * rasterised: points on the main rasterizer, sweeps, text draws, dims and dotted lines only feed a frame hash; the
 * raster points at a static scratch buffer as a safety net (not malloc: the GUI thread raced the main thread's heap) (a drawing path we do not hook lands there, not on the display).
 * Decision per GUI tick (two display buffers handed out in turn, [0x01D08520]):
 *   - record when both buffers hold the latest frame (hash_buf[A] == hash_buf[B] == last);
 *   - a record frame whose hash differs from `last` = a change: the next two ticks render (one per buffer); the shown
 *     buffer keeps the old frame for that one tick (16 ms latency after an idle period, no flicker);
 *   - otherwise render (animated pages render every tick, as stock).
 * The hash covers: main-rasterizer points (draw_line / paths), every sweep (rasterizer object, cell count, box, colour:
 * SVG shapes keep their cells between frames), text draws (object, position / colour words, string, matrix), dims,
 * dotted lines, fades (with their amount). */
#include "../op1_synth.h"

uint32_t gs_log[2][1024], gs_logn[2];          /* debug: the hashed values of the last render / record frame */
static uint32_t logn;
static uint8_t recording;                       /* this frame is a record frame */
static uint8_t no_hash;                         /* inside a region screen's static parts: not hashed (docs/75) */
#define FNV(h, v) do { uint32_t v_ = (uint32_t)(v); if (no_hash) break; if (logn < 1024) gs_log[recording][logn++] = v_; (h) = ((h) ^ v_) * 0x01000193u; } while (0)
static uint32_t fh = 0x811C9DC5u;               /* the frame being drawn */

static uint32_t hash_buf[2], last;              /* what each display buffer shows; the newest frame */
static uint32_t *cur_buf, *prev_buf, *buf_id[2];
static void *tgt_buf;                           /* the raster's target this frame (display buffer, or scratch in check frames) */
static uint8_t prev_rec, force;
volatile uint8_t gs_off;             /* 1: never record (A/B in one build: poke it) */
#ifdef GS_ON                                    /* frame-skip builds (OP1R_GUISKIP): else the code is linked but unused */
static uint8_t scratch[400 * 512 + 1024] __attribute__((aligned(32)));   /* raster target of record frames */
#else
static uint8_t scratch[4];
#endif
#if defined(GS_ON) && !defined(GS_EAGER)
const uint8_t gs_lazy = 1;                      /* dispatcher.c: plugin pages parsed when first shown (needs gs_frame) */
#else
const uint8_t gs_lazy = 0;
#endif
#ifdef LZ_NOBG
const uint8_t lz_bg = 0;                        /* test: lazy stock screens only on show / find_node */
#else
const uint8_t lz_bg = 1;
#endif
struct { uint32_t ticks, renders, records, changes, skipped_pts, skipped_sweeps, skipped_text, skipped_misc,
         verified, stale, stale_bytes, stale_first; } gs_stat;
/* self-check (GS_VERIFY builds): every 16th record tick renders in full into the scratch buffer instead; if its hash
 * says unchanged, the scratch must equal the buffer on display byte for byte (else counted as stale) */
#ifdef GS_VERIFY
static uint8_t verifying, vcount;
static uint32_t *vbuf;
#endif

#ifndef OP1_HOST
#define RASTER        (*(uint8_t *volatile *)0x01DF4E34)
#define TICK_BUF      (*(uint32_t *volatile *)0x01D08520)
#define RASTER_CLEAR  ((void (*)(void *))0x01A23466)
#define RASTER_TARGET ((void (*)(void *, void *))0x01A23442)
/* the panel as the raster object describes it (as kennel's kn_artcache reads it): [1] visible columns (198 on rev 1),
 * [2] bytes per row (512), [3] rows (400 rev 1, 376 rev 2, 320 FE); taken each frame in gs_frame. Pixel work needs
 * 512-byte rows and at most 400 rows (the buffers' size), else the normal drawing runs (geom_ok 0). */
static uint32_t g_rows = 400, g_cols = 198, g_ok;
#define BUF_ROWS      g_rows
#define BUF_BYTES     (g_rows << 9)
#define BUF_COLS      ((int)g_cols)

static int main_ras(const void *ras) { return ras == RASTER + 0x30; }
#ifdef GS_TILESTAT
/* measurement (docs/77): per rendered frame, the cells of all shapes and of the shapes touching a 16 px tile that
 * changed since the same buffer's previous rendered frame = what region drawing would still redraw */
#define TS_MAX 2048
#define TW 16
#define TH 26
static struct { uint32_t h, cells; int16_t x0, y0, x1, y1; } ts_sh[TS_MAX];
static int ts_n;
static uint32_t ts_prev[2][TW * TH], ts_now[TW * TH];
static uint8_t ts_dirty[TW * TH];
struct { uint32_t frames, cells, cells_replay, tiles_dirty, shapes; } gs_ts;
static void ts_shape(const uint8_t *ras, const uint8_t *bgra)
{
    const int16_t *b = (const int16_t *)(ras + 0x30);
    if (b[0] > b[2] || b[1] > b[3] || ts_n >= TS_MAX) return;
    uint32_t h = 0x811C9DC5u, c = *(const uint32_t *)(ras + 0xC);
    const uint32_t *cv = *(const uint32_t *const *)(ras + 0x10);   /* not used: the box + count + colour stand for it */
    (void)cv;
    h = (h ^ (uint32_t)(uintptr_t)ras) * 0x01000193u; h = (h ^ c) * 0x01000193u;
    h = (h ^ *(const uint32_t *)(ras + 0x30)) * 0x01000193u; h = (h ^ *(const uint32_t *)(ras + 0x34)) * 0x01000193u;
    h = (h ^ (bgra[0] | bgra[1] << 8 | bgra[2] << 16)) * 0x01000193u;
    if (main_ras(ras)) h = (h ^ fh) * 0x01000193u;            /* code-drawn: their points (in the frame hash so far) */
    ts_sh[ts_n].h = h; ts_sh[ts_n].cells = c;
    ts_sh[ts_n].x0 = b[0]; ts_sh[ts_n].y0 = b[1]; ts_sh[ts_n].x1 = b[2]; ts_sh[ts_n].y1 = b[3]; ts_n++;
}
static int tcl(int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; }
static void ts_frame_end(int slot)
{
    if (slot < 0) { ts_n = 0; return; }
    for (int t = 0; t < TW * TH; t++) ts_now[t] = 0x811C9DC5u;
    for (int i = 0; i < ts_n; i++)
        for (int ty = tcl(ts_sh[i].y0 >> 4, TH - 1); ty <= tcl(ts_sh[i].y1 >> 4, TH - 1); ty++)
            for (int tx = tcl(ts_sh[i].x0 >> 4, TW - 1); tx <= tcl(ts_sh[i].x1 >> 4, TW - 1); tx++)
                ts_now[ty * TW + tx] = (ts_now[ty * TW + tx] ^ ts_sh[i].h) * 0x01000193u;
    int nd = 0;
    for (int t = 0; t < TW * TH; t++) { ts_dirty[t] = ts_now[t] != ts_prev[slot][t]; nd += ts_dirty[t]; ts_prev[slot][t] = ts_now[t]; }
    for (int i = 0; i < ts_n; i++) {
        int hit = 0;
        for (int ty = tcl(ts_sh[i].y0 >> 4, TH - 1); ty <= tcl(ts_sh[i].y1 >> 4, TH - 1) && !hit; ty++)
            for (int tx = tcl(ts_sh[i].x0 >> 4, TW - 1); tx <= tcl(ts_sh[i].x1 >> 4, TW - 1) && !hit; tx++) hit = ts_dirty[ty * TW + tx];
        gs_ts.cells += ts_sh[i].cells; if (hit) gs_ts.cells_replay += ts_sh[i].cells;
    }
    gs_ts.frames++; gs_ts.tiles_dirty += (uint32_t)nd; gs_ts.shapes += (uint32_t)ts_n;
    ts_n = 0;
}
#endif

/* move_to / line_to entries: 1 = skip the body (record frame, main rasterizer) */
uint32_t gs_caller;                             /* the stub stores the caller's return address here */
int gs_point(void *ras, int x, int y)
{
    if (gs_caller >= 0x01914A9C && gs_caller < 0x01914B80) return 0;   /* the sweep closing its path (FUN_01914a9c): not a new point */
    if (main_ras(ras)) { FNV(fh, x); FNV(fh, y); if (recording) { gs_stat.skipped_pts++; return 1; } }
    return 0;                                   /* other rasterizers: SVG shapes rebuilding their kept cells */
}
int gs_sweep(const uint8_t *ras, const uint8_t *bgra)   /* the 5 sweep sites: 1 = skip */
{
#ifdef GS_TILESTAT
    if (!recording) ts_shape(ras, bgra);
#endif
    if (main_ras(ras)) FNV(fh, 0x5EE9);          /* its points are hashed already; no cells in record frames */
    else {                                       /* SVG shape: cells kept from an earlier frame */
        const int16_t *bx = (const int16_t *)(ras + 0x30);
        if (bx[0] > bx[2] || bx[1] > bx[3]) return recording;   /* no cells: draws nothing */
        FNV(fh, (uintptr_t)ras); FNV(fh, *(const uint32_t *)(ras + 0xC));
        FNV(fh, *(const uint32_t *)(ras + 0x30)); FNV(fh, *(const uint32_t *)(ras + 0x34));
    }
    FNV(fh, bgra[0] | bgra[1] << 8 | bgra[2] << 16 | (uint32_t)bgra[3] << 24);
    if (recording) gs_stat.skipped_sweeps++;
    return recording;
}
/* hook.S GS_TIMED (docs/91): words 0..7 cycles / calls of move_to, line_to, Text::draw, sweep; 8..11 the last call's cycles */
volatile uint32_t gs_prim[12];

/* ---- text cache (docs/91) ----
 * Text::draw 0x01A6259C rasterises its glyphs every time; its pixels all go through text_blend_glyph_rgb565 0xFFA08718
 * (one scanline of coverage spans from globals 0x01B61110.., called 3x from render_glyphs; L1 CALLs -> _l1_tglyph).
 * A string seen twice with the same key is captured (its spans + coverage); afterwards it is replayed with the same
 * blend arithmetic and Text::draw is skipped. The key: the string, the text words that place / colour it, the
 * matrix and the raster. */
#define TC_N    48
#define TC_POOL 24576
struct tc_ent { uint32_t k1, k2, used; uint16_t off, len; uint8_t state; };   /* state: 0 free, 1 seen, 2 capturing, 3 ready */
static struct tc_ent tc[TC_N];
static uint8_t tc_pool[TC_POOL] __attribute__((aligned(4)));
static uint32_t tc_top, tc_tick;
volatile struct tc_ent *volatile gs_cap;       /* the entry being captured (hook.S _l1_tglyph reads it) */
volatile uint8_t gs_tc_on = 1;                 /* 0: off (A/B: poke it) */
volatile uint32_t gs_tc_stat[4];
volatile uint8_t gs_tc_boxfill = 1;            /* 1: the backdrop as a plain rectangle (not pixel-exact at its edges) */
static const uint8_t *tc_box_r;                /* set while TE's backdrop routine runs for a cache hit */
/* dotted_hline(a = {x0, x1, y0, y1} q10, black): draw_line verticals of width [0x01DE5DE0] every [0x01DE5DDC] = a solid
 * box; filled here: pixels whose centre lies inside, through the raster matrix 0x01DE5E68. 1: done (skip the dots) */
static int tc_box_fill(const int32_t *a, const uint8_t *c)
{
    if (!tc_box_r || !gs_tc_boxfill || c[0] | c[1] | c[2] || c[3] != 0xFF) return 0;
    const int32_t hw = *(const int32_t *)0x01DE5DE0 / 2, st = *(const int32_t *)0x01DE5DDC;
    if (st > 2 * hw) return 0;                                   /* gaps between the dots: not a box */
    const int32_t p0[2] = { a[0] - hw, a[2] }, p1[2] = { a[1] + hw, a[3] };
    int32_t q0[2], q1[2];
    ((void (*)(int32_t *, const void *, const int32_t *))0x01A231B0)(q0, (const void *)0x01DE5E68, p0);
    ((void (*)(int32_t *, const void *, const int32_t *))0x01A231B0)(q1, (const void *)0x01DE5E68, p1);
    const uint8_t *d = *(const uint8_t *const *)(tc_box_r + 0x28);
    uint16_t *const *rows = *(uint16_t *const *const *)(d + 4);
    const int W = *(const int *)(d + 8), H = *(const int *)(d + 0xC);
    int32_t xa = q0[0] < q1[0] ? q0[0] : q1[0], xb = q0[0] < q1[0] ? q1[0] : q0[0];
    int32_t ya = q0[1] < q1[1] ? q0[1] : q1[1], yb = q0[1] < q1[1] ? q1[1] : q0[1];
    int x0 = (xa - 512 + 1023) >> 10, x1 = (xb - 512) >> 10, y0 = (ya - 512 + 1023) >> 10, y1 = (yb - 512) >> 10;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 >= W) x1 = W - 1; if (y1 >= H) y1 = H - 1;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) rows[y][x] = 0;
    return 1;
}                  /* test: skip the black dotted box on hits */                /* debug: dotted boxes drawn on hits, cycles */               /* hits, captures, flushes, overflows */
#define TG ((volatile uint8_t *)0x01B61110)
#define TG16(o) (*(volatile int16_t *)(TG + (o)))
#define TG32(o) (*(volatile uint32_t *)(TG + (o)))
static void tc_flush(void) { for (int i = 0; i < TC_N; i++) tc[i].state = 0; tc_top = 0; gs_tc_stat[2]++; }
void gs_tglyph(const uint8_t *const *ras, int n, const uint8_t *col)   /* from _l1_tglyph while capturing: one scanline */
{
    (void)ras; (void)n;
    struct tc_ent *e = (struct tc_ent *)gs_cap;
    if (!e) return;
    const int cnt = (int)TG32(0x1C);
    const int row = TG16(0x0E) + TG16(0x0A);                     /* 0x01B6111E + 0x01B6111A (high halves) */
    const int x0 = TG16(0x00) + TG16(0x08) - (int)TG32(0x10);    /* + cover pointer = x */
    const uint8_t *const *ptr = (const uint8_t *const *)TG32(0x14);
    const uint16_t *len = (const uint16_t *)TG32(0x18);
    uint32_t need = 8;
    for (int i = 1; i <= cnt; i++) need += 4 + ((len[i] + 1u) & ~1u);
    if (cnt <= 0 || cnt > 256 || tc_top + need > TC_POOL) { e->state = 0; gs_cap = 0; gs_tc_stat[3]++; return; }
    uint8_t *w = tc_pool + tc_top;
    *(int16_t *)w = (int16_t)row; *(uint16_t *)(w + 2) = (uint16_t)cnt;
    w[4] = col[0]; w[5] = col[1]; w[6] = col[2]; w[7] = col[3]; w += 8;   /* the colour as passed (a font may override it) */
    for (int i = 1; i <= cnt; i++) {
        const uint16_t l = len[i];
        *(int16_t *)w = (int16_t)((int)(uintptr_t)ptr[i] + x0); *(uint16_t *)(w + 2) = l; w += 4;
        for (int k = 0; k < l; k++) w[k] = ptr[i][k];
        w += (l + 1u) & ~1u;
    }
    tc_top += need; e->len = (uint16_t)(tc_top - e->off);
}
void gs_text_done(void)                        /* after the Text::draw body (hook.S): the capture is complete */
{
    struct tc_ent *e = (struct tc_ent *)gs_cap;
    if (e) { e->state = 3; gs_tc_stat[1]++; }
    gs_cap = 0;
}
static void tc_replay(const struct tc_ent *e, const uint8_t *r)   /* as text_blend_glyph_rgb565 */
{
    const uint8_t *d = *(const uint8_t *const *)(r + 0x28);     /* its raster: +4 rows, +8 width, +0xC height */
    uint16_t *const *rows = *(uint16_t *const *const *)(d + 4);
    const int W = *(const int *)(d + 8), H = *(const int *)(d + 0xC);
    const uint32_t M = 0x07E0F81Fu;
    const uint8_t *p = tc_pool + e->off, *end = p + e->len;
    while (p < end) {
        const int row = *(const int16_t *)p, cnt = *(const uint16_t *)(p + 2), a = p[7];
        const uint32_t c = ((uint32_t)p[6] << 8 | (uint32_t)p[5] << 19 | p[4] >> 3) & M;
        p += 8;
        uint16_t *rp = row >= 0 && row < H && a ? rows[row] : 0;
        for (int i = 0; i < cnt; i++) {
            int x = *(const int16_t *)p, l = *(const uint16_t *)(p + 2); p += 4;
            const uint8_t *cv = p; p += (l + 1u) & ~1u;
            if (!rp) continue;
            if (x < 0) { l += x; cv -= x; x = 0; if (l <= 0) continue; }
            if (x + l >= W) { l = W - x; if (l <= 0) continue; }
            uint16_t *q = rp + x;
            for (int k = 0; k < l; k++) {
                const uint32_t f = a == 0xFF ? (cv[k] + 4u) >> 3 : ((a * cv[k] >> 8) + 5u) >> 3;
                uint32_t v = (q[k] | (uint32_t)q[k] << 16) & M;
                v = (v + ((c - v) * f >> 5)) & M;
                q[k] = (uint16_t)(v | v >> 16);
            }
        }
    }
}
static int tc_lookup(const uint8_t *t, const uint8_t *r, const int32_t *m)   /* 1: replayed (skip Text::draw) */
{
    if (!gs_tc_on || !m) return 0;
    uint32_t k1 = 0x811C9DC5u, k2 = 0x9E3779B9u;
#define TK(v) do { uint32_t v_ = (uint32_t)(v); k1 = (k1 ^ v_) * 0x01000193u; k2 = (k2 + v_) * 0x85EBCA6Bu; k2 ^= k2 >> 13; } while (0)
    TK((uintptr_t)r); TK(*(const uint32_t *)(t + 0x18)); TK(*(const uint32_t *)(t + 0x1C));
    for (int o = 0x58; o < 0x74; o += 4) TK(*(const uint32_t *)(t + o));
    for (int o = 0x90; o < 0xA0; o += 4) TK(*(const uint32_t *)(t + o));   /* the dotted box (flag +0xA0) */
    TK(t[0xA0]);
    for (int i = 0; i < 9; i++) TK(m[i]);
    const char *s = *(const char *const *)(t + 0x74), *x = *(const char *const *)(t + 0x78);
    TK(x - s); for (; s < x; s++) TK(*s);
#undef TK
    tc_tick++;
    struct tc_ent *e = 0, *lru = &tc[0];
    for (int i = 0; i < TC_N; i++) {
        if (tc[i].state && tc[i].k1 == k1 && tc[i].k2 == k2) { e = &tc[i]; break; }
        if (!tc[i].state || (lru->state && tc[i].used < lru->used)) lru = &tc[i];
    }
    if (!e) { lru->k1 = k1; lru->k2 = k2; lru->state = 1; lru->used = tc_tick; return 0; }   /* first sight */
    e->used = tc_tick;
    if (e->state == 3) {
        if (t[0xA0]) {                                           /* Text::draw's black backdrop (0x01A60878: bounds +0x90..)
                                                                  * as one rectangle fill (gs_misc_dot) instead of its dots */
            tc_box_r = r;
            ((void (*)(const void *, const void *, const int32_t *))0x01A60878)(t, r, m);
            tc_box_r = 0;
        }   /* as Text::draw: its dotted box
                                                                  * first (bounds +0x90..+0x9C, dotted_hline: not glyph spans) */
        tc_replay(e, r); gs_tc_stat[0]++;
        return 1;
    }
    if (e->state == 1 && !gs_cap) {                              /* second sight: capture this draw */
        if (tc_top > TC_POOL - 2048) tc_flush();
        if (!e->state) return 0;
        e->off = (uint16_t)tc_top; e->len = 0; e->state = 2; gs_cap = e;
    }
    return 0;
}
int gs_text(const uint8_t *t, void *r, const int32_t *m)  /* Text::draw entry: 1 = skip */
{
    FNV(fh, (uintptr_t)t);
    for (int o = 0x58; o < 0x74; o += 4) FNV(fh, *(const uint32_t *)(t + o));   /* position, size, align, colour */
    const char *s = *(const char *const *)(t + 0x74), *e = *(const char *const *)(t + 0x78);
    for (; s < e; s++) FNV(fh, *s);
    if (m) for (int i = 0; i < 9; i++) FNV(fh, m[i]);
    if (recording) { gs_stat.skipped_text++; return 1; }
    return tc_lookup(t, (const uint8_t *)r, m);
}
int gs_misc(int id, const int32_t *a)          /* dim_all (id 1) / dotted_hline (id 2, a = {x0, x1, y}) */
{
    FNV(fh, 0xD1D0 + id);
    if (a) { FNV(fh, a[0]); FNV(fh, a[1]); FNV(fh, a[2]); }
    if (recording) gs_stat.skipped_misc++;
    return recording;
}

int gs_misc_dim(void *raster) { (void)raster; return gs_misc(1, 0); }
/* Raster::fade(raster, amount) 0x01A22F3C: the help screen's animated fade of everything under it (its update
 * 0x019B3A0C, the only caller); the amount changes per frame, so it must be in the hash (else the fade froze) */
int gs_misc_fade(void *raster, int32_t amount) { (void)raster; FNV(fh, amount); return gs_misc(3, 0); }
int gs_misc_dot(int id, const int32_t *a, const uint8_t *c) { (void)id; int r = gs_misc(2, a); FNV(fh, c[0] | c[1] << 8 | c[2] << 16); return r || tc_box_fill(a, c); }
static int slot_of(const uint32_t *b);

/* ---- static / dynamic screen regions (docs/75) ----
 * Region mode for a frame: a render frame (not record / check), the registered screen is the current one and the only
 * visible screen. Static parts are drawn once into `cache` (op1_screen_static = 1 in that frame); each display buffer
 * gets one full copy of it (buf_gen); afterwards only the declared areas are restored from the cache each frame. */
#define SCREEN_TABLE   ((uint8_t *const *)0x01DF4E54)
#define CUR_SCREEN     (*(volatile int *)0x018B65C8)
#define XFORM_PT       ((void (*)(int32_t *, const void *, const int32_t *))0x01A231B0)   /* (out, matrix, in) q10 */
#ifdef GS_ON
static uint8_t cache[400 * 512] __attribute__((aligned(32)));
#else
static uint8_t cache[4];
#endif
static void *dc_scr;                             /* the screen whose static SVG prefix the cache holds (draw_cached) */
static void *rg_scr;                             /* the screen that declared regions */
static op1_rect_t rg_scr_rect[OP1_REGIONS_MAX];  /* its areas (screen units) */
static int16_t rg_dev[OP1_REGIONS_MAX][4];       /* the same in device pixels (x0, y0, x1, y1), set per region frame */
static int rg_n;
static uint32_t gen = 1, cache_gen, buf_gen[2];
static uint8_t rg_active, rg_static, rg_prev_active;  /* this frame in region mode; static parts wanted this frame */
static void *rg_raster;
struct { uint32_t frames, statics, full_copies, restores; } gs_rg_stat;

void op1_screen_regions(void *scr, const op1_rect_t *r, int n)
{
#ifdef GS_NOREG
    (void)scr; (void)r; (void)n; return;          /* measurement builds: regions ignored */
#endif
    if (n > OP1_REGIONS_MAX) n = OP1_REGIONS_MAX;
    if (rg_scr != scr) { rg_scr = scr; gen++; }
    for (int i = 0; i < n; i++) rg_scr_rect[i] = r[i];
    rg_n = n;
}
int op1_screen_static(void *scr)
{
    int r = !recording && (!rg_active || (scr == rg_scr && rg_static));
    if (r && scr == rg_scr) no_hash = 1;         /* the hash covers the moving parts only, in every kind of frame */
    return r;
}
void op1_screen_static_dirty(void *scr) { if (scr == rg_scr) { gen++; force = 2; } }
static void copy_rows(uint8_t *dst, const uint8_t *src, int y0, int y1, int x0, int x1)   /* device px, inclusive */
{
    for (int y = y0; y <= y1; y++) {
        uint32_t *d = (uint32_t *)(dst + y * 512 + (x0 & ~1) * 2);
        const uint32_t *q = (const uint32_t *)(src + y * 512 + (x0 & ~1) * 2);
        for (int n = ((x1 | 1) - (x0 & ~1) + 1) / 2; n > 0; n--) *d++ = *q++;
    }
}
void op1_screen_static_done(void *scr)
{
    if (scr == rg_scr) no_hash = 0;
    if (!rg_active || scr != rg_scr || !rg_static) return;
    rg_static = 0;
    copy_rows((uint8_t *)cur_buf, cache, 0, (int)BUF_ROWS - 1, 0, BUF_COLS - 1);   /* the new static picture into the shown buffer */
    cache_gen = gen; buf_gen[slot_of(cur_buf)] = gen;
    gs_rg_stat.full_copies++;
    RASTER_TARGET(rg_raster, cur_buf);           /* the moving parts go to the display buffer */
}
static int visible_screens(void **only)
{
    int n = 0, cur = CUR_SCREEN;
    for (int i = 0; i < 0x44; i++) {
        uint8_t *o = SCREEN_TABLE[i];
        if (!o) continue;
        const uint8_t *vt = *(uint8_t *const *)(o + 0xC);
        int (*vis)(void *, int) = *(int (*const *)(void *, int))(vt + 0x14);
        if (vis(o + *(const int16_t *)(vt + 0x10), cur)) { n++; *only = o; }
    }
    return n;
}
static int16_t clamp_dev(int32_t v, int32_t hi) { return (int16_t)(v < 0 ? 0 : v > hi ? hi : v); }
static void rect_to_dev(const op1_rect_t *r, int16_t *d)
{
    int32_t in[4][2] = { { r->x0, r->y0 }, { r->x1 + 1, r->y0 }, { r->x0, r->y1 + 1 }, { r->x1 + 1, r->y1 + 1 } };
    int32_t x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (int k = 0; k < 4; k++) {
        int32_t p[2] = { in[k][0] << 10, in[k][1] << 10 }, o[2];
        XFORM_PT(o, (const void *)0x01DE5E68, p);
        if (o[0] < x0) x0 = o[0]; if (o[0] > x1) x1 = o[0];
        if (o[1] < y0) y0 = o[1]; if (o[1] > y1) y1 = o[1];
    }
    x0 = (x0 >> 10) - 2; y0 = (y0 >> 10) - 2; x1 = (x1 >> 10) + 2; y1 = (y1 >> 10) + 2;   /* antialiasing margin */
    d[0] = clamp_dev(x0, BUF_COLS - 1); d[1] = clamp_dev(y0, (int)BUF_ROWS - 1);   /* both ends: a rect off screen */
    d[2] = clamp_dev(x1, BUF_COLS - 1); d[3] = clamp_dev(y1, (int)BUF_ROWS - 1);   /* must not wrap in the int16 */
}
/* a render frame on the display buffer: region mode or the normal clear */
static void render_start(void *raster)
{
    void *only = 0;
    rg_active = 0;
    if (rg_scr && rg_n && g_ok && (void *)SCREEN_TABLE[CUR_SCREEN] == rg_scr && visible_screens(&only) == 1 && only == rg_scr
        && slot_of(cur_buf) >= 0) {
        if (!rg_prev_active) gen++;               /* the buffers / cache hold another picture */
        rg_active = 1; rg_raster = raster; gs_rg_stat.frames++;
        int sl = slot_of(cur_buf);
        if (cache_gen != gen) {                   /* static parts into the cache, then copied (static_done) */
            rg_static = 1; gs_rg_stat.statics++; dc_scr = 0;
            RASTER_TARGET(raster, cache); RASTER_CLEAR(raster);
        } else if (buf_gen[sl] != gen) {          /* this buffer still holds an older picture */
            rg_static = 0; copy_rows((uint8_t *)cur_buf, cache, 0, (int)BUF_ROWS - 1, 0, BUF_COLS - 1); buf_gen[sl] = gen; gs_rg_stat.full_copies++;
        } else {                                  /* restore the moving areas */
            rg_static = 0;
            for (int i = 0; i < rg_n; i++) {
                rect_to_dev(&rg_scr_rect[i], rg_dev[i]);
                copy_rows((uint8_t *)cur_buf, cache, rg_dev[i][1], rg_dev[i][3], rg_dev[i][0], rg_dev[i][2]);
            }
            gs_rg_stat.restores++;
        }
    } else RASTER_CLEAR(raster);
    rg_prev_active = rg_active;
}

/* ---- kennel's static SVG prefix (op1_screen_draw_cached, docs/77) ----
 * The prefix = the root's top-level children before first_live (0: all of them). After Hundekraft's kennel 3.2.2
 * (kn_artcache.c): a range of children is drawn by a copy of the root's group header with its child vector set to
 * that range (Group::draw 0x019EB818 reads matrix, children and visibility only; R1 = the document as
 * Document::draw passes it), so the shared tree is never changed (other threads may look nodes up / set them
 * meanwhile). The prefix is drawn once into `cache`, then each render frame copies the cache into the frame's
 * target buffer and draws the rest of the children. After a build the document's raster chunks are released
 * (its reset, vtable +0x7c, as on a screen change): the cache owns those pixels now. Falls back to the normal
 * draw when first_live is not a top-level child, the document's draw is not the known forwarder 0x01976560, or the
 * panel is not one we can copy. Ours: raw copy instead of kennel's run-length records, any panel size. */
#define SCREEN_DRAW ((void (*)(void *, int))0x01A3B4C8)
#define GROUP_DRAW  ((void (*)(void *, void *, const void *, void *))0x019EB818)
static void draw_range(void *doc, void **begin, void **end)
{
    uint32_t copy[0x58 / 4];
    for (unsigned i = 0; i < 0x58 / 4; i++) copy[i] = ((const uint32_t *)doc)[i];
    copy[0x44 / 4] = (uint32_t)(uintptr_t)begin; copy[0x48 / 4] = (uint32_t)(uintptr_t)end;
    GROUP_DRAW(copy, doc, (const void *)0x01B60FD8, *(void *volatile *)0x01DF4E34);   /* identity, the raster */
}
static void doc_reset(void *doc)
{
    uint8_t *v = *(uint8_t **)((uint8_t *)doc + 0x54);
    ((void (*)(void *))*(void **)(v + 0x7C))((uint8_t *)doc + *(int16_t *)(v + 0x78));
}
static void *dc_fl, *dc_raster;
static uint32_t dc_gen;
struct { uint32_t calls, builds, copies, fallbacks; } gs_dc_stat;
static void (*dc_extra)(void *scr);                              /* op1_screen_cached_extra: for the next cached draw */
void op1_screen_cached_extra(void (*fn)(void *scr)) { dc_extra = fn; }
void op1_screen_draw_cached_fn(void *scr, int dt, void *first_live)
{
    uint8_t *doc = scr ? *(uint8_t **)scr : 0;
    void (*xf)(void *) = dc_extra; dc_extra = 0;                 /* (one call) */
#if defined(GS_ON) && !defined(GS_NOCACHE)
    if (doc && !rg_active && dc_raster && tgt_buf && g_ok && *(volatile uint32_t *)0x01B60FFC) {
        const uint8_t *vt = *(uint8_t *const *)(doc + 0x54);
        void **begin = *(void ***)(doc + 0x44), **end = *(void ***)(doc + 0x48), **stop = end;
        int ok = *(const uint32_t *)(vt + 0x14) == 0x01976560u && *(const int16_t *)(vt + 0x10) == 0 && end - begin <= 1024;
        if (ok && first_live) { for (stop = begin; stop < end && *stop != first_live; stop++) ; ok = stop < end; }
        if (ok && stop > begin) {
            gs_dc_stat.calls++;
            if (!recording) {
                if (dc_scr != scr || dc_fl != first_live || dc_gen != gen) {   /* the static picture into the cache */
                    no_hash = 1;
                    RASTER_TARGET(dc_raster, cache); RASTER_CLEAR(dc_raster);
                    draw_range(doc, begin, stop);
                    if (xf) xf(scr);                             /* the plugin's static drawing joins the captured pixels */
                    RASTER_TARGET(dc_raster, tgt_buf);
                    no_hash = 0;
                    doc_reset(doc);
                    dc_scr = scr; dc_fl = first_live; dc_gen = gen; cache_gen = 0;   /* the regions' cache is gone */
                    gs_dc_stat.builds++;
                }
                copy_rows((uint8_t *)tgt_buf, cache, 0, (int)BUF_ROWS - 1, 0, BUF_COLS - 1);
                gs_dc_stat.copies++;
            }
            if (stop < end) draw_range(doc, stop, end);                         /* the live children */
            return;
        }
        gs_dc_stat.fallbacks++;
    }
#endif
    (void)dt;
    SCREEN_DRAW(scr, dt);
    if (xf) xf(scr);                                             /* no cache: after the whole SVG */
}

static int slot_of(const uint32_t *b) { if (buf_id[0] == b) return 0; if (buf_id[1] == b) return 1; return -1; }
/* Still pages (after Opus 2's "lighter screens"): once two record frames in a row matched the shown picture, the GUI
 * thread renders only every 4th tick; a key or a turn (lz_input) gives half a second of full rate, a held key all along. Both buffers hold the latest picture
 * then (record frames only run so), so a skipped tick presents the same pixels; the screens' update() and the hash run
 * every 4th tick, so a change of their own (meters, MIDI) shows within 3 ticks. CALL render at 0x019AB8B2. */
volatile uint8_t gs_throttle = 1;                /* 0: render every tick (A/B: poke it) */
volatile uint32_t gs_skipped;
volatile uint32_t gs_rcyc[4];                    /* rendered ticks, render cycles / 256 (wall), audio ISR / idle within (/256) */
static uint8_t idle_n;
void gs_tick(void *buf)
{
    extern volatile uint32_t lz_input; extern volatile uint8_t gs_keys_held;
    static uint32_t seen; static uint8_t n, hot;
    const uint32_t in = lz_input;
    if (in != seen) { seen = in; hot = 30; }                     /* input: full rate for half a second (a key's effect, e.g.
                                                                  * STEFAN's wind, may start a few frames later) */
    else if (hot) hot--;
    if (gs_throttle && !gs_off && idle_n >= 2 && !hot && !gs_keys_held && (++n & 3)) { gs_skipped++; return; }   /* and while a key is held */
    n = 0;
#ifndef OP1_HOST
    extern volatile uint32_t sm_aud_tot[2];
    extern uint32_t sm_idle_cycles(void);
    uint32_t c0, c1, a0 = sm_aud_tot[1], i0 = sm_idle_cycles();
    __asm__ volatile ("%0 = CYCLES;" : "=d"(c0));
#endif
    ((void (*)(void *))0x019B2674)(buf);
#ifndef OP1_HOST
    __asm__ volatile ("%0 = CYCLES;" : "=d"(c1));
    gs_rcyc[0]++; gs_rcyc[1] += (c1 - c0) >> 8; gs_rcyc[2] += sm_aud_tot[1] - a0;   /* audio ISR cycles inside it (/256) */
    gs_rcyc[3] += (sm_idle_cycles() - i0) >> 8;                  /* idle thread inside it: waits (/256) */
#endif
}
void gs_frame(void *raster)                    /* CALL Raster::clear in Screen::frame (0x01A3BBE0): a new frame */
{
    { extern void lazy_screens_frame(void); lazy_screens_frame(); }   /* plugin pages parsed when first shown (dispatcher.c) */
    dc_raster = raster;
    {   const uint32_t *r = raster;
        g_ok = r[2] == 512 && r[3] >= 64 && r[3] <= 400 && r[1] >= 32 && r[1] <= 256;
        if (g_ok) { g_rows = r[3]; g_cols = r[1]; } }
    /* the frame just drawn */
    uint32_t h = fh; fh = 0x811C9DC5u; no_hash = 0;
#ifdef GS_TILESTAT
    ts_frame_end(prev_buf && !prev_rec ? slot_of(prev_buf) : -1);
#endif
#ifdef GS_VERIFY
    if (verifying) {                            /* the check render: compare with the shown buffer */
        verifying = 0; prev_buf = 0;            /* not a display frame */
        if (h == last) {
            const uint8_t *x = scratch, *y = (const uint8_t *)vbuf;
            uint32_t n = BUF_BYTES, bad = 0, first = 0xFFFFFFFF;
            for (uint32_t i = 0; i < n; i++)    /* visible pixels only: device x 0..cols-1 of each 256-pixel row */
                if ((i & 511) < (uint32_t)BUF_COLS * 2 && x[i] != y[i]) { if (!bad) first = i; bad++; }
            gs_stat.verified++;
            if (bad) { gs_stat.stale++; gs_stat.stale_bytes += bad; gs_stat.stale_first = first; }
        }
    }
#endif
    gs_logn[recording] = logn; logn = 0;
    idle_n = prev_buf && prev_rec && h == last ? (uint8_t)(idle_n < 255 ? idle_n + 1 : 255) : 0;   /* still frames in a row */
    if (prev_buf) {
        int ps = slot_of(prev_buf);
        if (prev_rec) { if (h != last) { last = h; force = 2; gs_stat.changes++; } }
        else { if (ps >= 0) hash_buf[ps] = h; last = h; }
    }
    /* this tick's buffer */
    cur_buf = TICK_BUF;
    if (slot_of(cur_buf) < 0) {                 /* learn the two buffers */
        if (!buf_id[0]) buf_id[0] = cur_buf; else if (!buf_id[1]) buf_id[1] = cur_buf;
    }
    gs_stat.ticks++;
    int rec = !force && buf_id[1] && hash_buf[0] == last && hash_buf[1] == last && slot_of(cur_buf) >= 0
              && g_ok;
    if (force) force--;
    if (gs_off) rec = 0;
    recording = (uint8_t)rec; prev_rec = (uint8_t)rec; prev_buf = cur_buf;
#ifdef GS_VERIFY
    if (rec && !(++vcount & 15)) {              /* render this frame in full into scratch, then compare */
        verifying = 1; vbuf = cur_buf; recording = 0; prev_rec = 0; rg_active = 0;
        RASTER_TARGET(raster, scratch); RASTER_CLEAR(raster); tgt_buf = scratch;
        return;
    }
#endif
    if (rec) { gs_stat.records++; rg_active = 0; rg_prev_active = rg_prev_active && 1; RASTER_TARGET(raster, scratch); tgt_buf = scratch; }
    else { gs_stat.renders++; tgt_buf = slot_of(cur_buf) >= 0 ? cur_buf : 0; render_start(raster); }
}
#endif
