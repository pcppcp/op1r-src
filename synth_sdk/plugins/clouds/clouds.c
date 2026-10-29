/* "clouds" - granular texture FX, proof of concept: fixed-point port of Mutable Instruments Clouds,
 * GRANULAR mode only (verified against the original, harness/clouds_compare.py).
 *
 *   knob 0 (blue)  : position   where in the recorded buffer (~2.7 s) grains are taken from
 *   knob 1 (green) : size       grain size
 *   knob 2 (white) : density    < 50 %: regular grains, > 50 %: random grains; more grains away from 50 %
 *   knob 3 (red)   : texture    grain window shape; above 75 % adds diffusion
 * encoder press (page 2, params[5..7], saved with the patch):
 *   blue press  -> pitch      -24 .. +24 semitones (centre = 0)
 *   green press -> dry/wet
 *   white press -> reverb
 *   red press   -> FREEZE on/off (latch, not saved)
 * fixed: no feedback
 *
 * Memory: recording buffer + fx delay lines (CLOUDS_MEM_BYTES = 164 KB) live in this FX position's
 * scratch buffer (172 KB, shared with the built-in delay). OP-1 speed options: 24 grains (original 40),
 * linear grain interpolation, idle diffuser/reverb/feedback skipped - CPU budget to be measured on the device.
 * Original code (c) 2014 Emilie Gillet, MIT license (third_party/eurorack/LICENSE.clouds). */
#define CLOUDS_SR         44100
#define CLOUDS_MAX_GRAINS 24
#define CLOUDS_LINEAR_READ
#define CLOUDS_SKIP_IDLE
#include "clouds_dsp.c"
#include "op1_plugin.h"

static clouds_t cl[3];                                          /* synth, drum, master */

static void clouds_fx_init(int pos)
{
    void *mem = op1_fx_scratch(pos);
    if (mem) clouds_init(&cl[pos % 3], mem, CLOUDS_GRANULAR);
}

static void clouds_fx_render(int pos, int16_t *buf, const int16_t *p)
{
    clouds_t *c = &cl[pos % 3];
    if (!c->s16) return;                                            /* no scratch buffer */
    clouds_params_t prm = {
        .position = op1_fx_param_q15(p, 0), .size = op1_fx_param_q15(p, 1),
        .density = op1_fx_param_q15(p, 2), .texture = op1_fx_param_q15(p, 3),
        .pitch = ((int32_t)p[5] - 16384) * 3 / 8,                    /* +-24 semitones, *256 */
        .dry_wet = p[6], .reverb = p[7], .feedback = 0,
        .freeze = op1_fx_page[pos % 3][3], .trigger = 0 };
    int16_t out[OP1_FX_BLOCK];
#ifdef CLOUDS_KERN
    /* clouds_dsp.S leaves L0..L3 at 0 (linear addressing); the caller's values are put back on return */
    uint32_t l0, l1, l2, l3;
    __asm__ volatile("%0 = L0;\n\t%1 = L1;\n\t%2 = L2;\n\t%3 = L3;" : "=d"(l0), "=d"(l1), "=d"(l2), "=d"(l3));
    clouds_process(c, &prm, buf, out, OP1_FX_BLOCK);
    __asm__ volatile("L0 = %0;\n\tL1 = %1;\n\tL2 = %2;\n\tL3 = %3;" : : "d"(l0), "d"(l1), "d"(l2), "d"(l3));
#else
    clouds_process(c, &prm, buf, out, OP1_FX_BLOCK);
#endif
    int32_t peak = 0;
    for (int i = 0; i < OP1_FX_BLOCK; i++) {
        buf[i] = out[i];
        int32_t a = out[i] < 0 ? -out[i] : out[i];
        if (a > peak) peak = a;
    }
    volatile int16_t *m = op1_fx_meter(pos);
    int32_t mv = (*m * 0xfe00) >> 16;
    *m = (int16_t)(peak > mv ? peak : mv);
}

const op1_fx_t FX_clouds = { clouds_fx_init, clouds_fx_render,
                              { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_LATCH } };

/* ======================= screen (GUI thread) =======================
 * display/cloudsfx.svg (display/gen_clouds_svg.py, geometry in clouds_geom.h): recording buffer line (write
 * head left, older audio to the right: POSITION up = marker right), live grains as dots at their read position (size = envelope), blue bracket = position
 * window, dials show the active page (page 2: ring around the dial, needle = page-2 value), freeze: buffer
 * lit, head marker grey. Grains are single vertical ticks (one raster line each) to keep the GUI cheap. */
#include "clouds_geom.h"
static void *cbars[4];
static const uint8_t COL_GRAIN[4] = { 0xDF, 0xD9, 0xFF, 0xFF };
static const uint8_t COL_FROZEN[4] = { 0xBF, 0xD8, 0xFF, 0xFF };          /* stock lightblue */
static const uint8_t COL_HEAD[4] = { 0xFF, 0x3A, 0x5D, 0xFF };
static const uint8_t COL_DIM[4] = { 0x6F, 0x71, 0x73, 0xFF };

static inline int32_t clsin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }      /* table[k] = sin(2 pi k/8192) */
static void cline(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
static void ccircle(void *r, int32_t cx, int32_t cy, int32_t rad, const uint8_t *c)   /* Q10 */
{
    op1_pt p = { cx + rad, cy };
    for (int a = 512; a <= 8192; a += 512) {
        op1_pt q = { cx + (rad * clsin(a + 2048) >> 15), cy + (rad * clsin(a) >> 15) };
        op1_raster_line(r, &p, &q, 0x500, c);
        p = q;
    }
}

static void clouds_screen_init(void *scr)
{
    static const char *ids[4] = { "bar0", "bar1", "bar2", "bar3" };
    for (int i = 0; i < 4; i++) cbars[i] = op1_find_node(scr, ids[i]);
}

static void clouds_screen_update(void *scr, int dt)
{
    int pos = op1_fx_screen_pos();
    const int16_t *w = op1_fx_knobs(pos);
    int16_t k[4];
    op1_fx_knobs_live(k);
    int32_t val[4];
    for (int i = 0; i < 4; i++)
        val[i] = (i < 3 && op1_fx_page[pos][i]) ? w[5 + i] : k[i];
    for (int i = 0; i < 4; i++) if (cbars[i]) {
        int32_t a = (val[i] * 6144 >> 15) - 3072;
        op1_node_rotate(cbars[i], OP1_Q10(cs_dial_x[i]), OP1_Q10(CS_DIAL_Y), clsin(a), clsin(a + 2048));
    }
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    int frozen = op1_fx_page[pos][3];
    int32_t x0 = OP1_Q10(CS_BUF_X0), x1 = OP1_Q10(CS_BUF_X1), y = OP1_Q10(CS_BUF_Y);

    /* page-2 rings, freeze ring on red */
    for (int i = 0; i < 4; i++)
        if (op1_fx_page[pos][i]) ccircle(r, OP1_Q10(cs_dial_x[i]), OP1_Q10(CS_DIAL_Y), OP1_Q10(17), cs_dial_col[i]);
    /* buffer: lit when frozen; write head marker */
    if (frozen) cline(r, x0, y, x1, y, 0x600, COL_FROZEN);
    cline(r, x0, y - OP1_Q10(7), x0, y + OP1_Q10(7), 0x800, frozen ? COL_DIM : COL_HEAD);   /* write head */
    /* position window (blue): where new grains start (original: start = head - position * available) */
    int32_t px = x0 + (int32_t)(((int64_t)(x1 - x0) * k[0]) >> 15);
    cline(r, px, y - OP1_Q10(16), px, y - OP1_Q10(10), 0x600, cs_dial_col[0]);
    cline(r, px - OP1_Q10(6), y - OP1_Q10(16), px, y - OP1_Q10(16), 0x600, cs_dial_col[0]);

    /* live grains: a vertical tick at the read position (relative to the write head), height = envelope */
    clouds_t *c = &cl[pos % 3];
    if (!c->s16 || c->size <= 0) return;
    for (int i = 0; i < CLOUDS_MAX_GRAINS; i++) {
        const clouds_grain_t *g = &c->grains[i];
        if (!g->active) continue;
        int32_t idx = g->first_sample + (g->phase >> 16);
        int32_t age = c->head - idx; while (age < 0) age += c->size; while (age >= c->size) age -= c->size;
        int32_t gx = x0 + (int32_t)(((int64_t)(x1 - x0) * age) / c->size);
        uint32_t ph = g->env_phase, tri = ph >= 0x40000000u ? 0x80000000u - ph : ph;   /* Q30 */
        int32_t amp = (int32_t)(tri >> 15);                                           /* Q15 */
        int32_t h = OP1_Q10(2) + (amp * 14 >> 5);                                     /* 2 .. 16 px */
        cline(r, gx, y - h, gx, y + h, 0x600, COL_GRAIN);
    }
}

const op1_screen_t SCREEN_cloudsfx = { clouds_screen_init, clouds_screen_update };
