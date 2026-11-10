#!/usr/bin/env python3
"""Generate moai_tables.h: the lookup tables of the moai engine (run: python3 gen_tables.py).

The OP-1 has no FPU and no per-sample division, so everything that needs exp / log / tan is a table:

  MM_G[i]      stage coefficient gg = t / (1 + t), t = tan(theta), of the zero-delay (trapezoidal) ladder, Q15, for a
               cutoff of 20 Hz * 2^(i/16). theta = pi f / fs, so the resonance sits exactly at f up to 8 kHz. Above
               that theta runs on to the design's top (atan(4) at 44.1 kHz, the same frequency at fs: the filter opens
               up at the top of the knob instead of staying a few kHz dark: a 4-pole cascade has its -3 dB point well
               below its cutoff).
  MM_GG_K0/K1/KA/KB  where lad_coef takes the resonance out at the top: the design's gg 18022 .. 26214 at 44.1 kHz
               as gg at fs, and the design's linear fade (in its gg) as a quadratic in K1 - gg (within 1 % of k)
  MM_SAT_LP[i] the saturation's one-pole low-pass for SAT = 4096 i: the design's 1 - exp(-2 pi fc / 44100),
               29400 - 12700 SAT (15.7 .. 5.0 kHz), as the same fc at fs, Q15 (interpolated: within 0.5 %)
  fs is the voice's real rate, 88.2 kHz: R246 renders 128 samples a call at 88.2 kHz (until 2026-11-03 these were made
  at 44.1 kHz, which put the filter an octave high and ran the contour twice as fast).
  MM_LOG2[i]   log2(1 + i/32) in Q10 (pitch -> octaves for the keyboard tracking)
  MM_DEC[i]    envelope step per 32-sample chunk (Q18) for a time constant of 4 ms * 1.0625^i (decay / release)
  MM_INV[i]    1 / (1 + i/16), Q15 (the ladder's feedback solve, no division)
  MM_RCP[i]    2^30 / (32768 + 512 i) (reciprocal of a normalised phase increment for the polyBLEP, no division)
  MM_SEMI[i]   2^(i/12) in Q14 and MM_ISEMI[i] 2^(-i/12) in Q15, i = 0..12 (oscillator intervals)
"""
import math, os

FS = 88200.0                   # the voice's rate on R246 (128 samples a call, 689 calls/s)
FS_DESIGN = 44100.0            # the rate the curve above the knee and the thresholds were designed at
F0 = 20.0                      # cutoff table origin (Hz)
STEPS_PER_OCT = 16
F_MAX = 20480.0                # top of the knob: 10 octaves above F0
F_KNEE = 8000.0                # above this the table opens the filter faster than the true frequency
THETA_TOP = math.atan(4.0)     # at 44.1 kHz gg = 0.8: the 4-pole corner is then above 14 kHz (more would let the
                               # trapezoidal states swing far beyond the signal: the pole of a stage tends to z = -1)
CHUNK = 32                     # envelope update interval (samples), = CHUNK in moai.c
C4_INC = 12740185              # phase increment of 261.63 Hz at 88.2 kHz, 2^32 = one cycle/sample

def theta(f):                   # the design's theta at 44.1 kHz, as the same frequency at FS
    if f <= F_KNEE:
        return math.pi * f / FS
    x = (f - F_KNEE) / (F_MAX - F_KNEE)
    t0 = math.pi * F_KNEE / FS_DESIGN
    return (t0 + (THETA_TOP - t0) * x) * FS_DESIGN / FS

def gg_at_fs(g44):             # a design gg (44.1 kHz, Q15) -> the gg of the same frequency at FS
    return math.tan(math.atan(g44 / (32768 - g44)) * FS_DESIGN / FS)

n = int(round(math.log2(F_MAX / F0) * STEPS_PER_OCT))
g_tab = []
for i in range(n + 2):                                          # one extra entry for the interpolation
    t = math.tan(theta(F0 * 2 ** (i / STEPS_PER_OCT)))
    g_tab.append(min(32767, round(t / (1 + t) * 32768)))
log2_tab = [round(math.log2(1 + i / 32) * 1024) for i in range(33)]
dec_tab = []
for i in range(128):
    tau = 0.004 * 1.0625 ** i                                   # 4 ms .. ~8.5 s
    dec_tab.append(max(1, round((1 - math.exp(-CHUNK / FS / tau)) * 262144)))

kk = [round(t / (1 + t) * 32768) for t in (gg_at_fs(18022), gg_at_fs(26214))]       # lad_coef's resonance fade
def g44_of(g):                 # gg at FS (Q15) -> the design's gg at 44.1 kHz
    t = math.tan(math.atan(g / (32768 - g)) * FS / FS_DESIGN)
    return t / (1 + t) * 32768
pts = [(kk[1] - g, max(0.0, (26214 - g44_of(g)) * 4 - 1)) for g in range(kk[0], kk[1] + 1)]
# least squares f(d) = a d + b d^2 (f(0) = 0: no resonance at the top)
s11 = sum(d * d for d, _ in pts); s12 = sum(d ** 3 for d, _ in pts); s22 = sum(d ** 4 for d, _ in pts)
t1 = sum(d * f for d, f in pts); t2 = sum(d * d * f for d, f in pts)
det = s11 * s22 - s12 * s12
ka, kb = (t1 * s22 - t2 * s12) / det, (s11 * t2 - s12 * t1) / det
KA, KB = round(ka * 256), round(kb * 256 * 65536)                # f = (d (KA + ((d KB) >> 16))) >> 8
kerr = max(abs(((d * (KA + ((d * KB) >> 16))) >> 8) - f) for d, f in pts) / 32768
assert kerr < 0.01, kerr
sat_tab = []
for i in range(9):
    v = min(32767, i * 4096)
    a44 = (29400 - ((v * 12700) >> 15)) / 32768
    sat_tab.append(round((1 - (1 - a44) ** (FS_DESIGN / FS)) * 32768))

inv_tab = [min(32767, round(32768 / (1 + i / 16))) for i in range(16 * 5 + 2)]     # 1 + k G4 up to 6
rcp_tab = [round(2 ** 30 / (32768 + 512 * i)) for i in range(65)]
semi_tab = [round(2 ** (i / 12) * 16384) for i in range(13)]
isemi_tab = [min(32767, round(2 ** (-i / 12) * 32768)) for i in range(13)]

def arr(name, ctype, vals, per=12):
    rows = [', '.join(str(v) for v in vals[i:i + per]) for i in range(0, len(vals), per)]
    return f'static const {ctype} {name}[{len(vals)}] = {{\n    ' + ',\n    '.join(rows) + '\n};\n'

out = ['/* generated by gen_tables.py: do not edit */\n',
       f'#define MM_G_N      {n}      /* cutoff positions: 20 Hz * 2^(i/{STEPS_PER_OCT}), i = 0 .. MM_G_N (MM_G has one more) */\n',
       f'#define MM_C4_LOG2  {round(math.log2(C4_INC) * 1024)}     /* log2(phase increment of C4) in Q10 */\n',
       f'#define MM_GG_K0    {kk[0]}     /* gg where the resonance starts to fade (the design\'s 18022 at 44.1 kHz) */\n',
       f'#define MM_GG_K1    {kk[1]}     /* gg where it is gone (the design\'s 26214) */\n',
       f'#define MM_GG_KA    {KA}      /* the fade: (d (KA + ((d KB) >> 16))) >> 8, d = K1 - gg (within {kerr * 100:.1f} % of k) */\n',
       f'#define MM_GG_KB    {KB}\n',
       arr('MM_G', 'uint16_t', g_tab), arr('MM_LOG2', 'uint16_t', log2_tab), arr('MM_DEC', 'uint16_t', dec_tab),
       arr('MM_INV', 'uint16_t', inv_tab), arr('MM_RCP', 'uint16_t', rcp_tab),
       arr('MM_SEMI', 'uint16_t', semi_tab), arr('MM_ISEMI', 'uint16_t', isemi_tab), arr('MM_SAT_LP', 'uint16_t', sat_tab)]
path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'moai_tables.h')
open(path, 'w').write(''.join(out))
print(f'{path}: {n + 1} cutoff steps, gg {g_tab[0]} .. {g_tab[n]} (Q15)')
