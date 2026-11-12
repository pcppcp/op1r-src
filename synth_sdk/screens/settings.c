/* Settings page (clock / card + T3, docs/25), in the style of the playmode page like the USB out page: blue
 * "COUNT-IN" with 4 / 8 / 16 beats (tape/countin.c), green "VELOCITY" 1 .. 127: the velocity of notes played on the
 * OP-1's own keys (the keys send 127; scale/sc_fw.c sets it in the VoiceCtl key_down hook). Saved in op1rsettings
 * (keys 'countin', 'kbvel'). The white and red rows are free for future small settings. */
#include "../op1_synth.h"
#include "stroke_font.h"
#include "stxt.h"
#include "op1r_settings.h"

extern uint8_t ci_beats;
static const uint8_t ci_vals[3] = { 4, 8, 16 };
static uint8_t ci_idx;                                      /* index into ci_vals */

uint8_t kb_vel = 127;                                       /* keyboard note velocity (127: as the stock) */
extern volatile uint8_t tm_on;                              /* tape machine: 0 stock, 1 free (tape/tm.c, docs/82) */

static void st_load(void)
{
    int v = op1r_setting_get("countin", -1); if (v >= 0 && v <= 2) ci_idx = (uint8_t)v;
    v = op1r_setting_get("kbvel", 127); if (v >= 1 && v <= 127) kb_vel = (uint8_t)v;
    v = op1r_setting_get("tapemach", -1); if (v == 0 || v == 1) tm_on = (uint8_t)v;
}
static void st_save(void) { op1r_setting_put("countin", ci_idx); op1r_setting_put("kbvel", kb_vel); op1r_setting_put("tapemach", tm_on); }   /* KeyHandler thread */

static int32_t acc, vacc;
static uint8_t st_dirty;
void settings_flush(void) { if (st_dirty) { st_dirty = 0; st_save(); } }   /* at the next key event (as scale_flush) */
void settings_turn(int knob, int delta)                     /* blue: one value per detent, green: velocity, clamped */
{
    if (knob == 1) {                                        /* 4 steps per detent (2 events of 1024) */
        if ((vacc > 0 && delta < 0) || (vacc < 0 && delta > 0)) vacc = 0;
        vacc += delta;
        int st = vacc / 512; if (!st) return;
        vacc -= st * 512;
        int v = kb_vel + st; v = v < 1 ? 1 : v > 127 ? 127 : v;
        if (v != kb_vel) { kb_vel = (uint8_t)v; st_dirty = 1; }
        return;
    }
    if (knob == 2) {                                        /* white: tape machine stock / free (any detent flips) */
        if ((acc > 0 && delta < 0) || (acc < 0 && delta > 0)) acc = 0;
        acc += delta; if (acc / 1024 == 0) return;
        acc = 0; tm_on = (uint8_t)!tm_on; st_dirty = 1; return;
    }
    if (knob != 0) return;
    if ((acc > 0 && delta < 0) || (acc < 0 && delta > 0)) acc = 0;
    acc += delta;
    int st = acc / 1024; if (!st) return;
    acc = 0;
    int v = ci_idx + (st > 0 ? 1 : -1);
    if (v < 0 || v > 2) return;
    ci_idx = (uint8_t)v; ci_beats = ci_vals[ci_idx];
    st_dirty = 1;
}

static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF };
#define ST_S   OP1_Q10(5) / 2                               /* as the USB out page: 15 px caps */
#define ST_W   0x600
#define ST_LX  OP1_Q10(24)
#define ST_VX  OP1_Q10(164)
#define ST_Y0  OP1_Q10(40)
#define ST_DY  OP1_Q10(28)                                  /* row pitch, as the USB out page */

static void st_init(void *scr) { (void)scr; st_load(); ci_beats = ci_vals[ci_idx]; }
static void st_update(void *scr, int dt)
{
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    static const char *const txt[3] = { "4", "8", "16" };
    st_text(r, ST_SETTINGS + 0, ST_LX, (ST_Y0) >> 10, c_blue, "COUNT-IN");
    st_text(r, ST_SETTINGS + 1, ST_VX, (ST_Y0) >> 10, c_blue, txt[ci_idx]);
    char v[4]; int n = kb_vel, i = 0;
    if (n >= 100) v[i++] = (char)('0' + n / 100);
    if (n >= 10) v[i++] = (char)('0' + n / 10 % 10);
    v[i++] = (char)('0' + n % 10); v[i] = 0;
    st_text(r, ST_SETTINGS + 2, ST_LX, (ST_Y0 + ST_DY) >> 10, c_green, "VELOCITY");
    st_text(r, ST_SETTINGS + 3, ST_VX, (ST_Y0 + ST_DY) >> 10, c_green, v);
    static const uint8_t c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    st_text(r, ST_SETTINGS + 4, ST_LX, (ST_Y0 + 2 * ST_DY) >> 10, c_white, "TAPE");
    st_text(r, ST_SETTINGS + 5, ST_VX, (ST_Y0 + 2 * ST_DY) >> 10, c_white, tm_on ? "STEFAN" : "JESPER/OP1");
}
const op1_screen_t SCREEN_settings = { st_init, st_update };
