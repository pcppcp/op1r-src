/* Host test of the Delay Trio DSP as op1r drives it (plugins/delayed, pitchdelay, bubbler: ESC_CODE's code): an impulse
 * comes back after TIME; pitch 0 echoes are plain; bubbler grains make sound; nothing blows up at full feedback.
 *   cc -I../plugins/delayed ... (run_all: EXTRA) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../plugins/delayed/delay_dsp.h"
#include "../plugins/pitchdelay/pitch_dsp.h"
#include "../plugins/bubbler/bubbler_dsp.h"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static short ring[BB_STORAGE > DL_SAMPLES ? BB_STORAGE : DL_SAMPLES];
static int peak_after(short *out, int n, int from) { int best = from; for (int i = from; i < n; i++) if (abs(out[i]) > abs(out[best])) best = i; return best; }
int main(void)
{
    enum { N = 64 * 400 };
    static short out[N];
    /* delayed: 100 ms (knob 21760), feedback 0, open filter, full wet: the impulse at ~4410 samples */
    { static struct dl_state s; dl_init(&s); memset(ring, 0, sizeof ring);
      short par[8] = { 21760, 0, 32767, 32767, 14208, 0, 0, 16384 };
      for (int b = 0; b < N / 64; b++) { short *x = out + b * 64; memset(x, 0, 128); if (!b) x[0] = 16000; dl_render(&s, ring, x, 0, 64, par, 0, 0); }
      int t = dl_time(21760), p = peak_after(out, N, 1);
      CHECK(abs(p - t) <= 2 && abs(out[p]) > 4000, "delayed: echo at %d (want %d), %d", p, t, out[p]); }
    /* delayed at 200 % feedback, filter low, LFO on: stays bounded (softclip) */
    { static struct dl_state s; dl_init(&s); memset(ring, 0, sizeof ring);
      short par[8] = { 6000, 32767, 8000, 16384, 20000, 20000, 0, 16384 }; int mx = 0;
      for (int b = 0; b < N / 64; b++) { short *x = out + b * 64; for (int i = 0; i < 64; i++) x[i] = (short)((rand() & 8191) - 4096); dl_render(&s, ring, x, 0, 64, par, 40, 0); for (int i = 0; i < 64; i++) if (abs(x[i]) > mx) mx = abs(x[i]); }
      CHECK(mx <= 32767 && mx > 1000, "delayed: bounded at full feedback (%d)", mx); }
    /* pitch delay, pitch 0: a plain delay at TIME */
    { static struct pd_state s; pd_init(&s); memset(ring, 0, sizeof ring);
      short par[8] = { 21760, 0, 0, 32767, 0, 0, 0, 0 };
      for (int b = 0; b < N / 64; b++) { short *x = out + b * 64; memset(x, 0, 128); if (!b) x[0] = 16000; pd_render(&s, ring, x, 0, par, 0, 25); }
      int t = pd_time(21760), p = peak_after(out, N, 1);
      CHECK(abs(p - t) <= 4 && abs(out[p]) > 4000, "pitch delay: echo at %d (want ~%d), %d", p, t, out[p]); }
    /* pitch delay +12: a sine comes back at twice its frequency (zero crossings) */
    { static struct pd_state s; pd_init(&s); memset(ring, 0, sizeof ring);
      short par[8] = { 21760, 0, 34 * PD_PITCH_STEP, 32767, 0, 0, 0, 0 }; int zc = 0, ph = 0;
      for (int b = 0; b < N / 64; b++) { short *x = out + b * 64; for (int i = 0; i < 64; i++) { x[i] = (short)(ph < 50 ? 8000 : -8000); ph = (ph + 1) % 100; } pd_render(&s, ring, x, 0, par, 0, 25); }
      for (int i = N / 2; i < N - 1; i++) zc += (out[i] < 0) != (out[i + 1] < 0);
      CHECK(zc > (N / 2) / 100 * 2 * 3 / 2, "pitch delay +12: %d crossings (input %d)", zc, (N / 2) / 50); }
    /* bubbler: grains read the line: sound after an input burst, bounded */
    { static struct bb_state s; bb_init(&s); memset(ring, 0, sizeof ring); static int mix[64];
      struct bb_params v = { .scatter = 4112, .tone = 117, .reverse = 0, .density = 80, .size_var = 4112, .pitch_var = 0, .jitter = 4112, .size = 8192, .resonance = 0 };
      short par[8] = { 20480, 12000, 0, 32767, 0, 0, 0, 0 }; bb_encode(par, &v); s.pitch_mode = 0; long e = 0; int mx = 0;
      for (int b = 0; b < N / 64; b++) { short *x = out + b * 64; for (int i = 0; i < 64; i++) x[i] = b < 40 ? (short)((rand() & 16383) - 8192) : 0; bb_render_work(&s, ring, x, 0, 64, par, mix); if (b > 60) for (int i = 0; i < 64; i++) { e += abs(x[i]); if (abs(x[i]) > mx) mx = abs(x[i]); } }
      CHECK(e > 100000 && mx <= 32767, "bubbler: grains after the burst (energy %ld, peak %d)", e, mx); }
    printf(fails ? "esctrio: %d failed\n" : "esctrio: ok\n", fails);
    return fails != 0;
}
