/* elg_src_screen.c: elg/screen.c with the 2.x builder's renames (tools/kennel/import_hk_engines.py).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifdef OP1_HOST
#define HK_SCREEN_HOST
#endif
#define engine_note_on elg_note_on
#define engine_render elg_render
#define engine_note_off elg_note_off
#define engine_screen_load elg_screen_load
#define engine_screen_animate elg_screen_animate
#define engine_screen_draw elg_screen_draw
#include "screen.c"
