/* st_pitch.c - integer-only incremental YIN pitch detector, see st_pitch.h.
 *
 * Pipeline (all integer, no libc; 64-bit divisions only once per lag result):
 *   1. prep:   copy the window (up to 2048 samples from the middle of the range), sum / sum of squares for the
 *              level check (variance below -50 dBFS RMS -> no note), half-rate copy (average of sample pairs).
 *   2. diff:   half-rate difference d(tau) = sum (x[j]-x[j+tau])^2 over a fixed length for tau = 1..maxlag, with the
 *              cumulative mean normalised difference d'(tau) = d(tau) * tau / sum d(1..tau) in Q16.
 *   3. search: first tau >= 11 under 0.15, walked down to its local minimum; else the global minimum; above 0.35
 *              -> no note. Parabolic interpolation gives a half-rate lag estimate.
 *   4. refine: full-rate d at 7 lags around twice that lag, parabolic interpolation on the minimum (the half-rate
 *              grid is too coarse for 1 cent at 1-2 kHz).
 *   5. finish: lag -> frequency (one 64-bit division) -> log2 (65-entry table, linear interpolation, < 0.06 cent)
 *              -> MIDI note + cents.
 * Lags 22..1400 at full rate = 2004 Hz .. 31.5 Hz (shorter windows lower the longest lag, 1024 samples: 65 Hz).
 *
 * Cost of one full analysis of a 2048-sample window (inner-loop multiply-accumulates, counted in w->ops):
 *   prep 2048 + diff 683 lags * 341 = 232,903 + refine 7 * 677 = 4,739 + search ~680  ->  about 240,400.
 * Roughly 1-2 ms on a BF524 at 600 MHz (64-bit accumulate); 1 work unit (one lag) is 341 of them.
 * Needs libgcc's 64-bit divide on Blackfin (about 700 + 3 divisions per analysis).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "st_pitch.h"

enum { PH_DONE = 0, PH_PREP, PH_DIFF, PH_SEARCH, PH_REFINE, PH_FINISH };

#define PREP_CHUNK   128
#define TAU_MIN      11          /* half-rate: 22 at full rate */
#define THR_PICK     9830u       /* 0.15 in Q16 */
#define THR_VALID    22938u      /* 0.35 in Q16 */
#define MS_MIN       10737u      /* mean square at -50 dBFS RMS: 32768^2 * 1e-5 */
#define LOG2_440_Q24 415762225   /* round(log2(440 * 65536) * 2^24) */

/* log2(1 + i/64) in Q24 */
static const uint32_t log2_tab[65] = {
    0, 375270, 744810, 1108793, 1467383, 1820738, 2169009, 2512340, 2850868, 3184728, 3514044, 3838941, 4159533,
    4475935, 4788255, 5096595, 5401057, 5701737, 5998727, 6292118, 6581994, 6868440, 7151536, 7431359, 7707984,
    7981483, 8251926, 8519380, 8783912, 9045584, 9304457, 9560591, 9814042, 10064867, 10313120, 10558852,
    10802114, 11042956, 11281425, 11517568, 11751428, 11983051, 12212479, 12439752, 12664911, 12887994, 13109041,
    13328087, 13545168, 13760320, 13973576, 14184969, 14394532, 14602297, 14808293, 15012551, 15215099, 15415967,
    15615181, 15812769, 16008758, 16203172, 16396036, 16587377, 16777216
};

void st_pitch_begin(st_pitch_work *w, const int16_t *data, int32_t start, int32_t end)
{
    st_pitch_begin_rate(w,data,start,end,ST_PITCH_RATE);
}
void st_pitch_begin_rate(st_pitch_work *w,const int16_t *data,int32_t start,int32_t end,uint32_t rate)
{
    w->sample_rate = rate==22050 ? 22050 : ST_PITCH_RATE;
    int32_t n = end - start, len, off;
    w->valid = 0; w->note = 0; w->cents = 0; w->freq_q16 = 0; w->ops = 0;
    w->pos = 0; w->sum = 0; w->sumsq = 0; w->cum = 0;
    if (!data || start < 0 || n < ST_PITCH_MIN) { w->phase = PH_DONE; return; }
    if (n > ST_PITCH_N) { off = start + ((n - ST_PITCH_N) >> 1); len = ST_PITCH_N; }
    else { off = start; len = n & ~1; }
    w->src = data + off;
    w->len = len;
    /* longest lag: 700, or less so that the integration length stays at least a third of the half-rate window */
    {
        int32_t dlen = len >> 1;
        int32_t m = dlen - dlen / 3;
        w->maxlag = m < ST_PITCH_MAXLAG ? m : ST_PITCH_MAXLAG;
        w->wds = dlen - w->maxlag;
        w->wf = len - 2 * w->maxlag - 5;
    }
    w->phase = PH_PREP;
}

static void do_prep(st_pitch_work *w)
{
    int32_t i = w->pos, e = i + PREP_CHUNK;
    if (e > w->len) e = w->len;
    for (; i < e; i += 2) {
        int32_t a = w->src[i], b = w->src[i + 1];
        w->x[i] = (int16_t)a; w->x[i + 1] = (int16_t)b;
        w->sum += a + b;
        w->sumsq += (uint32_t)(a * a) + (uint32_t)(b * b);
        w->xd[i >> 1] = (int16_t)((a + b) >> 1);
    }
    w->ops += (uint32_t)(e - w->pos);
    w->pos = e;
    if (e >= w->len) {
        /* variance * len^2 = sumsq * len - sum^2 ; compare with MS_MIN * len^2 (no division) */
        uint64_t l = (uint64_t)w->len;
        uint64_t v = w->sumsq * l - (uint64_t)(w->sum * w->sum);
        if (v < (uint64_t)MS_MIN * l * l) { w->phase = PH_DONE; return; }
        w->pos = 1; w->cum = 0; w->dn[0] = 65536u;
        w->phase = PH_DIFF;
    }
}

static void do_diff(st_pitch_work *w)
{
    int32_t tau = w->pos, j, n = w->wds;
    const int16_t *a = w->xd, *b = w->xd + tau;
    uint64_t d = 0;
    for (j = 0; j < n; j++) {
        int32_t t = (int32_t)a[j] - (int32_t)b[j];
        d += (uint32_t)(t * t);
    }
    w->ops += (uint32_t)n;
    w->cum += d;
    if (w->cum == 0) w->dn[tau] = 65536u;
    else {
        /* d/cum in Q22, times tau, down to Q16 */
        uint64_t q = (d << 22) / w->cum;
        w->dn[tau] = (uint32_t)((q * (uint64_t)tau) >> 6);
    }
    if (++w->pos > w->maxlag) w->phase = PH_SEARCH;
}

static void do_search(st_pitch_work *w)
{
    int32_t minlag=w->sample_rate==22050?6:TAU_MIN;
    int32_t t, hi = w->maxlag - 1, best = -1;
    int64_t p;
    int64_t num, den;
    for (t = minlag; t <= hi; t++) {
        if (w->dn[t] < THR_PICK) {
            while (t < hi && w->dn[t + 1] < w->dn[t]) t++;
            best = t;
            break;
        }
    }
    if (best < 0) {
        uint32_t m = 0xFFFFFFFFu;
        for (t = minlag; t <= hi; t++)
            if (w->dn[t] < m) { m = w->dn[t]; best = t; }
    }
    w->ops += (uint32_t)(hi - minlag + 1);
    if (best < minlag || w->dn[best] > THR_VALID) { w->phase = PH_DONE; return; }
    /* parabolic interpolation on d' (only a seed for the full-rate refinement) */
    p = 0;
    if (best > minlag && best < hi) {
        num = (int64_t)w->dn[best - 1] - (int64_t)w->dn[best + 1];
        den = 2 * ((int64_t)w->dn[best - 1] - 2 * (int64_t)w->dn[best] + (int64_t)w->dn[best + 1]);
        if (den > 0) {
            p = (num * 65536) / den;
            if (p > 32768) p = 32768;
            if (p < -32768) p = -32768;
        }
    }
    w->l0 = (int32_t)((((int64_t)best << 17) + 2 * p + 32768) >> 16);
    w->pos = 0;
    w->phase = PH_REFINE;
}

static void do_refine(st_pitch_work *w)
{
    int32_t lag = w->l0 - 3 + w->pos, j, n = w->wf;
    const int16_t *a = w->x, *b = w->x + lag;
    uint64_t d = 0;
    for (j = 0; j < n; j++) {
        int32_t t = (int32_t)a[j] - (int32_t)b[j];
        d += (uint32_t)(t * t);
    }
    w->ops += (uint32_t)n;
    w->df[w->pos] = d;
    if (++w->pos >= 7) w->phase = PH_FINISH;
}

static void do_finish(st_pitch_work *w)
{
    int32_t k, kb = 1;
    int64_t p = 0, lag_q16, cents_total, x;
    uint64_t v, n32, frac, rem;
    uint32_t f, tab0;
    int e;
    int64_t lg;
    for (k = 2; k <= 5; k++)
        if (w->df[k] < w->df[kb]) kb = k;
    {
        int64_t a = (int64_t)w->df[kb - 1], b = (int64_t)w->df[kb], c = (int64_t)w->df[kb + 1];
        int64_t den = 2 * (a - 2 * b + c);
        if (den > 0) {
            p = ((a - c) * 65536) / den;
            if (p > 32768) p = 32768;
            if (p < -32768) p = -32768;
        }
    }
    lag_q16 = ((int64_t)(w->l0 - 3 + kb) << 16) + p;
    if (lag_q16 <= 0) { w->phase = PH_DONE; return; }
    v = (((uint64_t)w->sample_rate) << 32) / (uint64_t)lag_q16;   /* Hz in Q16 */
    f = (uint32_t)v;
    if (f == 0) { w->phase = PH_DONE; return; }
    w->freq_q16 = f;

    /* log2(f) in Q24: integer part from the top bit, fraction from the table */
    e = 31;
    while (!(f >> e)) e--;
    n32 = (uint64_t)f << (31 - e);                 /* top bit at bit 31 */
    frac = n32 & 0x7FFFFFFFu;                      /* 31 fractional bits */
    k = (int32_t)(frac >> 25);                     /* 0..63 */
    rem = frac & 0x1FFFFFFu;
    tab0 = log2_tab[k];
    lg = ((int64_t)e << 24) + (int64_t)tab0 + (int64_t)(((uint64_t)(log2_tab[k + 1] - tab0) * rem) >> 25);
    x = (lg - LOG2_440_Q24) * 1200;                /* cents in Q24 */
    cents_total = 6900 + ((x + (1 << 23)) >> 24);  /* arithmetic shift: floor(x + 0.5) */
    if (cents_total < 0) cents_total = 0;
    if (cents_total > 12799) cents_total = 12799;
    {
        int32_t nn = (int32_t)(cents_total + 50);
        int32_t note = 0;
        while (nn >= 100) { nn -= 100; note++; }   /* no division */
        w->note = (uint8_t)note;
        w->cents = (int8_t)(cents_total - note * 100);
    }
    w->valid = 1;
    w->phase = PH_DONE;
}

int st_pitch_step(st_pitch_work *w, int budget)
{
    while (budget > 0 && w->phase != PH_DONE) {
        switch (w->phase) {
        case PH_PREP:   do_prep(w); break;
        case PH_DIFF:   do_diff(w); break;
        case PH_SEARCH: do_search(w); break;
        case PH_REFINE: do_refine(w); break;
        case PH_FINISH: do_finish(w); break;
        default: w->phase = PH_DONE; break;
        }
        budget--;
    }
    return w->phase != PH_DONE;
}

void st_pitch_result_get(const st_pitch_work *w, st_pitch_result *r)
{
    r->valid = w->valid;
    r->note = w->valid ? w->note : 0;
    r->cents = w->valid ? w->cents : 0;
    r->freq_q16 = w->valid ? w->freq_q16 : 0;
}

int st_pitch_name(int note, char *out)
{
    static const char nm[12] = {'c', 'c', 'd', 'd', 'e', 'f', 'f', 'g', 'g', 'a', 'a', 'b'};
    static const char sh[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    int n = 0, oct = -2, s;
    if (note < 0 || note > 127) { out[0] = 0; return 0; }
    while (note >= 12) { note -= 12; oct++; }
    out[n++] = nm[note];
    if (sh[note]) out[n++] = '#';
    if (oct < 0) { out[n++] = '-'; s = -oct; } else s = oct;
    out[n++] = (char)('0' + s);
    out[n] = 0;
    return n;
}
