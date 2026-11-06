#!/usr/bin/env python3
"""Twelve MONOLIT sound presets as OP-1 patch files: python3 synth_sdk/plugins/monolit/presets.py [OUT_DIR]
-> OUT_DIR/*.aif (default: presets/ next to this script; the generated files are kept there for the user to install).
Uses the sibling MBIRA writer for the common OP-1 patch format and range checks; the type name is "monolit".
Copy the presets folder to the OP-1's synth/ folder in disk mode (Shift + COM, T3), e.g. as synth/monolit/.
Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""
from pathlib import Path
import importlib.util
import sys

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('kennel_patch_writer', HERE.parent / 'mbira/presets.py')
writer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(writer)

# page-2 Body: the centre of each of its five bands (osc 3: -1 octave, unison, fifth, +1 octave, -2 octaves)
SUB, UNI, FIFTH, OCT, SUB2 = 3277, 9830, 16384, 22938, 29491

# name: knobs (SHAPE, CUTOFF, CONTOUR, HEAT), page-2 words (BODY, VEL, AGE), ADSR, FX, LFO, octave
PRESETS = [
    ('ladder bass', [8192, 7000, 6000, 12000], [SUB, 12000, 4000], [64, 8000, 26000, 2500], None, None, -1),
    ('pluck bass', [4000, 3000, 9000, 20000], [SUB, 20000, 0], [64, 10000, 18000, 2000], None, None, -1),
    ('sub drop', [8192, 10000, 2000, 4000], [SUB2, 6000, 0], [64, 12000, 30000, 4000], None, None, -1),
    ('ladder ping', [0, 1500, 3000, 31000], [SUB, 16000, 0], [64, 9000, 12000, 2000], None, None, -1),
    ('acid line', [0, 5000, 11000, 28000], [SUB, 24000, 2000], [64, 7000, 20000, 3000],
     ('delay', [5000, 12000, 6000, 8000]), None, 0),
    ('fat lead', [24576, 18000, 4000, 14000], [OCT, 8000, 9000], [64, 9000, 30000, 5000],
     ('delay', [7000, 14000, 6000, 9000]), None, 0),
    ('fifth lead', [24576, 15000, 8000, 18000], [FIFTH, 10000, 6000], [64, 9000, 28000, 5000],
     ('delay', [8000, 15000, 7000, 10000]), None, 0),
    ('bright stab', [24576, 22000, 7000, 8000], [OCT, 16000, 4000], [64, 5000, 16000, 3000],
     ('spring', [12000, 14000, 3000, 8000]), None, 0),
    ('brass swell', [16384, 9000, 24000, 9000], [UNI, 14000, 6000], [1500, 9000, 28000, 7000],
     ('spring', [12000, 14000, 3000, 9000]), None, 0),
    ('worn tape lead', [16384, 13000, 12000, 10000], [UNI, 8000, 30000], [64, 9000, 28000, 6000],
     ('delay', [7000, 12000, 5000, 9000]), ('tremolo', [2600, 2000, 0, 1500]), 0),
    ('wide pad', [32767, 12000, 30000, 6000], [OCT, 6000, 20000], [6000, 12000, 24000, 12000],
     ('spring', [16448, 16000, 3000, 12000]), None, 0),
    ('night drone', [32767, 6000, 32767, 22000], [SUB2, 0, 26000], [9000, 16000, 32767, 16000],
     ('delay', [11200, 20000, 12000, 20000]), None, 0),
]


def meta(p):
    out = writer.meta(*p)
    out['type'] = 'monolit'
    return out


def main():
    if len(sys.argv) > 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1]) if len(sys.argv) == 2 else HERE / 'presets'
    out.mkdir(parents=True, exist_ok=True)
    for p in PRESETS:
        (out / (p[0] + '.aif')).write_bytes(writer.aifc(meta(p)))
    print(f'{len(PRESETS)} MONOLIT presets in {out}')


if __name__ == '__main__':
    main()
