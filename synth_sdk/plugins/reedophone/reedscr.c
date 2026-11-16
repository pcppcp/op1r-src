/* reedophone's screen: clarinet sections, a hair dryer blowing into the mouthpiece, a saw, a vent, a microphone at
 * the bell (reedscr.svg, made by gen_screen.py from the hand-drawn reedophone.svg).
 *
 *   blue  : the dryer's dial turns (9 o'clock .. 3 o'clock, clockwise); more dryer wind lines (1 .. 7)
 *   green : the saw slides left / right (+-10 px); off centre it touches a section, which vibrates
 *           (left: body_none, right: body_vent), harder further out. Its blade always spins
 *   white : the microphone rises (up to 30 px)
 *   orange: the vent slides down (up to 38 px); more vent wind lines (0 .. 5)
 *
 * While a key is held the dryer's trigger is pressed, and the key's valve opens ((MIDI note + 7) mod 24 -> keys 1..24).
 * The wind comes on one line at a time, keeping pace with the envelope's attack (its share of the lines = the
 * envelope's share of its peak; all of them once it peaks): the dryer lines in order, then the vent lines, then the
 * bell's. After the last key is released they go off in the same order, one every 50 ms.
 *
 * Copyright (C) 2026 nvmsocool. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"
#pragma GCC optimize("Os")

extern volatile uint8_t rv_scr_gate[OP1_VOICES], rv_scr_note[OP1_VOICES];   /* reedophone.c */
extern volatile uint32_t rv_scr_tick[OP1_VOICES];
extern volatile int16_t rv_scr_att[OP1_VOICES];                              /* attack progress, Q15 */

#include "reedscr_geom.h"   /* DIAL_*_Q10 (dryer dial centre), HUB_*_Q10 (saw blade hub): from the drawing */
#define SAW_DX     10       /* saw travel each way, px */
#define VENT_DY    38       /* vent travel down, px */
#define MIC_DY     30       /* microphone travel up, px */
#define SAW_DEAD   3277     /* green within 10 % of centre: the saw touches nothing */
                            /* vibration: up to 1.5 px at the knob's ends */
#define SWEEP_MS   50       /* wind lines going out: one every 50 ms */
#define N_DW       7
#define N_VW       5
#define N_KEYS     24
#define KEY_OFS    7        /* added to the MIDI note before the mod 24 that picks the valve */

static void *n_dial, *n_saw, *n_blade, *n_vent, *n_mic, *n_trig_on, *n_trig_off;
static void *n_bnone, *n_bvent, *n_bvent_b, *n_bell;
static void *n_dw[N_DW], *n_vw[N_VW], *n_open[N_KEYS], *n_closed[N_KEYS];
static uint32_t last_tick[OP1_VOICES];
static uint32_t blade_a;            /* blade angle, 8192 = one turn */
static uint32_t rng = 0x2545F491u;
static int was_held;
/* wind sweep: dryer lines [d_off, d_on) lit, vent lines [v_off, v_on), the bell's while bell_on */
static int d_on, d_off, v_on, v_off, bell_on, releasing, sweep_ms;
static int32_t att_prev;            /* attack progress last frame; att_done once it stops rising */
static int att_done;

static inline int32_t tsin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }
static inline int32_t tcos(int32_t a) { return OP1_SINE_TABLE[(a + 2048) & 8191]; }

static void vis(void *node, int on) { if (node) op1_node_set_visible(node, on); }
static void move(void *node, int32_t dx, int32_t dy)                                /* Q10 */
{
    if (!node) return;
    int32_t m[9] = { 1024, 0, 0, 0, 1024, 0, dx, dy, 1024 };
    op1_node_set_transform(node, m);
}
/* rotate by angle a (8192 = one turn, + = clockwise) about (cx, cy), then shift by (dx, dy); all Q10 */
static void turn(void *node, int32_t a, int32_t cx, int32_t cy, int32_t dx, int32_t dy)
{
    if (!node) return;
    int32_t c = tcos(a) >> 5, s = tsin(a) >> 5;
    int32_t m[9] = { c, s, 0, -s, c, 0, cx - ((cx * c - cy * s) >> 10) + dx, cy - ((cx * s + cy * c) >> 10) + dy, 1024 };
    op1_node_set_transform(node, m);
}
static int32_t jitter(int32_t amp)                                                  /* -amp .. amp */
{
    rng = rng * 1664525u + 1013904223u;
    return (int32_t)(int16_t)(rng >> 16) * amp >> 15;
}

static void rs_init(void *scr)
{
    static const char *const DW[N_DW] = { "dw1", "dw2", "dw3", "dw4", "dw5", "dw6", "dw7" };
    static const char *const VW[N_VW] = { "vw1", "vw2", "vw3", "vw4", "vw5" };
    char id[4];
    n_dial = op1_find_node(scr, "dial");
    n_saw = op1_find_node(scr, "saw");
    n_blade = op1_find_node(scr, "blade");
    n_vent = op1_find_node(scr, "vent");
    n_mic = op1_find_node(scr, "mic");
    n_trig_on = op1_find_node(scr, "trigger_on");
    n_trig_off = op1_find_node(scr, "trigger_off");
    n_bnone = op1_find_node(scr, "body_none");
    n_bvent = op1_find_node(scr, "body_vent");
    n_bvent_b = op1_find_node(scr, "body_vent2");
    n_bell = op1_find_node(scr, "bell-winds");
    for (int i = 0; i < N_DW; i++) n_dw[i] = op1_find_node(scr, DW[i]);
    for (int i = 0; i < N_VW; i++) n_vw[i] = op1_find_node(scr, VW[i]);
    for (int i = 0; i < N_KEYS; i++) {                                             /* "o1" .. "c24" */
        int k = i + 1, p = 1;
        if (k >= 10) id[p++] = (char)('0' + k / 10);
        id[p++] = (char)('0' + k % 10); id[p] = 0;
        id[0] = 'o'; n_open[i] = op1_find_node(scr, id);
        id[0] = 'c'; n_closed[i] = op1_find_node(scr, id);
    }
    for (int i = 0; i < OP1_VOICES; i++) last_tick[i] = rv_scr_tick[i];
    was_held = 0; d_on = d_off = v_on = v_off = bell_on = releasing = sweep_ms = 0; att_prev = 0; att_done = 0;
}

/* one sweep step: on = light the next line (dryer, vent, bell), else put out the next lit one in the same order */
static void sweep_step(int nd, int nv)
{
    if (!releasing) {
        if (d_on < nd) d_on++;
        else if (v_on < nv) v_on++;
        else bell_on = 1;
    } else {
        if (d_off < d_on) d_off++;
        else if (v_off < v_on) v_off++;
        else if (bell_on) bell_on = 0;
        else { d_on = d_off = v_on = v_off = 0; releasing = 0; }                    /* all out */
    }
}

static void rs_update(void *scr, int dt)
{
    int16_t k[4];
    op1_synth_knobs_live(OP1_SCREEN_SLOT & 7, k);
    if (dt < 0) dt = 0;
    if (dt > 100) dt = 100;
    for (int i = 0; i < 4; i++) if (k[i] < 0) k[i] = 0;

    /* held voices (gate on and still rendering) and the valves their notes open */
    uint32_t open = 0;
    int held = 0;
    int32_t att = 0;
    for (int i = 0; i < OP1_VOICES; i++) {
        uint32_t t = rv_scr_tick[i];
        if (rv_scr_gate[i] && t != last_tick[i]) {
            uint32_t nt = rv_scr_note[i] + KEY_OFS;
            held = 1;
            if (rv_scr_att[i] > att) att = rv_scr_att[i];
            while (nt >= N_KEYS) nt -= N_KEYS;                                      /* note mod 24 (no divide here) */
            open |= 1u << nt;
        }
        last_tick[i] = t;
    }
    for (int i = 0; i < N_KEYS; i++) { int o = (open >> i) & 1; vis(n_open[i], o); vis(n_closed[i], !o); }
    vis(n_trig_on, held); vis(n_trig_off, !held);

    /* wind lines: how many the knobs allow, swept on from the first key, off after the last */
    int nd = 1 + (((int32_t)k[0] * (N_DW - 1) + 16384) >> 15);                   /* blue: 1 .. 7 */
    int nv = ((int32_t)k[3] * N_VW + 16384) >> 15;                                  /* orange: 0 .. 5 */
    if (held && !was_held) { d_on = d_off = v_on = v_off = bell_on = 0; releasing = 0; att_prev = 0; att_done = 0; }
    if (!held && was_held) { releasing = 1; sweep_ms = SWEEP_MS; }
    was_held = held;
    if (d_on > nd) d_on = nd;                                                       /* knob turned down: fewer */
    if (v_on > nv) v_on = nv;
    if (d_off > d_on) d_off = d_on;
    if (v_off > v_on) v_off = v_on;
    if (held) {                         /* coming on with the attack: lit share = the envelope's share of its peak */
        if (att <= att_prev && att_prev > 0) att_done = 1;                          /* peaked (decay / sustain) */
        att_prev = att;
        int tot = nd + nv + 1, lit = d_on + v_on + bell_on;
        int want = att_done ? tot : (tot * att + 16384) >> 15;
        if (lit < want) sweep_step(nd, nv);                                         /* at most one a frame */
    } else if (releasing) {             /* going out: one every 50 ms */
        sweep_ms += dt;
        while (sweep_ms >= SWEEP_MS) { sweep_ms -= SWEEP_MS; sweep_step(nd, nv); }
    }
    for (int i = 0; i < N_DW; i++) vis(n_dw[i], i >= d_off && i < d_on);
    for (int i = 0; i < N_VW; i++) vis(n_vw[i], i >= v_off && i < v_on);
    vis(n_bell, bell_on);

    /* blue: the dryer's dial, 0 .. 180 deg clockwise */
    turn(n_dial, (int32_t)k[0] * 4096 >> 15, DIAL_X_Q10, DIAL_Y_Q10, 0, 0);

    /* green: the saw (+ its spinning blade); off centre it shakes the section it touches */
    int32_t g = (int32_t)k[1] - 16384;
    int32_t sx = g * SAW_DX >> 4;                                                   /* Q10, +-10 px */
    blade_a += (uint32_t)dt * 12;                                                   /* ~0.7 s a turn */
    move(n_saw, sx, 0);
    turn(n_blade, (int32_t)(blade_a & 8191), HUB_X_Q10, HUB_Y_Q10, sx, 0);
    int32_t d = g < 0 ? -g : g, shake = 0;
    if (d > SAW_DEAD) shake = (d - SAW_DEAD) * 120 >> 10;                          /* 0 .. SHAKE_MAX */
    int32_t jx = shake ? jitter(shake) : 0, jy = shake ? jitter(shake >> 1) : 0;
    int32_t ln = g < 0, rt = g > 0;
    move(n_bnone, ln ? jx : 0, ln ? jy : 0);
    move(n_bvent, rt ? jx : 0, rt ? jy : 0);
    move(n_bvent_b, rt ? jx : 0, rt ? jy : 0);

    /* orange: the vent slides down (inside body_vent: it shakes with it) */
    int32_t vy = (int32_t)k[3] * VENT_DY >> 5;                                      /* Q10 */
    move(n_vent, rt ? jx : 0, vy + (rt ? jy : 0));

    /* white: the microphone rises */
    move(n_mic, 0, -((int32_t)k[2] * MIC_DY >> 5));

    op1_screen_draw(scr, dt);
}

const op1_screen_t SCREEN_reedscr = { rs_init, rs_update };
