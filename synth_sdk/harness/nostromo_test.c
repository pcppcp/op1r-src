/* Host test for plugins/nostromo: the tables (layouts fit, knob mappings) and the parameter layer.
 *   mkdir -p build out && cc -O1 -fsanitize=address,undefined -DOP1_HOST -I.. nostromo_test.c -lm \
 *     -o build/nostromo_test && build/nostromo_test */
#include "../plugins/nostromo/nostromo.c"
#include "../plugins/reverb/reverb.c"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 44100
#define NEAR(a, b, tol) (fabs((double)(a) - (double)(b)) <= (tol) * fabs((double)(b)))
static int16_t K_DEF[8] = { 16645, 23494, 6553, 11468, 0, 17824, 1024, 15170 };    /* plugin.json defaults */
static int16_t clock_word(int c) { return (int16_t)(2048 * c + 1024); }      /* clock: HIFI, STD, LOFI, mid-step */
static int16_t prog_word(int p) { return (int16_t)((2 * p + 1) * 32768 / 54); }   /* program p + 1 */

static void t_tables(void)
{
    for (int r = 0; r < 2; r++)                                        /* per clock: 44.1, 22.05 kHz */
        for (int ty = 0; ty < 3; ty++) {
            assert(nt_base[r][ty][NT_LINES - 1] + nt_len[r][ty][NT_LINES - 1] + 1 == nt_used[r][ty] && nt_used[r][ty] <= NT_RING);
            for (int i = 0; i < NT_TAPS; i++) assert(nt_tap[r][ty][i] < nt_len[r][ty][nt_tap_line[i]]);
        }
    assert(nt_used[0][2] > 50000 && nt_used[0][0] < 21000 && nt_used[1][2] < 26000);   /* hall ~50.1k words, room ~20.1k */
    assert((NT_RING + NT_PD) * 2 <= OP1_FX_SCRATCH_BYTES && NT_LOFI_PD + nt_er_t[1][NT_ERS - 1] < NT_PD);
    for (int i = 0; i < NT_ERS; i++) assert(abs(2 * nt_er_t[1][i] - nt_er_t[0][i]) <= 1);   /* the reflections: half at 22.05 kHz */
    int32_t hb = 0;
    for (int j = 0; j < 8; j++) hb += abs(nt_hb[j]);
    assert(hb < 24576 && nt_hb[0] > 10000 && nt_hb[1] < 0);              /* the half-band's int32 bound */
    assert(nt_lut(nt_lofi_inc, 0) == 32768 && NEAR(nt_lut(nt_lofi_inc, 32767), 4096, .01));   /* LOFI: 22.05 .. 2.76 kHz */
    const int32_t floor_ms[3] = { 340, 550, 800 };                     /* mid decay: the type's floor .. 10 s */
    for (int ty = 0; ty < 3; ty++) assert(nt_lut(nt_rt[ty], 0) == floor_ms[ty] && NEAR(nt_lut(nt_rt[ty], 32767), 10000, .002));
    assert(NEAR(nt_lut(nt_rt[1], K_DEF[0]), 2400, .01));
    assert(NEAR(nt_lut(nt_rt[1], 17824), 2664, .01));                  /* bass: the mid's curve, its own word (2.7 s) */
    assert(nt_lut(nt_treb_hz, 0) == 500 && NEAR(nt_lut(nt_treb_hz, 32767), 16000, .002));
    assert(NEAR(nt_lut(nt_treb_hz, 23494), 6000, .01));
    for (int r = 0; r < 2; r++) assert(NEAR(nt_xo_b[r], 32768 * exp(-2 * M_PI * 700 / (r ? 22050 : 44100)), .0005));   /* crossover */
    assert(nt_exp2[0] == 32767 && NEAR(nt_exp2[128], 23170, .001));   /* 2^-0.5 */
    printf("ok tables\n");
}

static void t_params(void)
{
    int16_t k[8];
    memcpy(k, K_DEF, sizeof k);
    nt_par q;
    nt_params(k, &q);
    assert(q.clock == NT_HIFI && q.type == 1 && q.diff == 1 && q.mod == 0);   /* program 13: HIFI / plate / med / low */
    assert(NEAR(q.rt, 2400, .01) && q.rtb == nt_lut(nt_rt[1], 17824) && q.pd == 881 && q.mix == 11468);   /* bass: its own RT60 */
    double t = (double)(nt_len[0][1][4] + nt_len[0][1][5] + nt_len[0][1][6] + nt_len[0][1][7]) / SR;   /* half 1, s */
    assert(NEAR(q.gm[0], 32768 * pow(10, -3 * t / 2.4), .02));
    for (int c = 0; c < 3; c++)                                          /* every clock and program from their words */
        for (int p = 0; p < 27; p++) {
            k[6] = clock_word(c); k[7] = prog_word(p);
            nt_params(k, &q);
            assert(q.clock == c && q.type == p / 9 && q.diff == p / 3 % 3 && q.mod == p % 3);
            assert(q.lay == q.type + 3 * q.clock && q.rate == (q.clock != NT_HIFI));
            assert(q.pd == (q.clock == NT_LOFI ? NT_LOFI_PD : 881 >> q.rate));   /* pre-delay in tank samples */
            assert(q.bx == nt_xo_b[q.rate]);                                 /* the crossover: fixed */
        }
    const int16_t edge[8][2] = { { 0, 0 }, { 2047, 0 }, { 2048, 1 }, { 4095, 1 }, { 4096, 2 }, { 6143, 2 }, { 6144, 0 },
                                 { 32767, 0 } };   /* clock: a step every 2048, round and round */
    for (int j = 0; j < 8; j++) { k[6] = edge[j][0]; nt_params(k, &q); assert(q.clock == edge[j][1]); }
    k[5] = k[6] = k[7] = -1;                                             /* raw words below 0 count as 0 */
    nt_params(k, &q);
    assert(q.clock == 0 && q.type == 0 && q.diff == 0 && q.mod == 0 && q.rtb == nt_lut(nt_rt[0], 0));
    assert(q.bx == nt_xo_b[0]);
    k[0] = 32767; k[5] = 32767;                                          /* longest decays (room): below the cap, the bass */
    nt_params(k, &q);                                                    /* the mid's: one gain */
    for (int h = 0; h < 2; h++) assert(q.gm[h] + q.gb[h] <= NT_GMAX && q.gm[h] > 28000 && q.gb[h] == 0);   /* 10 s */
    assert(q.dd2 == 16384);
    k[0] = 16384;                                                        /* the bass longer than the mid, on its own */
    nt_params(k, &q);
    for (int h = 0; h < 2; h++) assert(q.gb[h] > 0 && q.gm[h] + q.gb[h] > 28000 && q.rtb == nt_lut(nt_rt[0], 32767));
    k[0] = 32767; k[5] = 0; nt_params(k, &q);                            /* and shorter */
    for (int h = 0; h < 2; h++) assert(q.gb[h] < 0 && q.rtb == 340);
    k[0] = 32767;
    k[0] = 0;                                                            /* shortest decay: near 0.25, within it */
    nt_params(k, &q);
    assert(q.dd2 >= 8192 && q.dd2 < 9000);
    printf("ok params\n");
}

/* firmware stand-ins; screen calls recorded per node id (the screen tests read them) */
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4];
#define MAXN 512
static char node_id[MAXN][8]; static uint8_t node_vis[MAXN]; static int32_t node_m[MAXN][9]; static int nnodes;
void *host_find_node(void *s, const char *id)
{
    (void)s;
    for (int i = 0; i < nnodes; i++) if (!strcmp(node_id[i], id)) return (void *)(intptr_t)(i + 1);
    assert(nnodes < MAXN && strlen(id) < 8);
    strcpy(node_id[nnodes], id); node_vis[nnodes] = 1;
    return (void *)(intptr_t)++nnodes;
}
void host_node_set_transform(void *n, const int32_t *m) { memcpy(node_m[(intptr_t)n - 1], m, 36); }
void host_node_set_visible(void *n, int on) { node_vis[(intptr_t)n - 1] = (uint8_t)(on != 0); }
static char style_id[128][8]; static uint8_t style_c[128][4]; static int nstyles;   /* recoloured nodes */
uint8_t *host_node_style_rgba(void *s, const char *id)
{
    (void)s;
    for (int i = 0; i < nstyles; i++) if (!strcmp(style_id[i], id)) return style_c[i];
    assert(nstyles < 128 && strlen(id) < 8);
    strcpy(style_id[nstyles], id); memset(style_c[nstyles], 0xFF, 4);
    return style_c[nstyles++];
}
static const uint8_t *style(const char *id) { for (int i = 0; i < nstyles; i++) if (!strcmp(style_id[i], id)) return style_c[i]; assert(0); return 0; }
static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } ln[1024]; static int nln;   /* the lines after the SVG */
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; nln = 0; }
void *host_raster(void) { return 0; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c)
{
    const op1_pt *p = a, *q = b;
    (void)r;
    assert(nln < 1024);
    ln[nln].x0 = p->x; ln[nln].y0 = p->y; ln[nln].x1 = q->x; ln[nln].y1 = q->y; ln[nln].w = w; memcpy(ln[nln].c, c, 4); nln++;
}

static int32_t scratch[3][OP1_FX_SCRATCH_BYTES / 4];
#define CLEAR_BLOCKS ((NT_RING + NT_PD + NT_CLEAR - 1) / NT_CLEAR)
#define BLOCKS(n) ((n) / OP1_FX_BLOCK * OP1_FX_BLOCK)
static int imax(int a, int b) { return a > b ? a : b; }

static void start(int pos, const int16_t *k)                  /* init, then the clear (silence in) */
{
    int16_t b[OP1_FX_BLOCK];
    nt_init(pos);
    for (int i = 0; i < CLEAR_BLOCKS; i++) { memset(b, 0, sizeof b); nt_render(pos, b, k); }
}
static void run(int pos, const int16_t *k, const int16_t *in, int16_t *out, int n)   /* n: whole blocks */
{
    for (int b = 0; b < n; b += OP1_FX_BLOCK) {
        memcpy(out + b, in + b, OP1_FX_BLOCK * 2);
        nt_render(pos, out + b, k);
    }
}

/* ---- the same algorithm in double: no saturation, no truncation but the converters', fixed settings ---- */
static struct {
    double ring[NT_RING], pd[NT_PD]; int wp, pw; double lp[2], xo[2], xo2[2], dc, lfo[2]; uint32_t ph[2];
    double hbx[32], hbw[16], hbo, hold, apk, dpk; int hbi, hbj, half, cvn, ask, dsk; uint32_t acc;
    uint32_t ws[2]; int32_t wt[2], wn[2], wv[2];
    double rg[256]; int rgi, blk;                                /* diffusion HIGH: the diffused input, a block back */
} fm;
static int fm_er = 1;                                            /* 0: the room without its reflections */
static double FQ(int32_t v) { return v / 32768.0; }
#define FR(i, off) fm.ring[(fm.wp + base[i] + (off)) & NT_MASK]
static double fm_ap(const int32_t *base, int i, int32_t len, double acc, double g)
{
    double tail = FR(i, len - 1);
    acc += tail * g; FR(i, 0) = acc;
    return acc * -g + tail;
}
static double fm_map(const int32_t *base, int i, int32_t len, double lfo, double amp, double acc)
{
    double pos = len + amp * lfo;
    int k = (int)pos;
    double a = FR(i, k), r = a + (FR(i, k + 1) - a) * (pos - k);
    acc -= r * FQ(NT_DD1); FR(i, 0) = acc;
    return acc * FQ(NT_DD1) + r;
}
static double fm_dl(const int32_t *base, int i, int32_t len, double acc) { FR(i, 0) = acc; return FR(i, len); }
static double fm_lp(double *y, double x, double b) { return *y = x + (*y - x) * b; }
static double fm_q12(double v, int s) { double q = 16 >> s; return v >= 0 ? floor(v / q) * q : -floor(-v / q) * q; }
static void fm_lfo(const nt_par *q, int r)                      /* the LFOs as the plugin's, in integers */
{
    for (int j = 0; j < 2; j++) {
        int32_t v;
        if (!q->mod) {                                           /* tank mod LOW: the wander */
            if (--fm.wn[j] <= 0) { fm.ws[j] = fm.ws[j] * 1103515245u + 12345u; fm.wt[j] = (int32_t)(fm.ws[j] >> 17); fm.wn[j] = nt_wander[r][j]; }
            v = fm.wv[j] += (fm.wt[j] - fm.wv[j]) >> nt_wander[r][2];
        } else {
            fm.ph[j] += (uint32_t)nt_mod[r][q->mod][j];
            v = (host_sine[fm.ph[j] >> 19] + 32768) >> 1;
        }
        fm.lfo[j] = v / 32768.0;
    }
}
static double fm_tick(const nt_par *q, double in)                /* one tank sample */
{
    const int r = q->rate;
    const int32_t *base = nt_base[r][q->type], *len = nt_len[r][q->type], *tap = nt_tap[r][q->type];
    double id0 = FQ(nt_diff[q->diff][0]), id1 = FQ(nt_diff[q->diff][1]), amp = nt_mod[r][q->mod][2] / 256.0;
    if (--fm.wp < 0) fm.wp += NT_RING;
    if ((fm.wp & 31) == 0) fm_lfo(q, r);
    fm.pd[fm.pw] = in + (q->diff == 2 ? FQ(NT_REGEN) * 2 * fm.rg[(fm.rgi - fm.blk) & 255] : 0);   /* HIGH: regenerated */
    double x = fm.pd[(fm.pw - q->pd + NT_PD) % NT_PD], er = 0;
    for (int i = 0; q->type == 0 && fm_er && i < NT_ERS; i++) {   /* room: the reflections */
        int d = q->pd + nt_er_t[r][i];
        er += fm.pd[(fm.pw - (d < NT_PD ? d : NT_PD - 1) + NT_PD) % NT_PD] * FQ(nt_er_g[i]);
    }
    for (int i = 0; q->type == 2 && q->diff == 0 && i < NT_SPS; i++) {   /* hall, LOW: the sparse taps */
        int d = q->pd + nt_sp_t[r][i];
        d = d < 0 ? 0 : (d < NT_PD ? d : NT_PD - 1);
        er += fm.pd[(fm.pw - d + NT_PD) % NT_PD] * FQ(nt_sp_g[i]);
    }
    if (++fm.pw == NT_PD) fm.pw = 0;
    if (r) { fm.apk = fmax(fm.apk, fabs(x)); x = fm_q12(x, fm.ask); }   /* the converters */
    double acc = x * 0.5;
    acc = fm_ap(base, 0, len[0], acc, id0); acc = fm_ap(base, 1, len[1], acc, id0);
    acc = fm_ap(base, 2, len[2], acc, id1); acc = fm_ap(base, 3, len[3], acc, id1);
    double apout = acc;
    fm.rg[fm.rgi++ & 255] = apout;
    acc += FR(11, len[11]);
    acc = fm_map(base, 4, len[4], fm.lfo[1], amp, acc);
    acc = fm_dl(base, 5, len[5], acc);
    acc = fm_lp(&fm.lp[0], acc, FQ(q->bt));
    acc = acc * FQ(q->gm[0]) + fm_lp(&fm.xo2[0], fm_lp(&fm.xo[0], acc, FQ(q->bx)), FQ(q->bx)) * FQ(q->gb[0]);
    acc = fm_ap(base, 6, len[6], acc, FQ(q->dd2));
    acc = fm_dl(base, 7, len[7], acc) + apout;
    acc = fm_map(base, 8, len[8], fm.lfo[0], amp, acc);
    acc = fm_dl(base, 9, len[9], acc);
    acc = fm_lp(&fm.lp[1], acc, FQ(q->bt));
    acc = acc * FQ(q->gm[1]) + fm_lp(&fm.xo2[1], fm_lp(&fm.xo[1], acc, FQ(q->bx)), FQ(q->bx)) * FQ(q->gb[1]);
    FR(11, 0) = fm_ap(base, 10, len[10], acc, FQ(q->dd2));
    double sum = 0;
    for (int i = 0; i < NT_TAPS; i++) sum += nt_tap_sign[i] * FR(nt_tap_line[i], tap[i]);
    double wet = sum * NT_WET_K / 16384.0 + er;
    wet -= fm_lp(&fm.dc, wet, FQ(nt_dc_b[r]));
    if (r) {
        fm.dpk = fmax(fm.dpk, fabs(wet)); wet = fm_q12(wet, fm.dsk);
        if (++fm.cvn == 32) { fm.cvn = 0; fm.ask = nt_gstep((int32_t)fm.apk); fm.dsk = nt_gstep((int32_t)fm.dpk); fm.apk = fm.dpk = 0; }
    }
    return wet;
}
static void fm_run(const int16_t *k, const int16_t *in, double *out, int n)
{
    nt_par q;
    nt_params(k, &q);
    memset(&fm, 0, sizeof fm);
    fm.ask = fm.dsk = 4; fm.ws[0] = 1; fm.ws[1] = 2;
    fm.blk = q.clock == NT_HIFI ? 64 : q.clock == NT_STD ? 32 : (64 * q.inc + 32768) >> 16;   /* tank ticks a block */
    for (int t = 0; t < n; t++) {
        double wet;
        if (q.clock == NT_HIFI) wet = fm_tick(&q, in[t]);
        else if (q.clock == NT_STD) {                            /* the same half-band, in double */
            fm.hbx[fm.hbi++ & 31] = in[t];
            if ((fm.half ^= 1)) wet = fm.hbo;
            else {
                int c = fm.hbi - 16, m;
                double a = 0.5 * fm.hbx[c & 31], b = 0;
                for (int j = 0; j < 8; j++) a += FQ(nt_hb[j]) * (fm.hbx[(c - 2 * j - 1) & 31] + fm.hbx[(c + 2 * j + 1) & 31]);
                fm.hbw[fm.hbj++ & 15] = fm_tick(&q, a);
                m = fm.hbj - 9;
                for (int j = 0; j < 8; j++) b += FQ(nt_hb[j]) * (fm.hbw[(m - j) & 15] + fm.hbw[(m + 1 + j) & 15]);
                wet = fm.hbw[m & 15]; fm.hbo = 2 * b;
            }
        } else {                                                 /* LOFI: the same clock */
            if ((fm.acc += (uint32_t)q.inc) >= 65536u) { fm.acc -= 65536u; fm.hold = fm_tick(&q, in[t]); }
            wet = fm.hold;
        }
        out[t] = in[t] + (wet - in[t]) * FQ(q.mix);
    }
}

/* RT60 of h (T20 x 3, Schroeder's backward integral) in a band: 4 x one-pole high-pass at lo (0: none), 4 x
 * low-pass at hi. h is overwritten. */
static double edc_rt(double *h, int n, double lo, double hi)
{
    double a = exp(-2 * M_PI * lo / SR), b = exp(-2 * M_PI * hi / SR), hp[4] = { 0 }, s[4] = { 0 }, tot = 0;
    for (int i = 0; i < n; i++) {
        double x = h[i];
        if (lo > 0) for (int j = 0; j < 4; j++) { hp[j] = x + (hp[j] - x) * a; x -= hp[j]; }
        for (int j = 0; j < 4; j++) x = s[j] = x + (s[j] - x) * b;
        h[i] = x;
    }
    for (int i = n - 1; i >= 0; i--) { tot += h[i] * h[i]; h[i] = tot; }
    int t5 = -1, t25 = -1;
    for (int i = 0; i < n && t25 < 0; i++) {
        if (t5 < 0 && h[i] <= tot * 0.316228) t5 = i;
        if (h[i] <= tot * 0.00316228) t25 = i;
    }
    assert(t5 >= 0 && t25 > t5);
    return 3.0 * (t25 - t5) / SR;
}
static void to_d(const int16_t *x, double *h, int n) { for (int i = 0; i < n; i++) h[i] = x[i]; }
/* the excitation: 50 ms of noise, bass-weighted (one-pole at ~210 Hz), peak 16000. A single-sample impulse leaves
 * the tank at the 16-bit floor, where truncation shortens the decay. */
static void burst(int16_t *x, int n)
{
    static double b[2205];
    double y = 0, pk = 0;
    memset(x, 0, n * sizeof *x);
    srand(1);
    for (int i = 0; i < 2205; i++) { y += (rand() % 32001 - 16000 - y) * 0.03; b[i] = y; pk = fmax(pk, fabs(y)); }
    for (int i = 0; i < 2205; i++) x[i] = (int16_t)lrint(b[i] * 16000 / pk);
}

static void t_mod_depth(void)                                /* tank mod at 44.1 kHz: LOW wanders over 48 samples, MED sweeps 120, HIGH 28 */
{
    static int16_t in[OP1_FX_BLOCK], out[OP1_FX_BLOCK];
    const int want[3] = { 48, 120, 28 };
    for (int m = 0; m < 3; m++)
        for (int c = 0; c < 2; c++) {                                    /* HIFI, STD: the same depth in time */
            int16_t k[8];
            memcpy(k, K_DEF, sizeof k); k[6] = clock_word(c); k[7] = prog_word(12 + m);
            start(0, k);
            int32_t hi = 0;
            for (int b = 0; b < 8 * SR; b += OP1_FX_BLOCK) {
                run(0, k, in, out, OP1_FX_BLOCK);
                for (int j = 0; j < 2; j++) hi = imax(hi, nt[0].amp * nt[0].lfo[j] >> 8);
            }
            double d = hi / 32768.0 * (c ? 2 : 1);                     /* in 44.1 kHz samples */
            assert(fabs(d - want[m]) < (m ? 1.5 : 12));                /* LOW: the largest of its random targets */
        }
    printf("ok mod depth\n");
}

static void t_hifi(void)                                     /* HIFI's sound, as it was before the 1978 clocks */
{
    static int16_t ex[SR], out[SR];
    burst(ex, SR);
    uint32_t h = 2166136261u;                                /* FNV-1a over 12 programs: plate, hall; mod med, high */
    for (int ty = 1; ty < 3; ty++)
        for (int d = 0; d < 3; d++)
            for (int m = 1; m < 3; m++) {
                int16_t k[8] = { 16645, 23494, 6553, 16384, 0, 17824, clock_word(0), prog_word(9 * ty + 3 * d + m) };
                start(0, k); run(0, k, ex, out, BLOCKS(SR));
                const uint8_t *b = (const uint8_t *)out;
                for (int i = 0; i < BLOCKS(SR) * 2; i++) { h ^= b[i]; h *= 16777619u; }
            }
    printf("ok hifi (0x%08x)\n", h);
    assert(h == 0x4bd34655u);                                /* the HIFI path, bit for bit (bass its own RT60, diffusion after
                                                                the CXM 1978) */
}

static uint32_t fnv(uint32_t h, const int16_t *x, int n)  /* FNV-1a over the samples' bytes */
{
    const uint8_t *b = (const uint8_t *)x;
    for (int i = 0; i < n * 2; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static void t_exact(void)                                    /* every clock, size and tank mod, and knob changes: bit for bit */
{
    static const uint32_t want[28] = {   /* HIFI, STD, LOFI x room, plate, hall x LOW, MED, HIGH; then the changes */
        0x394a4fe4, 0x0b57b6a4, 0x0f5456e4, 0x1baa48a1, 0xc97166fc, 0x482e3eb0, 0x04cf0742,
        0x2d3258e5, 0xda611763, 0x6053434c, 0x1937f8a3, 0x1abd292a, 0x1f596150, 0x4ddbc9f2,
        0xd5b07709, 0xc4d908eb, 0xc6b2149e, 0xd4823f0d, 0xf7872749, 0xcc6af79a, 0xf2dedaf0,
        0x3f2361f4, 0x6868b2cc, 0x06a3ef03, 0xb8de61ac, 0x22ab2989, 0xa7fb78c3, 0x46043313 };
    static int16_t ex[SR], out[SR];
    burst(ex, SR);
    uint32_t got[28];
    int n = 0, seg = BLOCKS(SR / 8), bad = 0;
    for (int c = 0; c < 3; c++)
        for (int ty = 0; ty < 3; ty++)
            for (int m = 0; m < 3; m++) {
                int16_t k[8] = { 16645, 23494, c == 2 ? 16384 : 6553, 16384, 0, 17824, clock_word(c), prog_word(9 * ty + 3 + m) };
                start(0, k); run(0, k, ex, out, BLOCKS(SR));
                got[n++] = fnv(2166136261u, out, BLOCKS(SR));
            }
    int16_t k[8] = { 16645, 23494, 6553, 16384, 0, 17824, clock_word(0), prog_word(12) };
    uint32_t h = 2166136261u;
    start(0, k);
    for (int i = 0; i < 8; i++) {                            /* pre-delay, mix, decay, LOW to MED, hall, STD, LOFI */
        if (i == 1) k[2] = 30000; else if (i == 2) k[3] = 30000; else if (i == 3) k[0] = 26000;
        else if (i == 4) k[7] = prog_word(13); else if (i == 5) k[7] = prog_word(22);
        else if (i == 6) k[6] = clock_word(1); else if (i == 7) { k[6] = clock_word(2); k[2] = 9000; }
        run(0, k, ex, out, seg);
        h = fnv(h, out, seg);
    }
    got[n++] = h;
    for (int i = 0; i < n; i++) if (got[i] != want[i]) { printf("  t_exact %2d: 0x%08x\n", i, got[i]); bad = 1; }
    assert(!bad);
    printf("ok exact\n");
}

static void t_std_room(void)                                 /* STD keeps the wet above full scale until the mix */
{
    static int16_t a[SR * 2], h[SR * 2], oa[SR * 2], oh[SR * 2];
    static double v[SR * 2];
    int n = BLOCKS(SR * 2);
    double mx = 0;
    for (int i = 0; i < n; i++) {                            /* a loud pad for 1.5 s: wet peaks above full scale */
        double t = (double)i / SR;
        v[i] = 0;
        if (t < 1.5) for (int k = 1; k <= 6; k++) v[i] += (sin(2 * M_PI * 130.8 * k * t) + sin(2 * M_PI * 196 * k * t) + sin(2 * M_PI * 261.6 * k * t)) / k;
        mx = fmax(mx, fabs(v[i]));
    }
    for (int i = 0; i < n; i++) { a[i] = (int16_t)lrint(v[i] / mx * 23000); h[i] = (int16_t)(a[i] / 2); }
    for (int c = 0; c < 2; c++) {                            /* HIFI, STD: half the input, half the output */
        int16_t k[8] = { 16645, 23494, 6553, 11468, 0, 17824, clock_word(c), prog_word(12) };
        start(0, k); run(0, k, a, oa, n);
        start(0, k); run(0, k, h, oh, n);
        int worst = 0;
        for (int i = 0; i < n; i++) worst = imax(worst, abs(oa[i] - 2 * oh[i]));
        printf("  %s: loud against twice half: %d at most\n", c ? "STD" : "HIFI", worst);
        assert(worst < 1000);
    }
    printf("ok std room\n");
}

static void pad(int16_t *x, int n, int peak)                 /* a sustained chord for 1.5 s, then silence */
{
    static double v[SR * 3];
    double mx = 0;
    for (int i = 0; i < n; i++) {
        double t = (double)i / SR;
        v[i] = 0;
        if (t < 1.5) for (int k = 1; k <= 6; k++) v[i] += (sin(2 * M_PI * 130.8 * k * t) + sin(2 * M_PI * 196 * k * t) + sin(2 * M_PI * 261.6 * k * t)) / k;
        mx = fmax(mx, fabs(v[i]));
    }
    for (int i = 0; i < n; i++) x[i] = (int16_t)lrint(v[i] / mx * peak);
}
static void t_knee(void)                                     /* the output bends above -1.2 dBFS instead of clipping */
{
    static int16_t a[SR * 2], o[SR * 2];
    int n = BLOCKS(SR * 2), flat = 0, pk = 0;
    pad(a, n, 29000);
    int16_t k[8] = { 26997, 23494, 6553, 22937, 0, 17824, clock_word(0), prog_word(21) };   /* hall, 6 s, mix 70 % */
    start(0, k); run(0, k, a, o, n);
    for (int i = 0; i < n; i++) { flat += o[i] == 32767 || o[i] == -32768; pk = imax(pk, abs(o[i])); }
    printf("  knee: %d samples at full scale, peak %d\n", flat, pk);
    assert(!flat && pk > 24576);
    printf("ok knee\n");
}
static void t_headroom(void)                                 /* reverb on reverb: the second tank's lines never saturate */
{
    static int16_t a[SR * 3], b[SR * 3];
    int n = BLOCKS(SR * 3), sat = 0;
    pad(a, n, 29000);
    int16_t k[8] = { 26997, 23494, 6553, 22937, 0, 17824, clock_word(0), prog_word(21) };   /* hall, 6 s, mix 70 %, twice */
    start(0, k); start(2, k);
    for (int t = 0; t < n; t += OP1_FX_BLOCK) {
        memcpy(b + t, a + t, OP1_FX_BLOCK * 2);
        nt_render(0, b + t, k);                              /* an instrument's FX, then the master's on its output */
        nt_render(2, b + t, k);
        if (t % (OP1_FX_BLOCK * 64) == 0)
            for (int i = 0; i < NT_RING; i++) sat += nt[2].ring[i] == 32767 || nt[2].ring[i] == -32768;
    }
    printf("  headroom: %d saturated words seen in the master's tank\n", sat);
    assert(!sat);
    printf("ok headroom\n");
}

static void hits(int16_t *x, int n, int peak)                /* drum-like: decaying noise hits, 4 a second */
{
    uint32_t sd = 3;
    for (int i = 0; i < n; i++) {
        double e = exp(-(i % (SR / 4)) / 900.0);
        sd = sd * 1103515245u + 12345u;
        double v = (int32_t)(sd >> 1) / 1073741824.0 * e;
        x[i] = (int16_t)lrint(fmax(-1, fmin(1, v)) * peak);
    }
}
static void t_hot(void)                                      /* hot input: the mix takes the wet in full, STD's pre-delay */
{                                                            /* holds the decimator's overshoot, dry peaks pass untouched */
    static int16_t a[SR * 2], h[SR * 2], oa[SR * 2], oh[SR * 2];
    int n = BLOCKS(SR * 2);
    pad(a, n, 31000);                                        /* -0.5 dBFS: the wet runs 2.3x full scale from the dry */
    for (int i = 0; i < n; i++) h[i] = (int16_t)(a[i] / 2);
    for (int c = 0; c < 2; c++) {
        int16_t k[8] = { 16645, 23494, 6553, 11468, 0, 17824, clock_word(c), prog_word(12) };   /* plate, mix 35 % */
        start(0, k); run(0, k, a, oa, n);
        start(0, k); run(0, k, h, oh, n);
        int worst = 0;
        for (int i = 0; i < n; i++) worst = imax(worst, abs(oa[i] - 2 * oh[i]));
        printf("  hot pad, %s: loud against twice half: %d at most\n", c ? "STD" : "HIFI", worst);
        assert(worst < 100);
    }
    hits(a, n, 32767);                                       /* STD: full-scale hits overshoot the decimator by 20 % */
    int16_t k[8] = { 16645, 23494, 6553, 11468, 0, 17824, clock_word(1), prog_word(12) };
    start(0, k); run(0, k, a, oa, n);
    int sat = 0;
    for (int i = 0; i < NT_PD; i++) sat += nt[0].pd[i] == 32767 || nt[0].pd[i] == -32768;
    printf("  hits, STD: %d saturated words in the pre-delay\n", sat);
    assert(!sat);
    for (int i = 0; i < n; i++) a[i] = (int16_t)lrint(27852 * sin(2 * M_PI * 440.0 * i / SR));   /* -1.4 dBFS, dry */
    int16_t kd[8] = { 16645, 23494, 6553, 0, 0, 17824, clock_word(0), prog_word(12) };
    start(0, kd); run(0, kd, a, oa, n);
    assert(!memcmp(a, oa, n * 2));
    printf("ok hot\n");
}

static void t_into_room(void)                                /* into a room: its reflections fade in, no click */
{
    static int16_t a[SR * 3], o[SR * 3];
    int n = BLOCKS(SR * 3), steady = 0, worst = 0;
    for (int i = 0; i < n; i++) { double t = (double)i / SR; a[i] = (int16_t)lrint(12000 * (sin(2 * M_PI * 130.8 * t) + sin(2 * M_PI * 196 * t)) / 2); }
    int16_t kr[8] = { 16645, 23494, 6553, 16384, 0, 17824, clock_word(0), prog_word(3) };   /* the room's own steps */
    start(0, kr); run(0, kr, a, o, n);
    for (int i = SR; i < n; i++) steady = imax(steady, abs(o[i] - o[i - 1]));
    for (int c = 0; c < 8; c++) {                            /* the step depends on the pad's phase at the restart */
        int16_t k[8] = { 16645, 23494, 6553, 16384, 0, 17824, clock_word(0), prog_word(12) };   /* plate, then room */
        int at = BLOCKS(SR + c * 1373);
        start(0, k);
        for (int t = 0; t < n; t += OP1_FX_BLOCK) {
            if (t == at) k[7] = prog_word(3);
            memcpy(o + t, a + t, OP1_FX_BLOCK * 2); nt_render(0, o + t, k);
        }
        for (int i = at; i < n; i++) worst = imax(worst, abs(o[i] - o[i - 1]));
    }
    printf("  into a room: largest step %d (the room's own %d)\n", worst, steady);
    assert(worst < 2 * steady);
    printf("ok into room\n");
}

static void t_minors(void)                                   /* mix 0 passes full scale untouched; a hot tank bends, not clips */
{
    static int16_t a[SR * 3], o[SR * 3];
    int n = BLOCKS(SR * 2);
    for (int i = 0; i < n; i++) a[i] = (int16_t)lrint(32767 * sin(2 * M_PI * 440.0 * i / SR));   /* 0 dBFS, dry */
    int16_t kd[8] = { 16645, 23494, 6553, 0, 0, 17824, clock_word(0), prog_word(12) };
    start(0, kd); run(0, kd, a, o, n);
    assert(!memcmp(a, o, n * 2));
    n = BLOCKS(SR * 3);
    uint32_t sd = 11; double lp = 0;                         /* 0 dBFS dense noise into a 10 s hall, bass x2.5 */
    for (int i = 0; i < n; i++) {
        sd = sd * 1103515245u + 12345u;
        lp += ((int32_t)(sd >> 1) / 1073741824.0 - lp) * 0.2;
        a[i] = (int16_t)lrint(fmax(-1, fmin(1, lp * 2.2)) * 32767);
    }
    int16_t kh[8] = { 32767, 23494, 6553, 16384, 0, 32767, clock_word(0), prog_word(18) };
    start(0, kh); run(0, kh, a, o, n);
    int sat = 0;
    for (int i = 0; i < NT_RING; i++) sat += nt[0].ring[i] == 32767 || nt[0].ring[i] == -32768;
    printf("  hot tank: %d words at full scale in the lines\n", sat);
    assert(!sat);
    printf("ok minors\n");
}

static void t_std_hot(void)                                  /* STD's interpolator holds a hot tank's wet: no 2x ceiling */
{
    static int16_t a[SR * 3], o[SR * 3];
    static double v[SR * 3];
    int n = BLOCKS(SR * 3);
    double mx = 0;
    const double f[3] = { 130.81, 164.81, 196.0 };            /* a saw chord at 0 dBFS, as a hot OP-1 synth */
    for (int i = 0; i < n; i++) {
        double t = (double)i / SR;
        v[i] = 0;
        if (t < 2.5) for (int c = 0; c < 3; c++) v[i] += 2 * fmod(f[c] * t, 1.0) - 1;
        mx = fmax(mx, fabs(v[i]));
    }
    for (int i = 0; i < n; i++) a[i] = (int16_t)lrint(v[i] / mx * 32767);
    int16_t k[8] = { 32767, 23494, 6553, 11468, 0, 32767, clock_word(1), prog_word(3) };   /* STD room, 10 s, bass 10 s, mix 35 % */
    start(0, k);
    int32_t top = 0;
    for (int t = 0; t < n; t += OP1_FX_BLOCK) {
        memcpy(o + t, a + t, OP1_FX_BLOCK * 2); nt_render(0, o + t, k);
        for (int i = 0; i < 16; i++) top = imax(top, abs(nt[0].hbw[i]));
    }
    printf("  STD history: %.2fx full scale at most\n", top / 32768.0);
    assert(top > 65535);
    assert(nt_hb[0] < 16384);                                /* nt_hbi's bounds: tap 1 split, the rest < 2^12 */
    for (int i = 1; i < 8; i++) assert(abs(nt_hb[i]) < 4096);
    int32_t h[16];                                           /* the interpolation sum: exact up to 8x full scale */
    uint32_t sd = 9;
    for (int r = 0; r < 20000; r++) {
        for (int i = 0; i < 16; i++) { sd = sd * 1103515245u + 12345u; h[i] = (int32_t)(sd % 524287) - 262143; }
        int64_t want = 0;
        for (int i = 0; i < 8; i++) want += (int64_t)nt_hb[i] * (h[(7 - i) & 15] + h[(8 + i) & 15]);
        assert(nt_hbi(h, 7) == want);
    }
    printf("ok std hot\n");
}

static void t_float_match(void)                              /* every program: fixed point against the model */
{
    static int16_t ex[SR * 3], fx[SR * 3];
    static double fl[SR * 3], h[SR * 3];
    int n = BLOCKS(SR * 3);
    burst(ex, SR * 3);
    for (int p = 0; p < 81; p++) {
        int16_t k[8] = { K_DEF[0], 32767, 0, 32767, 0, 17824, clock_word(p / 27), prog_word(p % 27) };   /* bright, wet, no pre-delay */
        start(0, k); run(0, k, ex, fx, n);
        fm_run(k, ex, fl, n);
        int on = 0;                                          /* 50 ms windows from 20 ms after the onset: within */
        while (on < n && fabs(fl[on]) < 2) on++;             /* 2 dB (LOFI's 4096-sample pre-delay comes first) */
        for (int w = 0; w < 20; w++) {
            double ex = 0, ef = 0;
            for (int i = on + 882 + w * 2205; i < on + 882 + (w + 1) * 2205; i++) { ex += (double)fx[i] * fx[i]; ef += fl[i] * fl[i]; }
            double db = 10 * log10((ex + 1) / (ef + 1));
            if (fabs(db) > 2) { printf("program %d window %d: %+.2f dB\n", p + 1, w, db); assert(0); }
        }
        for (int band = 0; band < 2; band++) {               /* RT60 below the crossover / mid, within 10 % */
            double lo = band ? 1000 : 0, hi = band ? 3000 : 175, rx, rf;
            to_d(fx, h, n); rx = edc_rt(h, n, lo, hi);
            memcpy(h, fl, n * sizeof *h); rf = edc_rt(h, n, lo, hi);
            if (!NEAR(rx, rf, .10)) { printf("program %d band %d: RT60 %.2f s, model %.2f s\n", p + 1, band, rx, rf); assert(0); }
        }
    }
    printf("ok float model\n");
}

static void t_rt60(void)                                     /* the knobs give the RT60s they promise */
{
    static int16_t ex[SR * 32], out[SR * 32];
    static double h[SR * 32];
    burst(ex, SR * 32);
    const double lofi = 32768.0 / nt_lut(nt_lofi_inc, 16384);   /* LOFI at the white knob's middle: x2.83 */
    struct { int16_t k0, k2, w5; int p; double mid, bass, tm, tb; } c[] = {   /* tank mod MED: HIFI plate; room, hall (p: clock * 27 + program) */
        { 6757, 0, 6757, 13, 1.0, 1.0, .1, .1 }, { 16645, 0, 16645, 13, 2.4, 2.4, .1, .15 },   /* the lows: as exact arithmetic, */
        { 26997, 0, 26997, 13, 6.0, 6.0, .1, .1 }, { 16645, 0, 18706, 13, 2.4, 2.88, .1, .15 }, /* up to 14 % over the knob; the */
        { 16645, 0, 26997, 13, 2.4, 6.0, .1, .1 },                                                  /* bass word: its own RT60 */
        { 16645, 0, 8815, 13, 2.4, 1.2, .15, .2 }, /* bass under mid: the two-pole shelf's phase lifts the lows' gain */
        { 18940, 0, 18940, 4, 2.4, 2.4, .1, .15 }, { 14255, 0, 14255, 22, 2.4, 2.4, .1, .15 },  /* room: few low modes; hall: MED moves its lows */
        { 16645, 0, 16645, 40, 2.4, 2.4, .1, .15 }, { 18940, 0, 18940, 31, 2.4, 2.4, .1, .15 },  /* STD */
        { 14255, 0, 14255, 49, 2.4, 2.4, .1, .15 },
        { 16645, 16384, 16645, 67, 2.4 * lofi, 2.4 * lofi, .1, .1 } };    /* LOFI: the decay stretches */
    for (unsigned j = 0; j < sizeof c / sizeof *c; j++) {
        int16_t k[8] = { c[j].k0, 32767, c[j].k2, 32767, 0, c[j].w5, clock_word(c[j].p / 27), prog_word(c[j].p % 27) };
        int n = BLOCKS((int)((3 * fmax(c[j].mid, c[j].bass) + 1) * SR));
        start(0, k); run(0, k, ex, out, n);
        double rm, rb;
        to_d(out, h, n); rm = edc_rt(h, n, 1000, 3000);
        to_d(out, h, n); rb = edc_rt(h, n, 0, 175);
        printf("  RT60 mid %.2f (%.2f) bass %.2f (%.2f) s\n", rm, c[j].mid, rb, c[j].bass);
        assert(NEAR(rm, c[j].mid, c[j].tm) && NEAR(rb, c[j].bass, c[j].tb));
    }
    const int16_t ends[4][2] = { { 0, 0 }, { 1, 0 }, { 2, 0 }, { 1, 32767 } };   /* the range ends: what the knob claims */
    for (int j = 0; j < 4; j++) {
        int16_t k[8] = { ends[j][1], 32767, 0, 32767, 0, ends[j][1], clock_word(0), prog_word(4 + 9 * ends[j][0]) };
        double claim = nt_lut(nt_rt[ends[j][0]], ends[j][1]) / 1000.0, rm;
        int n = BLOCKS((int)((3 * claim + 1) * SR));
        start(0, k); run(0, k, ex, out, n);
        to_d(out, h, n); rm = edc_rt(h, n, 1000, 3000);
        printf("  RT60 at the range end, type %d: %.2f (%.2f) s\n", ends[j][0], rm, claim);
        assert(NEAR(rm, claim, ends[j][0] ? .1 : .2));        /* the room's floor: 0.30 .. 0.40 s by setting */
    }
    printf("ok rt60\n");
}

static void t_predelay(void)                                 /* the onset moves by exactly the pre-delay */
{
    static int16_t imp[SR], out[SR];
    memset(imp, 0, sizeof imp); imp[0] = 16384;
    int on[2];
    const int16_t words[2] = { 0, 19000 };
    for (int j = 0; j < 2; j++) {
        int16_t k[8] = { 20814, 32767, words[j], 32767, 0, 14113, clock_word(0), prog_word(12) };
        start(0, k); run(0, k, imp, out, BLOCKS(SR));
        on[j] = 1;
        while (on[j] < BLOCKS(SR) && out[on[j]] == 0) on[j]++;
    }
    assert(on[1] - on[0] == nt_pd(19000));
    const int hifi_on = on[0];
    for (int j = 0; j < 2; j++) {                            /* STD: the tank's samples are two of the output's */
        int16_t k[8] = { 20814, 32767, words[j], 32767, 0, 14113, clock_word(1), prog_word(12) };
        start(0, k); run(0, k, imp, out, BLOCKS(SR));
        for (on[j] = 1; on[j] < BLOCKS(SR) && abs(out[on[j]]) <= 8; on[j]++) {}
    }
    assert(on[1] - on[0] == 2 * (nt_pd(19000) >> 1));
    double ticks[2];
    const int16_t clocks[2] = { 0, 16384 };
    for (int i = 1; i < 16; i++) imp[i] = 16384;             /* sample and hold can miss one sample: 16 */
    for (int j = 0; j < 2; j++) {                            /* LOFI: 4096 tank samples at its clock */
        int16_t k[8] = { 20814, 32767, clocks[j], 32767, 0, 14113, clock_word(2), prog_word(12) };
        start(0, k); run(0, k, imp, out, BLOCKS(SR));
        for (on[j] = 1; on[j] < BLOCKS(SR) && abs(out[on[j]]) <= 8; on[j]++) {}
        ticks[j] = on[j] * (double)nt_lut(nt_lofi_inc, clocks[j]) / 65536;
    }
    printf("  LOFI onsets: %d, %d samples = %.1f, %.1f tank samples\n", on[0], on[1], ticks[0], ticks[1]);
    assert(fabs(ticks[0] - ticks[1]) <= 2 && ticks[0] >= NT_LOFI_PD && ticks[0] <= NT_LOFI_PD + 600);
    printf("ok predelay (onset %d samples, +%d; LOFI %.0f tank samples)\n", hifi_on, nt_pd(19000), ticks[0]);
}

static double hf(const int16_t *x, int n)                    /* power at 14 .. 20 kHz (Goertzel) */
{
    double p = 0;
    for (int f = 14000; f <= 20000; f += 1000) {
        double w = 2 * cos(2 * M_PI * f / SR), s1 = 0, s2 = 0;
        for (int i = 0; i < n; i++) { double s0 = x[i] + w * s1 - s2; s2 = s1; s1 = s0; }
        p += s1 * s1 + s2 * s2 - w * s1 * s2;
    }
    return p;
}
static void t_clocks(void)                                   /* STD: band-limited; the converters' 12 bits */
{
    static int16_t in[SR], a[SR], b[SR];
    int n = BLOCKS(SR);
    srand(3);
    for (int i = 0; i < n; i++) in[i] = i < 2205 ? (int16_t)(rand() % 32001 - 16000) : 0;   /* white, 50 ms */
    int16_t k[8] = { 16645, 32767, 0, 32767, 0, 14113, clock_word(0), prog_word(12) };
    start(0, k); run(0, k, in, a, n);
    k[6] = clock_word(1);
    start(0, k); run(0, k, in, b, n);
    double db = 10 * log10(hf(a, n) / hf(b, n));
    printf("  STD: %.1f dB less above 13.5 kHz than HIFI\n", db);
    assert(db >= 40);
    assert(nt_gstep(0) == 4 && nt_gstep(2047) == 4 && nt_gstep(2048) == 3 && nt_gstep(16383) == 1 && nt_gstep(32767) == 0);
    assert(nt_q12(1234, 4) == 1234 && nt_q12(1234, 0) == 1232 && nt_q12(-1234, 0) == -1232 && nt_q12(-15, 0) == 0);
    assert(nt_q12(15, 1) == 8 && nt_q12(-9, 1) == -8);
    printf("ok clocks\n");
}

static void t_room(void)                                     /* the room's reflections lift its first 50 ms */
{
    static int16_t ex[SR], fx[SR];
    static double fl[SR];
    int n = BLOCKS(SR);
    burst(ex, SR);
    int16_t k[8] = { 18940, 23494, 6553, 32767, 0, 17824, clock_word(0), prog_word(3) };   /* room, 20 ms pre-delay, wet */
    start(0, k); run(0, k, ex, fx, n);
    fm_er = 0; fm_run(k, ex, fl, n); fm_er = 1;
    double a = 0, b = 0;
    for (int i = SR * 24 / 1000; i < SR * 50 / 1000; i++) { a += (double)fx[i] * fx[i]; b += fl[i] * fl[i]; }
    printf("  room: reflections add %+.1f dB at 24 .. 50 ms\n", 10 * log10(a / b));
    assert(10 * log10(a / b) >= 3);
    printf("ok room\n");
}

static void t_mix(void)
{
    static int16_t in[SR], out[SR];
    for (int i = 0; i < SR; i++) in[i] = (int16_t)(rand() % 20001 - 10000);
    int16_t k[8];
    memcpy(k, K_DEF, sizeof k); k[3] = 0;                     /* mix 0: the input exactly */
    start(0, k); run(0, k, in, out, BLOCKS(SR));
    for (int i = 0; i < BLOCKS(SR); i++) assert(out[i] == in[i]);
    k[3] = 32767;                                            /* mix 100 %: no dry (while muted, no wet either) */
    nt_init(0);
    for (int j = 0; j < CLEAR_BLOCKS; j++) {
        int16_t b[OP1_FX_BLOCK];
        memcpy(b, in + j * OP1_FX_BLOCK, sizeof b);
        nt_render(0, b, k);
        for (int i = 0; i < OP1_FX_BLOCK; i++) assert(abs(b[i]) <= 1);
    }
    printf("ok mix\n");
}

static void t_clear(void)                                    /* selected over another FX's data */
{
    int16_t *m = (int16_t *)scratch[0];
    for (int i = 0; i < NT_RING + NT_PD; i++) m[i] = 0x5A5A;
    int16_t k[8];
    memcpy(k, K_DEF, sizeof k); k[3] = 32767;
    nt_init(0);
    int cleared = 0;
    for (int j = 0; j < CLEAR_BLOCKS; j++) {
        int16_t b[OP1_FX_BLOCK];
        for (int i = 0; i < OP1_FX_BLOCK; i++) b[i] = 10000;
        nt_render(0, b, k);
        int z = 0;
        for (int i = 0; i < NT_RING + NT_PD; i++) z += m[i] == 0;
        assert(z >= cleared && z - cleared <= NT_CLEAR);
        cleared = z;
        for (int i = 0; i < OP1_FX_BLOCK; i++) assert(abs(b[i]) <= 1);   /* muted */
    }
    assert(cleared == NT_RING + NT_PD);
    printf("ok clear\n");
}

static void t_positions(void)                                /* synth, drum and master do not meet */
{
    static int16_t in[SR], solo[SR], a[SR], z[SR];
    int n = BLOCKS(SR);
    for (int i = 0; i < n; i++) in[i] = (int16_t)(rand() % 20001 - 10000);
    start(0, K_DEF); run(0, K_DEF, in, solo, n);
    start(0, K_DEF); start(1, K_DEF); start(2, K_DEF);
    for (int b = 0; b < n; b += OP1_FX_BLOCK) {
        int16_t m[OP1_FX_BLOCK];
        memcpy(a + b, in + b, OP1_FX_BLOCK * 2); nt_render(0, a + b, K_DEF);
        memset(z + b, 0, OP1_FX_BLOCK * 2); nt_render(1, z + b, K_DEF);
        memcpy(m, in + b, sizeof m); nt_render(2, m, K_DEF);
    }
    assert(!memcmp(a, solo, n * 2));
    for (int i = 0; i < n; i++) assert(z[i] == 0);
    printf("ok positions\n");
}

/* a 440 Hz sine at -12 dBFS: 3 s with ka, then 3 s with kb (pre-delay swept from 0 if sweep); the largest
 * sample step after the first second; out kept in g_out */
static int16_t g_out[SR * 6];
static int steps(const int16_t *ka, const int16_t *kb, int sweep)
{
    static int16_t in[SR * 6];
    int n = BLOCKS(SR * 6), sw = BLOCKS(SR * 3), m = 0;
    for (int i = 0; i < n; i++) in[i] = (int16_t)lrint(8192 * sin(2 * M_PI * 440 * i / SR));
    start(0, ka);
    for (int b = 0; b < n; b += OP1_FX_BLOCK) {
        int16_t k[8];
        memcpy(k, b < sw ? ka : kb, sizeof k);
        if (sweep && b >= sw) k[2] = (int16_t)(32767LL * (b - sw) / (n - sw));
        memcpy(g_out + b, in + b, OP1_FX_BLOCK * 2);
        nt_render(0, g_out + b, k);
    }
    for (int i = SR + 1; i < n; i++) m = imax(m, abs(g_out[i] - g_out[i - 1]));
    return m;
}
static double rms(const int16_t *x, int a, int b) { double e = 0; for (int i = a; i < b; i++) e += (double)x[i] * x[i]; return sqrt(e / (b - a)); }

static void t_long_level(void)                               /* held sound into a long decay: no louder than at 2.4 s */
{
    static int16_t in[SR * 12], out[SR * 12];
    int n = BLOCKS(SR * 12);
    const double band[2][2] = { { 100, 400 }, { 500, 2000 } };
    for (int f = 0; f < 2; f++) {
        double lo = 0, hi = 0, a = exp(-2 * M_PI * band[f][0] / SR), b = exp(-2 * M_PI * band[f][1] / SR), lv[2];
        srand(3);
        for (int i = 0; i < n; i++) {                                               /* noise in the band, held 12 s */
            double v = rand() % 32001 - 16000;
            hi = v + (hi - v) * b; lo = hi + (lo - hi) * a;
            in[i] = (int16_t)lrint(hi - lo);
        }
        for (int j = 0; j < 2; j++) {
            int16_t k[8];
            memcpy(k, K_DEF, 16); k[3] = 32767; k[0] = j ? 32767 : K_DEF[0];      /* plate, wet only: 2.4 s, 10 s */
            start(0, k); run(0, k, in, out, n);
            lv[j] = rms(out, n - 2 * SR, n);                                        /* the last 2 s: steady */
        }
        printf("  held noise at %.0f .. %.0f Hz, wet: 10 s %+.1f dB against 2.4 s\n", band[f][0], band[f][1], 20 * log10(lv[1] / lv[0]));
        assert(lv[1] < lv[0] * 1.19);                                               /* within 1.5 dB */
    }
    printf("ok long level\n");
}

static double edc_at(const double *h, int n, double db)       /* s until the backward energy integral is db below its total */
{
    double tot = 0, e = 0;
    for (int i = 0; i < n; i++) tot += h[i] * h[i];
    for (int i = n - 1; i >= 0; i--) { e += h[i] * h[i]; if (e > tot * pow(10, -db / 10)) return (double)i / SR; }
    return 0;
}
static void t_quiet_tails(void)                              /* a quiet sound's tail decays as a loud one's, to the floor */
{
    static int16_t src[SR * 30], in[SR * 30], out[SR * 30];
    static double h[SR * 30], f[SR * 30];
    int16_t kp[8], kh[8];
    memcpy(kp, K_DEF, 16); kp[3] = 32767;                                            /* plate 2.4 s, wet only */
    memcpy(kh, kp, 16); kh[0] = 32767; kh[7] = prog_word(21);                        /* hall 10 s */
    const int16_t *ks[2] = { kp, kh };
    int n = BLOCKS(SR * 30);
    burst(src, n);
    for (int j = 0; j < 2; j++)
        for (int l = 0; l < 2; l++) {                                                /* the burst at -6, -26 dBFS */
            for (int i = 0; i < n; i++) in[i] = (int16_t)lrint(src[i] * (l ? 0.1 : 1));
            start(0, ks[j]); run(0, ks[j], in, out, n);
            fm_run(ks[j], in, f, n);
            to_d(out, h, n);
            double t = edc_at(h, n, 40), tf = edc_at(f, n, 40);
            printf("  %s at %d dBFS: 40 dB down at %.2f s (exact arithmetic: %.2f s)\n", j ? "hall 10 s" : "plate 2.4 s", l ? -26 : -6, t, tf);
            assert(fabs(t - tf) <= 0.15 * tf);                                       /* the loop's allpasses cut */
                                                                                     /* towards zero: no ringing on */
        }
    printf("ok quiet tails\n");
}

static void t_review(void)                                   /* rough handling: mixed clocks, program flips, LOFI jumps */
{
    static int16_t in[SR * 2], solo[SR * 2], a[SR * 2];
    int n = BLOCKS(SR * 2);
    for (int i = 0; i < n; i++) in[i] = (int16_t)lrint(16000 * sin(2 * M_PI * 220 * i / SR));
    int16_t ks[3][8], k[8];
    const int progs[3] = { 39, 57, 12 };                     /* STD plate, LOFI room, HIFI plate */
    for (int p = 0; p < 3; p++) { memcpy(ks[p], K_DEF, 16); ks[p][6] = clock_word(progs[p] / 27); ks[p][7] = prog_word(progs[p] % 27); }
    start(0, ks[0]); run(0, ks[0], in, solo, n);             /* position 0 alone, then beside the others */
    start(0, ks[0]); start(1, ks[1]); start(2, ks[2]);
    for (int b = 0; b < n; b += OP1_FX_BLOCK) {
        int16_t m1[OP1_FX_BLOCK], m2[OP1_FX_BLOCK];
        memcpy(a + b, in + b, OP1_FX_BLOCK * 2); nt_render(0, a + b, ks[0]);
        memcpy(m1, in + b, sizeof m1); nt_render(1, m1, ks[1]);
        memcpy(m2, in + b, sizeof m2); nt_render(2, m2, ks[2]);
    }
    assert(!memcmp(a, solo, n * 2));
    start(0, K_DEF);                                         /* the program flips every 4 blocks (STD, LOFI), */
    for (int b = 0; b < n; b += OP1_FX_BLOCK) {              /* LOFI's clock jumps end to end every block */
        memcpy(k, K_DEF, 16);
        k[6] = clock_word(b / 256 % 2 ? 1 : 2); k[2] = b / 64 % 2 ? 32767 : 0;
        memcpy(a + b, in + b, OP1_FX_BLOCK * 2); nt_render(0, a + b, k);
    }
    for (int b = 0; b < n; b += OP1_FX_BLOCK) {              /* then LOFI alone, its clock still jumping: alive */
        memcpy(k, K_DEF, 16); k[6] = clock_word(2); k[2] = b / 64 % 2 ? 32767 : 0; k[3] = 32767;
        memcpy(a + b, in + b, OP1_FX_BLOCK * 2); nt_render(0, a + b, k);
    }
    assert(rms(a, n / 2, n) > 100);
    printf("ok review\n");
}

static void t_changes(void)                                  /* no clicks; only a type change clears */
{
    int16_t a[8] = { 20814, 23494, 6553, 16384, 0, 17824, clock_word(0), prog_word(12) };   /* plate / med / low, 50 % */
    int16_t hall[8], modh[8], difh[8], wild[8] = { 30000, 4000, 25000, 26000, 0, 30000, clock_word(0), prog_word(26) };
    memcpy(hall, a, 16); hall[7] = prog_word(21);                                   /* hall / med / low */
    memcpy(modh, a, 16); modh[7] = prog_word(14);                                   /* plate / med / high */
    memcpy(difh, a, 16); difh[7] = prog_word(15);                                   /* plate / high / low */
    const int tol = 1024, base = steps(a, a, 0);
    assert(steps(a, hall, 0) <= imax(base, steps(hall, hall, 0)) + tol);           /* fade out, clear */
    assert(steps(a, modh, 0) <= imax(base, steps(modh, modh, 0)) + tol);
    assert(steps(a, difh, 0) <= imax(base, steps(difh, difh, 0)) + tol);
    assert(steps(a, wild, 0) <= imax(base, steps(wild, wild, 0)) + tol);           /* a sound switch */
    int16_t w0[8], w1[8], wm[8], wd[8], h0[8], h1[8];
    memcpy(w0, a, 16); w0[2] = 0; w0[3] = 32767;                                    /* wet only */
    memcpy(h0, a, 16); h0[2] = 0;                    /* pre-delay swept, at 50 %: at full wet the sine swells into */
    for (int m = 0; m < 3; m += 2) {                 /* the tank's modes as the delay moves and saturates */
        h0[7] = prog_word(12 + m);                                                  /* plate: tank mod LOW, HIGH */
        memcpy(h1, h0, 16); h1[2] = 32767;
        assert(steps(h0, h0, 1) <= imax(steps(h0, h0, 0), steps(h1, h1, 0)) + tol);
    }
    memcpy(wm, w0, 16); wm[7] = prog_word(14);
    memcpy(wd, w0, 16); wd[7] = prog_word(15);
    const int sw = BLOCKS(SR * 3);
    for (int j = 0; j < 2; j++) {                                                    /* diffusion, tank mod: no clear */
        steps(w0, j ? wd : wm, 0);
        assert(rms(g_out, sw, sw + 4410) > rms(g_out, sw - 4410, sw) / 2);
    }
    memcpy(hall, w0, 16); hall[7] = prog_word(21);                                   /* wet only: a type change clears */
    steps(w0, hall, 0);
    assert(rms(g_out, sw + 1024, sw + 2048) < rms(g_out, sw - 4410, sw) / 4);
    int16_t modm[8], r0[8], r1[8];
    memcpy(modm, a, 16); modm[7] = prog_word(13);                                   /* tank mod LOW <-> MED: a glide */
    assert(steps(a, modm, 0) <= imax(base, steps(modm, modm, 0)) + tol);
    assert(steps(modm, a, 0) <= imax(steps(modm, modm, 0), base) + tol);
    memcpy(r0, a, 16); r0[2] = 0; r0[7] = prog_word(3);                             /* a room's reflections swept */
    memcpy(r1, r0, 16); r1[2] = 32767;
    assert(steps(r0, r0, 1) <= imax(steps(r0, r0, 0), steps(r1, r1, 0)) + tol);
    int16_t std[8], lofi[8], lofi0[8], lofi1[8], sw0[8];
    memcpy(std, a, 16); std[6] = clock_word(1);                                      /* the clocks: no click */
    memcpy(lofi, a, 16); lofi[6] = clock_word(2);
    assert(steps(a, std, 0) <= imax(base, steps(std, std, 0)) + tol);
    assert(steps(std, lofi, 0) <= imax(steps(std, std, 0), steps(lofi, lofi, 0)) + tol);
    assert(steps(lofi, a, 0) <= imax(steps(lofi, lofi, 0), base) + tol);
    memcpy(lofi0, a, 16); lofi0[2] = 0; lofi0[6] = clock_word(2);                     /* LOFI's clock swept */
    memcpy(lofi1, lofi0, 16); lofi1[2] = 32767;
    assert(steps(lofi0, lofi0, 1) <= imax(steps(lofi0, lofi0, 0), steps(lofi1, lofi1, 0)) + tol);
    memcpy(sw0, w0, 16); sw0[6] = clock_word(1);                                      /* wet only: a clock change clears */
    steps(w0, sw0, 0);
    assert(rms(g_out, sw + 1024, sw + 2048) < rms(g_out, sw - 4410, sw) / 4);
    printf("ok changes\n");
}

static void t_stable(void)                                   /* loud noise at the extremes, then silence: zero */
{
    static int16_t in[SR * 112], out[SR * 112];
    const int16_t progs[3] = { prog_word(8), prog_word(17), prog_word(26) };    /* room, plate, hall: high / high */
    for (int j = 0; j < 3; j++) {
        int16_t k[8] = { 32767, 32767, 0, 32767, 0, 32767, clock_word(0), progs[j] };
        nt_par q;
        nt_params(k, &q);
        int n = BLOCKS((int)((60 + 2.0 * q.rtb / 1000 + 1) * SR));          /* 60 s noise, 2 x RT60 + 1 s quiet */
        for (int i = 0; i < n; i++) in[i] = i < 60 * SR ? (int16_t)(rand() % 65535 - 32767) : 0;
        start(0, k); run(0, k, in, out, n);
        for (int i = n - OP1_FX_BLOCK; i < n; i++) assert(out[i] == 0);
    }
    for (int c = 1; c < 3; c++)                                 /* STD; LOFI at its slowest clock (x8): streamed */
        for (int j = 0; j < 3; j++) {
            int16_t k[8] = { 32767, 32767, 32767, 32767, 0, 32767, clock_word(c), prog_word(8 + 9 * j) };
            nt_par q;
            nt_params(k, &q);
            double rt = q.rtb / 1000.0 * (c == NT_LOFI ? 65536.0 / q.inc / 2 : 1);
            int n = BLOCKS((int)((60 + 2 * rt + 1) * SR)), zero = 1;
            start(0, k);
            for (int b = 0; b < n; b += OP1_FX_BLOCK) {
                int16_t blk[OP1_FX_BLOCK];
                for (int i = 0; i < OP1_FX_BLOCK; i++) blk[i] = b + i < 60 * SR ? (int16_t)(rand() % 65535 - 32767) : 0;
                nt_render(0, blk, k);
                if (b + OP1_FX_BLOCK == n) for (int i = 0; i < OP1_FX_BLOCK; i++) zero &= blk[i] == 0;
            }
            assert(zero);
        }
    printf("ok stable\n");
}

static void t_level(void)                                    /* full wet against plugins/reverb, same burst */
{
    static int16_t in[SR * 2], a[SR * 2], b[SR * 2];
    int n = BLOCKS(SR * 2);
    for (int i = 0; i < n; i++) in[i] = i < SR / 10 ? (int16_t)(rand() % 16001 - 8000) : 0;
    int16_t kr[8] = { 16384, 32767, 16384, 32767 };
    reverb_init(0);
    for (int i = 0; i < n; i += OP1_FX_BLOCK) { memcpy(a + i, in + i, OP1_FX_BLOCK * 2); reverb_render(0, a + i, kr); }
    int16_t k[8];
    memcpy(k, K_DEF, sizeof k); k[2] = 0; k[3] = 32767;
    start(0, k); run(0, k, in, b, n);
    double db = 20 * log10(rms(b, 0, n) / rms(a, 0, n));
    printf("level: nostromo %+.1f dB against reverb\n", db);
    assert(fabs(db) <= 3);
    printf("ok level\n");
}

static void wav(const char *fn, const int16_t *s, int n)
{
    FILE *f = fopen(fn, "wb"); if (!f) return;
    uint32_t b = n * 2, h[] = { 0x46464952, 36 + b, 0x45564157, 0x20746d66, 16, 0x00010001, SR, SR * 2, 0x00100002, 0x61746164, b };
    fwrite(h, 4, 11, f); fwrite(s, 2, n, f); fclose(f);
}
static void t_wav(void)                                      /* a noise-burst loop through three programs */
{
    static int16_t in[SR * 8], out[SR * 8];
    int n = BLOCKS(SR * 8);
    for (int i = 0; i < n; i++) { int ph = i % (SR / 2); in[i] = ph < 1500 ? (int16_t)((rand() % 32001 - 16000) * (1500 - ph) / 1500) : 0; }
    struct { int16_t w7; const char *fn; } d[3] = { { prog_word(12), "out/nostromo_plate.wav" }, { prog_word(3), "out/nostromo_room.wav" },
                                                   { prog_word(23), "out/nostromo_hall_mod.wav" } };
    for (int j = 0; j < 3; j++) {
        int16_t k[8];
        memcpy(k, K_DEF, sizeof k); k[3] = 16384; k[7] = d[j].w7;
        start(0, k); run(0, k, in, out, n); wav(d[j].fn, out, n);
    }
    printf("ok -> out/nostromo_plate.wav, out/nostromo_room.wav, out/nostromo_hall_mod.wav\n");
}

static int nvis(const char *id)
{
    for (int i = 0; i < nnodes; i++) if (!strcmp(node_id[i], id)) return node_vis[i];
    printf("no node %s\n", id); assert(0); return 0;
}
static int nknown(const char *id) { for (int i = 0; i < nnodes; i++) if (!strcmp(node_id[i], id)) return 1; return 0; }
static const int32_t *nmat(const char *id)
{
    for (int i = 0; i < nnodes; i++) if (!strcmp(node_id[i], id)) return node_m[i];
    printf("no node %s\n", id); assert(0); return 0;
}
static void readout(char *s, int *unit)                      /* the LED readout as text, e.g. " 2.4" */
{
    int k = 0;
    for (int d = 0; d < 3; d++) {
        int m = 0, ch = ' ';
        for (int g = 0; g < 7; g++) { char id[4] = { 's', (char)('0' + d), (char)('a' + g), 0 }; m |= nvis(id) << g; }
        for (int v = 0; v < 10; v++) if (m && ns_7seg[v] == m) ch = '0' + v;
        assert(!m || ch != ' ');
        s[k++] = (char)ch;
        if (d < 2) { char id[4] = { 'd', 'p', (char)('0' + d), 0 }; if (nvis(id)) s[k++] = '.'; }
    }
    s[k] = 0;
    *unit = NU_NONE;
    for (int u = 0; u < 4; u++) { char id[3] = { 'u', (char)('0' + u), 0 }; if (nvis(id)) { assert(*unit == NU_NONE); *unit = u; } }
}

static void t_fmt(void)
{
    struct { int c; int16_t v; const char *s; int unit; int ty; int32_t hz; } t[] = {
        { NC_MID, 16645, " 2.4", NU_SEC, 1 }, { NC_MID, 32767, "10.0", NU_SEC, 1 }, { NC_MID, 0, "0.55", NU_SEC, 1 },
        { NC_MID, 0, "0.34", NU_SEC, 0 }, { NC_MID, 0, "0.80", NU_SEC, 2 }, { NC_MID, 18940, " 2.4", NU_SEC, 0 },
        { NC_BASS, 17824, " 2.7", NU_SEC, 1 }, { NC_BASS, 0, "0.34", NU_SEC, 0 }, { NC_BASS, 32767, "10.0", NU_SEC, 2 },
        { NC_BASS, 16645, "19.2", NU_SEC, 1, 2756 },                                     /* the bass in seconds, as the mid */
        { NC_TREB, 23494, " 6.0", NU_KHZ }, { NC_TREB, 0, "500", NU_HZ }, { NC_TREB, 32767, "16.0", NU_KHZ },
        { NC_CLK, 1024, "44.1", NU_KHZ }, { NC_CLK, 3072, "22.1", NU_KHZ }, { NC_CLK, 5120, "7.80", NU_KHZ, 1, 7796 }, { NC_CLK, 7168, "44.1", NU_KHZ },
        { NC_PRE, 6553, " 20", NU_MS }, { NC_PRE, 32767, "500", NU_MS }, { NC_PRE, 0, "  0", NU_MS },
        { NC_MIX, 11468, " 35", NU_NONE }, { NC_MIX, 32767, "100", NU_NONE }, { NC_MIX, 0, "  0", NU_NONE },
        { NC_PROG, prog_word(12), " 13", NU_NONE }, { NC_PROG, 32767, " 27", NU_NONE }, { NC_PROG, 0, "  1", NU_NONE },
        { NC_PRE, 0, "22.1", NU_KHZ, 1, 22050 }, { NC_PRE, 16384, "7.80", NU_KHZ, 1, 7796 },   /* LOFI: the clock */
        { NC_PRE, 12461, "10.0", NU_KHZ, 1, 9999 },                                   /* 9.995 .. 9.999 kHz round up */
        { NC_MID, 16645, "19.2", NU_SEC, 1, 2756 }, { NC_TREB, 23494, "750", NU_HZ, 1, 2756 } };   /* stretched, scaled */
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
        uint8_t d[3]; int dp, unit, k = 0; char s[8];
        ns_fmt(t[i].c, t[i].v, t[i].ty, t[i].hz, d, &dp, &unit);
        for (int j = 0; j < 3; j++) { s[k++] = d[j] == NS_BLANK ? ' ' : (char)('0' + d[j]); if (dp == j) s[k++] = '.'; }
        s[k] = 0;
        if (strcmp(s, t[i].s) || unit != t[i].unit) { printf("fmt %d %d: '%s' unit %d\n", t[i].c, t[i].v, s, unit); assert(0); }
    }
    printf("ok fmt\n");
}

static void t_track(void)                                    /* last turned: threshold, re-arm per FX position */
{
    int32_t v[NC_N] = { 20814, 23494, 6553, 11468, 17824, 16299, 15170 };
    int pg[3] = { 0, 0, 0 };
    ns.armed = 0;
    ns_track(v, pg, 0);
    assert(ns.last == NC_MID);
    v[NC_MIX] = 30000;
    ns_track(v, pg, 1);                                      /* another position: re-armed, nothing marked */
    assert(ns.last == NC_MID);
    v[NC_MIX] = 30100;
    ns_track(v, pg, 1);                                      /* below the threshold */
    assert(ns.last == NC_MID);
    v[NC_MIX] = 30200;
    ns_track(v, pg, 1);                                      /* a slow turn adds up */
    assert(ns.last == NC_MIX);
    pg[1] = 1;
    ns_track(v, pg, 1);                                      /* a press: its new control */
    assert(ns.last == NC_CLK);
    printf("ok track\n");
}

static void frames(int n) { for (int f = 0; f < n; f++) SCREEN_nostromo.update(0, 25); }
static void open_screen(void)                                /* defaults, every page 1, quiet, no LFO */
{
    nnodes = 0; nstyles = 0;
    memcpy(host_fx_knobs[0], K_DEF, sizeof K_DEF);
    memset((void *)op1_fx_page, 0, sizeof op1_fx_page);
    memset(host_fx_lfo, 0, sizeof host_fx_lfo);
    host_fx_meter[0] = 0;
    SCREEN_nostromo.init(0);
}
static int32_t node_x(const char *id) { return nmat(id)[6] >> 10; }
static int32_t node_y(const char *id) { return nmat(id)[7] >> 10; }
static int is_exhaust(int i) { return i >= ns.nl && ln[i].y0 == ln[i].y1 && ln[i].w <= 3072; }
static int streaks(int *core_w)                              /* the exhaust's rows; the core's width */
{
    int32_t ys[8]; int n = 0;
    *core_w = 0;
    for (int i = 0; i < nln; i++) {
        if (!is_exhaust(i)) continue;
        int k = 0;
        while (k < n && ys[k] != ln[i].y0) k++;
        if (k == n) { assert(n < 8); ys[n++] = ln[i].y0; }
        if (ln[i].y0 == ((node_y("sh") + ns_nozzle[0]) << 10) + 512 && ln[i].w > *core_w) *core_w = ln[i].w;
    }
    return n;
}
static int32_t exhaust_left(void)
{
    int32_t m = 1 << 30;
    for (int i = 0; i < nln; i++) if (is_exhaust(i)) m = ln[i].x0 < m ? ln[i].x0 : m;
    return m;
}
static int bloom(void)                                       /* the flare's bloom at the nozzles */
{
    for (int i = ns.nl; i < nln; i++) if (ln[i].x0 == ln[i].x1 && ln[i].w == 2048) return 1;
    return 0;
}
static void on_screen(void)                                  /* every line: on the screen, in the scope, not zero length */
{
    assert(ns.nl <= NS_LINES && nln >= ns.nl);
    for (int i = 0; i < nln; i++) {
        assert(ln[i].x0 >= 8 << 10 && ln[i].x1 >= 8 << 10 && ln[i].x0 <= 312 << 10 && ln[i].x1 <= 312 << 10);
        assert(ln[i].y0 >= 36 << 10 && ln[i].y1 >= 36 << 10 && ln[i].y0 <= 116 << 10 && ln[i].y1 <= 116 << 10);
        assert(ln[i].x0 != ln[i].x1 || ln[i].y0 != ln[i].y1);
        assert(ln[i].w >= 1024 && ln[i].c[3] == 0xFF);
    }
}

static void t_caption(void)                                  /* MU/TH/UR, the program typed out, the eggs */
{
    open_screen();
    frames(1);
    assert(nvis("bt") && !nvis("cp") && !nvis("e0") && !nvis("e1") && nvis("cv") && nvis("cu"));   /* opens: MU/TH/UR 6000 */
    assert(nmat("cv")[0] == 1024);                                             /* the cover over all of it */
    frames(20);
    assert(nvis("bt") && !nvis("cv") && node_x("cu") == ns_cap_end[3]);     /* typed: the cursor after it */
    frames(44);                                                                /* 1.625 s: the program, typed out */
    assert(!nvis("bt") && nvis("cp") && nvis("cv"));
    for (int f = 0; f < 60 && nvis("cv"); f++) {
        int32_t s = nmat("cv")[0], left = (NS_COVER_R << 10) - (NS_COVER_R - NS_CAP_X) * s;   /* the cover's left edge: the cursor */
        assert(abs(left - (node_x("cu") << 10)) <= 1024);
        frames(1);
    }
    assert(!nvis("cv") && node_x("cu") == ns_cap_end[0]);
    assert(!nknown("pt1") && !nknown("po3") && nvis("wa1") && nvis("wb1") && nvis("wc0") && nvis("wd0") && !nvis("wd1"));
    int on = 0, off = 0;
    for (int f = 0; f < 40; f++) { frames(1); nvis("cu") ? on++ : off++; }
    assert(on >= 15 && off >= 15);                                             /* the cursor blinks */
    host_fx_knobs[0][7] = prog_word(14);                                       /* a new program: wiped, typed again */
    frames(1);
    assert(nvis("cv") && node_x("cu") < 30);
    frames(10);
    int32_t c0 = node_x("cu");
    host_fx_knobs[0][7] = prog_word(15);                                       /* turned on while typing: it carries on */
    frames(1);
    assert(nvis("cv") && node_x("cu") >= c0 && nvis("wb2"));
    frames(60);
    assert(!nvis("cv") && nvis("wb2") && nvis("wc0"));         /* 16: plate / high / low */
    host_fx_knobs[0][7] = prog_word(14);                                       /* within 10 s: in place, no wipe */
    frames(1);
    assert(!nvis("cv") && nvis("wb1") && nvis("wc2") && !nvis("wc0") && node_x("cu") == ns_cap_end[0]);
    frames(390);                                                               /* 9.775 s still: still in place */
    host_fx_knobs[0][7] = prog_word(13); frames(1);
    assert(!nvis("cv") && nvis("wc1"));
    host_fx_knobs[0][7] = prog_word(14); frames(1);
    frames(400);                                                               /* 10 s still: the next change wipes */
    host_fx_knobs[0][6] = clock_word(1);                                       /* the clock too */
    frames(1);
    assert(nvis("cv"));
    frames(60);
    assert(nvis("wd1") && node_x("cu") == ns_cap_end[1]);
    host_fx_knobs[0][7] = prog_word(21);                                       /* a hall */
    frames(140);
    assert(!nvis("e0"));
    host_fx_knobs[0][0] = 32767;                                               /* its mid decay at 10 s: SPECIAL ORDER 937 */
    frames(1);
    assert(nvis("e0") && !nvis("cp") && !nvis("bt") && nvis("cv"));
    frames(102);
    assert(nvis("e0") && !nvis("cv") && node_x("cu") == ns_cap_end[4]);
    frames(2);                                                                 /* 2.6 s: the program back */
    assert(!nvis("e0") && nvis("cp"));
    frames(60);
    assert(!nvis("e0"));                                                       /* once on the way */
    host_fx_knobs[0][0] = 20000; frames(1); host_fx_knobs[0][0] = 32767; frames(1);
    assert(nvis("e0"));
    frames(110);
    host_fx_knobs[0][7] = prog_word(12); host_fx_knobs[0][0] = 32767;          /* a plate at 10 s: no */
    frames(110);
    assert(!nvis("e0"));
    host_fx_lfo[3] = -32767;                                                   /* an LFO to dry: no */
    frames(2);
    assert(!nvis("e1"));
    host_fx_lfo[3] = 0;
    host_fx_knobs[0][3] = 0;                                                   /* the mix turned to 0: CREW EXPENDABLE */
    frames(1);
    assert(nvis("e1") && !nvis("cp"));
    frames(110);
    assert(!nvis("e1") && nvis("cp"));
    SCREEN_nostromo.update(0, 1 << 20);                                        /* back after the screen was hidden */
    assert(nvis("bt") && !nvis("e1"));
    open_screen();                                                             /* opening at 0 or a hall at 10 s: MU/TH/UR only */
    host_fx_knobs[0][3] = 0; host_fx_knobs[0][7] = prog_word(21); host_fx_knobs[0][0] = 32767;
    frames(1);
    assert(nvis("bt"));
    frames(120);
    assert(nvis("cp") && !nvis("e0") && !nvis("e1"));
    printf("ok caption\n");
}

static void t_scope(void)                                    /* the impulse response, kept; the ship at its end */
{
    open_screen();
    frames(100);
    on_screen();
    int n0 = ns.nl;
    assert(n0 > 30 && nln > n0);
    static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } kept[NS_LINES];
    memcpy(kept, ln, n0 * sizeof *ln);
    frames(1);
    assert(ns.nl == n0 && !memcmp(kept, ln, n0 * sizeof *ln));               /* nothing changed: the same lines */
    const int32_t B = NS_BASE << 10, H = NS_BASE - NS_TOP;
    int dry = 0;
    for (int i = 0; i < ns.nl; i++)
        if (ln[i].w == 3072) { dry++; assert(abs(ln[i].y0 - ln[i].y1 - ((H << 10) * (32768 - K_DEF[3]) >> 15)) <= 1); }   /* mix 35 % */
    assert(dry == 1);
    assert(ns.xs == NS_XD + 4 + (NS_PRE_PX * K_DEF[2] >> 15));
    int dash = 0;                                                              /* the bass dashed, 1.5 px as the mid: */
    for (int i = 0; i < ns.nl; i++)                                            /* blue page 1: the bass dim */
        if (ln[i].w == 1536 && ln[i].c[2] == (0xff * 140 >> 8)) {
            dash++;
            double dx = (ln[i].x1 - ln[i].x0) / 1024.0, dy = (ln[i].y1 - ln[i].y0) / 1024.0;
            assert(sqrt(dx * dx + dy * dy) <= 4.6);
        }
    assert(dash > 16);
    int echo = 0, sparse = 0;
    for (int i = 0; i < ns.nl; i++) { echo += ln[i].w == 2048; sparse += ln[i].w == 1024 && ln[i].c[0] == (0xff * 204 >> 8); }
    assert(!echo && !sparse);                                                  /* plate, MED: neither */
    host_fx_knobs[0][7] = prog_word(15); frames(1);                            /* diffusion HIGH: 4 echoes, halving */
    int32_t eh[4], ex4[4];
    for (int i = 0; i < ns.nl; i++)
        if (ln[i].w == 2048 && ln[i].x0 == ln[i].x1 && ln[i].y0 == B) { assert(echo < 4); eh[echo] = B - ln[i].y1; ex4[echo++] = ln[i].x0; }
    assert(echo == 4);
    for (int e = 1; e < 4; e++) assert(ex4[e] > ex4[e - 1] && abs(eh[e - 1] - 2 * eh[e]) <= 2);
    host_fx_knobs[0][7] = prog_word(9); frames(30);                            /* a plate at LOW: no cloud */
    for (int i = 0; i < ns.nl; i++) sparse += ln[i].w == 1024 && ln[i].c[0] == (0xff * 204 >> 8);
    assert(!sparse);
    host_fx_knobs[0][7] = prog_word(18); frames(30);                           /* a hall at LOW: the cloud, 8 taps, */
    int before = 0;                                                            /* some ahead of the gate */
    for (int i = 0; i < ns.nl; i++) if (ln[i].w == 1024 && ln[i].c[0] == (0xff * 204 >> 8) && ln[i].x0 == ln[i].x1) { sparse++; before += ln[i].x0 < ns.xs << 10; }
    assert(sparse == NT_SPS && before == 2);
    host_fx_knobs[0][7] = K_DEF[7]; frames(30);                 /* the gate: the pre-delay knob */
    int32_t rt = nt_lut(nt_rt[1], K_DEF[0]), W = NS_X1 - ns.xs, end = 0;       /* the ship at the mid decay's end */
    for (int i = 0; i < ns.nl; i++) if (ln[i].w == 1536 && ln[i].y1 == B) end = ln[i].x1;
    double u = log2(1 + rt / 250.0) / log2(41);
    assert(fabs(end / 1024.0 - (ns.xs + W * u)) < 1.5 && abs(node_x("sh") - (end >> 10)) <= 1);
    int cw;
    assert(streaks(&cw) == 2 && cw == 2048);                                   /* bass x1.2: 2 streaks */
    assert(abs(exhaust_left() - ((ns.xs + 2) << 10)) <= 1024);                 /* back to the gate */
    host_fx_knobs[0][5] = 0; frames(1);
    assert(streaks(&cw) == 1 && cw == 1024);                                   /* x0.5: 1 */
    host_fx_knobs[0][5] = 32767; frames(1);
    assert(streaks(&cw) == 3 && cw == 3072);                                   /* x2.5: 3, the core 3 px */
    host_fx_knobs[0][1] = 0; frames(1);                                        /* treble: the flame's heat */
    int red0 = 0, red1 = 0;
    for (int i = 0; i < nln; i++) if (is_exhaust(i) && ln[i].w == 3072) red0 = imax(red0, ln[i].c[0]);
    host_fx_knobs[0][1] = 32767; frames(1);
    for (int i = 0; i < nln; i++) if (is_exhaust(i) && ln[i].w == 3072) red1 = imax(red1, ln[i].c[0]);
    assert(red0 == 0 && red1 > 150);                                           /* green embers .. white-hot */
    int32_t x0 = node_x("sh");
    host_fx_knobs[0][0] = 32767; frames(1);
    assert(node_x("sh") > x0 + 40 && node_x("sh") <= NS_X1 - NS_SHIP_W - 1);   /* 10 s: further, on the screen */
    host_fx_knobs[0][2] = 0; frames(1);
    assert(ns.xs == NS_XD + 4);
    host_fx_knobs[0][2] = 32767; frames(1);
    assert(ns.xs == NS_XD + 4 + (NS_PRE_PX * 32767 >> 15));
    memcpy(host_fx_knobs[0], K_DEF, sizeof K_DEF); frames(1);
    int last = ns.last; x0 = node_x("sh");
    host_fx_lfo[0] = 8000; frames(1);                                          /* an LFO: the picture follows, nothing marked */
    assert(node_x("sh") > x0 && ns.last == last);
    host_fx_lfo[0] = 0;
    host_fx_knobs[0][6] = clock_word(1); frames(30);                           /* STD: reflections in 12-bit steps */
    int bars = 0;
    for (int i = 0; i < ns.nl; i++)
        if (ln[i].x0 == ln[i].x1 && ln[i].y0 == B && ln[i].w == 1024 && ln[i].c[0] == ln[i].c[1]) { bars++; assert((B - ln[i].y1) % ((H << 10) / 10) == 0); }
    assert(bars >= 12);
    host_fx_knobs[0][6] = clock_word(2); host_fx_knobs[0][2] = 32767; frames(30);   /* LOFI at 2.8 kHz: wide steps */
    int flat = 0;
    for (int i = 0; i < ns.nl; i++) if (ln[i].y0 == ln[i].y1 && ln[i].y0 <= B && ln[i].c[0] == ln[i].c[1] && ln[i].w == 1024) flat++;
    assert(flat >= 2 && flat <= (NS_X1 - ns.xs) / (8 * 22050 / 2756) + 1);
    host_fx_knobs[0][2] = 0; frames(1);                                        /* at 22 kHz: steps of 8 px */
    flat = 0;
    for (int i = 0; i < ns.nl; i++) if (ln[i].y0 == ln[i].y1 && ln[i].y0 <= B && ln[i].c[0] == ln[i].c[1] && ln[i].w == 1024) flat++;
    assert(flat >= (NS_X1 - ns.xs) / 8 - 1);
    for (int c = 0; c < 3; c++)                                                /* every clock and program, the knobs' ends */
        for (int p = 0; p < 27; p++)
            for (int e = 0; e < 4; e++) {
                for (int i = 0; i < 4; i++) host_fx_knobs[0][i] = (int16_t)((e >> (i & 1)) & 1 ? 32767 : 0);
                host_fx_knobs[0][5] = (int16_t)(e & 1 ? 32767 : 0);
                host_fx_knobs[0][6] = clock_word(c); host_fx_knobs[0][7] = prog_word(p);
                SCREEN_nostromo.update(0, 25);
                on_screen();
                assert(node_x("sh") >= ns.xs + 4 && node_x("sh") + NS_SHIP_W < NS_X1);
            }
    printf("ok scope\n");
}

static void t_ship(void)                                     /* its drift, bob, shimmer; the flare; the stars */
{
    open_screen();
    frames(100);
    int32_t lo = 99, hi = -99;
    for (int f = 0; f < 120; f++) { frames(1); int32_t y = node_y("sh") - NS_SHIP_Y; lo = y < lo ? y : lo; hi = y > hi ? y : hi; }
    assert(lo == -1 && hi == 1);                                               /* LOW: a 1 px drift */
    host_fx_knobs[0][7] = prog_word(13); frames(1);                            /* MED: a 2 px bob */
    lo = 99; hi = -99;
    for (int f = 0; f < 60; f++) { frames(1); int32_t y = node_y("sh") - NS_SHIP_Y; lo = y < lo ? y : lo; hi = y > hi ? y : hi; }
    assert(lo == -2 && hi == 2);
    host_fx_knobs[0][7] = prog_word(14); frames(1);                            /* HIGH: 4 px at 3 Hz, the flame shimmers */
    int32_t a = exhaust_left(), moved = 0;
    lo = 99; hi = -99;
    for (int f = 0; f < 40; f++) { frames(1); int32_t y = node_y("sh") - NS_SHIP_Y; lo = y < lo ? y : lo; hi = y > hi ? y : hi; moved |= exhaust_left() != a; }
    assert(moved && lo == -4 && hi == 4);                                      /* the most of the three */
    int flips = 0, above = 0;
    for (int f = 0; f < 40; f++) { frames(1); int up = node_y("sh") > NS_SHIP_Y; flips += up != above; above = up; }
    assert(flips >= 4);                                                        /* fast: 3 Hz over 1 s */
    host_fx_knobs[0][7] = prog_word(13); host_fx_knobs[0][6] = clock_word(1); frames(30);   /* STD: half rate, a 2 px grid */
    int changed = 0; int32_t py = node_y("sh");
    for (int f = 0; f < 40; f++) { frames(1); assert((node_y("sh") - NS_SHIP_Y) % 2 == 0); changed += node_y("sh") != py; py = node_y("sh"); }
    assert(changed > 0 && changed <= 20);
    host_fx_knobs[0][7] = prog_word(12); host_fx_knobs[0][6] = clock_word(0); frames(40);   /* after the jump */
    assert(!bloom());
    host_fx_meter[0] = 20000; frames(1);                                       /* a hit: the engines flare */
    assert(bloom());
    host_fx_meter[0] = 0;
    frames(12);                                                                /* 300 ms on: gone */
    assert(!bloom());
    host_fx_meter[0] = 12000; frames(40);                                      /* a held level: no flare */
    assert(!bloom());
    host_fx_meter[0] = 0; frames(40);
    int32_t sx = node_x("sh");
    for (int f = 0; f < 200; f++) {                                            /* the stars: clear of the ship, drifting */
        frames(1);
        for (int i = 0; i < 6; i++) { char id[4] = { 's', 'r', (char)('0' + i), 0 }; if (nvis(id)) assert(node_x(id) <= sx - 2 || node_x(id) >= sx + NS_SHIP_W + 2); }
    }
    for (int m = 0; m < 3; m++) {                                              /* the stars: steady, twinkling, flickering */
        host_fx_knobs[0][7] = prog_word(12 + m); host_fx_knobs[0][6] = clock_word(0); frames(30);
        int lo2 = 255, hi2 = 0, steps = 0, prev = style("sf0")[0];
        for (int f = 0; f < 40; f++) {
            frames(1);
            int c = style("sf0")[0];
            assert(style("sf0")[1] == c && style("sf0")[2] == c);              /* white */
            lo2 = c < lo2 ? c : lo2; hi2 = c > hi2 ? c : hi2; steps += abs(c - prev) > 20; prev = c;
        }
        if (m == 0) assert(lo2 == 255 && hi2 == 255);                           /* LOW: steady, the brightest at full */
        if (m == 1) assert(lo2 < 140 && hi2 > 240 && steps < 12);             /* MED: a slow, deep twinkle */
        if (m == 2) assert(lo2 < 100 && hi2 > 220 && steps >= 15);            /* HIGH: a fast flicker */
    }
    uint32_t t = ns.t;
    SCREEN_nostromo.update(0, -5);                                             /* a negative frame time: nothing moves */
    assert(ns.t == t);
    printf("ok ship\n");
}

static void t_jump(void)                                     /* a type or clock change: hyperspace and back */
{
    open_screen();
    frames(100);
    assert(ns.hj == 0);
    host_fx_knobs[0][7] = prog_word(13); frames(1);                            /* the same type: no jump */
    assert(ns.hj == 0);
    int32_t sx0 = ns.sx, gone = 0, warp = 0, prev = node_x("sh");
    host_fx_knobs[0][6] = clock_word(1);                                       /* STD: the reverb fades and clears */
    for (int f = 1; f <= 24; f++) {
        frames(1);
        int el = 25 * f;
        if (el < 250) { assert(nvis("sh") && node_x("sh") >= prev && bloom()); prev = node_x("sh"); }
        else if (el < 350) {
            assert(!nvis("sh")); gone++;
            for (int i = ns.nl; i < nln; i++) warp |= ln[i].w == 2048 && ln[i].x0 == ns.xs << 10 && ln[i].x1 == (NS_X1 + 4) << 10;
        } else if (el < 600) { assert(nvis("sh") && node_x("sh") <= ns.sx); if (el > 375) assert(node_x("sh") >= prev); prev = node_x("sh"); }
        if (el == 225) assert(node_x("sh") > sx0 + 20);
        if (el == 375) prev = node_x("sh");
    }
    assert(gone == 4 && warp && ns.hj == 0 && node_x("sh") == ns.sx);
    host_fx_knobs[0][7] = prog_word(22); frames(1);                            /* a hall: again */
    assert(ns.hj > 0);
    open_screen();                                                             /* opening on another clock: none */
    host_fx_knobs[0][6] = clock_word(2); frames(1);
    assert(ns.hj == 0);
    printf("ok jump\n");
}

static void t_screen(void)
{
    open_screen();
    static char svg[1 << 18];                                /* every node the code looks up exists */
    FILE *f = fopen("../plugins/nostromo/nostromo.svg", "rb");
    assert(f);
    size_t n = fread(svg, 1, sizeof svg - 1, f);
    fclose(f);
    assert(n < sizeof svg - 1);
    svg[n] = 0;
    for (int i = 0; i < nnodes; i++) { char pat[24]; snprintf(pat, sizeof pat, "id=\"%s\"", node_id[i]); assert(strstr(svg, pat)); }
    for (int i = 0; i < nstyles; i++) { char pat[24]; snprintf(pat, sizeof pat, "id=\"%s\"", style_id[i]); assert(strstr(svg, pat)); }
    int nl = 0;
    for (int i = 0; i < NS_LABELS; i++) { assert(ns_lab_n[i] <= NS_LAB_MAX); nl += ns_lab_n[i]; }
    assert(nstyles == nl + 6);                               /* and the 6 stars' fills */
    char s[8]; int unit;
    SCREEN_nostromo.update(0, 25);
    readout(s, &unit); assert(!strcmp(s, " 2.4") && unit == NU_SEC && nvis("nm0") && !nvis("nm4"));   /* opens: mid, named */
    const uint8_t *mid = style("l0a"), *bass = style("l1c");                   /* blue on page 1: MID bright, BASS dim */
    assert(mid[0] == 0x4d && mid[1] == 0x9e && mid[2] == 0xff && bass[2] == (0xff * 102 >> 8));
    assert(nvis("xt3") && nvis("xo5") && !nvis("xh1") && !nvis("xt0"));        /* mix 35 % */
    op1_fx_page[0][0] = 1;                                                     /* blue on page 2 */
    SCREEN_nostromo.update(0, 25);
    readout(s, &unit); assert(!strcmp(s, " 2.7") && unit == NU_SEC && nvis("nm4"));   /* bass: seconds, as mid */
    assert(style("l0e")[2] == (0xff * 102 >> 8) && style("l1a")[2] == 0xff);
    host_fx_knobs[0][1] = 0;                                                   /* treble turned */
    SCREEN_nostromo.update(0, 25);
    readout(s, &unit); assert(!strcmp(s, "500") && unit == NU_HZ && nvis("nm1") && !nvis("nm4"));
    op1_fx_page[0][2] = 1;                                                     /* white on page 2: program, PROGRAM bright */
    SCREEN_nostromo.update(0, 25);
    readout(s, &unit); assert(!strcmp(s, " 13") && nvis("nm6") && style("l5j")[0] == 0xff && style("l4a")[0] == (0xff * 102 >> 8));   /* the press: program */
    host_fx_knobs[0][7] = prog_word(26);                                       /* program 27 */
    SCREEN_nostromo.update(0, 25);
    readout(s, &unit); assert(!strcmp(s, " 27") && unit == NU_NONE && nvis("wa2") && nvis("wb2") && nvis("wc2"));
    host_fx_knobs[0][6] = 3072;                                                /* clock STD */
    SCREEN_nostromo.update(0, 25);
    readout(s, &unit); assert(!strcmp(s, "22.1") && unit == NU_KHZ && nvis("nm5") && nvis("wd1"));
    host_fx_knobs[0][6] = 5120;                                                /* clock LOFI: its clock in kHz */
    SCREEN_nostromo.update(0, 25);
    readout(s, &unit); assert(!strcmp(s, "14.5"));
    host_fx_knobs[0][2] = 16384;                                               /* LOFI: the white knob is the clock */
    SCREEN_nostromo.update(0, 25);
    readout(s, &unit); assert(!strcmp(s, "7.80") && unit == NU_KHZ && nvis("nm7") && !nvis("nm2"));   /* named RATE */
    assert(nvis("lr") && !nvis("lp") && style("l6a")[0] == (0xff * 102 >> 8));     /* the label too (white on page 2: dim) */
    op1_fx_page[0][2] = 0; SCREEN_nostromo.update(0, 25);
    assert(style("l6a")[0] == 0xff);
    host_fx_knobs[0][6] = 3072; SCREEN_nostromo.update(0, 25);                   /* STD: PRE-DLAY again */
    assert(!nvis("lr") && nvis("lp"));
    host_fx_knobs[0][2] = 0; SCREEN_nostromo.update(0, 25);
    readout(s, &unit); assert(!strcmp(s, "  0") && unit == NU_MS && nvis("nm2") && !nvis("nm7"));
    host_fx_knobs[0][6] = 5120; op1_fx_page[0][2] = 1;
    host_fx_knobs[0][3] = 32767; SCREEN_nostromo.update(0, 25);                /* mix 100 % */
    assert(nvis("xh1") && nvis("xt0") && nvis("xo0"));
    host_fx_knobs[0][5] = -5; host_fx_knobs[0][7] = -5;                         /* words below 0 */
    SCREEN_nostromo.update(0, 25);
    on_screen();
    op1_fx_page[0][0] = op1_fx_page[0][2] = 0;
    printf("ok screen\n");
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    for (int p = 0; p < 3; p++) host_fx_scratch[p] = scratch[p];
    t_tables(); t_params(); t_mod_depth();
    t_hifi(); t_exact(); t_std_room(); t_knee(); t_headroom(); t_hot(); t_into_room(); t_minors(); t_std_hot(); t_long_level(); t_quiet_tails(); t_float_match(); t_rt60(); t_predelay(); t_clocks(); t_review(); t_room(); t_mix(); t_clear(); t_positions(); t_changes(); t_stable();
    t_level(); t_wav();
    t_fmt(); t_track(); t_caption(); t_scope(); t_ship(); t_jump(); t_screen();
    printf("all ok\n");
    return 0;
}
