/* snowglobe_src_screen.c: snowglobe/screen.c with the 2.x builder's renames (tools/kennel/import_hk_engines.py).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifdef OP1_HOST
#define HK_SCREEN_HOST
#endif
#define fx_reset snowglobe_fx_reset
#define fx_process snowglobe_fx_process
#define engine_screen_load snowglobe_fx_screen_load
#define engine_screen_animate snowglobe_fx_screen_animate
#define engine_screen_draw snowglobe_fx_screen_draw
#include "screen.c"
