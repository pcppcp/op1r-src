/* pew_src_pew.c: pew/pew.c with the 2.x builder's renames (tools/kennel/import_hk_engines.py).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifdef OP1_HOST
#define ENGINE_HOST_TEST
#define FX_HOST_TEST
#endif
#define engine_note_on pew_note_on
#define engine_render pew_render
#define engine_note_off pew_note_off
#define engine_screen_load pew_screen_load
#define engine_screen_animate pew_screen_animate
#define engine_screen_draw pew_screen_draw
#include "pew.c"
