/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
/* Shared wavetable DSP. State and audio inputs belong to the caller; no firmware addresses. */
#ifndef WORKSHOP_WAVETABLE_DSP_H
#define WORKSHOP_WAVETABLE_DSP_H
#include <stdint.h>
#include "wavetables.h"
#include "wavetable_level.h"
#define WAVETABLE_BLOCK 128
#ifdef __bfin__
#include "wavetable_pairs.h"
/* wavetable_dsp.S's samples: r as wt_block's comment lists it */
#define WT_RING 13
#define WT_CHUNK 64                                      /* the ramp's samples per wt_block call */
typedef struct {
    uint32_t ph1, ph2, y23, y01;
    const uint32_t *pa;
    uint32_t n, k1, *ring, ring_bytes;
    const uint32_t *const *rows;
} WtRun;
void wt_block(WtRun *r, int16_t *out, const uint32_t *pitch);
typedef struct {
    uint32_t *ring;
    const uint32_t **rows;
    int32_t *control;
    const int32_t *delta;
    uint32_t n;
    const uint32_t *pairs;
} WtPrep;
void wt_prep(WtPrep *p);
#define WT_CV (0x8001u << 16 | 0x090Fu)
#define WT_CA (32767u << 16 | 1u)
#define WT_XF(position) (((uint32_t)(position) & 0x7FFF) << 16 | ((((uint32_t)(position) & 0x7FFF) - 32768u) & 0xFFFF))
#define WT_GC(g) ((uint32_t)(g) << 16 | (uint32_t)(32768 - (g)))
static uint32_t wt_ramp_ring[WT_CHUNK * WT_RING];
/* the rows the kernel reads: wt_pairs, or the copy in the L1 pool (wavetable.c rows_place) */
static const uint32_t (*wt_rows_at)[WT_SIZE] = wt_pairs;
static const uint32_t *wt_ramp_rows[WT_CHUNK];
#endif
typedef struct {
    uint32_t ph1, ph2;
    int32_t y1, y2, y3, y4;
    int32_t control[7]; /* previous block: wave position, detune, filter/gain coefficients and bounded static level trim */
    int control_ready;
} WavetableState;

/* Trilinear static compensation: wave x cutoff x resonance. The table is
 * bounded -6.02..+2dB and independent of the audio signal, pitch and velocity. */
static inline int32_t wt_level_lerp(int32_t a,int32_t b,int32_t f)
{ return a+(((b-a)*f)>>15); }
static inline int32_t wt_level_at(int32_t wave,int32_t cutoff,int32_t resonance)
{
    int32_t wp=wave*7,cp=cutoff*8,rp=resonance*4;
    int wi=wp>>15,ci=cp>>15,ri=rp>>15;
    int32_t a[2];
    for(int w=0;w<2;w++) {
        int i=((wi+w)*9+ci)*5+ri;
        int32_t lo=wt_level_lerp(wt_level[i],wt_level[i+1],rp&32767);
        int32_t hi=wt_level_lerp(wt_level[i+5],wt_level[i+6],rp&32767);
        a[w]=wt_level_lerp(lo,hi,cp&32767);
    }
    return wt_level_lerp(a[0],a[1],wp&32767);
}

/* soft clip: 1.5x - 0.5x^3 over +-32767, flat beyond */
static inline int32_t wavetable_soft(int32_t x)
{
    if (x > 32767) x = 32767;
    if (x < -32767) x = -32767;
    return (x * (98304 - ((x * x) >> 15))) >> 16;
}

/* one table at phase ph, linear interpolation between the 256 points */
static inline int32_t wavetable_read(const int16_t *w, uint32_t ph)
{
    const uint32_t i = ph >> 24;
    const int32_t f = (int32_t)((ph >> 9) & 0x7FFF);
    const int32_t a = w[i];
    return a + (((w[i + 1] - a) * f) >> 15);
}

static inline int32_t wavetable_osc(const int16_t *wa, const int16_t *wb, int32_t frac, uint32_t ph)
{
    const int32_t a = wavetable_read(wa, ph);
    return a + (((wavetable_read(wb, ph) - a) * frac) >> 15);
}

static inline void wavetable_dsp_on(WavetableState *v)
{
    v->control_ready = 0;
    v->ph1 = 0;
    v->ph2 = 0x55555555u;
    v->y1 = v->y2 = v->y3 = v->y4 = 0;
}

/* one output sample: oscillators, drive, ladder filter; the caller's ramped or constant controls. Always inlined:
   as a call (gcc's choice with two call sites) its ten arguments cost more than the fast path saves. */
static inline __attribute__((always_inline)) int16_t wavetable_sample(uint32_t *ph1, uint32_t *ph2, int32_t *y, uint32_t inc, int32_t position,
                                       int32_t k1, int32_t g, int32_t k, int32_t drive, int32_t makeup, int32_t level)
{
    const int16_t *wa = wt_waves[position >> 15];
    const int16_t *wb = wt_waves[(position >> 15) + ((position >> 15) < WT_WAVES - 1)];
    const int32_t frac = position & 0x7FFF;
    const uint32_t dd = ((inc >> 7) * (uint32_t)(k1 >> 3)) >> 12;   /* up to inc/128 each way */
    *ph1 += inc - dd;
    *ph2 += inc + dd;
    int32_t x = (wavetable_osc(wa, wb, frac, *ph1) + wavetable_osc(wa, wb, frac, *ph2)) >> 2;   /* +-15000: headroom for drive */
    x = wavetable_soft((x * drive) >> 12);
    const int32_t u = wavetable_soft(x - ((k * y[3]) >> 12));
    y[0] += (g * (u - y[0])) >> 15;
    y[1] += (g * (y[0] - y[1])) >> 15;
    y[2] += (g * (y[1] - y[2])) >> 15;
    y[3] += (g * (y[2] - y[3])) >> 15;
    /* Apply after the existing waveshaper so drive/feedback coloration stays intact. */
    int32_t sample = (wavetable_soft((y[3] * makeup) >> 12) * level) >> 12;
    return sample > 32767 ? 32767 : sample < -32768 ? -32768 : (int16_t)sample;
}

static inline void wavetable_dsp_render(WavetableState *v, int16_t *out,
                                       const int16_t *knobs,
                                       const volatile uint32_t *pitch, int vel)
{
    if (vel < 1 || vel > 127) vel = 100;
    const int32_t k0 = knobs[0] < 0 ? 0 : knobs[0];
    const int32_t k1 = knobs[1] < 0 ? 0 : knobs[1];
    const int32_t k2 = knobs[2] < 0 ? 0 : knobs[2];
    const int32_t k3 = knobs[3] < 0 ? 0 : knobs[3];

    const int32_t pos = k0 * (WT_WAVES - 1);              /* wave index in the high bits, Q15 fraction */

    const int32_t ci = k2 * WT_CUTOFFS;                   /* cutoff table index, Q15 fraction */
    const int32_t g0 = wt_cutoff[ci >> 15];
    const int32_t g = g0 + (((wt_cutoff[(ci >> 15) + 1] - g0) * (ci & 0x7FFF)) >> 15);
    const int32_t k = (k3 * 15974) >> 15;                 /* resonance, Q12, up to ~3.9 */
    const int32_t drive = 4096 + (k3 * 3 >> 3);           /* Q12, 1x to ~4x */
    /* output gain, Q12: 0.75x (resonance thins the passband, so up to ~1x) times velocity / 127; TE's voice level,
       so chords stay under the voice-sum bus's rail */
    /* level pass 2026-10-29: x1.161 (+1.3 dB, Q12 4755) and the preset trim (word 7); a 16-bit operand */
    int32_t makeup = (((((3072 + (k >> 2)) * vel) >> 7) * 4755 >> 12) * (knobs[7] > 0 ? knobs[7] : 8192)) >> 13;
    if (makeup > 32767) makeup = 32767;

    /* Ramp derived coefficients, not filter state. No exp/table setup in the sample loop. */
#ifdef KENNEL_LEVEL_CALIBRATION
    const int32_t level = 4096; /* offline generator: original production gain */
#else
    const int32_t level = wt_level_at(k0,k2,k3);
#endif
    const int32_t target[7] = {pos, k1, g, k, drive, makeup, level};
    int32_t control[7], delta[7];
    for (int j = 0; j < 7; j++) {
        const int32_t previous = v->control_ready ? v->control[j] : target[j];
        control[j] = previous * WAVETABLE_BLOCK;
        delta[j] = target[j] - previous;
        v->control[j] = target[j];
    }
    v->control_ready = 1;
    uint32_t ph1 = v->ph1, ph2 = v->ph2;
    int32_t y[4] = {v->y1, v->y2, v->y3, v->y4};
    if (!(delta[0] | delta[1] | delta[2] | delta[3] | delta[4] | delta[5] | delta[6])) {
        /* knobs unchanged (the usual case): the controls are constant over the block; same samples as the ramp */
        const int32_t position = control[0] >> 7, k1 = control[1] >> 7, g = control[2] >> 7;
        const int32_t k = control[3] >> 7, drive = control[4] >> 7, makeup = control[5] >> 7, level = control[6] >> 7;
#ifdef __bfin__
        /* the kernel's packed constants; position >> 15 is at most 6 (32767 * 7 < 7 * 32768), so wb is the next row */
        const uint32_t cs = 0x2000u << 16 | (uint32_t)drive, cf = 4096u << 16 | (uint32_t)k;
        const uint32_t cm = (uint32_t)makeup << 16 | (uint32_t)level, k1s = (uint32_t)(k1 >> 3);
        uint32_t ring[WT_RING] = {WT_CV, WT_XF(position), WT_CV, cs, WT_CA, WT_CV, cf, WT_CA, WT_GC(g), cm, WT_CV, WT_CA, k1s};
        WtRun r = {ph1, ph2, (uint32_t)y[3] << 16 | (uint16_t)y[2], (uint32_t)y[1] << 16 | (uint16_t)y[0],
                   wt_rows_at[position >> 15], WAVETABLE_BLOCK, k1s, ring, sizeof ring, 0};
        wt_block(&r, out, (const uint32_t *)pitch);
        ph1 = r.ph1;
        ph2 = r.ph2;
        y[0] = (int16_t)r.y01;
        y[1] = (int32_t)r.y01 >> 16;
        y[2] = (int16_t)r.y23;
        y[3] = (int32_t)r.y23 >> 16;
#else
        for (int i = 0; i < WAVETABLE_BLOCK; i++)
            out[i] = wavetable_sample(&ph1, &ph2, y, pitch[i], position, k1, g, k, drive, makeup, level);
#endif
    } else {
#ifdef __bfin__
        /* the ramp: each sample's constants and wave row, a chunk at a time, then the same kernel */
        WtRun r = {ph1, ph2, (uint32_t)y[3] << 16 | (uint16_t)y[2], (uint32_t)y[1] << 16 | (uint16_t)y[0],
                   0, WT_CHUNK, (uint32_t)(control[1] >> 10), wt_ramp_ring, sizeof wt_ramp_ring, wt_ramp_rows};
        if (!wt_ramp_ring[0])                            /* the ring's constant words and halves, once */
            for (uint32_t *q = wt_ramp_ring; q < wt_ramp_ring + WT_CHUNK * WT_RING; q += WT_RING) {
                q[0] = q[2] = q[5] = q[10] = WT_CV;
                q[4] = q[7] = q[11] = WT_CA;
                q[3] = 0x2000u << 16;
                q[6] = 4096u << 16;
                q[12] = 0;
            }
        WtPrep prep = {wt_ramp_ring, wt_ramp_rows, control, delta, WT_CHUNK, wt_rows_at[0]};
        for (int c = 0; c < WAVETABLE_BLOCK; c += WT_CHUNK) {
            r.k1 = (uint32_t)(control[1] >> 10);
            wt_prep(&prep);
            wt_block(&r, out + c, (const uint32_t *)pitch + c);
        }
        ph1 = r.ph1;
        ph2 = r.ph2;
        y[0] = (int16_t)r.y01;
        y[1] = (int32_t)r.y01 >> 16;
        y[2] = (int16_t)r.y23;
        y[3] = (int32_t)r.y23 >> 16;
#else
        for (int i = 0; i < WAVETABLE_BLOCK; i++) {
            out[i] = wavetable_sample(&ph1, &ph2, y, pitch[i], control[0] >> 7, control[1] >> 7, control[2] >> 7,
                                      control[3] >> 7, control[4] >> 7, control[5] >> 7, control[6] >> 7);
            for (int j = 0; j < 7; j++) control[j] += delta[j];
        }
#endif
    }
    v->ph1 = ph1;
    v->ph2 = ph2;
    v->y1 = y[0];
    v->y2 = y[1];
    v->y3 = y[2];
    v->y4 = y[3];
}

/* the morphed wave at knob position k0 (0..32767) and phase ph, for the screen's conveyor belt */
static inline int32_t wavetable_dsp_shape(int32_t k0, uint32_t ph)
{
    const int32_t pos = (k0 < 0 ? 0 : k0) * (WT_WAVES - 1);
    const int16_t *wa = wt_waves[pos >> 15];
    const int16_t *wb = wt_waves[(pos >> 15) + ((pos >> 15) < WT_WAVES - 1)];
    return wavetable_osc(wa, wb, pos & 0x7FFF, ph);
}

#endif
