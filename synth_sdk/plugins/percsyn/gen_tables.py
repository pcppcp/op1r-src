#!/usr/bin/env python3
"""Generate perc_tables.h: integer math tables, constants and the factory sounds.

    python3 plugins/percsyn/gen_tables.py   (from synth_sdk; rewrites plugins/percsyn/perc_tables.h)

The factory sounds are written in musical units (Hz, ms, semitones, dB) and stored as PO-32 patch words
(21 x 0..65535), the same format a transfer brings in, through the parameter maps documented in libpo32's
PATCH_PARAMETERS.md (MIT, Eric Lewis). The sounds themselves are our own.
"""
import math, os

HERE = os.path.dirname(os.path.abspath(__file__))
FS = 44100

def q(x, bits): return int(round(x * (1 << bits)))

# ---- tables ----
exp2 = [q(2 ** (i / 256), 30) for i in range(257)]                 # 2^(i/256), Q30
log2 = [q(math.log2(1 + i / 256), 16) for i in range(257)]         # log2(1 + i/256), Q16
qsin = [q(math.sin(math.pi / 2 * i / 256), 30) for i in range(257)]  # sin over a quarter turn, Q30
# strong distortion as a function of xd = x * drive (odd, flat above 4/3): 0.5 (f(xd - 1/3) - f(-xd - 1/3)),
# f(c) = c (|c| - c^2 + 1) on c clamped to [-1, 1]; 512 steps over 0 .. 4/3, Q24
def _f(c):
    c = max(-1.0, min(1.0, c)); return c * (abs(c) - c * c + 1)
dist = [q(0.5 * (_f(i / 384 - 1 / 3) - _f(-i / 384 - 1 / 3)), 24) for i in range(514)]

consts = {
    'L2_20':     q(math.log2(20), 16),
    'L2_1000':   q(math.log2(1000), 16),
    'L2_FS':     q(math.log2(FS), 16),
    'L2_0P01':   q(math.log2(0.01), 16),
    'L2_L2_1000': q(math.log2(math.log2(1000)), 16),   # log2 of log2(1000)
    'L2_10':     q(math.log2(10), 16),
    'L2_PI':     q(math.log2(math.pi), 16),
    'L2_2000':   q(math.log2(2000), 16),
    'L2_20000':  q(math.log2(20000), 16),
    'L2_0P0398': q(math.log2(0.03981071706), 16),
    'L2_10000':  q(math.log2(10000), 16),
    'L2_LN2INV': q(math.log2(1 / math.log(2)), 16),     # log2(1/ln 2)
    'L2_2PI':    q(math.log2(2 * math.pi), 16),
    'LN2_Q32':   q(math.log(2), 32),
    'VEL_K':     q(37 / 20 * math.log2(10), 16),        # velocity curve: 10^(37(x-1)/20) = 2^((x-1) VEL_K)
    'MIX_K':     q(25 / 20 * math.log2(10), 16),        # mix tilt 25 dB
    'DB_K':      q(math.log2(10) / 20, 16),             # dB -> log2 gain
    'MAX_HZ_L2': q(math.log2(0.45 * FS), 16),           # filter frequency cap 0.45 fs
    'MODK_Q32':  q(math.log2(1000) / FS, 32),
    'L2_2_3':    q(math.log2(2 / 3), 16),
    'L2_1P5MS':  q(math.log2(1.5e-3), 16),
    'REF_INC_L2': q(math.log2(261.6255653 * 2 ** 32 / 88200), 16),   # C4 at the voice rate: native pitch
}

# ---- inverse maps (musical units -> normalised 0..1) ----
def freq(hz): return math.log(hz / 20) / math.log(1000)
def dcy(ms): return (math.log10(ms / 1000) + 2) / 3
def atk(ms):
    if ms <= 0: return 0.0
    lo, hi = 0.0, 1.0
    for _ in range(60):
        m = (lo + hi) / 2
        if m * 0.03981071706 * 10 ** (2.4 * m) * 1000 < ms: lo = m
        else: hi = m
    return (lo + hi) / 2
def mod_decay_rate(ms): return min(1.0, 1.03 / (1 + 0.003 * ms))
def mod_hz(hz, sine):
    lo, hi = 0.0, 1.0
    for _ in range(60):
        m = (lo + hi) / 2
        if (2000 if sine else 20000) * m * 10 ** ((3.5 if sine else 4.0) * (m - 1)) < hz: lo = m
        else: hi = m
    return (lo + hi) / 2
def mod_amt(semis, decay=True):
    lim = 96 if decay else 48
    mag = min(1.0, abs(semis) / lim) ** 0.5
    return 0.5 + (0.5 if semis >= 0 else -0.5) * mag
def fq(qv): return math.log10(qv / 0.1) / 5
def level(db):
    lo, hi = 1e-4, 1.0
    for _ in range(60):
        m = (lo + hi) / 2
        if 60 * m - 49 - 1 / m < db: lo = m
        else: hi = m
    return (lo + hi) / 2
def eq_gain(db): return (db + 40) / 80

WAVE = {'sine': 0.0, 'tri': 0.5, 'saw': 1.0}
MOD = {'decay': 0.0, 'sine': 0.5, 'noise': 1.0}
FILT = {'lp': 0.0, 'bp': 0.5, 'hp': 1.0}
NENV = {'exp': 0.0, 'lin': 0.5, 'mod': 1.0}

def patch(wave='sine', hz=100, atk_ms=0, dcy_ms=200, mod='decay', mod_rate=None, semis=0, filt='lp', nhz=5000,
          nq=0.7, nenv='exp', natk_ms=0, ndcy_ms=50, mix=0.5, dist=0.0, eq_hz=1000, eq_db=0, level_db=0,
          vel=(0.5, 0.5, 0.5)):
    if mod == 'decay': rate = mod_decay_rate(mod_rate if mod_rate is not None else 50)
    else: rate = mod_hz(mod_rate if mod_rate is not None else 10, mod == 'sine')
    n_atk = atk(natk_ms * (1.5 if nenv == 'lin' else 1))
    n_dcy = dcy(ndcy_ms * (1.5 if nenv == 'lin' else 1))
    v = [WAVE[wave], freq(hz), atk(atk_ms), dcy(dcy_ms), MOD[mod], rate, mod_amt(semis, mod == 'decay'),
         FILT[filt], freq(nhz), fq(nq), NENV[nenv], n_atk, n_dcy, mix, dist, freq(eq_hz), eq_gain(eq_db),
         level(level_db), *vel]
    return [max(0, min(65535, int(round(x * 65535)))) for x in v]

# Factory sounds (Shift + blue, in this order). Starting points written for perc; tweak by ear.
FACTORY = [
    ('kick',    patch('sine', 52, 0, 420, 'decay', 45, 30, 'lp', 2500, 0.7, 'exp', 0, 12, 0.18, 0.18, 90, 0, 0)),
    ('snare',   patch('tri', 185, 0, 110, 'decay', 30, 10, 'bp', 3200, 0.8, 'exp', 0, 190, 0.62, 0.0, 4000, 0, -1)),
    ('hi-hat',  patch('saw', 6200, 0, 20, 'sine', 900, 6, 'hp', 8000, 1.1, 'exp', 0, 50, 0.92, 0.0, 10000, 4, -2)),
    ('clap',    patch('sine', 900, 0, 15, 'decay', 20, 0, 'bp', 1150, 1.8, 'mod', 0, 85, 0.96, 0.0, 1600, 0, 0)),
    ('perc',    patch('sine', 235, 0, 160, 'decay', 18, 9, 'bp', 1800, 1.4, 'exp', 0, 22, 0.25, 0.0, 500, 0, -2)),
    ('tom',     patch('sine', 105, 0, 320, 'decay', 110, 9, 'lp', 900, 0.7, 'exp', 0, 40, 0.2, 0.0, 200, 0, -1)),
    ('cymbal',  patch('saw', 3300, 0, 900, 'sine', 1300, 14, 'hp', 6500, 0.6, 'exp', 0, 1300, 0.85, 0.0, 9000, 6, -4)),
    ('rimshot', patch('tri', 1700, 0, 28, 'decay', 8, 5, 'bp', 2600, 2.0, 'exp', 0, 32, 0.45, 0.3, 2400, 0, -2)),
    ('cowbell', patch('saw', 587, 0, 280, 'sine', 270, 4, 'bp', 2600, 3.0, 'exp', 0, 40, 0.12, 0.0, 850, 8, -6)),
    ('shaker',  patch('sine', 100, 0, 10, 'decay', 50, 0, 'hp', 5200, 0.9, 'lin', 28, 85, 1.0, 0.0, 9000, 0, -2)),
]

# Their right sides (Shift + orange morphs a key from the left to the right side, as a PO-32): a related sound.
FACTORY_R = [
    ('boom',    patch('sine', 44, 0, 900, 'decay', 60, 40, 'lp', 1200, 0.7, 'exp', 0, 8, 0.08, 0.28, 70, 3, 0)),
    ('crack',   patch('tri', 230, 0, 70, 'decay', 20, 14, 'bp', 5200, 0.9, 'exp', 0, 120, 0.8, 0.1, 6000, 3, -1)),
    ('open',    patch('saw', 6200, 0, 120, 'sine', 900, 6, 'hp', 7500, 1.0, 'exp', 0, 380, 0.92, 0.0, 10000, 4, -3)),
    ('hall',    patch('sine', 900, 0, 15, 'decay', 20, 0, 'bp', 900, 1.4, 'mod', 0, 320, 0.97, 0.0, 1300, 0, -1)),
    ('block',   patch('sine', 520, 0, 60, 'decay', 10, 5, 'bp', 2600, 1.6, 'exp', 0, 12, 0.12, 0.0, 900, 2, -2)),
    ('rack',    patch('sine', 165, 0, 220, 'decay', 80, 12, 'lp', 1200, 0.7, 'exp', 0, 30, 0.18, 0.0, 300, 0, -1)),
    ('ride',    patch('saw', 4200, 0, 650, 'sine', 900, 8, 'bp', 7000, 1.2, 'exp', 0, 700, 0.6, 0.0, 5000, 5, -4)),
    ('clave',   patch('sine', 2500, 0, 22, 'decay', 5, 3, 'bp', 3000, 2.0, 'exp', 0, 10, 0.12, 0.0, 2500, 0, -3)),
    ('agogo',   patch('tri', 410, 0, 180, 'sine', 190, 3, 'bp', 1800, 3.0, 'exp', 0, 30, 0.1, 0.0, 600, 6, -5)),
    ('rain',    patch('sine', 100, 0, 10, 'decay', 50, 0, 'bp', 3500, 0.8, 'lin', 45, 160, 1.0, 0.0, 6000, 0, -2)),
]
assert len(FACTORY_R) == len(FACTORY)

# The drum kit's library (percdrum): 24 presets in key order, each a factory sound or a variation of one
# (name, factory index, semitones, decay time factor, bend factor), both sides. Baked into patch words.
KIT = [
    ('kick', 0, 0, 1, 1), ('kick lo', 0, -5, 1.78, 1.41), ('snare', 1, 0, 1, 1), ('rimshot', 7, 0, 1, 1),
    ('clap', 3, 0, 1, 1), ('hi-hat', 2, 0, 1, 1), ('hat op', 2, 0, 4.2, 1), ('tom lo', 5, -5, 1, 1),
    ('tom', 5, 0, 1, 1), ('tom hi', 5, 5, 1, 1), ('cymbal', 6, 0, 1, 1), ('cowbell', 8, 0, 1, 1),
    ('shaker', 9, 0, 1, 1), ('perc lo', 4, -5, 1, 1), ('perc', 4, 0, 1, 1), ('perc hi', 4, 7, 1, 1),
    ('kick 2', 0, 2, 0.56, 2.0), ('snare 2', 1, 2, 0.56, 1), ('clap 2', 3, 0, 2.37, 1), ('hat 2', 2, 3, 0.42, 1),
    ('cym 2', 6, 0, 0.42, 1), ('bell hi', 8, 7, 1, 1), ('rim hi', 7, 5, 1, 1), ('shake 2', 9, 0, 2.37, 1),
]
def vary(p, semis, tf, bend):
    p = list(p)
    cl = lambda v: max(0, min(65535, int(round(v))))
    p[1] = cl(p[1] + semis / 12 * 65535 / math.log2(1000))
    for i in (3, 12): p[i] = cl(p[i] + math.log10(tf) / 3 * 65535)
    p[6] = cl(32768 + (p[6] - 32768) * math.sqrt(bend))
    return p

def emit():
    o = ['/* generated by gen_tables.py: do not edit */', '#ifndef PERC_TABLES_H', '#define PERC_TABLES_H',
         '#include <stdint.h>']
    def arr(name, t, typ='uint32_t'):
        o.append(f'static const {typ} {name}[{len(t)}] = {{')
        for i in range(0, len(t), 8): o.append('    ' + ', '.join(f'{x}' for x in t[i:i + 8]) + ',')
        o.append('};')
    arr('PERC_EXP2', exp2); arr('PERC_LOG2', log2, 'int32_t'); arr('PERC_QSIN', qsin); arr('PERC_DIST', dist, 'int32_t')
    for k, val in consts.items(): o.append(f'#define PERC_{k} ({val})')
    o.append(f'#define PERC_FACTORY_COUNT {len(FACTORY)}')
    o.append('static const char PERC_FACTORY_NAME[PERC_FACTORY_COUNT][8] = {' +
             ', '.join(f'"{n}"' for n, _ in FACTORY) + '};')
    o.append('static const uint16_t PERC_FACTORY[PERC_FACTORY_COUNT][21] = {')
    for n, p in FACTORY: o.append(f'    {{{", ".join(map(str, p))}}},   /* {n} */')
    o.append('};')
    o.append('static const uint16_t PERC_FACTORY_R[PERC_FACTORY_COUNT][21] = {   /* their right sides (morph) */')
    for n, p in FACTORY_R: o.append(f'    {{{", ".join(map(str, p))}}},   /* {n} */')
    o.append('};')
    o.append(f'#define PERC_KIT_COUNT {len(KIT)}')
    o.append('static const char PERC_KIT_NAME[PERC_KIT_COUNT][8] = {' + ', '.join(f'"{k[0]}"' for k in KIT) + '};')
    for nm, src in (('PERC_KIT', FACTORY), ('PERC_KIT_R', FACTORY_R)):
        o.append(f'static const uint16_t {nm}[PERC_KIT_COUNT][21] = {{')
        for n, f, st, tf, b in KIT: o.append(f'    {{{", ".join(map(str, vary(src[f][1], st, tf, b)))}}},   /* {n} */')
        o.append('};')
    o.append('#endif')
    open(os.path.join(HERE, 'perc_tables.h'), 'w').write('\n'.join(o) + '\n')

if __name__ == '__main__':
    emit()
    print('wrote perc_tables.h')
