/* GUI dirty-region measurement (docs/74): a dev-only prototype, hooked only in builds with OP1R_DIRTYPROF=1.
 * Every shape the rasteriser draws (move_to / line_to points, then the sweep with its colour) gets a hash of its input
 * and a bounding box. At the end of each frame the shapes are folded per 16 px tile, in drawing order; a tile is dirty
 * when its hash differs from the same frame buffer's previous frame (two buffers in turn: two frames back). Counted:
 * shapes, shapes touching a dirty tile (what a replay would redraw), dirty tiles. Read with peek at dp_stat.
 * Not counted: text glyphs (their own path, docs/74). */
#include "../op1_synth.h"

#define DP_MAX_SHAPES 2048
#define TW 14                                   /* tiles of 16 px in device coordinates: the panel is portrait */
#define TH 26                                   /* (x 0..~200, y 0..~400, measured shape boxes) */
typedef struct { uint32_t h; int16_t x0, y0, x1, y1; uint32_t cells; } dp_shape_t;
dp_shape_t sh[DP_MAX_SHAPES];
static int nsh;
static uint32_t cur_h;                          /* the open shape */
static int32_t bx0, by0, bx1, by1, npts;
static uint32_t tile_prev[2][TW * TH], tile_now[TW * TH];
static uint8_t tile_dirty[TW * TH];
static uint32_t parity;
struct {
    uint32_t frames, shapes, replay, tiles_dirty, frames_clean, overflow, max_shapes, empty;
    uint32_t moves, lines, sweeps, nopix, cells, cells_replay;
} dp_stat;

static void point(int32_t x, int32_t y)
{
    cur_h = (cur_h ^ (uint32_t)x) * 0x01000193u;
    cur_h = (cur_h ^ (uint32_t)y) * 0x01000193u;
    if (!npts++) { bx0 = bx1 = x; by0 = by1 = y; return; }
    if (x < bx0) bx0 = x; if (x > bx1) bx1 = x;
    if (y < by0) by0 = y; if (y > by1) by1 = y;
}
void dp_move(void *ras, int x, int y) { (void)ras; dp_stat.moves++; point((int16_t)x, (int16_t)y); }
void dp_line(void *ras, int x, int y) { (void)ras; dp_stat.lines++; point((int16_t)x, (int16_t)y); }
void dp_shape(const uint8_t *ras, const uint8_t *bgra)   /* sweep: the shape is complete */
{
    dp_stat.sweeps++;
    if (!npts) dp_stat.empty++;                  /* cells kept from an earlier frame (SVG shapes) */
    const int16_t *b = (const int16_t *)(ras + 0x30);             /* x min / y min / x max / y max, px (to check) */
    uint32_t h = cur_h;
    h = (h ^ (uint32_t)(uintptr_t)ras) * 0x01000193u;
    h = (h ^ *(const uint32_t *)(ras + 0xC)) * 0x01000193u;       /* cell count */
    for (int i = 0; i < 4; i++) h = (h ^ (uint16_t)b[i]) * 0x01000193u;
    h = (h ^ ((uint32_t)bgra[0] | bgra[1] << 8 | bgra[2] << 16 | (uint32_t)bgra[3] << 24)) * 0x01000193u;
    if (b[0] > b[2] || b[1] > b[3]) dp_stat.nopix++;              /* no cells: draws nothing */
    else if (nsh < DP_MAX_SHAPES) {
        dp_shape_t *s = &sh[nsh++];
        s->h = h;
        s->x0 = b[0]; s->y0 = b[1]; s->x1 = b[2]; s->y1 = b[3];
        s->cells = *(const uint32_t *)(ras + 0xC);
    } else dp_stat.overflow++;
    cur_h = 0x811C9DC5u; npts = 0;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
#define TILES(s, a, b, c, d) int a = clampi((s)->x0 >> 4, 0, TW - 1), b = clampi((s)->x1 >> 4, 0, TW - 1), \
                                 c = clampi((s)->y0 >> 4, 0, TH - 1), d = clampi((s)->y1 >> 4, 0, TH - 1)
static void frame_end(void)
{
    for (int t = 0; t < TW * TH; t++) tile_now[t] = 0x811C9DC5u;
    for (int i = 0; i < nsh; i++) {
        TILES(&sh[i], tx0, tx1, ty0, ty1);
        for (int ty = ty0; ty <= ty1; ty++)
            for (int tx = tx0; tx <= tx1; tx++) { uint32_t *p = &tile_now[ty * TW + tx]; *p = (*p ^ sh[i].h) * 0x01000193u; }
    }
    uint32_t *prev = tile_prev[parity & 1];
    int nd = 0;
    for (int t = 0; t < TW * TH; t++) { tile_dirty[t] = tile_now[t] != prev[t]; nd += tile_dirty[t]; prev[t] = tile_now[t]; }
    int rep = 0;
    if (nd)
        for (int i = 0; i < nsh; i++) {
            TILES(&sh[i], tx0, tx1, ty0, ty1);
            int hit = 0;
            for (int ty = ty0; ty <= ty1 && !hit; ty++)
                for (int tx = tx0; tx <= tx1 && !hit; tx++) hit = tile_dirty[ty * TW + tx];
            rep += hit;
            if (hit) dp_stat.cells_replay += sh[i].cells;
        }
    for (int i = 0; i < nsh; i++) dp_stat.cells += sh[i].cells;
    dp_stat.frames++; dp_stat.shapes += (uint32_t)nsh; dp_stat.replay += (uint32_t)rep; dp_stat.tiles_dirty += (uint32_t)nd;
    dp_stat.frames_clean += !nd;
    if ((uint32_t)nsh > dp_stat.max_shapes) dp_stat.max_shapes = (uint32_t)nsh;
    parity++; nsh = 0; cur_h = 0x811C9DC5u; npts = 0;
}
#ifndef OP1_HOST
void dp_frame(void *raster)                      /* CALL Raster::clear in Screen::frame (0x01A3BBE0): a new frame */
{
    frame_end();
    ((void (*)(void *))0x01A23466)(raster);
}
#endif
