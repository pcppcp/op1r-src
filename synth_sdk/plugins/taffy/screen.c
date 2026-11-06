/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* TAFFY's screen after the owner's mockup A, "bubblegum" (gen_svg.py: the fit, the measurements, taffy.svg and
 * taffy_geom.h): a girl in profile blows a bubblegum bubble. The SVG is all static (the girl, clouds, stars, dots,
 * lips) and drawn through the static-prefix cache (taffy_plugin.c); everything below is drawn live on top. Each
 * knob owns the part in its own colour (taffy-motion, 2026-10-28):
 *   bubble     blue, WAVE: the outline is the wave (taffy_wave_cycle, each CZ shape alone at the drawing bend, mean
 *              removed, peak to peak 2) wrapped around the circle its own number of times: saw 3 leaning lobes,
 *              square the mockup's 6 (flat shoulders), pulse 12 narrow notches, saw-pulse 6 (half held). Between two
 *              shapes the outline mixes them as the voices do; the counts are harmonics (3/6, 6/12), so the morph stays
 *              symmetric. The drawing bend is the voices' distortion mapped onto 60 .. 80 % of the CZ bend, so the
 *              shapes read from DISTORTION 0. WAVE and the distortion are eased (a quarter a frame).
 *   depth      green, DISTORTION: the lobes' depth, 4 % of the radius at 0 to 17 % at full (the voices' distortion:
 *              the knob plus the ENVELOPE knob's share of the bend envelope); scaled down rather than clipped at
 *              R_MAX. The strands' tension: slack and sagging at 0, the mockup's bows at 25 %, straight at full.
 *   size       the loudest sounding voice's stock ADSR level x velocity in octaves (fast up, slow down): 33.2 px
 *              silent, the mockup's 50.7 at a held note of velocity 100; it moves right as it grows, so the strands
 *              stretch. While notes sound the lobes turn slowly.
 *   sparkles   white, ENVELOPE: at rest the knob lengthens the white sparkles (to twice at full, each stopping at
 *              rows 4.5 / 136) and brings out four more; each note's bend envelope (as the voices hear it,
 *              ~0.19 s) swells the bubble up to 4.4 px, deepens the lobes and shoots the sparkles out.
 *   highlights red, RESONANCE: glossy arcs (and two white glints) on a round path inside the outline (0.8 of the
 *              radius, 4 px inside the deepest notch with the ripple); none at 0, the mockup's set at 25 %, more and longer toward
 *              full; above 25 % they ripple (to 3.6 px), at the CZ resonance's ratio (it follows the distortion),
 *              moving while notes sound.
 *   strands    green gum from the lips to the outline, ending on the live outline.
 * Every value is integer; two taffy_wave_cycle setups per change of the shape pair or the drawing bend. */
#ifndef ENGINE_HOST_TEST
#include "hk/engine.h"
#include "taffy_geom.h"

void taffy_wave_cycle(int16_t *y, int n, int32_t k0, int32_t dcw);
extern volatile uint32_t taffy_blocks, taffy_vblk[ENGINE_VOICES];
extern volatile int16_t taffy_venv[ENGINE_VOICES], taffy_vlev[ENGINE_VOICES];
extern volatile int16_t taffy_knobs[4];

#define NW 32                      /* points of each computed cycle */
#define N 132                      /* outline points: 11 per cycle of the twelve-lobed pulse */
#define R_SIL TF_RSIL              /* silent radius: 33.2 px */
#define R_LOUD TF_RLOUD            /* 51.8 px: the mockup's mean radius (50.7) at a held note of velocity 100 (level 0.94) */
#define R_MAX TF_RMAX              /* outline points: 62 px, keeps y 7.7 .. 133.2 with the stroke */
/* the bend the outline is drawn with: the voices' distortion mapped onto 60 .. 80 % of the CZ bend, so each shape's
   character (the saw's lean, the square's flat shoulders, the pulse's narrow notch) shows from DISTORTION 0; 80 %
   keeps the pulse's notch 2.5 outline points wide */
#define DV0 19661
#define DV1 26214
/* WAVE: each CZ shape wraps around the bubble its own number of times, so the shape reads at any distortion: saw
   three leaning lobes, square the mockup's six (flat-shouldered), pulse twelve narrow notches, saw-pulse six again
   (half held). Neighbours are harmonics (3/6, 6/12, 12/6), so the morph between two shapes keeps the outline
   symmetric instead of beating. The shape's own k0 (taffy.c setup(): shape = k0 x 3 >> 15) */
static const uint8_t lobes[4] = {3, 6, 12, 6};
static const int16_t shape_k0[4] = {0, 10923, 21846, 32767};

static const uint8_t blue[4] = {0x69, 0x8E, 0xFF, 0xFF}, green[4] = {0x00, 0xED, 0x95, 0xFF};
static const uint8_t red[4] = {0xFF, 0x3A, 0x5D, 0xFF}, white[4] = {0xFF, 0xFF, 0xFF, 0xFF};

static int16_t wave[NW];
static int32_t wn[2][NW];                    /* the two neighbouring shapes' cycles, mean removed, peak to peak 65536 */
static int wave_sa = -1;
static int32_t wave_dv = -1;
static int32_t rad[N];                       /* outline radius per point, Q10 */
static int32_t hl_r, amp;                    /* the highlights' radius (inside the outline's deepest point), ripple */
static int32_t e_lev, k[4], kw, kd, dcw, flash, size, bx, rot, ripple;
static int playing;
static uint32_t last_blocks;

void engine_screen_load(void *screen)
{
    (void)screen;
    wave_sa = -1;
    kw = kd = -1;
}

/* log2 of x in Q8 (x > 0): the integer part from the leading bit, then 8 mantissa bits */
static int32_t log2q8(uint32_t x)
{
    const int b = 31 - __builtin_clz(x);
    const uint32_t m = b >= 8 ? x >> (b - 8) : x << (8 - b);
    return b * 256 + (int32_t)(m & 255);
}

static int32_t clamp15(int32_t v) { return v < 0 ? 0 : v > 32767 ? 32767 : v; }

/* a knob shown eased: a quarter of the way per frame (~0.1 s), so a turn morphs rather than jumps */
static int32_t ease(int32_t cur, int32_t to)
{
    if (cur < 0) return to;
    const int32_t d = to - cur;
    return d > -64 && d < 64 ? to : cur + (d >> 2);
}

void engine_screen_animate(void *screen)
{
    (void)screen;
    /* notes sound while taffy_blocks counts (it stops with the last voice); the sounding voices are the ones
       rendered within the last few voice blocks */
    const uint32_t blocks = taffy_blocks;
    int32_t env = 0, lev = 0;
    playing = blocks != last_blocks;
    last_blocks = blocks;
    for (int i = 0; playing && i < ENGINE_VOICES; i++) {
        const uint32_t b = taffy_vblk[i];
        if (!b || blocks - b >= 3 * ENGINE_VOICES) continue;      /* 0: never rendered */
        if (taffy_venv[i] > env) env = taffy_venv[i];
        if (taffy_vlev[i] > lev) lev = taffy_vlev[i];
    }
    for (int i = 0; i < 4; i++) k[i] = screen_knob_live(taffy_knobs, i, playing);
    kw = ease(kw, k[0]);

    /* level in octaves: 0 at -36 dB (2^9), 1 at full scale; fast up (half a frame), slow down (~0.5 s) */
    const int32_t t = lev > 0 ? clamp15((log2q8((uint32_t)lev) - 9 * 256) * 32767 / (6 * 256)) : 0;
    e_lev += ((t - e_lev) * (t > e_lev ? 16384 : 1092)) >> 15;
    if (t < e_lev && e_lev - t < 32) e_lev = t;

    /* the distortion as the voices hear it: the knob plus the envelope knob's share of the bend envelope */
    dcw = playing ? k[1] + ((k[2] * env) >> 15) : k[1];
    if (dcw > 32767) dcw = 32767;
    kd = ease(kd, dcw);
    /* the kick: the bend envelope, mostly with the envelope knob (a little on every note) */
    flash = playing ? (env * (9830 + ((k[2] * 22937) >> 15))) >> 15 : 0;
    size = R_SIL + (int32_t)(((int64_t)(R_LOUD - R_SIL) * e_lev) >> 15)
         + (int32_t)(((int64_t)(TF_KICK0 + ((k[2] * TF_KICK_ADD) >> 15)) * env) >> 15);
    bx = TF_BX + (((size - TF_BR) * 5) >> 2);                /* grows away from the lips: the strands stretch */
    if (playing) {
        rot += 2 + ((e_lev * 10) >> 15);                     /* up to ~30 degrees a second at 60 frames */
        ripple += 160;
    }
    const int32_t dv = DV0 + (((DV1 - DV0) * kd) >> 15);
    const int32_t pos = kw * 3, frac = pos & 0x7FFF;          /* as taffy.c: the shape pair and the mix */
    const int sa = pos >> 15, sb = sa + (sa < 3);
    if (sa != wave_sa || dv != wave_dv) {
        wave_sa = sa;
        wave_dv = dv;
        /* each shape alone at the drawing bend, mean removed and peak to peak set to 2: the size alone is the
           level's, the depth alone DISTORTION's */
        for (int s = 0; s < 2; s++) {
            taffy_wave_cycle(wave, NW, shape_k0[s ? sb : sa], dv);
            int32_t sum = 0, lo = 32767, hi = -32768;
            for (int i = 0; i < NW; i++) {
                sum += wave[i];
                if (wave[i] < lo) lo = wave[i];
                if (wave[i] > hi) hi = wave[i];
            }
            const int32_t mean = sum / NW, pp = hi - lo > 256 ? hi - lo : 256;
            for (int i = 0; i < NW; i++) wn[s][i] = (int32_t)(((int64_t)(wave[i] - mean) << 16) / pp);
        }
    }
    /* the outline: the two shapes, each at its own lobe count, mixed as the voices mix them */
    int32_t hi = 0;
    for (int i = 0; i < N; i++) {
        int32_t w2[2];
        for (int s = 0; s < 2; s++) {
            const int32_t m = (i * lobes[s ? sb : sa]) % N;
            const int32_t u = (m * 4067203) >> 16;           /* m / N of a cycle, Q8 of the NW points */
            const int j = u >> 8, j1 = j + 1 == NW ? 0 : j + 1;
            w2[s] = wn[s][j] + (((wn[s][j1] - wn[s][j]) * (u & 255)) >> 8);
        }
        rad[i] = w2[0] + (int32_t)(((int64_t)(w2[1] - w2[0]) * frac) >> 15);
        if (rad[i] > hi) hi = rad[i];
    }
    /* depth: 4 % of the radius at DISTORTION 0, 17 % at full (the voices' distortion, eased); never past R_MAX:
       the lobes scale down there rather than flatten */
    int32_t depth = (int32_t)(((int64_t)size * (41 + ((kd * 133) >> 15))) >> 10);
    if (hi > 0 && size + (int32_t)(((int64_t)depth * hi) >> 15) > R_MAX)
        depth = (int32_t)(((int64_t)(R_MAX - size) << 15) / hi);
    if (depth < 0) depth = 0;
    int32_t lo = R_MAX;
    for (int i = 0; i < N; i++) {
        rad[i] = size + (int32_t)(((int64_t)depth * rad[i]) >> 15);
        if (rad[i] < lo) lo = rad[i];
    }
    /* the highlights stay round (the outline is WAVE's and DISTORTION's): at the mockup's share of the radius, and
       4 px inside the deepest notch */
    amp = k[3] > 8192 ? (int32_t)(((int64_t)(k[3] - 8192) * 2 * TF_RIPPLE) / 24575) : 0;     /* to 3.6 px */
    hl_r = (int32_t)(((int64_t)size * TF_HL_INSET) >> 10);
    if (hl_r > lo - 4096 - amp) hl_r = lo - 4096 - amp;
}

/* a line, clipped to rows 3.7..137 (ink 3.05..137.65 with the 1.3 px strokes, inside y 3..138: sparkles reach out
   furthest on a loud kick, the bottom row is the OLED's lower target) */
static void line(void *canvas, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    enum { TOP = 3 * 1024 + 717, BOT = 137 * 1024 };
    if ((y0 < TOP && y1 < TOP) || (y0 > BOT && y1 > BOT)) return;
    if (y0 < TOP || y0 > BOT) { const int32_t e = y0 < TOP ? TOP : BOT; x0 += (int32_t)((int64_t)(x1 - x0) * (e - y0) / (y1 - y0)); y0 = e; }
    if (y1 < TOP || y1 > BOT) { const int32_t e = y1 < TOP ? TOP : BOT; x1 += (int32_t)((int64_t)(x0 - x1) * (e - y1) / (y0 - y1)); y1 = e; }
    const svg_pt a = {x0, y0}, b = {x1, y1};
    SVG_LINE(canvas, &a, &b, w, c);
}

static int32_t sn(uint32_t a) { return SVG_SIN(a); }
static int32_t cs(uint32_t a) { return SVG_SIN(a + 2048); }

/* the outline's radius at angle a (8192 a turn) */
static int32_t radius_at(uint32_t a)
{
    const uint32_t u = ((a - (uint32_t)TF_LOBE0 - (uint32_t)rot) & 8191) * N;
    const int i = (int)(u >> 13), j = i + 1 == N ? 0 : i + 1;
    const int32_t f = (int32_t)(u & 8191);
    return rad[i] + (int32_t)(((int64_t)(rad[j] - rad[i]) * f) >> 13);
}

static void polar(uint32_t a, int32_t r, int32_t *x, int32_t *y)
{
    *x = bx + (int32_t)(((int64_t)r * cs(a)) >> 15);
    *y = TF_BY + (int32_t)(((int64_t)r * sn(a)) >> 15);
}

/* quadratic curve p0 -> p2 bowed by bow (Q10 of the width, + up), 8 segments; returns the point at t (Q10) */
static void strand(void *canvas, int32_t x0, int32_t y0, int32_t x2, int32_t y2, int32_t bow, int32_t tq,
                   int32_t *tx, int32_t *ty)
{
    const int32_t x1 = (x0 + x2) >> 1, y1 = ((y0 + y2) >> 1) - (int32_t)(((int64_t)(x2 - x0) * bow) >> 10);
    int32_t px = x0, py = y0;
    for (int i = 1; i <= 8; i++) {
        const int32_t u = i * 128, v = 1024 - u;            /* Q10 */
        const int32_t x = (int32_t)(((int64_t)v * v * x0 + 2LL * u * v * x1 + (int64_t)u * u * x2) >> 20);
        const int32_t y = (int32_t)(((int64_t)v * v * y0 + 2LL * u * v * y1 + (int64_t)u * u * y2) >> 20);
        line(canvas, px, py, x, y, 1331, green);
        px = x;
        py = y;
    }
    const int32_t u = tq, v = 1024 - tq;
    *tx = (int32_t)(((int64_t)v * v * x0 + 2LL * u * v * x1 + (int64_t)u * u * x2) >> 20);
    *ty = (int32_t)(((int64_t)v * v * y0 + 2LL * u * v * y1 + (int64_t)u * u * y2) >> 20);
}

/* the strands' tension follows DISTORTION: the mockup's bows at the default 25 %, slack below (sagging down by up to
   0.4 of the strand's width at 0), pulled straight toward full */
static int32_t bow(int32_t b)
{
    const int32_t g = k[1];
    if (g < 8192) return b - (((8192 - g) * 410) >> 13);
    return (int32_t)(((int64_t)b * (32767 - g)) / 24575);
}

/* a radial dash around the bubble at angle a: from the mean radius + r0 to + r1, longer with the ENVELOPE knob at
   rest (to twice as long at full), reaching out and longer still with the kick */
static void dash(void *canvas, uint32_t a, int32_t r0, int32_t r1)
{
    const int32_t push = (flash * TF_PUSH) >> 15;            /* up to 3.3 px */
    const int32_t i = size + r0 + push;
    int32_t o = i + (int32_t)(((int64_t)(r1 - r0) * (32768 + k[2] + ((flash * 3) >> 2))) >> 15);
    /* ends inside rows 4.5 .. 136 (ink 3.85 .. 136.65): a sparkle near the top or bottom stops growing there
       rather than being cut flat by line()'s clip */
    const int32_t sv = sn(a), room = sv > 0 ? 136 * 1024 - TF_BY : sv < 0 ? TF_BY - 4608 : 0;
    if (sv) {
        const int32_t lim = (int32_t)(((int64_t)room << 15) / (sv > 0 ? sv : -sv));
        if (o > lim) o = lim;
        if (i >= o) return;
    }
    int32_t x0, y0, x1, y1;
    polar(a, i, &x0, &y0);
    polar(a, o, &x1, &y1);
    line(canvas, x0, y0, x1, y1, 1331, white);
}

void engine_screen_draw(void *screen, void *canvas)
{
    (void)screen;
    if (!canvas) return;

    /* strands first: the outline covers their ends */
    int32_t sx[3], sy[3];
    for (int s = 0; s < 5; s++) {
        const int32_t *d = tf_strand[s];
        const uint32_t a = (uint32_t)d[2];
        int32_t ex, ey, x0, y0, tx, ty;
        polar(a, radius_at(a), &ex, &ey);
        if (d[0] < 0) {
            x0 = TF_MX + (int32_t)(((int64_t)(ex - TF_MX) * d[3]) >> 10);
            y0 = TF_MY + (int32_t)(((int64_t)(ey - TF_MY) * d[3]) >> 10);
        } else {
            x0 = sx[d[0]];
            y0 = sy[d[0]];
        }
        /* the branches start along their parent strand: its point at t, computed when it was drawn */
        const int32_t tq = s == 0 ? tf_strand[3][1] : s == 2 ? tf_strand[4][1] : 0;
        strand(canvas, x0, y0, ex, ey, bow(d[4]), tq, &tx, &ty);
        if (s < 3) {
            sx[s] = tx;
            sy[s] = ty;
        }
    }

    /* the bubble */
    {
        int32_t px = 0, py = 0;
        for (int i = 0; i <= N; i++) {
            const int j = i == N ? 0 : i;
            const uint32_t a = (uint32_t)TF_LOBE0 + (uint32_t)rot + (uint32_t)((j * 8192 + N / 2) / N);
            int32_t x, y;
            polar(a, rad[j], &x, &y);
            if (i) line(canvas, px, py, x, y, 1536, blue);
            px = x;
            py = y;
        }
    }

    /* highlights: the mockup's set at the default resonance (25 %), from a short arc at 0 to longer ones and two
       more at full; above half they ripple at the resonance's ratio to the pitch (1 .. 16, with the distortion) */
    {
        const int32_t k3 = k[3];
        const int32_t len = k3 < 8192 ? 410 + ((k3 * 614) >> 13) : 1024 + (((k3 - 8192) * 1024) / 24575);  /* Q10 */
        const int32_t freq = 18 + ((((256 + ((dcw * 3840) >> 15)) >> 4) * 26) >> 8);                         /* 19 .. 44 a turn */
        for (unsigned h = 0; h < sizeof tf_hl / sizeof tf_hl[0]; h++) {
            const int32_t g = k3 - tf_hl[h][3];
            if (g <= 0) continue;
            const int32_t grow = g >= 3277 ? 1024 : (g * 1024) / 3277;
            const int32_t half = (int32_t)(((int64_t)tf_hl[h][1] * len >> 10) * grow >> 10);
            const int n = 1 + half / 23;                     /* ~1 degree steps over the span */
            const uint8_t *c = tf_hl[h][2] ? white : red;
            int32_t px = 0, py = 0;
            for (int i = 0; i <= n; i++) {
                const uint32_t a = (uint32_t)(tf_hl[h][0] - half + (2 * half * i) / n);
                int32_t r = hl_r;
                if (amp) r += (int32_t)(((int64_t)amp * sn(a * (uint32_t)freq + (uint32_t)ripple)) >> 15);
                int32_t x, y;
                polar(a, r, &x, &y);
                if (i) line(canvas, px, py, x, y, 1485, c);
                px = x;
                py = y;
            }
            if (!half) line(canvas, px, py, px + 300, py, 1485, c);   /* a dot */
        }
    }

    /* sparkles (white, ENVELOPE): the mockup's four around the bubble and four by the lips at 0; four more come out
       with the knob at rest (to 80 % of their reach) and flash full on a kick */
    for (unsigned s = 0; s < sizeof tf_spark / sizeof tf_spark[0]; s++)
        dash(canvas, (uint32_t)tf_spark[s][0], tf_spark[s][1], tf_spark[s][2]);
    const int32_t pop = flash > ((k[2] * 26214) >> 15) ? flash : (k[2] * 26214) >> 15;
    if (pop > 3277)
        for (unsigned s = 0; s < sizeof tf_pop / sizeof tf_pop[0]; s++) {
            const int32_t r1 = tf_pop[s][1] + (int32_t)(((int64_t)(tf_pop[s][2] - tf_pop[s][1]) * pop) >> 15);
            dash(canvas, (uint32_t)tf_pop[s][0], tf_pop[s][1], r1);
        }
    for (unsigned s = 0; s < sizeof tf_lipspark / sizeof tf_lipspark[0]; s++) {
        const int32_t *d = tf_lipspark[s];
        const int32_t dx = d[2] - d[0], dy = d[3] - d[1];
        const int32_t a = (flash * 410) >> 15, b = 1024 + a + ((flash * 666) >> 15) + ((k[2] * 410) >> 15);   /* Q10: out 0.4, longer 0.65 (0.4 more with the knob) */
        line(canvas, d[0] + ((dx * a) >> 10), d[1] + ((dy * a) >> 10), d[0] + ((dx * b) >> 10), d[1] + ((dy * b) >> 10), 1331, white);
    }
}
#endif
