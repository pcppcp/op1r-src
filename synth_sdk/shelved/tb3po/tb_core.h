/* TB-3PO pattern generator (tb3po/tb_core.c): a TB-303 style random line from a 16-bit seed, a density and a scale.
 * Port of Logarhythm's TB-3PO (O_C Phazerville software/src/applets/TB3PO.h, MIT, (c) 2020 Logarhythm,
 * modifications by djphazer); see tb_core.c. */
#ifndef TB_CORE_H
#define TB_CORE_H
#include <stdint.h>

#define TB_STEPS  32
#define TB_SCALES 10

typedef struct {
    uint16_t seed;
    uint8_t  density;            /* 0..14 = -7..+7: |d| more gates; d < 0 fewer pitch changes (d = -7: root only) */
    uint8_t  scale;              /* 0..TB_SCALES-1 */
    /* the generated pattern (tb_generate) */
    uint32_t gates, slides, accents, oct_up, oct_down;
    uint8_t  notes[TB_STEPS];    /* scale degrees */
} tb_pat;

extern const char *const tb_scale_name[TB_SCALES];
int  tb_scale_size(int scale);
void tb_generate(tb_pat *p);                                  /* deterministic: seed, density, scale */
/* the step's pitch in semitones above the root: degree (+ octave flags, + transpose in scale degrees) */
int  tb_semitones(const tb_pat *p, int step, int transpose);
static inline int tb_gate(const tb_pat *p, int s)   { return p->gates >> s & 1; }
static inline int tb_slide(const tb_pat *p, int s)  { return p->slides >> s & 1; }
static inline int tb_accent(const tb_pat *p, int s) { return p->accents >> s & 1; }
#endif
