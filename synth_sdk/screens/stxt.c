/* Stock-font text for op1r's settings-style pages (settings, USB out, scale, FX routing): a pool of firmware text
 * objects (op1_synth.h op1_text_*), one per slot, built on first use, re-set only when the string changes. The stock
 * font has no capitals (they come out as other glyphs): strings are lowercased here. Size 16 (the pages' old 15 px
 * stroke caps). GUI thread only. */
#include "../op1_synth.h"
#include "stxt.h"

static uint32_t pool[ST_SLOTS][OP1_TEXT_SIZE / 4];
static char shown[ST_SLOTS][24];
static uint8_t made[ST_SLOTS];

void st_text(void *r, int i, int32_t x_q10, int32_t y_px, const uint8_t *rgba, const char *s)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    if (i < 0 || i >= ST_SLOTS) return;
    char l[24]; int n = 0;
    for (; s[n] && n < 23; n++) l[n] = (char)(s[n] >= 'A' && s[n] <= 'Z' ? s[n] + 32 : s[n]);
    l[n] = 0;
    if (!made[i]) { op1_text_ctor(pool[i], x_q10, y_px, 16, rgba); made[i] = 1; shown[i][0] = 1; }   /* 1: force the set */
    int same = 1;
    for (int j = 0; j <= n; j++) if (shown[i][j] != l[j]) { same = 0; break; }
    if (!same) { op1_text_set(pool[i], l); for (int j = 0; j <= n; j++) shown[i][j] = l[j]; }
    op1_text_draw(pool[i], r, ident);
}
