/* wavetable_src_wavetable.c: wavetable/wavetable.c with the 2.x builder's renames (tools/kennel/import_hk_engines.py).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifdef OP1_HOST
#define ENGINE_HOST_TEST
#define FX_HOST_TEST
#endif
#define engine_note_on wavetable_note_on
#define engine_render wavetable_render
#define engine_note_off wavetable_note_off
#define engine_screen_load wavetable_screen_load
#define engine_screen_animate wavetable_screen_animate
#define engine_screen_draw wavetable_screen_draw
#include "wavetable.c"
