/* USB out page (clock screen + T2, docs/21), in the style of the synth playmode page: blue label "USB OUT", value
 * "MASTER" or "MASTER / + TRACKS". Blue turns between them; applies at once (USB stays 6 channels, the track channels
 * are silent and the track work is skipped with "MASTER"). Green row "MIDI SEQ" OFF / ON: the MIDI sequencer
 * device (seqdev/ms_fw.c, docs/39) is opt-in, off by default. Both saved in the DB table op1rsettings. */
#include "../op1_synth.h"
#include "stroke_font.h"
#include "op1r_settings.h"

extern volatile uint8_t usb_tracks_live;
static uint8_t setting;                                    /* 0 master, 1 master + tracks */
volatile uint8_t ms_enabled;                               /* MIDI sequencer device on (read by seqdev/ms_fw.c) */
volatile uint8_t ms_quant;                                 /* MIDI recorder quantize grid (MS_Q_*, set in its view) */

/* at USB start (boot, after the DB is up; also when USB restarts after disk mode) */
int usbout_load(void)
{
    setting = (uint8_t)(op1r_setting_get("usbtracks", 0) == 1);
    ms_enabled = (uint8_t)(op1r_setting_get("midiseq", 0) == 1);
    int q = op1r_setting_get("msquant", 0);
    ms_quant = (uint8_t)(q >= 0 && q <= 6 ? q : 0);
    return setting;
}
static void usbout_save(void) { op1r_setting_put("usbtracks", setting); }   /* KeyHandler thread, on change */
static void seq_save(void) { op1r_setting_put("midiseq", ms_enabled); }
void ms_quant_save(void) { op1r_setting_put("msquant", ms_quant % 7); }   /* KeyHandler thread (MIDI recorder) */

static int32_t acc;
void usbout_turn(int delta)                                 /* blue: one detent each way switches */
{
    if ((acc > 0 && delta < 0) || (acc < 0 && delta > 0)) acc = 0;
    acc += delta;
    int st = acc / 1024; if (!st) return;
    acc = 0;
    uint8_t v = st > 0;
    if (v == setting) return;
    setting = v;
    usb_tracks_live = v;                                    /* at once */
    usbout_save();
}
static int32_t acc_g;
void usbout_seq_turn(int delta)                             /* green: right on, left off */
{
    if ((acc_g > 0 && delta < 0) || (acc_g < 0 && delta > 0)) acc_g = 0;
    acc_g += delta;
    int st = acc_g / 1024; if (!st) return;
    acc_g = 0;
    uint8_t v = st > 0;
    if (v == ms_enabled) return;
    ms_enabled = v;                                         /* ms_fw.c stops / starts in its main-loop service */
    seq_save();
}

/* ---- screen: stroke-font text like the stock playmode page (label column, value column, 1.5 px lines) ---- */
static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF };
#define US_S   OP1_Q10(5) / 2                               /* 2.5 px per grid unit: 15 px caps, 15 px advance */
#define US_W   0x600                                        /* 1.5 px */
#define US_LX  OP1_Q10(24)
#define US_VX  OP1_Q10(164)
#define US_Y0  OP1_Q10(40)
#define US_DY  OP1_Q10(28)

static void us_init(void *scr) { (void)scr; }

static void us_update(void *scr, int dt)
{
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    sf_text(r, US_LX, US_Y0, US_S, "USB OUT", US_W, c_blue);
    sf_text(r, US_VX, US_Y0, US_S, "MASTER", US_W, c_blue);
    if (setting) sf_text(r, US_VX, US_Y0 + US_DY, US_S, "+ TRACKS", US_W, c_blue);
    sf_text(r, US_LX, US_Y0 + 2 * US_DY, US_S, "MIDI SEQ", US_W, c_green);
    sf_text(r, US_VX, US_Y0 + 2 * US_DY, US_S, ms_enabled ? "ON" : "OFF", US_W, c_green);
}

const op1_screen_t SCREEN_usbout = { us_init, us_update };
