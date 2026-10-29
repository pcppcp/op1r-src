/* "reverb" - fixed-point port of the Mutable Instruments Elements reverb (elements/dsp/fx/reverb.h):
 * Griesinger topology from the Dattorro paper, 4 input allpasses (the first one smeared by an LFO) into a loop
 * of 2 x (2 allpasses + low-pass + delay), long delay modulated by a slow LFO. Delay lengths scaled from
 * 32 kHz to 44.1 kHz so the room keeps its size. Mono: both loop outputs are summed.
 *   knob 0 (blue)  : time       decay, 0.35 .. 0.98 loop gain (Elements: 0.35 .. 0.95)
 *   knob 1 (green) : tone       loop low-pass 0.3 (dark) .. 0.95 (bright) (Elements: 0.7 .. 0.9)
 *   knob 2 (white) : diffusion  allpass gain 0.5 .. 0.75 (Elements: 0.55 .. 0.7)
 *   knob 3 (red)   : mix        dry .. wet          (green / red swapped in op1r 24)
 * Memory: a 32 Ki int16 line (64 KB, 16-bit like Elements) written twice (no index masks), 169 KB of this FX
 * position's scratch buffer.
 * CPU: the mirrored line, delay offsets as constants, knob scaling only when a knob moves and the LFO step in
 * 32 bits outside the sample loop. The samples are the same as the plain version's.
 * Original code (c) 2014 Emilie Gillet, MIT license. */
#include "op1_plugin.h"

#define RV_SIZE 32768
#define RV_MASK (RV_SIZE - 1)
#define RV_KNEE 24576
/* ap1..ap4, dap1a, dap1b, del1, dap2a, dap2b, del2 (Elements lengths * 44100 / 32000):
 * 207, 295, 440, 726, 3007, 3707, 6203, 3480, 3028, 8698. Each line takes length + 1 words from its base B_i;
 * T_i = B_i + length - 1 is its tail. Total 29801 words. */
enum { B0 = 0, B1 = 208, B2 = 504, B3 = 945, B4 = 1672, B5 = 4680, B6 = 8388, B7 = 14592, B8 = 18073, B9 = 21102,
       T0 = 206, T1 = 502, T2 = 943, T3 = 1670, T4 = 4678, T5 = 8386, T6 = 14590, T7 = 18071, T8 = 21100 };
/* The line is written twice, at q[o] and q[o + RV_SIZE] (q = buf + wp), so a read is q[o] with no index mask:
 * a read at q[o] lands on the copy written under the same logical index (wp + o) mod RV_SIZE. Reads stay below
 * 2 * RV_SIZE, writes below RV_SIZE + RV_SIZE + B9 + 1 words (173276 bytes of the 176460-byte scratch). */
#define RV_WORDS (2 * RV_SIZE + B9 + 1)
_Static_assert((RV_WORDS + 1) / 2 * 4 <= OP1_FX_SCRATCH_BYTES, "reverb line past the FX scratch");

typedef struct {
    int16_t *buf, *mirror;                                /* mirror = buf + RV_SIZE */
    int32_t wp;
    int32_t lp1, lp2;
    int32_t lfo_iir[2], lfo_y0[2], lfo_y1[2], lfo[2];    /* stmlib CosineOscillator (approximate), Q29; lfo Q15 0..1 */
    int32_t k0, f0, k9, f9;                               /* the two LFO taps' whole and fractional offsets */
    int16_t raw[4];                                       /* knob words the coefficients below were made from */
    int32_t age;                                          /* blocks since then (redone every 64: the range table) */
    int32_t krt, amount, kap, klp;
} reverb_t;

static reverb_t rv[3];                                    /* synth, drum, master */

static inline int32_t c17(int32_t x) { x = x > 65535 ? 65535 : x; return x < -65535 ? -65535 : x; }
/* x / 32768 truncated towards zero: >> alone biases the loop into a DC limit cycle and rounding into a
 * low-level one, magnitude truncation lets the tail die out */
static inline int32_t t15(int32_t x) { return (x + ((x >> 31) & 32767)) >> 15; }
/* |b| <= 32767: fits 32 bit */
static inline int32_t m15(int32_t a, int32_t b) { return t15(c17(a) * b); }
static inline int16_t sat(int32_t x) { x = x > 32767 ? 32767 : x; return (int16_t)(x < -32768 ? -32768 : x); }   /* min, max */

/* one LFO step (FxEngine: every 32 samples): y0' = (iir * y0 >> 29) - y1 in 32 bits. iir = 2^30 - d with
 * d = 2262 / 815, so (iir * y) >> 29 = 2y + floor(-d y / 2^29), the product split at bit 15 (exact) */
static void lfo_step(reverb_t *r)
{
    for (int k = 0; k < 2; k++) {
        int32_t y = r->lfo_y0[k], z = -y, d = (1 << 30) - r->lfo_iir[k];
        int32_t fl = (d * (z >> 15) + ((d * (z & 32767)) >> 15)) >> 14;
        r->lfo_y0[k] = 2 * y + fl - r->lfo_y1[k];
        r->lfo_y1[k] = y;
        r->lfo[k] = (y + (1 << 28)) >> 14;
    }
    /* Interpolate(line, offset + amp * lfo) in Q15: smear ap1 at 14 + 110 lfo1, del2 at 8550 + 138 lfo2 */
    int32_t a = 110 * r->lfo[0], b = 138 * r->lfo[1];
    r->k0 = B0 + 14 + (a >> 15); r->f0 = a & 32767;
    r->k9 = B9 + 8550 + (b >> 15); r->f9 = b & 32767;
}

static void reverb_init(int pos)
{
    reverb_t *r = &rv[pos % 3];
    r->buf = (int16_t *)op1_fx_scratch(pos);
    if (!r->buf) return;
    uint32_t *w = (uint32_t *)r->buf;                    /* init runs in the audio callback: 32-bit stores through a */
    /* local pointer (r->buf was reloaded for every halfword). Only the 2 * RV_SIZE words reads can reach: the
     * mirror's overhang above them is written before it is ever read */
    for (int i = 0; i < RV_SIZE; i++) w[i] = 0;
    r->mirror = r->buf + RV_SIZE;
    r->wp = 0; r->lp1 = r->lp2 = 0;
    for (int k = 0; k < 2; k++) {                          /* 0.5 Hz, 0.3 Hz, as clouds_dsp.c */
        int64_t f = ((int64_t)(k ? 3 : 5) * 32 << 29) / (10LL * 44100);
        int64_t fr = (1LL << 27) - f;
        r->lfo_iir[k] = (int32_t)((16 * fr * ((1LL << 29) - 2 * fr)) >> 29);
        r->lfo_y1[k] = r->lfo_iir[k] / 4;
        r->lfo_y0[k] = 1 << 28;
        r->lfo[k] = (r->lfo_y1[k] + (1 << 28)) >> 14;
    }
    int32_t a = 110 * r->lfo[0], b = 138 * r->lfo[1];
    r->k0 = B0 + 14 + (a >> 15); r->f0 = a & 32767;
    r->k9 = B9 + 8550 + (b >> 15); r->f9 = b & 32767;
    r->age = 64;
}

#define WR(o, v) do { const int16_t v_ = (v); q[o] = v_; qn[o] = v_; } while (0)
/* allpass stage: Read(TAIL, g) then WriteAllPass(line, -g); the tail is a buffer word (no clamp needed) */
#define ALLPASS(B, T, g) do { const int32_t prev_ = q[T]; acc += t15(prev_ * (g)); WR(B, sat(acc)); \
                              acc = m15(acc, -(g)) + prev_; } while (0)

/* n samples with the LFO taps fixed (they change every 32 samples); returns the peak */
static int32_t rv_run(reverb_t *r, int16_t *buf, int n, int32_t peak)
{
    int16_t *const line = r->buf, *const mirror = r->mirror;  /* two bases: the copy's offsets stay immediates */
    int32_t wp = r->wp, lp1 = r->lp1, lp2 = r->lp2;
    const int32_t krt = r->krt, amount = r->amount, kap = r->kap, klp = r->klp;
    const int32_t k0 = r->k0, f0 = r->f0, k9 = r->k9, f9 = r->f9;
    for (int t = 0; t < n; t++) {
        wp = (wp - 1) & RV_MASK;
        int16_t *const q = line + wp, *const qn = mirror + wp;
        int32_t in = buf[t], acc, apout, wet, a, b;
        /* smear AP1 inside the loop */
        a = q[k0]; b = q[k0 + 1];
        WR(B0 + 138, (int16_t)(a + t15((b - a) * f0)));
        acc = t15(in * 6554);                         /* input 0.2 as Elements: headroom in the 16-bit lines (0.5
                                                       * clipped them with loud input + long time: crackling) */
        ALLPASS(B0, T0, kap);
        ALLPASS(B1, T1, kap);
        ALLPASS(B2, T2, kap);
        ALLPASS(B3, T3, kap);
        apout = acc;
        /* loop half 1 */
        a = q[k9]; b = q[k9 + 1];
        acc = apout + t15((a + t15((b - a) * f9)) * krt);
        lp1 += m15(acc - lp1, klp); acc = lp1;
        ALLPASS(B4, T4, -kap);
        ALLPASS(B5, T5, kap);
        WR(B6, sat(acc));
        wet = acc;                                                     /* Write(del1, 2.0): *2, halved below */
        /* loop half 2 */
        acc = apout + t15(q[T6] * krt);
        lp2 += m15(acc - lp2, klp); acc = lp2;
        ALLPASS(B7, T7, kap);
        ALLPASS(B8, T8, -kap);
        WR(B9, sat(acc));
        wet = (wet + acc) * 5;                                         /* mono: 2 L + 2 R, makeup for the 0.2 input */
        int32_t o = in + t15(in * -amount) + (((wet >> 3) * amount) >> 12);   /* dry .. wet (wet can exceed 16 bit) */
        int32_t ao = o < 0 ? -o : o;                                   /* soft knee above -2.5 dBFS, not a hard clip */
        if (ao > RV_KNEE) {
            int32_t d = ao - RV_KNEE;
            ao = d < 16384 ? RV_KNEE + (d >> 2) : 28672 + ((d - 16384) >> 4);
            o = sat(o < 0 ? -ao : ao);
        }
        buf[t] = (int16_t)o;
        if (o < 0) o = -o;
        if (o > peak) peak = o;
    }
    r->wp = wp; r->lp1 = lp1; r->lp2 = lp2;
    return peak;
}

static void reverb_render(int pos, int16_t *buf, const int16_t *p)
{
    reverb_t *r = &rv[pos % 3];
    if (!r->buf) return;
    if (++r->age >= 64 || p[0] != r->raw[0] || p[1] != r->raw[1] || p[2] != r->raw[2] || p[3] != r->raw[3]) {
        for (int i = 0; i < 4; i++) r->raw[i] = p[i];
        r->age = 0;
        r->krt = 11469 + (op1_fx_param_q15(p, 0) * 20644 >> 15);      /* 0.35 .. 0.98 */
        r->amount = op1_fx_param_q15(p, 3);
        r->kap = 16384 + (op1_fx_param_q15(p, 2) * 8192 >> 15);       /* 0.5 .. 0.75 */
        r->klp = 9830 + (op1_fx_param_q15(p, 1) * 21299 >> 15);       /* 0.3 .. 0.95 */
    }
    int32_t peak = 0;
    for (int t = 0; t < OP1_FX_BLOCK; ) {
        int n = (r->wp - 1) & 31;                     /* samples before the next one whose wp is a multiple of 32 */
        if (n == 0) { lfo_step(r); n = 32; }
        if (n > OP1_FX_BLOCK - t) n = OP1_FX_BLOCK - t;
        peak = rv_run(r, buf + t, n, peak);
        t += n;
    }
    volatile int16_t *m = op1_fx_meter(pos);
    int32_t mv = (*m * 0xfe00) >> 16;
    *m = (int16_t)(peak > mv ? peak : mv);
}

const op1_fx_t FX_reverb = { reverb_init, reverb_render };

/* ======================= screen (GUI thread) =======================
 * display/kalireverb.svg (Kali artwork, display/gen_kali_svg.py from display/src/kali-reverb.svg). Knobs as the
 * stock screens see them (op1_fx_knobs_live):
 *   blue  (time)      1..6 arms visible (karm1..6: lower pair first, then middle, then upper) + TIME needle
 *   green (tone)      fire ring and flames fade in (below 10 % hidden, then 30 .. 100 %) + TONE needle (green)
 *   white (diffusion) brightness of the skull necklace (30 .. 100 %) + DIFFUSION needle
 *   red   (mix)       lights 0..10 lines of the right-hand scale from the bottom (kmix9 = bottom) in red
 * Only group visibility (node +0x50), the skull / fire style colours and needle rotations change; no raster lines. */
#include "kali_geom.h"
static void *k_arm[6], *k_skull, *k_fire, *k_mix[KALI_MIX_LINES], *k_needle[3];
static uint8_t *k_skc[KALI_NSKULL], *k_fic[KALI_NFIRE];               /* their style colours (RGBA) */

static void kali_screen_init(void *scr)
{
    char id[8] = { 'k', 0 };
    for (int i = 0; i < 6; i++) { id[1] = 'a'; id[2] = 'r'; id[3] = 'm'; id[4] = (char)('1' + i); id[5] = 0; k_arm[i] = op1_find_node(scr, id); }
    k_skull = op1_find_node(scr, "kskull"); k_fire = op1_find_node(scr, "kfire");
    for (int i = 0; i < KALI_NSKULL; i++) {                            /* ks0..ks13 */
        char a[5] = { 'k', 's', (char)(i < 10 ? '0' + i : '1'), (char)(i < 10 ? 0 : '0' + i - 10), 0 };
        k_skc[i] = kali_skull_kind[i] == 'f' ? op1_node_fill_rgba(scr, a) : op1_node_line_rgba(scr, a);
    }
    for (int i = 0; i < KALI_NFIRE; i++) {
        char a[4] = { 'k', 'f', (char)('0' + i), 0 };
        k_fic[i] = kali_fire_kind[i] == 'f' ? op1_node_fill_rgba(scr, a) : op1_node_line_rgba(scr, a);
    }
    for (int i = 0; i < KALI_MIX_LINES; i++) { char m[6] = { 'k', 'm', 'i', 'x', (char)('0' + i), 0 }; k_mix[i] = op1_find_node(scr, m); }
    k_needle[0] = op1_find_node(scr, "ktime"); k_needle[1] = op1_find_node(scr, "kdiff"); k_needle[2] = op1_find_node(scr, "ktone");
}

static void tint(uint8_t *const *c, int n, const uint8_t *rgb, int32_t l)   /* l 0..256 */
{
    for (int i = 0; i < n; i++) if (c[i]) { c[i][0] = (uint8_t)(rgb[0] * l >> 8); c[i][1] = (uint8_t)(rgb[1] * l >> 8); c[i][2] = (uint8_t)(rgb[2] * l >> 8); }
}

static void kali_screen_update(void *scr, int dt)
{
    int16_t k[4];
    op1_fx_knobs_live(k);
    int arms = 1 + (k[0] * 5 + 16383) / 32767;                         /* 1..6 */
    for (int i = 0; i < 6; i++) if (k_arm[i]) op1_node_set_visible(k_arm[i], i < arms);
    int lit = (k[3] * KALI_MIX_LINES + 16383) / 32767;                 /* 0..10 lines, from the bottom */
    for (int i = 0; i < KALI_MIX_LINES; i++) if (k_mix[i]) op1_node_set_visible(k_mix[i], i >= KALI_MIX_LINES - lit);
    /* skulls 30 .. 100 % by the white knob, fire hidden at 0 then 30 .. 100 % by the green knob (one copy each,
     * recoloured; was 5 copies at fixed levels) */
    static const uint8_t white[3] = { 0xFF, 0xFF, 0xFF }, red[3] = { 0xFF, 0x3A, 0x5D };
    tint(k_skc, KALI_NSKULL, white, 77 + (k[2] * 179 >> 15));
    int fire_on = k[1] > 3276;                                         /* below 10 %: no fire (as before: level 0) */
    if (k_fire) op1_node_set_visible(k_fire, fire_on);
    if (fire_on) tint(k_fic, KALI_NFIRE, red, 77 + ((k[1] - 3276) * 199 >> 15));
    (void)k_skull;
    static const int32_t px[3] = { KALI_TIME_X, KALI_DIFF_X, KALI_TONE_X }, py[3] = { KALI_TIME_Y, KALI_DIFF_Y, KALI_TONE_Y };
    static const uint8_t kn[3] = { 0, 2, 1 };                          /* time blue, diffusion white, tone green */
    for (int i = 0; i < 3; i++) if (k_needle[i]) {                     /* needle points left at 0, right at max */
        int32_t a = k[kn[i]] * 4096 >> 15;                             /* 0..180 deg (8192 = full turn) */
        op1_node_rotate(k_needle[i], px[i], py[i], OP1_SINE_TABLE[a & 8191], OP1_SINE_TABLE[(a + 2048) & 8191]);
    }
    op1_screen_draw(scr, dt);
}

const op1_screen_t SCREEN_reverb = { kali_screen_init, kali_screen_update };
