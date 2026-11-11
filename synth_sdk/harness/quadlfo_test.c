/* Host test of the quad LFO (lfo/quadlfo.c) with the firmware calls mocked.
 *   cc -DOP1_HOST -I.. -Iharness quadlfo_test.c -lm -o build/quadlfo_test && build/quadlfo_test [render]
 * Checks: frequency range, routing per pass (synth knobs pass 0, envelope pass 1, FX in apply_fx),
 * amount 0 = no call, modulation depth, quad detection vs. midi LFO defaults, encoder editing, per-LFO mute.
 * With "render": prints JSON frames (screen states) for harness/render_quadlfo.py. */
#include "../screens/op1r_settings.c"
#include "../lfo/quadlfo.c"
#define BPS   689                                  /* audio blocks per second (64 frames at 44.1 kHz) */
#define BPS_L 689L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4];
void *host_lfo_obj[6]; int16_t host_lfo_words[8]; uint32_t host_cycles, host_tempo_rate; int host_shift, host_pos, host_lfo_on[2] = { 1, 1 };

/* recorded firmware calls */
static int n_synth, n_env, n_fx, last_idx; static int16_t last_mod, max_mod;
static int n_drum, drum_eng, drum_idx;
void host_knob_range_drum(int k, int16_t *m, int e, void *p) { (void)m; (void)p; n_drum++; drum_eng = e; drum_idx = k; }
void host_knob_range_synth(int k, int16_t *m, int e, void *p) { (void)e; (void)p; n_synth++; last_idx = k; last_mod = *m; if (abs(*m) > max_mod) max_mod = (int16_t)abs(*m); }
int16_t host_fx_range[8][3], host_fx_params[8];
int16_t host_stage[8]; int host_eng_plugin = 1, host_fx_plugin = 1;
static int16_t *last_p; int host_master_type = 1;
void host_fx_knob_mod(int16_t *m, const int16_t *rg, int16_t *p) { n_fx++; last_idx = (int)(p - host_fx_params); last_p = p; last_mod = *m;
    if (rg[1] > rg[0]) { int v = *p + *m; *p = (int16_t)(v < rg[0] ? rg[0] : v > rg[1] ? rg[1] : v); } }   /* clamps as the firmware */
void host_env_mod(int k, int16_t *m, void *p) { (void)p; n_env++; last_idx = k; last_mod = *m; if (abs(*m) > max_mod) max_mod = (int16_t)abs(*m); }

/* screen mocks */
#define MAXL 512
static char node_id[8][16]; static int32_t node_m[8][9]; static int nnodes;
static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } lines[MAXL]; static int nlines;
void *host_find_node(void *s, const char *id) { (void)s; snprintf(node_id[nnodes], 16, "%s", id); return (void *)(intptr_t)(++nnodes); }
void host_node_set_transform(void *n, const int32_t *m) { memcpy(node_m[(intptr_t)n - 1], m, 36); }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; nlines = 0; }
void *host_raster(void) { return &nlines; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c)
{ (void)r; const op1_pt *p = a, *q = b; if (nlines < MAXL) { lines[nlines].x0 = p->x; lines[nlines].y0 = p->y; lines[nlines].x1 = q->x; lines[nlines].y1 = q->y; lines[nlines].w = w; memcpy(lines[nlines].c, c, 4); nlines++; } }

typedef struct { int32_t x, y, size; uint8_t c[4]; char s[32]; } htext_t;   /* stock font stand-in (pond screen) */
static htext_t *texts[32]; static int ntexts;
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t z, const uint8_t *c) { htext_t *t = o; t->x = x; t->y = y; t->size = z; memcpy(t->c, c, 4); t->s[0] = 0; return o; }
void host_text_set(void *t, const char *s) { snprintf(((htext_t *)t)->s, 32, "%s", s); }
void host_text_draw(void *t, void *r, const int32_t *m) { (void)r; (void)m; if (ntexts < 32) texts[ntexts++] = t; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
/* current-format words: freq(rate 0..63, wave, sync, smooth, offset), ctrl(type, target 0..11, amount -31..31) */
static int16_t fq(int rate, int wave, int sync, int smooth, int off) { return (int16_t)(wave << 13 | sync << 12 | smooth << 9 | off << 6 | rate); }
static int16_t ct(int type, int tgt, int amt) { return (int16_t)(type << 11 | tgt << 6 | (amt + 32)); }
/* the tests write LFO i as fq(...) in w[i] and ct(...) in w[4 + i] (readable); pk() packs that into the real
 * 128-bit layout through ql_enc */
static void pk(int16_t *w)
{
    ql_lfo o[4] = { { 0 } }; int16_t out[8] = { 0 };
    for (int i = 0; i < 4; i++) {
        int f = (uint16_t)w[i], c = (uint16_t)w[4 + i];
        o[i].rate = f & 63; o[i].sync = (f >> 12) & 1; o[i].wave = (f >> 13) & 7; o[i].smooth = (f >> 9) & 7; o[i].off = (f >> 6) & 7;
        o[i].type = (c >> 11) & 7; o[i].tgt = (c >> 6) & 31; o[i].amt = (int8_t)((c & 63) - 32);
    }
    for (int i = 0; i < 4; i++) ql_enc(&o[i], out, i);
    memcpy(w, out, 16);
}
static ql_lfo D(const int16_t *w, int i) { ql_lfo o; ql_dec(w, i, &o); return o; }
static void blocks(int n, const int16_t *w, int voice) { for (int b = 0; b < n; b++) { host_cycles += cpb; ql_apply_synth(host_lfo_obj[voice], w, 0); } }

static void frame_json(const char *label)
{
    printf("{\"label\": \"%s\", \"nodes\": {", label);
    for (int i = 0; i < nnodes; i++) { printf("%s\"%s\": [", i ? ", " : "", node_id[i]); for (int j = 0; j < 9; j++) printf("%s%d", j ? ", " : "", node_m[i][j]); printf("]"); }
    printf("}, \"lines\": [");
    for (int i = 0; i < nlines; i++) printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", lines[i].x0, lines[i].y0, lines[i].x1, lines[i].y1, lines[i].w, lines[i].c[0], lines[i].c[1], lines[i].c[2], lines[i].c[3]);
    printf("], \"texts\": [");
    for (int i = 0; i < ntexts; i++) printf("%s[%d, %d, %d, [%d, %d, %d, %d], \"%s\"]", i ? ", " : "", texts[i]->x, texts[i]->y, texts[i]->size,
                                         texts[i]->c[0], texts[i]->c[1], texts[i]->c[2], texts[i]->c[3], texts[i]->s);
    printf("]}\n");
    ntexts = 0;
}

int main(int argc, char **argv)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    static int objs[6]; for (int i = 0; i < 6; i++) host_lfo_obj[i] = &objs[i];
    int render = argc > 1 && !strcmp(argv[1], "render");

    /* frequency range: inc per 128-sample block -> Hz */
    double fmin = ql_inc(0) / 4294967296.0 * 44100 / 64, fmax = ql_inc(32767) / 4294967296.0 * 44100 / 64;
    double fmid = ql_inc(16384) / 4294967296.0 * 44100 / 64;
    if (!render) printf("frequency: min %.4f Hz, mid %.3f Hz, max %.2f Hz\n", fmin, fmid, fmax);
    CHECK(fabs(fmin - 0.05) < 0.002, "min freq %.4f", fmin);
    CHECK(fabs(fmax - 20.0) / 20.0 < 0.02, "max freq %.3f", fmax);
    CHECK(fabs(fmid - 1.0) / 1.0 < 0.03, "mid freq %.3f (expected 1 Hz)", fmid);
    for (int x = 0; x < 32767; x += 97) CHECK(ql_inc(x + 97) >= ql_inc(x), "freq not monotonic at %d", x);

    {   /* Lorenz waveforms (docs/62): bounded, deterministic, chaotic at rho 28, settles at rho 20, restart repeats */
        uint32_t fast = ql_inc(32767) * 4;                           /* 20 Hz x 4 (LFO -> LFO frequency at full) */
        for (int rk = 0; rk < 8; rk++) {
            ql_st s = { 0 }; int ok = 1;
            for (long b = 0; b < 10L * 60 * BPS; b++) { int32_t v = ql_lorenz(&s, fast, rk, b & 1, 0);
                ok &= v >= -32767 && v <= 32767 && s.lz > 0 && s.lx > -60 * QL_L1 && s.lx < 60 * QL_L1; }
            CHECK(ok, "lorenz rho_k %d: out of bounds after 10 min at full speed", rk);
        }
        uint32_t inc1 = ql_inc(16384);                               /* 1 loop per second */
        ql_st a = { 0 }, c = { 0 }; int same = 1, flips = 0, prev = 0;
        for (int b = 0; b < BPS * 100; b++) {                        /* 100 s */
            int32_t va = ql_lorenz(&a, inc1, 0, 0, 0), vc = ql_lorenz(&c, inc1, 0, 0, 0);
            same &= va == vc;
            int sg = va > 3000 ? 1 : va < -3000 ? -1 : prev;
            if (prev && sg != prev) flips++;
            prev = sg;
        }
        CHECK(same, "lorenz not deterministic");
        CHECK(flips > 10 && flips < 90, "rho 28: %d wing jumps in ~100 loops (want irregular, some dozens)", flips);
        ql_st q = { 0 }; int32_t lo = 99999, hi = -99999;
        for (int b = 0; b < BPS * 120; b++) { int32_t v = ql_lorenz(&q, inc1, 7, 0, 0); if (b > BPS * 100) { if (v < lo) lo = v; if (v > hi) hi = v; } }
        CHECK(hi - lo < 1500, "rho 14 settles: last 20 s spread %d", hi - lo);
        ql_st r1 = { 0 }; int32_t first[50]; int rep = 1;
        for (int b = 0; b < 50; b++) first[b] = ql_lorenz(&r1, inc1, 0, 0, 0);
        for (int b = 0; b < 2000; b++) ql_lorenz(&r1, inc1, 0, 0, 0);
        ql_restart(&r1);
        for (int b = 0; b < 50; b++) rep &= ql_lorenz(&r1, inc1, 0, 0, 0) == first[b];
        CHECK(rep, "lorenz restart does not repeat the path");
        int16_t lw[8] = { 0 }; ql_lfo o; ql_dec(lw, 0, &o);
        for (int k = 0; k < 9; k++) ql_turn(lw, 0, 2, 1024, 1);       /* Shift + white: waveform up past the end */
        ql_dec(lw, 0, &o);
        CHECK(o.wave == 7, "waveform turn reaches lorenz z (%d)", o.wave);
        int16_t uv = lw[0]; (void)uv;
    }
    {   /* dropped fox LFO (docs/63): its saved words still load as a quad LFO, silent, and the first edit makes them quad */
        int16_t fw[8] = { 0 }; ql_put(fw, 0, 6, 31); ql_put(fw, 28, 6, 63); ql_put(fw, 120, 8, 0xA6);   /* rate, rabbits +100 % */
        ql_lfo L[4]; for (int i = 0; i < 4; i++) ql_dec(fw, i, &L[i]);
        CHECK(ql_is_quad(fw) && !ql_is_pond(fw) && L[0].amt == 0 && L[1].amt == 0 && L[0].wave == 0, "fox words: quad, silent");
        ql_turn(fw, 0, 1, 1024, 0);
        CHECK(((uint16_t)fw[7] >> 8) == 0xA5, "fox words: the first edit writes quad words");
    }
    {   /* pond LFO (docs/64) */
        pond_set f = { 31, 0, 0, 0, { 0, 1, 9, 27 }, { 31, -20, 10, 5 } }, g; int16_t pw[8] = { 0 };
        pond_put(&f, pw); pond_get(pw, &g);
        CHECK(!memcmp(&f, &g, sizeof f) && ql_is_quad(pw) && ql_is_pond(pw), "pond words round trip / recognised");
        ql_lfo L[4]; for (int i = 0; i < 4; i++) ql_dec(pw, i, &L[i]);
        CHECK(L[0].wave == 10 && L[3].wave == 13 && L[3].tgt == 27 && L[1].amt == -20 && L[2].type == QT_FREE, "pond decode");
        double cv[2] = { 0, 0 }, per0 = 0;
        for (int ci = 0; ci < 2; ci++) {                            /* chaos 0 regular, chaos 31 irregular; ~1 swing per cycle */
            memset(sh_st, 0, sizeof sh_st);
            pond_set h = f; h.chaos = (uint8_t)(ci ? 31 : 0); int16_t hw[8] = { 0 }; pond_put(&h, hw);
            for (int i = 0; i < 4; i++) ql_dec(hw, i, &L[i]);
            int up = 0, prev = 0, last = -1; double sum = 0, sum2 = 0; int ok = 1;
            for (int b = 0; b < BPS * 240; b++) {
                for (int i = 0; i < 4; i++) ql_step(&sh_st[0][i], &L[i], 65536, i);
                int32_t v = sh_st[0][0].val;
                ok &= sh_st[0][0].lx >= 1049 && sh_st[0][0].lx <= 2097152 && v >= -32767 && v <= 32767;
                int sg = v > 0;
                if (b > BPS * 20 && sg && !prev) { if (last >= 0) { double d = (b - last) / (double)BPS; sum += d; sum2 += d * d; up++; } last = b; }
                prev = sg;
            }
            double m = sum / up, sd = sqrt(fabs(sum2 / up - m * m));
            cv[ci] = sd / m; if (!ci) per0 = m;
            CHECK(ok, "pond chaos %d: in range", ci ? 31 : 0);
            if (!render) printf("pond chaos %2d: %d swings, interval %.2f +- %.2f s\n", ci ? 31 : 0, up, m, sd);
        }
        double hz = ql_inc((31 << 9) | 0x100) / 4294967296.0 * 44100 / 64;
        CHECK(fabs(per0 * hz - 1.0) < 0.15, "pond chaos 0: ~1 swing per LFO cycle (%.2f s at %.2f Hz)", per0, hz);
        CHECK(cv[0] < 0.05 && cv[1] > 0.12, "pond: chaos 0 regular (cv %.3f), chaos 31 irregular (cv %.2f)", cv[0], cv[1]);
        {   /* bounded at 4 x the top speed for 10 minutes, full chaos */
            memset(sh_st, 0, sizeof sh_st);
            pond_set h = f; h.chaos = 31; int16_t hw[8] = { 0 }; pond_put(&h, hw);
            for (int i = 0; i < 4; i++) { ql_dec(hw, i, &L[i]); L[i].inc = ql_inc(32767) * 4; }
            int ok = 1;
            for (long b = 0; b < BPS_L * 600; b++) for (int i = 0; i < 4; i++) { ql_step(&sh_st[0][i], &L[i], 65536, i); ok &= sh_st[0][i].val >= -32767 && sh_st[0][i].val <= 32767; }
            CHECK(ok && sh_st[0][0].lx >= 1049 && sh_st[0][0].d1 <= 2097152, "pond: bounded at full speed for 10 min");
        }
        {   /* controls: the selected species */
            int16_t cw[8] = { 0 }; pond_put(&f, cw);
            ql_press(0, 2); ql_turn(cw, 0, 2, 1024 * 3, 0); pond_get(cw, &g);
            CHECK(g.amt[2] == 13 && g.amt[0] == 31, "pond: white edits the selected species' amount (%d)", g.amt[2]);
            ql_turn(cw, 0, 3, 1024, 0); pond_get(cw, &g); CHECK(g.tgt[2] != 9 && g.tgt[0] == 0, "pond: red edits its target");
            ql_turn(cw, 0, 1, 1024 * 40, 0); pond_get(cw, &g); CHECK(g.chaos == 31, "pond: chaos clamps");
            ql_turn(cw, 0, 0, 1024, 1); pond_get(cw, &g); CHECK(g.sync && g.rate == 4, "pond: Shift + blue: sync");
            ql_turn(cw, 0, 1, 1024, 1); pond_get(cw, &g); CHECK(g.retrig, "pond: Shift + green: retrig");
            ql_press(0, 0);
        }
        memset(sh_st, 0, sizeof sh_st);
    }

    /* detection: packed words, midi defaults not */
    int16_t midi_def[8] = { 2000, 2000, 2000, 2000, 2000, 2000, 2000, 2000 };
    CHECK(!ql_is_quad(midi_def), "midi defaults detected as quad");
    int16_t w[8] = { fq(31, 0, 0, 0, 0), 0, 0, 0, ct(QT_VOICE, 2, 31), ct(QT_VOICE, 5, 0), ct(QT_FREE, 11, -16), ct(QT_VOICE, 4, 20) }; pk(w);
    CHECK(ql_is_quad(w), "quad words not detected");

    /* routing: LFO0 synth knob 2 full +, LFO1 env amount 0 (no call), LFO2 fx knob 3, LFO3 env A */
    n_synth = n_env = n_fx = 0; max_mod = 0;
    for (int b = 0; b < 400; b++) { host_cycles += 1161000; ql_apply_synth(host_lfo_obj[0], w, 0); ql_apply_synth(host_lfo_obj[0], w, 1); }
    CHECK(n_synth == 400, "synth knob calls %d (expected 400)", n_synth);
    CHECK(n_env == 400, "env calls %d (expected 400: LFO3 only)", n_env);
    CHECK(max_mod > 32000, "full amount reaches %d", max_mod);
    n_fx = 0;
    for (int b = 0; b < 10; b++) { host_cycles += 1161000; ql_apply_fx(0, w, 2); ql_apply_fx(0, w, 3); }
    CHECK(n_fx == 10 && last_idx == 3, "fx calls %d idx %d", n_fx, last_idx);

    /* LFO switched off in the slot: no calls, no phase movement (synth and drum) */
    {   ql_st keep = sh_st[0][2]; host_lfo_on[0] = host_lfo_on[1] = 0; n_synth = n_env = n_fx = 0; int any = 0;
        for (int b = 0; b < 50; b++) { host_cycles += 1161000; any |= ql_apply_synth(host_lfo_obj[0], w, 0) | ql_apply_synth(host_lfo_obj[0], w, 1)
                                       | ql_apply_fx(0, w, 2) | ql_apply_fx(0, w, 5) | ql_apply_drum(0, w, 3); }
        CHECK(!any && !n_synth && !n_env && !n_fx && sh_st[0][2].ph == keep.ph, "LFO off still runs (%d %d %d)", n_synth, n_env, n_fx);
        host_lfo_on[0] = host_lfo_on[1] = 1; }

    /* frequency: rate 0 / 31 / 63 */
    { int16_t q[8] = { fq(0,0,0,0,0), fq(31,0,0,0,0), fq(63,0,0,0,0), 0, ct(0,0,0), ct(0,0,0), ct(0,0,0), ct(0,0,0) }; pk(q);
      double f0 = D(q,0).inc / 4294967296.0 * 44100 / 64, f1 = D(q,1).inc / 4294967296.0 * 44100 / 64, f2 = D(q,2).inc / 4294967296.0 * 44100 / 64;
      if (!render) printf("rate 0/31/63: %.3f %.3f %.2f Hz\n", f0, f1, f2);
      CHECK(f0 > 0.049 && f0 < 0.06 && f1 > 0.8 && f1 < 1.2 && f2 > 17 && f2 < 21, "rates %.3f %.3f %.3f", f0, f1, f2); }

    /* clock calibration */
    for (int b = 0; b < 300; b++) { host_cycles += 375000; ql_apply_synth(host_lfo_obj[0], w, 0);
                                    host_cycles += 375000; ql_apply_synth(host_lfo_obj[3], w, 0); }
    CHECK(cpb > 725000 && cpb < 775000, "cpb did not converge: %u", cpb);

    /* editing: blue rate, green amount, white target wraps over 12, red type wraps over 6; Shift: offset, smooth, wave, sync */
    {
        int16_t e[8] = { fq(0,0,0,0,0), fq(0,0,0,0,0), fq(0,0,0,0,0), fq(0,0,0,0,0), ct(0,0,0), ct(0,0,0), ct(0,0,0), ct(0,0,0) }; pk(e);
        ql_lfo e0 = D(e, 0), e3 = D(e, 3);
        ql_press(0, 2);
        for (int i = 0; i < 70; i++) ql_turn(e, 0, 0, 1024, 0);
        CHECK(D(e, 2).rate == 63 && D(e, 0).rate == e0.rate && D(e, 0).tgt == e0.tgt && D(e, 3).amt == e3.amt && D(e, 3).type == e3.type && ql_is_quad(e), "rate edit of LFO 3 only");
        ql_turn(e, 0, 1, -40 * 1024, 0); CHECK(D(e, 2).amt == -31, "amount clamp %d", D(e, 2).amt);
        static const int want[28] = { 0, 1, 2, 3, 24, 25, 26, 4, 5, 6, 7, 8, 9, 10, 11, 27, 28, 29, 12, 13, 14, 15, 16, 17, 21, 22, 23, 0 };
        int seq[28], ok = 1; for (int k = 0; k < 28; k++) { seq[k] = D(e, 2).tgt; ok &= seq[k] == want[k]; ql_turn(e, 0, 2, 1024, 0); }
        CHECK(ok, "LFO 3: 27 targets, page 2 after its page 1, own 18..20 skipped (%d %d %d %d %d)", seq[4], seq[7], seq[15], seq[18], seq[24]);
        ql_turn(e, 0, 2, -2048, 0); CHECK(D(e, 2).tgt == 23, "target wraps back (%d)", D(e, 2).tgt);
        ql_turn(e, 0, 2, -3 * 1024, 0); CHECK(D(e, 2).tgt == 17, "backwards skips its own too (%d)", D(e, 2).tgt);
        {   /* drum page: 24 targets, no engine page 2 (24..26) */
            int16_t dw[8]; for (int k = 0; k < 8; k++) dw[k] = e[k];
            int dseq[25], dp2 = 0;
            for (int k = 0; k < 25; k++) { dseq[k] = D(dw, 2).tgt; dp2 |= dseq[k] >= 24 && dseq[k] <= 26; ql_turn(dw, 1, 2, 1024, 0); }
            CHECK(!dp2 && dseq[24] == dseq[0], "drum page: 24 targets, engine page 2 skipped");
        }
        for (int k = 0; k < 6; k++) ql_turn(e, 0, 3, 1024, 0);
        CHECK(D(e, 2).type == QT_VOICE, "type wraps after 6");
        ql_turn(e, 0, 3, -1024, 0); CHECK(D(e, 2).type == QT_RAND, "type wraps back -> RAND");
        ql_turn(e, 0, 0, 9 * 1024, 1); CHECK(D(e, 2).off == 1, "offset wraps 8 (%d)", D(e, 2).off);
        ql_turn(e, 0, 1, 3 * 1024, 1); ql_turn(e, 0, 2, 4 * 1024, 1);
        CHECK(D(e, 2).smooth == 3 && D(e, 2).wave == 4, "smooth / wave");
        host_tempo_rate = 1662149u * 120;
        ql_turn(e, 0, 3, 1024, 1);
        double hz = D(e, 2).inc / 4294967296.0 * 44100 / 64;
        CHECK(D(e, 2).sync && D(e, 2).rate == 4 && fabs(hz - 2.0) < 0.01, "sync 1/4 at 120 bpm: %.3f Hz", hz);
        ql_turn(e, 0, 0, 30 * 1024, 0); CHECK(D(e, 2).rate == QL_DIVS - 1, "division clamp");
        ql_turn(e, 0, 3, -1024, 1); CHECK(!D(e, 2).sync && D(e, 2).rate == 31, "sync off");
        CHECK(ql_is_quad(e), "marker kept");
    }

    /* types: GATE restarts the voice phase on note-on, VOIC does not; MONO restarts the shared phase; 1SHT stops after
     * one cycle; RAND holds a new value per note */
    {
        int16_t t[8] = { fq(40,0,0,0,0), fq(40,0,0,0,0), fq(55,0,0,0,0), fq(40,0,0,0,0),
                         ct(QT_GATE, 0, 31), ct(QT_VOICE, 1, 31), ct(QT_1SHT, 2, 31), ct(QT_RAND, 3, 31) }; pk(t);
        blocks(50, t, 1);
        CHECK(v_st[1][0].ph && v_st[1][1].ph, "voice phases run");
        uint32_t keep = v_st[1][1].ph;
        ql_trig_synth(host_lfo_obj[1], t);
        CHECK(v_st[1][0].ph == 0 && v_st[1][1].ph == keep, "GATE restarts, VOIC keeps");
        int16_t r0 = v_st[1][3].val; blocks(5, t, 1); int16_t r1 = v_st[1][3].val; blocks(5, t, 1);
        CHECK(r1 == v_st[1][3].val, "RAND holds between notes");
        ql_trig_synth(host_lfo_obj[1], t); blocks(1, t, 1);
        CHECK(v_st[1][3].val != r1 || r0 != r1, "RAND changes at note-on");
        ql_trig_synth(host_lfo_obj[1], t);
        blocks(2000, t, 1);
        CHECK(v_st[1][2].done && v_st[1][2].ph == 0xFFFFFFFFu, "1SHT stops after one cycle");
        int16_t m[8] = { fq(30,0,0,0,0), 0, 0, 0, ct(QT_MONO, 0, 31), ct(0,0,0), ct(0,0,0), ct(0,0,0) }; pk(m);
        blocks(100, m, 0);
        CHECK(sh_st[0][0].ph, "MONO runs");
        ql_trig_synth(host_lfo_obj[4], m);
        CHECK(sh_st[0][0].ph == 0, "MONO restarts on any voice's note-on");
        int16_t fx[8] = { fq(30,0,0,0,0), 0, 0, 0, ct(QT_GATE, 9, 31), ct(0,0,0), ct(0,0,0), ct(0,0,0) }; pk(fx);
        blocks(50, fx, 0); ql_trig_synth(host_lfo_obj[2], fx);
        CHECK(sh_st[0][0].ph == 0, "GATE on an FX target restarts the shared phase");
        int16_t mid[8] = { 1, 2, 3, 4, 2000, 2000, 2000, 2000 };
        ql_trig_synth(host_lfo_obj[0], mid);                      /* midi LFO words: ignored */
        /* offset: 2 = quarter cycle -> sine starts at the top */
        int16_t of[8] = { fq(0,0,0,0,2), 0, 0, 0, ct(QT_GATE, 0, 31), ct(0,0,0), ct(0,0,0), ct(0,0,0) }; pk(of);
        ql_trig_synth(host_lfo_obj[5], of); blocks(1, of, 5);
        CHECK(v_st[5][0].val > 32000, "offset 90 deg: %d", v_st[5][0].val);
    }
    /* op1r <= 15 words (per-word 0x4000 / 0x6000 markers): still quad, read as silent defaults, first edit converts */
    {
        int16_t old[8] = { 16384, 16384, 16384, 16384, 0x4000 | 2 << 10 | 255, 0x5000 | 128, 0x6000 | 31 << 6 | 63, 0x4000 | 128 };
        CHECK(ql_is_quad(old) && ql_is_old(old), "old quad words still quad");
        int silent = 1; for (int i = 0; i < 4; i++) silent &= D(old, i).a128 == 0 && D(old, i).tgt == i;
        CHECK(silent, "old words decode as silent defaults");
        n_synth = n_env = 0; for (int b = 0; b < 20; b++) { host_cycles += cpb; ql_apply_synth(host_lfo_obj[0], old, 0); ql_apply_synth(host_lfo_obj[0], old, 1); }
        CHECK(n_synth == 0 && n_env == 0, "old words modulate nothing");
        ql_press(0, 1); ql_turn(old, 0, 1, 3 * 1024, 0);
        CHECK(!ql_is_old(old) && ((uint16_t)old[7] >> 8) == QL_MARK && D(old, 1).amt == 3 && D(old, 0).amt == 0 && D(old, 3).rate == 54,
              "first edit converts the set (amt %d)", D(old, 1).amt);
    }
    /* LFO -> LFO: frequency (+-2 octaves), amount (x 0..2), phase offset (+-1/2 cycle); round trip of all fields */
    {
        ql_lfo L[4]; int16_t src[4] = { 32767, 0, 0, 0 };
        int16_t m[8] = { fq(31,0,0,0,0), fq(31,0,0,0,0), fq(31,0,0,0,0), fq(31,0,0,0,0),
                         ct(QT_FREE, 12 + 3 * 1 + 0, 31), ct(QT_FREE, 0, 20), ct(QT_FREE, 1, 20), ct(QT_FREE, 2, 20) }; pk(m);
        ql_dec_all(m, L); uint32_t inc0 = L[1].inc; ql_mod(L, src);
        CHECK(L[1].inc > inc0 * 39 / 10 && L[1].inc < inc0 * 41 / 10, "freq mod +full = x4 (%.2f)", (double)L[1].inc / inc0);
        src[0] = -32767; ql_dec_all(m, L); ql_mod(L, src);
        CHECK(L[1].inc > inc0 * 24 / 100 && L[1].inc < inc0 * 26 / 100, "freq mod -full = x1/4 (%.3f)", (double)L[1].inc / inc0);
        int16_t a[8] = { fq(31,0,0,0,0), fq(31,0,0,0,0), fq(31,0,0,0,0), fq(31,0,0,0,0),
                         ct(QT_FREE, 12 + 3 * 2 + 1, 31), ct(QT_FREE, 0, 20), ct(QT_FREE, 1, 10), ct(QT_FREE, 2, 20) }; pk(a);
        src[0] = 32767; ql_dec_all(a, L); int a0 = L[2].a128; ql_mod(L, src);
        CHECK(L[2].a128 > a0 * 19 / 10 && L[2].a128 <= 127, "amount mod +full ~ x2 (%d -> %d)", a0, L[2].a128);
        src[0] = -32767; ql_dec_all(a, L); ql_mod(L, src);
        CHECK(L[2].a128 == 0, "amount mod -full = 0 (%d)", L[2].a128);
        int16_t o[8] = { fq(31,0,0,0,0), fq(31,0,0,0,0), fq(31,0,0,0,0), fq(31,0,0,0,0),
                         ct(QT_FREE, 12 + 3 * 3 + 2, 31), ct(QT_FREE, 0, 20), ct(QT_FREE, 1, 20), ct(QT_FREE, 2, 20) }; pk(o);
        src[0] = 16384; ql_dec_all(o, L); ql_mod(L, src);
        CHECK(L[3].phoff > 0x3F000000u && L[3].phoff < 0x41000000u, "offset mod half = 1/4 cycle (%08x)", L[3].phoff);
        int16_t self[8] = { fq(31,0,0,0,0), fq(31,0,0,0,0), fq(31,0,0,0,0), fq(31,0,0,0,0),
                            ct(QT_FREE, 12, 31), ct(QT_FREE, 0, 0), ct(QT_FREE, 0, 0), ct(QT_FREE, 0, 0) }; pk(self);
        src[0] = 32767; ql_dec_all(self, L); inc0 = L[0].inc; ql_mod(L, src);
        CHECK(L[0].inc == inc0, "a stored self target is ignored");
        n_synth = 0; for (int b = 0; b < 200; b++) { host_cycles += cpb; ql_apply_synth(host_lfo_obj[0], self, 0); }
        CHECK(n_synth == 0, "LFO targets never touch knobs (%d calls)", n_synth);
        /* page 2: engine words 5..7 (plugin engines, unset 0 = 8192), FX words 5..7 (plugin FX), clamped 0..32767 */
        int16_t p2[8] = { fq(31,0,0,0,0), fq(31,0,0,0,0), fq(31,0,0,0,0), fq(31,0,0,0,0),
                          ct(QT_FREE, 24 + 1, 31), ct(QT_FREE, 27 + 2, -31), ct(QT_FREE, 0, 0), ct(QT_FREE, 0, 0) }; pk(p2);
        int mx_eng = 0, mn_eng = 32767, mx_fx = 0, mn_fx = 32767, n_other = n_synth + n_fx;
        for (int b = 0; b < 400; b++) {
            host_cycles += cpb;
            for (int k = 0; k < 8; k++) host_stage[k] = 0, host_fx_params[k] = 16000;   /* the firmware copies the words per block */
            ql_apply_synth(host_lfo_obj[0], p2, 0); ql_apply_fx(0, p2, 2);
            if (host_stage[6] > mx_eng) mx_eng = host_stage[6]; if (host_stage[6] < mn_eng) mn_eng = host_stage[6];
            if (host_fx_params[7] > mx_fx) mx_fx = host_fx_params[7]; if (host_fx_params[7] < mn_fx) mn_fx = host_fx_params[7];
            CHECK(host_stage[5] == 0 && host_stage[7] == 0 && host_fx_params[5] == 16000 && host_fx_params[6] == 16000, "page 2: only the target word");
        }
        CHECK(mn_eng >= 1 && mn_eng < 8192 && mx_eng > 8192 && mx_eng <= 32767, "engine page 2 around the unset 8192 (%d..%d)", mn_eng, mx_eng);
        CHECK(mn_fx >= 0 && mn_fx < 16000 && mx_fx > 16000 && mx_fx <= 32767, "FX page 2 around its value (%d..%d)", mn_fx, mx_fx);
        CHECK(n_synth + n_fx == n_other, "page 2 never calls the knob range functions");
        /* screens: op1_page2_live shows the modulated word while applied (engine green, synth FX white), else stored */
        CHECK(ql_page2_live(0, 0, 1, 1234) == host_stage[6] && ql_page2_live(1, 0, 2, 1234) == host_fx_params[7],
              "page 2 shown as modulated (%d / %d)", ql_page2_live(0, 0, 1, 1234), ql_page2_live(1, 0, 2, 1234));
        CHECK(ql_page2_live(0, 0, 0, 1234) == 1234 && ql_page2_live(1, 1, 2, 1234) == 1234 && ql_page2_live(1, 2, 2, 1234) == 1234,
              "page 2 not modulated: stored word (other knob, drum FX, master)");
        host_cycles += cpb * 40;                                     /* LFO stopped applying */
        CHECK(ql_page2_live(0, 0, 1, 1234) == 1234, "page 2 shown: stored word after the LFO stops");
        host_eng_plugin = host_fx_plugin = 0;
        for (int k = 0; k < 8; k++) host_stage[k] = 0, host_fx_params[k] = 16000;
        for (int b = 0; b < 50; b++) { host_cycles += cpb; ql_apply_synth(host_lfo_obj[0], p2, 0); ql_apply_fx(0, p2, 2); }
        CHECK(host_stage[6] == 0 && host_fx_params[7] == 16000, "stock engine / FX: page 2 targets do nothing");
        host_eng_plugin = host_fx_plugin = 1;
        int nd = n_drum; for (int b = 0; b < 50; b++) { host_cycles += cpb; ql_apply_drum(0, p2, 3); }
        CHECK(n_drum == nd && host_stage[6] == 0, "drums: no engine page 2");
        /* round trip: every field of every LFO */
        int16_t rt[8] = { 0 }; ql_lfo q[4];
        for (int i = 0; i < 4; i++) { q[i].rate = (uint8_t)(10 + i * 13); q[i].sync = 0; q[i].wave = (uint8_t)(i + 1); q[i].smooth = (uint8_t)(7 - i);
                                      q[i].off = (uint8_t)(i * 2 + 1); q[i].type = (uint8_t)(i + 2); q[i].tgt = (uint8_t)(23 - i * 5); q[i].amt = (int8_t)(-31 + i * 20);
                                      ql_enc(&q[i], rt, i); }
        int okrt = ql_is_quad(rt);
        for (int i = 0; i < 4; i++) { ql_lfo d = D(rt, i);
            okrt &= d.rate == q[i].rate && d.wave == q[i].wave && d.smooth == q[i].smooth && d.off == q[i].off && d.type == q[i].type && d.tgt == q[i].tgt && d.amt == q[i].amt; }
        CHECK(okrt, "pack / unpack round trip");
    }
    /* drums: pass 3 knobs, pass 4 env, pass 5 FX; drum trigger restarts the shared drum state */
    {
        int16_t d[8] = { fq(50,0,0,0,0), fq(31,0,0,0,0), fq(31,0,0,0,0), 0, ct(QT_GATE, 2, 31), ct(QT_VOICE, 5, 31), ct(QT_FREE, 11, 31), ct(0, 0, 0) }; pk(d);
        int ns = n_synth, ne = n_env, nf = n_fx; n_drum = 0;
        for (int b = 0; b < 10; b++) { host_cycles += 1161000; ql_apply_drum(0, d, 3); ql_apply_fx(0, d, 4); ql_apply_fx(0, d, 5); }
        CHECK(n_drum == 10 && drum_eng == 3 && drum_idx == 2, "drum knob: calls %d engine %d knob %d", n_drum, drum_eng, drum_idx);
        CHECK(n_synth == ns && n_env == ne && n_fx - nf == 20, "drum routing");
        ql_trig_drum(0, d);
        CHECK(sh_st[1][0].ph == 0 && sh_st[1][1].ph != 0, "drum GATE restarts, VOIC (= free) keeps");
    }

    {   /* master LFO (lfo/ql_master.h): defaults, keys, audio copy, targets, storage */
        op1r_setting_host_clear(); ml_load();
        ql_lfo o; ql_dec(ml_words(), 0, &o);
        CHECK(!ml_on && ml_type == 0 && ql_is_quad(ml_words()) && o.tgt == 8 && o.amt == 0 && o.type == QT_FREE, "master: defaults (off, quad, FX knob, amount 0)");
        int16_t live[8] = { 1000, 2000, 3000, 4000, 0, 0, 0, 0 }, keep[8]; memcpy(keep, live, sizeof keep);
        for (int k = 0; k < 4; k++) { host_fx_range[k][0] = 0; host_fx_range[k][1] = 32767; host_fx_range[k][2] = 1; }
        CHECK(ml_params(live) == live, "master off: the FX gets its own words");
        static int mscr; ql_master_scr[0] = &mscr;
        CHECK(ml_t4(0) == 1 && ml_view == 1 && PAGE_POS(&mscr) == 2 && PAGE_POS(0) == host_pos, "T4: the master LFO page");
        ml_t4(0); CHECK(ml_on && ml_dirty, "T4 again: on");
        CHECK(ml_params(live) == live, "master on, every amount 0: own words");
        ml_press(0); for (int k = 0; k < 31; k++) ml_turn(1, 1024, 0);   /* LFO 1 amount +100 % */
        ql_dec(ml_words(), 0, &o); CHECK(o.amt == 31, "green: amount (%d)", o.amt);
        int nf = n_fx, moved = 0; const int16_t *pp = 0;
        for (int b = 0; b < 400; b++) { host_cycles += cpb; pp = ml_params(live); if (pp != live && pp[0] != 1000) moved = 1; }
        CHECK(pp == ml_copy && n_fx > nf && last_p >= ml_copy && last_p < ml_copy + 8 && moved, "master: knob 1 modulated in the copy");
        CHECK(!memcmp(live, keep, sizeof keep), "master: the live words keep the set values");
        int seen[QL_NTGT] = { 0 }, bad = 0;
        for (int k = 0; k < 40; k++) { ml_turn(2, 1024, 0); ql_dec(ml_words(), 0, &o); seen[o.tgt] = 1; bad |= o.tgt < 8 || (o.tgt >= QL_P2 && o.tgt < QL_P2 + 3) || (o.tgt >= 12 && o.tgt < 15); }
        CHECK(!bad && seen[9] && seen[QL_P2 + 3] && seen[15], "master targets: FX knobs, FX page 2, other LFOs only");
        while (o.tgt != 8) { ml_turn(2, 1024, 0); ql_dec(ml_words(), 0, &o); }
        ml_turn(3, 1024 * 3, 0); ql_dec(ml_words(), 0, &o); CHECK(o.type == QT_GATE && ql_pol(2, &o) == 1, "master: red right = + (clamped)");
        ml_turn(3, -1024, 0); ql_dec(ml_words(), 0, &o); CHECK(o.type == QT_FREE && ql_pol(2, &o) == 0, "master: red back = +-");
        ml_turn(3, -1024 * 4, 0); ql_dec(ml_words(), 0, &o); CHECK(o.type == QT_MONO && ql_pol(2, &o) == -1, "master: red left = - (clamped)");
        CHECK(ql_polar(32767, 1) == 32767 && ql_polar(-32767, 1) == 0 && ql_polar(32767, -1) == -32767 && ql_polar(-32767, -1) == 0 && ql_polar(-5, 0) == -5, "polarity: unipolar ranges");
        {   /* a - LFO only moves the knob down: every copy value <= the set value */
            int hi = 0, lo = 0;
            for (int b = 0; b < 400; b++) { host_cycles += cpb; const int16_t *q = ml_params(live); if (q[0] > 1000) hi = 1; if (q[0] < 1000) lo = 1; }
            CHECK(!hi && lo, "master -: knob only below its value (hi %d lo %d)", hi, lo);
        }
        ml_turn(3, 1024, 0);                                   /* back to +- */
        CHECK(ml_t4(1) == 2 && ml_cur == 0, "Shift + T4: type list at the current type");
        ml_turn(1, 1024, 0); CHECK(ml_cur == 0, "type list: only blue moves");
        ml_turn(0, 1024 * 5, 0); CHECK(ml_cur == ML_NTYPES - 1, "type list: blue, clamped");
        CHECK(ml_t4(0) == 1 && ml_type == 1 && ql_is_pond(ml_words()) && ml_on, "T4 in the list: the chosen type's page");
        pond_set pf; pond_get(ml_words(), &pf);
        CHECK(pf.tgt[0] == 8 && pf.tgt[3] == 11 && pf.amt[0] == 0, "master pond defaults: FX knobs, amount 0");
        ml_press(1); for (int k = 0; k < 20; k++) ml_turn(2, 1024, 0);   /* pond: species 2 amount */
        pond_get(ml_words(), &pf);
        nf = n_fx; for (int b = 0; b < 200; b++) { host_cycles += cpb; ml_params(live); }
        CHECK(pf.amt[1] == 20 && n_fx > nf && !memcmp(live, keep, sizeof keep), "master pond: modulates the copy");
        {   /* pond retrig decodes as MONO: not the master's - polarity (knob both sides of its value) */
            ml_turn(1, 1024, 1); pond_get(ml_words(), &pf); ql_dec(ml_words(), 1, &o);
            int hi = 0, lo = 0;
            for (int b = 0; b < 4000; b++) { host_cycles += cpb; const int16_t *q = ml_params(live); if (q[1] > 2000) hi = 1; if (q[1] < 2000) lo = 1; }
            CHECK(pf.retrig && ql_pol(2, &o) == 0 && hi && lo, "master pond retrig: no polarity (pol %d hi %d lo %d)", ql_pol(2, &o), hi, lo);
            ml_turn(1, -1024, 1);
        }
        ml_flush();
        int16_t wq[8], wp[8]; memcpy(wq, ml_w[0], 16); memcpy(wp, ml_w[1], 16);
        memset(ml_w, 0, sizeof ml_w); ml_on = 0; ml_type = 0; ml_load();
        CHECK(ml_on && ml_type == 1 && !memcmp(wq, ml_w[0], 16) && !memcmp(wp, ml_w[1], 16), "master: saved and loaded back (on %d type %d q %d p %d)", ml_on, ml_type, memcmp(wq, ml_w[0], 16), memcmp(wp, ml_w[1], 16));
        ml_t4(0); CHECK(!ml_on, "T4 on the page: off");
        CHECK(ml_params(live) == live, "master off again: own words");
        ml_leave(); CHECK(!ml_view, "leaving: the page closes");
        {   /* rate: a 1 Hz master LFO steps one block per master render */
            op1r_setting_host_clear(); ml_load(); ml_t4(0); ml_t4(0);
            ql_lfo q; ql_dec(ml_words(), 1, &q); q.rate = 31; q.amt = 31; q.tgt = 9; ql_enc(&q, ml_words(), 1);
            uint32_t p0 = sh_st[2][1].ph, wraps = sh_st[2][1].cnt;
            for (int b = 0; b < BPS * 10; b++) { host_cycles += cpb; ml_params(live); }
            double cyc = (sh_st[2][1].cnt - wraps) + ((double)sh_st[2][1].ph - p0) / 4294967296.0;
            double want = q.inc / 4294967296.0 * BPS * 10;
            CHECK(fabs(cyc - want) < 0.05, "master rate: %.2f cycles in 10 s, want %.2f", cyc, want);
            ml_leave();
        }
        ml_view = 0;
    }
    /* per-LFO mute (Shift + push, the marker byte's bits 6 / 3 / 2 / 0): saved with the words, old words unmuted, fade, no leak */
    {
        int16_t m[8] = { fq(40,0,0,0,0), fq(31,0,0,0,0), fq(42,0,0,0,0), fq(54,0,0,0,0),
                         ct(QT_FREE, 0, 31), ct(QT_FREE, 12 + 3 * 1 + 0, 20), ct(QT_VOICE, 2, 20), ct(QT_GATE, 3, 20) }; pk(m);
        int ok = ((uint16_t)m[7] >> 8) == 0xA5;
        for (int i = 0; i < 4; i++) ok &= !D(m, i).mute;
        CHECK(ok, "an unmuted set keeps marker 0xA5 (%04X)", (uint16_t)m[7]);
        int16_t base[8]; memcpy(base, m, 16);
        int all = 1, seen[256] = { 0 };
        for (int mask = 0; mask < 16; mask++) {                   /* every combination: still quad, never pond, each bit its own LFO */
            int16_t w[8]; memcpy(w, base, 16);
            for (int i = 0; i < 4; i++) if (mask >> i & 1) ql_turn(w, 0, i, 0, 2);
            for (int i = 0; i < 4; i++) all &= D(w, i).mute == (mask >> i & 1);
            unsigned hi = (uint16_t)w[7] >> 8;
            all &= ql_is_quad(w) && !ql_is_pond(w) && !ql_is_fox(w) && !ql_is_old(w) && ql_mark_quad(hi) && !seen[hi]++;
            for (int k = 0; k < 7; k++) all &= w[k] == base[k] || k == 6 ? 1 : 0;
            ql_lfo a = D(w, 2), b = D(base, 2);                    /* the other fields are untouched */
            all &= a.rate == b.rate && a.tgt == b.tgt && a.amt == b.amt && a.type == b.type;
            for (int i = 0; i < 4; i++) if (mask >> i & 1) ql_turn(w, 0, i, 0, 2);
            all &= !memcmp(w, base, 16);                           /* a second push undoes it exactly */
        }
        CHECK(all, "mute bits: 16 combinations, each LFO its own bit, toggling back restores the words");
        { int16_t w[8]; memcpy(w, base, 16); ql_turn(w, 0, 1, 0, 2); ql_lfo o = D(w, 1); o.rate++; ql_enc(&o, w, 1);
          CHECK(D(w, 1).mute && !D(w, 0).mute && D(w, 1).rate == o.rate, "an edit of a muted LFO keeps its mute and the others'");
          ql_turn(w, 0, 3, 1024, 1); CHECK(D(w, 1).mute, "a turn keeps the mutes"); }
        { int16_t pw[8] = { 0 }, c[8]; for (int k = 0; k < 8; k++) pw[k] = (int16_t)(0x1234 * (k + 1)); pw[7] = (int16_t)(0xA700 | 0x11);
          memcpy(c, pw, 16); ql_turn(c, 0, 1, 0, 2); CHECK(!memcmp(c, pw, 16) && ql_is_pond(c), "pond words: Shift + push changes nothing"); }
        { int16_t old2[8] = { 16384, 16384, 16384, 16384, 0x4000 | 255, 0x5000 | 128, 0x6000 | 63, 0x4000 | 128 };
          ql_turn(old2, 0, 2, 0, 2); CHECK(!ql_is_old(old2) && D(old2, 2).mute && !D(old2, 0).mute && D(old2, 3).rate == 54,
                                           "an old set converts on the first mute"); }

        /* fade: LFO 1 (shared, FREE, knob blue, full amount): muting takes 32 blocks (46 ms) to reach 0, no jump, the phase keeps running */
        host_tempo_rate = 1662149u * 120;
        int16_t f[8] = { fq(40,0,0,0,0), fq(31,0,0,0,0), fq(42,0,0,0,0), fq(54,0,0,0,0),
                         ct(QT_FREE, 0, 31), ct(QT_FREE, 1, 0), ct(QT_VOICE, 2, 0), ct(QT_GATE, 3, 0) }; pk(f);
        memcpy(host_lfo_words, f, 16);
        n_synth = 0; blocks(300, host_lfo_words, 0);
        int moved = 0, prev = last_mod; max_mod = 0;
        for (int b = 0; b < 200; b++) { blocks(1, host_lfo_words, 0); if (abs(last_mod) > 2000) moved = 1; }
        CHECK(moved && max_mod > 20000, "unmuted LFO 1 moves its knob (max %d)", max_mod);
        ql_turn(host_lfo_words, 0, 0, 0, 2);                       /* mute LFO 1 */
        prev = last_mod;
        uint32_t ph0 = sh_st[0][0].ph; int worst = 0, n0 = 0;
        for (int b = 0; b < 80; b++) {
            blocks(1, host_lfo_words, 0);
            int d = abs(last_mod - prev); if (d > worst) worst = d; prev = last_mod;
            if (sh_st[0][0].mc == 32768 && !n0) n0 = b + 1;
        }
        CHECK(worst < 6000, "mute fade: largest step between calls %d (no click)", worst);
        CHECK(n0 >= 31 && n0 <= 34, "mute fade reaches 0 after %d blocks (32 = 46 ms)", n0);
        printf("mute: fade step %d (a full-scale knob jump would be %d), unmuted max mod %d\n", worst, max_mod, max_mod);
        CHECK(sh_st[0][0].mc == 32768 && sh_st[0][0].val == 0 && last_mod == 0, "muted LFO contributes 0 (mc %u, val %d, mod %d)", sh_st[0][0].mc, sh_st[0][0].val, last_mod);
        CHECK(sh_st[0][0].ph != ph0, "the muted LFO keeps its phase running");
        {                                                          /* LFO 1 aims at LFO 2's frequency: no effect once muted */
          int16_t g[8] = { fq(40,0,0,0,0), fq(31,0,0,0,0), fq(42,0,0,0,0), fq(54,0,0,0,0),
                           ct(QT_FREE, 12 + 3 * 1 + 0, 31), ct(QT_FREE, 1, 20), ct(QT_VOICE, 2, 0), ct(QT_GATE, 3, 0) }; pk(g);
          ql_lfo Lb[4]; ql_lfo g2 = D(g, 1); int steers = 0, still = 1;
          for (int b = 0; b < 300; b++) { blocks(1, g, 0); const ql_lfo *Lp = ql_params(g, 0, -1, Lb); if (Lp[1].inc != g2.inc) steers = 1; }
          ql_turn(g, 0, 0, 0, 2);
          for (int b = 0; b < 80; b++) { blocks(1, g, 0); const ql_lfo *Lp = ql_params(g, 0, -1, Lb); if (b > 40 && Lp[1].inc != g2.inc) still = 0; }
          CHECK(steers && still, "a muted LFO steers nothing (steers before %d, after the fade %d)", steers, still); }
        ql_turn(host_lfo_words, 0, 0, 0, 2);                       /* unmute: back to moving, smoothly */
        blocks(1, host_lfo_words, 0); moved = 0; max_mod = 0; worst = 0; prev = last_mod;
        for (int b = 0; b < 300; b++) { blocks(1, host_lfo_words, 0); int d = abs(last_mod - prev); if (d > worst) worst = d; prev = last_mod; if (abs(last_mod) > 20000) moved = 1; }
        CHECK(moved && !sh_st[0][0].mc && worst < 6000, "unmuted again: moves, gain back to 1, step %d", worst);
    }
    /* a voice LFO (VOIC, knob green = LFO 3) muted: a new note starts muted (no 46 ms leak), the others still run */
    {
        int16_t m[8] = { fq(40,0,0,0,0), fq(40,0,0,0,0), fq(45,0,0,0,0), fq(54,0,0,0,0),
                         ct(QT_FREE, 0, 31), ct(QT_FREE, 1, 31), ct(QT_VOICE, 2, 31), ct(QT_GATE, 3, 31) }; pk(m);
        ql_turn(m, 0, 2, 0, 2); ql_turn(m, 0, 3, 0, 2);            /* LFO 3 (VOIC) and 4 (GATE) muted, 1 and 2 not */
        memcpy(host_lfo_words, m, 16);
        v_st[2][2].mc = 0; v_st[2][3].mc = 0;
        ql_trig_synth(host_lfo_obj[2], host_lfo_words);
        CHECK(v_st[2][2].mc == 32768 && v_st[2][3].mc == 32768 && v_st[2][0].mc == 0, "a voice starts at its LFOs' mute state");
        int a = 0, b2 = 0; max_mod = 0;
        for (int k = 0; k < 400; k++) { blocks(1, host_lfo_words, 2); a |= abs(sh_st[0][0].val) > 20000; b2 |= abs(sh_st[0][1].val) > 20000; }
        CHECK(a && b2 && v_st[2][2].val == 0 && v_st[2][3].val == 0, "muted voice LFOs stay at 0, the others move");
    }

    if (!render) { printf(fails ? "%d FAILED\n" : "OK: all quad LFO checks pass\n", fails); return fails != 0; }

    /* screen frames */
    struct { const char *label; int pos, sel; int16_t w[8]; int blocks; } st[] = {
        { "synth: defaults (amount 0, VOIC, knobs b g w r)", 0, 0, { fq(19,0,0,0,0), fq(31,0,0,0,0), fq(42,0,0,0,0), fq(54,0,0,0,0),
          ct(QT_VOICE, 0, 0), ct(QT_VOICE, 1, 0), ct(QT_VOICE, 2, 0), ct(QT_VOICE, 3, 0) }, 50 },
        { "synth: knob b FREE, env A GATE, fx r 1/8T MONO, env S 1SHT; LFO2 selected", 0, 1, { fq(30,0,0,0,0), fq(45,1,0,0,0), fq(7,0,1,0,0), fq(20,3,0,0,0),
          ct(QT_FREE, 0, 25), ct(QT_GATE, 4, -12), ct(QT_MONO, 11, 31), ct(QT_1SHT, 6, 18) }, 37 },
        { "drum page: drum knob g RAND, fx w FREE; LFO1 selected", 1, 0, { fq(28,5,0,0,0), fq(40,0,0,0,0), fq(12,0,0,0,0), fq(4,0,1,0,0),
          ct(QT_RAND, 1, 20), ct(QT_FREE, 10, -31), ct(QT_GATE, 7, 9), ct(QT_VOICE, 3, 0) }, 60 },
        { "LFO targets: LFO1 -> freq of LFO2 (F, green dot), LFO3 -> amount of LFO4 (A), LFO4 -> offset of LFO1 (O)", 0, 0,
          { fq(12,0,0,0,0), fq(40,0,0,0,0), fq(20,3,0,0,0), fq(45,0,0,0,0),
            ct(QT_FREE, 12 + 3 * 1 + 0, 25), ct(QT_FREE, 0, 20), ct(QT_FREE, 12 + 3 * 3 + 1, -20), ct(QT_VOICE, 12 + 3 * 0 + 2, 31) }, 70 },
        { "page 2: LFO1 -> engine page 2 green, LFO2 -> engine knob green, LFO3 -> FX page 2 white, LFO4 -> FX white", 0, 0,
          { fq(25,0,0,0,0), fq(36,0,0,0,0), fq(18,0,0,0,0), fq(44,0,0,0,0),
            ct(QT_FREE, 24 + 1, 25), ct(QT_VOICE, 1, 20), ct(QT_FREE, 27 + 2, -18), ct(QT_FREE, 10, 31) }, 55 },
        { "SHIFT: offset / smoothing / waveform / sync; LFO2 selected", 0, 1, { fq(30,0,0,0,0), fq(7,1,1,2,2), fq(50,4,0,5,4), fq(63,5,0,0,7),
          ct(QT_FREE, 0, 25), ct(QT_GATE, 4, -12), ct(QT_MONO, 11, 31), ct(QT_1SHT, 6, 18) }, 60 },
        { "lorenz: LFO1 x (rho 28) -> knob b, LFO2 z (rho 28) -> fx g, LFO3 x (rho 20) GATE -> env A, LFO4 sine", 0, 0,
          { fq(36,6,0,0,0), fq(30,7,0,0,0), fq(40,6,0,0,4), fq(31,0,0,0,0),
            ct(QT_FREE, 0, 28), ct(QT_FREE, 9, 24), ct(QT_GATE, 4, 20), ct(QT_VOICE, 3, 10) }, 400 },
        { "SHIFT: lorenz x R28, lorenz z R24, lorenz x R14, sine 90 deg", 0, 0,
          { fq(36,6,0,0,0), fq(30,7,0,1,2), fq(40,6,0,0,7), fq(31,0,0,0,2),
            ct(QT_FREE, 0, 28), ct(QT_FREE, 9, 24), ct(QT_GATE, 4, 20), ct(QT_VOICE, 3, 10) }, 60 } };
    host_tempo_rate = 1662149u * 120;
    ql_screen_init(0);
    for (unsigned k = 0; k < sizeof st / sizeof st[0]; k++) {
        memcpy(host_lfo_words, st[k].w, 16); pk(host_lfo_words);
        host_pos = st[k].pos;
        ql_sel[st[k].pos] = (uint8_t)st[k].sel;
        for (int b = 0; b < st[k].blocks; b++) { host_cycles += cpb; ql_apply_synth(host_lfo_obj[0], host_lfo_words, 0); ql_apply_fx(0, host_lfo_words, 2); ql_apply_drum(0, host_lfo_words, 3); }
        host_shift = !strncmp(st[k].label, "SHIFT", 5);
        ql_screen_update(0, 16);
        frame_json(st[k].label);
    }
    {   /* pond LFO frames */
        struct { const char *label; int chaos, sel, shift; } ps[] = {
            { "POND: chaos 0 %, species 1 selected", 0, 0, 0 }, { "POND: chaos 75 %, species 2 selected", 23, 1, 0 },
            { "POND: chaos 100 %, species 4 selected", 31, 3, 0 }, { "POND SHIFT: clock / mode", 31, 0, 1 } };
        for (unsigned k = 0; k < sizeof ps / sizeof ps[0]; k++) {
            pond_set f = { 31, 0, 0, (uint8_t)ps[k].chaos, { 0, 1, 9, 27 }, { 25, -20, 10, 31 } };
            memset(host_lfo_words, 0, 16); pond_put(&f, host_lfo_words); memset(sh_st, 0, sizeof sh_st);
            memset((void *)pond_trail, 0, sizeof pond_trail);
            host_pos = 0; ql_sel[0] = (uint8_t)ps[k].sel;
            for (int b = 0; b < BPS * 40; b++) { host_cycles += cpb; ql_apply_synth(host_lfo_obj[0], host_lfo_words, 0); }
            host_shift = ps[k].shift;
            pd_screen_update(0, 16);
            frame_json(ps[k].label);
        }
        host_shift = 0; ql_sel[0] = 0;
    }
    {   /* master LFO frames: the type list, the quad page (master FX knobs, LFO targets), the pond page */
        static int mq, mp; ql_master_scr[0] = &mq; ql_master_scr[1] = &mp;
        op1r_setting_host_clear(); ml_load(); host_pos = 0; int16_t live[8] = { 8000, 8000, 8000, 8000, 0, 0, 0, 0 };
        ml_t4(1); ml_list_update(0, 16); frame_json("MASTER LIST: Shift + T4 (quad chosen)");
        ml_turn(0, 1024, 0); ml_list_update(0, 16); frame_json("MASTER LIST: blue turned (pond)");
        ml_cur = 0; ml_t4(0); ml_t4(0);                        /* quad page, on */
        int16_t *w = ml_words();
        ql_lfo o;
        for (int i = 0; i < 4; i++) { ql_dec(w, i, &o); o.amt = (int8_t)(i == 2 ? 0 : 31 - 7 * i); if (i == 3) o.tgt = 12; ql_enc(&o, w, i); }
        ql_sel[2] = 1;
        for (int b = 0; b < 300; b++) { host_cycles += cpb; ml_params(live); }
        ql_screen_update(&mq, 16); frame_json("MASTER QUAD: on; LFO1..3 -> master FX b g w, LFO4 -> LFO1 frequency; LFO2 selected");
        ql_dec(w, 0, &o); o.type = QT_GATE; ql_enc(&o, w, 0); ql_dec(w, 1, &o); o.type = QT_MONO; ql_enc(&o, w, 1);
        for (int b = 0; b < 60; b++) { host_cycles += cpb; ml_params(live); }
        ql_screen_update(&mq, 16); frame_json("MASTER QUAD: polarity (red) LFO1 +, LFO2 -, LFO3/4 +-");
        ql_dec(w, 0, &o); o.type = QT_FREE; ql_enc(&o, w, 0); ql_dec(w, 1, &o); o.type = QT_FREE; ql_enc(&o, w, 1);
        ml_t4(0); ql_screen_update(&mq, 16); frame_json("MASTER QUAD: off (T4 on the page)");
        ml_t4(0); ml_t4(1); ml_turn(0, 1024, 0); ml_t4(0);    /* on again, type list, pond */
        pond_set f; pond_get(ml_words(), &f); f.amt[0] = 25; f.amt[1] = -20; f.amt[2] = 10; f.amt[3] = 31; f.chaos = 23; pond_put(&f, ml_words());
        for (int b = 0; b < BPS * 40; b++) { host_cycles += cpb; ml_params(live); }
        pd_screen_update(&mp, 16); frame_json("MASTER POND: chaos 75 %, species 1..4 -> master FX knobs");
        ml_leave();
    }
    return 0;
}
