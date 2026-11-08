/* "aether" - a tilt theremin (docs/36). The key sets the note; tilting the OP-1 plays it like the theremin's
 * antennas: forward / back bends the pitch, raising the right side fades the volume out, shaking adds vibrato.
 * The angle at note-on is "home" (play at any angle). Mono / legato / portamento: the stock play modes.
 *   knob 0 (blue)  : pitch range at full tilt, 1 .. 24 semitones
 *   knob 1 (green) : timbre, sine .. reedy (even harmonics + soft drive)
 *   knob 2 (white) : vibrato from shaking, 0 .. +-0.5 semitone
 *   knob 3 (red)   : volume tilt, 0 (off) .. full (silent at 30 degrees)
 *   blue press     : page 2 = dead zone around home, 0 .. 10 degrees   (word 5; unset = 8192 = 2.5 degrees)
 *   green press    : page 2 = tilt for the full range, 20 .. 60 degrees (word 6; unset = 30 degrees)
 *   white press    : page 2 = vibrato rate, 3 .. 8 Hz                  (word 7; unset = 4.25 Hz)
 * Sensor (docs/36): ADXL345, firmware copy at 0xFF801054 (X left side up +, Y front edge up -, Z up +), 1 g = 16384,
 * updated ~69 Hz. Tilt is used as the gravity component itself (sin of the angle), no atan: the home offset removes
 * the sensor offsets. Per block: smoothing (~30 ms), tilt -> pitch / gain, shake detector (change of the tilt
 * velocity), vibrato LFO, exp2; per sample: phase increment x multiplier (ramped across the block), sine, shaping.
 * A few divisions per block, none per sample. State per voice in our own array (voice memory is shared). */
#include "op1_plugin.h"

#ifndef OP1_HOST
#define AE_ACCEL ((const volatile int16_t *)0xFF801054)
#else
extern int16_t host_accel[3];
#define AE_ACCEL host_accel
#endif

typedef struct {
    uint32_t ph, vph;                 /* oscillator, vibrato LFO phase */
    int32_t sm[3], prev[3], slow[3];  /* smoothed vector (raw << 8), previous, slow average of its velocity */
    int32_t env;                      /* shake envelope, Q15 */
    int32_t gain, mult;               /* gain Q15, pitch multiplier Q14 (end of the last block) */
    int32_t semis;                    /* pitch bend incl. vibrato, semitones Q16 (screen) */
    int32_t envl;                     /* stock envelope (T2) at the block's end, Q15 (screen) */
    int16_t hx, hy;                   /* home (raw) */
} ae_t;
static ae_t st[OP1_VOICES];
static volatile int ae_cur;           /* voice of the last note-on (screen) */

#define A_SM    1547                  /* 1 - exp(-128 / 88200 / 0.03)  Q15 (blocks at OP1_VOICE_RATE) */
#define A_SLOW  316                   /* 1 - exp(-128 / 88200 / 0.15)  Q15 */
#define SHAKE0  (48 << 8)             /* sum of |velocity change| per block, raw << 8: below = still (noise ~35) */
#define SHAKE_S 95                    /* ... full shake 95 raw per block above that (2.5 g/s) */
#define VOL_FULL 8192                 /* sin 30 deg: right side raised this far = silent at full red */
#define LEVEL   22000                 /* output level Q15 (matched to the other engines by ear) */

static inline int32_t page_word(const int16_t *p, int i) { return p[i] > 0 ? p[i] : 8192; }
static inline int32_t sin_deg_q10(int32_t deg_q10)  /* sin of an angle in degrees (Q10) -> raw units (1 g = 16384) */
{
    return op1_sin((deg_q10 * 8192 / 360 >> 10) & 8191) >> 1;
}
static inline int32_t iabs(int32_t a) { return a < 0 ? -a : a; }

/* 2^(x / 65536) for x in Q16 octaves, -2 .. +2 -> Q14 */
static int32_t exp2_q14(int32_t x)
{
    int32_t o = x >> 16, f = x & 0xFFFF;                     /* floor octave, fraction Q16 */
    f >>= 1;                                                 /* Q15 */
    /* 2^f ~ 1 + f (0.69515 + f (0.22568 + f 0.07917)); with the Q14 result < 1 cent (test) */
    int32_t p = 2594;                                        /* 0.07917 Q15 */
    p = 7395 + (p * f >> 15);
    p = 22779 + (p * f >> 15);
    p = 32768 + (p * f >> 15);                               /* Q15 */
    p >>= 1;                                                 /* Q14 */
    return o >= 0 ? p << o : p >> -o;
}

static void ae_note_on(void *self, const int16_t *params)
{
    (void)params;
    ae_cur = op1_voice_index(self) % OP1_VOICES;
    ae_t *s = &st[ae_cur];
    for (int i = 0; i < 3; i++) { s->sm[i] = s->prev[i] = AE_ACCEL[i] * 256; s->slow[i] = 0; }
    s->hx = AE_ACCEL[0]; s->hy = AE_ACCEL[1];
    s->ph = 0; s->env = 0; s->gain = 32767; s->mult = 16384; s->envl = 0;
}

static void ae_render(void *self, int16_t *out, const int16_t *params)
{
    ae_t *s = &st[op1_voice_index(self) % OP1_VOICES];
    /* ---- per block: sensor ---- */
    int32_t hp = 0;
    for (int i = 0; i < 3; i++) {
        s->sm[i] += (int32_t)(((int64_t)((AE_ACCEL[i] * 256) - s->sm[i]) * A_SM) >> 15);
        int32_t vel = s->sm[i] - s->prev[i]; s->prev[i] = s->sm[i];
        s->slow[i] += (int32_t)(((int64_t)(vel - s->slow[i]) * A_SLOW) >> 15);
        hp += iabs(vel - s->slow[i]);
    }
    hp <<= 1;                                                 /* change per 344.5 / s, as SHAKE0 / SHAKE_S were set */
    int32_t m = hp > SHAKE0 ? (hp - SHAKE0) / SHAKE_S : 0;     /* Q8 (256 = full) */
    m = m > 256 ? 32767 : m << 7;
    s->env += m > s->env ? (m - s->env) * 11 >> 6 : -(s->env >> 9) - (s->env > 0);   /* fast attack, ~1 s release (689 blocks / s) */

    /* pitch: forward tilt (front edge up = Y down) relative to home, dead zone, linear in sin(angle) */
    int32_t dead = sin_deg_q10(page_word(params, 5) * 10 * 1024 / 32767);
    int32_t full = sin_deg_q10((20 * 1024) + page_word(params, 6) * 40 * 1024 / 32767);
    if (full <= dead + 64) full = dead + 64;
    int32_t d = s->hy - (s->sm[1] >> 8);
    int32_t a = iabs(d) - dead; if (a < 0) a = 0;
    int32_t range_q8 = 256 + (params[0] * 23 >> 7);           /* semitones Q8, 1 .. 24 */
    int32_t t = (int32_t)(((int64_t)a << 15) / (full - dead)); if (t > 32767) t = 32767;   /* 0..1 Q15 */
    int32_t semis = (int32_t)((int64_t)range_q8 * t >> 7);    /* Q16 */
    if (d < 0) semis = -semis;
    /* vibrato */
    int32_t rate_mhz = 3000 + page_word(params, 7) * 5000 / 32767;
    s->vph += (uint32_t)rate_mhz * 6233u;                      /* 2^32 * Hz * 128 / 88200 / 1000 */
    int32_t depth = (int32_t)((int64_t)params[2] * s->env >> 15);              /* Q15 of 0.5 semitone */
    semis += (int32_t)((int64_t)depth * op1_sin(s->vph >> 19) >> 15);  /* +-32768 Q16 = 0.5 semitone */
    s->semis = semis;
    int32_t m1 = exp2_q14(semis / 12);                         /* constant divisor: compiled as a multiply */
    if (m1 > 65535) m1 = 65535;
    /* volume: right side raised (X down) relative to home */
    int32_t dx = (s->sm[0] >> 8) - s->hx, g1 = 32767;
    if (dx < 0) { g1 = 32767 + (int32_t)((int64_t)params[3] * dx / VOL_FULL); if (g1 < 0) g1 = 0; }

    /* ---- per sample ---- */
    const int32_t *inc = OP1_PITCH_INC;
    int32_t tim = params[1];
    int32_t even = tim >> 2, drive = 8192 + (tim * 3 >> 2);    /* even harmonics up to 0.25, drive 1x .. 4x (Q13) */
    int32_t vg = op1_voice_vel_gain(self);
    int32_t m0 = s->mult, g0 = s->gain;
    uint32_t ph = s->ph;
    for (int n = 0; n < OP1_BLOCK; n++) {
        uint32_t mm = (uint32_t)(m0 + ((m1 - m0) * n >> 7));
        uint32_t i = (uint32_t)inc[n];
        ph += ((i >> 16) * mm << 2) + ((i & 0xFFFF) * mm >> 14);   /* i * mm / 16384, no 64-bit */
        int32_t x = op1_sin(ph >> 19);
        x += (even * ((x * x) >> 15) >> 15) - (even >> 1);        /* x^2 = 2nd harmonic (minus its DC) */
        x = x * drive >> 13;                                       /* |x| < 41000, drive < 32768: 32-bit */
        if (x > 32767) x = 32767; else if (x < -32767) x = -32767;
        { int32_t x2 = (x * x) >> 15; x -= ((x2 * x) >> 15) * 10923 >> 15; }   /* soft clip */
        int32_t g = g0 + ((g1 - g0) * n >> 7);
        out[n] = op1_sat16((((x * g >> 15) * vg >> 15) * LEVEL) >> 15);   /* all 32-bit */
    }
    s->ph = ph; s->mult = m1; s->gain = g1;
    s->envl = OP1_VOICE_ENV[OP1_BLOCK - 1];                     /* computed before render (docs: OP1_VOICE_ENV) */
}

const op1_engine_t ENGINE_aether = { ae_note_on, ae_render, 0, { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_NONE } };

/* ======================= screen (GUI thread) =======================
 * aether.svg (gen_svg.py from the designer's orbit-1.svg): a satellite dish on a tower, four dials with sliders.
 * The dish (nodes "dish" + "waves") follows the OP-1's tilt, live sensor, smoothed: front / back tilt turns it up / down
 * about its hinge (elevation, +-30 degrees), the side tilt turns it around (azimuth: squeezed horizontally, mirrored
 * past 90 degrees). FORM (green knob) shows 1..4 radio waves, which shake while a voice plays. Dials (needle + value
 * arc) = the knobs: blue ORBIT = range, green FORM = timbre, white SPACE = vibrato, red DEPTH = volume tilt; sliders =
 * the page-2 values (dead zone, tilt range, vibrato rate); red has none. The active page is bright, the other dim. Stars: blue / white / red, brightness = ORBIT / SPACE / DEPTH.
 * Page-2 values also shape the picture: blue = blue star size, green = wave shake depth, white = signal stretch; the
 * signals follow the playing voice's envelope (T2) and tilt volume.
 * Labels in the stock font, the dotted signals drawn here (AGG ran out of cell blocks on them as SVG). */
#include "op1_synth.h"                    /* stock-font text (firmware calls, no imports) */
#include "aether_geom.h"
static const uint8_t ae_col[4][4] = { { 0x4D, 0x9E, 0xFF, 0xFF }, { 0x00, 0xED, 0x95, 0xFF },
                                      { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0x3A, 0x5D, 0xFF } };
#define AE_DIM 90                     /* the inactive page: 35 % (as the SVG's dim copies) */
static int32_t sx, sy, sz = 16384;    /* smoothed sensor (screen) */
static uint32_t wob;                  /* wave shake phase */
static void *dish, *waves, *wave[4], *dl[2][4], *ndl[2][4], *sl[2][3], *hdl[2][3];   /* [0] bright, [1] dim; red: no slider */
static uint32_t lab[2][4][OP1_TEXT_SIZE / 4];
static uint8_t lab_made;
#ifndef OP1_HOST
#define AE_VOICE_ON ((const volatile uint8_t *)0xFF80105C)   /* per voice: active (docs/46) */
#else
static const uint8_t AE_VOICE_ON[6] = { 1 };
#endif

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)   /* Q10 */
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
static void dimc(uint8_t *o, const uint8_t *c, int32_t l)          /* l 0..256 */
{
    for (int i = 0; i < 3; i++) o[i] = (uint8_t)(c[i] * l >> 8);
    o[3] = 0xFF;
}
static int32_t isqrt(int32_t v) { int32_t r = 0, b = 1 << 30; while (b > v) b >>= 2; while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; } return r; }

static void ae_screen_init(void *scr)
{
    static const char *const id[8][4] = {
        { "d0", "d1", "d2", "d3" }, { "dd0", "dd1", "dd2", "dd3" }, { "nd0", "nd1", "nd2", "nd3" }, { "nl0", "nl1", "nl2", "nl3" },
        { "s0", "s1", "s2", "s3" }, { "sd0", "sd1", "sd2", "sd3" }, { "hb0", "hb1", "hb2", "hb3" }, { "hd0", "hd1", "hd2", "hd3" } };
    static const char *const wid[4] = { "wave0", "wave1", "wave2", "wave3" };
    dish = op1_find_node(scr, "dish"); waves = op1_find_node(scr, "waves");
    for (int i = 0; i < 4; i++) {
        wave[i] = op1_find_node(scr, wid[i]);
        for (int b = 0; b < 2; b++) {
            dl[b][i] = op1_find_node(scr, id[b][i]); ndl[b][i] = op1_find_node(scr, id[2 + b][i]);
            if (i < 3) { sl[b][i] = op1_find_node(scr, id[4 + b][i]); hdl[b][i] = op1_find_node(scr, id[6 + b][i]); }
        }
    }
}

/* sin / cos (Q15) of a dial position (sine table: 8192 / turn) */
static inline int32_t dsin(int32_t a) { return op1_sin(a & 8191); }
static inline int32_t dcos(int32_t a) { return op1_sin((a + 2048) & 8191); }

static void ae_screen_update(void *scr, int dt)
{
    int slot = OP1_SCREEN_SLOT & 7;
    int16_t k[4];
    op1_synth_knobs_live(slot, k);
    const int16_t *w = op1_synth_knobs(slot);
    const ae_t *s = &st[ae_cur];
    if (!lab_made) {                                                    /* stock-font labels, bright and dim */
        for (int b = 0; b < 2; b++)
            for (int i = 0; i < 4; i++) {
                uint8_t c[4];
                dimc(c, ae_col[i], b ? AE_DIM : 256);
                int32_t n = 0; while (ae_label[i][n]) n++;
                op1_text_ctor(lab[b][i], ae_dial_x[i] - n * 3584, (ae_dial_y[i] >> 10) + AE_LABEL_DY, 12, c);
                op1_text_set(lab[b][i], ae_label[i]);
            }
        lab_made = 1;
    }
    int act[4];                                                         /* dial page active (else page 2) */
    for (int i = 0; i < 4; i++) act[i] = !(i < 3 && op1_eng_page[i]);

    /* dish: elevation from the front / back tilt (Y against Z), azimuth from the side tilt (X against Z) */
    sx += (AE_ACCEL[0] - sx) >> 2; sy += (AE_ACCEL[1] - sy) >> 2; sz += (AE_ACCEL[2] - sz) >> 2;
    int32_t ny = isqrt(sy * sy + sz * sz), nx = isqrt(sx * sx + sz * sz);
    if (ny < 1024) ny = 1024;
    if (nx < 1024) nx = 1024;
    int32_t es = -sy * 32767 / ny;                                     /* sin(elevation): front edge up = dish up */
    if (es > 16384) es = 16384; else if (es < -16384) es = -16384;     /* +-30 degrees */
    int32_t ec = isqrt(32767 * 32767 - es * es);
    int32_t az = sz * 32767 / nx;                                      /* cos(azimuth): flat = facing as drawn */
    if (az >= 0 && az < 6000) az = 6000; else if (az < 0 && az > -6000) az = -6000;   /* never a line */
    /* row-vector matrix about the hinge: rotate (elevation), then scale x by cos(azimuth) */
    int32_t a = (ec >> 5) * az >> 15, b = -(es >> 5), c = (es >> 5) * az >> 15, d = ec >> 5;
    int32_t hx = AE_HINGE_X_Q10, hy = AE_HINGE_Y_Q10;
    int32_t m[9] = { a, b, 0, c, d, 0, hx - (int32_t)(((int64_t)hx * a + (int64_t)hy * c) >> 10),
                     hy - (int32_t)(((int64_t)hx * b + (int64_t)hy * d) >> 10), 1024 };
    if (dish) op1_node_set_transform(dish, m);
    /* waves: as the dish, shaking (a small pulse about the feed) while a voice plays */
    int playing = 0;
    for (int v = 0; v < OP1_VOICES; v++) playing |= AE_VOICE_ON[v];
    int32_t sc = 1024;
    if (playing) {                                                     /* ~8 Hz, depth by the green page-2 word */
        int32_t depth = page_word(w, 6) * 96 >> 15;                    /* Q10: 0 .. 9 % (unset: 2.3 %) */
        wob += (uint32_t)dt * 1100; sc = 1024 + (dsin((int32_t)(wob >> 10)) * depth >> 15);
    }
    if (waves) {
        int32_t fx = AE_FEED_X_Q10 * (1024 - sc) >> 10, fy = AE_FEED_Y_Q10 * (1024 - sc) >> 10;   /* scale about the feed */
        int32_t mw[9] = { a * sc >> 10, b * sc >> 10, 0, c * sc >> 10, d * sc >> 10, 0,
                          (int32_t)(((int64_t)fx * a + (int64_t)fy * c) >> 10) + m[6],
                          (int32_t)(((int64_t)fx * b + (int64_t)fy * d) >> 10) + m[7], 1024 };
        op1_node_set_transform(waves, mw);
    }
    int nw = 1 + (k[1] * 4 >> 15); if (nw > 4) nw = 4;                 /* FORM: 1..4 waves */
    for (int i = 0; i < 4; i++) if (wave[i]) op1_node_set_visible(wave[i], i < nw);

    /* dials, sliders: both copies moved, the active page's shown bright */
    int32_t ang[4];
    for (int i = 0; i < 4; i++) {
        ang[i] = ((int32_t)k[i] * 6144 >> 15) - 3072;                   /* -135 .. +135 degrees */
        int32_t v = i < 3 ? page_word(w, 5 + i) : 0;                    /* page 2 (red: no slider) */
        int32_t dy = AE_SLD_HALF_Q10 - (int32_t)((int64_t)2 * AE_SLD_HALF_Q10 * v >> 15);
        int32_t mt[9] = { 1024, 0, 0, 0, 1024, 0, 0, dy, 1024 };
        int sb = !act[i];                                               /* slider bright */
        for (int bb = 0; bb < 2; bb++) {
            if (ndl[bb][i]) op1_node_rotate(ndl[bb][i], ae_dial_x[i], ae_dial_y[i], dsin(ang[i]), dcos(ang[i]));
            if (i < 3 && hdl[bb][i]) op1_node_set_transform(hdl[bb][i], mt);
            if (dl[bb][i]) op1_node_set_visible(dl[bb][i], bb == !act[i]);
            if (i < 3 && sl[bb][i]) op1_node_set_visible(sl[bb][i], bb == !sb);
        }
    }
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    for (int i = 0; i < 4; i++) op1_text_draw(lab[!act[i]][i], r, ident);
    /* value arcs: from the lower left (-135 degrees) to the needle, segments of 22.5 degrees */
    for (int i = 0; i < 4; i++) {
        uint8_t cc[4];
        dimc(cc, ae_col[i], act[i] ? 256 : AE_DIM);
        int32_t cx = ae_dial_x[i], cy = ae_dial_y[i], R = AE_ARC_R_Q10;
        int32_t px = cx + (R * dsin(-3072) >> 15), py = cy - (R * dcos(-3072) >> 15);
        for (int32_t an = -3072 + 512; ; an += 512) {
            int32_t e = an < ang[i] ? an : ang[i];
            int32_t qx = cx + (R * dsin(e) >> 15), qy = cy - (R * dcos(e) >> 15);
            if (qx != px || qy != py) ln(r, px, py, qx, qy, 0x600, cc);
            px = qx; py = qy;
            if (e >= ang[i]) break;
        }
    }
    /* dotted signals: stretched about their centres by the white page-2 word, brightness follows the playing voice's envelope */
    uint8_t c_dot[4];
    int32_t el = playing ? s->envl * (s->gain >> 7) >> 15 : 0;        /* envelope x tilt volume, 0..255 */
    dimc(c_dot, ae_col[2], 64 + (el * 192 >> 8));                     /* 25 % .. 100 % */
    int32_t stretch = 768 + (page_word(w, 7) * 1024 >> 15);               /* Q10: 0.75 .. 1.75 x (unset: 1 x) */
    for (unsigned q = 0, sg = 0; q < sizeof ae_dot / sizeof ae_dot[0]; q++) {
        while (q >= ae_dot_end[sg]) sg++;
        int32_t cx = (int32_t)ae_dot_cx[sg] << 4;
        int32_t x = cx + ((((int32_t)ae_dot[q][0] << 4) - cx) * stretch >> 10), y = (int32_t)ae_dot[q][1] << 4;
        ln(r, x - 0x280, y, x + 0x280, y, 0x500, c_dot);
    }
    /* stars: 4-point, concave sides (8 lines); blue = ORBIT, white = SPACE, red = DEPTH, 15 .. 100 % */
    static const uint8_t star_knob[3] = { 3, 2, 0 };                    /* big top right, bottom, top left */
    for (int j = 0; j < 3; j++) {
        uint8_t cc[4];
        int kn = star_knob[j];
        dimc(cc, ae_col[kn], 38 + (k[kn] * 218 >> 15));
        int32_t x = ae_star[j][0], y = ae_star[j][1], hw = ae_star[j][2], hh = ae_star[j][3];
        if (kn == 0) {                                                  /* blue star: size by the blue page-2 word */
            int32_t z = 640 + (page_word(w, 5) * 1536 >> 15);           /* Q10: 0.6 .. 2.1 x (unset: 1 x) */
            hw = hw * z >> 10; hh = hh * z >> 10;
        }
        int32_t iw = hw * 3 >> 4, ih = hh * 3 >> 4;                     /* waist of the concave sides */
        int32_t P[8][2] = { { x, y - hh }, { x + iw, y - ih }, { x + hw, y }, { x + iw, y + ih },
                            { x, y + hh }, { x - iw, y + ih }, { x - hw, y }, { x - iw, y - ih } };
        for (int q = 0; q < 8; q++) ln(r, P[q][0], P[q][1], P[(q + 1) & 7][0], P[(q + 1) & 7][1], 0x500, cc);
    }
}

const op1_screen_t SCREEN_aether = { ae_screen_init, ae_screen_update };
