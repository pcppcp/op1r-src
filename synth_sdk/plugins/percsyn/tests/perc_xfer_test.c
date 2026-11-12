/* perc's transfer reader against transfers made by libpo32's modulator (host only).
 *   cc -O2 -DOP1_HOST -I.. -I../../.. -I$LIBPO32/core/include -I$LIBPO32/core/src perc_xfer_test.c ../perc_xfer.c ../perc_xbank.c \
 *      $LIBPO32/core/src/po32.c -lm -o perc_xfer_test && ./perc_xfer_test
 * A 16-instrument transfer (both morph sides) is rendered at 44.1 kHz, impaired like a speaker-to-mic recording and cut
 * to the 6 s a slot's sample holds; perc's integer decoder must recover every patch that is complete in it, the same
 * as libpo32's float demodulator on the same audio. */
#include "perc_xfer.h"
#include "po32.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_voice_env[128]; int16_t host_knobs[8][8]; int host_shift; int host_slot;
void perc_xfer_decode_into(int slot, const int16_t *s, unsigned n);
const perc_xfer_t *perc_xfer_last(void);

#define FS 44100
#define MAXN (FS * 14)
static uint16_t sent[16][2][21];
static float clean[MAXN]; static size_t clean_n;
static float work[MAXN]; static int16_t pcm[MAXN];

static int ref_cb(const po32_packet_t *p, void *u)
{
    int *cnt = u;
    if (p->tag_code == PO32_TAG_PATCH && p->payload_len == 43) {
        int side = p->payload[0] >> 4, inst = p->payload[0] & 15, ok = 1;
        for (int i = 0; i < 21; i++) ok &= (p->payload[1 + 2 * i] | (p->payload[2 + 2 * i] << 8)) == sent[inst][side - 1][i];
        if (ok) (*cnt)++;
    }
    return 0;
}
static double frand(void) { return rand() / (double)RAND_MAX; }

/* one case: transform clean -> work (n samples), cut at max_n, decode both ways */
static void run(const char *name, size_t n, size_t max_n, int expect_min)
{
    if (n > max_n) n = max_n;
    for (size_t i = 0; i < n; i++) { float v = work[i] * 32767; pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
    perc_xfer_decode_into(0, pcm, (unsigned)n);
    const perc_xfer_t *x = perc_xfer_last();
    int ok = 0, bad = 0;
    for (int in = 0; in < 16; in++) for (int s = 0; s < 2; s++) if (x->have[in] & (1 << s)) {
        if (!memcmp(x->patch[in][s], sent[in][s], sizeof sent[in][s])) ok++; else bad++;
    }
    /* the same patches in arrival order (a drum kit fills its keys from this list): instrument e, both sides paired */
    for (int e = 0; e < x->list_n; e++) {
        int in = x->list_inst[e];
        if (in != e || memcmp(x->list[e][0], sent[in][0], 42) || memcmp(x->list[e][1], sent[in][x->list_right[e] ? 1 : 0], 42)) bad++;
    }
    if (x->list_n * 2 < ok - 1) bad++;
    /* reference: libpo32's float demodulator on the same 16-bit audio */
    static float f[MAXN]; for (size_t i = 0; i < n; i++) f[i] = pcm[i] / 32768.0f;
    po32_demodulator_t d; po32_demodulator_init(&d, FS); int ref = 0;
    po32_demodulator_push(&d, f, n, ref_cb, &ref); po32_demodulator_flush(&d, ref_cb, &ref);
    printf("%-34s %5.2f s  perc %2d patches (%d wrong)%s  libpo32 %2d   %s\n", name, n / (double)FS, ok, bad,
           x->done ? ", tail" : "", ref, (ok >= expect_min && !bad && ok >= ref) ? "ok" : "FAIL");
}

int main(void)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    srand(3);
    static uint8_t frame[8192]; po32_builder_t b; po32_builder_init(&b, frame, sizeof frame);
    for (int in = 0; in < 16; in++) for (int s = 0; s < 2; s++) {
        uint8_t pl[43]; pl[0] = (uint8_t)((s ? 0x20 : 0x10) | in);
        for (int i = 0; i < 21; i++) { sent[in][s][i] = (uint16_t)(rand() & 0xffff); pl[1 + 2 * i] = (uint8_t)sent[in][s][i]; pl[2 + 2 * i] = (uint8_t)(sent[in][s][i] >> 8); }
        po32_builder_append_packet(&b, PO32_TAG_PATCH, pl, 43, NULL);
    }
    size_t flen; po32_builder_finish(&b, &flen);
    clean_n = po32_render_sample_count(flen, FS);
    po32_render_dpsk_f32(frame, flen, FS, clean, MAXN);
    const size_t six = 264600;
    {   /* side b before side a, and a later single sound with the same instrument number: paired, in order */
        static uint8_t fr2[4096]; po32_builder_t b2; po32_builder_init(&b2, fr2, sizeof fr2);
        static const uint8_t cmd[5] = { 0x23, 0x13, 0x15, 0x25, 0x17 };   /* b3 a3 a5 b5 a7 */
        for (int k = 0; k < 5; k++) { uint8_t pl[43]; pl[0] = cmd[k]; int in = cmd[k] & 15, sd = (cmd[k] >> 4) - 1;
            for (int i = 0; i < 21; i++) { pl[1 + 2 * i] = (uint8_t)sent[in][sd][i]; pl[2 + 2 * i] = (uint8_t)(sent[in][sd][i] >> 8); }
            po32_builder_append_packet(&b2, PO32_TAG_PATCH, pl, 43, NULL); }
        size_t fl2; po32_builder_finish(&b2, &fl2);
        size_t n2 = po32_render_sample_count(fl2, FS); po32_render_dpsk_f32(fr2, fl2, FS, work, MAXN);
        size_t gap = FS / 4, tot = n2 + gap + n2;                         /* the same transfer recorded twice */
        for (size_t i = 0; i < n2; i++) pcm[i] = pcm[n2 + gap + i] = (int16_t)(work[i] * 26000);
        for (size_t i = 0; i < gap; i++) pcm[n2 + i] = 0;
        perc_xfer_decode_into(1, pcm, (unsigned)tot);
        const perc_xfer_t *x = perc_xfer_last();
        static const int want[3] = { 3, 5, 7 }; int ok2 = x->list_n == 6;
        for (int e = 0; e < 6 && ok2; e++) { int in = want[e % 3];
            ok2 = x->list_inst[e] == in && x->list_right[e] == (in != 7) && x->list_ses[e] != x->list_ses[(e + 3) % 6]
                && !memcmp(x->list[e][0], sent[in][0], 42) && !memcmp(x->list[e][1], sent[in][in != 7], 42); }
        printf("ordered list: side b first, two transfers in one recording: %d entries   %s\n", x->list_n, ok2 ? "ok" : "FAIL");
    }
    printf("transfer: 32 patch packets, %.2f s; a slot's sample keeps 6.00 s\n", clean_n / (double)FS);

    for (size_t i = 0; i < clean_n; i++) work[i] = clean[i] * 0.8f;
    run("clean, full transfer (no 6 s cut)", clean_n, MAXN, 32);
    run("clean, 6 s", clean_n, six, 15);
    run("clean, 12 s (a drum kit's sample)", clean_n, 529152, 32);
    for (size_t i = 0; i < clean_n; i++) work[i] = clean[i] * 0.03f;
    run("quiet (-30 dB)", clean_n, six, 15);
    for (size_t i = 0; i < clean_n; i++) { float v = clean[i] * 4; work[i] = v > 0.9f ? 0.9f : v < -0.9f ? -0.9f : v; }
    run("clipped (x4, hard)", clean_n, six, 15);
    for (size_t i = 0; i < clean_n; i++) work[i] = clean[i] * 0.5f + (float)((frand() - 0.5) * 0.5);
    run("noise (SNR ~ 10 dB)", clean_n, six, 15);
    run("noise (SNR ~ 10 dB), 12 s", clean_n, 529152, 32);
    { size_t lead = FS / 2; for (size_t i = 0; i < lead; i++) work[i] = (float)((frand() - 0.5) * 0.01);
      for (size_t i = 0; i < clean_n; i++) work[lead + i] = clean[i] * 0.5f;
      run("0.5 s room noise first", clean_n + lead, six, 13); }
    static const double offs[4] = { -0.001, -0.0002, 0.0002, 0.001 };   /* sample clock offsets (crystals: < 0.01 %) */
    static const char *const offn[4] = { "clock -0.1 %", "clock -0.02 %", "clock +0.02 %", "clock +0.1 %" };
    for (int o = 0; o < 4; o++) {
        double r = 1 + offs[o]; size_t m = (size_t)(clean_n / r);
        for (size_t i = 0; i < m; i++) { double t = i * r; size_t k = (size_t)t; double fr = t - k; work[i] = (float)((k + 1 < clean_n ? clean[k] * (1 - fr) + clean[k + 1] * fr : 0) * 0.5); }
        run(offn[o], m, six, 14);
    }
    {   /* a small speaker and a mic: high pass 400 Hz, low pass 6 kHz, some room echo */
        double hp = 0, lp = 0, px = 0, a_hp = exp(-2 * M_PI * 400 / FS), a_lp = 1 - exp(-2 * M_PI * 6000 / FS);
        for (size_t i = 0; i < clean_n; i++) {
            double x = clean[i] + (i > 900 ? 0.3 * clean[i - 900] : 0);
            hp = a_hp * (hp + x - px); px = x; lp += a_lp * (hp - lp); work[i] = (float)(lp * 0.4);
        }
        run("speaker + mic + echo", clean_n, six, 13);
    }
    return 0;
}
