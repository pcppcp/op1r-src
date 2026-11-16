import sys, numpy as np
from clar_eval import *
import json; base = {k: str(v) for k, v in json.load(open('fits/v1.json'))['params'].items()}
key = sys.argv[1]; vals = [float(v) for v in sys.argv[2].split(',')]; midis = [int(m) for m in sys.argv[3].split(',')]
extra = dict(p.split('=') for p in sys.argv[4:])
for m in midis:
    for v in vals:
        p = dict(base); p.update(extra); p[key] = v
        if key == 'gamma': p['gamma_f'] = p['gamma_p'] = v; del p['gamma']
        x = Proto(p).render(m, 127)
        f = features(x, m)
        if f is None: print(f"midi {m} {key}={v}: silent"); continue
        h = f['harm']; print(f"midi {m} {key}={v:.3f}: lvl {f['lvl']:6.1f} t3 {f['t3']:4.0f} cents {f['cents']:6.1f}  H1..6 " + ' '.join(f'{x - h[0]:5.0f}' for x in h[:6]))
