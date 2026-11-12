/* Template tool: Shift + metronome shows a small panel over the current page for about 1.5 s, with one bar per press
 * since boot. It does not swallow the key (key returns 0), so the OP-1 still does what Shift + metronome does there.
 * A tool gets every key after op1r's own handlers and before the stock ones (key: KeyHandler thread), a call every
 * main-loop pass (service) and, after op1_overlay(&TOOL_mytool), draw(raster) every frame (GUI thread) until it
 * returns 0. Keep key and draw short; do slow work (files, sounds) in service.
 * Copyright (C) <year> <you>. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"

#define MY_KEY      0x01                   /* metronome */
#define MY_FRAMES   90                     /* panel shown for this many frames */

extern const op1_tool_t TOOL_mytool;
static volatile uint8_t my_req;            /* key -> service: show the panel */
static volatile int my_frames;             /* frames left to draw (GUI thread) */
static int my_count;

static int my_key(int code, int pressed)
{
    if (code == MY_KEY && pressed && op1_shift_held()) { my_count++; my_req = 1; }
    return 0;                              /* 1 would swallow the press (and its release): nothing else would see it */
}

static void my_service(void)
{
    if (!my_req) return;
    my_req = 0;
    my_frames = MY_FRAMES;
    op1_overlay(&TOOL_mytool);
}

static int my_draw(void *r)
{
    static const uint8_t white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF };
    if (my_frames <= 0) return 0;
    my_frames--;
    op1_raster_dim(r);                      /* the page darkened, as HELP does */
    op1_pt a = { OP1_Q10(80), OP1_Q10(50) }, b = { OP1_Q10(240), OP1_Q10(50) }, c = { OP1_Q10(240), OP1_Q10(110) }, d = { OP1_Q10(80), OP1_Q10(110) };
    op1_raster_line(r, &a, &b, 0x600, white); op1_raster_line(r, &b, &c, 0x600, white);
    op1_raster_line(r, &c, &d, 0x600, white); op1_raster_line(r, &d, &a, 0x600, white);
    int n = my_count < 24 ? my_count : 24;  /* one bar per press, up to 24 */
    for (int i = 0; i < n; i++) {
        op1_pt p = { OP1_Q10(88 + i * 6), OP1_Q10(100) }, q = { OP1_Q10(88 + i * 6), OP1_Q10(60) };
        op1_raster_line(r, &p, &q, 0x800, blue);
    }
    return 1;
}

const op1_tool_t TOOL_mytool = { my_key, my_service, my_draw };
