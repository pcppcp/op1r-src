/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* The snow globe screen (snowglobe.svg, docs/design/engine-screens/round5/build_round5.py) in motion; globe
 * centre (160, 68.9), screen px:
 *   snow0..2  the blue snow: 1 .. 3 layers fall with longer decay, each swirling round the globe at its own
 *             speed, faster while the tail rings (frozen: they hang)
 *   shimmer   the green trail is 55 .. 115 % with the shimmer about its foot (186.3, 105), twinkling while the
 *             tail rings
 *   halo      the white rings either side of the glass are 85 .. 120 % with the globe size
 *   key       the red wind-up key turns a sixth of a turn back over the mix, and keeps winding while frozen,
 *             about its stem (204, 121.2)
 *   smoke     rises from the chimney while the tail rings
 */
#ifndef FX_HOST_TEST
#include "hk/fx.h"
#include "hk/engine.h"

extern volatile uint32_t snowglobe_blocks;
extern volatile int32_t snowglobe_level, snowglobe_freeze;
extern volatile int16_t snowglobe_knobs[FX_INSTANCES][4];
extern volatile uint32_t snowglobe_kblocks[FX_INSTANCES];

#define PX(v) ((int32_t)(v) << 10)
#define ONE 1024
#define Q(v) ((int32_t)((v) * 1024))   /* screen px -> Q10 */
#define GX PX(160)
#define GY Q(68.91)

static void *e_snow[3], *e_shimmer, *e_halo, *e_key, *e_smoke;
static uint32_t last_blocks, t, swirl[3], key;
static int32_t level, smoke;

static uint32_t kseen[FX_INSTANCES];
static int32_t kn[4];   /* the params this frame, as the effect hears them (after the LFO) */

void engine_screen_load(void *screen)
{
    static const char *const snow[3] = {"snow0", "snow1", "snow2"};
    for (int i = 0; i < 3; i++) e_snow[i] = SVG_FIND(screen, snow[i]);
    e_shimmer = SVG_FIND(screen, "shimmer");
    e_halo = SVG_FIND(screen, "halo");
    e_key = SVG_FIND(screen, "key");
    e_smoke = SVG_FIND(screen, "smoke");
}

void engine_screen_animate(void *screen)
{
    (void)screen;
    fx_screen_snapshot(snowglobe_knobs, snowglobe_kblocks, kseen, kn);
    const uint32_t blocks = snowglobe_blocks;
    const int running = blocks != last_blocks;
    last_blocks = blocks;
    t++;
    const int32_t target = running ? snowglobe_level : 0;
    level += (target - level) >> 3;
    const int frozen = running && snowglobe_freeze;

    /* snow: 1 .. 3 layers with the decay, the rest out of view; each swirls about the globe's centre */
    const int layers = 1 + ((kn[0] * 2 + 16384) >> 15);
    for (int i = 0; i < 3; i++) {
        if (!frozen) swirl[i] += (uint32_t)(((4 + level / 600) * (3 - i)) >> 1);
        if (i < layers) svg_turn(e_snow[i], (i & 1) ? -swirl[i] : swirl[i], GX, GY);
        else svg_move(e_snow[i], 0, PX(400));
    }
    /* shimmer: 55 .. 115 % with the shimmer, twinkling up to 5 % with the tail */
    const int32_t tw = level > 8000 ? 51 : (level * 51) / 8000;
    const int32_t sh = 563 + ((kn[1] * 614) >> 15) + ((SVG_SIN(t * 700) * tw) >> 15);
    svg_scale(e_shimmer, sh, sh, Q(186.27), Q(105.03));
    /* halo: 85 .. 120 % with the size */
    const int32_t h = 870 + ((kn[2] * 358) >> 15);
    svg_scale(e_halo, h, h, GX, GY);
    /* key: 0 .. -60 degrees over the mix, winding on while frozen */
    if (frozen) key += 40;
    svg_turn(e_key, (uint32_t)(-((kn[3] * 1365) >> 15)) + key, PX(204), Q(121.19));
    /* smoke: rises 3 px and starts again while the tail rings; still otherwise */
    if (level > 400) smoke = (smoke + 120) % PX(3);
    svg_move(e_smoke, 0, -smoke);
}

void engine_screen_draw(void *screen, void *canvas)
{
    (void)screen;
    (void)canvas;
}
#endif
