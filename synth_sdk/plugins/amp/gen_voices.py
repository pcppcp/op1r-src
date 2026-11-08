#!/usr/bin/env python3
"""Writes amp_voices.h: Amp's five voices (tone stack, cabinet, drive gain, bias, sag) and the 2x half-band filter.

    python3 gen_voices.py            (needs numpy and scipy; run from anywhere)

Tone stack: the Fender Bassman 5F6-A treble/middle/bass network at fixed knob positions per voice, from D. T. Yeh and
J. O. Smith's analysis (https://ccrma.stanford.edu/~dtyeh/tonestack/, "Discretization of the '59 Fender Bassman
tone stack", DAFx 2006), component values as published there. The bass pot is log taper, l = exp((L - 1) * 3.4).
Cabinet: an original voicing per voice (one-pole low cut, resonant low-pass for presence, low-pass roll-off).
Each voice's stack and cabinet are one cascade (bilinear transform): two first-order low cuts and three Q14 biquads,
checked here against the float design. Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
"""
import math
from pathlib import Path

import numpy as np
from scipy import signal

RATES = (22050, 44100)
R1, R2, R3, R4, C1, C2, C3 = 250e3, 1e6, 25e3, 56e3, 250e-12, 20e-9, 20e-9

# name, stack (treble, middle, bass), cab (low cut Hz, presence Hz, presence Q, roll-off Hz),
# drive gain at full Drive (x), bias (fraction of full scale, asymmetry), sag (0..1), output level (dB; set from native
# captures of a guitar-like DI at Volume 50: Drive 25 about as loud as Drive 0, full Drive near the old Amp's full
# Drive, about -16 dBFS rms, with peaks below -1.5 dBFS; CLEAN stops lower, at about -19.5, for its peaks)
VOICES = (
    ('CLEAN', (0.7, 0.6, 0.4), (70, 4000, 1.1, 6500), 4, 0.00, 0.10, -1.6),
    ('CRUNCH', (0.5, 0.5, 0.5), (90, 3000, 1.5, 5000), 8, 0.06, 0.20, -2.8),
    ('LEAD', (0.6, 0.35, 0.6), (100, 2800, 2.0, 4500), 20, 0.10, 0.25, -4.4),
    ('BASS', (0.35, 0.8, 0.7), (35, 2200, 0.9, 3500), 6, 0.04, 0.15, -2.2),
    ('FUZZ', (0.5, 0.2, 0.6), (90, 2500, 1.8, 4000), 32, 0.25, 0.35, -5.8),
)


def stack_s(t, m, L):
    l = math.exp((L - 1) * 3.4)
    b1 = t*C1*R1 + m*C3*R3 + l*(C1*R2 + C2*R2) + (C1*R3 + C2*R3)
    b2 = (t*(C1*C2*R1*R4 + C1*C3*R1*R4) - m*m*(C1*C3*R3*R3 + C2*C3*R3*R3) + m*(C1*C3*R1*R3 + C1*C3*R3*R3 + C2*C3*R3*R3)
          + l*(C1*C2*R1*R2 + C1*C2*R2*R4 + C1*C3*R2*R4) + l*m*(C1*C3*R2*R3 + C2*C3*R2*R3) + (C1*C2*R1*R3 + C1*C2*R3*R4 + C1*C3*R3*R4))
    b3 = (l*m*(C1*C2*C3*R1*R2*R3 + C1*C2*C3*R2*R3*R4) - m*m*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4)
          + m*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4) + t*C1*C2*C3*R1*R3*R4 - t*m*C1*C2*C3*R1*R3*R4 + t*l*C1*C2*C3*R1*R2*R4)
    a1 = (C1*R1 + C1*R3 + C2*R3 + C2*R4 + C3*R4) + m*C3*R3 + l*(C1*R2 + C2*R2)
    a2 = (m*(C1*C3*R1*R3 - C2*C3*R3*R4 + C1*C3*R3*R3 + C2*C3*R3*R3) + l*m*(C1*C3*R2*R3 + C2*C3*R2*R3) - m*m*(C1*C3*R3*R3 + C2*C3*R3*R3)
          + l*(C1*C2*R2*R4 + C1*C2*R1*R2 + C1*C3*R2*R4 + C2*C3*R2*R4)
          + (C1*C2*R1*R4 + C1*C3*R1*R4 + C1*C2*R3*R4 + C1*C2*R1*R3 + C1*C3*R3*R4 + C2*C3*R3*R4))
    a3 = (l*m*(C1*C2*C3*R1*R2*R3 + C1*C2*C3*R2*R3*R4) - m*m*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4)
          + m*(C1*C2*C3*R3*R3*R4 + C1*C2*C3*R1*R3*R3 - C1*C2*C3*R1*R3*R4) + l*C1*C2*C3*R1*R2*R4 + C1*C2*C3*R1*R3*R4)
    return [b3, b2, b1, 0.0], [a3, a2, a1, 1.0]


def voice_filters(v, fs):
    """Two first-order low cuts (the stack's lowest pole with its zero at DC, and the cabinet's low cut; Q15 poles keep
    them accurate near DC) and three biquads (the rest of the stack, presence, roll-off). Band power 150..4000 Hz = the voice's level."""
    _, (t, m, L), (hp, pf, pq, lp), *_ = v
    z, p, k = signal.bilinear_zpk(*signal.tf2zpk(*stack_s(t, m, L)), fs)
    z, p = list(z), list(np.real_if_close(p))
    zdc = min(range(len(z)), key=lambda i: abs(z[i] - 1)); z.pop(zdc)
    plo = max(range(len(p)), key=lambda i: p[i].real); pole = p.pop(plo).real
    lows = [pole, math.exp(-2 * math.pi * hp / fs)]                       # (1 - z^-1) / (1 - p z^-1)
    bqs = [np.concatenate([np.real(np.poly(z)), np.real(np.poly(p))])]
    for f, q in ((pf, pq), (lp, 0.707)):
        w = 2 * math.pi * f / fs; al = math.sin(w) / (2 * q); c = math.cos(w)
        bqs.append(np.array([(1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al]) / (1 + al))
    gain = k
    fr = np.geomspace(150, 4000, 64)
    gain /= math.sqrt(np.mean(np.abs(response(lows, bqs, gain, fr, fs)) ** 2))
    gain *= 10 ** (v[6] / 20)
    return lows, bqs, gain


def response(lows, bqs, gain, f, fs):
    zi = np.exp(-2j * np.pi * f / fs)
    h = np.full(len(f), gain, complex)
    for pl in lows:
        h *= (1 - zi) / (1 - pl * zi)
    for b in bqs:
        h *= (b[0] + b[1] * zi + b[2] * zi * zi) / (b[3] + b[4] * zi + b[5] * zi * zi)
    return h


def quantize(lows, bqs, gain, fs):
    """Q15 poles; each biquad's numerator scaled to peak gain <= 1 (headroom for int16 states), Q14; the total gain
    goes to one Q12 output multiply."""
    ql = [round(pl * 32768) for pl in lows]
    qb, g = [], gain
    f = np.linspace(1, fs / 2 - 1, 2048)
    for b in bqs:
        pk = np.max(np.abs(response([], [b], 1, f, fs))); g *= pk
        qb.append([round(c * 16384) for c in list(b[:3] / pk) + list(b[4:6])])
    if max(abs(c) for row in qb for c in row) > 32767 or max(ql) > 32767 or round(g * 4096) > 32767:
        raise SystemExit('coefficient overflow')
    return ql, qb, round(g * 4096)


def check(name, fs, lows, bqs, gain, ql, qb, qg):
    f = np.geomspace(60, 0.42 * fs, 200)
    h = response(lows, bqs, gain, f, fs)
    hq = response([c / 32768 for c in ql], [[r[0] / 16384, r[1] / 16384, r[2] / 16384, 1, r[3] / 16384, r[4] / 16384] for r in qb],
                  qg / 4096, f, fs)
    err = np.max(np.abs(20 * np.log10(np.abs(hq) + 1e-12) - 20 * np.log10(np.abs(h) + 1e-12)) * (np.abs(h) > 0.03))
    if err > 0.5:
        raise SystemExit(f'{name} {fs}: quantized error {err:.2f} dB')
    return err


def halfband():
    """15-tap half-band for the 2x drive stage: centre 1/2, taps at +-1, 3, 5, 7 (2x samples) summing to 1/2."""
    h = signal.remez(15, [0, 0.18, 0.32, 0.5], [1, 0], fs=1.0)    # 2x rate: pass 0..0.36 fs, stop from 0.64 fs
    odd = np.array([h[7 + d] + h[7 - d] for d in (1, 3, 5, 7)])
    odd *= 0.5 / odd.sum()
    c = [round(v * 32768) for v in odd / 2]
    c[0] = 8192 - sum(c[1:])
    full = np.zeros(15); full[7] = 0.5
    for d, v in zip((1, 3, 5, 7), c):
        full[7 - d] = full[7 + d] = v / 32768
    _, hz = signal.freqz(full, worN=np.linspace(0.32, 0.5, 200) * 2 * np.pi)
    print(f'half-band stopband {20 * np.log10(np.max(np.abs(hz))):.1f} dB')
    return c


def main():
    out = ['/* Generated by gen_voices.py: Amp voices and the 2x half-band filter. Do not edit. */',
           '#ifndef AMP_VOICES_H', '#define AMP_VOICES_H', '#include <stdint.h>',
           '#define AMP_VOICES 5',
           'typedef struct { int16_t gain, bias, sag, out, low[2], bq[3][5]; } amp_voice;   /* low: Q15 poles; bq: b0 b1 b2 a1 a2, Q14 */',
           f'static const int16_t amp_hb[4]={{{",".join(map(str, halfband()))}}};   /* half-band taps at +-1, 3, 5, 7 (Q15) */',
           'static const char *const amp_voice_names[AMP_VOICES]={' + ','.join(f'"{v[0]}"' for v in VOICES) + '};']
    for fs in RATES:
        rows = []
        for v in VOICES:
            lows, bqs, gain = voice_filters(v, fs)
            ql, qb, qg = quantize(lows, bqs, gain, fs)
            err = check(v[0], fs, lows, bqs, gain, ql, qb, qg)
            print(f'{fs} {v[0]:7s} quantized max error {err:.3f} dB, output gain {qg / 4096:.2f}')
            rows.append('{%d,%d,%d,%d,{%d,%d},{%s}}' % (v[3] * 256, round(v[4] * 32768), round(v[5] * 32768), qg, *ql,
                                                       ','.join('{%s}' % ','.join(map(str, r)) for r in qb)))
        out.append(f'static const amp_voice amp_voices_{fs}[AMP_VOICES]={{\n ' + ',\n '.join(rows) + '};')
    out.append('#endif')
    Path(__file__).with_name('amp_voices.h').write_text('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
