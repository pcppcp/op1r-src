/* Host test for plugins/sitting ("in a room") with a model of the stock metronome's beat clock (phase counts down
 * 0x100000 per 64-sample block, or the tape speed step; wraps by the beat length; a tempo change rescales the phase,
 * as metronome::set_tempo does). Checks the tempo-synced capture: start on the grid point, loop length = musical
 * length to the sample, phase lock over many generations, tempo changes, the 11.025 kHz rate, bars from the tape,
 * plus level / overflow / garbage checks. Build (UBSan traps):
 *   cc -O1 -g -DOP1_HOST -fsanitize=undefined -I . harness/sitting_test.c -lm -o sitting_test.exe
 *   ./sitting_test.exe                 run the tests
 *   ./sitting_test.exe screen <n>      one screen state as JSON (plugins/sitting/preview.py renders them) */
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

#include "../plugins/sitting/sitting.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the metronome clock model ---- */
static struct { int32_t len, phase, step, follows; int64_t t; int32_t tape_pos; int tape_run; } M;   /* t: samples */
static int32_t bpm_len(double bpm) { return (int32_t)(2646000.0 / bpm * 16384.0); }
static void clock_set_tempo(double bpm)
{
    int32_t nl = bpm_len(bpm);
    if (M.len) M.phase = (int32_t)((int64_t)M.phase * nl / M.len);          /* metronome::set_tempo keeps the fraction */
    M.len = nl;
}
/* beat times (output samples) seen by the model, for the checks */
static int64_t beats[20000]; static int nbeats;
static void clock_block_pre(void)
{
    op1_beat_t *b = host_beat();
    b->len = M.len; b->next = M.phase; b->follows_tape = (uint8_t)M.follows; b->tape_pos = M.tape_pos;
    b->step = M.follows ? (M.tape_run ? M.step : 0) : 0x100000;
    int32_t st = b->step;
    if (st > 0 && M.phase < st && nbeats < 20000)
        beats[nbeats++] = M.t + (st == 0x100000 ? M.phase >> 14 : (int32_t)((int64_t)M.phase * 64 / st));
}
static void clock_block_post(void)
{
    int32_t st = M.follows ? (M.tape_run ? M.step : 0) : 0x100000;
    M.phase -= st;
    if (M.phase < 0) M.phase += M.len;
    if (M.follows && M.tape_run) M.tape_pos += (int32_t)(((int64_t)st * 64) >> 20);
    M.t += 64;
}

/* ---- input: speech-like (glottal pulses through two vowel formants, fricatives, pauses), deterministic ---- */
static uint32_t rng = 12345;
static int32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return (int32_t)(rng >> 16) - 32768; }
static double sp_ph, f1s[2], f2s[2];
static int16_t speech(int64_t n)
{
    static const double F1[5] = { 730, 270, 530, 300, 640 }, F2[5] = { 1090, 2290, 1840, 870, 1190 };
    int64_t seg = n / 6615, in = n % 6615;                                   /* 150 ms syllables */
    int v = (int)(seg % 5), kind = (int)((seg * 7) % 11);
    double f0 = 110 + 25 * sin(n * 0.00007), x = 0;
    if (kind == 3 || kind == 9) return 0;                                    /* pause */
    if (kind == 5 && in < 2600) x = rnd() / 32768.0 * 0.35;                  /* fricative */
    else { sp_ph += f0 / 44100; if (sp_ph >= 1) { sp_ph -= 1; x = 1.0; } }
    double r = 0.985, out = 0;
    double *ss[2] = { f1s, f2s }; double fr[2] = { F1[v], F2[v] };
    for (int k = 0; k < 2; k++) {
        double c = 2 * r * cos(2 * M_PI * fr[k] / 44100), y = x + c * ss[k][0] - r * r * ss[k][1];
        ss[k][1] = ss[k][0]; ss[k][0] = y; out += y * (k ? 0.5 : 1);
    }
    double env = in < 300 ? in / 300.0 : (in > 6000 ? (6615 - in) / 615.0 : 1);
    int32_t s = (int32_t)(out * env * 900);
    return (int16_t)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
}

/* ---- the FX at position 0 ---- */
static int16_t knobs[8];
static int32_t scratch_mem[44115 + 64];                                     /* + guard words */
static int16_t outbuf[64];
static int64_t gen_at[512]; static int ngen;                                 /* generation starts (output sample) */
static int64_t rec_at, erode_at;
static int last_mode, last_gen;

static void fx_reset(uint32_t seed)
{
    rng = seed;
    for (int i = 0; i < 44115; i++) scratch_mem[i] = (int32_t)((uint32_t)rnd() << 16 ^ (uint32_t)rnd());   /* dirty scratch */
    for (int i = 44115; i < 44115 + 64; i++) scratch_mem[i] = 0x5A5A5A5A;
    host_fx_scratch[0] = scratch_mem;
    memset((void *)op1_fx_page, 0, sizeof op1_fx_page);
    knobs[0] = 28900; knobs[1] = 28000; knobs[2] = 13000; knobs[3] = 32767; knobs[4] = 0x5307;
    knobs[5] = 8100; knobs[6] = 6000; knobs[7] = 0;
    memset(&M, 0, sizeof M); nbeats = 0; ngen = 0; rec_at = erode_at = -1;
    clock_set_tempo(120); M.phase = M.len / 3;                               /* first beat a third of a beat in */
    sit_init(0);
    last_mode = st[0].mode; last_gen = 0;
}
static int32_t elapsed(const sit_t *s)                                       /* output samples since the (re)start */
{
    int32_t D = 1 << s->dsh;
    return s->mode == S_REC ? s->i * D + s->ph : s->i * D - (s->ph ? D - s->ph : 0);
}
static int32_t blkpeak;
static void block(int16_t (*src)(int64_t))
{
    for (int n = 0; n < 64; n++) outbuf[n] = src ? src(M.t + n) : 0;
    clock_block_pre();
    sit_render(0, outbuf, knobs);
    blkpeak = 0;
    for (int n = 0; n < 64; n++) { int a = abs(outbuf[n]); if (a > blkpeak) blkpeak = a; }
    sit_t *s = &st[0];
    int64_t end = M.t + 64;
    if (s->mode == S_REC && elapsed(s) <= 64) rec_at = end - elapsed(s);  /* a capture started in this block */
    if (s->mode == S_ERODE && last_mode == S_REC) { erode_at = end - elapsed(s); ngen = 0; gen_at[ngen++] = erode_at; }
    else if (s->mode == S_ERODE && s->gen != last_gen && ngen < 512) gen_at[ngen++] = end - elapsed(s);
    last_mode = s->mode; last_gen = s->gen;
    clock_block_post();
}
static void press(void) { op1_fx_page[0][3] ^= 1; }
static int16_t src_speech(int64_t n) { return speech(n); }

/* first quarter-grid sample (model) at or after `from` with quarter index (from the beat) % a == 0; a <= 4 */
static int64_t grid_after(int64_t from, int nth_beat_only)
{
    for (int k = 0; k < nbeats; k++) if (beats[k] >= from) { (void)nth_beat_only; return beats[k]; }
    return -1;
}

/* ---------- tests ---------- */
static void test_beat_lengths(void)
{
    static const double bpms[] = { 120, 97, 133.3, 71, 160 };
    for (int sel = 2; sel < 6; sel++)
        for (unsigned b = 0; b < sizeof bpms / sizeof bpms[0]; b++) {
            fx_reset(77 + sel * 13 + b);
            clock_set_tempo(bpms[b]);
            int32_t lq = sync_lq[sel];
            knobs[5] = (int16_t)(FREE_V + sel * (32768 - FREE_V) / NSYNC + 500);
            CHECK(sync_sel(knobs[5]) == sel, "sel %d", sel);
            int64_t arm_t = 0;
            for (int k = 0; k < 3000; k++) {                                 /* ~4.4 s of speech, then press */
                if (k == 37) { press(); arm_t = M.t; }
                block(src_speech);
            }
            double spb = M.len / 16384.0, kl = spb * lq / 4;
            int lqr = lq, dsh; sync_fit(M.len, &lqr, &dsh);
            kl = spb * lqr / 4;
            /* capture: on the first beat after the press (bars: no tape -> the next beat is beat 1) */
            int64_t g0 = grid_after(arm_t, 1);
            CHECK(rec_at == g0, "bpm %.1f sel %d: rec at %lld, grid %lld", bpms[b], sel, (long long)rec_at, (long long)g0);
            for (int k = 0; k < 160000 && ngen < 26; k++) block(src_speech);
            CHECK(ngen >= 21, "bpm %.1f sel %d: only %d generations", bpms[b], sel, ngen);
            int bad = 0; double maxerr = 0;
            for (int g = 0; g < ngen; g++) {
                int64_t want = rec_at + (int64_t)0;                         /* expected: grid point (g + 1) * lq / 4 beats on */
                int bi = -1;
                for (int k = 0; k < nbeats; k++) if (beats[k] == rec_at) { bi = k; break; }
                int nb = lqr / 4 * (g + 1);
                if (bi < 0 || bi + nb >= nbeats) { bad++; continue; }
                want = beats[bi + nb];
                if (gen_at[g] != want) bad++;
                double ideal = rec_at + (g + 1) * kl, err = fabs(gen_at[g] - ideal);
                if (err > maxerr) maxerr = err;
            }
            CHECK(!bad, "bpm %.1f sel %d: %d generation starts off the beat grid", bpms[b], sel, bad);
            CHECK(maxerr < 1.0, "bpm %.1f sel %d: drift %.2f samples", bpms[b], sel, maxerr);
            int64_t mn = 1 << 30, mx = 0;
            for (int g = 1; g < ngen; g++) { int64_t L = gen_at[g] - gen_at[g - 1]; if (L < mn) mn = L; if (L > mx) mx = L; }
            CHECK(mn >= (int64_t)floor(kl) && mx <= (int64_t)ceil(kl), "bpm %.1f sel %d: lengths %lld..%lld, musical %.3f",
                  bpms[b], sel, (long long)mn, (long long)mx, kl);
            printf("  %6.1f BPM %-6s: rate %s, musical %.3f samples, generation lengths %lld..%lld, %d gens, "
                   "max |start - ideal| %.2f, grid exact %s\n", bpms[b], sync_name[sel], dsh == 2 ? "11k" : "22k", kl,
                   (long long)mn, (long long)mx, ngen, maxerr, bad ? "NO" : "yes");
            CHECK(memcmp(scratch_mem + 44115, (int32_t[]){ 0x5A5A5A5A }, 4) == 0, "guard");
        }
}

static void test_sub_beat(void)
{
    for (int sel = 0; sel < 2; sel++) {
        fx_reset(5 + sel);
        clock_set_tempo(124);
        knobs[5] = (int16_t)(FREE_V + sel * (32768 - FREE_V) / NSYNC + 500);
        for (int k = 0; k < 15000; k++) { if (k == 101) press(); block(src_speech); }
        double q = M.len / 16384.0 / 4, kl = q * sync_lq[sel];
        /* quarter grid of the model: beats - j q */
        int bad = 0; double maxerr = 0;
        for (int g = 0; g < ngen; g++) {
            double ideal = rec_at + (g + 1) * kl, err = fabs(gen_at[g] - ideal);
            if (err > maxerr) maxerr = err;
            if (err > 1.0) bad++;
        }
        int64_t start_ok = 0;                                                /* the start is on a quarter grid point */
        for (int k = 0; k < nbeats; k++) for (int j = 0; j < 4; j++) {
            double tq = beats[k] - j * q;
            if (fabs(tq - rec_at) <= 1.0 && (j % sync_lq[sel]) == 0) start_ok = 1;
        }
        CHECK(start_ok, "sub-beat %s: start %lld not on its grid", sync_name[sel], (long long)rec_at);
        CHECK(ngen > 40 && !bad, "sub-beat %s: %d gens, %d off by > 1 sample", sync_name[sel], ngen, bad);
        printf("  124.0 BPM %-6s: musical %.3f samples, %d gens, max |start - ideal| %.2f samples\n",
               sync_name[sel], kl, ngen, maxerr);
    }
}

static void test_tempo_change(void)
{
    static const double to[2] = { 132, 108 };
    for (int c = 0; c < 2; c++) {
        fx_reset(900 + c);
        clock_set_tempo(120);
        knobs[5] = (int16_t)(FREE_V + 4 * (32768 - FREE_V) / NSYNC + 500);   /* 1 BAR */
        for (int k = 0; k < 8000; k++) { if (k == 20) press(); block(src_speech); }
        int g_before = ngen;
        int64_t tchange = M.t + 1000;
        for (int k = 0; k < 15000; k++) {
            if (M.t <= tchange && tchange < M.t + 64) clock_set_tempo(to[c]);
            block(src_speech);
            CHECK(blkpeak < 32767, "tempo change: clipped output");
        }
        /* after the change: every generation starts on a bar of the new tempo (4 counted beats) */
        double kl = M.len / 16384.0 * 4;
        int64_t mn = 1 << 30, mx = 0;
        for (int g = g_before + 2; g < ngen; g++) {
            int64_t L = gen_at[g] - gen_at[g - 1];
            if (L < mn) mn = L;
            if (L > mx) mx = L;
        }
        int64_t Lchg = gen_at[g_before] - gen_at[g_before - 1];
        CHECK(mn >= (int64_t)floor(kl) && mx <= (int64_t)ceil(kl), "tempo %g: lengths %lld..%lld want %.2f", to[c],
              (long long)mn, (long long)mx, kl);
        /* the loop keeps its recorded length: faster -> cut (end not played), slower -> a gap of silence */
        sit_t *s = &st[0];
        printf("  120 -> %.0f BPM mid-loop (1 BAR): loop keeps %d samples (%s), generation across the change %lld "
               "samples, then %lld..%lld (bar = %.2f), %d gens, still locked\n", to[c], s->len << s->dsh,
               (s->len << s->dsh) > kl ? "cut short each pass" : "silence before each bar line",
               (long long)Lchg, (long long)mn, (long long)mx, kl, ngen);
    }
}

static void test_long_and_slow(void)
{
    /* 2 bars at 120 BPM: 4 s does not fit 3.9 s at 22.05 kHz -> 11.025 kHz; 2 bars at 50 BPM (9.6 s): 1 bar, 11k */
    static const double bp[2] = { 120, 50 };
    for (int c = 0; c < 2; c++) {
        fx_reset(31 + c);
        clock_set_tempo(bp[c]);
        knobs[5] = 32767;                                                     /* 2 BARS */
        for (int k = 0; k < 90000 && ngen < 22; k++) { if (k == 10) press(); block(src_speech); }
        sit_t *s = &st[0];
        double kl = M.len / 16384.0 * s->lq / 4;
        int64_t mn = 1 << 30, mx = 0;
        for (int g = 1; g < ngen; g++) { int64_t L = gen_at[g] - gen_at[g - 1]; if (L < mn) mn = L; if (L > mx) mx = L; }
        CHECK(s->dsh == 2, "long loop not at 11k");
        CHECK(c == 0 ? s->lq == 32 : s->lq == 16, "lq %d", s->lq);
        CHECK(ngen >= 21 && mn >= (int64_t)floor(kl) && mx <= (int64_t)ceil(kl), "long: %d gens %lld..%lld want %.2f", ngen,
              (long long)mn, (long long)mx, kl);
        printf("  %3.0f BPM 2 BARS: runs as %s at %s, loop %d samples at 11.025 kHz, generation %lld..%lld (musical %.2f)\n",
               bp[c], s->lq == 32 ? "2 BARS" : "1 BAR", s->dsh == 2 ? "11.025 kHz" : "22.05 kHz", s->len, (long long)mn,
               (long long)mx, kl);
    }
}

static void test_tape_bars(void)
{
    /* the clock follows the tape: stopped -> our own copy runs (armed beats still come); running -> bars from the
     * tape position (beat index % 4 == 0) */
    fx_reset(4242);
    M.follows = 1; M.tape_run = 0; M.step = 0x100000;
    knobs[5] = (int16_t)(FREE_V + 2 * (32768 - FREE_V) / NSYNC + 500);       /* 1 BEAT */
    for (int k = 0; k < 6000; k++) { if (k == 15) press(); block(src_speech); }
    int own_gens = st[0].gen;
    CHECK(st[0].mode == S_ERODE && own_gens > 10, "tape stopped: own clock did not run the loop (mode %d gen %d)",
          st[0].mode, own_gens);
    /* tape starts at 1.3 beats past a bar line; the metronome restarts on a beat at the play position */
    int32_t spb = M.len >> 14;
    M.tape_pos = spb * 9 + spb * 3 / 10; M.tape_run = 1; M.phase = 0;
    knobs[5] = (int16_t)(FREE_V + 4 * (32768 - FREE_V) / NSYNC + 500);       /* 1 BAR */
    int64_t press_t = M.t;
    press();
    int32_t tp_at_rec = -1;
    for (int k = 0; k < 4000; k++) {
        int was = st[0].mode == S_REC && st[0].lq == 16;
        int32_t tp = M.tape_pos;
        block(src_speech);
        if (!was && st[0].mode == S_REC && st[0].lq == 16 && tp_at_rec < 0) tp_at_rec = tp + (int32_t)(rec_at - (M.t - 64));
    }
    int32_t idx = (tp_at_rec + spb / 2) / spb;
    CHECK(tp_at_rec >= 0 && (idx & 3) == 0, "tape bars: capture at tape beat %d (pos %d)", idx, tp_at_rec);
    printf("  tape stopped (clock follows tape): own clock ran %d generations; tape playing: 1 BAR capture started at "
           "tape beat %d (%s a bar line), %lld samples after the press\n", own_gens, idx, (idx & 3) ? "NOT on" : "on",
           (long long)(rec_at - press_t));
    /* half tape speed: the clock and the loop run at half speed (twice the samples), still on the grid */
    fx_reset(4343);
    M.follows = 1; M.tape_run = 1; M.step = 0x80000;
    knobs[5] = (int16_t)(FREE_V + 2 * (32768 - FREE_V) / NSYNC + 500);       /* 1 BEAT */
    for (int k = 0; k < 20000 && ngen < 22; k++) { if (k == 15) press(); block(src_speech); }
    int64_t mn = 1 << 30, mx = 0;
    for (int g = 1; g < ngen; g++) { int64_t L = gen_at[g] - gen_at[g - 1]; if (L < mn) mn = L; if (L > mx) mx = L; }
    CHECK(ngen >= 21 && mn >= 44099 && mx <= 44101, "half speed: %lld..%lld", (long long)mn, (long long)mx);
    printf("  tape at half speed: 1 BEAT loop = %lld..%lld samples (2 x 22050), %d gens\n", (long long)mn, (long long)mx, ngen);
}

static void test_arm_cancel_and_garbage(void)
{
    fx_reset(99);
    int32_t mx = 0;
    for (int k = 0; k < 300; k++) { block(src_speech); if (blkpeak > mx) mx = blkpeak; }
    CHECK(mx <= 1, "garbage before the first capture: %d", mx);
    knobs[5] = 32000;
    press(); block(src_speech); press();                                      /* arm, cancel */
    for (int k = 0; k < 2000; k++) block(src_speech);
    CHECK(st[0].mode == S_IDLE && !st[0].armed, "cancel: mode %d armed %d", st[0].mode, st[0].armed);
    printf("  idle output max |%d| (dirty scratch), arm + cancel: stays idle\n", mx);
}

static void test_random(void)
{
    for (int r = 0; r < 40; r++) {
        fx_reset(1000 + r);
        clock_set_tempo(40 + (rnd() & 0xffff) % 200);
        for (int k = 0; k < 4000; k++) {
            if ((rnd() & 255) == 0) press();
            if ((rnd() & 127) == 0) for (int i = 0; i < 8; i++) if (i != 4) knobs[i] = (int16_t)(rnd() & 0x7fff);
            if ((rnd() & 1023) == 0) clock_set_tempo(30 + (rnd() & 0xffff) % 260);
            if ((rnd() & 1023) == 0) M.phase = (int32_t)((uint32_t)rnd() % (uint32_t)M.len);   /* clock jump */
            if ((rnd() & 2047) == 0) { M.follows ^= 1; M.tape_run = rnd() & 1; M.step = 0x40000 + (rnd() & 0x1ffff) * 2; }
            op1_fx_page[0][0] = (rnd() & 63) == 0; op1_fx_page[0][1] = (rnd() & 63) == 0;
            block((rnd() & 3) ? src_speech : 0);
        }
        CHECK(memcmp(scratch_mem + 44115, (int32_t[]){ 0x5A5A5A5A }, 4) == 0, "guard after random run %d", r);
    }
    printf("  40 random runs (knobs, presses, tempo 30..290 BPM, clock jumps, tape follow / speed): no trap\n");
}

/* clicks at the loop boundary: the step |y[n] - y[n-1]| at each generation start vs the largest step inside the
 * generation (wet only, silence in once the loop runs) */
static int16_t rec_out[1 << 21]; static int64_t rec_n;
static int16_t src_until_loop(int64_t n) { return st[0].mode == S_ERODE ? 0 : speech(n); }
static int16_t src_tone(int64_t n);
static void wrap_case(const char *name, int16_t w5, double *worst, int16_t w7)
{
    fx_reset(4242);
    knobs[5] = w5; knobs[3] = 32767; knobs[7] = w7;
    rec_n = 0;
    int64_t t0 = M.t;
    for (int k = 0; k < 30000 && ngen < 24; k++) {
        if (k == 5) press();
        block(w7 ? src_tone : src_until_loop);                             /* crossfade: a smooth tone (raw speech
                                                                            * is spikier than the room's output) */
        for (int n = 0; n < 64 && rec_n < (1 << 21); n++) rec_out[rec_n++] = outbuf[n];
    }
    double w = 0; int at = 0;
    for (int g = 1; g + 1 < ngen; g++) {
        int64_t a = gen_at[g] - t0, e = gen_at[g + 1] - t0;
        if (e >= rec_n || a < 8) break;
        int edge = 0, in = 0;                                                /* second difference: a click */
        for (int64_t n = a - 3; n <= a + 3; n++) { int d = abs(rec_out[n] - 2 * rec_out[n - 1] + rec_out[n - 2]); if (d > edge) edge = d; }
        for (int64_t n = a + 16; n < e - 16; n++) { int d = abs(rec_out[n] - 2 * rec_out[n - 1] + rec_out[n - 2]); if (d > in) in = d; }
        if (getenv("WRAPDBG")) { printf("    g%d edge %d inner %d:", g, edge, in); for (int64_t n = a - 6; n <= a + 6; n++) printf(" %d", rec_out[n]); printf("\n"); }
        double r = in ? (double)edge / in : 0;
        if (r > w) { w = r; at = g; }
    }
    printf("  %s: worst boundary step / largest inner step %.2f (generation %d of %d)\n", name, w, at, ngen);
    *worst = w;
}
/* crossfade (white page 2): a steady tone loops without the level dip of the end fades */
static int16_t src_tone(int64_t n) { return st[0].mode == S_ERODE ? 0 : (int16_t)(12000 * sin(n * 2 * M_PI * 220 / 44100)); }
static double dip_case(int16_t w7)
{
    fx_reset(99);
    knobs[5] = 1000; knobs[3] = 32767; knobs[7] = w7; knobs[2] = 0;           /* free 0.6 s, least erosion */
    rec_n = 0;
    int64_t t0 = M.t;
    for (int k = 0; k < 30000 && ngen < 6; k++) {
        if (k == 5) press();
        block(src_tone);
        for (int n = 0; n < 64 && rec_n < (1 << 21); n++) rec_out[rec_n++] = outbuf[n];
    }
    double worst = 1e9;
    for (int g = 1; g + 1 < ngen && g < 4; g++) {
        int64_t a = gen_at[g] - t0;                                            /* around the boundary vs inside */
        double in = 0, edge = 1e9;
        for (int64_t n = a + 2000; n < a + 6000; n++) in += (double)rec_out[n] * rec_out[n];
        in = sqrt(in / 4000);
        for (int64_t c = a - 600; c < a + 600; c += 50) {
            double e = 0; for (int64_t n = c; n < c + 100; n++) e += (double)rec_out[n] * rec_out[n];
            e = sqrt(e / 100); if (e < edge) edge = e;
        }
        if (in > 0 && edge / in < worst) worst = edge / in;
    }
    return worst;
}

static void test_wrap_click(void)
{
    double off = dip_case(0), on = dip_case(32767);
    printf("  steady tone, free loop: level at the boundary (lowest 100-sample RMS / inside) fades %.2f, crossfade %.2f\n", off, on);
    CHECK(on > 0.6 && on > off + 0.2, "crossfade: boundary dip %.2f (fades %.2f)", on, off);
    double a, b;
    for (int x = 0; x < 2; x++) {
        int16_t w7 = x ? 20000 : 0;
        wrap_case(x ? "free 0.6 s, crossfade" : "free 0.6 s", 1000, &a, w7);
        wrap_case(x ? "synced 1 BEAT, crossfade" : "synced 1 BEAT", (int16_t)(FREE_V + 2 * (32768 - FREE_V) / NSYNC + 500), &b, w7);
        CHECK(a < 1.5 && b < 1.5, "click at the loop boundary: free %.2f, synced %.2f", a, b);
    }
}

static void test_level(void)
{
    /* level over the generations of a synced loop: no runaway, no stuck saturation, no collapse */
    fx_reset(555);
    knobs[5] = (int16_t)(FREE_V + 4 * (32768 - FREE_V) / NSYNC + 500);
    int32_t pk[64] = { 0 };
    for (int k = 0; k < 60000 && ngen < 30; k++) {
        if (k == 5) press();
        block(src_speech);
        if (st[0].mode == S_ERODE && st[0].gen < 64 && blkpeak > pk[st[0].gen]) pk[st[0].gen] = blkpeak;
    }
    printf("  1 BAR loop, generation peaks (dBFS):");
    for (int g = 0; g < 30; g += 3) printf(" g%d %.1f", g, 20 * log10((pk[g] + 1) / 32768.0));
    printf("\n");
    for (int g = 2; g < 30; g++) CHECK(pk[g] > 2000 && pk[g] < 32767, "gen %d peak %d", g, pk[g]);
}

/* ---------- slow tape, long free loops, gain ramp across blocks ---------- */
static int32_t dry_run, dry_best;                                            /* blocks with the recording used up */
static void block_dry(int16_t (*src)(int64_t))
{
    block(src);
    dry_run = st[0].mode == S_ERODE && st[0].i >= st[0].len ? dry_run + 1 : 0;
    if (dry_run > dry_best) dry_best = dry_run;
}
static void test_slow_tape(void)
{
    /* the reported case: a 1 BAR loop captured at the nominal tempo (tape stopped: our clock copy), then the tape runs
     * at 25 %. Before: 1 beat of loop, 3 beats of silence (the recording used up until the bar line). Now: the loop
     * restarts every beat, and when the tape goes back to 100 % it realigns to the tape's bar lines */
    static const int32_t steps[3] = { 0x40000, 0x3FFFF, 0x40001 };
    for (int c = 0; c < 3; c++) {
        fx_reset(7100 + c);
        M.follows = 1; M.tape_run = 0; M.step = 0x100000;
        knobs[5] = (int16_t)(FREE_V + 4 * (32768 - FREE_V) / NSYNC + 500);   /* 1 BAR */
        for (int k = 0; k < 12000 && st[0].gen < 3; k++) { if (k == 15) press(); block(src_speech); }
        CHECK(st[0].mode == S_ERODE && st[0].lq == 16 && st[0].dsh == 1, "slow tape: capture %d lq %d", st[0].mode, st[0].lq);
        int32_t spb = M.len >> 14;
        M.tape_pos = spb * 8; M.tape_run = 1; M.step = steps[c]; M.phase = 0;  /* play from a bar line at 25 % */
        int g0 = ngen, peak = 0;
        dry_best = dry_run = 0;
        for (int k = 0; k < 24000; k++) {
            block_dry(src_speech);
            if (blkpeak > peak) peak = blkpeak;
        }
        int64_t mn = 1 << 30, mx = 0;
        for (int g = g0 + 3; g < ngen; g++) { int64_t L = gen_at[g] - gen_at[g - 1]; if (L < mn) mn = L; if (L > mx) mx = L; }
        CHECK(dry_best <= 1, "slow tape step %x: recording used up for %d blocks", steps[c], dry_best);
        CHECK(mn >= 88190 && mx <= 88210, "slow tape step %x: generations %lld..%lld (1 beat at 25 %% = 88200)", steps[c],
              (long long)mn, (long long)mx);
        CHECK(peak < 32767, "slow tape: clipped");
        printf("  1 BAR captured at 120 BPM, tape at step 0x%X: restarts every %lld..%lld samples (1 beat), recording used "
               "up for at most %d block(s) in a row, peak %.1f dBFS\n", steps[c], (long long)mn, (long long)mx, dry_best,
               20 * log10(peak / 32768.0));
        if (c) continue;
        /* back to 100 %: one shorter pass to the next bar line, then whole bars on the tape's bar lines */
        M.step = 0x100000;
        int g1 = ngen, ok = 1, full = 0;
        for (int k = 0; k < 12000; k++) {
            int gb = st[0].gen;
            block(src_speech);
            if (st[0].gen != gb && ngen > g1 + 2) {
                int64_t L = gen_at[ngen - 1] - gen_at[ngen - 2];
                if (L >= 88199 && L <= 88201) full++;
                if (st[0].qbar != 0) ok = 0;                                 /* every start on a bar line (tape) */
            }
        }
        CHECK(ok && full >= 5, "back to 100 %%: on bar lines %d, full bars %d", ok, full);
        printf("  tape back to 100 %%: %d whole-bar generations, all starting on the tape's bar lines\n", full);
    }
    /* captured at 25 % (1 BAR = 8 s does not fit, recorded as 1/2): no gaps either */
    fx_reset(7200);
    M.follows = 1; M.tape_run = 1; M.step = 0x40000;
    knobs[5] = (int16_t)(FREE_V + 4 * (32768 - FREE_V) / NSYNC + 500);
    for (int k = 0; k < 40000 && ngen < 6; k++) { if (k == 15) press(); block(src_speech); }
    dry_best = dry_run = 0;
    for (int k = 0; k < 12000; k++) block_dry(src_speech);
    CHECK(st[0].lq == 8 && st[0].dsh == 2 && dry_best <= 1, "captured at 25 %%: lq %d dsh %d used up %d", st[0].lq, st[0].dsh,
          dry_best);
    printf("  1 BAR captured at 25 %%: runs as 1/2 at 11.025 kHz, recording used up for at most %d block(s)\n", dry_best);
}

static void test_free_long(void)
{
    static const int16_t w[3] = { 8100, 12000, 13000 };
    for (int c = 0; c < 3; c++) {
        fx_reset(7300 + c);
        knobs[5] = w[c];
        int clip = 0;
        for (int k = 0; k < 80000 && ngen < 4; k++) {
            if (k == 15) press();
            block(src_speech);
            if (blkpeak >= 32767 && st[0].mode == S_ERODE && st[0].gen > 0 && !clip) {   /* (the speech input reaches
                                                                              * full scale: REC and generation 0 play it) */ clip = 1; printf("    clip at gen %d mode %d i %d\n", st[0].gen, st[0].mode, st[0].i); }
        }
        sit_t *s = &st[0];
        int32_t want = free_len(w[c]);
        int64_t L = gen_at[3] - gen_at[2];
        CHECK(!clip, "free %d: clipped", w[c]);
        CHECK(ngen >= 4 && s->dsh == (want > NMAX ? 2 : 1) && L == (int64_t)s->len << s->dsh && s->len == want >> (s->dsh - 1),
              "free %d: dsh %d len %d gen %lld", w[c], s->dsh, s->len, (long long)L);
        printf("  free w5 %5d: %.2f s at %s, generation %lld samples\n", w[c], (double)L / 44100,
               s->dsh == 2 ? "11.025 kHz" : "22.05 kHz", (long long)L);
    }
}

static void test_ramp_blocks(void)
{
    /* a free wrap's gain ramp (256 loop samples) spans several blocks: the old gain must survive the block boundary */
    fx_reset(7400);
    knobs[5] = 1000;
    int bad = 0, seen = 0;
    int32_t gm_prev = 0, rs_prev = 0;
    for (int k = 0; k < 20000; k++) {
        int gb = st[0].gen;
        int32_t gmb = st[0].gm, rsb = st[0].rs;
        if (k == 15) press();
        block(src_speech);
        if (st[0].mode == S_ERODE && st[0].gen != gb) { gm_prev = gmb; rs_prev = rsb; seen++; continue; }
        if (st[0].mode == S_ERODE && st[0].gk > 0 && seen && (st[0].gmo != gm_prev || st[0].rso != rs_prev)) bad++;
    }
    CHECK(seen > 5 && !bad, "gain ramp: %d blocks with a lost old gain (%d wraps)", bad, seen);
    printf("  free loop: the previous gain is kept through the ramp in all blocks (%d wraps)\n", seen);
}

/* ---------- screen states (JSON for plugins/sitting/preview.py) ---------- */
static void screen_state(int n)
{
    fx_reset(7);
    int16_t *fk = host_fx_knobs[0];
    int bar1 = (int16_t)(FREE_V + 4 * (32768 - FREE_V) / NSYNC + 500);
    switch (n) {
    case 0: knobs[5] = (int16_t)bar1; op1_fx_page[0][0] = 1; break;          /* idle, page 2 blue: SYNC 1 BAR */
    case 1:                                                                   /* free loop GEN 05, armed for 1 BAR */
        for (int k = 0; k < 40000 && st[0].gen < 5; k++) { if (k == 10) press(); block(src_speech); }
        knobs[5] = (int16_t)bar1; press();
        for (int k = 0; k < 20; k++) block(src_speech);
        break;
    case 2:                                                                   /* synced 1 BAR, recording */
        knobs[5] = (int16_t)bar1; press();
        for (int k = 0; k < 900; k++) block(src_speech);
        break;
    case 3:                                                                   /* synced 1 BAR, GEN 08 */
        knobs[5] = (int16_t)bar1; press();
        for (int k = 0; k < 40000 && st[0].gen < 8; k++) block(src_speech);
        for (int k = 0; k < 600; k++) block(src_speech);
        break;
    case 4:                                                                   /* 2 BARS 11K, golden room, GEN 24 */
        knobs[0] = 14000; knobs[1] = 10000; knobs[2] = 20000; knobs[5] = 32767; press();
        for (int k = 0; k < 90000 && st[0].gen < 24; k++) block(src_speech);
        for (int k = 0; k < 2000; k++) block(src_speech);
        break;
    case 5:                                                                   /* free 2.6 s, GEN 03, page 2 blue */
        press();
        for (int k = 0; k < 40000 && st[0].gen < 3; k++) block(src_speech);
        for (int k = 0; k < 1300; k++) block(src_speech);
        op1_fx_page[0][0] = 1;
        break;
    case 6:                                                                   /* 1/4 BEAT, small cube, page 2 green */
        knobs[0] = 2000; knobs[1] = 0; knobs[2] = 22000; knobs[5] = (int16_t)(FREE_V + 500); press();
        for (int k = 0; k < 4000; k++) block(src_speech);
        op1_fx_page[0][1] = 1;
        break;
    case 7:                                                                   /* 2 BEATS, bolt, max erosion, GEN 40 */
        knobs[0] = 22000; knobs[1] = 20000; knobs[2] = 32767; knobs[5] = (int16_t)(FREE_V + 3 * (32768 - FREE_V) / NSYNC + 500);
        press();
        for (int k = 0; k < 90000 && st[0].gen < 40; k++) block(src_speech);
        for (int k = 0; k < 300; k++) block(src_speech);
        break;
    case 8:                                                                   /* idle at 50 BPM: SYNC 1 BAR LO-FI */
    case 9:                                                                   /* idle, tape at 25 %: SYNC 1 BAR AS 1/2 */
        clock_set_tempo(n == 8 ? 50 : 120);
        if (n == 9) { M.follows = 1; M.tape_run = 1; M.step = 0x40000; }
        knobs[5] = (int16_t)bar1; op1_fx_page[0][0] = 1;
        for (int k = 0; k < 20; k++) block(0);
        break;
    case 10:                                                                  /* free 7.5 s at 11.025 kHz, GEN 01 */
        knobs[5] = 13000; press();
        for (int k = 0; k < 40000 && st[0].gen < 1; k++) block(src_speech);
        for (int k = 0; k < 1300; k++) block(src_speech);
        op1_fx_page[0][0] = 1;
        break;
    }
    for (int i = 0; i < 8; i++) fk[i] = knobs[i];
    sit_screen_update(0, 300);                                                /* blink phase: on */
    nlines = 0;
    sit_screen_update(0, 0);
    printf("{\"nodes\": {}, \"hidden\": [], \"lines\": [");
    for (int i = 0; i < nlines; i++)
        printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", lines[i].x0, lines[i].y0, lines[i].x1,
               lines[i].y1, lines[i].w, lines[i].c[0], lines[i].c[1], lines[i].c[2], lines[i].c[3]);
    printf("], \"knobs\": [%d, %d, %d, %d]}\n", knobs[0], knobs[1], knobs[2], knobs[3]);
}

/* raw int16 output of a synced run (speech, mix 100 %) for listening / analysis; stderr: modes and generation starts */
static void wav_run(int sel, double bpm, int gens, int k0, int k1, int k2)
{
    fx_reset(3);
    clock_set_tempo(bpm);
    knobs[0] = (int16_t)k0; knobs[1] = (int16_t)k1; knobs[2] = (int16_t)k2;
    knobs[5] = (int16_t)(sel < 0 ? 8100 : FREE_V + sel * (32768 - FREE_V) / NSYNC + 500);
    for (int k = 0; k < 400000 && ngen <= gens; k++) {
        if (k == 30) press();
        block(src_speech);
        fwrite(outbuf, 2, 64, stdout);
    }
    fprintf(stderr, "modes");
    for (int k = 0; k < NMODE; k++) fprintf(stderr, " %.2f", st[0].room.f[k] / 256.0);
    fprintf(stderr, "\nrec %lld dsh %d gens", (long long)rec_at, st[0].dsh);
    for (int g = 0; g < ngen; g++) fprintf(stderr, " %lld", (long long)gen_at[g]);
    fprintf(stderr, "\n");
}

int main(int argc, char **argv)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    if (argc > 2 && !strcmp(argv[1], "screen")) { screen_state(atoi(argv[2])); return 0; }
    if (argc > 4 && !strcmp(argv[1], "wav")) {         /* wav <sel -1..5> <bpm> <gens> [k0 k1 k2] > out.raw */
        wav_run(atoi(argv[2]), atof(argv[3]), atoi(argv[4]), argc > 5 ? atoi(argv[5]) : 28900,
                argc > 6 ? atoi(argv[6]) : 28000, argc > 7 ? atoi(argv[7]) : 13000);
        return 0;
    }
    printf("beat lengths (start on the grid, every generation on the grid, length = musical length):\n");
    test_beat_lengths();
    printf("sub-beat lengths:\n");
    test_sub_beat();
    printf("tempo change while a loop plays:\n");
    test_tempo_change();
    printf("long loops / slow tempo:\n");
    test_long_and_slow();
    printf("tape-following clock:\n");
    test_tape_bars();
    printf("idle / cancel:\n");
    test_arm_cancel_and_garbage();
    printf("loop boundary:\n");
    test_wrap_click();
    printf("level:\n");
    test_level();
    printf("slow tape / long free loops:\n");
    test_slow_tape();
    test_free_long();
    test_ramp_blocks();
    printf("robustness:\n");
    test_random();
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
