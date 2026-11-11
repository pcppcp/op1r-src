/* Sample editor for the synth sampler (docs/27). Synth key + scissors opens it for the current slot.
 * Markers = the stock sampler's knob words (0 start, 3 end; fractions of the whole 6 s buffer, 32768 = 1.0), so keys
 * play the edited range at once. Optional snap to zero crossings. Trim rotates the kept region to the front of the
 * buffer (in place, no copy): undo (Shift + left) rotates back until the editor closes or the audio is edited again;
 * closing (or that edit) zeroes the rest. */
#include "../op1_synth.h"

#define SE_N_SYNTH  (264600 - 8)             /* audio samples after the 8 guard samples (capacity 264600) */
#define SE_N_DRUM   (529200 - 8)             /* drum: 12 s */
#define SE_N        se_n
static int32_t se_n = SE_N_SYNTH;           /* the open buffer's audio length (one editor at a time) */
#define SE_ZC_RANGE 2048                     /* zero-crossing search: +- samples around the marker */
#define SE_X0       10                       /* waveform area (px) */
#define SE_X1       310
#define SE_COLS     (SE_X1 - SE_X0)
#define SE_YMID     64
#define SE_YAMP     38
#define SE_KEY_Y    128                      /* T1..T4 labels: one 80 px zone per key */

typedef struct {
    int16_t *d;                              /* sample data (after the guard samples) */
    int16_t *w;                              /* synth: knob words [8] (live) */
    uint8_t drum;                            /* 0 synth sampler (markers = knob words 0 / 3), 1 drum (pad table) */
    uint8_t pad;                             /* drum: pad 0..23 being edited */
    int16_t *pt;                             /* drum: live pad table of the slot, 24 x 8 int16 (docs/27) */
    int32_t umax;                            /* largest marker unit: 32767 (words) or se_n (samples) */
    int32_t view0, view_len;                 /* visible sample range */
    uint8_t snap, last;                      /* snap: 0 off, 1 zero crossings, 2 transients; last moved: 0 start, 1 end */
    uint8_t on_dirty;                        /* transient list needs recomputing (audio changed) */
    int16_t n_on;
    int32_t on[256];                         /* transient positions (samples), ascending */
    uint8_t trimmed;                         /* undo available */
    int32_t tr_a, tr_b;                      /* trim: kept range [a, b) before the rotation */
    int16_t tr_w[8];                         /* knob words before the trim */
    int16_t tr_edge[2][132];                 /* samples under the de-click fades (restored by undo) */
    int32_t tr_fade, tr_pe;                  /* fade length; the end (samples) the fades were made at */
    int32_t acc[4];
    int16_t col_min[SE_COLS], col_max[SE_COLS];
    int32_t shown_cs;                        /* length text currently set (centiseconds, -1: none) */
    int32_t att;                             /* attenuation in 0.5 dB steps (-SE_BOOST_MAX .. SE_ATT_MAX; < 0 = louder), applied on close */
    int32_t att_amp;                         /* audio factor, Q16 */
    int16_t w7;                              /* sampler gain word before the preview */
    int32_t shown_att;
    int16_t shown_pad;
    uint8_t loopsel;                         /* synth: 1 = blue / green move the loop points (blue press toggles) */
    uint8_t mode;                            /* 0 edit, 1 fade, 2 auto-cut (drum) */
    uint8_t ac_method, ac_undo;              /* auto-cut: 0 hits, 1 equal; undo available */
    int8_t ac_sens;                          /* transient rise ratio 2..8, lower = more hits (auto-cut blue, tr: shift + red) */
    int8_t shown_sens;
    int16_t ac_n;                            /* proposed cuts */
    int8_t ac_parts;                         /* equal: number of parts 1..24 (blue) */
    int32_t ac_cut[24][2];                   /* proposed pad regions (samples) */
    int32_t ac_old[24][2];                   /* pad regions before the last auto-cut (undo) */
    uint8_t fd_shape, fd_curve;              /* fade: 0 in, 1 out, 2 both; 0 linear, 1 equal power, 2 exp */
    int16_t fd_pct;                          /* fade length, % of the selection (both: of each half) */
    int32_t cache_v0, cache_len;             /* view the columns were computed for (-1: none) */
} sampedit;

/* ---- core (host-tested) ---- */
int32_t se_w2s(int32_t w) { return (int32_t)(((int64_t)w * SE_N) >> 15); }
int32_t se_s2w(int32_t s, int round_down)
{
    int64_t v = ((int64_t)s << 15) + (round_down ? 0 : SE_N / 2);
    int32_t w = (int32_t)(v / SE_N);
    return w < 0 ? 0 : w > 32767 ? 32767 : w;
}

/* markers in units: synth = knob words (0 start, 3 end, 1 / 2 loop), drum = samples of the current pad.
 * Drum pad entry (int16): +1 start >> 16, +2 end >> 16, +5 / +6 low 16 bits >> 1; value = fraction x 2^31 of 12 s */
#define SE_CAP_DRUM 529200
static int32_t se_pad_get(const sampedit *e, int k)
{
    const int16_t *p = e->pt + e->pad * 8;
    int32_t v = ((int32_t)p[k ? 2 : 1] << 16) + ((uint16_t)p[k ? 6 : 5] << 1);
    int32_t s = (int32_t)(((int64_t)v * SE_CAP_DRUM + (1LL << 30)) >> 31);
    return s > SE_N ? SE_N : s < 0 ? 0 : s;
}
static void se_pad_set(sampedit *e, int k, int32_t s)
{
    int16_t *p = e->pt + e->pad * 8;
    int64_t v = (((int64_t)s << 31) + SE_CAP_DRUM / 2) / SE_CAP_DRUM;
    if (v > 0x7FFFFFFF) v = 0x7FFFFFFF;
    p[k ? 2 : 1] = (int16_t)(v >> 16); p[k ? 6 : 5] = (int16_t)((v & 0xFFFF) >> 1);
}
/* drum: room for a paste at a: up to the next other pad that starts after a (the pad's own end, if later) */
static int32_t se_pad_room(sampedit *e, int32_t a)
{
    int keep = e->pad;
    int32_t lim = SE_N, own = se_pad_get(e, 1);
    for (int i = 0; i < 24; i++) {
        if (i == keep) continue;
        e->pad = (uint8_t)i;
        int32_t s = se_pad_get(e, 0), t = se_pad_get(e, 1);
        if (t > s && s > a && s < lim) lim = s;
    }
    e->pad = (uint8_t)keep;
    return (own > lim ? own : lim) - a;
}
static int32_t SE_GET(const sampedit *e, int k) { return e->drum ? se_pad_get(e, k == 3) : e->w[k]; }
static void SE_SET(sampedit *e, int k, int32_t v) { if (e->drum) se_pad_set(e, k == 3, v); else e->w[k] = (int16_t)v; }
static int32_t U2S(const sampedit *e, int32_t u) { return e->drum ? u : se_w2s(u); }
static int32_t S2U(const sampedit *e, int32_t s, int rd) { return e->drum ? (s < 0 ? 0 : s > SE_N ? SE_N : s) : se_s2w(s, rd); }

/* nearest zero crossing (sign change between i-1 and i, or a 0 sample) within +- SE_ZC_RANGE; -1 if none */
int32_t se_zero_cross(const int16_t *d, int32_t s)
{
    for (int32_t r = 0; r <= SE_ZC_RANGE; r++) {
        for (int k = 0; k < 2; k++) {
            int32_t i = k ? s - r : s + r;
            if (i < 1 || i >= SE_N || (k && !r)) continue;
            if (d[i] == 0 || (d[i - 1] < 0) != (d[i] < 0)) return i;
        }
    }
    return -1;
}

/* first zero crossing at or after s in direction dir (+1 / -1), within SE_ZC_RANGE; -1 if none */
int32_t se_zero_cross_dir(const int16_t *d, int32_t s, int dir)
{
    for (int32_t r = 0, i = s; r <= SE_ZC_RANGE; r++, i += dir) {
        if (i < 1 || i >= SE_N) return -1;
        if (d[i] == 0 || (d[i - 1] < 0) != (d[i] < 0)) return i;
    }
    return -1;
}

static void se_reverse(int16_t *d, int32_t a, int32_t b)          /* [a, b) */
{
    for (b--; a < b; a++, b--) { int16_t t = d[a]; d[a] = d[b]; d[b] = t; }
}
/* rotate d[0, n) left by k: [k, n) to the front */
void se_rotate(int16_t *d, int32_t n, int32_t k)
{
    if (k <= 0 || k >= n) return;
    se_reverse(d, 0, k); se_reverse(d, k, n); se_reverse(d, 0, n);
}

/* trim to [start, end) of the knob words; returns 0 if nothing to do */
int se_trim(sampedit *e)
{
    int32_t a = U2S(e, SE_GET(e, 0)), b = U2S(e, SE_GET(e, 3));
    if (e->trimmed || b - a < 64 || (a == 0 && b >= SE_N - 8)) return 0;
    for (int i = 0; i < 8; i++) e->tr_w[i] = e->w[i];
    e->tr_a = a; e->tr_b = b;
    se_rotate(e->d, b, a);                                        /* [a, b) now at [0, b - a) */
    int32_t len = b - a;
    int16_t endw = (int16_t)S2U(e, len, 1);                       /* down: never play into the moved head */
    for (int i = 1; i <= 2; i++) {                                /* loop points move with the audio, clamped */
        int32_t p = U2S(e, e->w[i]) - a;
        e->w[i] = (int16_t)(p <= 0 ? 0 : S2U(e, p, 1) > endw ? endw : S2U(e, p, 1));
    }
    SE_SET(e, 0, 0); SE_SET(e, 3, endw);
    /* de-click: 3 ms linear fade in / out at the new edges (the end at the played end word) */
    int32_t pe = U2S(e, endw), f = 132;
    if (f > pe / 4) f = pe / 4;
    e->tr_fade = f; e->tr_pe = pe;
    for (int32_t i = 0; i < f; i++) {
        e->tr_edge[0][i] = e->d[i]; e->tr_edge[1][i] = e->d[pe - 1 - i];
        e->d[i] = (int16_t)(e->d[i] * i / f);
        e->d[pe - 1 - i] = (int16_t)(e->d[pe - 1 - i] * i / f);
    }
    e->trimmed = 1; e->cache_len = -1; e->on_dirty = 1;
    e->view0 = 0; e->view_len = SE_N;
    return 1;
}
int se_undo(sampedit *e)
{
    if (!e->trimmed) return 0;
    int32_t pe = e->tr_pe;                                        /* where the fades were made (the end marker may have moved) */
    for (int32_t i = 0; i < e->tr_fade; i++) { e->d[i] = e->tr_edge[0][i]; e->d[pe - 1 - i] = e->tr_edge[1][i]; }
    se_rotate(e->d, e->tr_b, e->tr_b - e->tr_a);                  /* right by a = left by b - a */
    for (int i = 0; i < 8; i++) e->w[i] = e->tr_w[i];
    e->trimmed = 0; e->cache_len = -1; e->on_dirty = 1;
    return 1;
}
/* closing the editor: a trim becomes final, the area after the new end is silence */
void se_finalize(sampedit *e)
{
    if (!e->trimmed) return;
    for (int32_t i = e->tr_b - e->tr_a; i < SE_N; i++) e->d[i] = 0;
    e->trimmed = 0; e->cache_len = -1;
}
/* an audio edit after a trim: the trim becomes final first (undo would rotate audio the edit moved or changed, and
 * closing would zero from the old end, over pasted audio) */
static void se_edit(sampedit *e) { se_finalize(e); }

/* red: gain of the whole sample, -60 .. +24 dB in 0.5 dB steps. Preview: the stock sampler gain (word 7; playback
 * gain ~ word^2, SamplerScreen shows 20 log10(16 p^2) dB) set to w7 * sqrt(A) (capped at the word's maximum);
 * applied to the audio when the editor closes (louder: saturating, peaks above full scale clip). */
#define SE_ATT_MAX   120                     /* 60 dB down */
#define SE_BOOST_MAX 48                      /* 24 dB up */
#define SE_AMP_STEP  61870                   /* 10^(-0.5/20), Q16 */
#define SE_WORD_STEP 63677                   /* 10^(-0.5/40), Q16 */
#define SE_AMP_UP    69419                   /* 10^(0.5/20), Q16 */
#define SE_WORD_UP   67450                   /* 10^(0.5/40), Q16 */
void se_attenuate(sampedit *e, int32_t steps)
{
    int32_t a = e->att + steps;
    a = a < -SE_BOOST_MAX ? -SE_BOOST_MAX : a > SE_ATT_MAX ? SE_ATT_MAX : a;
    if (a == e->att) return;
    e->att = a;
    int32_t amp = 65536, wf = 65536, n = a < 0 ? -a : a;
    for (int32_t i = 0; i < n; i++) {
        amp = (int32_t)(((int64_t)amp * (a < 0 ? SE_AMP_UP : SE_AMP_STEP) + 0x8000) >> 16);
        wf = (int32_t)(((int64_t)wf * (a < 0 ? SE_WORD_UP : SE_WORD_STEP) + 0x8000) >> 16);
    }
    e->att_amp = amp;
    if (!e->drum) {                                               /* drum: preview on screen only */
        int32_t w = (int32_t)(((int64_t)e->w7 * wf + 0x8000) >> 16);
        e->w[7] = (int16_t)(w > 32767 ? 32767 : w);
    }
}
/* closing: bake the gain into the audio (saturating), the gain word back as it was */
int se_apply_att(sampedit *e)
{
    if (!e->att) return 0;
    for (int32_t i = 0; i < SE_N; i++) {
        int32_t v = (int32_t)(((int64_t)e->d[i] * e->att_amp + 0x8000) >> 16);
        e->d[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    if (!e->drum) e->w[7] = e->w7;
    e->att = 0; e->att_amp = 65536; e->cache_len = -1;           /* (scaling keeps the transient positions) */
    return 1;
}

/* transients: envelope = max |x| per 128 samples; an onset is a block above -30 dB and >= 4x (12 dB) the two
 * before it, >= 16 blocks (~46 ms) after the previous one; position = first sample above 1/4 of that block's peak,
 * moved back to the preceding zero crossing (the marker sits just before the hit). Once per audio change. */
#define SE_TR_BLK   128
#define SE_TR_MIN   1036                     /* -30 dBFS */
void se_transients(sampedit *e)
{
    e->n_on = 0; e->on_dirty = 0;
    /* stricter (hardware: hits inside one sound): a hit must rise above the loudest of the previous 8 blocks
     * (~23 ms, so peaks in a decaying tail do not count), reach -26 dB relative to the sample's loudest point
     * (and -30 dBFS), and be >= 32 blocks (~93 ms) after the previous one */
    int32_t gpk = 0;
    for (int32_t i = 0; i < SE_N; i++) { int32_t v = e->d[i] < 0 ? -e->d[i] : e->d[i]; if (v > gpk) gpk = v; }
    int32_t floor_ = gpk / 20 > SE_TR_MIN ? gpk / 20 : SE_TR_MIN;
    int32_t hist[8] = { 0 }, last = -1000;
    for (int32_t k = 0; (k + 1) * SE_TR_BLK <= SE_N; k++) {
        int32_t pk = 0;
        for (int32_t i = k * SE_TR_BLK; i < (k + 1) * SE_TR_BLK; i++) { int32_t v = e->d[i] < 0 ? -e->d[i] : e->d[i]; if (v > pk) pk = v; }
        int32_t before = 0;
        for (int h = 0; h < 8; h++) if (hist[h] > before) before = hist[h];
        if (pk >= floor_ && pk >= (e->ac_sens ? e->ac_sens : 4) * before && k - last >= 32 && e->n_on < 256) {
            int32_t i = k * SE_TR_BLK, thr = pk / 4;
            while (i < (k + 1) * SE_TR_BLK - 1 && (e->d[i] < 0 ? -e->d[i] : e->d[i]) < thr) i++;
            int32_t z = se_zero_cross_dir(e->d, i, -1);
            if (z >= 0 && i - z <= 2 * SE_TR_BLK) i = z;
            if (!e->n_on || i > e->on[e->n_on - 1]) e->on[e->n_on++] = i;
            last = k;
        }
        hist[k & 7] = pk;
    }
}
/* next snap target from s in direction dir: zero crossing or transient; -1 if none */
static int32_t se_snap_next(sampedit *e, int32_t s, int dir)
{
    if (e->snap == 1) return se_zero_cross_dir(e->d, s, dir);
    if (e->on_dirty) se_transients(e);
    if (dir > 0) { for (int i = 0; i < e->n_on; i++) if (e->on[i] >= s) return e->on[i]; }
    else for (int i = e->n_on - 1; i >= 0; i--) if (e->on[i] <= s) return e->on[i];
    return -1;
}

/* Shift + T1: remove the DC offset of the selection (its mean) */
int se_dc(sampedit *e)
{
    int32_t a = U2S(e, SE_GET(e, 0)), b = U2S(e, SE_GET(e, 3));
    if (b <= a) return 0;
    int64_t sum = 0;
    for (int32_t i = a; i < b; i++) sum += e->d[i];
    int32_t m = (int32_t)(sum / (b - a));
    if (!m) return 0;
    se_edit(e);
    for (int32_t i = a; i < b; i++) { int32_t v = e->d[i] - m; e->d[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
    e->cache_len = -1; e->on_dirty = 1;
    return 1;
}

/* ---- fade (Shift + T2 enters the fade mode; T1 ok applies) ---- */
static const int16_t se_qsin[65] = {     /* 32767 sin(pi/2 i/64): equal-power curve */
    0,804,1608,2410,3212,4011,4808,5602,6393,7179,7962,8739,9512,10278,11039,11793,12539,13279,14010,14732,15446,
    16151,16846,17530,18204,18868,19519,20159,20787,21403,22005,22594,23170,23731,24279,24811,25329,25832,26319,
    26790,27245,27683,28105,28510,28898,29268,29621,29956,30273,30571,30852,31113,31356,31580,31785,31971,32137,
    32285,32412,32521,32609,32678,32728,32757,32767 };
static const int16_t se_qexp[65] = {     /* 32767 (10^(3 i/64) - 1) / 999: exponential, 60 dB range */
    0,4,8,13,18,23,30,37,45,54,64,75,87,101,116,133,152,173,196,222,251,284,320,360,405,454,510,572,641,718,803,
    898,1004,1123,1254,1401,1564,1746,1949,2175,2427,2707,3019,3367,3755,4187,4667,5203,5800,6465,7205,8030,8949,
    9973,11113,12384,13799,15375,17131,19088,21267,23694,26399,29411,32767 };
static int32_t se_curve(const sampedit *e, int32_t t)            /* t, result: Q15 (0..32768) */
{
    if (!e->fd_curve) return t;
    const int16_t *c = e->fd_curve == 2 ? se_qexp : se_qsin;
    int32_t x = t * 64, i = x >> 15, f = x & 0x7FFF;              /* interpolated table */
    if (i >= 64) return 32767;
    return c[i] + (((c[i + 1] - c[i]) * f) >> 15);
}
/* gain (Q15) at sample i of the selection [a, b) */
int32_t se_fade_gain(const sampedit *e, int32_t i, int32_t a, int32_t b)
{
    int32_t n = b - a, len = e->fd_shape == 2 ? n / 2 : n;
    len = (int32_t)((int64_t)len * e->fd_pct / 100);
    if (len < 1) return 32768;
    int32_t g = 32768, p = i - a;
    if (e->fd_shape != 1 && p < len) g = se_curve(e, (int32_t)(((int64_t)p << 15) / len));                 /* in */
    if (e->fd_shape != 0 && b - 1 - i < len) {                                                                /* out */
        int32_t go = se_curve(e, (int32_t)(((int64_t)(b - 1 - i) << 15) / len));
        if (go < g) g = go;
    }
    return g;
}
int se_fade_apply(sampedit *e)
{
    int32_t a = U2S(e, SE_GET(e, 0)), b = U2S(e, SE_GET(e, 3));
    if (b - a < 2) return 0;
    se_edit(e);
    for (int32_t i = a; i < b; i++) e->d[i] = (int16_t)((e->d[i] * se_fade_gain(e, i, a, b)) >> 15);
    e->cache_len = -1; e->on_dirty = 1;
    return 1;
}
void se_fade_len(sampedit *e, int32_t steps)                      /* blue in the fade mode: 2 % per step */
{
    int32_t v = e->fd_pct + 2 * steps;
    e->fd_pct = (int16_t)(v < 1 ? 1 : v > 100 ? 100 : v);
}

/* ---- clipboard (the stock audio clipboard, as a single-track tape lift leaves it; docs/27) ---- */
#ifdef OP1_HOST
extern int16_t host_clip[];
extern int32_t host_clip_len, host_clip_kind;
#define CLIP_READY()     1
#define CLIP_BUF()       host_clip
#define CLIP_MAX         (1 << 20)
static void clip_set(int32_t n) { host_clip_len = n; host_clip_kind = 1; }
static const int16_t *clip_get(int32_t *n) { *n = host_clip_kind == 1 ? host_clip_len : 0; return host_clip; }
#else
#define CLIP_READY()     (*(volatile int32_t *)0x01AA7330 == *(volatile int32_t *)0x01AA7334)   /* queue idle */
#define CLIP_BUF()       ((int16_t *)0x02040010)                  /* audio_clipboard 0x0191C9FC */
#define CLIP_MAX         0x3C8FE7
static void clip_set(int32_t n)
{
    int32_t *tab = (int32_t *)0x02040000;
    tab[0] = 0x02040010; tab[1] = tab[2] = tab[3] = 0;
    *(volatile int32_t *)0x01AA7100 = 0;                         /* source channel */
    *(volatile int32_t *)0x01AA7104 = n;                         /* length */
    *(volatile int32_t *)0x01AA7108 = 1;                         /* tape audio */
    *(volatile int32_t *)0x01AA710C = 0;                         /* single track (-1: all four, mixed) */
    *(volatile int32_t *)0x01AA7110 = n;
    *(volatile int32_t *)0x01AA7114 = 1;
}
static const int16_t *clip_get(int32_t *n)                       /* tape audio only (kind 1, one channel) */
{
    int32_t ch = *(volatile int32_t *)0x01AA7100;
    if (*(volatile int32_t *)0x01AA7108 != 1 || ch < 0 || ch > 3) { *n = 0; return 0; }
    *n = *(volatile int32_t *)0x01AA7104;
    return (const int16_t *)((int32_t *)0x02040000)[ch];
}
#endif
int se_copy(sampedit *e)                                          /* lift */
{
    int32_t a = U2S(e, SE_GET(e, 0)), b = U2S(e, SE_GET(e, 3)), n = b - a;
    if (n < 2 || n > CLIP_MAX || !CLIP_READY()) return 0;
    int16_t *c = CLIP_BUF();
    for (int32_t i = 0; i < n; i++) c[i] = e->d[a + i];
    clip_set(n);
    return 1;
}
int se_cut(sampedit *e)                                           /* scissors: copy, remove, close the gap */
{
    if (!se_copy(e)) return 0;
    se_edit(e);
    int32_t a = U2S(e, SE_GET(e, 0)), b = U2S(e, SE_GET(e, 3));
    if (e->drum) {                                                /* drum: the other pads stay where they are: zero */
        for (int32_t i = a; i < b; i++) e->d[i] = 0;
        e->cache_len = -1; e->on_dirty = 1;
        return 1;
    }
    for (int32_t i = b; i < SE_N; i++) e->d[a + i - b] = e->d[i];
    for (int32_t i = SE_N - (b - a); i < SE_N; i++) e->d[i] = 0;
    int32_t ew = SE_GET(e, 3) - (SE_GET(e, 3) - SE_GET(e, 0));                   /* selection collapses at the start */
    SE_SET(e, 3, (ew + 1 > e->umax ? e->umax : ew + 1));
    e->cache_len = -1; e->on_dirty = 1;
    return 1;
}
int se_paste(sampedit *e)                                         /* drop: insert at the start marker */
{
    int32_t n; const int16_t *c = clip_get(&n);
    int32_t a = U2S(e, SE_GET(e, 0));
    if (!c || n <= 0 || !CLIP_READY()) return 0;
    if (n > SE_N - a) n = SE_N - a;
    if (e->drum) { int32_t m = se_pad_room(e, a); if (n > m) n = m; }   /* drum: overwrite up to the next pad */
    if (n <= 0) return 0;
    se_edit(e);
    if (!e->drum)                                                 /* drum: overwrite (other pads keep their audio) */
        for (int32_t i = SE_N - 1; i >= a + n; i--) e->d[i] = e->d[i - n];   /* push the rest right (cut off at 6 s) */
    for (int32_t i = 0; i < n; i++) e->d[a + i] = c[i];
    SE_SET(e, 3, S2U(e, a + n, 1));                          /* the pasted part is the selection */
    if (SE_GET(e, 3) <= SE_GET(e, 0)) SE_SET(e, 3, (SE_GET(e, 0) + 1));
    e->cache_len = -1; e->on_dirty = 1;
    return 1;
}

/* ---- drum: fit, auto-cut ---- */
#define SE_SIL      103                      /* -50 dBFS: "silent" for fit and the pad ends */
static int32_t se_abs(int16_t v) { return v < 0 ? -v : v; }
/* T1 (drum): shrink the pad to its audible part (start on the preceding zero crossing) */
int se_fit(sampedit *e)
{
    int32_t a = U2S(e, SE_GET(e, 0)), b = U2S(e, SE_GET(e, 3)), i = a, j = b - 1;
    while (i < b && se_abs(e->d[i]) < SE_SIL) i++;
    while (j > i && se_abs(e->d[j]) < SE_SIL) j--;
    if (i >= b) return 0;
    int32_t z = se_zero_cross_dir(e->d, i, -1);
    if (z >= a && z <= i) i = z;
    if (i == a && j + 1 == b) return 0;                          /* already tight */
    SE_SET(e, 0, i); SE_SET(e, 3, j + 1);
    return 1;
}
/* end of a pad starting at s: the next cut, or earlier where the sound stays below SE_SIL until it */
static int32_t se_pad_end(const sampedit *e, int32_t s, int32_t next)
{
    int32_t j = next - 1;
    while (j > s && se_abs(e->d[j]) < SE_SIL) j--;
    return j + 1 > s + 64 ? j + 1 : (next > s + 64 ? next : s + 64);
}
/* compute the proposed cuts (preview) */
void se_ac_compute(sampedit *e)
{
    int32_t last = SE_N - 1;
    while (last > 0 && se_abs(e->d[last]) < SE_SIL) last--;
    int32_t st[24], n = 0;
    if (e->ac_method == 0) {
        se_transients(e);
        for (int i = 0; i < e->n_on && n < 24; i++) if (e->on[i] < last) st[n++] = e->on[i];   /* first 24 */
    } else if (last > e->ac_parts * 64) {
        for (n = 0; n < e->ac_parts; n++) {
            int32_t p = (int32_t)((int64_t)(last + 1) * n / e->ac_parts);
            int32_t z = n ? se_zero_cross_dir(e->d, p, -1) : 0;
            st[n] = z >= 0 && p - z < 1024 ? z : p;
        }
    }
    for (int i = 0; i < n; i++) {
        int32_t next = i + 1 < n ? st[i + 1] : last + 1;
        e->ac_cut[i][0] = st[i];
        e->ac_cut[i][1] = e->ac_method ? next : se_pad_end(e, st[i], next);
    }
    e->ac_n = (int16_t)n;
}
static void se_pad_set_pair(sampedit *e, int pad, int32_t a, int32_t b)
{
    int keep = e->pad; e->pad = (uint8_t)pad;
    se_pad_set(e, 0, a); se_pad_set(e, 1, b);
    e->pad = (uint8_t)keep;
}
/* T1 in the auto-cut mode: write the pads (from pad 1 upwards), keep the old regions for undo */
int se_ac_apply(sampedit *e)
{
    if (!e->drum || !e->ac_n) return 0;
    int keep = e->pad;
    for (int i = 0; i < 24; i++) { e->pad = (uint8_t)i; e->ac_old[i][0] = se_pad_get(e, 0); e->ac_old[i][1] = se_pad_get(e, 1); }
    e->pad = (uint8_t)keep;
    for (int i = 0; i < e->ac_n; i++) se_pad_set_pair(e, i, e->ac_cut[i][0], e->ac_cut[i][1]);
    e->ac_undo = 1;
    return 1;
}
int se_ac_undo(sampedit *e)
{
    if (!e->ac_undo) return 0;
    for (int i = 0; i < 24; i++) se_pad_set_pair(e, i, e->ac_old[i][0], e->ac_old[i][1]);
    e->ac_undo = 0;
    return 1;
}
/* transient snap sensitivity (Shift + red while tr is on): right = more hits; shown as "tr 1".."tr 7" */
void se_tr_sens(sampedit *e, int32_t steps)
{
    int32_t v = (e->ac_sens ? e->ac_sens : 4) - steps;
    e->ac_sens = (int8_t)(v < 2 ? 2 : v > 8 ? 8 : v);
    e->on_dirty = 1;                                              /* re-detect (once, lazily) */
}
void se_ac_parts(sampedit *e, int32_t steps)                      /* blue in the auto-cut mode (equal): 1..24 parts */
{
    int32_t v = e->ac_parts + steps;
    e->ac_parts = (int8_t)(v < 1 ? 1 : v > 24 ? 24 : v);
    se_ac_compute(e);
}
void se_ac_sens(sampedit *e, int32_t steps)                       /* blue in the auto-cut mode (hits) */
{
    int32_t v = (e->ac_sens ? e->ac_sens : 4) - steps;            /* right: more hits (lower ratio) */
    e->ac_sens = (int8_t)(v < 2 ? 2 : v > 8 ? 8 : v);
    se_ac_compute(e);
}

/* the selection not (fully) visible: zoom / scroll so it fills ~2/3 of the view */
void se_show_region(sampedit *e)
{
    int32_t a = U2S(e, SE_GET(e, 0)), b = U2S(e, SE_GET(e, 3));
    if (a >= e->view0 && b <= e->view0 + e->view_len) return;
    int32_t len = (b - a) * 3 / 2;
    if (len < SE_COLS) len = SE_COLS;
    if (len > SE_N) len = SE_N;
    e->view_len = len;
    e->view0 = a - (len - (b - a)) / 2;
    if (e->view0 < 0) e->view0 = 0;
    if (e->view0 + len > SE_N) e->view0 = SE_N - len;
}

/* T4: reverse the selection (in place; reversing again restores it) */
int se_reverse_sel(sampedit *e)
{
    int32_t a = U2S(e, SE_GET(e, 0)), b = U2S(e, SE_GET(e, 3));
    if (b - a < 2) return 0;
    se_edit(e);
    se_reverse(e->d, a, b);
    e->cache_len = -1; e->on_dirty = 1;
    return 1;
}

/* T2: silence the selection */
int se_zero(sampedit *e)
{
    int32_t a = U2S(e, SE_GET(e, 0)), b = U2S(e, SE_GET(e, 3));
    if (b <= a) return 0;
    se_edit(e);
    for (int32_t i = a; i < b; i++) e->d[i] = 0;
    e->cache_len = -1; e->on_dirty = 1;
    return 1;
}
/* T3: scale the selection so its loudest sample reaches full scale (gain once, Q16; no division per sample) */
int se_normalize(sampedit *e)
{
    int32_t a = U2S(e, SE_GET(e, 0)), b = U2S(e, SE_GET(e, 3)), peak = 0;
    for (int32_t i = a; i < b; i++) { int32_t v = e->d[i] < 0 ? -e->d[i] : e->d[i]; if (v > peak) peak = v; }
    if (peak == 0 || peak >= 32767) return 0;
    se_edit(e);
    int32_t g = (int32_t)(((int64_t)32767 << 16) / peak);
    for (int32_t i = a; i < b; i++) {
        int32_t v = (int32_t)(((int64_t)e->d[i] * g + 0x8000) >> 16);
        e->d[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    e->cache_len = -1; e->on_dirty = 1;
    return 1;
}
/* synth: loop points stay inside start..end (as in the stock sampler), loop start < loop end */
static void se_loop_clamp(sampedit *e)
{
    if (e->drum) return;
    int16_t *w = e->w;
    if (w[1] < w[0]) w[1] = w[0];
    if (w[1] > w[3] - 1) w[1] = (int16_t)(w[3] - 1);
    if (w[2] > w[3]) w[2] = w[3];
    if (w[2] < w[1] + 1) w[2] = (int16_t)(w[1] + 1);
}
/* Shift + white: move the selection, length kept (ZC: the start lands on a crossing in the turning direction) */
void se_shift_sel(sampedit *e, int32_t steps)
{
    int32_t len = SE_GET(e, 3) - SE_GET(e, 0);
    int32_t per = S2U(e, e->view_len, 0) / 200 > 1 ? S2U(e, e->view_len, 0) / 200 : 1;
    int32_t w0 = SE_GET(e, 0) + steps * per;
    if (e->snap && steps) {
        int dir = steps > 0 ? 1 : -1;
        int32_t s = U2S(e, w0), wz = SE_GET(e, 0);
        for (int tries = 0; tries < 64 && wz == SE_GET(e, 0); tries++) {
            int32_t z = se_snap_next(e, s, dir);
            if (z < 0) break;
            wz = S2U(e, z, 0); s = z + dir;
        }
        if (wz != SE_GET(e, 0)) w0 = wz;
    }
    if (w0 < 0) w0 = 0;
    if (w0 + len > e->umax) w0 = e->umax - len;
    SE_SET(e, 0, w0); SE_SET(e, 3, (w0 + len));
    se_loop_clamp(e);
}

/* move marker m (0 start, 1 end) by delta knob steps (fine: 1 word, else ~1/200 of the view) */
void se_move(sampedit *e, int m, int32_t steps, int fine)
{
    int lp = e->loopsel && !e->drum, idx = lp ? 1 + m : m ? 3 : 0, oth = lp ? 2 - m : m ? 0 : 3;
    int32_t per = fine ? 1 : (S2U(e, e->view_len, 0) / 200 > 1 ? S2U(e, e->view_len, 0) / 200 : 1);
    int32_t w = SE_GET(e, idx) + steps * per;
    int32_t lo = m ? SE_GET(e, oth) + 1 : lp ? SE_GET(e, 0) : 0, hi = m ? (lp ? SE_GET(e, 3) : e->umax) : SE_GET(e, oth) - 1;
    w = w < lo ? lo : w > hi ? hi : w;
    if (e->snap && steps) {
        /* the next crossing in the turning direction whose stored position differs from the old one (positions are
         * ~8-sample words): always moves on, zoomed in from crossing to crossing (hardware: "nearest" snapped back) */
        int dir = steps > 0 ? 1 : -1, old = SE_GET(e, idx);
        int32_t s = U2S(e, w), wz = old;
        for (int tries = 0; tries < 64 && wz == old; tries++) {
            int32_t z = se_snap_next(e, s, dir);
            if (z < 0) break;
            wz = S2U(e, z, 0);
            s = z + dir;
        }
        if (wz != old && wz >= lo && wz <= hi) w = wz;
    }
    SE_SET(e, idx, w);
    se_loop_clamp(e);                                             /* start / end moves push the loop along */
    if (lp) e->w[4 + idx] = 0;                                    /* fine word 5 / 6 off: the loop point is where it is drawn */
    e->last = (uint8_t)m;
}

/* zoom by factor 2 steps around the last moved marker; scroll by 1/8 view */
void se_zoom(sampedit *e, int steps)
{
    int32_t c = U2S(e, SE_GET(e, e->loopsel && !e->drum ? 1 + e->last : e->last ? 3 : 0));
    int32_t len = e->view_len;
    for (; steps > 0 && len > SE_COLS; steps--) len /= 2;
    for (; steps < 0 && len < SE_N; steps++) len *= 2;
    if (len > SE_N) len = SE_N;
    if (len < SE_COLS) len = SE_COLS;
    e->view_len = len;
    e->view0 = c - len / 2;
    if (e->view0 < 0) e->view0 = 0;
    if (e->view0 + len > SE_N) e->view0 = SE_N - len;
}
void se_scroll(sampedit *e, int steps)
{
    e->view0 += steps * (e->view_len / 8);
    if (e->view0 < 0) e->view0 = 0;
    if (e->view0 + e->view_len > SE_N) e->view0 = SE_N - e->view_len;
}

/* min / max per column for the current view (recomputed only when the view or the audio changed) */
void se_columns(sampedit *e)
{
    if (e->cache_v0 == e->view0 && e->cache_len == e->view_len) return;
    for (int c = 0; c < SE_COLS; c++) {
        int32_t s0 = e->view0 + (int32_t)((int64_t)e->view_len * c / SE_COLS);
        int32_t s1 = e->view0 + (int32_t)((int64_t)e->view_len * (c + 1) / SE_COLS);
        if (s1 <= s0) s1 = s0 + 1;
        int16_t lo = 32767, hi = -32768;
        for (int32_t i = s0; i < s1 && i < SE_N; i++) { if (e->d[i] < lo) lo = e->d[i]; if (e->d[i] > hi) hi = e->d[i]; }
        e->col_min[c] = lo; e->col_max[c] = hi;
    }
    e->cache_v0 = e->view0; e->cache_len = e->view_len;
}

/* ---- screen ---- */
#define SE_T_LEN  0
#define SE_T_ZC   1
#define SE_T_UNDO 2
#define SE_T_ATT  7
#define SE_T_TR   8
#define SE_T_PAD  9
#define SE_T_KEY  3                          /* + 0..3: labels above T1..T4 (each key ~ a quarter of the display) */
#define SE_TEXT_SIZE 12
static const char *const se_label[4] = { "trim", "zero", "norm", "rev" };
static uint32_t se_txt[SE_T_PAD + 1][OP1_TEXT_SIZE / 4];
static const char *se_shown_lbl[4];
#ifdef OP1_HOST
int se_host_shift;
static int se_shift_held(void) { return se_host_shift; }
#else
static int se_shift_held(void) { return *(volatile uint8_t *)0x01DD7F78; }
#endif
static const int32_t se_ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_grey[4] = { 0x6F, 0x71, 0x73, 0xFF },
                     c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF }, c_loop[4] = { 0xAE, 0xB1, 0xDC, 0xFF };
static void se_ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
static int32_t se_x(const sampedit *e, int32_t s)                 /* sample -> x Q10 (may be off screen) */
{
    return OP1_Q10(SE_X0) + (int32_t)(((int64_t)(s - e->view0) * SE_COLS << 10) / e->view_len);
}

void se_draw(sampedit *e, void *r)
{
    se_columns(e);
    int32_t a = U2S(e, SE_GET(e, 0)), b = U2S(e, SE_GET(e, 3));
    for (int c = 0; c < SE_COLS; c++) {                            /* waveform: one vertical line per column */
        int32_t s = e->view0 + (int32_t)((int64_t)e->view_len * c / SE_COLS);
        const uint8_t *col = s >= a && s < b ? c_white : c_grey;
        int32_t amp = e->att_amp;
        if (e->mode == 1 && s >= a && s < b) amp = (int32_t)(((int64_t)amp * se_fade_gain(e, s, a, b)) >> 15);
        int32_t hi = (int32_t)(((int64_t)e->col_max[c] * amp) >> 16), lo = (int32_t)(((int64_t)e->col_min[c] * amp) >> 16);
        hi = hi > 32767 ? 32767 : hi; lo = lo < -32768 ? -32768 : lo;  /* louder: clipped peaks stay in the area */
        int32_t y0 = OP1_Q10(SE_YMID) - (hi * SE_YAMP >> 5);         /* * 1024 / 32768 */
        int32_t y1 = OP1_Q10(SE_YMID) - (lo * SE_YAMP >> 5);
        if (y1 - y0 < 0x400) { y0 -= 0x200; y1 += 0x200; }        /* >= 1 px: zero-length lines are dropped */
        int32_t x = OP1_Q10(SE_X0 + c) + 0x200;
        se_ln(r, x, y0, x, y1, 0x400, col);
    }
    if (!e->drum) {                                                /* sampler loop points (words 1 / 2); blue / green while edited */
        int32_t ls = se_w2s(e->w[1]), le = se_w2s(e->w[2]), yb = OP1_Q10(SE_YMID + SE_YAMP + 2);
        if (le > ls) {
            int32_t xs = se_x(e, ls), xe = se_x(e, le), x0 = OP1_Q10(SE_X0), x1 = OP1_Q10(SE_X1);
            int32_t ca = xs < x0 ? x0 : xs, cb = xe > x1 ? x1 : xe;
            if (cb > ca) se_ln(r, ca, yb, cb, yb, 0x300, c_loop);   /* loop span along the bottom */
            for (int m = 0; m < 2; m++) {                          /* line + flag at the bottom: [ ... ] */
                int32_t x = m ? xe : xs;
                if (x < x0 || x > x1) continue;
                const uint8_t *lc = !e->loopsel ? c_loop : m ? c_green : c_blue;
                se_ln(r, x, OP1_Q10(SE_YMID - SE_YAMP), x, yb, e->loopsel ? 0x600 : 0x400, lc);
                se_ln(r, x, yb, x + (m ? -OP1_Q10(5) : OP1_Q10(5)), yb, 0x600, lc);
            }
        }
    }
    for (int m = 0; m < 2; m++) {                                  /* markers: line + flag at the top */
        int32_t x = se_x(e, m ? b : a);
        if (x < OP1_Q10(SE_X0) || x > OP1_Q10(SE_X1)) continue;
        const uint8_t *col = e->loopsel && !e->drum ? c_grey : m ? c_green : c_blue;
        se_ln(r, x, OP1_Q10(SE_YMID - SE_YAMP - 6), x, OP1_Q10(SE_YMID + SE_YAMP + 2), 0x600, col);
        int32_t f = m ? -OP1_Q10(5) : OP1_Q10(5);
        se_ln(r, x, OP1_Q10(SE_YMID - SE_YAMP - 6), x + f, OP1_Q10(SE_YMID - SE_YAMP - 6), 0x600, col);
    }
    if (e->mode == 1) {                                            /* fade: the gain curve over the selection */
        int32_t px = -1, py = 0;
        for (int c = 0; c <= SE_COLS; c += 2) {
            int32_t s = e->view0 + (int32_t)((int64_t)e->view_len * c / SE_COLS);
            if (s < a || s >= b) { px = -1; continue; }
            int32_t x = OP1_Q10(SE_X0 + c);
            int32_t y = OP1_Q10(SE_YMID + SE_YAMP) - (int32_t)(((int64_t)se_fade_gain(e, s, a, b) * OP1_Q10(2 * SE_YAMP)) >> 15);
            if (px >= 0 && (x != px || y != py)) se_ln(r, px, py, x, y, 0x600, c_blue);
            px = x; py = y;
        }
    }
    if (e->drum) {                                                 /* drum: every pad's start as a tick below */
        int keep = e->pad;
        for (int i = 0; i < 24; i++) {
            e->pad = (uint8_t)i;
            int32_t x = se_x(e, se_pad_get(e, 0));
            if (x < OP1_Q10(SE_X0) || x > OP1_Q10(SE_X1)) continue;
            se_ln(r, x, OP1_Q10(SE_YMID + SE_YAMP + 1), x, OP1_Q10(SE_YMID + SE_YAMP + 5), 0x400, i == keep ? c_white : c_grey);
        }
        e->pad = (uint8_t)keep;
    }
    if (e->mode == 2) {                                            /* auto-cut: the proposed cuts */
        for (int i = 0; i < e->ac_n; i++) {
            int32_t x = se_x(e, e->ac_cut[i][0]);
            if (x < OP1_Q10(SE_X0) || x > OP1_Q10(SE_X1)) continue;
            se_ln(r, x, OP1_Q10(SE_YMID - SE_YAMP - 2), x, OP1_Q10(SE_YMID + SE_YAMP + 2), 0x400, c_blue);
        }
    }
    if (e->snap == 2) {                                            /* transients: ticks above the waveform */
        if (e->on_dirty) se_transients(e);
        for (int i = 0; i < e->n_on; i++) {
            int32_t x = se_x(e, e->on[i]);
            if (x < OP1_Q10(SE_X0) || x > OP1_Q10(SE_X1)) continue;
            se_ln(r, x, OP1_Q10(SE_YMID - SE_YAMP - 4), x, OP1_Q10(SE_YMID - SE_YAMP + 1), 0x400, c_grey);
        }
    }
    /* overview bar (whole buffer, the view as a bracket) */
    int32_t oy = OP1_Q10(112), ox0 = OP1_Q10(SE_X0), ow = SE_COLS;
    se_ln(r, ox0, oy, OP1_Q10(SE_X1), oy, 0x400, c_grey);
    int32_t sa = ox0 + (int32_t)(((int64_t)a * ow << 10) / SE_N), sb = ox0 + (int32_t)(((int64_t)b * ow << 10) / SE_N);
    if (sb - sa < 0x400) sb = sa + 0x400;
    se_ln(r, sa, oy, sb, oy, 0x600, c_white);
    int32_t va = ox0 + (int32_t)(((int64_t)e->view0 * ow << 10) / SE_N);
    int32_t vb = ox0 + (int32_t)(((int64_t)(e->view0 + e->view_len) * ow << 10) / SE_N);
    se_ln(r, va, oy - OP1_Q10(4), va, oy + OP1_Q10(4), 0x400, c_grey);
    se_ln(r, vb, oy - OP1_Q10(4), vb, oy + OP1_Q10(4), 0x400, c_grey);
    /* texts in the stock font (firmware Text objects; lowercase = the font's capitals) */
    int32_t cs = e->mode == 2 ? 100000 + e->ac_n : (int32_t)((int64_t)(b - a) * 100 / 44100);
    if (cs >= 100000 && cs != e->shown_cs) {                      /* auto-cut: number of pads */
        char t[12]; int n = e->ac_n;
        t[0] = (char)('0' + n / 10); t[1] = (char)('0' + n % 10); t[2] = ' '; t[3] = 'p'; t[4] = 'a'; t[5] = 'd'; t[6] = 's'; t[7] = 0;
        op1_text_set(se_txt[SE_T_LEN], t[0] == '0' ? t + 1 : t);
        e->shown_cs = cs;
    }
    if (cs != e->shown_cs) {                                      /* x.xx seconds */
        char t[8];
        t[0] = (char)('0' + cs / 100 % 10); t[1] = '.'; t[2] = (char)('0' + cs / 10 % 10); t[3] = (char)('0' + cs % 10); t[4] = 0;
        op1_text_set(se_txt[SE_T_LEN], t);
        e->shown_cs = cs;
    }
    op1_text_draw(se_txt[SE_T_LEN], r, se_ident);
    if (e->snap == 1) op1_text_draw(se_txt[SE_T_ZC], r, se_ident);
    if (e->snap == 2) {                                            /* "tr N": sensitivity 1..7 (= 9 - rise ratio) */
        int s = 9 - (e->ac_sens ? e->ac_sens : 4);
        if (s != e->shown_sens) {
            char t[5] = { 't', 'r', ' ', (char)('0' + s), 0 };
            op1_text_set(se_txt[SE_T_TR], t);
            e->shown_sens = (int8_t)s;
        }
        op1_text_draw(se_txt[SE_T_TR], r, se_ident);
    }
    if (e->att) {                                                 /* -x.x db / +x.x db */
        if (e->att != e->shown_att) {
            char t[12]; int n = 0, db10 = (e->att < 0 ? -e->att : e->att) * 5;
            t[n++] = e->att < 0 ? '+' : '-'; if (db10 >= 100) t[n++] = (char)('0' + db10 / 100);
            t[n++] = (char)('0' + db10 / 10 % 10); t[n++] = '.'; t[n++] = (char)('0' + db10 % 10);
            t[n++] = ' '; t[n++] = 'd'; t[n++] = 'b'; t[n] = 0;
            op1_text_set(se_txt[SE_T_ATT], t);
            e->shown_att = e->att;
        }
        op1_text_draw(se_txt[SE_T_ATT], r, se_ident);
    }
    if (e->trimmed || (e->drum && e->ac_undo)) op1_text_draw(se_txt[SE_T_UNDO], r, se_ident);
    if (e->drum) {                                                /* pad number */
        if (e->pad != e->shown_pad) {
            char t[8]; int n = e->pad + 1;
            t[0] = 'p'; t[1] = 'a'; t[2] = 'd'; t[3] = ' '; t[4] = n >= 10 ? (char)('0' + n / 10) : (char)('0' + n);
            t[5] = n >= 10 ? (char)('0' + n % 10) : 0; t[6] = 0;
            op1_text_set(se_txt[SE_T_PAD], t);
            e->shown_pad = e->pad;
        }
        op1_text_draw(se_txt[SE_T_PAD], r, se_ident);
    }
    static const char *const shift_lbl[4] = { "dc", "fade", 0, 0 }, *const shift_lbl_d[4] = { "dc", "fade", "auto", 0 };
    static const char *const drum_lbl[4] = { "fit", "zero", "norm", "rev" }, *const ac_lbl[2] = { "hits", "equal" };
    static const char *const hits_lbl[7] = { "hits 1", "hits 2", "hits 3", "hits 4", "hits 5", "hits 6", "hits 7" };
    static const char *const fd_shape_lbl[3] = { "in", "out", "both" }, *const fd_curve_lbl[3] = { "linear", "power", "exp" };
    int shift = se_shift_held();
    for (int k = 0; k < 4; k++) {
        const char *l = e->mode == 1 ? (k == 0 ? "ok" : k == 1 ? fd_shape_lbl[e->fd_shape] : k == 2 ? fd_curve_lbl[e->fd_curve] : "cancel")
                      : e->mode == 2 ? (k == 0 ? "ok" : k == 1 ? (e->ac_method ? ac_lbl[1] : hits_lbl[8 - (e->ac_sens ? e->ac_sens : 4)])   /* hits N: sensitivity, blue */
                                       : k == 2 ? 0 : "cancel")
                      : shift ? (e->drum ? shift_lbl_d[k] : shift_lbl[k]) : e->drum ? drum_lbl[k] : se_label[k];
        if (!l) continue;
        if (l != se_shown_lbl[k]) { op1_text_set(se_txt[SE_T_KEY + k], l); se_shown_lbl[k] = l; }
        op1_text_draw(se_txt[SE_T_KEY + k], r, se_ident);
    }
}

/* GUI thread, once (screen init): the Text objects */
void se_texts_init(void)
{
    op1_text_ctor(se_txt[SE_T_LEN], OP1_Q10(SE_X0), 4, SE_TEXT_SIZE, c_white);
    op1_text_ctor(se_txt[SE_T_ZC], OP1_Q10(150), 4, SE_TEXT_SIZE, c_red);
    op1_text_set(se_txt[SE_T_ZC], "zc");
    op1_text_ctor(se_txt[SE_T_TR], OP1_Q10(150), 4, SE_TEXT_SIZE, c_red);
    op1_text_set(se_txt[SE_T_TR], "tr");
    op1_text_ctor(se_txt[SE_T_PAD], OP1_Q10(200), 4, SE_TEXT_SIZE, c_white);
    op1_text_ctor(se_txt[SE_T_ATT], OP1_Q10(64), 4, SE_TEXT_SIZE, c_red);
    op1_text_ctor(se_txt[SE_T_UNDO], OP1_Q10(254), 4, SE_TEXT_SIZE, c_red);
    op1_text_set(se_txt[SE_T_UNDO], "undo");
    for (int k = 0; k < 4; k++) {                                 /* text set when drawn (labels change) */
        op1_text_ctor(se_txt[SE_T_KEY + k], OP1_Q10(40 + 80 * k), SE_KEY_Y, SE_TEXT_SIZE, c_white);
        *(int32_t *)((uint8_t *)se_txt[SE_T_KEY + k] + 0x60) = 1;              /* centred on x (docs/23) */
        se_shown_lbl[k] = 0;
    }
}

static void se_open_common(sampedit *e, int16_t *data);
void se_open(sampedit *e, int16_t *data, int16_t *words)
{
    e->drum = 0; e->umax = 32767; se_n = SE_N_SYNTH;
    e->w7 = words[7]; e->w = words;
    se_open_common(e, data);
}
/* drum: the slot's live pad table (24 x 8 int16), the pad being edited */
void se_open_drum(sampedit *e, int16_t *data, int16_t *table, int pad)
{
    e->drum = 1; se_n = SE_N_DRUM; e->umax = SE_N_DRUM; e->w = 0;
    e->pt = table; e->pad = (uint8_t)(pad < 0 || pad > 23 ? 0 : pad);
    e->ac_undo = 0; if (!e->ac_sens) e->ac_sens = 4;
    if (!e->ac_parts) e->ac_parts = 24;
    se_open_common(e, data);
}
static void se_open_common(sampedit *e, int16_t *data)
{
    e->shown_cs = -1; e->shown_att = 0x7FFF; e->shown_pad = -1; e->shown_sens = -1; e->mode = 0; e->loopsel = 0;
    if (!e->ac_sens) e->ac_sens = 4;
    if (!e->fd_pct) e->fd_pct = 100;
    e->on_dirty = 1; e->n_on = 0;
    e->att = 0; e->att_amp = 65536;
    e->d = data;
    e->view0 = 0; e->view_len = SE_N; e->last = 0; e->trimmed = 0; e->cache_len = -1; e->cache_v0 = -1;
    for (int i = 0; i < 4; i++) e->acc[i] = 0;
}

#ifndef OP1_HOST
/* ---- firmware glue: synth / drum key + scissors on the sampler (screen 6) / drum (screen 0x10) page ---- */
#define SE_SET_KNOBS     ((void (*)(void *, const int16_t *))0x01A00D14)
#define SE_PATCH_S(s)    ((void *)(0x01DDD7E8 + ((s) & 7) * 0x38))
#define SE_PATCH_D(s)    ((void *)(0x01DDD9A8 + ((s) & 7) * 0x38))
#define SE_DIRTY_S       ((void (*)(int))0x019412AC)              /* bg saver: synth_<slot>.raw */
#define SE_DIRTY_D       ((void (*)(int))0x01941286)              /* bg saver: drum_<slot>.raw */
#define SE_DRUM_TO_PATCH ((void (*)(void *))0x019FFC50)           /* live pad table -> patch JSON (+ changed) */
#define SE_DRUM_PAD      (*(volatile int *)0xFF801044)            /* last played pad 0..23 */
static sampedit se;
static uint8_t *se_sb;
static int se_slot = -1;

int se_is_open(void) { return se_slot >= 0; }
int se_screen_id(void) { return se.drum ? 0x10 : 6; }

static void se_peaks(void)                                        /* as SampleBuffer::load_file does */
{
    int16_t *all = *(int16_t **)se_sb, *pk = (int16_t *)(se_sb + 4);
    int32_t cap = *(int32_t *)(se_sb + 0x1038);
    for (int i = 0; i < OP1_PEAK_BUCKETS; i++) pk[i] = 0;
    for (int32_t i = 0; i < cap; i++) if (all[i] > pk[i >> 8]) pk[i >> 8] = all[i];
}
static void se_markers(void)                                      /* persist the markers */
{
    se_loop_clamp(&se);                                           /* also after trim / cut / paste */
    if (se.drum) SE_DRUM_TO_PATCH(SE_PATCH_D(se_slot)); else SE_SET_KNOBS(SE_PATCH_S(se_slot), se.w);
}
static void se_store(int audio)                                   /* persist markers (+ audio) */
{
    se_markers();
    if (audio) { se_peaks(); if (se.drum) SE_DIRTY_D(se_slot); else SE_DIRTY_S(se_slot); }
}
static void se_follow_pad(void)                                   /* drum: edit the pad played last */
{
    int p = SE_DRUM_PAD;
    if (se.drum && se.mode == 0 && p >= 0 && p < 24 && p != se.pad) { se.pad = (uint8_t)p; se.last = 0; se_show_region(&se); }
}

/* key thread: synth / drum key held + scissors; returns 1 if consumed */
int se_open_close(int drum)
{
    if (se_slot >= 0) {                                           /* close: trim / attenuation become final */
        int was = se.trimmed;
        se_finalize(&se);
        was |= se_apply_att(&se);
        se_store(was);
        se_slot = -1;
        return 1;
    }
    int slot = *(volatile int *)(drum ? 0xFF8016D8 : 0xFF8016D4) & 7;
    int type = *(volatile int *)((drum ? 0xFF801704 : 0xFF8016E4) + slot * 4);
    if (type != (drum ? 8 : 6) || *(volatile int *)0x018B65C8 != (drum ? 0x10 : 6)) return 0;
    if (drum) {
        uint32_t *obj = ((uint32_t **)0x01CC10CC)[slot];         /* DrumSampler (0x01978DA8) */
        se_sb = obj ? (uint8_t *)obj[0] : 0;
        if (!se_sb) return 0;
        se_open_drum(&se, *(int16_t **)se_sb + OP1_SAMPLE_GUARD, (int16_t *)(0x01AAADB4 + slot * 0x180), SE_DRUM_PAD);
    } else {
        se_sb = (uint8_t *)op1_sample_buffer(slot);
        if (!se_sb) return 0;
        se_open(&se, *(int16_t **)se_sb + OP1_SAMPLE_GUARD, (int16_t *)op1_synth_knobs(slot));
    }
    se_slot = slot;
    return 1;
}
void se_key(int code)                                             /* pressed keys while open */
{
    int shift = *(volatile uint8_t *)0x01DD7F78;
    se_follow_pad();
    if (se.mode == 1) {                                           /* fade mode: T1 ok, T2 shape, T3 curve, T4 cancel */
        if (code == 0x06) { if (se_fade_apply(&se)) se_store(1); se.mode = 0; }
        else if (code == 0x07) se.fd_shape = (uint8_t)((se.fd_shape + 1) % 3);
        else if (code == 0x08) se.fd_curve = (uint8_t)((se.fd_curve + 1) % 3);
        else if (code == 0x09) se.mode = 0;
        return;
    }
    if (se.mode == 2) {                                           /* auto-cut: T1 ok, T2 hits / equal, T4 cancel */
        if (code == 0x06) { if (se_ac_apply(&se)) se_store(0); se.mode = 0; }
        else if (code == 0x07) { se.ac_method ^= 1; se_ac_compute(&se); }
        else if (code == 0x09) se.mode = 0;
        return;
    }
    if (code == 0x07 && shift) { se.mode = 1; return; }            /* Shift + T2: fade mode */
    if (code == 0x08 && shift && se.drum) { se.mode = 2; se_ac_compute(&se); return; }   /* Shift + T3: auto-cut */
    if (code == 0x06 && shift) { if (se_dc(&se)) se_store(1); }   /* Shift + T1: DC offset */
    else if (code == 0x06 && se.drum) { if (se_fit(&se)) se_store(0); }   /* T1 (drum): fit */
    else if (code == 0x06) { if (se_trim(&se)) se_store(1); }     /* T1: trim (+ de-click) */
    else if (code == 0x07) { if (se_zero(&se)) se_store(1); }     /* T2: zero */
    else if (code == 0x08) { if (se_normalize(&se)) se_store(1); } /* T3: normalize */
    else if (code == 0x09) { if (se_reverse_sel(&se)) se_store(1); } /* T4: reverse */
    else if (code == 0x24 && shift) {                             /* Shift + left: undo (trim / auto-cut) */
        if (se.drum ? se_ac_undo(&se) : se_undo(&se)) se_store(!se.drum);
    }
    else if (code == 0x0A) se_copy(&se);                          /* lift: copy the selection */
    else if (code == 0x0C) { if (se_cut(&se)) se_store(1); }      /* scissors: cut (drum: copy + zero) */
    else if (code == 0x0B) { if (se_paste(&se)) se_store(1); }    /* drop: paste (drum: overwrite) */
    else if (code == 0x3B && !se.drum) { se.loopsel ^= 1; se.last = 0; }   /* blue press: loop points <-> start / end */
    else if (code == 0x3E) se.snap = (uint8_t)((se.snap + 1) % 3); /* red press: zc -> transients -> off */
}
void se_knob(int knob, int delta)
{
    int shift = *(volatile uint8_t *)0x01DD7F78;
    se_follow_pad();
    int32_t *acc = &se.acc[knob & 3];
    if ((*acc > 0 && delta < 0) || (*acc < 0 && delta > 0)) *acc = 0;
    *acc += delta;
    int unit = knob < 2 ? 256 : 1024;                             /* markers: 4 steps per detent */
    int st = *acc / unit; if (!st) return;
    *acc -= st * unit;
    if (se.mode == 1) { if (knob == 0) se_fade_len(&se, st); return; }    /* fade mode: blue = fade length */
    if (se.mode == 2) {                                           /* auto-cut: blue = sensitivity (hits) / parts (equal) */
        if (knob == 0) { if (se.ac_method) se_ac_parts(&se, st); else se_ac_sens(&se, st); }
        return;
    }
    /* drum: marker moves change the live pad table at once (heard), the patch is written on close / edits */
    if (knob < 2) { se_move(&se, knob, st, shift); if (!se.drum) se_markers(); }
    else if (knob == 2 && shift) { se_shift_sel(&se, st); if (!se.drum) se_markers(); }
    else if (knob == 2) se_zoom(&se, st);
    else if (shift && se.snap == 2) se_tr_sens(&se, st);          /* red + Shift with tr: detection sensitivity */
    else if (shift) se_scroll(&se, st);                           /* red + Shift: scroll */
    else { se_attenuate(&se, -st); if (!se.drum) se_markers(); }  /* red: volume (right: less attenuation) */
}
/* play position: sampler voices in L1 (synth 6 x 0x10, vtable 0x019119F4, index = pos >> 12; drum 12 x 0x14,
 * vtable 0x0190FAC4, index = pos >> 11), +4 = the slot's sampler object (obj[0] = SampleBuffer), +8 = position.
 * A red line per voice of this buffer whose position moved since the last frame (stopped voices draw nothing). */
static void se_playheads(void *r)
{
    static int32_t prev[12];
    int n = se.drum ? 12 : 6, stride = se.drum ? 0x14 : 0x10, sh = se.drum ? 11 : 12;
    uint32_t vt = se.drum ? 0x0190FAC4 : 0x019119F4;
    for (int v = 0; v < n; v++) {
        volatile uint32_t *vo = (volatile uint32_t *)(0xFFB00000 + v * stride);
        int32_t pos = (int32_t)vo[2], moved = pos != prev[v];
        prev[v] = pos;
        uint32_t *obj = (uint32_t *)vo[1];
        if (vo[0] != vt || !obj || (uint8_t *)obj[0] != se_sb || !moved) continue;
        int32_t x = se_x(&se, pos >> sh);
        if (x < OP1_Q10(SE_X0) || x > OP1_Q10(SE_X1)) continue;
        se_ln(r, x, OP1_Q10(SE_YMID - SE_YAMP), x, OP1_Q10(SE_YMID + SE_YAMP), 0x400, c_red);
    }
}
static void se_init_screen(void *scr) { (void)scr; se_texts_init(); }
static void se_update(void *scr, int dt)
{
    op1_screen_draw(scr, dt);
    if (se_slot >= 0) { se_follow_pad(); void *r = op1_raster(); se_draw(&se, r); se_playheads(r); }
}
const op1_screen_t SCREEN_sampedit = { se_init_screen, se_update };
#endif
