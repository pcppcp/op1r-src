/* Host test of screens/usbtrack.c: the mix-loop records + per-track resampling give the master's result when the
 * resampled tracks are mixed with the same gains (linearity), for normal / half / double speed and reverse; tracks
 * without records are silent.  cc -DOP1_HOST -I.. usbtrack_test.c ../screens/usbtrack.c -lm */
#include "../op1_synth.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

extern volatile intptr_t tr_rec[]; extern int16_t tr_out[4][64];
void tr_resample(int32_t pos, int32_t inc, const int16_t *gain_inc);
int fr_tracks_needed(void) { return 0; }
volatile uint8_t sc_blk, tr_at;
static int16_t coef[16 * 64];
const int16_t *tr_host_coef = coef; int16_t tr_host_gain = 32767;
static int16_t track[4][2000], mixL[600];                  /* mixL lives at "address" 0x1000 for the hooks */

static int16_t s16(double v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)lrint(v); }

/* master resampler as in L1 0xFFA06194 (one channel) */
static void master(const int16_t *src, int32_t pos, int32_t inc, int16_t *out)
{
    for (int j = 0; j < 64; j++) {
        pos += inc;
        int32_t idx = pos >> 6, ph = 63 - (pos & 63), acc = 0;
        for (int k = 0; k < 16; k++) acc += coef[ph + 64 * k] * src[idx + k];
        out[j] = (int16_t)(((acc >> 15) * 32767) >> 15);
    }
}

int main(void)
{
    extern volatile uint8_t usb_tracks_live; usb_tracks_live = 1;          /* the track work is gated (usbout.c) */
    for (int k = 0; k < 16; k++)                              /* 64-phase windowed sinc, 16 taps */
        for (int ph = 0; ph < 64; ph++) {
            double x = (k - 7.5) + ph / 64.0, w = 0.5 + 0.5 * cos(M_PI * (k - 7.5 + ph / 64.0) / 8.0);
            coef[ph + 64 * k] = s16(32767 * w * (fabs(x) < 1e-9 ? 1 : sin(M_PI * x) / (M_PI * x)) * 0.98);
        }
    for (int t = 0; t < 4; t++) for (int i = 0; i < 2000; i++)
        track[t][i] = s16(6000 * sin(i * (0.01 + 0.013 * t)) + 2000 * sin(i * 0.31 * (t + 1)));
    const int16_t gain[4] = { 32767, 16384, 8192, 0 };        /* track 4 at gain 0: still has a track out */
    const int32_t speeds[][3] = { { 576, 64, 83 }, { 576, 32, 51 }, { 576, 128, 147 }, { 4736, -64, 83 } };
    for (int sp = 0; sp < 4; sp++) {
        int N = speeds[sp][2];
        for (int i = 0; i < 600; i++) mixL[i] = 0;
        tr_rec[0] = 0;
        for (int t = 0; t < 4; t++) {                         /* mix loop 1, as the L1 code + the hook's log */
            int r = tr_rec[0]++;
            tr_rec[1 + 4 * r] = (intptr_t)&track[t][100];
            tr_rec[2 + 4 * r] = 0x1000;                       /* dest: start of the mixed buffer */
            tr_rec[3 + 4 * r] = t; tr_rec[4 + 4 * r] = N;
            for (int i = 0; i < N; i++) mixL[i] = s16(mixL[i] + ((gain[t] * track[t][100 + i]) >> 15));
        }
        int16_t m[64];
        master(mixL, speeds[sp][0], speeds[sp][1], m);
        tr_resample(speeds[sp][0], speeds[sp][1], 0);
        double err = 0, ref = 0;
        for (int j = 0; j < 64; j++) {
            int32_t sum = 0;
            for (int t = 0; t < 4; t++) sum += (gain[t] * tr_out[t][j]) >> 15;
            err += (double)(sum - m[j]) * (sum - m[j]); ref += (double)m[j] * m[j];
        }
        double snr = 10 * log10(ref / (err + 1e-9));
        printf("speed %d/64, start %d: mixed track outs vs master SNR %.1f dB\n", speeds[sp][1], speeds[sp][0], snr);
        assert(snr > 50);
        int nz = 0; for (int j = 0; j < 64; j++) nz |= tr_out[3][j];
        assert(nz);                                           /* pre-fader: track 4 (gain 0) still has signal */
    }
    tr_rec[0] = 0; tr_resample(576, 64, 0);
    for (int t = 0; t < 4; t++) for (int j = 0; j < 64; j++) assert(!tr_out[t][j]);   /* no records: silence */
    printf("ok\n");
    return 0;
}
