#!/usr/bin/env python3
"""Fit model parameters to the reference set with CMA-ES (clar_eval score as the objective).

    python3 fit.py spec.json [--gens 40] [--pop 16] [--set quick] [--tag name] [--start best_x.json]

spec.json: {"engine": "proto", "fixed": {k: v}, "free": [[name, lo, hi, "log"|"lin", init], ...]}
Writes fits/<tag>.json (best params) and prints progress.
"""
import argparse, json, os, sys, time
import numpy as np, cma
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def decode(spec, z):
    p = dict(spec.get('fixed', {}))
    for (name, lo, hi, kind, *_), v in zip(spec['free'], z):
        v = min(1.0, max(0.0, v))
        p[name] = lo * (hi / lo) ** v if kind == 'log' else lo + (hi - lo) * v
    return p


def encode(spec, p):
    z = []
    for name, lo, hi, kind, *init in spec['free']:
        v = p.get(name, init[0] if init else (lo + hi) / 2)
        z.append(np.log(v / lo) / np.log(hi / lo) if kind == 'log' else (v - lo) / (hi - lo))
    return np.clip(z, 0, 1)


_G = {}
def _init(spec, setname):
    import clar_eval
    _G['ce'] = clar_eval; clar_eval.CACHE = False; _G['spec'] = spec; _G['notes'] = clar_eval.note_sets()[setname]


def objective(z):
    ce, spec = _G['ce'], _G['spec']
    p = decode(spec, z)
    try:
        eng = ce.make_engine(spec['engine'], {k: f'{v:.6g}' for k, v in p.items()}, spec.get('knobs'))
        rows = ce.evaluate(eng, _G['notes'], threads=1)
        s = ce.summary(rows)
        pen = 0.0
        # out-of-bounds penalty (CMA may step outside [0,1])
        pen += 20 * float(np.sum(np.clip(-np.asarray(z), 0, None) + np.clip(np.asarray(z) - 1, 0, None)))
        v = s['score'] + pen
        return (v if np.isfinite(v) else 1e3), s
    except Exception as e:
        return 1e3, {'err': str(e)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('spec'); ap.add_argument('--gens', type=int, default=40); ap.add_argument('--pop', type=int, default=16)
    ap.add_argument('--set', default='quick'); ap.add_argument('--tag'); ap.add_argument('--start'); ap.add_argument('--sigma', type=float, default=0.25)
    a = ap.parse_args()
    spec = json.load(open(a.spec)); tag = a.tag or os.path.splitext(os.path.basename(a.spec))[0]
    os.makedirs(os.path.join(HERE, 'fits'), exist_ok=True)
    start = {}
    if a.start: start = json.load(open(a.start))['params']
    x0 = encode(spec, start)
    es = cma.CMAEvolutionStrategy(list(x0), a.sigma, {'popsize': a.pop, 'bounds': [0, 1], 'verbose': -9, 'seed': 1})
    best = (1e9, None, None); t0 = time.time()
    with Pool(min(a.pop, 18), initializer=_init, initargs=(spec, a.set)) as pool:
        for gen in range(a.gens):
            zs = es.ask()
            res = pool.map(objective, zs)
            es.tell(zs, [r[0] for r in res])
            i = int(np.argmin([r[0] for r in res]))
            if res[i][0] < best[0]:
                best = (res[i][0], decode(spec, zs[i]), res[i][1])
                json.dump({'score': best[0], 'summary': best[2], 'params': best[1], 'engine': spec['engine']},
                          open(os.path.join(HERE, 'fits', tag + '.json'), 'w'), indent=1)
            s = best[2]
            print(f"gen {gen:3d} {time.time() - t0:6.0f}s  gen-best {res[i][0]:.3f}  best {best[0]:.3f}  "
                  + '  '.join(f"{k} {v:.2f}" for k, v in s.items() if k != 'score'), flush=True)
    print(json.dumps(best[1], indent=1))


if __name__ == '__main__':
    main()
