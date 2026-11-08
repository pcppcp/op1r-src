/* Example engine 2: 2-operator FM bell/e-piano.
 *   knob 0: modulator ratio (0.5 .. 8, quantised to half steps)
 *   knob 1: modulation index
 *   knob 2: index decay speed (bell-ness)
 *   knob 3: feedback on modulator
 */
#include "op1_plugin.h"

typedef struct {
    uint32_t vtable;          /* firmware-owned */
    uint32_t pc, pm;          /* carrier / modulator phase */
    int32_t  env;             /* modulation-index envelope, Q15 */
    int16_t  fb;              /* last modulator output */
    uint8_t  gate;            /* key held (for the screen only) */
} fm_voice_t;

static inline int32_t sine(uint32_t ph) { return op1_sin(((int32_t)ph >> 19) + 4096); }

static void fm_note_on(void *self, const int16_t *params)
{
    fm_voice_t *v = (fm_voice_t *)self; (void)params;
    v->pc = 0; v->pm = 0; v->env = 32767; v->fb = 0; v->gate = 1;
}

static void fm_note_off(void *self) { ((fm_voice_t *)self)->gate = 0; }

static void fm_render(void *self, int16_t *out, const int16_t *params)
{
    fm_voice_t *v = (fm_voice_t *)self;
    const int32_t *inc = OP1_PITCH_INC;
    uint32_t ratio2 = 1 + (params[0] >> 11);            /* 1..16 -> ratio 0.5..8 */
    int32_t  index  = params[1] >> 3;                     /* 0..4095 */
    int32_t  decay  = 1 + (params[2] >> 9);               /* per-block decay step */
    int32_t  fbamt  = params[3] >> 4;
    uint32_t pc = v->pc, pm = v->pm; int32_t fb = v->fb;
    int32_t  idx = (index * v->env) >> 15;
    int32_t  vg = op1_voice_vel_gain(self);              /* note velocity */

    for (int n = 0; n < OP1_BLOCK; n++) {
        uint32_t i = (uint32_t)inc[n];
        pm += (i >> 1) * ratio2;
        int32_t m = sine(pm + (uint32_t)(fb * fbamt * 32));
        fb = m;
        pc += i;
        out[n] = (int16_t)((sine(pc + (uint32_t)(m * idx * 16)) * vg) >> 16);   /* half scale: six voices stay near TE's engines on the synth bus */
    }
    v->env -= (v->env * decay + 4096) >> 13;             /* per block, 689 / s (88.2 kHz voices): half the old >> 12 step, same floor */
    if (v->env < 0) v->env = 0;
    v->pc = pc; v->pm = pm; v->fb = (int16_t)fb;
}

const op1_engine_t ENGINE_fmbell = { fm_note_on, fm_render, fm_note_off };

/* ======================= screen (GUI thread) =======================
 * display/fmbell.svg: line-art bell + clapper group, four dials in the corners (bar0..3).
 * Drawn here each frame: strike rings left/right of the bell (green, count = index knob, length =
 * live modulation envelope), clapper swing (live envelope). */
#define BELL_X   160
#define BELL_Y   78
#define PIVOT_Y  46
static const int DIAL_X[4] = { 36, 284, 36, 284 };
static const int DIAL_Y[4] = { 32, 32, 128, 128 };
static const uint8_t COL_RING[4] = { 0x00, 0xED, 0x95, 0xFF };   /* green = index */
static void *bars[4], *clapper;
static int32_t level;            /* displayed strike level, Q15, falls slowly after release */
static uint32_t swing_ph;        /* clapper swing phase, 8192 = one period */

static void fm_screen_init(void *scr)
{
    static const char *ids[4] = { "bar0", "bar1", "bar2", "bar3" };
    for (int i = 0; i < 4; i++) bars[i] = op1_find_node(scr, ids[i]);
    clapper = op1_find_node(scr, "clapper");
}

/* sine/cosine for a = 0..8191 per turn, Q15 (table[k] = sin(2*pi*k/8192), see op1_synth.h) */
static inline int32_t tsin(int32_t a) { return op1_sin(a & 8191); }
static inline int32_t tcos(int32_t a) { return tsin(a + 2048); }

static void arc(void *r, int32_t rad, int32_t a0, int32_t a1)    /* angles 0..8191, 0 = right, CW */
{
    int32_t step = (a1 - a0) / 6; if (!step) step = 1;
    op1_pt p = { OP1_Q10(BELL_X) + rad * tcos(a0) / 32, OP1_Q10(BELL_Y) + rad * tsin(a0) / 32 };
    for (int32_t a = a0 + step; ; a += step) {
        if (a > a1) a = a1;
        op1_pt q = { OP1_Q10(BELL_X) + rad * tcos(a) / 32, OP1_Q10(BELL_Y) + rad * tsin(a) / 32 };
        op1_raster_line(r, &p, &q, 0x600, COL_RING);
        p = q;
        if (a == a1) break;
    }
}

static void fm_screen_update(void *scr, int dt)
{
    int slot = OP1_SCREEN_SLOT & 7;
    int16_t k[4];
    op1_synth_knobs_live(slot, k);

    /* live strike level: loudest held voice's modulation envelope; slow fall after release */
    int32_t tgt = 0;
    for (int i = 0; i < OP1_VOICES; i++) {
        const fm_voice_t *v = (const fm_voice_t *)OP1_VOICE(i);
        if (v->gate == 1 && v->env > tgt && v->env <= 32767) tgt = v->env;
    }
    level = tgt > level ? tgt : level - (level >> 4) - 64;
    if (level < 0) level = 0;
    swing_ph += 340;

    for (int i = 0; i < 4; i++) if (bars[i]) {                     /* dials: -135..+135 degrees */
        int32_t a = ((int32_t)k[i] * 6144 >> 15) - 3072;
        op1_node_rotate(bars[i], OP1_Q10(DIAL_X[i]), OP1_Q10(DIAL_Y[i]), tsin(a), tcos(a));
    }
    if (clapper) {                                                 /* swing up to ~ +-11 degrees */
        int32_t a = ((tsin((int32_t)swing_ph) * (level >> 5)) >> 15) * 250 >> 10;   /* +-250 = 11 deg */
        op1_node_rotate(clapper, OP1_Q10(BELL_X), OP1_Q10(PIVOT_Y), tsin(a), tcos(a));
    }
    op1_screen_draw(scr, dt);

    /* strike rings: 1..4 from the index knob, arc span 8..35 degrees with the live level */
    void *r = op1_raster();
    int n = 1 + ((int32_t)k[1] * 4 >> 15); if (n > 4) n = 4;
    int32_t half = 180 + (level * 620 >> 15);                     /* half span, 8192 = 360 deg */
    for (int i = 0; i < n; i++) {
        int32_t rad = 56 + i * 9;
        int32_t h = half - i * 40; if (h < 120) h = 120;
        arc(r, rad, -h, h);                                        /* right side */
        arc(r, rad, 4096 - h, 4096 + h);                           /* left side  */
    }
}

const op1_screen_t SCREEN_fmbell = { fm_screen_init, fm_screen_update };
