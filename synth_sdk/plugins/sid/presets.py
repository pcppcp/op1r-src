#!/usr/bin/env python3
"""Eight SID settings-only AIFC presets for kennel's current wave/cutoff controls.
Uses the sibling MBIRA writer for the common OP-1 patch format/range checks.
Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with OP-1 firmware permission.
"""
from pathlib import Path
import importlib.util
import sys
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('kennel_patch_writer', HERE.parent/'mbira/presets.py')
writer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(writer)
# name, wave index, green, white(log cutoff), red, ADSR, FX, octave.
# Centre of a current 512-unit wave band; covers all six modes without legacy encoding.
# Round Bass uses a half-duty pulse trio through the lowpass, minimum resonance
# and stronger native sustain. The former near-solo triangle was a level outlier.
# This is preset voicing only: no SID DSP gain or extra effect is introduced.
PRESETS = [
    ('round bass', 2, 16384, 23000, 0, [64, 5000, 28000, 2500], None, -1),
    ('chip bass', 2, 10000, 25200, 6500, [64, 4500, 19000, 2500], None, -1),
    ('saw lead', 1, 6500, 29200, 5500, [64, 6500, 22000, 3500], None, 0),
    ('pulse keys', 2, 18000, 27800, 3500, [64, 6500, 10000, 4500], None, 0),
    ('soft triangle', 0, 10000, 28000, 2500, [4500, 7000, 22000, 6500],
     ('delay', [7000, 12000, 4000, 5000]), 0),
    ('sync bite', 4, 10923, 30000, 6000, [64, 5500, 12000, 3000], None, 0),
    ('ring bell', 5, 16384, 31000, 3500, [64, 7000, 0, 6000],
     ('spring', [8000, 12000, 3000, 4500]), 0),
    ('noise hats', 3, 0, 31500, 3000, [64, 4500, 0, 1500], None, 0),
]


def meta(p):
    name, mode, green, white, red, adsr, fx, octave = p
    out = writer.meta(name, [mode*512+256, green, white, red], [0, 0, 0], adsr, fx, None, octave)
    out['type'] = 'sid'
    return out


def main():
    if len(sys.argv)>2:
        sys.exit(__doc__)
    out = Path(sys.argv[1]) if len(sys.argv)==2 else HERE/'presets'
    out.mkdir(parents=True, exist_ok=True)
    for p in PRESETS:
        (out/(p[0]+'.aif')).write_bytes(writer.aifc(meta(p)))
    print(f'{len(PRESETS)} SID presets in {out}')


if __name__=='__main__':
    main()
