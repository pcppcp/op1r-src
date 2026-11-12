#pragma GCC optimize ("no-tree-loop-distribute-patterns") /* no implicit libc calls */
/* perc's page drawing (GUI thread), shared by the synth engine (perc_screen.c) and the drum kit (percdrum): the sound as a
 * neon line drawing on a stage.
 *
 *   the drum      white lines, one drawing per type; its head (blue) is as large as the pitch is low
 *   shockwave     green rings from the drum on every hit, wider and slower with the length
 *   strike        a white starburst at the hit, longer with the punch
 *   floor         an orange perspective grid, denser with a brighter tone; it shimmers with the noise
 *   bottom row    the four encoders (labels in their colours, values in white); Shift: type, drive, level (, morph)
 *
 * Everything is drawn in code (the SVG is empty). GPL-3.0-or-later with the OP1R firmware permission. */
#include "op1_synth.h"                    /* stock-font text (firmware calls, no imports) */
#include "perc_draw.h"
#include "perc_tables.h"
#include "perc_dsp.h"

#define Q(v) ((int32_t)(v) * 1024)
#define TS 12
#define W   0x600                 /* line width, Q10 */
static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     c_grey[4] = { 0x6F, 0x71, 0x73, 0xFF };
static const uint8_t *const col4[4] = { c_blue, c_green, c_white, c_red };

enum { TX_NAME, TX_IDX, TX_LAB, TX_VAL = 6, TX_N = 10 };
static uint32_t tx[TX_N][OP1_TEXT_SIZE / 4];
static char shown[TX_N][14];
static uint8_t tx_made;

static void *rr;                                     /* the frame's raster */
static void ln(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)   /* Q10 */
{
    if (x0 == x1 && y0 == y1) return;
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(rr, &a, &b, w, c);
}
static void txt(int i, const char *s)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    int same = 1;
    for (int j = 0; j < 14; j++) { if (shown[i][j] != s[j]) { same = 0; break; } if (!s[j]) break; }
    if (!same) { op1_text_set(tx[i], s); int j = 0; for (; j < 13 && s[j]; j++) shown[i][j] = s[j]; shown[i][j] = 0; }
    op1_text_draw(tx[i], rr, ident);
}
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *num(char *p, int32_t x) { char t[10]; int n = 0; if (x < 0) { *p++ = '-'; x = -x; } do t[n++] = (char)('0' + x % 10); while ((x /= 10)); while (n) *p++ = t[--n]; *p = 0; return p; }
static char *snum(char *p, int32_t x) { if (x > 0) *p++ = '+'; return num(p, x); }
static char *dec1(char *p, int32_t x10) { if (x10 < 0) { *p++ = '-'; x10 = -x10; } p = num(p, x10 / 10); *p++ = '.'; return num(p, x10 % 10); }

static inline int32_t sn(int32_t a) { return op1_sin(a & 8191); }
static inline int32_t cs(int32_t a) { return op1_sin((a + 2048) & 8191); }
/* ellipse arc: centre Q10, radii Q10, angles 8192 a turn (0 = right, 2048 = down), n segments */
static void arc(int32_t cx, int32_t cy, int32_t rx, int32_t ry, int32_t a0, int32_t a1, int n, int32_t w, const uint8_t *c)
{
    int32_t px = 0, py = 0;
    for (int i = 0; i <= n; i++) {
        int32_t a = a0 + (a1 - a0) * i / n;
        int32_t x = cx + (int32_t)((int64_t)rx * cs(a) >> 15), y = cy + (int32_t)((int64_t)ry * sn(a) >> 15);
        if (i) ln(px, py, x, y, w, c);
        px = x; py = y;
    }
}
static void ell(int32_t cx, int32_t cy, int32_t rx, int32_t ry, int n, int32_t w, const uint8_t *c) { arc(cx, cy, rx, ry, 0, 8192, n, w, c); }
static void dot(int32_t x, int32_t y, int32_t r, const uint8_t *c) { ln(x - r, y, x + r, y, 0x700, c); ln(x, y - r, x, y + r, 0x700, c); }
static void tripod(int32_t x, int32_t top, int32_t foot, int32_t spread, const uint8_t *c)
{
    ln(x, top, x, foot - Q(6), W, c);
    ln(x, foot - Q(6), x - spread, foot, W, c);
    ln(x, foot - Q(6), x + spread, foot, W, c);
}
/* a drum seen from the front and a little above: head ellipse (blue), shell sides, lower rim */
static void shell(int32_t cx, int32_t top, int32_t rx, int32_t h, const uint8_t *head)
{
    int32_t ry = rx / 3;
    ell(cx, top, rx, ry, 24, W, head);
    ln(cx - rx, top, cx - rx, top + h, W, c_white);
    ln(cx + rx, top, cx + rx, top + h, W, c_white);
    arc(cx, top + h, rx, ry, 0, 4096, 12, W, c_white);
    for (int i = 1; i <= 3; i++) {                        /* lugs */
        int32_t x = cx - rx + 2 * rx * i / 4;
        int32_t y = top + (int32_t)((int64_t)ry * sn(2048 + (int32_t)((int64_t)(x - cx) * 2048 / rx)) >> 15);
        ln(x, y + Q(3), x, y + h - Q(2), 0x400, c_grey);
    }
}

/* the drawings: centre (cx, cy) Q10, size s Q10 (1024 = nominal), head colour */
static void draw_type(int type, int32_t cx, int32_t cy, int32_t s, const uint8_t *head)
{
#define S(v) ((int32_t)((int64_t)Q(v) * s >> 10))
    const int32_t foot = Q(96);
    switch (type) {
    case 0:                                               /* kick: front head, port, spurs */
        ell(cx, cy, S(30), S(30), 40, W, c_white);
        ell(cx, cy, S(26), S(26), 40, W, head);
        ell(cx + S(10), cy + S(9), S(6), S(6), 16, W, c_white);
        ln(cx - S(24), cy + S(18), cx - S(34), foot, W, c_white);
        ln(cx + S(24), cy + S(18), cx + S(34), foot, W, c_white);
        break;
    case 1:                                               /* snare: shell on a stand, wires under */
        shell(cx, cy - S(8), S(26), S(14), head);
        for (int i = -2; i <= 2; i++) ln(cx + S(5 * i), cy + S(6), cx + S(5 * i), cy + S(10), 0x400, c_grey);
        tripod(cx, cy + S(12), foot, S(16), c_white);
        break;
    case 2:                                               /* hi-hat: two cymbals on a stand */
        ell(cx, cy - S(14), S(28), S(5), 24, W, head);
        ell(cx, cy - S(8), S(28), S(5), 24, W, c_white);
        ln(cx, cy - S(24), cx, cy - S(14), W, c_white);
        tripod(cx, cy - S(8), foot, S(18), c_white);
        break;
    case 3:                                               /* clap: two palms meeting, the burst between them */
        for (int e = -1; e <= 1; e += 2) {
            ell(cx + e * S(9), cy, S(8), S(19), 24, W, c_white);
            ln(cx + e * S(9), cy + S(19), cx + e * S(12), foot - Q(14), W, c_white);       /* wrists */
        }
        for (int i = 0; i < 3; i++) ln(cx - S(4) + S(4 * i), cy - S(24), cx - S(8) + S(8 * i), cy - S(32), 0x500, head);
        break;
    case 4:                                               /* conga: tall tapered barrel */
        ell(cx, cy - S(22), S(16), S(5), 20, W, head);
        arc(cx - S(18), cy, S(6), S(22), -2048, 2048, 10, W, c_white);
        arc(cx + S(18), cy, S(6), S(22), 2048, 6144, 10, W, c_white);
        arc(cx, cy + S(22), S(12), S(4), 0, 4096, 10, W, c_white);
        ln(cx, cy + S(26), cx, foot, 0x500, c_grey);
        break;
    case 5:                                               /* tom: wide shell on an arm */
        shell(cx, cy - S(12), S(28), S(22), head);
        ln(cx - S(28), cy, cx - S(42), cy + S(6), W, c_white);
        ln(cx - S(42), cy + S(6), cx - S(42), foot, W, c_white);
        break;
    case 6:                                               /* cymbal: big tilted plate, bell, stand */
        ell(cx, cy - S(14), S(40), S(7), 32, W, head);
        ell(cx, cy - S(16), S(8), S(3), 12, W, c_white);
        tripod(cx, cy - S(12), foot, S(20), c_white);
        break;
    case 7:                                               /* rimshot: snare with a stick across the rim */
        shell(cx, cy - S(6), S(26), S(14), head);
        ln(cx - S(34), cy - S(22), cx + S(20), cy - S(4), 0x800, c_white);
        tripod(cx, cy + S(10), foot, S(16), c_white);
        break;
    case 8:                                               /* cowbell: a bell with its mouth and clamp */
        ln(cx - S(10), cy - S(20), cx + S(10), cy - S(20), W, c_white);
        ln(cx - S(10), cy - S(20), cx - S(18), cy + S(14), W, c_white);
        ln(cx + S(10), cy - S(20), cx + S(18), cy + S(14), W, c_white);
        ell(cx, cy + S(14), S(18), S(5), 20, W, head);
        arc(cx, cy - S(20), S(5), S(6), 4096, 8192, 8, W, c_white);
        break;
    case 9:                                               /* shaker: a capsule with seeds */
        arc(cx - S(18), cy, S(10), S(10), 2048, 6144, 12, W, c_white);
        arc(cx + S(18), cy, S(10), S(10), -2048, 2048, 12, W, c_white);
        ln(cx - S(18), cy - S(10), cx + S(18), cy - S(10), W, c_white);
        ln(cx - S(18), cy + S(10), cx + S(18), cy + S(10), W, c_white);
        for (int i = 0; i < 5; i++) dot(cx - S(16) + S(8 * i), cy + S((i & 1) ? 3 : -3), S(1), head);
        break;
    default:                                              /* a transferred PO-32 sound: a pad with its number */
        ln(cx - S(22), cy - S(22), cx + S(22), cy - S(22), W, c_white);
        ln(cx + S(22), cy - S(22), cx + S(22), cy + S(22), W, c_white);
        ln(cx + S(22), cy + S(22), cx - S(22), cy + S(22), W, c_white);
        ln(cx - S(22), cy + S(22), cx - S(22), cy - S(22), W, c_white);
        ell(cx, cy, S(12), S(12), 24, W, head);
        break;
    }
#undef S
}

/* the newest hit: frames since it (shockwave age) and its velocity */
static uint32_t last_seq, age = 1000;
static int32_t hit_vel;
static uint32_t frame;

void perc_draw_page(void *scr, int dt, const perc_view_t *pv)
{
    const int16_t *k = pv->k;
    if (!tx_made) {
        op1_text_ctor(tx[TX_NAME], Q(8), 22, TS, c_white);
        op1_text_ctor(tx[TX_IDX], Q(262), 4, TS, c_grey);    /* top right, where drum+ shows its name */
        for (int i = 0; i < 4; i++) {
            op1_text_ctor(tx[TX_LAB + i], Q(8 + 78 * i), 114, TS, col4[i]);
            op1_text_ctor(tx[TX_VAL + i], Q(8 + 78 * i), 127, TS, c_white);
        }
        tx_made = 1;
    }
    frame++;
    if (pv->hit_seq != last_seq) { last_seq = pv->hit_seq; age = 0; hit_vel = pv->hit_vel; } else if (age < 1000) age++;
    op1_screen_draw(scr, dt);
    rr = op1_raster();
    int type = pv->type;
    /* floor: orange perspective grid, lines denser with the tone */
    const int32_t vx = Q(160), hy = Q(98), by = Q(107);
    uint8_t cf[4] = { 0xFF, 0x3A, 0x5D, 0xFF };
    int32_t glow = age < 30 ? 30 - (int32_t)age : 0;                  /* the floor flashes with a hit */
    for (int i = 0; i < 3; i++) cf[i] = (uint8_t)(c_red[i] * (55 + glow * 6) >> 8);
    int nv = 3 + (k[3] * 4 >> 15);
    for (int i = -nv; i <= nv; i++) ln(vx + i * Q(16), hy, vx + i * Q(40), by, 0x300, cf);
    for (int j = 0; j < 3; j++) {                                      /* rows run towards the viewer after a hit */
        int32_t t = (int32_t)((j * 341 + (age < 1000 ? age * 24 : 0)) & 1023);
        int32_t y = hy + (int32_t)((int64_t)(by - hy) * t * t >> 20);
        int32_t half = Q(16) * nv + (int32_t)((int64_t)(Q(40) - Q(16)) * nv * t >> 10);
        ln(vx - half, y, vx + half, y, 0x300, cf);
    }

    /* shockwave: rings out of the drum, as far and as slow as the length */
    const int32_t cx = Q(160), cy = Q(60);
    int32_t len = k[1];                                                /* 0 .. 32767 */
    if (age < 200 && hit_vel) {
        int32_t speed = 6 - (len * 4 >> 15);                           /* px per frame */
        for (int ring = 0; ring < 3; ring++) {
            int32_t a = (int32_t)age - ring * 6;
            if (a < 0) continue;
            int32_t rx = Q(34) + Q(speed) * a, ry = rx / 3;
            int32_t fade = 255 - a * (12 - (len * 9 >> 15));
            if (fade <= 0 || rx > Q(160)) continue;
            uint8_t cg[4]; for (int i = 0; i < 3; i++) cg[i] = (uint8_t)((((int32_t)c_green[i] * fade) >> 8) * hit_vel >> 15); cg[3] = 255;
            ell(cx, cy + Q(4), rx, ry, 40, 0x500, cg);
        }
    }
    /* strike: a white starburst on the hit, longer with the punch */
    if (age < 8) {
        int32_t l = Q(4) + (Q(14) * k[2] >> 15);
        l = l * (8 - (int32_t)age) / 8;
        for (int i = 0; i < 8; i++) {
            int32_t a = i * 1024 + 512;
            int32_t r0 = Q(32);
            ln(cx + (int32_t)((int64_t)r0 * cs(a) >> 15), cy + (int32_t)((int64_t)r0 * sn(a) >> 15) / 2,
               cx + (int32_t)((int64_t)(r0 + l) * cs(a) >> 15), cy + (int32_t)((int64_t)(r0 + l) * sn(a) >> 15) / 2, 0x500, c_white);
        }
    }
    /* the drum: smaller with a higher pitch */
    int32_t sz = 1024 - ((k[0] - 16384) >> 6);                        /* 0.75 .. 1.25 */
    draw_type(type, cx, cy, sz, pv->has_patch ? c_blue : c_grey);

    /* text: the sound, its place */
    char v[16];
    txt(TX_NAME, pv->name);
    txt(TX_IDX, pv->idx);

    /* the bottom row */
    for (int i = 0; i < 4; i++) ln(Q(8 + 78 * i), Q(110), Q(78 + 78 * i), Q(110), 0x800, col4[i]);
    /* each lane shows what its knob edits now: page 2 (pressed, or Shift held) or the knob */
    int sh = pv->shift;
    if (sh || pv->page2[0]) { txt(TX_LAB, pv->blue2); txt(TX_VAL, type < PERC_FACTORY_COUNT ? PERC_FACTORY_NAME[type] : "po-32"); }
    else { int32_t st = ((k[0] - 16384) * 24 + 8192) >> 14; txt(TX_LAB, "pitch"); put(snum(v, st), " st"); txt(TX_VAL, v); }
    if (sh || pv->page2[1]) { txt(TX_LAB + 1, "drive"); put(num(v, ((pv->drive > 0 ? pv->drive : 0) * 100 + 16383) >> 15), "%"); txt(TX_VAL + 1, v); }
    else {   /* length: x 10^((k - 16384) / 16384), shown as x0.1 .. x10 */
        int32_t lg = ((k[1] - 16384) * 13 + ((k[1] - 16384) >> 2));
        int32_t f10 = (int32_t)(10 * (int64_t)perc_pow2(lg, 16) >> 16);
        txt(TX_LAB + 1, "length"); { char *p = put(v, "x"); dec1(p, f10); } txt(TX_VAL + 1, v);
    }
    if (sh || pv->page2[2]) {
        int32_t db10 = pv->level > 0 ? (pv->level - 16384) * 120 / 16384 : 0;
        txt(TX_LAB + 2, "level"); { char *p = v; if (db10 > 0) *p++ = '+'; put(dec1(p, db10), "db"); } txt(TX_VAL + 2, v);
    } else { txt(TX_LAB + 2, "punch"); put(snum(v, ((k[2] - 16384) * 100) >> 14), "%"); txt(TX_VAL + 2, v); }
    if (sh && pv->morph_lane == 1) { txt(TX_LAB + 3, "morph"); put(num(v, (pv->morph * 100 + 16383) >> 15), "%"); txt(TX_VAL + 3, v); }
    else if (sh && pv->morph_lane == 2) { txt(TX_LAB + 3, "morph"); txt(TX_VAL + 3, "1 side"); }
    else if (sh) { txt(TX_LAB + 3, " "); txt(TX_VAL + 3, " "); }
    else { int32_t t10 = ((k[3] - 16384) * 30) >> 14; txt(TX_LAB + 3, "tone"); { char *p = v; if (t10 > 0) *p++ = '+'; put(dec1(p, t10), " oct"); } txt(TX_VAL + 3, v); }
}

