/* Host test for plugins/ente (sidechain compressor, docs/61): static curve on SELF, ducking by a tape track (stand-in
 * op1_sc_source), linked stereo gain on the master, BEAT pulses per division at 120 BPM, attack / release, makeup,
 * no gain change on a silent source.
 *   ./ente_test               run the checks
 *   ./ente_test screen <n>    one screen state as JSON lines (plugins/ente/preview.py renders them) */
#include "../op1_synth.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
static int raster_obj, nlines;
static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } lines[4096];
void *host_raster(void) { return &raster_obj; }
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c)
{
    const op1_pt *a = p0, *b = p1; (void)r;
    if (nlines == 4096) return;
    lines[nlines].x0 = a->x; lines[nlines].y0 = a->y; lines[nlines].x1 = b->x; lines[nlines].y1 = b->y;
    lines[nlines].w = w; memcpy(lines[nlines].c, c, 4); nlines++;
}
typedef struct { int32_t x, y, size; uint8_t c[4]; char s[32]; } htext_t;   /* stock font stand-in (as screen_harness.c) */
static htext_t *texts[64]; static int ntexts;
void *host_text_ctor(void *obj, int32_t x_q10, int32_t y, int32_t size, const uint8_t *rgba)
{ htext_t *t = obj; t->x = x_q10; t->y = y; t->size = size; memcpy(t->c, rgba, 4); t->s[0] = 0; return obj; }
void host_text_set(void *t, const char *s) { snprintf(((htext_t *)t)->s, 32, "%s", s); }
void host_text_draw(void *t, void *r, const int32_t *m9) { (void)r; (void)m9; if (ntexts < 64) texts[ntexts++] = t; }
static int16_t sc_buf[8][64]; static int sc_asked;              /* stand-in core sources */
int op1_sc_source(int src, int16_t *out) { sc_asked = src; memcpy(out, sc_buf[src & 7], 128); return 1; }

#include "../plugins/ente/ente.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int16_t P[8];
static void knobs(int thr, int ratio, int rel, int src_k, int att, int mk, int w7)
{ P[0] = (int16_t)thr; P[1] = (int16_t)ratio; P[2] = (int16_t)rel; P[3] = (int16_t)src_k; P[4] = 0x5308; P[5] = (int16_t)att; P[6] = (int16_t)mk; P[7] = (int16_t)w7; }
static int srcd(int pos, int s, int d)                         /* red word (host: 0..32767) of the entry (source s, division d) */
{
    (void)pos;
    for (int e = 0; e < NENT; e++) if (ent_src[e] == s && (s != S_BEAT || div_of(e) == d)) return op1_fx_list_word(e, NENT, 0, 32767);
    printf("no entry %d/%d\n", s, d); exit(1);
}
static int srck(int pos, int s) { return srcd(pos, s, 2); }
static double db(double a) { return 20 * log10(a / 32767.0); }

/* run n blocks of a sine (amp) through pos, returns the output peak of the last 20 blocks */
static int run_sine(int pos, int amp, int nb, int16_t *sc_track, int sc_amp)
{
    static double ph; int pk = 0;
    for (int b = 0; b < nb; b++) {
        int16_t l[64], r[64];
        for (int i = 0; i < 64; i++, ph += 2 * M_PI * 441 / 44100) {
            l[i] = r[i] = (int16_t)(amp * sin(ph));
            if (sc_track) sc_track[i] = (int16_t)(sc_amp * sin(ph * 1.5));
        }
        if (pos == OP1_FX_MASTER) ente_render_stereo(pos, l, r, P); else ente_render(pos, l, P);
        if (b >= nb - 20) for (int i = 0; i < 64; i++) { int a = abs(l[i]); if (a > pk) pk = a; }
    }
    return pk;
}

static void tests(void)
{
    /* SELF, threshold -24 dB, ratio inf: a 0 dB sine comes out near -24 dB */
    ente_init(0); knobs(16384, 32767, 12000, srck(0, S_SELF), 0, 0, 0);
    int pk = run_sine(0, 32000, 400, 0, 0);
    CHECK(fabs(db(pk) + 24) < 2.0, "SELF inf ratio: %.1f dB (want -24)", db(pk));
    /* ratio 2:1 (rq8 = 512: k1 = 32767 * sqrt(1 / 19)): 24 dB over -> 12 dB over */
    ente_init(0); knobs(16384, 7518, 12000, srck(0, S_SELF), 0, 0, 0);
    pk = run_sine(0, 32000, 400, 0, 0);
    CHECK(fabs(db(pk) + 12) < 2.0, "SELF 2:1: %.1f dB (want -12)", db(pk));
    /* below threshold: unity (also makeup 0) */
    ente_init(0); knobs(16384, 32767, 12000, srck(0, S_SELF), 0, 0, 0);
    pk = run_sine(0, 1000, 200, 0, 0);
    CHECK(abs(pk - 1000) < 30, "below threshold: %d (want 1000)", pk);
    /* makeup: word 32767 = +24 dB, half = +12 dB, on a quiet signal */
    ente_init(0); knobs(16384, 32767, 12000, srck(0, S_SELF), 0, 32767, 0);
    pk = run_sine(0, 1000, 200, 0, 0);
    CHECK(fabs(db(pk) - db(1000) - 24) < 0.9, "makeup max: %.1f dB over the input (want 24)", db(pk) - db(1000));
    ente_init(0); knobs(16384, 32767, 12000, srck(0, S_SELF), 0, 16384, 0);
    pk = run_sine(0, 1000, 200, 0, 0);
    CHECK(fabs(db(pk) - db(1000) - 12) < 0.7, "makeup half: %.1f dB over the input (want 12)", db(pk) - db(1000));
    /* +24 dB on a loud signal must clip, not wrap (16x gain; the multiply is split) */
    ente_init(0); knobs(16384, 32767, 12000, srck(0, S_SELF), 0, 32767, 0);
    pk = run_sine(0, 30000, 100, 0, 0);
    CHECK(pk >= 32000, "makeup +24 on a loud sine clips, peak %d", pk);

    /* T1 ducks the instrument: loud track -> quiet output, silent track -> unity; the source asked for is T1 */
    ente_init(0); knobs(16384, 32767, 12000, srck(0, S_T1), 0, 0, 0);
    pk = run_sine(0, 8000, 300, sc_buf[OP1_SC_T1], 32000);
    CHECK(sc_asked == OP1_SC_T1 && db(pk) < db(8000) - 18, "T1 ducking: %.1f dB (input %.1f)", db(pk), db(8000));
    pk = run_sine(0, 8000, 400, sc_buf[OP1_SC_T1], 0);
    CHECK(abs(pk - 8000) < 200, "T1 silent: released to %d", pk);
    /* the source list: synth / drum have no INST; the top of the knob is BEAT there, INST on the master */
    {   /* list knob: the core's step (entry i at i / 15 of the raw range 8192..32767, as dispatcher.c) decodes back
         * to i for every entry, also +- 300 raw of jitter; +1 from the last entry wraps to the first */
        int ok = 1;
        for (int e = 0; e < NENT; e++) {
            int32_t w = op1_fx_list_word(e, NENT, 8192, 32767);
            for (int j = -300; j <= 300; j += 300) {
                int32_t q = (w + j - 8192) * 32767 / (32767 - 8192); q = q < 0 ? 0 : q > 32767 ? 32767 : q;
                ok &= ent_of(q) == e;
            }
        }
        CHECK(ok, "list knob: every entry round-trips");
        CHECK((15 + 1 + NENT) % NENT == 0 && ent_of(0) == 0 && ent_of(32767) == NENT - 1, "list ends / wrap");
        CHECK(ent_src[7] == S_INST && ent_src[8] == S_BEAT && div_of(8) == 0 && div_of(15) == 7, "list order");
        CHECK(!strcmp(div_name[5], "1/4t"), "lowercase triplets");
    }
    {   /* the former defaults (thresh -36 dB, 8:1; plugin.json now starts at 0 dB) at a real track level (peak 2900 = -21 dBFS, measured in op1emu): >= 10 dB duck */
        ente_init(0); knobs(8192, 19840, 12000, srck(0, S_T1), 4000, 0, 9000);
        int pk = run_sine(0, 8000, 300, sc_buf[OP1_SC_T1], 2900);
        CHECK(db(8000) - db(pk) >= 10, "defaults, track at -21 dBFS: %.1f dB of ducking", db(8000) - db(pk));
    }
    {   /* INST on synth / drum = its own input (SELF): no core source asked */
        ente_init(0); knobs(16384, 32767, 12000, srck(0, S_INST), 0, 0, 0); sc_asked = 0;
        int pk = run_sine(0, 32000, 400, 0, 0);
        CHECK(sc_asked == 0 && fabs(db(pk) + 24) < 2.0, "INST on synth = SELF: asked %d, %.1f dB", sc_asked, db(pk));
    }
    /* master: linked stereo gain (L loud, R quiet: R gets the same reduction) */
    ente_init(2); knobs(16384, 32767, 12000, srck(2, S_SELF), 0, 0, 0);
    {
        int pl = 0, pr = 0;
        for (int b = 0; b < 400; b++) {
            int16_t l[64], r[64];
            for (int i = 0; i < 64; i++) { double s = sin(2 * M_PI * 441 * (b * 64 + i) / 44100.0); l[i] = (int16_t)(32000 * s); r[i] = (int16_t)(4000 * s); }
            ente_render_stereo(2, l, r, P);
            if (b >= 380) for (int i = 0; i < 64; i++) { if (abs(l[i]) > pl) pl = abs(l[i]); if (abs(r[i]) > pr) pr = abs(r[i]); }
        }
        CHECK(fabs(db(pl) - db(pr) - db(32000) + db(4000)) < 0.3, "linked: L/R ratio %.2f dB (want %.2f)", db(pl) - db(pr), db(32000) - db(4000));
        CHECK(db(pl) < -20, "master compressed: %.1f dB", db(pl));
    }

    /* INST on the master asks the core for OP1_SC_INST */
    ente_init(2); knobs(16384, 32767, 12000, srck(2, S_INST), 0, 0, 0); sc_asked = 0;
    run_sine(2, 8000, 2, sc_buf[OP1_SC_INST], 0);
    CHECK(sc_asked == OP1_SC_INST, "INST source: asked %d", sc_asked);

    /* BEAT: pulses per second at 120 BPM per division (clock running in real time) */
    static const int want[NDIV] = { 0, 0, 2, 4, 8, 0, 6, 12 };            /* per second; 1/1, 1/2, 1/4T: per 2 s below */
    for (int d = 0; d < NDIV; d++) {
        ente_init(0); knobs(16384, 32767, 2000, srcd(0, S_BEAT, d), 0, 0, 0);
        op1_beat_t *bc = host_beat(); bc->len = 22050 << 14; bc->next = 5000 << 14; bc->step = 0x100000; bc->follows_tape = 0;
        int n = 0, nb = 44100 * 4 / 64;                                    /* 4 s */
        uint8_t t0 = st[0].trig;
        for (int b = 0; b < nb; b++) {
            int16_t l[64]; for (int i = 0; i < 64; i++) l[i] = 8000;
            ente_render(0, l, P);
            bc->next -= bc->step; if (bc->next < 0) bc->next += bc->len;
        }
        n = (uint8_t)(st[0].trig - t0);
        int w = d == 0 ? 2 : d == 1 ? 4 : d == 5 ? 12 : want[d] * 4;     /* in 4 s: 8 beats; 1/4T = 3 per 2 beats */
        CHECK(abs(n - w) <= 1, "BEAT %s: %d pulses in 4 s (want %d)", div_name[d], n, w);
    }
    /* BEAT pumps: right after a pulse the gain is down, a beat later it is back */
    {
        ente_init(0); knobs(16384, 32767, 2000, srcd(0, S_BEAT, 2), 0, 0, 0);   /* 1/4 */
        op1_beat_t *bc = host_beat(); bc->len = 22050 << 14; bc->next = 64 << 14; bc->step = 0x100000;
        int16_t l[64]; int mn = 99999, last = 0;
        for (int b = 0; b < 300; b++) {
            for (int i = 0; i < 64; i++) l[i] = 8000;
            ente_render(0, l, P);
            bc->next -= bc->step; if (bc->next < 0) bc->next += bc->len;
            if (b > 2 && b < 40 && l[63] < mn) mn = l[63];
            if (b == 340 - 1 - 40) last = l[63];
        }
        CHECK(mn < 2000 && last > 7000, "BEAT pump: min %d, before the next beat %d", mn, last);
    }
    /* clock stands still: no pulses */
    {
        ente_init(0); knobs(16384, 32767, 2000, srcd(0, S_BEAT, 2), 0, 0, 0);
        op1_beat_t *bc = host_beat(); bc->len = 22050 << 14; bc->next = 64 << 14; bc->step = 0;
        uint8_t t0 = st[0].trig;
        for (int b = 0; b < 100; b++) { int16_t l[64] = { 0 }; ente_render(0, l, P); }
        CHECK(st[0].trig == t0, "stopped clock: no pulses");
    }
    /* attack: fast attack reacts within a block, slow one (50 ms) does not */
    {
        ente_init(0); knobs(16384, 32767, 12000, srck(0, S_T1), 0, 0, 0);
        int16_t l[64]; for (int i = 0; i < 64; i++) { l[i] = 8000; sc_buf[OP1_SC_T1][i] = 32000; }
        for (int b = 0; b < 3; b++) { for (int i = 0; i < 64; i++) l[i] = 8000; ente_render(0, l, P); }
        int fast = l[63];
        ente_init(0); knobs(16384, 32767, 12000, srck(0, S_T1), 32767, 0, 0);
        for (int b = 0; b < 3; b++) { for (int i = 0; i < 64; i++) l[i] = 8000; ente_render(0, l, P); }
        CHECK(fast < 2000 && l[63] > 5000, "attack: fast %d, slow %d", fast, l[63]);
    }
    /* screen: the duck flattens with the gain reduction, all lines on screen, above y 140 */
    {
        knobs(16384, 24000, 12000, srck(0, S_SELF), 4000, 0, 9000); memcpy(host_fx_knobs[0], P, sizeof P);
        st[0].gr = 0; shown = 0; nlines = ntexts = 0; ente_screen_update(0, 16);
        int32_t top0 = 1 << 30; for (int i = 0; i < nlines; i++) { if (lines[i].x0 > Q(64) && lines[i].y0 < top0) top0 = lines[i].y0; }   /* the duck, not the text */
        for (int f = 0; f < 40; f++) { st[0].gr = 4096; nlines = ntexts = 0; ente_screen_update(0, 16); }
        int32_t top1 = 1 << 30, ok = 1;
        for (int i = 0; i < nlines; i++) {
            if (lines[i].c[1] == 0xED) continue;                           /* the bar */
            if (lines[i].x0 > Q(64) && lines[i].y0 < top1) top1 = lines[i].y0;
            ok &= lines[i].x0 >= 0 && lines[i].x0 <= Q(320) && lines[i].y0 >= 0 && lines[i].y0 <= Q(140);
        }
        CHECK(ok && top1 > top0 + Q(30), "screen: top %d -> %d px, in bounds %d", top0 >> 10, top1 >> 10, ok);
        CHECK(nlines < 200, "screen: %d lines", nlines);
        ntexts = 0; ente_screen_update(0, 16);
        CHECK(ntexts == 8 && !strcmp(texts[0]->s, "thresh") && !strcmp(texts[1]->s, "-24 db") && !strcmp(texts[7]->s, "self"),
              "bottom row: %d texts, %s / %s / %s", ntexts, ntexts ? texts[0]->s : "", ntexts > 1 ? texts[1]->s : "", ntexts > 7 ? texts[7]->s : "");
        /* the plugin's defaults (thresh word 32767 = 0 dB) and makeup readout: max word = +24 db, half = +12 db */
        knobs(32767, 19840, 12000, srck(0, S_SELF), 4000, 32767, 9000); memcpy(host_fx_knobs[0], P, sizeof P);
        ntexts = 0; ente_screen_update(0, 16);
        CHECK(ntexts == 8 && !strcmp(texts[1]->s, "0 db"), "default threshold readout: %s", ntexts > 1 ? texts[1]->s : "");
        op1_fx_page[0][1] = 1; ntexts = 0; ente_screen_update(0, 16);
        CHECK(ntexts == 8 && !strcmp(texts[3]->s, "+24 db"), "makeup max readout: %s", ntexts > 3 ? texts[3]->s : "");
        P[6] = 16384; memcpy(host_fx_knobs[0], P, sizeof P); ntexts = 0; ente_screen_update(0, 16);
        CHECK(!strcmp(texts[3]->s, "+12 db"), "makeup half readout: %s", texts[3]->s);
        op1_fx_page[0][1] = 0;
    }
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "screen")) {                        /* preview states */
        int n = atoi(argv[2]);
        static const struct { int gr, src, w7, page; } S[] = {
            { 0, S_T1, 9000, -1 }, { 1500, S_SELF, 9000, -1 }, { 4096, S_BEAT, 3, -1 },
            { 800, S_IN, 9000, 2 }, { 2600, S_TAPE, 9000, 0 }, { 0, S_T3, 9000, 1 } };
        int pos = 0;                                                       /* host screen: pos 0 */
        knobs(8192, 19840, 12000, srcd(pos, S[n].src, S[n].src == S_BEAT ? S[n].w7 : 2), 4000, 9000, 9000);
        memcpy(host_fx_knobs[0], P, sizeof P);
        memset((void *)op1_fx_page, 0, sizeof op1_fx_page);
        if (S[n].page >= 0) op1_fx_page[0][S[n].page] = 1;
        ente_screen_init(0);
        for (int f = 0; f < 40; f++) { st[0].gr = S[n].gr; nlines = ntexts = 0; ente_screen_update(0, 16); }
        printf("{\"lines\": [");
        for (int i = 0; i < nlines; i++)
            printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", lines[i].x0, lines[i].y0, lines[i].x1, lines[i].y1,
                   lines[i].w, lines[i].c[0], lines[i].c[1], lines[i].c[2], lines[i].c[3]);
        printf("], \"texts\": [");
        for (int i = 0; i < ntexts; i++)
            printf("%s[%d, %d, %d, [%d, %d, %d, %d], \"%s\"]", i ? ", " : "", texts[i]->x, texts[i]->y, texts[i]->size,
                   texts[i]->c[0], texts[i]->c[1], texts[i]->c[2], texts[i]->c[3], texts[i]->s);
        printf("]}\n");
        return 0;
    }
    tests();
    printf(fails ? "%d failed\n" : "ente: all ok\n", fails);
    return fails != 0;
}
