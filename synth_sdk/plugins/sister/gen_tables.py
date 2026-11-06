#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
"""Generate sister_tables.h: every rate-dependent constant of SISTER from two numbers.

FS is the FX callback rate (the OP-1's 44.1 kHz output stream: 64 FX samples per block, see
OP1-Emulator docs/emu-audio-clock-2026-10-21.md). The wet path runs at FI = FS / 2 (32 samples per block).
Change FS here and regenerate if the native pipes check says otherwise; nothing else holds a rate.

Formant values: textbook vowel averages (Peterson and Barney 1952, adult voices, rounded and blended
toward a mixed choir). Half-band filter: Kaiser-windowed sinc. Decay gains: g = 10^(-3 L / (FI RT60)).
No firmware data.
"""
from pathlib import Path
import math

P = Path(__file__).resolve().parent
FS = 44100
FI = FS // 2
N = 32                                   # internal samples per block
BLOCK_HZ = FS / 64                       # blocks per second

# tank: four unequal lines (mutually prime, 77..136 ms at 22.05 kHz), in-loop allpasses, input diffusers
LINES = (1693, 2083, 2531, 3001)
MOD_MAX = 24                             # modulation room per line (samples)
LAP = (211, 263, 337, 401)               # in-loop allpass delays (ring 512)
DIFF = (142, 107, 379, 277)              # input diffusers (ring 512)
ER_L = (307, 541, 829, 1187)             # early taps after the pre-delay (pre ring)
ER_R = (389, 653, 977, 1361)
PRE_MAX_MS = 60

# reverse swell ring: two windows' worth of history at the longest window plus fade and stereo offset
W_MIN_S, W_MAX_S = 0.45, 1.40
F_MAX_S = 0.060
DELTA_MAX_S = 0.020
REV = 2 * (round(W_MAX_S * FI) + round(F_MAX_S * FI)) + round(DELTA_MAX_S * FI) + 64

C3 = 130.8128


def q15(x):
    return max(-32768, min(32767, round(x * 32768)))


def lerp_rows(f, n=129):
    return [f(i / (n - 1)) for i in range(n)]


def halfband(taps=19, beta=7.0):
    """odd-tap coefficients c_j for taps +-(2j+1); the centre is 1/2 (Q15)"""
    m = taps // 2

    def i0(x):
        s, t, k = 1.0, 1.0, 1
        while t > 1e-12:
            t *= (x / (2 * k)) ** 2
            s += t
            k += 1
        return s
    h = []
    for n in range(-m, m + 1):
        w = i0(beta * math.sqrt(1 - (n / m) ** 2)) / i0(beta)
        s = 0.5 if n == 0 else math.sin(math.pi * n / 2) / (math.pi * n)
        h.append(s * w)
    tot = sum(h)
    h = [x / tot for x in h]
    return [q15(h[m + 2 * j + 1]) for j in range(m // 2 + 1) if 2 * j + 1 <= m]


def space_row(s):
    rt = 1.8 * (13.0 / 1.8) ** s                       # RT60 seconds
    gains = [q15(10 ** (-3 * L / (FI * rt))) for L in LINES]
    fc = 7500 * (0.45 ** s)                            # tank damping corner
    damp = q15(1 - math.exp(-2 * math.pi * fc / FI))
    pre = round((0.008 + 0.052 * s) * FI)
    er = q15(0.30 - 0.15 * s)
    win = round((W_MIN_S + (W_MAX_S - W_MIN_S) * s) * FI)
    mod = round(7 * 256)                               # tank modulation depth, samples Q8 (fixed: a moving depth jumps the reads)
    return gains + [damp, pre, er, win, mod]


def smooth(x):
    x = max(0.0, min(1.0, x))
    return x * x * (3 - 2 * x)


def voice_row(v):
    xc = max(0.0, min(1.0, (v - 0.30) / 0.40))         # CHOIR -> PIPES crossfade
    choir = math.cos(xc * math.pi / 2)
    pipes = math.sin(xc * math.pi / 2)
    body = 0.32 + 0.25 * math.sin(xc * math.pi)        # the plain tail, fuller in the blend
    d = smooth(v / 0.30)
    depth = 0.30 + 1.20 * d                            # vowel drift, vowel units (aah = 0, ooh = 1, eeh = 2)
    rate = 0.06 + 0.10 * d                             # Hz
    qinv = 1 / (6.0 + 5.0 * d)                         # formant 1/Q
    t60 = 1.0 * 4.0 ** max(0.0, (v - 0.30) / 0.70)     # pipe ring time
    k = math.log2(1000) / (FI * t60)                   # per-sample log2 decay
    bright = 0.52 + 0.28 * smooth((v - 0.3) / 0.7)     # comb loop lowpass coefficient (>= 0.5: no rounding deadband)
    return [q15(choir), q15(pipes), q15(body), round(depth * 4096), round(rate * 2 ** 32 / BLOCK_HZ),
            round(qinv * 16384), round(k * 2 ** 24 * 16), q15(bright)]


def swell_row(w):
    rev = math.sin(w * math.pi / 2)
    fwd = math.cos(w * math.pi / 2)
    fade = round((F_MAX_S - (F_MAX_S - 0.020) * w) * FI)
    delta = round((0.005 + (DELTA_MAX_S - 0.005) * w) * FI)
    return [q15(fwd), q15(rev), fade, delta]


def mix_row(m):
    dry = math.cos(m * math.pi / 2)
    wet = math.sin(m * math.pi / 2)
    tilt = max(0.0, (m - 0.6) / 0.4) * 0.5
    return [round(dry * 16384), round(wet * 16384), q15(tilt)]


# vowels: (log index Q8 of F, gain Q15) for F1..F3; F = 150 * 2^(i/32)
VOWELS = {
    'aah': ((780, 1.0), (1180, 0.55), (2750, 0.22)),
    'ooh': ((340, 1.0), (720, 0.45), (2550, 0.10)),
    'eeh': ((290, 1.0), (2250, 0.40), (2950, 0.24)),
}
FLOG0, FLOG_STEPS = 150.0, 32


def flog(f):
    return round(math.log2(f / FLOG0) * FLOG_STEPS * 256)


def main():
    hb = halfband()
    sp = lerp_rows(space_row)
    vo = lerp_rows(voice_row)
    sw = lerp_rows(swell_row)
    mx = lerp_rows(mix_row)
    nlog = int(math.log2(4200 / FLOG0) * FLOG_STEPS) + 2
    # resonator per log-frequency index: 2 cos(theta) (Q14) and pi F / FI * log2(e) (Q24, times 1/Q -> pole radius)
    fco = [[round(2 * math.cos(2 * math.pi * fr / FI) * 16384), round(math.pi * fr / FI * math.log2(math.e) * 2 ** 24)]
           for fr in (FLOG0 * 2 ** (i / FLOG_STEPS) for i in range(nlog))]
    exp2 = [q15(2 ** (-i / 256)) for i in range(257)]
    sq1mg = [q15(math.sqrt(1 - 2 ** (-i / 256))) for i in range(257)]   # comb input: sqrt(1 - g), g = 2^-x
    root = [round(FI / (C3 * 2 ** (s / 12)) * 65536) for s in range(-24, 25)]
    sine = [q15(math.sin(2 * math.pi * i / 256)) for i in range(257)]
    quarter = [q15(math.sin(i / 128 * math.pi / 2)) for i in range(129)]
    vrows = [[(flog(f), q15(g)) for f, g in VOWELS[v]] for v in ('aah', 'ooh', 'eeh')]

    def arr(name, typ, rows):
        if rows and isinstance(rows[0], list):
            body = ',\n'.join('{' + ','.join(map(str, r)) + '}' for r in rows)
            return f'static const {typ} {name}[{len(rows)}][{len(rows[0])}]={{\n{body}}};\n'
        return f'static const {typ} {name}[{len(rows)}]={{' + ','.join(map(str, rows)) + '};\n'

    lines_total = sum(L + MOD_MAX + 2 for L in LINES)
    out = ['/* Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission. */',
           '/* Generated by gen_tables.py (FS %d, internal %d). Do not edit. */' % (FS, FI),
           '#ifndef SISTER_TABLES_H', '#define SISTER_TABLES_H', '#include <stdint.h>',
           f'#define SI_FS {FS}', f'#define SI_FI {FI}', f'#define SI_N {N}',
           f'#define SI_LINE_MOD {MOD_MAX}', f'#define SI_LINES_TOTAL {lines_total}',
           f'#define SI_REV {REV}', f'#define SI_W_MAX {round(W_MAX_S * FI)}', f'#define SI_F_MAX {round(F_MAX_S * FI)}',
           f'#define SI_DELTA_MAX {round(DELTA_MAX_S * FI)}', f'#define SI_PRE_MAX {round(PRE_MAX_MS / 1000 * FI)}',
           f'#define SI_FLOG_N {nlog}', f'#define SI_HB_N {len(hb)}',
           arr('si_line_len', 'uint16_t', list(LINES)), arr('si_lap_len', 'uint16_t', list(LAP)),
           arr('si_diff_len', 'uint16_t', list(DIFF)),
           arr('si_er_l', 'uint16_t', list(ER_L)), arr('si_er_r', 'uint16_t', list(ER_R)),
           '/* half-band odd taps (Q15) and twice them for the interpolator; word-aligned for the kernels */',
           'static const int16_t si_hb[6] __attribute__((aligned(4)))={' + ','.join(map(str, hb + [0])) + '};',
           'static const int16_t si_hb2[6] __attribute__((aligned(4)))={' + ','.join(str(2 * v) for v in hb + [0]) + '};',
           '/* space: line gains x4, damping, pre-delay, ER gain, swell window, modulation depth (Q8) */',
           arr('si_space', 'int32_t', sp),
           '/* voice: choir, pipes, body, drift depth (Q12 vowels), drift rate (phase/block), 1/Q (Q14), comb log2 decay (Q28/sample), comb brightness */',
           arr('si_voice', 'int32_t', vo),
           '/* swell: forward, reverse, fade samples, stereo offset samples */',
           arr('si_swell', 'int32_t', sw),
           '/* mix: dry, wet (Q14, 16384 = 1), brightness tilt */',
           arr('si_mix', 'int32_t', mx),
           '/* formant resonators, F = 150 * 2^(i/32): 2 cos(2 pi F / FI) Q14, pi F / FI log2(e) Q24 */',
           arr('si_fco', 'int32_t', fco),
           '/* vowels aah, ooh, eeh: F1..F3 as {log index Q8, gain Q15} */',
           'static const int32_t si_vowel[3][3][2]={' + ','.join(
               '{' + ','.join('{%d,%d}' % fg for fg in v) + '}' for v in vrows) + '};',
           arr('si_exp2', 'int16_t', exp2), arr('si_sq1mg', 'int16_t', sq1mg),
           '/* root comb delay for C3 -24..+24 semitones, samples Q16 */',
           arr('si_root', 'int32_t', root),
           arr('si_sine', 'int16_t', sine), arr('si_quarter', 'int16_t', quarter),
           '#endif']
    (P / 'sister_tables.h').write_text('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
