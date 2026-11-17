/* Shared screen bits of ESC_CODE's Delay Trio in op1r (delayed, pitch delay, bubbler): the bottom row in the stock
 * font as ente / dddd (per knob a line in its colour, the label, the value), colours, the FX position's words.
 * Copyright (C) 2026 pcppcp. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef ESC_UI_H
#define ESC_UI_H
#include "op1_synth.h"
#include "seq/seq_text.h"

#define Q(v) ((int32_t)((v) * 1024))
static const uint8_t C_WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, C_RED[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     C_GREEN[4] = { 0x00, 0xED, 0x95, 0xFF }, C_BLUE[4] = { 0x4D, 0x9E, 0xFF, 0xFF },
                     C_GREY[4] = { 0x6F, 0x71, 0x73, 0xFF }, C_DIM[4] = { 0x40, 0x42, 0x44, 0xFF };
static const uint8_t *const col4[4] = { C_BLUE, C_GREEN, C_WHITE, C_RED };
static inline int32_t esc_word(const int16_t *p, int i) { return p[i] < 0 ? 0 : p[i]; }   /* page-2 words 0..32767 */
static inline int esc_clamp(int x, int a, int b) { return x < a ? a : x > b ? b : x; }

enum { TX_LAB, TX_VAL = 4, TX_N = 8 };
static uint32_t esc_tx[TX_N][OP1_TEXT_SIZE / 4];
static char esc_shown[TX_N][14];
static uint8_t esc_made;
static void esc_txt(void *r, int i, const char *s)               /* set only when changed */
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    if (!esc_made) {
        for (int j = 0; j < 4; j++) {
            op1_text_ctor(esc_tx[TX_LAB + j], Q(8 + 78 * j), 114, 12, col4[j]);
            op1_text_ctor(esc_tx[TX_VAL + j], Q(8 + 78 * j), 127, 12, C_WHITE);
        }
        esc_made = 1;
    }
    int same = 1;
    for (int j = 0; j < 14; j++) { if (esc_shown[i][j] != s[j]) { same = 0; break; } if (!s[j]) break; }
    if (!same) { op1_text_set(esc_tx[i], s); int j = 0; for (; j < 13 && s[j]; j++) esc_shown[i][j] = s[j]; esc_shown[i][j] = 0; }
    op1_text_draw(esc_tx[i], r, ident);
}
static void esc_row(void *r, int k, const char *label, const char *value)   /* one knob's label and value */
{
    sq_line(r, Q(8 + 78 * k), Q(110), Q(78 + 78 * k), Q(110), 0x800, col4[k]);
    esc_txt(r, TX_LAB + k, label);
    esc_txt(r, TX_VAL + k, value);
}
static char *esc_pct(char *v, int32_t q15, int full)            /* 0..32767 -> "n%" of full */
{
    char *p = sq_num(v, (uint32_t)((q15 * full + 16383) >> 15), 1);
    return sq_cat(p, "%");
}
static char *esc_ms(char *v, int samples)                       /* samples at 44.1 kHz -> "n ms" */
{
    char *p = sq_num(v, (uint32_t)((samples * 10 + 220) / 441), 1);
    return sq_cat(p, " ms");
}
static __attribute__((unused)) void esc_vline(void *r, int32_t x, int32_t y0, int32_t y1, int32_t w, const uint8_t *c) { if (y1 > y0) sq_line(r, x, y0, x, y1, w, c); }
#endif
