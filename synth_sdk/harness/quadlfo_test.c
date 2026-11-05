/* Host test of the quad LFO (lfo/quadlfo.c) with the firmware calls mocked.
 *   cc -DOP1_HOST -I.. -Iharness quadlfo_test.c -lm -o build/quadlfo_test && build/quadlfo_test [render]
 * Checks: frequency range, routing per pass (synth knobs pass 0, envelope pass 1, FX in apply_fx),
 * amount 0 = no call, modulation depth, quad detection vs. midi LFO defaults, encoder editing.
 * With "render": prints JSON frames (screen states) for harness/render_quadlfo.py. */
#include "../lfo/quadlfo.c"
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
void host_fx_knob_mod(int16_t *m, const int16_t *rg, int16_t *p) { n_fx++; last_idx = (int)(p - host_fx_params); last_mod = *m; (void)rg; }
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

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
/* current-format words: freq(rate 0..63, wave, sync, smooth, offset), ctrl(type, target 0..11, amount -31..31) */
static int16_t fq(int rate, int wave, int sync, int smooth, int off) { return (int16_t)(wave << 13 | sync << 12 | smooth << 9 | off << 6 | rate); }
static int16_t ct(int type, int tgt, int amt) { return (int16_t)(type << 11 | tgt << 6 | (amt + 32)); }
/* the tests write LFO i as fq(...) in w[i] and ct(...) in w[4 + i] (readable); pk() packs that into the real
 * 128-bit layout through ql_enc */
static void pk(int16_t *w)
{
    ql_lfo o[4]; int16_t out[8] = { 0 };
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
    printf("]}\n");
}

int main(int argc, char **argv)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    static int objs[6]; for (int i = 0; i < 6; i++) host_lfo_obj[i] = &objs[i];
    int render = argc > 1 && !strcmp(argv[1], "render");

    /* frequency range: inc per 128-sample block -> Hz */
    double fmin = ql_inc(0) / 4294967296.0 * 44100 / 128, fmax = ql_inc(32767) / 4294967296.0 * 44100 / 128;
    double fmid = ql_inc(16384) / 4294967296.0 * 44100 / 128;
    if (!render) printf("frequency: min %.4f Hz, mid %.3f Hz, max %.2f Hz\n", fmin, fmid, fmax);
    CHECK(fabs(fmin - 0.05) < 0.002, "min freq %.4f", fmin);
    CHECK(fabs(fmax - 20.0) / 20.0 < 0.02, "max freq %.3f", fmax);
    CHECK(fabs(fmid - 1.0) / 1.0 < 0.03, "mid freq %.3f (expected 1 Hz)", fmid);
    for (int x = 0; x < 32767; x += 97) CHECK(ql_inc(x + 97) >= ql_inc(x), "freq not monotonic at %d", x);

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
      double f0 = D(q,0).inc / 4294967296.0 * 44100 / 128, f1 = D(q,1).inc / 4294967296.0 * 44100 / 128, f2 = D(q,2).inc / 4294967296.0 * 44100 / 128;
      if (!render) printf("rate 0/31/63: %.3f %.3f %.2f Hz\n", f0, f1, f2);
      CHECK(f0 > 0.049 && f0 < 0.06 && f1 > 0.8 && f1 < 1.2 && f2 > 17 && f2 < 21, "rates %.3f %.3f %.3f", f0, f1, f2); }

    /* clock calibration */
    for (int b = 0; b < 300; b++) { host_cycles += 750000; ql_apply_synth(host_lfo_obj[0], w, 0);
                                    host_cycles += 750000; ql_apply_synth(host_lfo_obj[3], w, 0); }
    CHECK(cpb > 1450000 && cpb < 1550000, "cpb did not converge: %u", cpb);

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
        double hz = D(e, 2).inc / 4294967296.0 * 44100 / 128;
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
          ct(QT_FREE, 0, 25), ct(QT_GATE, 4, -12), ct(QT_MONO, 11, 31), ct(QT_1SHT, 6, 18) }, 60 } };
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
    return 0;
}
