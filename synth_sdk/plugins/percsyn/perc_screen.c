#pragma GCC optimize ("no-tree-loop-distribute-patterns") /* no implicit libc calls */
/* perc synth engine's page (GUI thread): its knobs and Shift words into perc_draw_page (perc_draw.c).
 * GPL-3.0-or-later with the OP1R firmware permission. */
#include "op1_synth.h"
#include "perc.h"
#include "perc_tables.h"
#include "perc_xfer.h"
#include "perc_draw.h"

static char *num2(char *p, int x) { if (x < 10) *p++ = '0'; char t[4]; int n = 0; do t[n++] = (char)('0' + x % 10); while ((x /= 10)); while (n) *p++ = t[--n]; *p = 0; return p; }
static char *cat(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }

static void ps_init(void *scr) { (void)scr; }
static void ps_update(void *scr, int dt)
{
    int slot = OP1_SCREEN_SLOT & 7;
    perc_view_t v;
    op1_synth_knobs_live(slot, v.k);
    int16_t wsh[8]; const int16_t *w = op1_knobs_shown(0, 0, op1_synth_knobs(slot), wsh);
    for (int i = 0; i < 4; i++) if (v.k[i] < 0) v.k[i] = 0;
    v.type = perc_type_of(w);
    v.has_patch = perc_patch_of(v.type) != 0;
    v.drive = w[6]; v.level = w[7];
    v.morph = 0; v.morph_lane = 0;                                  /* no fourth Shift word on a synth */
    static char name[16], idx[16];
    if (v.type < PERC_FACTORY_COUNT) cat(name, PERC_FACTORY_NAME[v.type]);
    else { char *p = cat(name, "po-32 "); p = num2(p, v.type - PERC_FACTORY_COUNT + 1); if (!v.has_patch) cat(p, " empty"); }
    if (perc_xfer_busy()) cat(idx, "read..");                         /* a recording in the slot is being decoded */
    else { char *p = num2(idx, v.type + 1); p = cat(p, "/"); num2(p, PERC_TYPES); }
    v.name = name; v.idx = idx;
    v.shift = (uint8_t)op1_shift_held();
    for (int i = 0; i < 3; i++) v.page2[i] = op1_eng_page[i];
    v.blue2 = "type";
    v.hit_seq = perc_hit_seq; v.hit_vel = perc_hit_vel;
    perc_draw_page(scr, dt, &v);
}

const op1_screen_t SCREEN_perc = { ps_init, ps_update };
