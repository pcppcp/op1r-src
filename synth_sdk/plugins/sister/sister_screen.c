/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
 * SISTER's screen, after the owner's sketch: a gothic nave of column pairs and pointed arches whose depth follows
 * SPACE, the rose window behind the nun's head like a halo (its petals sing CHOIR's drifting vowels or stand as
 * PIPES' tuned pipes, greener with VOICE, lit by the wet level), the nun, three candles whose flames grow with
 * SWELL and flicker with the tail, MIX as the red bar. Geometry and the fit from gen_svg.py (sister_geom.h);
 * everything still (floor, candle bodies and wicks, the rose's ring and quatrefoil, the nun's outline) is the SVG,
 * drawn through the static-prefix cache (the whole SVG is static: first_live NULL); the moving parts are drawn after
 * it, clipped where the still art used to cover them. Every moving value eases per frame toward its target. */
#include "op1_synth.h"
#include "sister_geom.h"
#include "sister_dsp.h"           /* si_pos_t sister_pos[3]: the telemetry */

static unsigned text_slot;
static uint8_t text_objects[5][OP1_TEXT_SIZE] __attribute__((aligned(4)));
static uint8_t made[5];
static char cached[5][16];
static int32_t e_depth = -1, e_xc, e_vow[2], e_lit, e_mix, e_flame[3], e_flick[3], e_green, e_fast, e_slow, e_mid, e_breath;
static uint32_t frames, rng = 0x2545F491u;

static const uint8_t white[4] = { 255, 255, 255, 255 };
static const uint8_t blue[4] = { 0x69, 0x8E, 0xFF, 255 }, green[4] = { 0x00, 0xED, 0x95, 255 }, red[4] = { 0xFF, 0x3A, 0x5D, 255 };
static const uint8_t dimred[4] = { 80, 21, 30, 255 }, teal[4] = { 0x0B, 0x9A, 0x9E, 255 };

#define VX (SISTER_VPX * 4)                 /* Q10 */
#define VY (SISTER_VPY * 4)

/* Q10 screen coordinates; clipped to the visible rows like Father's helper */
static void line(void *r, int x, int y, int xx, int yy, int width, const uint8_t *c)
{
 if ((y < 1024 && yy < 1024) || (y > 154 * 1024 && yy > 154 * 1024)) return;
 if (y < 1024 || y > 154 * 1024) { int b = y < 1024 ? 1024 : 154 * 1024; x += (int)((int64_t)(xx - x) * (b - y) / (yy - y)); y = b; }
 if (yy < 1024 || yy > 154 * 1024) { int b = yy < 1024 ? 1024 : 154 * 1024; xx += (int)((int64_t)(x - xx) * (b - yy) / (y - yy)); yy = b; }
 op1_pt a = { x, y }, b = { xx, yy };
 op1_raster_line(r, &a, &b, width, c);
}
/* a line and its mirror about the axis */
static void cline(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint8_t *c, int mode);
static void line2(void *r, int x, int y, int xx, int yy, const uint8_t *c)
{
 cline(r, x, y, xx, yy, c, 0);
 cline(r, 2 * VX - x, y, 2 * VX - xx, yy, c, 0);
}
/* ---- the live parts against the still art (sister.svg holds the ring, the quatrefoil and the nun's outline, drawn
   first through the cache): the nave stays outside the ring's outer edge, where the black disc used to hide it, and
   the petals and pipes inside its inner edge, where the ring used to cover them; both stop half a pixel outside the
   nun's outline, where her black fill used to hide them. Segments are walked in steps of at most 2 px and each
   visible stretch is drawn as one line, its ends found by bisection. ---- */
#define RX (SISTER_ROSE_X * 4)
#define RY (SISTER_ROSE_Y * 4)
static int32_t nun_half(int32_t y)               /* y Q10 -> the veil's half width Q10 (0 outside her rows) */
{
 const int32_t y8 = y >> 2;
 const unsigned n = sizeof sister_veil / sizeof sister_veil[0];
 if (y8 < sister_veil[0].y || y8 > SISTER_HEM_Y) return 0;
 for (unsigned i = 1; i < n; i++)
  if (y8 <= sister_veil[i].y) {
   const int32_t y0 = sister_veil[i - 1].y, y1 = sister_veil[i].y, x0 = sister_veil[i - 1].x, x1 = sister_veil[i].x;
   const int32_t x = y1 > y0 ? x0 + (x1 - x0) * (y8 - y0) / (y1 - y0) : x1;
   return (SISTER_VPX - x) * 4;
  }
 return (SISTER_VPX - sister_veil[n - 1].x) * 4;
}
/* hidden: mode 0 (the nave) inside the ring's outer edge or behind the nun; mode 1 (petals) outside the ring's inner
   edge or behind the nun. Q10 in; the distance test in Q4 stays inside 32 bits */
static int hidden(int32_t x, int32_t y, int mode)
{
 const int32_t dx = (x - RX) >> 6, dy = (y - RY) >> 6;
 const int32_t d2 = dx * dx + dy * dy;
 if (mode ? d2 > (SISTER_PETAL_MAX_R >> 4) * (SISTER_PETAL_MAX_R >> 4) : d2 < (SISTER_NAVE_R >> 4) * (SISTER_NAVE_R >> 4)) return 1;
 const int32_t h = nun_half(y);
 return h && (x > VX ? x - VX : VX - x) < h + 512;
}
static void cline(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint8_t *c, int mode)
{
 if (!mode) {                                    /* the nave: most of it is clear of the rose and the nun */
  const int32_t lo = x0 < x1 ? x0 : x1, hi = x0 < x1 ? x1 : x0, top = y0 < y1 ? y0 : y1, bot = y0 < y1 ? y1 : y0;
  const int32_t rr = SISTER_NAVE_R * 4;
  const int rose = hi > RX - rr && lo < RX + rr && bot > RY - rr && top < RY + rr;
  const int nun = bot > sister_veil[0].y * 4 && top < SISTER_HEM_Y * 4 && hi > VX - 30 * 1024 && lo < VX + 30 * 1024;
  if (!rose && !nun) { line(r, x0, y0, x1, y1, 1024, c); return; }
 }
 const int32_t ax = x1 > x0 ? x1 - x0 : x0 - x1, ay = y1 > y0 ? y1 - y0 : y0 - y1;
 const int n = (int)((ax > ay ? ax : ay) >> 11) + 1;            /* steps of at most 2 px */
 int32_t sx = x0, sy = y0, px = x0, py = y0;
 int pv = !hidden(x0, y0, mode);
 for (int k = 1; k <= n; k++) {
  const int32_t qx = x0 + (x1 - x0) * k / n, qy = y0 + (y1 - y0) * k / n;      /* |d| * k < 2^31 */
  const int qv = !hidden(qx, qy, mode);
  if (qv != pv) {                                /* the edge between p and q: 8 halvings, under 1/100 px */
   int32_t vx = pv ? px : qx, vy = pv ? py : qy, hx = pv ? qx : px, hy = pv ? qy : py;
   for (int i = 0; i < 8; i++) {
    const int32_t mx = (vx + hx) >> 1, my = (vy + hy) >> 1;
    if (hidden(mx, my, mode)) { hx = mx; hy = my; } else { vx = mx; vy = my; }
   }
   if (pv) line(r, sx, sy, vx, vy, 1024, c);    /* a visible stretch ends */
   else { sx = vx; sy = vy; }                     /* one begins */
  }
  px = qx; py = qy; pv = qv;
 }
 if (pv) line(r, sx, sy, x1, y1, 1024, c);
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
static void blend(uint8_t *o, const uint8_t *a, const uint8_t *b, int32_t t15)
{
 for (unsigned i = 0; i < 3; i++) o[i] = (uint8_t)(a[i] + (((int32_t)b[i] - a[i]) * t15 >> 15));
 o[3] = 255;
}
/* one easing step: v moves toward t by a (Q15) of the way, at least one unit */
static int32_t ease(int32_t v, int32_t t, int32_t a)
{
 int32_t d = (int32_t)(((int64_t)(t - v) * a) >> 15);
 if (!d && t != v) d = t > v ? 1 : -1;
 return v + d;
}
/* log2 of x in Q8 (x > 0): the integer part from the leading bit, then 8 mantissa bits (linear in between) */
static int32_t log2q8(uint32_t x)
{
 const int b = 31 - __builtin_clz(x);
 const uint32_t m = b >= 8 ? x >> (b - 8) : x << (8 - b);
 return b * 256 + (int32_t)(m & 255);
}
static int32_t persp(int32_t z) { return (int32_t)((4096 * 4096) / (4096 + z)); }   /* depth Q12 -> scale Q12 */
static void init(void *s) { (void)s; e_depth = -1; }

static void update(void *scr, int dt)
{
 int16_t kn[4];
 op1_fx_knobs_live(kn);
 int32_t k[4];
 for (int i = 0; i < 4; i++) k[i] = kn[i] < 0 ? 0 : kn[i];
 const int pos = op1_fx_screen_pos() % 3;
 if (dt <= 0 || dt > 200) dt = 33;
 const int32_t a = (int32_t)((int64_t)dt * 32768 / (dt + 110));        /* ~110 ms time constant */
 const int32_t af = (int32_t)((int64_t)dt * 32768 / (dt + 45));         /* flames, faster */
 frames++;

 /* targets: the depth step between column pairs (Q12, 0.17 .. 0.64, 0.34 at the default SPACE 60) */
 const int32_t t_depth = SISTER_D_MIN + (int32_t)((((int64_t)k[0] * k[0] >> 15) * SISTER_D_K) >> 15);
 int32_t t_xc = (k[1] - 9830) * 32767 / 13107;                            /* CHOIR -> PIPES, as the DSP */
 if (t_xc < 0) t_xc = 0;
 if (t_xc > 32767) t_xc = 32767;
 if (e_depth < 0) { e_depth = t_depth; e_xc = t_xc; e_mix = k[3]; e_green = k[1]; e_vow[0] = sister_pos[pos].vowel[0]; e_vow[1] = sister_pos[pos].vowel[1]; }
 e_depth = ease(e_depth, t_depth, a);
 e_xc = ease(e_xc, t_xc, a);
 e_green = ease(e_green, k[1], a);
 e_mix = ease(e_mix, k[3], a);
 for (int v = 0; v < 2; v++) {                  /* the vowel position wraps at 3: ease the short way round */
  int32_t d = sister_pos[pos].vowel[v] - e_vow[v];
  if (d > 6144) d -= 12288;
  if (d < -6144) d += 12288;
  e_vow[v] += (int32_t)(((int64_t)d * a) >> 15);
  if (e_vow[v] < 0) e_vow[v] += 12288;
  if (e_vow[v] >= 12288) e_vow[v] -= 12288;
 }
 /* the tail's level, screen side: the wet block peak (sister_pos[].peak; 0 below 5) in octaves above 4 (Q8), so it reads
    the same at any loudness. In op1emu a held Wavetable C4 at the defaults gives block peaks of only ~5..30 (its
    FX input peaks ~60..90 of 32767); the host model's 20000 is ten octaves louder. Three followers: fast (~40 ms
    up, ~120 ms down), slow (~60 ms up, ~700 ms down) and an even ~300 ms one. The slow one lights the petals, sets
    how far they sing and how much the flames flicker (all of it at 6 octaves, peak 256); the fast one's lead over
    the even one makes the petals breathe with the playing at any level. Silence returns all to 0: the still sketch. */
 const int32_t pk = sister_pos[pos].peak;
 const int32_t lg = pk > 4 ? log2q8((uint32_t)pk) - 512 : 0;            /* 0 .. 3328 */
 const int32_t a_up = (int32_t)((int64_t)dt * 32768 / (dt + 40)), a_fd = (int32_t)((int64_t)dt * 32768 / (dt + 120));
 const int32_t a_su = (int32_t)((int64_t)dt * 32768 / (dt + 60)), a_sd = (int32_t)((int64_t)dt * 32768 / (dt + 700));
 e_fast = ease(e_fast, lg, lg > e_fast ? a_up : a_fd);
 e_slow = ease(e_slow, lg, lg > e_slow ? a_su : a_sd);
 e_mid = ease(e_mid, lg, (int32_t)((int64_t)dt * 32768 / (dt + 300)));
 e_lit = e_slow >= 1536 ? 32767 : e_slow * 32767 / 1536;
 {                                                                         /* breath: 1 px per octave of lead, +-1.2 px */
  int32_t b = e_fast - e_mid;
  if (b > 307) b = 307;
  if (b < -307) b = -307;
  e_breath = ease(e_breath, b, af);
 }

 op1_screen_draw_cached(scr, dt, 0);            /* floor, candles, ring, quatrefoil, nun: all static */
 void *r = op1_raster();

 /* ---- the nave, far to near: per level its pointed transverse arch, the lancet on each wall between it and
    the nearer pair, and its columns (levels 0..3) ---- */
 for (int f = 5; f >= 0; f--) {
  const int32_t s = persp(f * e_depth);
  {                                                                     /* the transverse arch */
   const int32_t sc = f > 3 ? persp(3 * e_depth) : s;                  /* 4 and 5 spring from level 3's pair */
   const int32_t sx = VX - (SISTER_ARCH_X * sc >> 10), sy = VY + (SISTER_CAP_Y * sc >> 10);
   const int32_t ty = SISTER_APEX_V * 4 - (SISTER_APEX_A * s >> 10);
   int32_t px = sx, py = sy;
   for (unsigned j = 1; j < sizeof sister_profile / sizeof sister_profile[0]; j++) {
    const int32_t x = sx + (int32_t)(((int64_t)(VX - sx) * sister_profile[j][0]) >> 12);
    const int32_t y = sy - (int32_t)(((int64_t)(sy - ty) * sister_profile[j][1]) >> 12);
    line2(r, px, py, x, y, teal);
    px = x; py = y;
   }
  }
  if (f >= 1 && f <= 3) {                                               /* the lancets, both legs */
   const unsigned nl = sizeof sister_legs / sizeof sister_legs[0];
   for (int leg = 0; leg < 2; leg++) {
    int32_t px = 0, py = 0;
    for (unsigned j = 0; j < nl; j++) {
     const int32_t u = sister_legs[leg ? nl - 1 - j : j][0], v = sister_legs[leg ? nl - 1 - j : j][1];
     const int32_t wu = leg ? 2 * SISTER_LANCET_U - (SISTER_LANCET_U * u >> 12) : SISTER_LANCET_U * u >> 12;
     const int32_t ls = persp(f * e_depth + (wu * e_depth >> 12));
     const int32_t hy = SISTER_CAP_Y + ((SISTER_LANCET_Y - SISTER_CAP_Y) * v >> 12);
     const int32_t x = VX - (SISTER_WALL_X * ls >> 10), y = VY + (hy * ls >> 10);
     if (j) line2(r, px, py, x, y, teal);
     px = x; py = y;
    }
   }
  }
  if (f <= 3) {                                                         /* the columns */
   const unsigned n = f <= 1 ? sizeof sister_col / sizeof sister_col[0] : SISTER_COL_FAR;
   for (unsigned i = 0; i < n; i++)
    line2(r, VX + (sister_col[i][0] * s >> 10), VY + (sister_col[i][1] * s >> 10),
          VX + (sister_col[i][2] * s >> 10), VY + (sister_col[i][3] * s >> 10), teal);
  }
 }

 /* ---- the rose window's petals (its ring and quatrefoil are the SVG's) ---- */
 {
  const int cx = RX, cy = RY;
  /* petals: green (paler at low VOICE), lit by the tail; CHOIR's are the sketch's lobes when silent and sing the
     drifting vowels with the level (aah tall, ooh round, eeh wide; alternate petals for the left and right voices),
     PIPES' are pipes of the root, fifth and octave; between the two ends both, crossfading */
  uint8_t base[4], pc[4], cc[4], gp[4];
  static const uint8_t pale[4] = { 0xA8, 0xF0, 0xD4, 255 }, glow[4] = { 0xB4, 0xFF, 0xE0, 255 };
  blend(base, pale, green, 26214 + (int32_t)(((int64_t)e_green * 6553) >> 15));     /* 80 % green at VOICE 0 */
  shade(base, base, 30147);                                                          /* 92 % when silent */
  blend(pc, base, glow, (int32_t)(((int64_t)e_lit * 11469) >> 15));                 /* lit: up to 35 % toward glow */
  const int32_t tc = (32767 - e_xc) * 2 > 32767 ? 32767 : (32767 - e_xc) * 2, tp = e_xc * 2 > 32767 ? 32767 : e_xc * 2;
  shade(cc, pc, tc);
  shade(gp, pc, tp);
  for (unsigned i = 0; i < 12; i++) {
   if (i == 6) continue;                                                        /* straight down: behind the veil */
   const int32_t dx = sister_petal_dir[i][0], dy = sister_petal_dir[i][1];      /* Q14, radial */
   #define PT(rr, t, X, Y) do { X = cx + (int32_t)(((int64_t)dx * (rr) - (int64_t)dy * (t)) >> 12); \
                                Y = cy + (int32_t)(((int64_t)dy * (rr) + (int64_t)dx * (t)) >> 12); } while (0)
   if (e_xc < 31000) {
    const int32_t p = e_vow[i & 1];
    const unsigned vi = (unsigned)p >> 12, vj = vi == 2 ? 0 : vi + 1;
    const int32_t fr = p & 4095;
    /* Q8 head centre, radial and tangential radius: aah, ooh, eeh */
    static const int16_t vc[3] = { 4147, 4019, 4070 }, vr[3] = { 1101, 768, 717 }, vt[3] = { 742, 768, 1024 };
    const int32_t c0 = vc[vi] + (((vc[vj] - vc[vi]) * fr) >> 12), r0 = vr[vi] + (((vr[vj] - vr[vi]) * fr) >> 12),
                  t0 = vt[vi] + (((vt[vj] - vt[vi]) * fr) >> 12);
    const int32_t hc = SISTER_PETAL_C + (int32_t)(((int64_t)(c0 - SISTER_PETAL_C) * e_lit) >> 15) + (i & 1 ? e_breath : e_breath * 3 / 5);
    const int32_t hr = SISTER_PETAL_R + (int32_t)(((int64_t)(r0 - SISTER_PETAL_R) * e_lit) >> 15);
    const int32_t ht = SISTER_PETAL_R + (int32_t)(((int64_t)(t0 - SISTER_PETAL_R) * e_lit) >> 15);
    int32_t px, py, x, y;
    PT(SISTER_PETAL_IN, -SISTER_PETAL_STEM, px, py);
    for (unsigned j = 0; j < sizeof sister_arc / sizeof sister_arc[0]; j++) {
     PT(hc + (hr * sister_arc[j][0] >> 12), ht * sister_arc[j][1] >> 12, x, y);
     cline(r, px, py, x, y, cc, 1);
     px = x; py = y;
    }
    PT(SISTER_PETAL_IN, SISTER_PETAL_STEM, x, y);
    cline(r, px, py, x, y, cc, 1);
   }
   if (e_xc > 1700) {                                                           /* a pipe from the hub */
    static const int16_t len[3] = { 3328, 2227, 1664 };                         /* Q8: 13, 8.7, 6.5 px */
    const int32_t L = len[i % 3] + e_breath / 2, w = 410;
    int32_t q[4][2];
    PT(SISTER_PETAL_IN, -w, q[0][0], q[0][1]);
    PT(SISTER_PETAL_IN + L, -w, q[1][0], q[1][1]);
    PT(SISTER_PETAL_IN + L, w, q[2][0], q[2][1]);
    PT(SISTER_PETAL_IN, w, q[3][0], q[3][1]);
    for (unsigned j = 0; j < 3; j++) cline(r, q[j][0], q[j][1], q[j + 1][0], q[j + 1][1], gp, 1);
    int32_t m0x, m0y, m1x, m1y;                                                 /* the pipe's mouth */
    PT(SISTER_PETAL_IN + L * 3 / 10, -w, m0x, m0y);
    PT(SISTER_PETAL_IN + L * 3 / 10, w, m1x, m1y);
    cline(r, m0x, m0y, m1x, m1y, gp, 1);
   }
   #undef PT
  }
 }

 /* ---- the candles: flames at the sketch's size at SWELL 0, up to twice that at 99 (VESPER), flickering with the tail ---- */
 {
  for (unsigned c = 0; c < 3; c++) {
   if ((frames + c * 3) % 4 == 0) {                         /* a new flicker target now and then, eased toward */
    rng = rng * 1664525u + 1013904223u;
    e_flick[c] = (int32_t)(rng >> 17) - 16384;            /* -16384 .. 16383 */
   }
   static int32_t fk[3];
   fk[c] = ease(fk[c], (int32_t)(((int64_t)e_flick[c] * e_lit) >> 15), af);
   const int32_t h0 = sister_candle[c][2], w0 = sister_candle[c][3];          /* Q8 */
   /* height Q8: the sketch's at SWELL 0, twice at 99, +-1.6 px of flicker at full tail */
   const int32_t t = h0 + (int32_t)(((int64_t)k[2] * h0) >> 15) + (int32_t)(((int64_t)fk[c] * 410) >> 14);
   e_flame[c] = ease(e_flame[c] ? e_flame[c] : t, t, af);
   const int32_t h = e_flame[c] < 512 ? 512 : e_flame[c], w = w0 + ((h - h0) >> 2);
   const int32_t sway = (int32_t)(((int64_t)fk[c] * 160) >> 14);              /* the tip leans with the flicker */
   const int32_t bx = sister_candle[c][0] * 4, by = sister_candle[c][1] * 4;
   const unsigned nfl = sizeof sister_flame / sizeof sister_flame[0];
   for (int side = -1; side <= 1; side += 2) {
    int32_t px = 0, py = 0;
    for (unsigned j = 0; j < nfl; j++) {
     const int32_t yq = sister_flame[j].y;                 /* 0 .. -256 (Q8 of the height) */
     const int32_t x = bx + side * (int32_t)(((int64_t)sister_flame[j].x * w) >> 6) - (int32_t)(((int64_t)sway * yq) >> 6);
     const int32_t y = by + (int32_t)(((int64_t)yq * h) >> 6);
     if (j) line(r, px, py, x, y, 1024, white);
     px = x; py = y;
    }
   }
  }
 }

 /* ---- MIX bar (Father's place) and the corner labels ---- */
 const int height = (int)(((int64_t)e_mix * 46 + 16383) >> 15);
 line(r, 302 * 1024, 36 * 1024, 302 * 1024, 82 * 1024, 9 * 1024, dimred);
 if (height) line(r, 302 * 1024, (82 - height) * 1024, 302 * 1024, 82 * 1024, 9 * 1024, red);
 const unsigned value = ((unsigned)k[0] * 99u + 16383u) >> 15;
 char number[3] = { (char)('0' + value / 10), (char)('0' + value % 10), 0 };
 text_slot = 0;
 text(r, 14, 11, 12, "space", blue);
 text(r, 14, 27, 30, number, white);
 if (op1_fx_page[pos][1]) {                                   /* green's page 2: the PIPES key */
  static const char names[12][3] = { "c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b" };   /* the font has no capitals */
  int32_t c = sister_pos[pos].key_cents, semi = (c + 2400 + 50) / 100;  /* 0 .. 48 from C1 */
  if (semi < 0) semi = 0;
  if (semi > 48) semi = 48;
  char kt[10] = "key ";
  unsigned n = 4;
  for (const char *p = names[semi % 12]; *p; p++) kt[n++] = *p;
  kt[n++] = (char)('1' + semi / 12);
  kt[n] = 0;
  text(r, 14, 126, 12, kt, green);
 } else text(r, 14, 126, 12, "voice", green);
 text(r, 260, 126, 12, "swell", white);
 text(r, 284, 11, 12, "mix", red);
 /* The native stock FX overlay owns bypass dimming and its central FX OFF label. */
}
const op1_screen_t SCREEN_sister = { init, update };
