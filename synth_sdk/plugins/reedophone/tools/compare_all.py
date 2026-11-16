#!/usr/bin/env python3
"""One listening file: for a spread of notes, the reference recording then reedophone, licorice, chalumeau
(each 2.6 s, rendered through the fixed-point engines). -> analysis/compare_all.wav"""
import os, numpy as np, soundfile as sf
import clar_eval as ce
from clar_common import load, ref_path, SR
KN = [26214, 16384, 16384, 8192]
ATT = {'reedophone': ['135']}                     # envelope attack (ms) that matches the clarinet; others: default
eng = [ce.Plugin(n, KN, ATT.get(n)) for n in ('reedophone', 'licorice', 'chalumeau')]
key = {n: m for n, m, _ in ce.note_sets()['full']}
notes = [(1, 120), (5, 120), (9, 120), (14, 120), (19, 120), (28, 85), (40, 85), (53, 45), (62, 45), (69, 45)]
out = []; gap = np.zeros(int(0.35 * SR))
for n, vel in notes:
    midi = key[n]
    x = load(ref_path(n))[:int(ce.DUR * SR)]
    seg = [x / (np.abs(x).max() + 1e-9) * 0.5]
    for e in eng:
        y = e.render(midi, vel); seg.append(y / (np.abs(y).max() + 1e-9) * 0.5)
    for s in seg: out += [s, gap]
    out.append(np.zeros(int(0.6 * SR)))
dst = os.path.join(os.path.dirname(ce.HERE), 'analysis', 'compare_all.wav')
sf.write(dst, np.concatenate(out), SR, subtype='PCM_16'); print(dst)
