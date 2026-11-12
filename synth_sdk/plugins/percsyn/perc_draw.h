/* perc's page drawing, shared by the synth engine and the drum kit. GPL-3.0-or-later with the OP1R permission. */
#ifndef PERC_DRAW_H
#define PERC_DRAW_H
#include <stdint.h>
typedef struct {
    int type, has_patch;            /* the sound drawn (built-in index, else a transferred one) and whether it plays */
    int16_t k[4];                   /* pitch, length, punch, tone: 0..32767, centre 16384 */
    int16_t drive, level;           /* Shift words: 0..32767 (level 0 = centre, as a sound from before) */
    int16_t morph;                  /* 0 left .. 32767 right */
    uint8_t morph_lane;             /* Shift + orange: 0 nothing, 1 morph, 2 morph of a one-sided sound */
    const char *name, *idx;         /* top line: the sound, its place */
    uint8_t page2[3], shift;        /* lanes showing their page-2 value; Shift held */
    const char *blue2;              /* blue's page-2 label ("type" / "sound") */
    uint32_t hit_seq; int32_t hit_vel;   /* bumped on a hit, its velocity (Q15) */
} perc_view_t;
void perc_draw_page(void *scr, int dt, const perc_view_t *v);
#endif
