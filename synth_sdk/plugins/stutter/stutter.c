/* STUTTER: a beat-locked stutter. The OP-1's beat clock (op1_beat_clock, the metronome's) cuts time into slices;
 * at each slice a dice roll decides whether the slice plays through or stutters: its first part is played and
 * recorded, then repeated until the slice ends, so the repeats always land on the grid.
 *   blue   RATE     slice length: 1/4, 1/8, 1/8 triplet, 1/16, 1/16 triplet, 1/32 of a beat ... (6 steps)
 *   green  MODE     REPEAT (plain), BRAKE (each repeat slower and lower, a tape stop), REVERSE (repeats backwards),
 *                   GATE (no repeats: the slice is chopped into short bursts of the live sound)
 *   white  SHAPE    edge softness of each repeat (2 .. 12 ms fades); in GATE the open time (10 .. 90 %)
 *   red    CHANCE   how many slices stutter: none .. all
 * Page 2 / press:
 *   blue   word 5   REPEATS: how many parts a stuttering slice is cut into: 2, 3, 4, 6, 8, 16
 *   red    latch    FREEZE: keep repeating the part playing now, across slices, until pressed again
 * With no clock (tape stopped while it follows the tape, or no tempo yet) the grid runs on at 120 BPM.
 * Ring buffer: 65536 samples (1.49 s) of the position's FX scratch; a slice longer than that (1/4 under 40 BPM)
 * is cut short. While frozen the ring keeps the frozen part (nothing new is recorded). State per FX position 0..2. On the master the dispatcher feeds a mono sum and adds the
 * difference to both sides, so a stutter there repeats the mid signal over the live sides.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include "op1_plugin.h"

#define NBUF 65536
#define MASK (NBUF - 1)
#define HIST 16
enum { M_REPEAT, M_BRAKE, M_REVERSE, M_GATE };

static const uint8_t DIVS[6] = { 1, 2, 3, 4, 6, 8 };          /* slices per beat */
static const uint8_t REPS[6] = { 2, 3, 4, 6, 8, 16 };

typedef struct {
    uint32_t w;                     /* write index into the ring */
    int32_t bp;                     /* free-running beat phase (no clock), 1/16384 samples to the next beat */
    int32_t pos, len, sub;          /* sample in the slice, slice length, part length */
    int32_t ps, part;               /* sample in the part, part number (counted, no division per sample) */
    uint32_t start;                 /* ring index of the slice start (the recorded part) */
    uint8_t stut, mode, faded;      /* this slice stutters; its mode; the last slice ended faded out */
    int32_t fade, finc, duty;       /* fade samples, 32768 / fade, gate open samples per part */
    int32_t rd, rate;               /* BRAKE: read position in the part (Q16) and rate (Q16) */
    int32_t gin;                    /* fade-in after a stutter, samples left */
    uint8_t latch, frozen;
    int32_t rec;                    /* frozen: samples of the part still to record (then the ring is kept) */
    uint32_t rng;
    /* screen */
    int32_t slice;                  /* slice number in the beat */
    uint32_t hist;                  /* stutter flags of the last HIST slices, bit 0 = newest */
    uint8_t ndiv, reps;
} st_t;

static st_t st[3];
static volatile uint32_t sx_blocks[3];

/* Shared by audio, stepped rings and labels. */
static unsigned sx_range(int32_t q,unsigned count) { unsigned v=(unsigned)(q<0?0:q); unsigned n=v*count>>15; return n<count?n:count-1; }
static unsigned sx_select(int32_t q, unsigned count) { return ((unsigned)(q < 0 ? 0 : q) >> 9) % count; }
static inline int32_t q15(const int16_t *p, int i) { return op1_fx_param_q15(p, i); }
static inline uint32_t lcg(uint32_t *s) { return *s = *s * 1664525u + 1013904223u; }

#ifdef FX_SPREAD_INIT
#include "fx_spread.h"
static fx_spread_t sx_spread[3];
#endif
static void sx_init(int pos)
{
    pos %= 3;
    st_t *s = &st[pos];
    s->w = 0; s->bp = -1; s->pos = 0; s->len = 0; s->sub = 1; s->start = 0; s->stut = 0; s->faded = 0;
    s->gin = 0; s->latch = op1_fx_page[pos][3]; s->frozen = 0;
    s->rng = 0x2545F491u + (uint32_t)pos * 0x9E3779B9u;
    s->slice = 0; s->hist = 0;
    int16_t *buf = (int16_t *)op1_fx_scratch(pos);
#ifdef FX_SPREAD_INIT                                         /* cleared per block, dry meanwhile (fx_spread.h) */
    fx_spread_start(&sx_spread[pos], buf, NBUF / 2);
#else
    if (buf) for (int i = 0; i < NBUF / 2; i++) ((uint32_t *)(void *)buf)[i] = 0;   /* audio callback: 32-bit clear */
#endif
}

/* the next grid point in this block: its sample 0..63 or -1; *len = slice length (samples, real time) */
static int grid(st_t *s, int div, int32_t *slen)
{
    op1_beat_t c;
    op1_beat_clock(&c);
    int32_t len = c.len, p, step;
    const int moving = len >= (256 << 14) && c.step > 0 && c.step <= 0x400000 && c.next >= 0 && c.next <= len;
    if (len < (256 << 14)) len = 22050 << 14;                                /* no clock: 120 BPM */
    if (moving) { p = c.next; step = c.step; }
    else { p = s->bp < 0 || s->bp > len ? len : s->bp; step = 0x100000; }
    const int32_t np = p - step;
    s->bp = np < 0 ? np + len : np;
    const int32_t sq = step >> 14;
    const int32_t bl = sq > 0 && sq != 64 ? len / sq * 64 : len >> 14;      /* the beat in real time, samples */
    *slen = bl / div;
    const int32_t q = len / div;
    for (int j = div - 1; j >= 0; j--) {                                     /* the earliest point in [0, step) */
        const int32_t t = p - j * q;
        if (t < 0 || t >= step) continue;
        s->slice = j == 0 ? 0 : div - j;                                     /* slice that starts here (0 = on the beat) */
        return step == 0x100000 ? t >> 14 : (int)(((int64_t)t * 64) / step);
    }
    return -1;
}

static void slice_start(st_t *s, int32_t slen, const int16_t *p, int pos)
{
    const int32_t chance = q15(p, 3);
    const int32_t shape = q15(p, 2);
    const int32_t w5 = p[5] < 0 ? 0 : p[5];
    const int reps = REPS[sx_select(w5, 6)];
    const int frz = op1_fx_page[pos][3] != 0;
    if (slen > NBUF - 64) slen = NBUF - 64;
    if (slen < 64) slen = 64;
    s->len = slen;
    s->pos = 0;
    s->ps = 0;
    s->part = 0;
    s->mode = (uint8_t)(sx_range(q15(p, 1), 4));
    const uint8_t was = s->stut;
    if (frz) {
        s->stut = 1;
        if (!s->frozen) { s->frozen = 1; s->start = s->w; s->sub = slen / reps; s->rec = s->sub; }   /* freeze this slice's first part */
    } else {
        s->frozen = 0;
        s->stut = (uint8_t)((int32_t)((lcg(&s->rng) >> 17) & 0x7FFF) < chance);
        s->start = s->w;
        s->sub = slen / reps;
    }
    if (s->sub < 32) s->sub = 32;
    int32_t f = 88 + ((shape * 441) >> 15);                                 /* 2 .. 12 ms */
    if (f > s->sub / 4) f = s->sub / 4;
    s->fade = f < 1 ? 1 : f;
    s->finc = 32768 / s->fade;
    s->duty = (int32_t)(((int64_t)s->sub * (3277 + ((shape * 26214) >> 15))) >> 15);
    s->rd = 0;
    s->rate = 65536;
    s->gin = was && !s->stut ? s->fade : 0;                                   /* fade the dry sound back in */
    s->faded = was && s->stut && s->mode != M_GATE;
    s->hist = (s->hist << 1) | s->stut;
    s->reps = (uint8_t)reps;
}

/* samples [a, b) of the block */
static void run(st_t *s, int16_t *buf, int16_t *ring, int a, int b)
{
    for (int i = a; i < b; i++) {
        const int32_t x = buf[i];
        if (!s->frozen || s->rec > 0) { ring[s->w & MASK] = (int16_t)x; if (s->rec > 0) s->rec--; }   /* frozen: keep the part */
        int32_t y = x;
        if (s->stut && s->len) {
            const int32_t sub = s->sub, ps = s->ps, part = s->part, f = s->fade, fi = s->finc;
            int32_t e = 32768;                                          /* unity: the repeats are exact copies */
            if (s->mode == M_GATE) {
                if (ps >= s->duty) e = ps < s->duty + f ? 32768 - (ps - s->duty) * fi : 0;
                else if (part && ps < f) e = ps * fi;
            } else {
                if (part == 0 && !s->frozen) {                               /* the live part, recorded */
                    if (s->faded && ps < f) e = ps * fi;
                } else {
                    int32_t r;
                    if (ps == 0 && s->mode == M_BRAKE) {                    /* each repeat slower: -12 %, down to 30 % */
                        s->rate = 65536 - part * 7864;
                        if (s->rate < 19661) s->rate = 19661;
                        s->rd = 0;
                    }
                    if (s->mode == M_BRAKE) { r = s->rd >> 16; s->rd += s->rate; }
                    else if (s->mode == M_REVERSE) r = sub - 1 - ps;
                    else r = ps;
                    y = ring[(s->start + (uint32_t)r) & MASK];
                    if (ps < f) e = ps * fi;
                }
                if (ps >= sub - f) { const int32_t d = (sub - ps) * fi; if (d < e) e = d; }
            }
            y = (y * e) >> 15;
        } else if (s->gin > 0) {
            y = (y * (32768 - s->gin * s->finc)) >> 15;
            s->gin--;
        }
        buf[i] = (int16_t)y;
        s->w++;
        s->pos++;
        if (++s->ps >= s->sub) { s->ps = 0; s->part++; }
    }
}

static void sx_render(int pos, int16_t *buf, const int16_t *p)
{
    pos %= 3;
    st_t *s = &st[pos];
    int16_t *ring = (int16_t *)op1_fx_scratch(pos);
    if (!ring) return;
#ifdef FX_SPREAD_INIT
    if (fx_spread_step(&sx_spread[pos])) return;
#endif
    const int div = DIVS[sx_range(q15(p, 0), 6)];
    s->ndiv = (uint8_t)div;
    int32_t slen;
    const int off = grid(s, div, &slen);
    if (s->len == 0) slice_start(s, slen, p, pos);                           /* first block */
    if (off < 0) {
        run(s, buf, ring, 0, OP1_FX_BLOCK);
        if (s->pos >= s->len + s->len / 4) slice_start(s, slen, p, pos);    /* the clock jumped: start anew */
    } else {
        run(s, buf, ring, 0, off);
        slice_start(s, slen, p, pos);
        run(s, buf, ring, off, OP1_FX_BLOCK);
    }
    int32_t pk = 0;
    for (int i = 0; i < OP1_FX_BLOCK; i++) { const int32_t a = buf[i] < 0 ? -buf[i] : buf[i]; if (a > pk) pk = a; }
    volatile int16_t *m = op1_fx_meter(pos);
    const int32_t mv = (*m * 0xfe00) >> 16;
    *m = (int16_t)(pk > mv ? pk : mv);
    sx_blocks[pos]++;
}

const op1_fx_t FX_stutter = { sx_init, sx_render, { OP1_PRESS_PAGE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_LATCH } };

/* ======================= screen (GUI thread) =======================
 * stutter.svg and stutter_geom.h (OP1-Emulator docs/design/engine-screens/night/build_night2.py): a film projector
 * traced from the night concept, its reel a node (reel), and four knob readouts in the corners (label, dim ring
 * track; no slider bars, owner). Drawn here:
 *   the knob rings: value arc, pointer and knob face in the knob's colour, for RATE (blue, 6 steps), SHAPE (white),
 *     MODE (green, 4 steps) and CHANCE (red), as the effect hears them (op1_fx_knobs_live: LFO included); with the
 *     blue press (page 2) the RATE ring shows PARTS (2 .. 16, 6 steps); while a part is frozen the CHANCE ring gets a
 *     lit inner ring (FREEZE)
 *   the projected picture: the last 16 slices, newest at the right (white); a slice that stuttered is a tall bar
 *     (REPEAT full, BRAKE stepping down, REVERSE slanted, GATE dashed), one that played through a short one
 *   the reel turns a slice's share of a turn at each slice of the beat (its spokes tick with RATE's grid) */
#include "stutter_geom.h"
#pragma GCC push_options
#pragma GCC optimize("Os")                             /* GUI code: size, not speed */
#define QS(v) ((int32_t)((v) * 1024))
static const uint8_t S_BLUE[4] = { 0x69, 0x8E, 0xFF, 0xFF }, S_GREEN[4] = { 0x00, 0xED, 0x95, 0xFF },
                     S_WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, S_RED[4] = { 0xFF, 0x3A, 0x5D, 0xFF };
static void *sx_reel;
static int32_t sx_angle;                                     /* reel angle, 8192 a turn, eased toward the target */

static void sl(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    const op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}

/* point on a circle, angle a (8192 a turn, 0 = right, y down) */
static void on_circle(int32_t cx, int32_t cy, int32_t rad, int32_t a, int32_t *x, int32_t *y)
{
    *x = cx + ((rad * OP1_SINE_TABLE[(a + 2048) & 8191]) >> 15);
    *y = cy + ((rad * OP1_SINE_TABLE[a & 8191]) >> 15);
}

/* a knob ring: arc from 135 degrees (lower left) over the top to 45 degrees (lower right), 270 degrees for v 0..32767 */
static void ring(void *r, int32_t cx, int32_t cy, int32_t v, const uint8_t *c)
{
    const int32_t a0 = 3072, a1 = a0 + (int32_t)(((int64_t)v * 6144) >> 15);
    int32_t px, py, x, y;
    on_circle(cx, cy, SX_RING_R, a0, &px, &py);
    for (int32_t a = a0 + 256; a <= a1; a += 256) {
        on_circle(cx, cy, SX_RING_R, a, &x, &y);
        sl(r, px, py, x, y, 0x800, c);
        px = x; py = y;
    }
    on_circle(cx, cy, (SX_RING_R * 3) >> 2, 0, &px, &py);     /* the knob face */
    for (int32_t a = 512; a <= 8192; a += 512) {
        on_circle(cx, cy, (SX_RING_R * 3) >> 2, a, &x, &y);
        sl(r, px, py, x, y, 0x700, c);
        px = x; py = y;
    }
    on_circle(cx, cy, (SX_RING_R * 5) >> 3, a1, &x, &y);      /* the pointer */
    sl(r, cx, cy, x, y, 0x800, c);
}

static void sx_screen_init(void *scr) { sx_reel = op1_find_node(scr, "reel"); }

static void sx_screen_update(void *scr, int dt)
{
    const int pos = op1_fx_screen_pos() % 3;
    const st_t *s = &st[pos];
    int16_t kn[4];
    op1_fx_knobs_live(kn);
    int32_t k[4];
    for (int i = 0; i < 4; i++) k[i] = kn[i] < 0 ? 0 : kn[i];
    const int32_t w5 = op1_fx_knobs(pos)[5] < 0 ? 0 : op1_fx_knobs(pos)[5];
    const int ri = sx_range(k[0], 6), mi = sx_range(k[1], 4);

    /* the reel: a slice's share of a turn per slice, eased */
    const int div = DIVS[ri];
    const int32_t target = (s->slice % div) * (8192 / div);
    sx_angle += ((((target - sx_angle) + 4096) & 8191) - 4096) >> 2;
    op1_node_rotate(sx_reel, SX_REEL_X, SX_REEL_Y, OP1_SINE_TABLE[sx_angle & 8191], OP1_SINE_TABLE[(sx_angle + 2048) & 8191]);
    op1_screen_draw_cached(scr, dt, sx_reel);
    void *r = op1_raster();

    /* the rings (RATE / PARTS and MODE in steps); FREEZE as a lit inner ring on CHANCE */
    const int pi = sx_select(w5, 6);
    ring(r, SX_RING0_X, SX_RING0_Y, (op1_fx_page[pos][0] ? pi : ri) * 32767 / 5, S_BLUE);
    ring(r, SX_RING1_X, SX_RING1_Y, k[2], S_WHITE);
    ring(r, SX_RING2_X, SX_RING2_Y, mi * 32767 / 3, S_GREEN);
    ring(r, SX_RING3_X, SX_RING3_Y, k[3], S_RED);
    if (s->frozen) {
        int32_t px, py, x, y;
        on_circle(SX_RING3_X, SX_RING3_Y, SX_RING_R >> 1, 0, &px, &py);
        for (int32_t a = 1024; a <= 8192; a += 1024) {
            on_circle(SX_RING3_X, SX_RING3_Y, SX_RING_R >> 1, a, &x, &y);
            sl(r, px, py, x, y, 0x700, S_WHITE);
            px = x; py = y;
        }
    }

    /* the picture: 16 slices */
    const int32_t cw = (SX_PIC_X1 - SX_PIC_X0) / HIST, top = SX_PIC_Y0 + QS(3), bot = SX_PIC_Y1 - QS(2);
    for (int c = 0; c < HIST; c++) {
        const int on = (s->hist >> (HIST - 1 - c)) & 1;
        const uint8_t *col = c == HIST - 1 ? S_WHITE : S_GREEN;
        const int32_t x = SX_PIC_X0 + cw * c + cw / 2;
        if (!on) { sl(r, x, bot - QS(3), x, bot, 0x500, col); continue; }
        if (mi == M_GATE) {
            for (int32_t y = top; y < bot; y += QS(5)) sl(r, x, y, x, y + QS(2.5), 0x500, col);
        } else if (mi == M_REVERSE) {
            sl(r, x + QS(0.8), top, x - QS(0.8), bot, 0x500, col);
        } else if (mi == M_BRAKE) {
            sl(r, x, top + ((c & 3) * QS(5)), x, bot, 0x500, col);
        } else {
            sl(r, x, top, x, bot, 0x500, col);
        }
    }
}
#pragma GCC pop_options

const op1_screen_t SCREEN_stutter = { sx_screen_init, sx_screen_update };
