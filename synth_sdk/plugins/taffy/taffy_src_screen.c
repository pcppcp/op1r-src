/* taffy_src_screen.c: taffy/screen.c with the 2.x builder's renames (tools/kennel/import_hk_engines.py).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifdef OP1_HOST
#define HK_SCREEN_HOST
#endif
#define engine_note_on taffy_note_on
#define engine_render taffy_render
#define engine_note_off taffy_note_off
#define engine_screen_load taffy_screen_load
#define engine_screen_animate taffy_screen_animate
#define engine_screen_draw taffy_screen_draw
#include "screen.c"
