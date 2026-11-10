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
 * dotted lines. */
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
#define BUF_ROWS      (*(volatile uint32_t *)0xFF8017B4)   /* panel long side: 400 rev 1, 376 rev 2, 320 FE (Opus kit) */
#define BUF_BYTES     (BUF_ROWS << 9)
#define BUF_COLS      ((int)BUF_ROWS / 2)                  /* its short side: 200 / 188 / 160 visible of each 256-pixel row */

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
int gs_text(const uint8_t *t, void *r, const int32_t *m)  /* Text::draw entry: 1 = skip */
{
    (void)r;
    FNV(fh, (uintptr_t)t);
    for (int o = 0x58; o < 0x74; o += 4) FNV(fh, *(const uint32_t *)(t + o));   /* position, size, align, colour */
    const char *s = *(const char *const *)(t + 0x74), *e = *(const char *const *)(t + 0x78);
    for (; s < e; s++) FNV(fh, *s);
    if (m) for (int i = 0; i < 9; i++) FNV(fh, m[i]);
    if (recording) gs_stat.skipped_text++;
    return recording;
}
int gs_misc(int id, const int32_t *a)          /* dim_all (id 1) / dotted_hline (id 2, a = {x0, x1, y}) */
{
    FNV(fh, 0xD1D0 + id);
    if (a) { FNV(fh, a[0]); FNV(fh, a[1]); FNV(fh, a[2]); }
    if (recording) gs_stat.skipped_misc++;
    return recording;
}

int gs_misc_dim(void *raster) { (void)raster; return gs_misc(1, 0); }
int gs_misc_dot(int id, const int32_t *a, const uint8_t *c) { (void)id; int r = gs_misc(2, a); FNV(fh, c[0] | c[1] << 8 | c[2] << 16); return r; }
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
    d[0] = (int16_t)(x0 < 0 ? 0 : x0); d[1] = (int16_t)(y0 < 0 ? 0 : y0);
    d[2] = (int16_t)(x1 > BUF_COLS - 1 ? BUF_COLS - 1 : x1); d[3] = (int16_t)(y1 > (int)BUF_ROWS - 1 ? (int)BUF_ROWS - 1 : y1);
}
/* a render frame on the display buffer: region mode or the normal clear */
static void render_start(void *raster)
{
    void *only = 0;
    rg_active = 0;
    if (rg_scr && rg_n && BUF_ROWS <= 400 && (void *)SCREEN_TABLE[CUR_SCREEN] == rg_scr && visible_screens(&only) == 1 && only == rg_scr
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
 * A screen's SVG tree: the screen's word 0 = the root node; a node: +0x44 / +0x48 its children (vector of node
 * pointers), +0x50 visible, drawn in order (group draw 0x019EB818). The static part = the nodes before first_live in
 * that order (first_live 0: all). It is drawn once into `cache` (visibility switched so only it shows), then each
 * render frame copies the cache into the display buffer and draws the live nodes only. */
#define SCREEN_DRAW ((void (*)(void *, int))0x01A3B4C8)
static uint8_t *vis_node[256], vis_old[256];
static int vis_n;
static void vis_set(uint8_t *n, int on) { if (vis_n < 256) { vis_node[vis_n] = n; vis_old[vis_n++] = n[0x50]; } n[0x50] = (uint8_t)on; }
static void vis_restore(void) { while (vis_n) { vis_n--; vis_node[vis_n][0x50] = vis_old[vis_n]; } }
static int contains(uint8_t *n, const uint8_t *fl)
{
    if (n == fl) return 1;
    for (uint8_t **c = *(uint8_t ***)(n + 0x44), **e = *(uint8_t ***)(n + 0x48); c < e; c++) if (contains(*c, fl)) return 1;
    return 0;
}
static void hide_live(uint8_t *n, const uint8_t *fl, int *reached)   /* static pass: hide fl and all after it */
{
    if (*reached || n == fl) { *reached = 1; vis_set(n, 0); return; }
    for (uint8_t **c = *(uint8_t ***)(n + 0x44), **e = *(uint8_t ***)(n + 0x48); c < e; c++) hide_live(*c, fl, reached);
}
static void hide_static(uint8_t *n, const uint8_t *fl)               /* live pass: hide whatever lies before fl */
{
    for (uint8_t **c = *(uint8_t ***)(n + 0x44), **e = *(uint8_t ***)(n + 0x48); c < e; c++) {
        if (*c == fl) return;                    /* fl and everything after it stay */
        if (contains(*c, fl)) { hide_static(*c, fl); return; }
        vis_set(*c, 0);
    }
}
static void *dc_fl, *dc_raster;
static uint32_t dc_gen;
struct { uint32_t calls, builds, copies; } gs_dc_stat;
void op1_screen_draw_cached_fn(void *scr, int dt, void *first_live)
{
    uint8_t *root = scr ? *(uint8_t **)scr : 0;
#if defined(GS_ON) && !defined(GS_NOCACHE)
    if (root && !rg_active && dc_raster && tgt_buf && BUF_ROWS <= 400) {
        gs_dc_stat.calls++;
        if (!recording) {
            if (dc_scr != scr || dc_fl != first_live || dc_gen != gen) {   /* the static picture into the cache */
                int reached = 0;
                if (first_live) hide_live(root, first_live, &reached);
                no_hash = 1;
                RASTER_TARGET(dc_raster, cache); RASTER_CLEAR(dc_raster);
                SCREEN_DRAW(scr, dt);
                RASTER_TARGET(dc_raster, tgt_buf);
                no_hash = 0;
                vis_restore();
                dc_scr = scr; dc_fl = first_live; dc_gen = gen; cache_gen = 0;   /* the regions' cache is gone */
                gs_dc_stat.builds++;
            }
            copy_rows((uint8_t *)tgt_buf, cache, 0, (int)BUF_ROWS - 1, 0, BUF_COLS - 1);
            gs_dc_stat.copies++;
        }
        if (first_live) { hide_static(root, first_live); SCREEN_DRAW(scr, dt); vis_restore(); }   /* the live nodes */
        return;
    }
#endif
    SCREEN_DRAW(scr, dt);
}

static int slot_of(const uint32_t *b) { if (buf_id[0] == b) return 0; if (buf_id[1] == b) return 1; return -1; }
void gs_frame(void *raster)                    /* CALL Raster::clear in Screen::frame (0x01A3BBE0): a new frame */
{
    dc_raster = raster;
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
              && BUF_BYTES <= sizeof scratch;
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
