/* washer_src_screen.c: washer/screen.c with the 2.x builder's renames (tools/kennel/import_hk_engines.py).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifdef OP1_HOST
#define HK_SCREEN_HOST
#endif
#define fx_reset washer_fx_reset
#define fx_process washer_fx_process
#define engine_screen_load washer_fx_screen_load
#define engine_screen_animate washer_fx_screen_animate
#define engine_screen_draw washer_fx_screen_draw
#include "screen.c"
