/* Master FX source (docs/54): Shift + mixer T2 page. Blue chooses what the master FX processes: "master" (the whole
 * mix, stock) or "instrument" (the live synth / drum bus only: out = mix + FX(inst) - inst). Saved in op1rsettings
 * (key 'mfxsrc').
 * The instrument's stereo part is not in a buffer of its own: tape L / R is copied right after tape_play_mix (the
 * LFO / sequencer tick 0x019C9D04, fr_snap) and subtracted from tape L / R at the metronome entry 0x019DE548
 * (fr_capture), after Core_mix_master has added the instrument. Exact unless the mix clipped there.
 * Output (green, instrument source only): "stereo master" (the FX's stereo result replaces the dry instrument in
 * the master, not recorded: stock) or "mono tape": the FX runs on the raw instrument bus (l = r = bus) and its mono
 * sum is written back into the bus, which tape_record_block records after Core_mix_master; the monitor hears it with
 * the track's volume / pan, estimated per block as gain = sum(inst * bus) / sum(bus^2) (one division per block).
 * "aux": the tape tracks' sends (mixer T1, screens/mixaux.c) from the track outs (screens/usbtrack.c tr_out, pre-fader)
 * x vol x level x mute fade x send; stereo master adds FX(aux) - aux, mono tape adds the FX's mono output to the
 * recorded bus (the selected track's own send muted: no feedback). The track work runs only while a send is up. */
#include "../op1_synth.h"
#include "stroke_font.h"
#include "op1r_settings.h"

uint8_t fr_src;                                             /* 0 master, 1 instrument, 2 aux */
uint8_t fr_out;                                             /* instrument / aux: 0 stereo master, 1 mono tape */
uint8_t fr_send[4];                                         /* aux sends 0..100 */
volatile uint8_t fr_aux_live;                               /* aux with a send up: track outs needed */
extern int16_t tr_out[4][64];
/* the routing runs in every master FX's render (plugins: dispatcher.c fx_render_stereo; stock: the fr_st_* wrappers
 * below). With the master FX off: no copies and no track work (5-7 % CPU with aux) */
#ifdef OP1_HOST
uint8_t fr_host_mfx = 1;
#define MFX_ON() fr_host_mfx
#else
#define MFX_ON() (*(volatile uint8_t *)0x01AAAC40)
#endif
volatile uint8_t sc_want;                                   /* sidechain asked for track outs: blocks left (op1_sc_source) */
volatile uint8_t sc_blk, tr_at;                             /* block counter (fr_snap) / block of the last track outs */
int fr_tracks_needed(void) { return (fr_aux_live && MFX_ON()) || sc_want; }   /* screens/usbtrack.c: the track work */
static void fr_live(void) { fr_aux_live = fr_src == 2 && (fr_send[0] | fr_send[1] | fr_send[2] | fr_send[3]); }
static int16_t fr_snl[64], fr_snr[64], fr_il[64], fr_ir[64];
static volatile uint8_t fr_snapped, fr_have;

#ifdef OP1_HOST
int16_t fr_host_tape[2][64];
int16_t fr_host_bus[64];
int16_t fr_host_input[64];
#define INPUT  fr_host_input
#define TAPE_L fr_host_tape[0]
#define TAPE_R fr_host_tape[1]
#define BUS    fr_host_bus
int16_t fr_host_meter[4], fr_host_peak, fr_host_level = 0x7FFF; uint8_t fr_host_mflag = 1; int32_t fr_host_track;
#define METER     fr_host_meter
#define PEAK      fr_host_peak
#define TAPE_LVL  fr_host_level
#define METER_ON  fr_host_mflag
#define TRACK     fr_host_track
int16_t fr_host_vol[4] = { 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF }, fr_host_lev[4] = { 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF },
        fr_host_mute[4] = { 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF }, fr_host_pan[4];
#define T_VOL     fr_host_vol
#define T_LEV     fr_host_lev
#define T_MUTE    fr_host_mute
#define T_PAN     fr_host_pan
#else
#define T_VOL     ((volatile int16_t *)0xFF801928)          /* per track, as Core_mix_master / tape_play_mix use them */
#define T_LEV     ((volatile int16_t *)0xFF8018B4)
#define T_MUTE    ((volatile int16_t *)0xFF8018C4)          /* mute fade (Core::update_mutes 0x01967ED0) */
#define T_PAN     ((volatile int16_t *)0xFF801930)
#define TAPE_L ((int16_t *)0xFF800D34)
#define TAPE_R ((int16_t *)0xFF800DB4)
#define BUS    ((int16_t *)0xFF800BB4)                       /* synth_mix_bus: recorded after Core_mix_master */
#define INPUT  ((int16_t *)0xFF800CB4)                       /* input block (mic / line / USB), mono, docs/29 */
/* the record meter (Core_mix_master loop 1): block peak W[0xFF801084] = max |bus x tape level [0x01B4EF6E]|, then
 * the selected track's meter W[0xFF802990 + 2t] += peak if B[0xFF80103C] (0xFFA00FE0..0xFFA01002) */
#define METER     ((volatile int16_t *)0xFF802990)
#define PEAK      (*(volatile int16_t *)0xFF801084)
#define TAPE_LVL  (*(volatile int16_t *)0x01B4EF6E)
#define METER_ON  (*(volatile uint8_t *)0xFF80103C)
#define TRACK     (*(volatile int32_t *)0xFF802964)
#endif

void fr_snap(void)                                          /* audio ISR, after the tape mix */
{
#ifndef OP1_HOST
    *(volatile uint8_t *)0x01DD591E = 0;                    /* the stock body of 0x019C9D04 */
#endif
    sc_blk++;                                               /* once per block, after the tape mix (track outs) */
    if (sc_want) sc_want--;
    if (fr_src != 1 || !MFX_ON()) return;
    for (int i = 0; i < 64; i++) { fr_snl[i] = TAPE_L[i]; fr_snr[i] = TAPE_R[i]; }
    fr_snapped = 1;
}

void fr_capture(void)                                       /* audio ISR, after the instrument was mixed in */
{
    if (!fr_snapped) return;
    fr_snapped = 0;
    for (int i = 0; i < 64; i++) { fr_il[i] = (int16_t)(TAPE_L[i] - fr_snl[i]); fr_ir[i] = (int16_t)(TAPE_R[i] - fr_snr[i]); }
    fr_have = 1;
}

static void fr_meter(int32_t pk)                            /* record meter: the wet peak instead of the dry one */
{
    if (pk > 32767) pk = 32767;
    uint32_t t = (uint32_t)TRACK;
    if (METER_ON && t < 4) METER[t] = op1_sat16(METER[t] - PEAK + pk);
    PEAK = (int16_t)pk;
}

static inline int32_t q15(int32_t a, int32_t b) { return (a * b) >> 15; }

static void fr_route_aux(void (*fx)(void *, int16_t *, int16_t *, const int16_t *), void *self, int16_t *l, int16_t *r, const int16_t *params)
{
    if (!fr_aux_live) { fx(self, l, r, params); return; }   /* no send up: FX2 on the master as stock */
    uint32_t sel = fr_out ? (uint32_t)TRACK : 4;            /* mono tape: the recording track's send muted */
    int32_t g[4];
    for (int t = 0; t < 4; t++)                             /* post-fader send gain, Q15 */
        g[t] = t == (int)sel ? 0 : q15(q15(q15(T_VOL[t], T_LEV[t]), T_MUTE[t]), fr_send[t] * 327);
    int16_t x[64], a[64], b[64];
    for (int i = 0; i < 64; i++) {
        int32_t v = (tr_out[0][i] * g[0] + tr_out[1][i] * g[1] + tr_out[2][i] * g[2] + tr_out[3][i] * g[3]) >> 15;
        x[i] = a[i] = b[i] = op1_sat16(v);
    }
    fx(self, a, b, params);
    if (!fr_out) {                                          /* stereo master: the FX's change added */
        for (int i = 0; i < 64; i++) { l[i] = op1_sat16(l[i] + a[i] - x[i]); r[i] = op1_sat16(r[i] + b[i] - x[i]); }
        return;
    }
    uint32_t t = (uint32_t)TRACK;                           /* mono tape: FX output into the recorded bus */
    int32_t gl = 0, gr = 0, pk = 0, tl = TAPE_LVL;
    if (t < 4) {                                            /* monitor at the track's gains (Core_mix_master loop 1) */
        int32_t v = q15(T_VOL[t], T_LEV[t]), p = T_PAN[t];
        int32_t pl = 2 * (0x4000 - p), pr = 2 * (p + 0x4000);
        pl = pl > 0x7FFF ? 0x7FFF : pl < 0 ? 0 : pl; pr = pr > 0x7FFF ? 0x7FFF : pr < 0 ? 0 : pr;
        gl = q15(q15(v, pl), tl); gr = q15(q15(v, pr), tl);
    }
    for (int i = 0; i < 64; i++) {
        int32_t w = (a[i] + b[i]) >> 1;
        int16_t m = op1_sat16(BUS[i] + w);
        int32_t pv = q15(m, tl); if (pv < 0) pv = -pv; if (pv > pk) pk = pv;
        BUS[i] = m;
        l[i] = op1_sat16(l[i] + q15(w, gl));
        r[i] = op1_sat16(r[i] + q15(w, gr));
    }
    fr_meter(pk);
}

/* master FX call: run fx on the instrument part only when chosen and captured this block, else on l / r */
void fr_route(void (*fx)(void *, int16_t *, int16_t *, const int16_t *), void *self, int16_t *l, int16_t *r, const int16_t *params)
{
    if (fr_src == 2) { fr_route_aux(fx, self, l, r, params); return; }
    if (!fr_src || !fr_have) { fx(self, l, r, params); return; }
    fr_have = 0;
    int16_t a[64], b[64];
    if (fr_out) {                                           /* mono to tape */
        int32_t sl = 0, sr = 0, sm = 0;
        for (int i = 0; i < 64; i++) {
            int32_t m = BUS[i]; a[i] = b[i] = (int16_t)m;
            sl += (fr_il[i] * m) >> 8; sr += (fr_ir[i] * m) >> 8; sm += (m * m) >> 8;
        }
        fx(self, a, b, params);
        static int32_t gl, gr;                              /* Q14 track gain of the bus in the mix: kept while the
                                                               bus is quiet (FX tail), smoothed (crackle) */
        if (sm > 2048) {
            while (sm >= 1 << 15) { sm >>= 1; sl >>= 1; sr >>= 1; }   /* sl * 16384 stays in 32 bits */
            sl = sl > 2 * sm ? 2 * sm : sl < -2 * sm ? -2 * sm : sl;
            sr = sr > 2 * sm ? 2 * sm : sr < -2 * sm ? -2 * sm : sr;
            gl += (sl * 16384 / sm - gl) >> 2; gr += (sr * 16384 / sm - gr) >> 2;
        }
        int32_t pk = 0, tl = TAPE_LVL;
        for (int i = 0; i < 64; i++) {
            int32_t w = (a[i] + b[i]) >> 1, d = w - BUS[i];
            int32_t v = (w * tl) >> 15; if (v < 0) v = -v; if (v > pk) pk = v;
            BUS[i] = (int16_t)w;
            l[i] = op1_sat16(l[i] + ((d * gl) >> 14));
            r[i] = op1_sat16(r[i] + ((d * gr) >> 14));
        }
        fr_meter(pk);
        return;
    }
    for (int i = 0; i < 64; i++) { a[i] = fr_il[i]; b[i] = fr_ir[i]; }
    fx(self, a, b, params);
    for (int i = 0; i < 64; i++) { l[i] = op1_sat16(l[i] + a[i] - fr_il[i]); r[i] = op1_sat16(r[i] + b[i] - fr_ir[i]); }
}

static void fr_decode(int v) { if (v >= 0 && v <= 3) { fr_src = v & 1; fr_out = (uint8_t)(v >> 1); } else if (v == 4 || v == 5) { fr_src = 2; fr_out = (uint8_t)(v - 4); } }
static void fr_decode_sends(int32_t v) { for (int t = 0; t < 4; t++) { int s = v >> (7 * t) & 127; fr_send[t] = (uint8_t)(s > 100 ? 100 : s); } }
static int fr_code(void) { return fr_src == 2 ? 4 + fr_out : fr_src + 2 * fr_out; }   /* old values 0..3 stay valid */
static int32_t fr_sends_code(void) { return fr_send[0] | fr_send[1] << 7 | fr_send[2] << 14 | fr_send[3] << 21; }
static void fr_load(void)
{
    int v = op1r_setting_get("mfxsrc", -1), s = op1r_setting_get("auxsend", -1);
    if (v >= 0) fr_decode(v);
    if (s >= 0) fr_decode_sends(s);
    fr_live();
}
static void fr_save(void) { op1r_setting_put("mfxsrc", fr_code()); }
void fr_save_sends(void) { op1r_setting_put("auxsend", fr_sends_code()); }   /* KeyHandler thread */

uint8_t fr_send_dirty;                                       /* set by MIDI: saved at the next key event (dispatcher.c) */
void fr_set_send(int t, int v)                               /* MIDI params CC 114..117 (midi/mx.c), KeyHandler thread */
{
    if (t < 0 || t > 3) return;
    fr_send[t] = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    fr_live(); fr_send_dirty = 1;
}

static int32_t sacc[4];
void fr_send_turn(int t, int delta)                         /* mixer T1, encoder held + turned: send 0..100 */
{
    if ((sacc[t] > 0 && delta < 0) || (sacc[t] < 0 && delta > 0)) sacc[t] = 0;
    sacc[t] += delta;
    int st = sacc[t] / 1024; if (!st) return;
    sacc[t] -= st * 1024;
    int v = fr_send[t] + st;
    fr_send[t] = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    fr_live();
}

static int32_t acc;
static uint8_t fr_dirty;
void fxroute_flush(void) { if (fr_dirty) { fr_dirty = 0; fr_save(); } }   /* at the next key event (as scale_flush) */
void fxroute_turn(int knob, int delta)                      /* blue: source, green: output (instrument only) */
{
    if (knob > 1 || (knob == 1 && !fr_src)) return;
    static int8_t last = -1;
    if (knob != last) { acc = 0; last = (int8_t)knob; }
    if ((acc > 0 && delta < 0) || (acc < 0 && delta > 0)) acc = 0;
    acc += delta;
    int st = acc / 1024; if (!st) return;
    acc = 0;
    uint8_t *t = knob ? &fr_out : &fr_src;
    int v = *t + (st > 0 ? 1 : -1);
    if (v < 0 || v > (knob ? 1 : 2)) return;
    *t = (uint8_t)v; fr_have = fr_snapped = 0;
    fr_live();
    fr_dirty = 1;
}

static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF };
#define FR_S   OP1_Q10(5) / 2                               /* as the settings page */
#define FR_W   0x600
#define FR_LX  OP1_Q10(24)
#define FR_Y1  OP1_Q10(50)
#define FR_Y2  OP1_Q10(80)
#define FR_Y3  OP1_Q10(102)
#define FR_Y0  OP1_Q10(28)

static void fr_init(void *scr) { (void)scr; fr_load(); }
static void fr_update(void *scr, int dt)
{
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    sf_text(r, FR_LX, FR_Y0, FR_S, "FX2 SOURCE", FR_W, c_blue);
    static const char *const src[3] = { "MASTER", "INSTRUMENT", "AUX" };
    sf_text(r, FR_LX, FR_Y1, FR_S, src[fr_src], FR_W, c_blue);
    if (!fr_src) return;                                    /* master source: stereo to master only */
    sf_text(r, FR_LX, FR_Y2, FR_S, "FX2 OUTPUT", FR_W, c_green);
    sf_text(r, FR_LX, FR_Y3, FR_S, fr_out ? "MONO TAPE" : "STEREO MASTER", FR_W, c_green);
}
const op1_screen_t SCREEN_fxroute = { fr_init, fr_update };

/* Sidechain sources for plugins (docs/61, plugin export): this block's 64 samples of a signal other than the FX's own
 * input. Tracks are pre-fader (tr_out): asking for them starts the track work, which runs until 32 blocks after the
 * last ask; until fresh track outs exist (first block) and for an unknown source: zeros, returns 0. */
int op1_sc_source(int src, int16_t *out)
{
    if (src >= OP1_SC_T1 && src <= OP1_SC_TAPE) {
        sc_want = 32;
        if (tr_at != sc_blk) { for (int i = 0; i < 64; i++) out[i] = 0; return 0; }
        if (src != OP1_SC_TAPE) { const int16_t *t = tr_out[src - OP1_SC_T1]; for (int i = 0; i < 64; i++) out[i] = t[i]; }
        else for (int i = 0; i < 64; i++) out[i] = op1_sat16(tr_out[0][i] + tr_out[1][i] + tr_out[2][i] + tr_out[3][i]);
        return 1;
    }
    const int16_t *b = src == OP1_SC_IN ? INPUT : src == OP1_SC_INST ? BUS : 0;
    for (int i = 0; i < 64; i++) out[i] = b ? b[i] : 0;
    return b != 0;
}

#ifndef OP1_HOST
/* stock master FX: their stereo render slots (make_firmware.py DATA_PATCHES) call these; the FX itself unchanged */
typedef void (*fr_fx)(void *, int16_t *, int16_t *, const int16_t *);
#define FR_ST(name, orig) \
    void fr_st_##name(void *self, int16_t *l, int16_t *r, const int16_t *p) { fr_route((fr_fx)(orig), self, l, r, p); }
FR_ST(delay, 0x0197DC42) FR_ST(grid, 0x019AF83C) FR_ST(punch, 0x019E662C) FR_ST(phone, 0x01A1C25C)
FR_ST(spring, 0x01A32FFC) FR_ST(nitro, 0x019A684C) FR_ST(cwo, 0x019450FC)
#endif
