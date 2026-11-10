"""Levels of typical settings: peak / rms in dBFS (reference: supersaw phrase peak -6.0, rms -10.5 dBFS)."""
import numpy as np
from stats import render
def db(v): return 20 * np.log10(v / 32768 + 1e-12)
W = lambda n: n * 1024 + 512                           # setup n (0-based) on the blue home knob
# (label, midi, setup, cutoff, emphasis, contour knob, detune, drive, drift[, session overrides])
S = [('default C3',            48, W(0), 15000, 9000, 12000, 0, 0, 0),
     ('open, saw C2',          36, W(0), 32767, 0, 0, 0, 0, 0),
     ('open, saw C3',          48, W(0), 32767, 0, 0, 0, 0, 0),
     ('open, saw C5',          72, W(0), 32767, 0, 0, 0, 0, 0),
     ('open, clarinet C3',     48, W(15), 32767, 0, 0, 0, 0, 0),
     ('open, oboe C3',         48, W(16), 32767, 0, 0, 0, 0, 0),
     ('open, flute C3',        48, W(14), 32767, 0, 0, 0, 0, 0),
     ('cut 40% saw C3',        48, W(0), 13000, 0, 0, 0, 0, 0),
     ('cut 40% emp 90%',       48, W(0), 13000, 29500, 0, 0, 0, 0),
     ('cut 40% emp 100%',      48, W(0), 13000, 32767, 0, 0, 0, 0),
     ('fat bass pluck',        36, W(1), 8000, 12000, 9000, 8000, 0, 0),
     ('fat bass, drive 4x',    36, W(1), 8000, 12000, 9000, 8000, 32767, 0),
     ('fat bass, feedback 80%', 36, W(1), 8000, 12000, 9000, 8000, 0, 0, '20:32767'),
     ('minor chord',           60, W(27), 20000, 6000, 30000, 4000, 0, 0),
     ('breath',                60, W(29), 20000, 6000, 30000, 4000, 0, 0),
     ('wind (noise only)',     60, W(31), 20000, 20000, 30000, 0, 0, 0)]
for s in S:
    x = render([s[1], *s[2:6], *s[6:9], 1.0, 0.8, 127, 0] + ([s[9]] if len(s) > 9 else []))
    x = x[int(0.05 * 44100):]
    print(f'{s[0]:24s} peak {db(np.abs(x).max()):6.1f}  rms {db(np.sqrt((x**2).mean())):6.1f}  dc {x.mean():7.0f}')
