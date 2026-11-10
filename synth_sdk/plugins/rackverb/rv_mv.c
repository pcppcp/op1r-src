/* rackverb: the machine kernel (plain C reference) and the program generator. See rv_mv.h.
 *
 * The machine (per step: acc and one ring word; halving rounds toward zero; acc is 16 bits and wraps):
 *   SUMHLF acc += m/2    LDHLF acc = m/2    STRPOS m = acc, acc *= 1.5    STRNEG m = ~acc, acc = ~acc/2
 * Step 0 writes the ADC word; steps 0x60 / 0x70 give the word they touch to the right / left DAC; those three steps
 * leave acc alone. Step k touches ring[(p[k] + n) & mask] in pass n, so a word written at w is read d samples later
 * at w - d: a "line" is a region of the ring, written at its top address and read below it.
 *
 * The generator writes our own programs from a row: macros for loads, allpasses (g = 0.5), 0.5 low-pass and
 * high-pass, a gain ladder (gains in 1/16), modulated reads, then the I/O steps go in at 0, 0x60 and 0x70. It runs at a
 * program change (not per sample), so it may divide.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "rv_mv.h"
#if defined(__bfin__) && !defined(RV_NO_ASM)
#define RV_ASM                                       /* rv_kern.S */
void rv_vsum(int16_t *acc, const int16_t *src, int pairs);
void rv_vld(int16_t *acc, const int16_t *src, int pairs);
void rv_vstp(int16_t *acc, int16_t *dst, int pairs);
void rv_vstn(int16_t *acc, int16_t *dst, int pairs);
#endif

/* ---------------------------------------------------------------------------------------------- the kernel */
static inline int16_t rv_half(int16_t x) { return (int16_t)((x >> 1) + (x < 0)); }

void rv_mv_run_ref(rv_mv_t *m, const int16_t *in, int16_t *l, int16_t *r, int count)
{
    const rv_prog_t *p = &m->prog;
    int16_t acc = m->acc;
    uint16_t n = m->n;
    for (int i = 0; i < count; i++, n++) {
        int16_t outl = 0, outr = 0;
        for (int k = 0; k < RV_STEPS; k++) {
            int16_t *w = &m->ring[(uint16_t)(p->p[k] + n) & (RV_RING - 1)], data, nacc;
            switch (p->op[k]) {
            case RV_SUM: data = *w; nacc = (int16_t)(acc + rv_half(data)); break;
            case RV_LD:  data = *w; nacc = rv_half(data); break;
            case RV_STP: data = acc; *w = data; nacc = (int16_t)(acc + rv_half(acc)); break;
            default:     data = (int16_t)~acc; *w = data; nacc = rv_half(data); break;
            }
            if (k == RV_IO_IN) *w = in[i];
            else if (k == RV_IO_R) outr = data;
            else if (k == RV_IO_L) outl = data;
            else acc = nacc;
        }
        l[i] = outl; r[i] = outr;
    }
    m->acc = acc;
    m->n = n;
}

/* the same machine without a branch a step: every step loads its word and writes one back (a read writes back what
 * it read, into the line it just fetched), data = the word read or acc (inverted for STRNEG), acc = (acc or 0) + data/2 */
static inline int16_t rv_step(int16_t *ring, uint32_t e, uint16_t n, int16_t *acc)
{
    int16_t *w = &ring[(uint16_t)(e + n) & (RV_RING - 1)];
    int32_t keep = -(int32_t)(e >> 16 & 1), st = -(int32_t)(e >> 17 & 1), inv = -(int32_t)(e >> 18 & 1);
    int16_t data = (int16_t)((*w & ~st) | ((*acc ^ inv) & st));
    *w = data;
    *acc = (int16_t)((*acc & keep) + rv_half(data));
    return data;
}
/* the block engine: one step at a time over `count` samples (acc is a row). Exact when every read reaches back 0
 * samples (a word this pass wrote earlier) or at least `count` (rv_mv_run cuts the call into such passes: prog.chunk),
 * so no read needs a word a later step of this pass writes; the 0.5 low-pass groups (a one-sample recurrence) run as
 * one loop; the DAC steps read their rows after the pass; the lines are RV_BLOCK_MIN apart (g_line). Writes first touch their lines (no write-allocate
 * in the data cache: a store to a line not in cache is a single SDRAM write). */
static inline void rv_touch(const int16_t *ring, uint32_t a, int count)
{
    volatile int16_t sink;
    for (int i = 0; i < count + 15; i += 16) sink = ring[(a + (uint32_t)i) & (RV_RING - 1)];
    (void)sink;
}
static void rv_mv_block(rv_mv_t *m, const int16_t *in, int16_t *l, int16_t *r, int count)
{
    const rv_prog_t *p = &m->prog;
    int16_t *ring = m->ring, *acc = m->row;          /* the row lives in the state, not on the stack */
    uint32_t n = m->n;
    uint32_t a = (p->p[RV_IO_IN] + n) & (RV_RING - 1);
    rv_touch(ring, a, count);
    for (int i = 0; i < count; i++) ring[(a + i) & (RV_RING - 1)] = in[i];
    for (int k = 1; k < RV_STEPS; k++) {
        int kind = p->kind[k];
        if (kind >= 2) continue;
        a = (p->p[k] + n) & (RV_RING - 1);
        int n1 = RV_RING - (int)a < count ? RV_RING - (int)a : count;   /* the span up to the ring's end, then from 0 */
        int16_t *w = &ring[a];
        if (kind == 1) {                             /* STP t; LD t; SUM ln 1; STP ln: y = acc/2 + y[-1]/2 */
            uint32_t at = (p->p[k] + n) & (RV_RING - 1), ay = (p->p[k + 3] + n) & (RV_RING - 1);
            uint32_t ap = (p->p[k + 2] + n) & (RV_RING - 1);
            rv_touch(ring, at, count); rv_touch(ring, ay, count);
            int16_t y1 = ring[ap];
            for (int i = 0; i < count; i++) {
                ring[(at + i) & (RV_RING - 1)] = acc[i];
                int16_t y = (int16_t)(rv_half(acc[i]) + rv_half(y1));
                ring[(ay + i) & (RV_RING - 1)] = y;
                acc[i] = (int16_t)(y + rv_half(y));
                y1 = y;
            }
            continue;
        }
#ifdef RV_ASM
        if (n1 == count && count >= 2) {            /* no wrap: two samples at a time (rv_kern.S), then an odd one */
            int pairs = count >> 1, o = count - 1;
            switch (p->op[k]) {
            case RV_SUM: rv_vsum(acc, w, pairs); if (count & 1) acc[o] = (int16_t)(acc[o] + rv_half(w[o])); break;
            case RV_LD:  rv_vld(acc, w, pairs); if (count & 1) acc[o] = rv_half(w[o]); break;
            case RV_STP: rv_touch(ring, a, count); rv_vstp(acc, w, pairs);
                         if (count & 1) { w[o] = acc[o]; acc[o] = (int16_t)(acc[o] + rv_half(acc[o])); } break;
            default:     rv_touch(ring, a, count); rv_vstn(acc, w, pairs);
                         if (count & 1) { int16_t v = (int16_t)~acc[o]; w[o] = v; acc[o] = rv_half(v); } break;
            }
            continue;
        }
#endif
        switch (p->op[k]) {
        case RV_SUM:
            for (int i = 0; i < n1; i++) acc[i] = (int16_t)(acc[i] + rv_half(w[i]));
            for (int i = n1; i < count; i++) acc[i] = (int16_t)(acc[i] + rv_half(ring[i - n1]));
            break;
        case RV_LD:
            for (int i = 0; i < n1; i++) acc[i] = rv_half(w[i]);
            for (int i = n1; i < count; i++) acc[i] = rv_half(ring[i - n1]);
            break;
        case RV_STP:
            rv_touch(ring, a, count);
            for (int i = 0; i < n1; i++) { w[i] = acc[i]; acc[i] = (int16_t)(acc[i] + rv_half(acc[i])); }
            for (int i = n1; i < count; i++) { ring[i - n1] = acc[i]; acc[i] = (int16_t)(acc[i] + rv_half(acc[i])); }
            break;
        default:
            rv_touch(ring, a, count);
            for (int i = 0; i < n1; i++) { int16_t v = (int16_t)~acc[i]; w[i] = v; acc[i] = rv_half(v); }
            for (int i = n1; i < count; i++) { int16_t v = (int16_t)~acc[i]; ring[i - n1] = v; acc[i] = rv_half(v); }
            break;
        }
    }
    uint32_t ar = (p->p[RV_IO_R] + n) & (RV_RING - 1), al = (p->p[RV_IO_L] + n) & (RV_RING - 1);
    for (int i = 0; i < count; i++) { r[i] = ring[(ar + i) & (RV_RING - 1)]; l[i] = ring[(al + i) & (RV_RING - 1)]; }
    m->n = (uint16_t)(n + count);
}

void rv_mv_run(rv_mv_t *m, const int16_t *in, int16_t *l, int16_t *r, int count)
{
    if (m->prog.chunk < 8) {                         /* a short delay (a flange): one sample at a time, active steps */
        const rv_prog_t *p = &m->prog;
        int16_t *ring = m->ring, acc = 0;
        uint16_t n = m->n;
        for (int i = 0; i < count; i++, n++) {
            ring[(uint16_t)(p->p[RV_IO_IN] + n) & (RV_RING - 1)] = in[i];
            for (int j = 0; j < p->nact; j++) rv_step(ring, p->e[p->act[j]], n, &acc);
            r[i] = ring[(uint16_t)(p->p[RV_IO_R] + n) & (RV_RING - 1)];   /* the DAC reads after the pass: the same */
            l[i] = ring[(uint16_t)(p->p[RV_IO_L] + n) & (RV_RING - 1)];
        }
        m->n = n;
        return;
    }
    for (int o = 0, c = m->prog.chunk; o < count; o += c)   /* passes no longer than the shortest delay: exact */
        rv_mv_block(m, in + o, l + o, r + o, count - o < c ? count - o : c);
}

void rv_mv_run_steps(rv_mv_t *m, const int16_t *in, int16_t *l, int16_t *r, int count)   /* one sample at a time */
{
    const uint32_t *e = m->prog.e;
    int16_t *ring = m->ring, acc = m->acc;
    uint16_t n = m->n;
    for (int i = 0; i < count; i++, n++) {
        ring[(uint16_t)(e[0] + n) & (RV_RING - 1)] = in[i];
        for (int k = 1; k < RV_IO_R; k++) rv_step(ring, e[k], n, &acc);
        int16_t keep = acc;
        r[i] = rv_step(ring, e[RV_IO_R], n, &acc); acc = keep;
        for (int k = RV_IO_R + 1; k < RV_IO_L; k++) rv_step(ring, e[k], n, &acc);
        keep = acc;
        l[i] = rv_step(ring, e[RV_IO_L], n, &acc); acc = keep;
        for (int k = RV_IO_L + 1; k < RV_STEPS; k++) rv_step(ring, e[k], n, &acc);
    }
    m->acc = acc;
    m->n = n;
}

/* ------------------------------------------------------------------------------------------- the generator */
#define G_BODY 125
#define G_LINES 40
typedef struct {
    int32_t fs2, err, k_ms, k_cms;
    int top, nb, nl;
    uint8_t op[G_BODY], ln[G_BODY];
    uint16_t d[G_BODY];
    uint16_t lw[G_LINES], ll[G_LINES];
    int nmod, nrec;
    int8_t at[G_BODY];                               /* body op -> step (assembly) */
    uint8_t rec_at[16];                              /* low-pass groups (4 steps: STP t, LD t, SUM ln 1, STP ln) */
    uint8_t mod_at[RV_LFOS], mod_lfo[RV_LFOS], mod_ln[RV_LFOS];
    int32_t mod_d4[RV_LFOS], mod_depth4[RV_LFOS];
} gen_t;

static int g_line(gen_t *g, int len)                 /* a line of len samples (0: a scratch word) -> its id */
{
    if (g->nl >= G_LINES || len < 0) { g->err = 1; return 0; }
    int w = g->top;
    g->top = w - len - 1 - RV_BLOCK_MIN;             /* a guard: no two lines meet within one call (block engine) */
    if (g->top < RV_BLOCK_MIN) g->err = 1;
    g->lw[g->nl] = (uint16_t)(w < 0 ? 0 : w);
    g->ll[g->nl] = (uint16_t)len;
    return g->nl++;
}
static void g_emit(gen_t *g, int op, int ln, int d)
{
    if (g->nb >= G_BODY || d < 0 || d > g->ll[ln]) { g->err = 1; return; }
    g->op[g->nb] = (uint8_t)op; g->ln[g->nb] = (uint8_t)ln; g->d[g->nb] = (uint16_t)d; g->nb++;
}
/* ms and 0.01 ms -> samples, by multiplying: fs2 / 2000 and fs2 / 200000 in Q16 (exact at both rates; no division) */
static int g_ms(gen_t *g, int32_t ms) { int32_t s = (ms * g->k_ms + 32768) >> 16; return s < 1 ? 1 : s; }
static int g_cms(gen_t *g, int32_t cms) { int32_t s = (cms * g->k_cms + 32768) >> 16; return s < 1 ? 1 : s; }

/* acc semantics in the comments: x = the word read */
static void g_load(gen_t *g, int ln, int d) { g_emit(g, RV_LD, ln, d); g_emit(g, RV_SUM, ln, d); }   /* acc = x */
static void g_store(gen_t *g, int ln) { g_emit(g, RV_STP, ln, 0); }                                /* ln = acc */
/* g = 0.5 allpass, output inverted: s = -(a + s[-L]/2); acc = -a/2 + 3/4 s[-L] = -(0.5 + z^-L)/(1 + 0.5 z^-L) a */
static void g_allpass(gen_t *g, int ln)
{
    int L = g->ll[ln];
    g_emit(g, RV_SUM, ln, L); g_emit(g, RV_STN, ln, 0); g_emit(g, RV_SUM, ln, L); g_emit(g, RV_SUM, ln, L);
}
/* ln[0] = (acc + ln[-1]) / 2: the line's newest word is the one-pole's state (tmp: a scratch word) */
static void g_rec(gen_t *g) { if (g->nrec < 16) g->rec_at[g->nrec++] = (uint8_t)g->nb; else g->err = 1; }
static void g_lowpass_into(gen_t *g, int tmp, int ln)
{
    g_rec(g);
    g_emit(g, RV_STP, tmp, 0); g_emit(g, RV_LD, tmp, 0); g_emit(g, RV_SUM, ln, 1); g_emit(g, RV_STP, ln, 0);
}
/* acc = acc - lowpass(acc): x0 a scratch word, st the one-pole's 1-word line, neg a scratch word */
static void g_highpass(gen_t *g, int x0, int st, int neg)
{
    g_rec(g);
    g_emit(g, RV_STP, x0, 0); g_emit(g, RV_LD, x0, 0); g_emit(g, RV_SUM, st, 1); g_emit(g, RV_STP, st, 0);
    g_load(g, st, 0); g_emit(g, RV_STN, neg, 0);                                    /* neg = -s */
    g_load(g, x0, 0); g_emit(g, RV_SUM, neg, 0); g_emit(g, RV_SUM, neg, 0);         /* acc = x - s */
}
/* h[0..3] = x/2, x/4, x/8, x/16 of src[-d]; afterwards src[-d] read with SUMHLF adds x/2, h[i] adds x/2^(i+2) */
static void g_ladder(gen_t *g, int src, int d, const int *h)
{
    g_emit(g, RV_LD, src, d); g_emit(g, RV_STP, h[0], 0);
    for (int i = 1; i < 4; i++) { g_emit(g, RV_LD, h[i - 1], 0); g_emit(g, RV_STP, h[i], 0); }
}
static void g_gain(gen_t *g, int src, int d, const int *h, int k, int first)   /* acc (+)= k/32 * x */
{
    k = k < 0 ? 0 : (k > 31 ? 31 : k);
    int j = 0;
    if (k & 16) { g_emit(g, first ? RV_LD : RV_SUM, src, d); j++; }
    for (int i = 0; i < 4; i++) if (k & (8 >> i)) { g_emit(g, first && !j ? RV_LD : RV_SUM, h[i], 0); j++; }
}
/* a modulated read of ln around d4 (1/4 samples) +- depth4: RV_LFO_READS reads, the first an LDHLF when first */
static void g_modread(gen_t *g, int ln, int32_t d4, int32_t depth4, int lfo, int first)
{
    if (g->nmod >= RV_LFOS) { g->err = 1; return; }
    g->mod_at[g->nmod] = (uint8_t)g->nb; g->mod_lfo[g->nmod] = (uint8_t)lfo; g->mod_ln[g->nmod] = (uint8_t)ln;
    g->mod_d4[g->nmod] = d4; g->mod_depth4[g->nmod] = depth4; g->nmod++;
    int d = d4 >> 2;
    for (int i = 0; i < RV_LFO_READS; i++) g_emit(g, first && !i ? RV_LD : RV_SUM, ln, d);
}

/* -log2(k/32) in Q12, k = 0..32: the loop gain nearest a wanted attenuation */
static const uint16_t g_nlog[33] = { 65535, 20480, 16384, 13988, 12288, 10969, 9892, 8981, 8192, 7496, 6873, 6310, 5796, 5323, 4885, 4477, 4096, 3738, 3400, 3080, 2777, 2489, 2214, 1951, 1700, 1459, 1227, 1004, 789, 582, 381, 188, 0 };
static int g_k_for(int32_t e_q12)
{
    int best = 1;
    for (int k = 1; k <= 31; k++) {
        int32_t a = e_q12 - g_nlog[k], b = e_q12 - g_nlog[best];
        if ((a < 0 ? -a : a) < (b < 0 ? -b : b)) best = k;
    }
    return best;
}
/* the per-pass gain (k/32) that gives RT60 rt_cs (10 ms) for a loop of loop samples taking `passes` gains */
static int g_k_rt(gen_t *g, int32_t loop, int32_t rt_cs, int passes, int tone)
{
    if (rt_cs < 1) rt_cs = 1;
    /* e = log2(1000) * loop / fs / RT / passes = 9.966 * (2 loop / fs2) / (rt_cs / 100) / passes, Q12 */
    int32_t e = loop * 81641 / (g->fs2 / 100);         /* 9.966 * 2 * 4096 * loop / fs2 (x100 for rt_cs), 32 bits */
    e = e / (rt_cs * passes);
    if (tone) e = e * 10 / 13;                       /* the loop low-pass shortens the tail: measured x1.3 */
    return g_k_for(e > 60000 ? 60000 : e);
}

static const int16_t SZ[4] = { 90, 179, 294, 384 };   /* size classes, Q8: small, medium, large, xlarge */
static const uint8_t AP_IN[4] = { 31, 47, 79, 113 };   /* 0.1 ms */
static const uint16_t AP_LOOP[4] = { 233, 317, 271, 379 }, DL[2] = { 890, 1070 };   /* 0.1 ms */

static void g_reverb(gen_t *g, const rv_row_t *row)
{
    int32_t sz = SZ[row->a & 3], pd = row->u;
    int32_t room = g->top - 25 * (RV_BLOCK_MIN + 1) - (pd ? g_ms(g, pd) + 2 : 0) - 64;   /* 25 lines follow */
    int ai[4], al[4], da, db, lai[4], lal[4], lda, ldb;
    for (;;) {                                       /* shrink the size until the lines fit the ring */
        int32_t sum = 0;
        for (int i = 0; i < 4; i++) {
            lai[i] = g_cms(g, AP_IN[i] * 10 * (179 + 77 * sz / 256) / 256);
            lal[i] = g_cms(g, AP_LOOP[i] * 10 * sz / 256);
            sum += lai[i] + lal[i] + 2;
        }
        lda = g_cms(g, DL[0] * 10 * sz / 256); ldb = g_cms(g, DL[1] * 10 * sz / 256);
        sum += lda + ldb + 2;
        if (sum <= room || sz < 40) break;
        sz = sz * 15 >> 4;
    }
    int ha[4], hb[4], t0 = g_line(g, 0), t1 = g_line(g, 0), pdl = 0;
    for (int i = 0; i < 4; i++) { ha[i] = g_line(g, 0); hb[i] = g_line(g, 0); }
    for (int i = 0; i < 4; i++) { ai[i] = g_line(g, lai[i]); al[i] = g_line(g, lal[i]); }
    da = g_line(g, lda); db = g_line(g, ldb);
    if (pd) pdl = g_line(g, g_ms(g, pd));
    int32_t loop = lda + ldb + lal[0] + lal[1] + lal[2] + lal[3];
    int tone = row->b == 2 && row->t > 600 ? 1 : row->b;   /* two 0.5 low-passes cap the tail near 8 s: one for longer */
    int k = g_k_rt(g, loop, row->t, 2, tone);
    g_ladder(g, db, ldb, hb);
    g_ladder(g, da, lda, ha);
    if (pd) { g_load(g, 0, 0); g_store(g, pdl); g_load(g, pdl, g->ll[pdl]); }
    else g_load(g, 0, 0);
    for (int i = 0; i < 4; i++) g_allpass(g, ai[i]);
    g_gain(g, db, ldb, hb, k, 0);
    g_allpass(g, al[0]); g_allpass(g, al[1]);
    if (tone >= 1) g_lowpass_into(g, t0, da); else g_store(g, da);
    g_gain(g, da, lda, ha, k, 1);
    g_allpass(g, al[2]); g_allpass(g, al[3]);
    if (tone >= 2) g_lowpass_into(g, t1, db); else g_store(g, db);
    static const uint8_t TL[4] = { 33, 105, 172, 212 }, TR[4] = { 49, 95, 156, 233 };   /* tap positions, /256 */
    for (int side = 0; side < 2; side++) {
        for (int j = 0; j < 4; j++) {
            int ln = (j & 1) ^ side ? db : da;
            int d = g->ll[ln] * (side ? TR[j] : TL[j]) >> 8;
            g_emit(g, j ? RV_SUM : RV_LD, ln, d);
            if (sz > 128) g_emit(g, RV_SUM, ln, d);
        }
        g_store(g, side ? 2 : 1);                    /* lines 1, 2: OUTL, OUTR */
    }
}

/* GATE (flat), REVERSE (rising) and BLOOM (rising, regenerated into a long swell): a tap cloud over the window */
static void g_cloud(gen_t *g, const rv_row_t *row)
{
    int ai[3], s0 = g_line(g, 0), h[4];
    static const uint8_t AP[3] = { 23, 37, 53 };     /* 0.1 ms */
    for (int i = 0; i < 4; i++) h[i] = g_line(g, 0);
    for (int i = 0; i < 3; i++) ai[i] = g_line(g, g_cms(g, AP[i] * 10));
    int W = g_ms(g, row->t), room = g->top - 2 * (RV_BLOCK_MIN + 1) - 8;
    if (W > room) W = room;
    int G = g_line(g, W);
    int regen = row->shape == RV_BLOOM ? g_k_rt(g, W, row->u, 1, 0) : 2 * row->b;   /* 1/32 */
    if (regen) g_ladder(g, G, W, h);
    g_load(g, 0, 0);
    for (int i = 0; i < 3; i++) g_allpass(g, ai[i]);
    g_store(g, s0); g_emit(g, RV_LD, s0, 0);         /* acc = y/2 */
    if (regen) g_gain(g, G, W, h, regen, 0);
    g_store(g, G);
    int n = 18, rise = row->shape == RV_GATE ? 0 : (row->c ? row->c : 2);
    for (int side = 0; side < 2; side++) {
        int first = 1;
        for (int k = 0; k < n; k++) {
            int32_t t = (2 * k + side) * 1820 >> 6;     /* the tap's place in the window, Q10 (1024 / 36 = 28.4) */
            int32_t rr = rise * t * t >> 20;
            int reps = 1 + (rr > 2 ? 2 : rr);
            int d = W * (20 + (t * 993 >> 10)) >> 10;   /* 2 % .. 99 % of the window */
            if (d < RV_BLOCK_MIN) d = RV_BLOCK_MIN;      /* the first taps of a short gate: block-wise */
            if (d > W) d = W;
            for (int j = 0; j < reps; j++) { g_emit(g, first ? RV_LD : RV_SUM, G, d); first = 0; }
        }
        g_store(g, side ? 2 : 1);
    }
}

/* the filter of an echo's loop: acc in, acc out (LPF ends with the store into E itself) -> 1 when E was written */
static int g_filter(gen_t *g, int f, int E)
{
    if (f == RV_HPF || f == RV_BPF) {
        int x0 = g_line(g, 0), st = g_line(g, 1), neg = g_line(g, 0);
        g_highpass(g, x0, st, neg);
    }
    if (f == RV_LPF || f == RV_BPF) { g_lowpass_into(g, g_line(g, 0), E); return 1; }
    return 0;
}

/* ECHO / TAPS: one line, regeneration through the filter, taps spread over L/R by the stereo mode */
static void g_echo(gen_t *g, const rv_row_t *row)
{
    int h[4];
    for (int i = 0; i < 4; i++) h[i] = g_line(g, 0);
    int D = g_ms(g, row->t), room = g->top - 5 * (RV_BLOCK_MIN + 1) - 8;
    if (D > room) D = room;
    int E = g_line(g, D), taps = row->a < 1 ? 1 : (row->a > 8 ? 8 : row->a), regen = 2 * row->u;   /* 1/32 */
    if (regen) g_ladder(g, E, D, h);
    g_load(g, 0, 0);
    if (regen) g_gain(g, E, D, h, regen, 0);
    if (!g_filter(g, row->b, E)) g_store(g, E);
    int st = row->c;
    for (int side = 0; side < 2; side++) {
        int first = 1;
        for (int k = 1; k <= taps; k++) {
            static const uint16_t RCP[9] = { 0, 4096, 2048, 1366, 1024, 820, 683, 586, 512 };   /* 4096 / taps */
            int d = D * k * RCP[taps] >> 12;         /* the taps, the last at the full time */
            if (k == taps) d = D;
            if (taps == 1) {                         /* a single echo: the stereo mode places the two sides */
                if (st == RV_AMBI && side) d = D * 15 >> 4;
                if (st == RV_WIDE && side) d = D * 5 >> 3;
                if (st == RV_PAN && side) d = D / 2;
                if (st == RV_THICKS) { g_emit(g, first ? RV_LD : RV_SUM, E, D * (32 - 2 * side) >> 5); first = 0; d = D * (31 - 2 * side) >> 5; }
            } else if (st == RV_PAN) {               /* taps alternate sides */
                if ((k & 1) == side) continue;
            } else if (side) d = d * (st == RV_WIDE ? 13 : 15) >> 4;
            if (d < 1) d = 1;
            g_emit(g, first ? RV_LD : RV_SUM, E, d); first = 0;
            if (taps <= 2 || st == RV_PAN) g_emit(g, RV_SUM, E, d);
        }
        if (first) g_emit(g, RV_LD, E, D);
        g_store(g, side ? 2 : 1);
    }
}

/* MOD (chorus / flange / thickener): the input at half level into M, two modulated reads (L, R) of four quarter-
 * sample reads each sum back to unit gain; feedback (signed) from the left read through the ladder */
static void g_mod(gen_t *g, const rv_row_t *row)
{
    int h[4], ta = g_line(g, 0), tb = g_line(g, 0), tn = g_line(g, 0);
    for (int i = 0; i < 4; i++) h[i] = g_line(g, 0);
    int32_t base4 = 4 * g_cms(g, row->t), depth4 = row->u ? 4 * g_cms(g, row->u) : 0;
    int32_t off4 = row->a > 1 ? 4 * g_cms(g, 70) : 0;  /* the right voice 0.7 ms further */
    if (base4 < depth4 + 4 * 16) base4 = depth4 + 4 * 16;   /* the sweep's floor 16 samples: passes of 16 (block engine) */
    int M = g_line(g, (base4 + off4 + depth4) / 4 + 3);
    int fb = row->b > 7 ? row->b - 16 : row->b;      /* -8..7, loop gain fb/8 */
    g_modread(g, M, base4, depth4, 0, 1); g_store(g, ta);
    g_modread(g, M, base4 + off4, depth4, 1, 1); g_store(g, tb);
    if (fb) {
        int src = ta;
        if (fb < 0) { g_load(g, ta, 0); g_emit(g, RV_STN, tn, 0); src = tn; fb = -fb; }
        g_ladder(g, src, 0, h);
        g_emit(g, RV_LD, 0, 0);                      /* x/2 */
        g_gain(g, src, 0, h, 2 * fb, 0);             /* + fb/16 y: loop gain fb/8 */
    } else
        g_emit(g, RV_LD, 0, 0);
    g_store(g, M);
    if (row->b) { g_emit(g, RV_LD, ta, 0); g_store(g, 1); g_emit(g, RV_LD, tb, 0); g_store(g, 2); }   /* resonant: half */
    else { g_load(g, ta, 0); g_store(g, 1); g_load(g, tb, 0); g_store(g, 2); }
}

/* SGEN: L = (x + x[-D]) / 2, R = (x - x[-D]) / 2, a complementary comb pair (stereo from mono) */
static void g_sgen(gen_t *g, const rv_row_t *row)
{
    int D = g_ms(g, row->t), S = g_line(g, D), neg = g_line(g, 0);
    g_load(g, 0, 0); g_store(g, S);
    g_emit(g, RV_LD, 0, 0); g_emit(g, RV_SUM, S, D); g_store(g, 1);
    g_load(g, S, D); g_emit(g, RV_STN, neg, 0); g_emit(g, RV_LD, 0, 0); g_emit(g, RV_SUM, neg, 0); g_store(g, 2);
}

int rv_mv_build(rv_prog_t *p, const rv_row_t *row, int32_t fs2)
{
    static gen_t g __attribute__((aligned(32)));     /* 1 KB: not on the audio stack (no reentrancy: UI-free) */
    {                                                /* touch the lines first: no write-allocate (rv_touch) */
        volatile uint32_t sink;
        for (unsigned i = 0; i < sizeof g / 4; i += 8) sink = ((uint32_t *)&g)[i];
        for (unsigned i = 0; i < sizeof *p / 4; i += 8) sink = ((uint32_t *)p)[i];
        (void)sink;
    }
    for (unsigned i = 0; i < sizeof g / 4; i++) ((uint32_t *)&g)[i] = 0;
    g.fs2 = fs2; g.top = RV_RING - 1;
    g.k_ms = (int32_t)(((uint32_t)fs2 << 10) / 2000 * 64);   /* fs2 / 2000 in Q16 */
    g.k_cms = (int32_t)(((uint32_t)fs2 << 10) / 200000 * 64 + (((uint32_t)fs2 << 10) % 200000) * 64 / 200000);   /* Q16 */
    g_line(&g, 0); g_line(&g, 1); g_line(&g, 1);     /* lines 0 IN, 1 OUTL, 2 OUTR */
    switch (row->shape) {
    case RV_REV: g_reverb(&g, row); break;
    case RV_GATE: case RV_REVERSE: case RV_BLOOM: g_cloud(&g, row); break;
    case RV_ECHO: case RV_TAPS: g_echo(&g, row); break;
    case RV_MOD: g_mod(&g, row); break;
    case RV_SGEN: g_sgen(&g, row); break;
    default: break;                                  /* DEFEAT: no body; the wrapper mutes the wet */
    }
    if (g.err) return -1;
    /* the I/O steps go in at 0, 0x60, 0x70; the body fills the rest in order, padded with loads of IN */
    int8_t *at = g.at;
    int b = 0, wout[3] = { 999, 999, 999 };
    for (int k = 0; k < RV_STEPS; k++) {
        int op = RV_LD, ln = 0, d = 0;
        if (k == RV_IO_IN) ;
        else if (k == RV_IO_R || k == RV_IO_L) ln = k == RV_IO_R ? 2 : 1, d = -1;
        else if (b < g.nb) { op = g.op[b]; ln = g.ln[b]; d = g.d[b]; at[b] = (int8_t)k; b++;
                             if ((op == RV_STP || op == RV_STN) && ln <= 2) wout[ln] = k; }
        p->op[k] = (uint8_t)op; p->w[k] = g.lw[ln]; p->d[k] = (uint16_t)(d < 0 ? 0 : d);
    }
    /* a DAC step reads this pass's sum when it was written before it, else the last pass's (one sample late) */
    p->d[RV_IO_R] = (uint16_t)(wout[2] < RV_IO_R ? 0 : 1);
    p->d[RV_IO_L] = (uint16_t)(wout[1] < RV_IO_L ? 0 : 1);
    if (g.nb && g.op[0] != RV_LD) return -1;          /* a pass starts by loading: no state in acc between samples */
    for (int k = 0; k < RV_STEPS; k++) p->kind[k] = 3;
    for (int i = 0; i < g.nb; i++) p->kind[at[i]] = 0;
    for (int i = 0; i < g.nrec; i++) {
        int a = at[g.rec_at[i]];
        for (int j = 1; j < 4; j++) if (at[g.rec_at[i] + j] != a + j) return -1;   /* contiguous */
        p->kind[a] = 1; p->kind[a + 1] = p->kind[a + 2] = p->kind[a + 3] = 2;
    }
    p->nact = 0;
    for (int k = 0; k < RV_STEPS; k++) if (p->kind[k] != 3) p->act[p->nact++] = (uint8_t)k;
    p->nmod = (uint8_t)g.nmod;
    for (int i = 0; i < g.nmod; i++) {
        p->mod[i].step = (uint8_t)at[g.mod_at[i]];
        p->mod[i].lfo = g.mod_lfo[i]; p->mod[i].w = g.lw[g.mod_ln[i]];
        p->mod[i].d4 = g.mod_d4[i]; p->mod[i].depth4 = g.mod_depth4[i];
        for (int j = 1; j < RV_LFO_READS; j++) if (at[g.mod_at[i] + j] != at[g.mod_at[i]] + j) return -1;   /* contiguous */
    }
    /* LFOs: a triangle, phase 0 .. 2^25 (2^24 = the top), Q8 per sample */
    int32_t fs = fs2 / 2, mhz = row->shape == RV_MOD ? row->v : 0;
    for (int i = 0; i < 2; i++) {
        p->lfo_inc[i] = mhz * 33554 / fs;           /* 2^25 / 1000 = 33554.4: phase steps a sample */
        p->lfo_ph[i] = i ? (1 << 23) : 0;            /* R a quarter cycle on (a stereo sweep) */
    }
    p->trig = row->shape == RV_MOD && row->c;
    p->used = (uint8_t)g.nb;
    p->ring_used = (uint16_t)(RV_RING - 1 - g.top);
    p->size_q15 = -1;                                /* rv_mv_size (the caller, maybe a call later) sets p[] and e[] */
    return g.nb;
}

void rv_mv_size(rv_prog_t *p, int32_t size_q15)
{
    if (size_q15 == p->size_q15) return;
    p->size_q15 = size_q15;
    int32_t f = 16384 + (size_q15 >> 1);             /* 50..100 % in Q15 */
    int32_t ch = RV_BLOCK_MIN;
    for (int k = 0; k < RV_STEPS; k++) {
        int32_t d = p->d[k];
        if (d > 1) {
            int32_t d0 = d;
            d = d * f >> 15;
            if (d < 2) d = 2;
            if (d0 >= RV_BLOCK_MIN && d < RV_BLOCK_MIN) d = RV_BLOCK_MIN;   /* a long delay stays block-wise */
        }
        if (p->kind[k] == 0 && p->op[k] <= RV_LD && d && d < ch) ch = d;
        p->p[k] = (uint16_t)((p->w[k] - d) & (RV_RING - 1));
    }
    for (int m = 0; m < p->nmod; m++) { int32_t d = (p->mod[m].d4 - p->mod[m].depth4) / 4; if (d < ch) ch = d; }
    p->chunk = (uint8_t)(ch < 1 ? 1 : ch);
    rv_mv_lfo(p, 0);                                 /* the modulated reads keep their own delays; it writes e[] */
}

static void rv_mv_encode(rv_prog_t *p)             /* p[] and op[] -> the kernel's e[] */
{
    static const uint8_t F[4] = { 1, 0, 3, 4 | 2 };  /* SUM keep; LD -; STP keep store; STN store invert */
    for (int k = 0; k < RV_STEPS; k++) p->e[k] = p->p[k] | (uint32_t)F[p->op[k]] << 16;
}

void rv_mv_lfo(rv_prog_t *p, int samples)
{
    for (int i = 0; i < 2; i++) {
        int32_t ph = p->lfo_ph[i] + p->lfo_inc[i] * samples;
        if (p->trig) { if (ph > (1 << 24)) ph = 1 << 24; }   /* triggered: one sweep up, then it holds */
        else ph &= (1 << 25) - 1;
        p->lfo_ph[i] = ph;
    }
    for (int m = 0; m < p->nmod; m++) {
        const rv_mod_t *q = &p->mod[m];
        int32_t ph = p->lfo_ph[q->lfo], tri = ph < (1 << 24) ? ph : (1 << 25) - ph;   /* 0 .. 2^24 */
        int32_t d4 = q->d4 - q->depth4 + (q->depth4 * (tri >> 9) >> 14);   /* depth4 < 2^14, tri >> 9 < 2^15 */
        int32_t d = d4 >> 2, f = d4 & 3;
        for (int j = 0; j < RV_LFO_READS; j++)
            p->p[q->step + j] = (uint16_t)((q->w - d - (j >= RV_LFO_READS - f)) & (RV_RING - 1));
    }
    if (!samples || p->nmod) rv_mv_encode(p);
}

void rv_mv_trigger(rv_prog_t *p) { p->lfo_ph[0] = 0; p->lfo_ph[1] = 0; }
