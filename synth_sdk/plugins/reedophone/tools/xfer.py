"""Scatter (ref - synth) harmonic level differences against frequency: is the mismatch a fixed filter?"""
import sys, numpy as np, matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from clar_eval import *
params = dict(p.split('=') for p in sys.argv[2:]); tag = sys.argv[1]
rows = evaluate(make_engine('proto', params, None), note_sets()['full'])
fig, ax = plt.subplots(1, 3, figsize=(24, 6))
for i, L in enumerate(('f', 'mf', 'p')):
    for r in rows:
        if r['layer'] != L or r['sf'] is None: continue
        k = min(len(r['rf']['harm']), len(r['sf']['harm'])); f = np.arange(1, k + 1) * r['rf']['f0']
        d = r['rf']['harm'][:k] - r['sf']['harm'][:k]; m = (r['rf']['harm'][:k] > -55) | (r['sf']['harm'][:k] > -55)
        ax[i].semilogx(f[m], d[m], '.-', color=plt.cm.viridis((r['midi'] - 50) / 44), alpha=0.6, lw=0.5)
    ax[i].set_title(f'{L}: ref - syn (dB) per harmonic vs Hz, colour = pitch'); ax[i].grid(); ax[i].set_ylim(-40, 30)
plt.tight_layout(); plt.savefig(f'../analysis/xfer_{tag}.png', dpi=50); print('ok')
