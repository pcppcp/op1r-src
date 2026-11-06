/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* The moose close-up screen (elg.svg, docs/design/engine-screens/round5/build_round5.py) in motion. iter's draw
 * turns the hidden bar0..bar3; the knobs are scene parts (screen px, centre x 160):
 *   antlers  the blue antlers are 90 .. 100 % wide and tall with the pitch (the third voice's interval), about
 *            their base (160, 61.7); the scale glides to its target (a one-pole per frame, ~0.3 s) and ignores
 *            the slot LFO, so the knob's steps do not make the horns jump
 *   breath   the green breath clouds are 60 .. 105 % with the detune, about the nostrils (160, 110.6), and puff
 *            up to 10 % more with the level
 *   hum      the white hum arcs beside the muzzle are 50 .. 150 % with the timbre, plus up to 25 % with the
 *            level, about (160, 94.3)
 *   aurora   the red aurora is 40 .. 130 % tall with the movement, about its top (y 15), waving with its LFO
 *   eyes     blink now and then
 * The other knob parts follow the slot LFO while notes play (elg_knobs, copied by engine_render).
 */
#ifndef ENGINE_HOST_TEST
#include "hk/engine.h"

extern volatile uint32_t elg_blocks;
extern volatile int32_t elg_level, elg_lfo;
extern volatile int16_t elg_knobs[4];
extern int elg_interval(int32_t value);

#define PX(v) ((int32_t)(v) << 10)
#define ONE 1024
#define Q(v) ((int32_t)((v) * 1024))   /* screen px -> Q10 */

static void *e_antlers, *e_breath, *e_hum, *e_aurora, *e_eyes;
static uint32_t last_blocks, seed = 4711;
static int32_t act, blink, ant;                       /* ant: the antlers' scale, Q10, 0 = not set yet */
/* a knob as the voices hear it (after the slot LFO) while notes play, else as set */
#define K(i) screen_knob_live(elg_knobs, (i), act > 64)

void engine_screen_load(void *screen)
{
    e_antlers = SVG_FIND(screen, "antlers");
    e_breath = SVG_FIND(screen, "breath");
    e_hum = SVG_FIND(screen, "hum");
    e_aurora = SVG_FIND(screen, "aurora");
    e_eyes = SVG_FIND(screen, "eyes");
}

void engine_screen_animate(void *screen)
{
    (void)screen;
    const uint32_t blocks = elg_blocks;
    const int on = blocks != last_blocks;
    last_blocks = blocks;
    act += ((on ? ONE : 0) - act) >> 4;
    const int32_t level = on ? elg_level : 0, lfo = on ? elg_lfo : 0;

    /* antlers: 90 .. 100 % with the pitch (the knob as set, no LFO), gliding there: a one-pole per frame, ~16 frames */
    const int32_t at = 922 + elg_interval(SCREEN_KNOB(0) < 0 ? 0 : SCREEN_KNOB(0)) * 51 / 2;
    ant = ant ? ant + ((at - ant) >> 4) : at;
    svg_scale(e_antlers, ant, ant, PX(160), Q(61.73));
    /* breath: 60 .. 105 % with the detune, plus up to 10 % with the level */
    const int32_t b = 614 + ((K(1) * 461) >> 15) + ((level * act) >> 18);
    svg_scale(e_breath, b, b, PX(160), Q(110.55));
    /* hum arcs: 50 .. 150 % with the timbre, plus up to 25 % with the level */
    const int32_t hum = 512 + ((K(2) * 1024) >> 15) + ((level * act) >> 17);
    svg_scale(e_hum, hum, hum, PX(160), Q(94.28));
    /* aurora: 40 .. 130 % tall, waving with the LFO */
    const int32_t aur = 410 + ((K(3) * 922) >> 15) + (lfo >> 6);
    svg_scale(e_aurora, ONE, aur, 0, Q(14.96));
    /* eyes: blink for 6 frames every 3 .. 6 s */
    if (blink > 0) {
        blink--;
    } else {
        seed = seed * 1664525u + 1013904223u;
        if ((seed >> 24) < 2) blink = 6;
    }
    svg_scale(e_eyes, ONE, blink > 0 ? 154 : ONE, 0, Q(79.55));
}

void engine_screen_draw(void *screen, void *canvas)
{
    (void)screen;
    (void)canvas;
}
#endif
