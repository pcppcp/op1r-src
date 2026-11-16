"""Floor for the scores: compare each reference note with its nearest neighbour (same layer, other note),
transposed features (harmonic numbers line up, bands shift by < 2 semitones)."""
import numpy as np
from clar_eval import *
sets = note_sets(); full = sets['full']
feats = {n: ref_features(n, m) for n, m, L in full}
rows = []
for n, m, L in sets['quick']:
    cands = [(abs(m2 - m), n2, m2) for n2, m2, L2 in full if L2 == L and n2 != n]
    dm, n2, m2 = min(cands)
    d = compare(feats[n], feats[n2]); d['lvl'] = 0
    d['score'] = d['harm'] + 0.5 * d['band'] + 0.3 * d['att'] + 0.1 * d['hnr']
    rows.append(d); print(f"{n:2d} vs {n2:2d} (dm {dm})  score {d['score']:.2f} harm {d['harm']:.2f} band {d['band']:.2f} att {d['att']:.2f} hnr {d['hnr']:.1f}")
print('MEAN', {k: round(float(np.mean([r[k] for r in rows])), 2) for k in ('score', 'harm', 'band', 'att', 'hnr')})
