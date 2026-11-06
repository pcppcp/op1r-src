/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* PEW's macros, shared by the sound (pew.c) and the screen (screen.c), so the drawbars on screen are the ones heard.
 * Block rate only (UI or once per audio block): no per-sample work here.
 *
 * blue (REGISTER): sweeps classic registrations, mellow to full, each bar's level moving straight between them:
 *     008000000 (8' flute) -> 888000000 -> 888800000 -> 888808008 -> 888888888
 *   bars in Hammond order: 16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1'
 * page 2 of blue / green / white (preset words 5, 6, 7): push the low (16', 5 1/3', 8'), middle (4', 2 2/3', 2') or
 *   high (1 3/5', 1 1/3', 1') bars further out: 0 = as REGISTER sets them, 32767 = all the way out (8)
 */
#ifndef PEW_MACRO_H
#define PEW_MACRO_H
#include <stdint.h>

#define PEW_REGS 5
static const uint8_t pew_regs[PEW_REGS][9] = {
    {0, 0, 8, 0, 0, 0, 0, 0, 0},
    {8, 8, 8, 0, 0, 0, 0, 0, 0},
    {8, 8, 8, 8, 0, 0, 0, 0, 0},
    {8, 8, 8, 8, 0, 8, 0, 0, 8},
    {8, 8, 8, 8, 8, 8, 8, 8, 8},
};

/* the nine drawbar levels, Q12 (8 = 32768 = all the way out), from the blue knob and the three page-2 words */
static inline void pew_registration(int32_t blue, const int16_t *push, int32_t out[9])
{
    if (blue < 0) blue = 0;
    if (blue > 32767) blue = 32767;
    const int32_t x = blue * (PEW_REGS - 1);                  /* segment in the top bits, 15-bit fraction */
    const int seg = x >> 15;
    const int32_t f = x & 32767;
    const uint8_t *a = pew_regs[seg], *b = pew_regs[seg < PEW_REGS - 1 ? seg + 1 : seg];
    static const uint8_t group[9] = {0, 0, 0, 1, 1, 1, 2, 2, 2};                /* no division in the loop */
    for (int i = 0; i < 9; i++) {
        int32_t l = (a[i] << 12) + ((((int32_t)b[i] - a[i]) * f) >> 3);          /* (b - a) * f / 32768 * 4096 */
        const int32_t p = push ? push[group[i]] : 0;
        if (p > 0) l += (((32768 - l) >> 3) * (p < 32767 ? p : 32767)) >> 12;     /* towards 8 */
        out[i] = l;
    }
}

/* red (LESLIE), three zones of the knob: brake (rotors spin down and stop), slow (chorale), fast (tremolo); the rotor
 * speeds as phase per 128-sample block, 2^32 * f * 128 / 44100 */
#define PEW_SLOW 9972000                                       /* 0.8 Hz */
#define PEW_FAST 83526000                                      /* 6.7 Hz */
static inline int pew_leslie_zone(int32_t red) { return red < 10923 ? 0 : red < 21845 ? 1 : 2; }
static inline int32_t pew_leslie_target(int32_t red)
{
    const int z = pew_leslie_zone(red);
    return z == 0 ? 0 : z == 1 ? PEW_SLOW : PEW_FAST;
}

#endif
