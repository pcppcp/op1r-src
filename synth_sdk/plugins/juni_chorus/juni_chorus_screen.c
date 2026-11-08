/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
 * juni chorus's screen, after the owner's mockup C ("swaying birches"): five birches at the water line sway with the
 * chorus LFO (the trees alternate sides like the two chorus taps, 180 degrees apart, with a small spread so they
 * don't move as one), as far as DEPTH and the mode's sweep reach and MIX lets them; the red reflections follow their
 * tree and ripple with the same LFO, stronger while sound comes in; TONE dims the moon and draws its dashed halo in.
 * MIX 0 leaves the scene still. Geometry and the fit from gen_svg.py (juni_chorus_geom.h); the ranges, cliffs,
 * horizon, clouds and stars are the SVG, all static, drawn through the static-prefix cache (first_live NULL).
 * Only reads the plugin's telemetry (juni_chorus.c); the audio never reads anything back. */
#include "op1_synth.h"
#include "juni_chorus_geom.h"

extern volatile uint32_t juni_chorus_blocks[3], juni_chorus_ph[3], juni_chorus_inc[3];
extern volatile int32_t juni_chorus_depth[3];
extern volatile int16_t juni_chorus_level[3];

/* The shown sway runs at half the chorus LFO, so the trees move calmer than the sound (I 0.25 Hz, II 0.4 Hz), locked
 * to half the plugin's phase while the LFO is slower than this; above it (I+II, 9 Hz) it runs on its own at half
 * this rate (1.25 Hz), so the frame rate can't alias it into a slow wave. 2^32 * 2.5 / 44100 (the FX rate). */
#define SHOW_INC_MAX 243479u
#define LEAN_MAX     (8 * 256)          /* Q8 px: a tree top's lean at the full sweep (DEPTH 99 in I / II) */
#define SWEEP_FULL   20480              /* the plugin's full sweep, Q8 samples at 44.1 kHz (SWEEP) */
#define RIPPLE_MAX   (3 * 256)          /* Q8 px: the reflections' ripple at MIX 99 and a loud input */
#define TONE_REST    5898               /* TONE at or below the default 18 %: the moon as drawn */

static unsigned text_slot;
static uint8_t text_objects[5][OP1_TEXT_SIZE] __attribute__((aligned(4)));
static uint8_t made[5];
static char cached[5][16];
static uint32_t show_ph, prev_blocks;
static unsigned idle;
static int started;
static int32_t e_amp, e_ripple, e_level, e_tone, e_mix;
static int32_t lean[JC_TREES];

static const uint8_t black[4] = { 1, 1, 1, 255 }, white[4] = { 255, 255, 255, 255 };
static const uint8_t blue[4] = { 0x69, 0x8E, 0xFF, 255 }, green[4] = { 0x00, 0xED, 0x95, 255 }, red[4] = { 0xFF, 0x3A, 0x5D, 255 };
static const uint8_t dimred[4] = { 80, 21, 30, 255 }, moonc[4] = { 0x2E, 0x84, 0xF5, 255 };

static int32_t sine(uint32_t ph) { return OP1_SINE_TABLE[ph >> 19]; }       /* Q15 */

/* Q8 screen coordinates in, Q10 to the raster; kept on the screen */
static void line(void *r, int32_t x, int32_t y, int32_t xx, int32_t yy, int32_t w8, const uint8_t *c)
{
 if (x < 0) x = 0;
 if (xx < 0) xx = 0;
 if (x > 320 * 256) x = 320 * 256;
 if (xx > 320 * 256) xx = 320 * 256;
 op1_pt a = { x * 4, y * 4 }, b = { xx * 4, yy * 4 };
 op1_raster_line(r, &a, &b, w8 * 4, c);
}
static void text(void *r, int x, int y, int size, const char *s, const uint8_t *c)
{
 unsigned n = text_slot++;
 if (n >= 5) return;
 static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
 if (!made[n]) { op1_text_ctor(text_objects[n], OP1_Q10(x), y, size, c); made[n] = 1; }
 unsigned j = 0;
 while (j < 15 && s[j] && cached[n][j] == s[j]) j++;
 if (cached[n][j] != s[j]) { op1_text_set(text_objects[n], s); for (j = 0; j < 15 && s[j]; j++) cached[n][j] = s[j]; cached[n][j] = 0; }
 op1_text_draw(text_objects[n], r, ident);
}
static void shade(uint8_t *o, const uint8_t *c, int32_t q15)
{
 for (unsigned i = 0; i < 3; i++) o[i] = (uint8_t)(1 + (((int32_t)c[i] - 1) * q15 >> 15));
 o[3] = 255;
}
/* one easing step: v moves toward t by a (Q15) of the way, at least one unit */
static int32_t ease(int32_t v, int32_t t, int32_t a)
{
 int32_t d = (int32_t)(((int64_t)(t - v) * a) >> 15);
 if (!d && t != v) d = t > v ? 1 : -1;
 return v + d;
}
/* the input peak as a level, Q15: octaves above 16, full at 5 octaves (a peak of 512). The FX input is quiet in
 * practice (a held Wavetable note peaks near 75 of 32767 in op1emu, about 600 on the attack), so a linear scale
 * would never move; the log reads a held note as about half. */
static int32_t level_q15(int32_t pk)
{
 if (pk <= 16) return 0;
 int32_t o = 0;
 while ((pk >> (o + 1)) > 0) o++;                                        /* floor(log2) */
 const int32_t q = ((o - 4) << 8) + (((pk << 8) >> o) - 256);             /* Q8 octaves above 4, linear between */
 return q >= 5 * 256 ? 32767 : q * 32767 / (5 * 256);
}
/* a point of tree t at weight w (Q12) moved by the tree's lean */
#define SWAY(t, w) ((lean[t] * (w)) >> 12)               /* 2048 x 4096 fits */

static void init(void *s) { (void)s; started = 0; }

static void update(void *scr, int dt)
{
 int16_t kn[4];
 op1_fx_knobs_live(kn);
 int32_t k[4];
 for (int i = 0; i < 4; i++) k[i] = kn[i] < 0 ? 0 : kn[i];
 const unsigned pos = (unsigned)op1_fx_screen_pos() % 3;
 if (dt <= 0 || dt > 200) dt = 33;
 const int32_t a = (int32_t)((int64_t)dt * 32768 / (dt + 110));        /* ~110 ms */
 const int32_t af = (int32_t)((int64_t)dt * 32768 / (dt + 30));         /* the level's attack */
 const int32_t as = (int32_t)((int64_t)dt * 32768 / (dt + 400));        /* its release */

 /* the effect runs while its block count moves; when it stops (bypass, another FX) the scene stops too */
 const uint32_t blocks = juni_chorus_blocks[pos];
 if (!started) { started = 1; prev_blocks = blocks; idle = 99; e_amp = e_ripple = e_level = 0; e_mix = k[3];
                 e_tone = k[2]; show_ph = juni_chorus_ph[pos] >> 1; }
 idle = blocks != prev_blocks ? 0 : (idle < 99 ? idle + 1 : idle);
 prev_blocks = blocks;
 const int running = idle < 4;

 e_mix = ease(e_mix, k[3], a);
 e_tone = ease(e_tone, k[2], a);
 const int32_t mixs = e_mix * 2 > 32767 ? 32767 : e_mix * 2;                 /* full sway from MIX 50 % */
 if (running) {
  const uint32_t inc = juni_chorus_inc[pos];
  const uint32_t samples = (uint32_t)dt * 441u / 10u;                        /* 44.1 kHz */
  if (inc <= SHOW_INC_MAX) {
   show_ph += (inc >> 1) * samples;
   /* locked to half the plugin's phase: the error mod 2^31 (either half-phase will do), sign-extended from 31 bits */
   show_ph += (uint32_t)(((int32_t)(((juni_chorus_ph[pos] >> 1) - show_ph) << 1) >> 1) >> 2);
  } else show_ph += (SHOW_INC_MAX >> 1) * samples;
  const int32_t lvl = level_q15(juni_chorus_level[pos]);
  e_level = ease(e_level, lvl, lvl > e_level ? af : as);
  int32_t d = juni_chorus_depth[pos];
  if (d > SWEEP_FULL) d = SWEEP_FULL;
  e_amp = ease(e_amp, (int32_t)(((int64_t)d * LEAN_MAX / SWEEP_FULL) * mixs >> 15), a);
  e_ripple = ease(e_ripple, (int32_t)(((int64_t)RIPPLE_MAX * mixs >> 15) * (6554 + (e_level * 26213 >> 15)) >> 15), a);
 }
 for (int t = 0; t < JC_TREES; t++)
  lean[t] = (int32_t)((e_amp * sine(show_ph + (t & 1 ? 0x80000000u : 0) + (uint32_t)t * 0x0A000000u)) >> 15);

 op1_screen_draw_cached(scr, dt, 0);             /* ranges, cliffs, horizon, clouds, stars: all static */
 void *r = op1_raster();

 /* ---- the moon: TONE above its default dims it and draws the halo in toward it ---- */
 {
  int32_t t = (e_tone - TONE_REST) * 32767 / (32767 - TONE_REST);
  if (t < 0) t = 0;
  if (t > 32767) t = 32767;
  uint8_t mc[4], hc[4];
  shade(mc, moonc, 32767 - (t * 19661 >> 15));                               /* down to 40 % */
  shade(hc, moonc, 32767 - (t * 22937 >> 15));                               /* down to 30 % */
  int32_t px = JC_MOON_X + JC_MOON_R, py = JC_MOON_Y;
  for (unsigned i = 1; i <= JC_RING_N; i++) {
   const uint32_t ang = i * (0xFFFFFFFFu / JC_RING_N + 1u);
   const int32_t x = JC_MOON_X + (JC_MOON_R * sine(ang + 0x40000000u) >> 15);
   const int32_t y = JC_MOON_Y + (JC_MOON_R * sine(ang) >> 15);
   if ((JC_MOON_SEEN >> (i - 1)) & 1) line(r, px, py, x, y, 256, mc);   /* behind the crowns as drawn */
   px = x; py = y;
  }
  const int32_t hr = JC_HALO_R - (int32_t)(((int64_t)(JC_HALO_R - JC_MOON_R - 3 * 256) * t) >> 15);
  for (unsigned i = 0; i < sizeof jc_dash / sizeof jc_dash[0]; i++) {
   const uint32_t a0 = (uint32_t)jc_dash[i][0] << 16, span = (uint32_t)(uint16_t)(jc_dash[i][1] - jc_dash[i][0]) << 16;
   const unsigned n = 2;                        /* two chords: within 0.2 px of the arc */
   int32_t qx = 0, qy = 0;
   for (unsigned j = 0; j <= n; j++) {
    const uint32_t ang = a0 + (span >> 1) * j;                         /* n = 2 */
    const int32_t x = JC_HALO_X + (hr * sine(ang + 0x40000000u) >> 15);
    const int32_t y = JC_HALO_Y + (hr * sine(ang) >> 15);
    if (j) line(r, qx, qy, x, y, 256, hc);
    qx = x; qy = y;
   }
  }
 }

 /* ---- the birches: trunk, bark marks, branches, leaves; every point moves by its tree's lean x weight ---- */
 for (int t = 0; t < JC_TREES; t++) {
  for (unsigned i = jc_trunk_at[t]; i + 1 < jc_trunk_at[t + 1]; i++) {
   const int32_t *p = jc_trunk[i], *q = jc_trunk[i + 1];
   /* butt caps: run each piece half a pixel into the next so the bends close */
   const int32_t dx = q[0] + SWAY(t, q[3]) - p[0] - SWAY(t, p[3]), dy = q[1] - p[1];
   const int32_t ex = i + 2 < jc_trunk_at[t + 1] ? dx * 128 / (dy < 0 ? -dy : dy + 1) : 0;
   line(r, p[0] + SWAY(t, p[3]), p[1], q[0] + SWAY(t, q[3]) + ex, q[1] - (i + 2 < jc_trunk_at[t + 1] ? 128 : 0),
        (p[2] + q[2]) / 2, white);
  }
  for (unsigned i = jc_mark_at[t]; i < jc_mark_at[t + 1]; i++) {
   const int32_t *m = jc_mark[i];
   const int32_t s = SWAY(t, m[4]);
   line(r, m[0] + s, m[1], m[2] + s, m[1], m[3], black);
  }
  for (unsigned b = jc_tree_br[t]; b < jc_tree_br[t + 1]; b++)
   for (unsigned i = jc_br_at[b]; i + 1 < jc_br_at[b + 1]; i++)
    line(r, jc_brp[i][0] + SWAY(t, jc_brp[i][2]), jc_brp[i][1], jc_brp[i + 1][0] + SWAY(t, jc_brp[i + 1][2]), jc_brp[i + 1][1],
         (jc_brp[i][3] + jc_brp[i + 1][3]) / 2, white);
  for (unsigned i = jc_leaf_at[t]; i < jc_leaf_at[t + 1]; i++) {           /* a diamond: four lines */
   const int32_t *l = jc_leaf[i];
   const int32_t cx = l[0] + SWAY(t, l[2]), cy = l[1];
   const int32_t ax = l[3], ay = l[4], bx = l[5], by = l[6];
   line(r, cx + ax, cy + ay, cx + bx, cy + by, 205, green);
   line(r, cx + bx, cy + by, cx - ax, cy - ay, 205, green);
   line(r, cx - ax, cy - ay, cx - bx, cy - by, 205, green);
   line(r, cx - bx, cy - by, cx + ax, cy + ay, 205, green);
  }
 }

 /* ---- the reflections: each column follows its tree's lean, mirrored (deeper = higher up the tree); every dash
    ripples with the LFO (three waves to its cycle, travelling down the column; show_ph runs at half the LFO, so x6)
    and stretches, as far as MIX and the input level let it; the free dashes drift with it ---- */
 for (unsigned i = 0; i < sizeof jc_refl / sizeof jc_refl[0]; i++) {
  const int32_t *d = jc_refl[i];
  const int32_t depth = d[2] - JC_WATER_Y;                                    /* Q8 px below the water line */
  int32_t dx, dl;
  if (d[3] >= 0) {
   const int t = d[3];
   int32_t q = depth * 4096 / jc_foot[t][2];
   if (q > 4096) q = 4096;
   const uint32_t ph = show_ph * 6u + (uint32_t)t * 0x30000000u - (uint32_t)depth * 0x00180000u;
   dx = SWAY(t, q * q >> 12) + (e_ripple * sine(ph) >> 15);
   dl = (e_ripple * sine(ph + 0x40000000u) >> 16);
  } else {
   const uint32_t ph = show_ph * 4u + (uint32_t)d[0] * 0x00050000u + (uint32_t)depth * 0x00130000u;
   dx = (e_ripple * 5 / 8 * sine(ph) >> 15);
   dl = (e_ripple * sine(ph + 0x40000000u) >> 16);
  }
  line(r, d[0] + dx - dl, d[2], d[1] + dx + dl, d[2], 320, red);           /* 1.25 px: solid on the panel (the art's 0.9 dims to a faint line) */
 }

 /* ---- MIX bar under its label, and the corner labels (native text) ---- */
 const int len = (int)(((int64_t)e_mix * 38 + 16383) >> 15);
 line(r, 268 * 256, 131 * 256, 306 * 256, 131 * 256, 4 * 256, dimred);
 if (len) line(r, 268 * 256, 131 * 256, (268 + len) * 256, 131 * 256, 4 * 256, red);
 text_slot = 0;
 text(r, 14, 11, 12, "mode", blue);
 text(r, 14, 27, 22, k[0] < 11264 ? "1" : k[0] < 21504 ? "2" : "1+2", white);
 text(r, 268, 11, 12, "depth", green);
 text(r, 14, 126, 12, "tone", white);
 text(r, 284, 113, 12, "mix", red);
 /* The native stock FX overlay owns bypass dimming and its central FX OFF label. */
}
const op1_screen_t SCREEN_juni_chorus = { init, update };
