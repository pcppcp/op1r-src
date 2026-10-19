/* "horn" screen for the brass engine: Dr. Seuss-inspired horn with animated parts.
 *
 *   blue knob : timbre (top = most, bottom = least)
 *   green knob: trombone slide extends (0 = closed, full = fully extended)
 *   white knob: balloon inflates (0 = 25% size, full = 100% size)
 *   red knob  : fidget spinner speed (0 = still, full = fast spin)
 *
 * Colors: blue (#4D9EFF), green (#00ED95), white (#FFFFFF), red (#FF3A5D), yellow (#FFD700) */
#include "op1_synth.h"

//static const uint8_t C_BLUE[4]  = { 0x4D, 0x9E, 0xFF, 0xFF };
static const uint8_t C_GREEN[4] = { 0x00, 0xED, 0x95, 0xFF };
//static const uint8_t C_WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
//static const uint8_t C_RED[4]   = { 0xFF, 0x3A, 0x5D, 0xFF };
//static const uint8_t C_YELLOW[4]= { 0xFF, 0xD7, 0x00, 0xFF };

static void *mute1_node, *mute2_node;
static void *green_move_node;
static void *balloon_node;
static void *spinner_node;

static uint32_t spinner_ph, vibrato_ph;

static inline int32_t tsin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }
static inline int32_t tcos(int32_t a) { return tsin(a + 2048); }

/* Build a 3x3 transform matrix (Q10) for scaling around a center point */
static void scale_mat(int32_t m[9], int32_t cx, int32_t cy, int32_t s15)
{
    int32_t s = s15 >> 5;  /* Q15 -> Q10 */
    m[0] = s;  m[1] = 0;   m[2] = 0;
    m[3] = 0;  m[4] = s;   m[5] = 0;
    m[6] = cx - ((cx * s) >> 10);
    m[7] = cy - ((cy * s) >> 10);
    m[8] = 1024;
}

/* Build a 3x3 transform matrix (Q10) for translation */
static void trans_mat(int32_t m[9], int32_t tx, int32_t ty)
{
    m[0] = 1024; m[1] = 0;   m[2] = 0;
    m[3] = 0;    m[4] = 1024;m[5] = 0;
    m[6] = tx;   m[7] = ty;  m[8] = 1024;
}

static void horn_init(void *scr)
{
    mute1_node = op1_find_node(scr, "blue-1");
    mute2_node = op1_find_node(scr, "blue-2");
    green_move_node = op1_find_node(scr, "green-move");

    balloon_node = op1_find_node(scr, "white");
    spinner_node = op1_find_node(scr, "red-spinner");
    spinner_ph = 0;
}

static void horn_update(void *scr, int dt)
{
    int16_t k[4];
    op1_synth_knobs_live(OP1_SCREEN_SLOT & 7, k);

    /* Blue knob: mute rotation (0 = closed, full = 90 deg open) */
    if (mute1_node) {
        int32_t a = -(int32_t)(32767 - k[0]) * 1820 >> 15;  /* 0..-1820 = 0..-80 deg */
        op1_node_rotate(mute1_node, OP1_Q10(102.8), OP1_Q10(33.9), tsin(a), tcos(a));
    }
    if (mute2_node) {
        int32_t a = -(int32_t)(32767 - k[0]) * 1820 >> 15;
        op1_node_rotate(mute2_node, OP1_Q10(36.5), OP1_Q10(69.7), tsin(a), tcos(a));
    }

    /* Green knob: trombone slide extension (0 = closed, full = extended) */
    if (green_move_node) {
        int32_t ext = (int32_t)k[1] * 60 >> 15;  /* 0..60 px */
        int32_t m[9];
        trans_mat(m, OP1_Q10(ext), OP1_Q10(0));
        op1_node_set_transform(green_move_node, m);
    }
    /* Green knob: draw stretch lines with moving right endpoints */
    {
        int32_t ext = (int32_t)k[1] * 61440 >> 15;  /* Q10: 0..60 */
        void *r = op1_raster();
        static const int16_t y4[] = { 14, 47, 20, 41 };
        for (int i = 0; i < 4; i++) {
            int32_t x0 = OP1_Q10(184.6);
            int32_t x1 = OP1_Q10(206.7) + ext;
            int32_t y = OP1_Q10(y4[i]);
            op1_pt a = { x0, y }, b = { x1, y };
            op1_raster_line(r, &a, &b, OP1_Q10(2), C_GREEN);
        }
    }

    /* White knob: balloon scale (0 = 25%, full = 100%) + vibrato shake */
    if (balloon_node) {
        int32_t s = 8192 + ((int32_t)k[2] * 24575 >> 15);  /* Q10: 256..1024 */
        if (k[2] > 0) {
            vibrato_ph += (uint32_t)k[2] * dt >> 11;
            int32_t shake = tsin(vibrato_ph) * 102 >> 15;  /* ±102 in Q10 ≈ ±10% */
            s += shake;
        }
        int32_t m[9];
        scale_mat(m, OP1_Q10(83), OP1_Q10(126), s);
        op1_node_set_transform(balloon_node, m);
    }

    /* Red knob: fidget spinner speed */
    if (spinner_node && k[3] > 0) {
        spinner_ph += (uint32_t)k[3] * dt >> 9;
        int32_t angle = spinner_ph & 8191;
        op1_node_rotate(spinner_node, OP1_Q10(208.1), OP1_Q10(104.8), tsin(angle), tcos(angle));
    }

    op1_screen_draw(scr, dt);
}

const op1_screen_t SCREEN_horn = { horn_init, horn_update };
