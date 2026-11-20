/* windwood: kennel copies of reedophone.c's setup(), set_delays(), warp() and rv_render(), bit for bit the same
 * results without a 64-bit divide (kennel addition; included by ww_rate.c after reedophone.c, whose own versions stay
 * unchanged under the names *_up and are the reference of harness/windwood_exact_test.c).
 *
 * The Blackfin has no divide instruction: each 64-bit `/` was a libgcc __divdi3 / __udivdi3 call of about a thousand
 * cycles, eight of them in every setup() (run on every knob or pitch move), three in set_delays(), eight in a warp().
 * Here a quotient whose operands fit 32 bits is a 32-bit `/` (same truncation), and the others are exact long
 * divisions (ww_shdiv). Products that fit 32 bits are 32-bit; wider ones are split exactly. setup() is cut by its
 * inputs: the values that depend on the pitch alone, and those that also depend on the bore knob, are kept with the
 * inputs they were made from (ww_pc) and reused while those inputs are the same (a knob move at a held pitch).
 * Ranges: pitch increments any (setup clamps them), knob words 0..32767 (the unit's), envelope words int16.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */

/* floor((r 2^32 + lo) / 2^(32 - n) / d) mod 2^32 for r < d: restoring division shifting in the top n bits of lo,
 * one quotient bit a step (d may use all 32 bits) */
static uint32_t ww_divlo(uint32_t r, uint32_t lo, int n, uint32_t d)
{
    uint32_t q = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = r >> 31;
        r = (r << 1) | (lo >> 31); lo <<= 1; q <<= 1;
        if (c || r >= d) { r -= d; q |= 1; }
    }
    return q;
}
#define ww_shdiv(r, n, d) ww_divlo((r), 0, (n), (d))     /* floor(r 2^n / d) mod 2^32, r < d */
/* floor((hi 2^32 + lo) / d), the whole 64-bit quotient */
static uint64_t ww_udiv64(uint32_t hi, uint32_t lo, uint32_t d)
{
    return ((uint64_t)(hi / d) << 32) | ww_divlo(hi % d, lo, 32, d);
}
/* (a b) >> s for a < 2^32, b < 2^16, s <= 16, when the result fits 32 bits: a split at bit s */
static inline uint32_t ww_mulsh(uint32_t a, uint32_t b, int s)
{
    return (a >> s) * b + (((a & ((1u << s) - 1)) * b) >> s);
}

static int32_t ww_blue_mk(int32_t k)             /* blue_mk with a 32-bit divide (the product fits 32 bits) */
{
    int i = 0; while (i < 6 && k > BLUE_K[i + 1]) i++;
    return BLUE_MK[i] + ((BLUE_MK[i + 1] - BLUE_MK[i]) * (k - BLUE_K[i])) / (BLUE_K[i + 1] - BLUE_K[i]);
}
static int32_t ww_atan_q16(int32_t y, int32_t x) /* atan_q16 with a 32-bit divide (x <= 65535, y < x or x < y < 2^15) */
{
    int inv = y > x; int32_t t = inv ? (int32_t)(((uint32_t)x << 15) / (uint32_t)y) : (int32_t)(((uint32_t)y << 15) / (uint32_t)x);
    uint32_t i = (uint32_t)t >> 10, f = (uint32_t)t & 1023;
    if (i >= 32) { i = 31; f = 1024; }
    int32_t a = ATT[i] + (int32_t)(((ATT[i + 1] - ATT[i]) * f) >> 10);
    return inv ? 102944 - a : a;
}

/* setup()'s values that depend on the pitch alone (p_inc) and on the pitch and the bore knob (b_inc, b_k2) */
typedef struct {
    uint32_t p_inc, P, f0_q8, hinc;
    int32_t midi_q8, oct_q15, sn, cs, gq15, zeta;
    uint32_t b_inc; int32_t b_k2;
    int32_t lpc, tau, gth;
} ww_pc_t;
static ww_pc_t ww_pc[OP1_VOICES];

static void ww_setup(rv_voice_t *v, rv_state_t *s, ww_pc_t *c0, uint32_t inc, const int16_t *k)
{
    if (inc < 400000u) inc = 400000u;
    if (c0->p_inc != inc) {                      /* the pitch alone (p_inc 0 never matches: inc >= 400000) */
        int32_t l2 = log2_q16(inc);
        int32_t midi_q8 = (69 << 8) + (((l2 - L2_A4) * 12) >> 8);                 /* |l2 - L2_A4| < 2^21 */
        c0->midi_q8 = midi_q8;
        c0->oct_q15 = ((midi_q8 - (70 << 8)) * 32768) / (12 << 8);                 /* |midi_q8 - 70 << 8| < 2^15 */
        c0->P = ww_shdiv(1, 48, inc);                                              /* 2^48 / inc */
        c0->f0_q8 = ((inc >> 16) * 44100u + (((inc & 0xffff) * 44100u) >> 16)) >> 8;   /* inc 44100 >> 24 */
        c0->sn = sin_q15(inc); c0->cs = sin_q15(inc + 0x40000000u);
        int32_t pw = (int32_t)exp2_q15((((midi_q8 - (60 << 8)) * LOSS_K_Q12) / 12) >> 4);
        int32_t g = 32768 - (int32_t)(((uint32_t)LOSS_Q15 * (uint32_t)pw) >> 15);
        if (g > 32767) g = 32767;
        if (g < 16384) g = 16384;
        c0->gq15 = g;
        c0->zeta = clampi(ZETA_Q15 + ((ZSLOPE * c0->oct_q15) >> 15), 600, 32767);
        uint32_t h = (inc >> 16) * 46341u + (((inc & 0xffff) * 46341u) >> 16);   /* inc 46341 >> 16 */
        while (h > 16556555u) h >>= 1;
        while (h < 8278277u) h <<= 1;
        c0->hinc = h;
        c0->p_inc = inc; c0->b_inc = 0;
    }
    if (c0->b_inc != inc || c0->b_k2 != k[2]) {  /* the pitch and the bore knob */
        uint32_t fc_q8 = (uint32_t)FC_HZ << 8, fl = ww_mulsh(c0->f0_q8, FCK_Q8, 8);
        if (fl > fc_q8) fc_q8 = fl;
        fc_q8 = ww_mulsh(fc_q8, exp2_q15(((int32_t)k[2] - 16384) * 4), 15);   /* exp2 <= 65534 */
        if (fc_q8 > (16000u << 8)) fc_q8 = 16000u << 8;
        int32_t xw = (int32_t)ww_mulsh(fc_q8, 3449u, 16);
        int32_t a = (int32_t)exp2_q15(-xw);
        int32_t lpc = 32768 - a; if (lpc > 32767) lpc = 32767;
        c0->lpc = lpc;
        int32_t re = 32768 - ((a * c0->cs) >> 15), im = (a * c0->sn) >> 15;
        if (im < 0) im = 0;
        int32_t th = ww_atan_q16(im, re);
        uint32_t t1 = ((uint32_t)th * 10430u) >> 16, P = c0->P;                    /* th <= 102944 */
        c0->tau = (int32_t)(t1 * (P >> 16) + ((t1 * (P & 0xffff)) >> 16));        /* t1 P >> 16 */
        uint32_t A = (uint32_t)re * (uint32_t)re, B = (uint32_t)im * (uint32_t)im;   /* re <= 65535 */
        uint32_t den = (A >> 15) + (B >> 15) + (((A & 0x7fff) + (B & 0x7fff)) >> 15);
        if (!den) den = 1;
        uint32_t na = (uint32_t)(32768 - a), sq2 = na * na;                         /* (32768 - a)^2 << 15 / den */
        uint32_t q = ((sq2 / den) << 15) | ww_shdiv(sq2 % den, 15, den);
        int32_t mag = (int32_t)isqrt_fast(q | 4);
        if (mag > 32767) mag = 32767;
        int32_t R = (c0->gq15 * mag) >> 15;
        if (R > 32700) R = 32700;
        int32_t cc = (int32_t)ww_shdiv((uint32_t)(32768 - R), 27, (uint32_t)(c0->zeta * (32768 + R)));   /* < 2^31 */
        if (cc > 32767) cc = 32767;
        int32_t sq = (cc + (int32_t)isqrt_fast((uint32_t)(cc * cc) + (3u << 24) + 4)) / 3;
        c0->gth = (sq * sq) >> 11;
        c0->b_inc = inc; c0->b_k2 = k[2];
    }
    s->lpc = c0->lpc; s->gq15 = c0->gq15; s->zeta = c0->zeta; s->gth = c0->gth;
    s->P = c0->P; s->tau = c0->tau; s->midi_q8 = c0->midi_q8; s->corr = 0x40000000;   /* forces set_delays */
    /* knobs */
    s->dyn_scale = ((int32_t)k[0] * 32768) / 26214;
    s->noise = (NOISE_Q15 * (int32_t)k[3]) >> 13;
    {
        int32_t ah = k[1] < 16384 ? (16384 - (int32_t)k[1]) * 2 : 0, af = k[1] > 16384 ? ((int32_t)k[1] - 16384) * 2 : 0;
        ah = (ah * ah) >> 15; af = (af * af) >> 15;
        int32_t sq = (int32_t)isqrt_fast(((uint32_t)ah << 15) | 4);
        int32_t a3 = (((ah * ah) >> 15) * ah) >> 15, f3 = (((af * af) >> 15) * af) >> 15;
        s->hum_d = (((ah * sq) >> 15) * HUM_DMAX_Q14) >> 15;                      /* ah, sq <= 2^15 */
        s->hjit = ah ? 164 + ((HUM_JMAX_Q15 * a3) >> 15) : 0;
        s->flt_d = (af * FLT_DMAX_Q14) >> 15;
        s->fjit = af ? 983 + ((FLT_JMAX_Q15 * f3) >> 15) : 0;
        s->finc = 2142397u + (uint32_t)af * 29u + (((uint32_t)af * 23638u) >> 15);   /* + 973910 af >> 15 */
        int32_t fs = (int32_t)isqrt_fast(((uint32_t)af << 15) | 4);
        int32_t b = (af * fs) >> 15;                                                  /* (a3 115400 + b 37000) >> 15 */
        int32_t mk = 3 * a3 + b + ((17096 * a3 + 4232 * b) >> 15);
        mk += ww_blue_mk(k[0]);
        uint32_t og = ((uint32_t)OUT_GAIN * exp2_q15(mk)) >> 15;                      /* exp2 < 2^20 here */
        s->outg = og > 32767 ? 32767 : (int32_t)og;
        s->hinc = c0->hinc;
    }
    v->inc = inc; for (int i = 0; i < 4; i++) v->kn[i] = k[i];
}

static void ww_set_delays(rv_state_t *s, int32_t corr)
{
    s->corr = corr;
    uint32_t P = ww_mulsh(s->P, exp2_q15((corr * 3495) >> 10), 15);              /* s->P < 2^30, exp2 < 2^16 */
    int32_t rt = (int32_t)((P >> 1) - (uint32_t)s->tau);
    if (rt < (6 << 16)) rt = 6 << 16;
    s->rt = rt;
    s->lbb = ((rt - 65536) >> 1) >> 16;
    int32_t lf = rt - 65536 - (s->lbb << 16);
    s->lbf_i = lf >> 16; int32_t frac = lf & 0xffff;
    if (s->lbb > LN - 4) s->lbb = LN - 4;
    if (s->lbf_i < 1) { s->lbf_i = 1; frac = 0; }
    if (s->lbf_i > LN - 4) s->lbf_i = LN - 4;
    s->age[0] = (s->lbf_i << 16) + frac - 65536; s->age[1] = s->lbb << 16;
    /* the Lagrange coefficients: every product fits 32 bits (f < 2^15), the one that reaches 2^31 is unsigned */
    int32_t f = frac >> 1, fm1 = f - 32768, fm2 = f - 65536, fp1 = f + 32768;
    int32_t t01 = (f * fm1) >> 15;
    s->lc[0] = (-((t01 * fm2) >> 15) / 6) >> 1;
    s->lc[1] = (int32_t)(((uint32_t)(-((fp1 * fm1) >> 15)) * (uint32_t)(-fm2)) >> 15) / 2 >> 1;
    s->lc[2] = (-(((fp1 * f >> 15) * fm2) >> 15) / 2) >> 1;
    s->lc[3] = (((fp1 * t01) >> 15) / 6) >> 1;
}

/* warp_set() on one line's column q (q[2 j] = sample j) */
#define WW_WSET(a, src_) do {                                                            \
        uint32_t sv_ = (src_); int32_t i_ = (int32_t)(sv_ >> 16), f_ = (int32_t)((sv_ & 0xffff) >> 1); \
        if (i_ > LN - 2) { i_ = LN - 2; f_ = 0; }                                       \
        int32_t p0_ = q[2 * ((w - i_) & LM)], p1_ = q[2 * ((w - i_ - 1) & LM)];          \
        q[2 * ((w - (a)) & LM)] = (int16_t)(p0_ + (((p1_ - p0_) * f_) >> 15));           \
    } while (0)
/* warp() with exact 32-bit long divisions instead of its 64-bit ones and running sums for the read positions */
static void ww_warp(rv_state_t *s, const int32_t *a0)
{
    const int32_t w = (int32_t)s->w;
    for (int c = 0; c < 2; c++) {
        int32_t a1 = s->age[c];
        if (a1 < 65536 || a0[c] < 65536 || a1 == a0[c]) continue;
        const uint32_t ua0 = (uint32_t)a0[c], ua1 = (uint32_t)a1;
        uint32_t r = (uint32_t)ww_udiv64(ua0 >> 16, ua0 << 16, ua1);              /* a0 << 16 / a1 (< 2^26) */
        if (r > (16u << 16)) r = 16u << 16;
        int32_t amax = (a1 >> 16) + 3;
        int16_t *const q = &s->q[0][c];
        if (r < 65536 || r > (8u << 16)) {
            if (r > 65536) { uint32_t src = 0; for (int a = 0; a <= amax; a++, src += r) WW_WSET(a, src); }
            else { uint32_t src = (uint32_t)amax * r; for (int a = amax; a >= 0; a--, src -= r) WW_WSET(a, src); }
            continue;
        }
        int32_t K = (int32_t)((r - 65536) * 8 / 7), rate = 65536 + K;
        int32_t b = (a1 >> 2) * 3, ab = (b >> 16) + 1, e = (ab << 16) - b;          /* 0 < e <= 65536 */
        /* T = K (2 e^2 / a1), D = (K (2e + 65536) << 17) / a1, DD = (K << 34) / a1: the 64-bit numerators
         * long-divided (all positive: K >= 0) */
        uint32_t ee = (uint32_t)e * (uint32_t)e;                                 /* wraps to 0 at e = 65536 */
        uint32_t nh = e == 65536 ? 2u : ee >> 31, nl = ee << 1;                  /* 2 e^2 = nh 2^32 + nl */
        int64_t T = (int64_t)K * (int32_t)ww_udiv64(nh, nl, ua1);
        uint64_t n = (uint64_t)(uint32_t)K * (uint32_t)(2 * e + 65536);           /* < 2^37 */
        int64_t D = (int64_t)ww_udiv64((uint32_t)(n >> 15), (uint32_t)n << 17, ua1);
        int64_t DD = (int64_t)ww_udiv64((uint32_t)K << 2, 0, ua1);
        /* T, D, DD (>= 0) kept as their high part >> 16 (mod 2^32, as the int32 cast of T >> 16) and low 16 bits */
        uint32_t Th = (uint32_t)(T >> 16), Tl = (uint32_t)T & 0xffff, Dh = (uint32_t)(D >> 16), Dl = (uint32_t)D & 0xffff;
        const uint32_t DDh = (uint32_t)(DD >> 16), DDl = (uint32_t)DD & 0xffff;
        uint32_t src = 0;
        int a = 0;
        for (; a <= amax && a < ab; a++, src += (uint32_t)rate) WW_WSET(a, src);
        for (; a <= amax; a++, src += (uint32_t)rate) {
            WW_WSET(a, src - Th);
            Tl += Dl; Th += Dh + (Tl >> 16); Tl &= 0xffff;
            Dl += DDl; Dh += DDh + (Dl >> 16); Dl &= 0xffff;
        }
    }
}
/* rv_render() calling the copies above; its own divides are 32-bit (env << 15 < 2^31 in magnitude, the divisors
 * positive), its 64-bit products split or kept where a 32-bit one could overflow */
static void ww_rv_render(void *self, int16_t *out, const int16_t *k)
{
    rv_voice_t *v = (rv_voice_t *)self;
    int vi = op1_voice_index(self); if (vi < 0 || vi >= OP1_VOICES) vi = 0;
    rv_state_t *s = &st[vi];
    rv_scr_tick[vi]++;
    if (v->sig != SIG) { rv_reset(s); v->sig = SIG; v->gate = 1; v->fresh = 1; v->tongued = 1; v->inc = 0; }
    uint32_t inc = (uint32_t)OP1_PITCH_INC[0];
    int32_t rt0 = s->rt, a0[2] = { s->age[0], s->age[1] };
    if (inc != v->inc || k[0] != v->kn[0] || k[1] != v->kn[1] || k[2] != v->kn[2] || k[3] != v->kn[3]) ww_setup(v, s, &ww_pc[vi], inc, k);
    rv_scr_note[vi] = (uint8_t)clampi((s->midi_q8 + 128) >> 8, 0, 127);

    int32_t env = OP1_VOICE_ENV[OP1_BLOCK - 1] * 2; if (env > 32767) env = 32767;
    rv_scr_att[vi] = (int16_t)clampi((env * 32768) / op1_voice_vel_gain(self), 0, 32767);
    int32_t dyn = (int32_t)(((int64_t)env * s->dyn_scale) >> 15); if (dyn > 40959) dyn = 40959;
    if (v->fresh) {
        v->fresh = 0;
        s->tongue = v->tongued ? TONGUE_SMP : NO_TONGUE;
    }
    {
        int32_t mq = s->midi_q8 - (TUNE_M0 << 8); mq = clampi(mq, 0, ((TUNE_NM - 1) << 8) - 1);
        int32_t dq = dyn >> 3; dq = clampi(dq, 0, ((TUNE_ND - 1) << 10) - 1);
        int32_t mi = mq >> 8, mf = mq & 255, dj = dq >> 10, dfr = dq & 1023;
        int32_t t0 = TUNE[mi][dj] + (((TUNE[mi][dj + 1] - TUNE[mi][dj]) * dfr) >> 10);
        int32_t t1 = TUNE[mi + 1][dj] + (((TUNE[mi + 1][dj + 1] - TUNE[mi + 1][dj]) * dfr) >> 10);
        int32_t c = t0 + (((t1 - t0) * mf) >> 8);
        if (c - s->corr > 3 || s->corr - c > 3) ww_set_delays(s, c);
    }
    if (rt0 && (s->rt - rt0 > 32768 || rt0 - s->rt > 32768)) ww_warp(s, a0);
    int32_t di = dyn >> 10, df = dyn & 1023;
    int32_t dp = DYN_POW[di] + (((DYN_POW[di < 40 ? di + 1 : 40] - DYN_POW[di]) * df) >> 10);
    int32_t tgt = s->gth + D_P_Q13 + (((D_F_Q13 - D_P_Q13) * dp) >> 14);
    if (!v->gate) {
        if (!v->rel_init) { v->rel_e = env > 64 ? env : 64; v->rel_init = 1; }
        int32_t r = (env * 32768) / v->rel_e; if (r > 32768) r = 32768;
        tgt = (tgt * r) >> 15;
    }
    if (s->tongue > -OPEN_RAMP_SMP) {
        int32_t e = s->tongue > 0 ? 0 : (-s->tongue) & ~(OP1_BLOCK - 1);
        int32_t cap = HOLD_P_Q13 + (int32_t)(((16383 - HOLD_P_Q13) * e) / OPEN_RAMP_SMP);
        if (tgt > cap) tgt = cap;
    }
    if (tgt < 0) tgt = 0;
    int32_t tgt16 = tgt << 16, a1 = v->gate ? 0 : REL_B30, a2 = v->gate ? ATT2_B30 : REL_B30;
    int32_t g1n = a2 ? tgt16 + (int32_t)(((int64_t)(s->g1 - tgt16) * a2) >> 30) : tgt16;
    int32_t gamn = a1 ? g1n + (int32_t)(((int64_t)(s->gam - g1n) * a1) >> 30) : g1n;   /* a1 0 while held */
    int32_t gstep = (gamn - s->gam) >> 7;
    s->g1 = g1n;

    int16_t zeta = (int16_t)s->zeta;
    int32_t t0 = s->tongue;
    if (t0 > 0) zeta = 0;
    if (t0 > -(TONGUE_FADE_SMP + OP1_BLOCK) || t0 > -(OPEN_RAMP_SMP + OP1_BLOCK)) s->tongue -= OP1_BLOCK;
    run_block(s, out, gstep, zeta);
    if (t0 <= 0) {
        int32_t e0 = (-t0) & ~(OP1_BLOCK - 1);
        if (e0 < TONGUE_FADE_SMP)
            for (int n = 0; n < OP1_BLOCK && e0 + n < TONGUE_FADE_SMP; n++) {
                int32_t x = ((e0 + n) * FADE_RCP) >> 10;
                x = (((x * x) >> 15) * (3 * 32768 - 2 * x)) >> 15;
                out[n] = (int16_t)((out[n] * x) >> 15);
            }
    }
    s->gam = gamn;
}
