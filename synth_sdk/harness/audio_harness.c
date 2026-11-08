/* Host audio render of one plugin -> raw int16 mono 44.1 kHz on stdout (harness/audio.py writes the WAV).
 *   Voices run at OP1_VOICE_RATE (128 samples per block, as on R246) and are decimated 2:1 here like the firmware.
 *   synth: -DENGINE=<symbol>: a phrase on voice 0 (C3 E3 G3 C4 G3 E3, 0.4 s each, velocities 127 / 64 alternating)
 *   fx:    -DFX=<symbol>: plucked saw notes through the FX at position 0, then 1.5 s of silence (tails)
 * args: knob0..3 (0..32767). Reports a peak / NaN-free summary on stderr; exit 1 if the output is silent. */
#include "../op1_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6]; int16_t host_voice_env[128]; int16_t host_accel[3] = { 0, 0, 16384 };
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_node_set_visible(void *n, int on) { (void)n; (void)on; }
uint8_t *host_node_style_rgba(void *s, const char *id) { static uint8_t c[4]; (void)s; (void)id; return c; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }
void *host_raster(void) { return 0; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c) { (void)r; (void)a; (void)b; (void)w; (void)c; }
void *read_fx_param_safe(int slot, int pos) { (void)slot; (void)pos; return 0; }
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t sz, const uint8_t *c) { (void)x; (void)y; (void)sz; (void)c; return o; }
void host_text_set(void *t, const char *s) { (void)t; (void)s; }
void host_text_draw(void *t, void *r, const int32_t *m) { (void)t; (void)r; (void)m; }
int host_shift;
int op1_sc_source(int src, int16_t *out) { (void)src; (void)out; return 0; }   /* ENTE: no sidechain on the host */

static int peak; static long nonzero;
static void emit(const int16_t *b, int n)
{
    for (int i = 0; i < n; i++) { int a = abs(b[i]); if (a > peak) peak = a; nonzero += b[i] != 0; }
    fwrite(b, 2, (size_t)n, stdout);
}
static int32_t inc_of(int midi, double sr) { return (int32_t)lrint(4294967296.0 * 440.0 * pow(2, (midi - 69) / 12.0) / sr); }
#ifdef ENGINE
/* 2:1 decimator for the voice output: a windowed-sinc half-band low-pass (our own design, like R246's in role) */
#define DEC_TAPS 63
static double dec_h[DEC_TAPS], dec_z[DEC_TAPS];
static void dec_init(void)
{
    for (int n = 0; n < DEC_TAPS; n++) {
        double x = n - (DEC_TAPS - 1) / 2.0, w = 0.42 - 0.5 * cos(2 * M_PI * n / (DEC_TAPS - 1)) + 0.08 * cos(4 * M_PI * n / (DEC_TAPS - 1));
        dec_h[n] = (x == 0 ? 0.5 : sin(M_PI * x / 2) / (M_PI * x)) * w;
    }
}
static void dec_block(const int16_t *in, int16_t *out)    /* OP1_BLOCK in -> OP1_BLOCK / 2 out */
{
    for (int i = 0; i < OP1_BLOCK; i++) {
        for (int n = DEC_TAPS - 1; n > 0; n--) dec_z[n] = dec_z[n - 1];
        dec_z[0] = in[i];
        if (i & 1) { double a = 0; for (int n = 0; n < DEC_TAPS; n++) a += dec_h[n] * dec_z[n]; long v = lrint(a); out[i >> 1] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
    }
}
#endif

int main(int argc, char **argv)
{
    int16_t k[8] = { 0, 0, 0, 0, 0x5301, 0, 0, 0 };
    for (int i = 0; i < 4 && i + 1 < argc; i++) k[i] = (int16_t)atoi(argv[i + 1]);
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
#ifdef ENGINE
    extern const op1_engine_t ENGINE;
    /* a 2 s sampler recording for engines that use the slot's sample: a vowel-ish chord with a slow swell */
    static int16_t smp[OP1_SAMPLE_GUARD + 88200]; static struct { int16_t *d; int16_t p[OP1_PEAK_BUCKETS]; } sb;
    for (int i = 0; i < 88200; i++)
        smp[OP1_SAMPLE_GUARD + i] = (int16_t)(9000 * sin(i * 0.0373) * (0.5 + 0.5 * sin(i * 0.00007)) + 3000 * sin(i * 0.0559));
    sb.d = smp; for (int i = 0; i < OP1_PEAK_BUCKETS; i++) sb.p[i] = i < 345 ? 12000 : 0;
    host_sample_buffer[0] = (const uint8_t *)&sb;
    static const int notes[] = { 48, 52, 55, 60, 55, 52 };
    int16_t out[OP1_BLOCK], dec[OP1_BLOCK / 2];
    dec_init();
    for (int n = 0; n < 6; n++) {
        for (int i = 0; i < 128; i++) host_pitch_inc[i] = inc_of(notes[n], OP1_VOICE_RATE);
        host_voice_vel[0] = (int16_t)((n & 1 ? 64 : 127) << 8);
        ENGINE.note_on(host_voice_mem, k);
        for (int b = 0; b < 276; b++) {                         /* 0.4 s at 689 blocks / s */
            if (b == 220 && ENGINE.note_off) ENGINE.note_off(host_voice_mem);
            /* a stand-in stock envelope: 20-block attack, sustain 3/4, release from the note off (not applied to
             * the output here, as before: the harness renders the raw engine) */
            int32_t e = b < 20 ? 32767 * b / 20 : b < 220 ? 24575 : 24575 - 24575 * (b - 220) / 56;
            for (int i = 0; i < 128; i++) host_voice_env[i] = (int16_t)e;
            ENGINE.render(host_voice_mem, out, k); dec_block(out, dec); emit(dec, OP1_BLOCK / 2);
        }
    }
#else
    extern const op1_fx_t FX;
    static int32_t scratch[OP1_FX_SCRATCH_BYTES / 4];
    for (int i = 0; i < 3; i++) host_fx_scratch[i] = scratch;
    for (int i = 0; i < 8; i++) host_fx_knobs[0][i] = k[i];
    FX.init(0);
    int16_t buf[OP1_FX_BLOCK]; uint32_t ph = 0; int32_t env = 0; int note = 0;
    static const int notes[] = { 48, 55, 60, 63, 67, 60 };
    for (int b = 0; b < 6 * 276 + 1034; b++) {                 /* 6 plucks of 0.4 s, then 1.5 s silence */
        if (b % 276 == 0 && note < 6) { env = 20000; ph = 0; note++; }
        int32_t inc = inc_of(notes[note ? note - 1 : 0], OP1_SAMPLE_RATE);
        for (int i = 0; i < OP1_FX_BLOCK; i++) {
            ph += (uint32_t)inc;
            buf[i] = (int16_t)((((int32_t)ph >> 16) * env) >> 15);
            env -= env >> 11;
        }
        if (note == 6 && b % 276 > 200) for (int i = 0; i < OP1_FX_BLOCK; i++) buf[i] = 0, env = 0;
        FX.render(0, buf, k); emit(buf, OP1_FX_BLOCK);
    }
#endif
    fprintf(stderr, "peak %d (%.1f dBFS), non-zero samples %ld\n", peak, 20 * log10((peak + 1e-9) / 32768.0), nonzero);
    return peak < 64;
}
