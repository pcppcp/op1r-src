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
#define FNV(h, v) do { uint32_t v_ = (uint32_t)(v); if (logn < 1024) gs_log[recording][logn++] = v_; (h) = ((h) ^ v_) * 0x01000193u; } while (0)
static uint32_t fh = 0x811C9DC5u;               /* the frame being drawn */

static uint32_t hash_buf[2], last;              /* what each display buffer shows; the newest frame */
static uint32_t *cur_buf, *prev_buf, *buf_id[2];
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
#define BUF_BYTES     ((*(volatile uint32_t *)0xFF8017B4) << 9)

static int main_ras(const void *ras) { return ras == RASTER + 0x30; }

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

static int slot_of(const uint32_t *b) { if (buf_id[0] == b) return 0; if (buf_id[1] == b) return 1; return -1; }
void gs_frame(void *raster)                    /* CALL Raster::clear in Screen::frame (0x01A3BBE0): a new frame */
{
    /* the frame just drawn */
    uint32_t h = fh; fh = 0x811C9DC5u;
#ifdef GS_VERIFY
    if (verifying) {                            /* the check render: compare with the shown buffer */
        verifying = 0; prev_buf = 0;            /* not a display frame */
        if (h == last) {
            const uint8_t *x = scratch, *y = (const uint8_t *)vbuf;
            uint32_t n = BUF_BYTES, bad = 0, first = 0xFFFFFFFF;
            for (uint32_t i = 0; i < n; i++)    /* visible pixels only: device x 0..199 of each 256-pixel row */
                if ((i & 511) < 400 && x[i] != y[i]) { if (!bad) first = i; bad++; }
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
        verifying = 1; vbuf = cur_buf; recording = 0; prev_rec = 0;
        RASTER_TARGET(raster, scratch); RASTER_CLEAR(raster);
        return;
    }
#endif
    if (rec) { gs_stat.records++; RASTER_TARGET(raster, scratch); }
    else { gs_stat.renders++; RASTER_CLEAR(raster); }
}
#endif
