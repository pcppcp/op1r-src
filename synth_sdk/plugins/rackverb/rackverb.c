/* rackverb: late-80s rack reverbs and effects on the OP-1. Units VERB I and FEX (the Midiverb I / MidiFex class
 * machine at 23,437.5 Hz), VERB II (the same machine at 31,250 Hz, LFO-patched chorus and flange) and QUAD (a
 * Quadraverb-style multi-effect at 44,100 Hz, rv_quad.c, its state in the position's big buffer). Every program is our own (rv_mv.c's generator, rows in
 * rv_programs.h); no ROM data. Design: OP1-Emulator docs/midiverb-design-2026-11-04.md.
 *
 *   blue   FEEDBACK  the wet sum back into the input, through CUTOFF; its ceiling depends on the program
 *          page 2: SIZE 50..100 % (a word of 0, an unset page, is 100 %)
 *   green  CUTOFF    a one-pole on the wet, 500 Hz .. 11 kHz
 *          page 2: MODE AUTH / CLEAN (2 steps): 12/13-bit converters and 10 kHz filters, or 14 bits and open filters
 *   white  MIX       dry .. wet, equal power, exactly dry at 0
 *          page 2: UNIT VERB I / VERB II / FEX / QUAD (4 steps)
 *   orange PROGRAM   one program a detent (OP1_FX_STEPS(100)); past the unit's last program it stays on the last
 *
 * Around the machine at 44.1 kHz: the analog low-pass (two biquads), linear resampling to the machine's rate, the
 * converters, the machine, back and the reconstruction low-pass. A program, unit or mode change fades the wet out
 * over one call, clears the state 1,024 words a call (the dry plays on), builds the program and fades the wet in.
 * Mono in (the master sums L+R), stereo out; the dry path keeps the master's stereo. QUAD runs on the call's own
 * 64 samples (no resampling), CUTOFF and FEEDBACK at 44.1 kHz.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef OP1_HOST
#pragma GCC optimize ("no-tree-loop-distribute-patterns")          /* no memset calls */
#endif
#include "op1_plugin.h"
#include "rv_mv.h"
#include "rv_programs.h"
#include "rv_tables.h"
#include "rv_fbceil.h"
#include "rv_quad.h"

#define FX_SPREAD_WORDS 1024
#include "fx_spread.h"

enum { U_VERB1, U_VERB2, U_FEX, U_QUAD, U_N };
static const uint8_t U_COUNT[U_N] = { 63, 100, 63, QD_PRESETS }, U_FIRST[U_N] = { 1, 0, 1, 1 };
#define RV_PROG_STEPS 100
#define RV_MAXTICK 48                          /* machine samples a call: 34 or 35 at 23.4 kHz, 45 or 46 at 31.25 */

/* the call's rows: written before read in every call, so they live in the L1 pool's partition when the core gives it
 * (op1_l1_claim every call: no cache misses, no SDRAM writes), else in rv_state's home copy. Not on the stack (the FX
 * can run on R246's 1.5 KB L1 kernel stack). VERB / FEX use the resampler's rows, QUAD its work area (rv_quad.h). */
typedef struct {
    int16_t x[64], wl[64], wr[64];             /* the mono input (QUAD: with FEEDBACK added in place), the wet L, R */
    union {
        struct { int16_t xin[65], tin[RV_MAXTICK], tl[RV_MAXTICK], tr[RV_MAXTICK]; } mv;
        qd_work q;
    } u;
} __attribute__((aligned(32))) rv_rows;
_Static_assert(sizeof(rv_rows) <= 4160, "rv_rows must fit the master's L1 partition (OP1_L1_MASTER, 4,160 bytes)");

typedef struct {
    union { rv_mv_t mv; } u;                   /* first: on the 32-byte line (QUAD's state: the big buffer, qd_of) */
    int8_t unit, prog, mode;
    int16_t fade;                              /* the wet's gain at the start of this call, Q15 */
    int32_t ph, inc, rinc;                     /* resampler: machine periods in Q16 since the last tick; inc a sample */
    int16_t xprev, y0[2], y1[2];
    int16_t aa[3][4];                          /* biquad histories (x1, x2, y1, y2): input, wet L, wet R */
    int16_t lp[2];                             /* CUTOFF one-poles, at the unit's rate */
    int16_t fb[64];                            /* the last call's wet sum, at the unit's rate, for FEEDBACK */
    int32_t env;                               /* triggered flange: input envelope */
    uint8_t armed, defeat;
    int32_t peak;
    rv_rows home;                              /* the rows without an L1 partition: last, neither init nor build clears it */
} rv_state;
_Static_assert(QD_N == 64, "QUAD runs at the FX call's 44.1 kHz");
_Static_assert(sizeof(rv_state) + 31 <= OP1_FX_SCRATCH_BYTES, "rackverb scratch overflow");

#ifdef OP1_HOST
__attribute__((weak)) volatile uint8_t op1_fx_wrap[3][4];   /* the core's (dispatcher.c) on the OP-1 */
#ifndef OP1_L1_HOST_STUBS                     /* op1r: op1_l1.h has the host stubs */
__attribute__((weak)) void *op1_l1_claim(int part, const void *owner, uint32_t bytes, int *fresh)   /* no pool */
{ (void)part; (void)owner; (void)bytes; if (fresh) *fresh = 0; return 0; }
#endif
#endif
static uint8_t l1_owner[3];                    /* the L1 lease's owner per position */
volatile int16_t rv_live[3][6];                /* screen: unit, program index, mode, size, output peak L, R */
static int32_t peak_r[3];                      /* the R output peak's hold (L: rv_state.peak) */
static const char *rv_name_live[3];
static fx_spread_t spread[3];
static int8_t phase[3];                        /* 0 run, 1 fade out, 2 clear, 3 build: outside the cleared state */
static int16_t raw[3][4];
static int32_t q15[3][4];
static uint8_t valid[3];

static rv_state *state(int p) { uintptr_t a = (uintptr_t)op1_fx_scratch(p); return a ? (rv_state *)((a + 31) & ~(uintptr_t)31) : 0; }
/* QUAD's state, 32-byte aligned: the position's fast lease in heap 0 (op1_fx_fast, SDRAM bank 0: QUAD reads ~40
 * ring spans a block, and a cold line costs the unit ~1.8x in the pool's bank 2), else its big buffer (op1_fx_big, the
 * pool), else 0 (QUAD stays dry). The core's answer is fixed from boot on, so a position never changes its lease. */
#ifdef OP1_HOST
static uint8_t host_big[3][sizeof(qd_state) + 32];
#endif
static qd_state *qd_of(int p)
{
#ifdef OP1_HOST
    return (qd_state *)(((uintptr_t)host_big[p % 3] + 31) & ~(uintptr_t)31);
#else
    uint32_t size = 0;
    uintptr_t a = (uintptr_t)op1_fx_fast(p % 3, &size);
    if (!a || size < sizeof(qd_state) + 31) { size = 0; a = (uintptr_t)op1_fx_big(p % 3, &size); }
    return a && size >= sizeof(qd_state) + 31 ? (qd_state *)((a + 31) & ~(uintptr_t)31) : 0;
#endif
}
/* clearing QUAD's state 8 KB a call, each line touched before it is written (no write-allocate: a store to a line
 * not in cache is a single SDRAM write) */
#ifndef RV_QD_CLEAR
#define RV_QD_CLEAR __builtin_offsetof(qd_state, home)   /* the work area after it is scratch (quad_cpu.py's A/B may set it) */
#endif
static struct { uint32_t *p, left; } bigclr[3];
static int bigclr_step(int p)
{
    uint32_t n = bigclr[p].left < 2048 ? bigclr[p].left : 2048, *w = bigclr[p].p;
    if (!n) return 0;
    volatile uint32_t sink;
    for (uint32_t i = 0; i < n; i += 8) sink = w[i];
    (void)sink;
    for (uint32_t i = 0; i < n; i++) w[i] = 0;
    bigclr[p].p = w + n; bigclr[p].left -= n;
    return 1;
}

const char *rv_name(int unit, int prog)
{
    if (unit == U_QUAD) return qd_name(prog);
    const rv_row_t *t = unit == U_VERB1 ? rv_verb1 : (unit == U_VERB2 ? rv_verb2 : rv_fex);
    return t[prog].name;
}
static const rv_row_t *row_of(int unit, int prog)
{
    return unit == U_VERB1 ? &rv_verb1[prog] : (unit == U_VERB2 ? &rv_verb2[prog] : &rv_fex[prog]);
}

#ifdef RV_FBCEIL_SEARCH
int32_t rv_fb_override = -1;                           /* rackverb_host fbceil: the search sets the gain itself */
#endif

/* a biquad over a row in place: Q14 coefficients, 16 x 16 products, the history in locals */
static void bq_row(int16_t *z, const int16_t *c, int16_t *x, int n)
{
    int32_t b0 = c[0], b1 = c[1], b2 = c[2], a1 = c[3], a2 = c[4];
    int16_t x1 = z[0], x2 = z[1], y1 = z[2], y2 = z[3];
    for (int i = 0; i < n; i++) {
        int16_t v = op1_sat16((b0 * x[i] + b1 * x1 + b2 * x2 + a1 * y1 + a2 * y2) >> 14);
        x2 = x1; x1 = x[i]; y2 = y1; y1 = v; x[i] = v;
    }
    z[0] = x1; z[1] = x2; z[2] = y1; z[3] = y2;
}
static inline int32_t softclip(int32_t x)                          /* x (3 - x^2) / 2 in Q15, |x| <= 1 */
{
    x = x > 32767 ? 32767 : (x < -32767 ? -32767 : x);
    int32_t x2 = x * x >> 15;
    return x * (98304 - x2) >> 16;
}

static uint8_t early[3];                       /* a VERB / FEX build runs in the first clearing call */
static uint8_t steady[3];                      /* calls the new selection has held (a fast turn waits) */
static uint8_t built[3];                       /* a program is built (outside the cleared state, as phase) */
static void start(rv_state *s, int p, int unit)       /* clear the unit's state: the ring or QUAD's lines */
{
    phase[p % 3] = 2;
    early[p % 3] = unit != U_QUAD;
    qd_state *q = unit == U_QUAD ? qd_of(p) : 0;
    bigclr[p % 3].p = (uint32_t *)q; bigclr[p % 3].left = q ? RV_QD_CLEAR / 4 : 0;
    if (unit == U_QUAD) fx_spread_start(&spread[p % 3], 0, 0);
    else fx_spread_start(&spread[p % 3], s->u.mv.ring, sizeof s->u.mv.ring / 4);   /* the program is built, not cleared */
}

static void build(rv_state *s, int p, int unit, int prog, int mode)
{
    for (uint8_t *b = (uint8_t *)&s->unit; b < (uint8_t *)&s->home; b++) *b = 0;   /* the wrapper's state */
    s->unit = (int8_t)unit; s->prog = (int8_t)prog; s->mode = (int8_t)mode;
    if (unit == U_QUAD) { qd_state *q = qd_of(p); if (q) qd_select(q, prog); else s->defeat = 1; s->inc = 0; return; }
    const rv_row_t *r = row_of(unit, prog);
    int32_t fs2 = unit == U_VERB2 ? 62500 : 46875;
    s->u.mv.acc = 0; s->u.mv.n = 0;
    if (rv_mv_build(&s->u.mv.prog, r, fs2) < 0 || r->shape == RV_DEFEAT) s->defeat = 1;
    s->inc = unit == U_VERB2 ? 46440 : 34830;          /* fs * 65536 / 44100 */
    s->rinc = (int32_t)(0x80000000u / (uint32_t)s->inc);   /* 2^31 / inc */
}

/* CUTOFF and the FEEDBACK sum on the unit's own rate: wet l, r (n samples) in place */
static void wet_tail(rv_state *s, int16_t *l, int16_t *r, int n, int32_t a)
{
    int16_t zl = s->lp[0], zr = s->lp[1];
    for (int j = 0; j < n; j++) {
        zl = (int16_t)(zl + ((a * (l[j] - zl)) >> 15));
        zr = (int16_t)(zr + ((a * (r[j] - zr)) >> 15));
        l[j] = zl; r[j] = zr;
        s->fb[j] = (int16_t)softclip((zl + zr) >> 1);
    }
    for (int j = n; j < 64; j++) s->fb[j] = n ? s->fb[n - 1] : 0;
    s->lp[0] = zl; s->lp[1] = zr;
}

/* one call of a VERB / FEX unit: x[64] (44.1 kHz, mono) -> wet l, r at 44.1 kHz */
static void run_mv(rv_state *s, rv_rows *rw, const int16_t *x, int32_t fbk, int32_t cut, int16_t *wl, int16_t *wr)
{
    const int16_t *aa = rv_aa[s->mode ? (s->unit == U_VERB2 ? 2 : 1) : 0];
    int16_t *xin = rw->u.mv.xin, *tin = rw->u.mv.tin, *tl = rw->u.mv.tl, *tr = rw->u.mv.tr;
    xin[0] = s->xprev;
    for (int i = 0; i < 64; i++) xin[i + 1] = x[i];
    bq_row(s->aa[0], aa, xin + 1, 64);
    s->xprev = xin[64];
    /* the machine samples due in this call: the input at each instant, by linear interpolation */
    int32_t ph = s->ph, inc = s->inc, rinc = s->rinc, n = 0;
    uint8_t mode = s->mode;
    for (int i = 1; i <= 64; i++) {
        ph += inc;
        if (ph < 65536 || n >= RV_MAXTICK) continue;
        ph -= 65536;
        int32_t back = (int32_t)((uint32_t)ph * (uint32_t)rinc >> 16);   /* before xin[i], 44.1 kHz samples, Q15 */
        int32_t xi = xin[i] - ((xin[i] - xin[i - 1]) * back >> 15) + (s->fb[n] * fbk >> 15);
        xi = op1_sat16(xi);
        tin[n++] = (int16_t)(mode ? (xi >> 2) : (xi >> 4) * 2);   /* CLEAN 14 bits, AUTH 12 bits << 1 */
    }
    rv_mv_run(&s->u.mv, tin, tl, tr, n);
    rv_mv_lfo(&s->u.mv.prog, n);
    int32_t lim = mode ? 16383 : 4095, sh = mode ? 1 : 3;   /* the DAC: CLEAN 15 bits, AUTH 13 */
    for (int j = 0; j < n; j++) {
        int32_t a = tl[j], b = tr[j];
        a = a > lim ? lim : (a < -lim - 1 ? -lim - 1 : a);
        b = b > lim ? lim : (b < -lim - 1 ? -lim - 1 : b);
        tl[j] = (int16_t)(a * (1 << sh)); tr[j] = (int16_t)(b * (1 << sh));
    }
    wet_tail(s, tl, tr, n, cut);
    /* back to 44.1 kHz: between the last two machine samples (one machine sample late), then the analog filter */
    int32_t ph2 = s->ph;
    int16_t y0l = s->y0[0], y0r = s->y0[1], y1l = s->y1[0], y1r = s->y1[1];
    int k = 0;
    for (int i = 0; i < 64; i++) {
        ph2 += inc;
        if (ph2 >= 65536 && k < n) { ph2 -= 65536; y0l = y1l; y0r = y1r; y1l = tl[k]; y1r = tr[k]; k++; }
        int32_t f = ph2 >> 1;                             /* Q15 */
        wl[i] = (int16_t)(y0l + ((y1l - y0l) * f >> 15));
        wr[i] = (int16_t)(y0r + ((y1r - y0r) * f >> 15));
    }
    bq_row(s->aa[1], aa, wl, 64);
    bq_row(s->aa[2], aa, wr, 64);
    s->ph = ph2;
    s->y0[0] = y0l; s->y0[1] = y0r; s->y1[0] = y1l; s->y1[1] = y1r;
}

/* QUAD at the FX call's own rate: the wet straight from rv_quad.c, CUTOFF and the FEEDBACK sum at 44.1 kHz */
static void run_quad(rv_state *s, qd_state *q, rv_rows *rw, int32_t size, int32_t fbk, int32_t cut, int16_t *wl, int16_t *wr)
{
    int16_t *x = rw->x;
    if (!q) { for (int i = 0; i < 64; i++) wl[i] = wr[i] = 0; return; }
    for (int i = 0; i < 64; i++) x[i] = op1_sat16(x[i] + (s->fb[i] * fbk >> 15));   /* in place: x is not read again */
    qd_render_w(q, &rw->u.q, x, wl, wr, size);
    wet_tail(s, wl, wr, 64, cut);
}

static void init(int p) { rv_state *s = state(p); if (s) { fx_spread_start(&spread[p % 3], s, __builtin_offsetof(rv_state, home) / 4); bigclr[p % 3].left = 0; phase[p % 3] = 2; early[p % 3] = 0; built[p % 3] = 0; } valid[p % 3] = 0; }

static void render(int p, int16_t *l, int16_t *r, const int16_t *params)
{
    rv_state *s = state(p);
    if (!s) return;
    p %= 3;
    for (int i = 0; i < 4; i++)                          /* the range conversion divides: once per changed word */
        if (!valid[p] || raw[p][i] != params[i]) { raw[p][i] = params[i]; q15[p][i] = op1_fx_param_q15(params, i); }
    valid[p] = 1;
    int unit = (params[7] < 0 ? 0 : params[7]) * U_N >> 15, mode = (params[6] < 0 ? 0 : params[6]) * 2 >> 15;
    int32_t size = params[5] > 0 ? params[5] : 32767;
    int prog = (int)(q15[p][3] * RV_PROG_STEPS >> 15);
    if (unit >= U_N) unit = U_N - 1;
    if (prog >= U_COUNT[unit]) prog = U_COUNT[unit] - 1;
    rv_live[p][0] = (int16_t)unit; rv_live[p][1] = (int16_t)(prog + U_FIRST[unit]); rv_live[p][2] = (int16_t)mode;
    rv_live[p][3] = (int16_t)size;
    rv_name_live[p] = rv_name(unit, prog);
    op1_fx_wrap[p][3] = U_COUNT[unit];                 /* PROGRAM turns round within the unit's list (core: kn_coarse.h) */

    if (phase[p] == 2) {                               /* dry while clearing */
        if (early[p]) { build(s, p, unit, prog, mode); s->fade = 0; early[p] = 0; built[p] = 1; return; }   /* the generator alone */
        if (s->unit != U_QUAD && s->u.mv.prog.size_q15 < 0) { rv_mv_size(&s->u.mv.prog, size); return; }   /* then p[] */
        if (fx_spread_step(&spread[p]) || bigclr_step(p)) return;
        phase[p] = built[p] && s->unit == unit && s->prog == prog && s->mode == mode && unit != U_QUAD ? 0 : 3;
        if (!phase[p]) return;
    }
    if (phase[p] == 3) {                               /* the build's calls stay dry */
        if (!built[p] || s->unit != unit || s->prog != prog || s->mode != mode || s->unit == U_QUAD) {
            build(s, p, unit, prog, mode); s->fade = 0; built[p] = 1;
            if (unit == U_QUAD) phase[p] = 0;
            return;
        }
        if (s->u.mv.prog.size_q15 < 0) { rv_mv_size(&s->u.mv.prog, size); phase[p] = 0; return; }
        phase[p] = 0;
    }
    if (phase[p] == 0 && (unit != s->unit || prog != s->prog || mode != s->mode)) {
        if (++steady[p] >= 6) { phase[p] = 1; steady[p] = 0; }   /* held for 6 calls (9 ms): a fast turn doesn't rebuild */
    } else steady[p] = 0;

    rv_rows *rw = (rv_rows *)op1_l1_claim(op1_l1_fx_part(p), &l1_owner[p], sizeof(rv_rows), 0);
    if (!rw) rw = &s->home;                               /* no partition this call (no pool, or another FX holds it) */
    int16_t *x = rw->x;
    int32_t fbk = q15[p][0] * rv_fbceil[s->unit & 3][s->prog] >> 15;   /* the program's own ceiling (rv_fbceil.h) */
#ifdef RV_FBCEIL_SEARCH
    if (rv_fb_override >= 0) fbk = rv_fb_override;
#endif
    for (int i = 0; i < 64; i++) x[i] = r ? (int16_t)((l[i] + r[i]) >> 1) : l[i];
    if (s->unit == U_VERB2 && s->u.mv.prog.trig) {      /* triggered flange: the sweep restarts on an attack */
        int32_t pk = 0;
        for (int i = 0; i < 64; i++) { int32_t a = x[i] < 0 ? -x[i] : x[i]; if (a > pk) pk = a; }
        if (pk > s->env) s->env = pk; else s->env -= s->env >> 5;
        if (s->armed && pk > 4000) { rv_mv_trigger(&s->u.mv.prog); s->armed = 0; }
        if (s->env < 1000) s->armed = 1;
    }
    /* CUTOFF's one-pole at the unit's rate */
    const int16_t *ct = rv_cut[s->unit == U_VERB2 ? 1 : (s->unit == U_QUAD ? 2 : 0)];
    int32_t ci = q15[p][1] * 32, cf = ci & 32767; ci >>= 15;
    int32_t a = ci >= 32 ? ct[32] : ct[ci] + ((ct[ci + 1] - ct[ci]) * cf >> 15);
    int16_t *wl = rw->wl, *wr = rw->wr;
    if (s->unit == U_QUAD) run_quad(s, qd_of(p), rw, size, fbk, a, wl, wr);
    else {
        rv_mv_size(&s->u.mv.prog, size);
#ifdef __bfin__
        /* rv_kern.S zeroes L0..L2; the caller's values come back afterwards, as father_kern.S does */
        uint32_t l0, l1, l2;
        __asm__ volatile("%0 = L0;\n\t%1 = L1;\n\t%2 = L2;" : "=d"(l0), "=d"(l1), "=d"(l2));
        run_mv(s, rw, x, fbk, a, wl, wr);
        __asm__ volatile("L0 = %0;\n\tL1 = %1;\n\tL2 = %2;" :: "d"(l0), "d"(l1), "d"(l2));
#else
        run_mv(s, rw, x, fbk, a, wl, wr);
#endif
    }

    /* the fade and the MIX */
    int32_t mi = q15[p][2] * 32, mf = mi & 32767; mi >>= 15;
    int32_t gd = mi >= 32 ? rv_mixd[32] : rv_mixd[mi] + ((rv_mixd[mi + 1] - rv_mixd[mi]) * mf >> 15);
    int32_t gw = mi >= 32 ? rv_mixw[32] : rv_mixw[mi] + ((rv_mixw[mi + 1] - rv_mixw[mi]) * mf >> 15);
    int32_t f0 = s->fade, f1 = phase[p] == 1 || s->defeat ? 0 : 32767, pl = 0, pr = 0;
    if (f0 != f1 || f1 != 32767)                          /* fading (or muted): the wet's gain moves over the call */
        for (int i = 0; i < 64; i++) {
            int32_t g = f0 + ((f1 - f0) * i >> 6);
            wl[i] = (int16_t)(wl[i] * g >> 15); wr[i] = (int16_t)(wr[i] * g >> 15);
        }
    if (q15[p][2]) {                                      /* MIX 0: exactly dry */
        if (r)
            for (int i = 0; i < 64; i++) {
                l[i] = op1_sat16((l[i] * gd + wl[i] * gw) >> 15);
                r[i] = op1_sat16((r[i] * gd + wr[i] * gw) >> 15);
            }
        else
            for (int i = 0; i < 64; i++) l[i] = op1_sat16((l[i] * gd + ((wl[i] + wr[i]) >> 1) * gw) >> 15);
    }
    s->fade = (int16_t)f1;
    /* the screen's meter: the output's peaks, read after the mix (nothing rendered changes); mono: L on both */
    for (int i = 0; i < 64; i++) { int32_t a = l[i] < 0 ? -l[i] : l[i]; if (a > pl) pl = a; }
    if (r) for (int i = 0; i < 64; i++) { int32_t a = r[i] < 0 ? -r[i] : r[i]; if (a > pr) pr = a; }
    else pr = pl;
    s->peak = pl > s->peak ? pl : s->peak - (s->peak >> 4);
    peak_r[p] = pr > peak_r[p] ? pr : peak_r[p] - (peak_r[p] >> 4);
    rv_live[p][4] = (int16_t)(s->peak > 32767 ? 32767 : s->peak);
    rv_live[p][5] = (int16_t)(peak_r[p] > 32767 ? 32767 : peak_r[p]);
    if (phase[p] == 1) start(s, p, unit);                /* faded out: clear, then build */
}
static void mono(int p, int16_t *b, const int16_t *params) { render(p, b, 0, params); }
const char *rv_live_name(int p) { return rv_name_live[p % 3]; }

/* op1r: page-2 MODE / UNIT as OP1_FX_PAGE_STEPS (kennel: OP1_PRESS_PAGE_STEPS, which is a list knob for op1r FX) */
const op1_fx_stereo_t FX_rackverb = { { init, mono, { OP1_PRESS_PAGE, OP1_FX_PAGE_STEPS(2), OP1_FX_PAGE_STEPS(4),
                                                      OP1_FX_STEPS(RV_PROG_STEPS) } }, render };
