#!/usr/bin/env python3
"""Writes rv_programs.h: one row per program of VERB I (63), VERB II (00..99) and FEX (63), read from the program names.

The names are the units' program list (short factual labels; source: thement/midiverb_emulator names-*.h, GPL-3.0,
see PROVENANCE.md). Each name already says what the program is ("1.0 SEC MEDIUM WARM", "ECHO MED HPF AMBI"), so the
row comes from the name; rv_mv.c's generator turns a row into our own 128-step program. No ROM data.
Run: python3 gen_programs.py   (writes rv_programs.h next to this script)
Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE)."""
import math
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent

VERB1 = """.2 SEC SMALL BRIGHT|.2 SEC SMALL WARM|.2 SEC MEDIUM BRIGHT|.3 SEC SMALL BRIGHT|.3 SEC SMALL WARM|.4 SEC MEDIUM BRIGHT
|.4 SEC MEDIUM WARM|.6 SEC SMALL BRIGHT|.6 SEC MEDIUM WARM|.6 SEC MEDIUM DARK|.8 SEC SMALL BRIGHT|.8 SEC LARGE WARM
|1.0 SEC SMALL WARM|1.0 SEC MEDIUM WARM|1.0 SEC LARGE BRIGHT|1.2 SEC MEDIUM WARM|1.2 SEC SMALL WARM|1.2 SEC SMALL BRIGHT
|1.4 SEC LARGE WARM|1.4 SEC LARGE DARK|1.4 SEC MEDIUM WARM|1.6 SEC SMALL DARK|1.6 SEC LARGE BRIGHT|1.6 SEC MEDIUM BRIGHT
|1.8 SEC LARGE DARK|1.8 SEC LARGE BRIGHT|1.8 SEC MEDIUM WARM|2.0 SEC LARGE BRIGHT|2.0 SEC MEDIUM WARM|2.0 SEC LARGE WARM
|2.5 SEC MEDIUM WARM|2.5 SEC LARGE BRIGHT|2.5 SEC MEDIUM DARK|2.8 SEC SMALL BRIGHT|2.8 SEC MEDIUM BRIGHT|3.0 SEC LARGE BRIGHT
|3.0 SEC LARGE WARM|3.0 SEC MEDIUM DARK|3.5 SEC LARGE BRIGHT|3.5 SEC MEDIUM WARM|4.0 SEC LARGE DARK|4.0 SEC MEDIUM BRIGHT
|5 SEC LARGE WARM|8 SEC LARGE BRIGHT|8 SEC LARGE WARM|10 SEC LARGE BRIGHT|10 SEC LARGE WARM|16 SEC LARGE DARK
|18 SEC XLARGE BRIGHT|20 SEC XLARGE DARK|GATED 100 MS|GATED 150 MS|GATED 200 MS|GATED 250 MS|GATED 300 MS
|GATED 350 MS|GATED 400 MS|GATED 500 MS|GATED 600 MS|REVERSE 300 MS|REVERSE 400 MS|REVERSE 500 MS|REVERSE 600 MS"""

VERB2 = """DEFEAT|SMALL BRIGHT .1 SEC|SMALL BRIGHT .2 SEC|SMALL BRIGHT .3 SEC|MEDIUM WARM 1.1 SEC|MEDIUM BRIGHT .6 SEC
|LARGE BRIGHT 1.2 SEC|LARGE DARK 1.0 SEC|MEDIUM DARK .6 SEC|MEDIUM DARK .5 SEC|MEDIUM BRIGHT .6 SEC|MEDIUM BRIGHT .8 SEC
|LARGE WARM 1.0 SEC|LARGE WARM 1.1 SEC|MEDIUM DARK 1.0 SEC|MEDIUM BRIGHT 1.1 SEC|MEDIUM BRIGHT 1.15 SEC
|LARGE BRIGHT 1.6 SEC|LARGE DARK 1.7 SEC|MEDIUM BRIGHT 1.65 SEC|MEDIUM BRIGHT 1.9 SEC|LARGE WARM 2.2 SEC
|LARGE WARM 1.75 SEC|LARGE BRIGHT 1.45 SEC|LARGE DARK 2.2 SEC|LARGE WARM 2.3 SEC|LARGE BRIGHT 2.4 SEC
|LARGE BRIGHT 2.5 SEC|XLARGE WARM 5.0 SEC|XLARGE WARM 15.0 SEC|SLOW GATE 100 MS|SLOW GATE 200 MS|SLOW GATE 250 MS
|SLOW GATE 400 MS|SLOW GATE 450 MS|FAST GATE 150 MS|FAST GATE 75 MS|FAST GATE 200 MS|FAST GATE 100 MS|FAST GATE 175 MS
|REVERSE 400 MS|REVERSE 450 MS|REVERSE 250 MS|REVERSE 200 MS|REVERSE 150 MS|BLOOM 1 8 SEC|REVERSE 500 MS
|REVERSE REGEN 2 SEC|REVERSE REGEN 8 SEC|BLOOM 2 7 SEC|TRIGGERED FLANGE|FLANGE PAN 1|FLANGE PAN 2|TRIGGERED FLANGE 2
|FLANGE PAN 3|FLANGE PAN 4|FLANGE PAN 5|TRIGGERED FLANGE 3|FLANGE PAN 6|FLANGE PAN 7|LIGHT CHORUS 1|LIGHT CHORUS 2
|MEDIUM CHORUS 1|MEDIUM CHORUS 2|MEDIUM CHORUS 3|DEEP CHORUS 1|DEEP CHORUS 2|DEEP CHORUS 3|FAST CHORUS 1|FAST CHORUS 2
|35 MS|55 MS|75 MS|115 MS|140 MS|155 MS|160 MS|170 MS|175 MS|180 MS|185 MS|195 MS|205 MS|210 MS|220 MS|240 MS|250 MS
|275 MS|375 MS|460 MS|2 TAP AMBIENT|3 TAP PAN|MULTITAP|MULTITAP REVERSE PAN|THICKENER/FROZEN FLANGE|STEREO GENERATION
|STEREO GEN WIDE|REGEN DELAY 2 SEC|REGEN DELAY 3 SEC|REGEN DELAY 4 SEC"""

FEX = """ECHO LONG FLAT AMBI|ECHO LONG FLAT THICK|ECHO LONG HPF|ECHO LONG HPF WIDE|ECHO LONG BPF AMBI|ECHO LONG LPF WIDE
|ECHO MED FLAT AMBI|ECHO MED FLAT WIDE|ECHO MED HPF AMBI|ECHO MED BPF AMBI|ECHO MED LPF AMBI|ECHO MED LPF WIDE
|ECHO MED FLAT THICK|ECHO SHORT FLAT|ECHO SHORT LPF AMBI|ECHO SHORT HPF AMBI|ECHO SHORT BPF AMBI|ECHO SHORT FLAT
|ECHO SHORT FLAT WIDE|ECHO XSHORT FLAT|ECHO XSHORT BPF WIDE|2TAP MED FLAT AMBI|2TAP MED HPF AMBI|2TAP MED BPF AMBI
|2TAP MED FLAT THICK|2TAP SHORT FLAT WIDE|2TAP SHORT HPF PAN|2TAP SHORT BPF AMBI|2TAP SHORT LPF AMBI
|2TAP XSHORT FLAT WIDE|3TAP MED FLAT PAN|3TAP SHORT FLAT PAN|3TAP SHORT LPF AMBI|3TAP SHORT BPF AMBI
|3TAP SHORT HPF AMBI|3TAP XSHORT FLAT AMBI|REGEN MED FLAT|REGEN MED HPF AMBI|REGEN MED BPF AMBI|REGEN MED LPF AMBI
|REGEN SHORT FLAT|REGEN XSHORT FLAT|SLAP1|SLAP2|SLAP3|SLAP4|SLAP5|REVERB SHORT GATE|REVERB MEDIUM WARM
|REVERB MEDIUM BLOOM|REVERB MEDIUM PAN|REVERB LONG HPF|REVERB REVERSE|REVERB REVERSE REGEN|MULTITAP PAN
|MULTITAP REVERB|MULTITAP REVERSE PAN|THICKENER|THICKENER DENSE|STEREOGEN AMBIENT|STEREOGEN THICK|STEREOGEN WIDE
|STEREOGEN XWIDE"""

SHAPES = 'DEFEAT REV GATE REVERSE ECHO TAPS MOD THICK SGEN BLOOM'.split()
FILT = {'FLAT': 0, 'HPF': 1, 'BPF': 2, 'LPF': 3}
ST = {'AMBI': 0, 'WIDE': 1, 'THICK': 2, 'PAN': 3}
SIZE = {'SMALL': 0, 'MEDIUM': 1, 'LARGE': 2, 'XLARGE': 3, 'EX.': 3}
TONE = {'BRIGHT': 0, 'WARM': 1, 'DARK': 2}
PRE = [0, 5, 15, 25]


def row(shape, a=0, b=0, c=0, t=0, u=0, v=0, name=''):
    assert len(name) <= 23 and re.fullmatch(r'[A-Z0-9 ./%+#:-]*', name), name
    return dict(shape=shape, a=a, b=b, c=c, t=t, u=u, v=v, name=name)


def regen_for(rt_s, rep_ms):          # the per-repeat gain in 1/16 for a regenerating line's RT60
    g = 10 ** (-3 * rep_ms / 1000 / rt_s)
    return max(1, min(15, round(16 * g)))


def reverb_row(name):
    sec = float(re.search(r'(\d*\.?\d+) SEC', name).group(1))
    size = next(v for k, v in SIZE.items() if k in name.split())
    tone = next(v for k, v in TONE.items() if k in name.split())
    return row('REV', size, tone, 1, round(sec * 100), PRE[size], name=name)


def verb1(i, name):
    if name.startswith('GATED'):
        return row('GATE', t=int(name.split()[1]), name=name)
    if name.startswith('REVERSE'):
        return row('REVERSE', c=2, t=int(name.split()[1]), name=name)
    return reverb_row(name)


FLANGE = {  # program: (rate mHz, base 0.01 ms, depth 0.01 ms, feedback -8..7, triggered)
    50: (900, 60, 55, 5, 1), 51: (150, 90, 80, 4, 0), 52: (250, 70, 60, -4, 0), 53: (1400, 50, 45, 6, 1),
    54: (80, 110, 100, 5, 0), 55: (400, 60, 50, 3, 0), 56: (600, 80, 70, -5, 0), 57: (2000, 40, 35, -6, 1),
    58: (120, 140, 120, 6, 0), 59: (1000, 70, 60, 2, 0)}
CHORUS = {  # program: (rate mHz, base 0.01 ms, depth 0.01 ms)
    60: (600, 1200, 80), 61: (900, 1500, 100), 62: (700, 1600, 160), 63: (450, 1900, 200), 64: (1100, 1400, 150),
    65: (500, 2000, 300), 66: (350, 2400, 380), 67: (800, 1800, 320), 68: (2800, 1200, 120), 69: (4200, 1000, 90)}


def verb2(i, name):
    if i == 0:
        return row('DEFEAT', name=name)
    if i <= 29:
        return reverb_row(name)
    if i <= 39:
        return row('GATE', c=1 if name.startswith('SLOW') else 0, t=int(name.split()[2]), name=name)
    if i <= 49:
        if name.startswith('BLOOM'):
            sec = int(name.split()[2])
            return row('BLOOM', c=3, t=450, u=sec * 100, name=name)
        if 'REGEN' in name:
            sec = int(name.split()[2])
            return row('REVERSE', b=regen_for(sec, 480), c=2, t=480, name=name)
        return row('REVERSE', c=2, t=int(name.split()[1]), name=name)
    if i <= 59:
        r, base, depth, fb, trig = FLANGE[i]
        return row('MOD', 2, fb & 15, trig, base, depth, r, name=name)
    if i <= 69:
        r, base, depth = CHORUS[i]
        return row('MOD', 2, 0, 0, base, depth, r, name=name)
    if i <= 89:
        ms = int(name.split()[0])
        return row('ECHO', 1, 0, ST['WIDE'] if ms < 150 else ST['AMBI'], ms, 3, name=name)
    efx = {
        90: row('TAPS', 2, 0, ST['AMBI'], 300, 2, name=name),
        91: row('TAPS', 3, 0, ST['PAN'], 360, 2, name=name),
        92: row('TAPS', 8, 0, ST['AMBI'], 480, 3, name=name),
        93: row('REVERSE', 0, 0, 3, 480, name=name),
        94: row('MOD', 2, 6, 0, 60, 0, 0, name=name),
        95: row('SGEN', t=12, name=name),
        96: row('SGEN', t=22, name=name),
    }
    if i in efx:
        return efx[i]
    sec = int(name.split()[2])
    return row('ECHO', 1, 3, ST['PAN'], 460, regen_for(sec, 460), name=name)


FEX_T = {'LONG': 600, 'MED': 350, 'SHORT': 180, 'XSHORT': 80}


def fex(i, name):
    w = name.split()
    if w[0] in ('ECHO', '2TAP', '3TAP', 'REGEN'):
        t = FEX_T[w[1]]
        filt = FILT.get(w[2], 0) if len(w) > 2 else 0
        st = ST.get(w[3], ST['AMBI']) if len(w) > 3 else ST['AMBI']
        taps = {'ECHO': 1, 'REGEN': 1, '2TAP': 2, '3TAP': 3}[w[0]]
        regen = 9 if w[0] == 'REGEN' else 0
        return row('TAPS' if taps > 1 else 'ECHO', taps, filt, st, t, regen, name=name)
    if w[0].startswith('SLAP'):
        n = int(w[0][4:])
        return row('ECHO', 1, 0, ST['WIDE'], 40 + 20 * n, 0, name=name)
    special = {
        'REVERB SHORT GATE': row('GATE', t=250, name=name),
        'REVERB MEDIUM WARM': row('REV', 1, 1, 1, 140, 5, name=name),
        'REVERB MEDIUM BLOOM': row('BLOOM', 0, 0, 3, 400, 250, name=name),
        'REVERB MEDIUM PAN': row('REV', 1, 0, 1, 120, 5, name=name),
        'REVERB LONG HPF': row('REV', 2, 0, 1, 300, 15, name=name),
        'REVERB REVERSE': row('REVERSE', 0, 0, 2, 400, name=name),
        'REVERB REVERSE REGEN': row('REVERSE', 0, regen_for(8, 450), 2, 450, name=name),
        'MULTITAP PAN': row('TAPS', 8, 0, ST['PAN'], 480, 2, name=name),
        'MULTITAP REVERB': row('TAPS', 8, 3, ST['AMBI'], 420, 8, name=name),
        'MULTITAP REVERSE PAN': row('REVERSE', 0, 0, 3, 480, name=name),
        'THICKENER': row('MOD', 2, 0, 0, 1200, 30, 400, name=name),
        'THICKENER DENSE': row('MOD', 2, 4, 0, 2000, 50, 300, name=name),
        'STEREOGEN AMBIENT': row('SGEN', t=8, name=name),
        'STEREOGEN THICK': row('SGEN', t=14, name=name),
        'STEREOGEN WIDE': row('SGEN', t=22, name=name),
        'STEREOGEN XWIDE': row('SGEN', t=32, name=name),
    }
    return special[name]


def names(s):
    return [n.strip() for n in s.replace('\n', '').split('|')]


def c_rows(rows):
    out = []
    for r in rows:
        out.append('    { RV_%s, %d, %d, %d, %d, %d, %d, "%s" },' % (r['shape'], r['a'], r['b'], r['c'], r['t'], r['u'], r['v'], r['name']))
    return '\n'.join(out)


def main():
    v1 = [verb1(i, n) for i, n in enumerate(names(VERB1))]
    v2 = [verb2(i, n) for i, n in enumerate(names(VERB2))]
    fx = [fex(i, n) for i, n in enumerate(names(FEX))]
    assert len(v1) == 63 and len(v2) == 100 and len(fx) == 63, (len(v1), len(v2), len(fx))
    h = f'''/* Generated by gen_programs.py: do not edit. VERB I 1..63 (rv_verb1[0..62]), VERB II 00..99, FEX 1..63.
 * Rows from the units' program names (see gen_programs.py); the programs themselves are rv_mv.c's.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef RV_PROGRAMS_H
#define RV_PROGRAMS_H
#include "rv_mv.h"
static const rv_row_t rv_verb1[63] = {{
{c_rows(v1)}
}};
static const rv_row_t rv_verb2[100] = {{
{c_rows(v2)}
}};
static const rv_row_t rv_fex[63] = {{
{c_rows(fx)}
}};
#endif
'''
    (HERE / 'rv_programs.h').write_text(h)
    print('rows: VERB I %d, VERB II %d, FEX %d' % (len(v1), len(v2), len(fx)))


if __name__ == '__main__':
    main()
