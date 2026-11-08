/* Stock Tombola in short physics steps.
 *
 * R246 steps Tombola's ball world from the Gfx thread: screens_update (0x01A3BB20) hands the frame's tick count
 * (frame_counter 0xFF8016E0, one tick per audio block) to the page player's advance slot, and tombola_player_advance
 * (0x01A65BBC) runs one World::Step of min(ticks, 102) * 64 (16.16 seconds), skipping frames under 10 ticks. A normal
 * frame is 23..25 ticks. The hexagon is a dynamic body on a joint and the balls are not bullets, so nothing stops a
 * ball passing a wall in one long step: after a slow Gfx frame (a heavy page, the full redraw after a page switch, a
 * first parse) balls fall out of the hexagon, FUN_01a64c3a removes them past |10.0|, and once all are gone Tombola
 * is silent. In the emulator, frames forced to 54..102 ticks lose every ball within 20..60 frames; 24-tick frames
 * lose none.
 *
 * The advance slot of Tombola's player vtable (0x01911E04, slot 6; make_firmware.py SLOTS) points here. A frame up to
 * TOMBOLA_STEP ticks goes to R246 unchanged; a longer one (clamped to R246's 102) is split into 2..4 equal advances of
 * 13..26 ticks, each a whole stock advance (step, ball positions, note on / off timers), so the physics runs as in
 * normal frames and the notes' timing adds up to the same ticks.
 * Copyright (C) 2026 Hundekraft. */
#include "tombola.h"

#ifdef OP1_HOST
void host_tombola_advance(void *player, int ticks);
#define TOMBOLA_ADVANCE host_tombola_advance
#else
#define TOMBOLA_ADVANCE ((void (*)(void *, int))0x01A65BBC)   /* tombola_player_advance(player, ticks) */
#endif

void tombola_advance(void *player, int ticks)
{
    if (ticks <= TOMBOLA_STEP) { TOMBOLA_ADVANCE(player, ticks); return; }
    if (ticks > TOMBOLA_MAX) ticks = TOMBOLA_MAX;                  /* R246's own clamp: time past it is dropped */
    int n = 1;
    while (n * TOMBOLA_STEP < ticks) n++;                          /* at most 4 advances (Gfx thread, not audio) */
    for (int i = 0; i < n; i++)
        TOMBOLA_ADVANCE(player, ticks / n + (i < ticks % n));      /* >= 13: every part steps (R246 skips < 10) */
}
