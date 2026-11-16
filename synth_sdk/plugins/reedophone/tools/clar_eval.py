#!/usr/bin/env python3
"""Score a clarinet synthesis engine against the Crisis clarinet samples.

    python3 clar_eval.py proto [k=v ...] [--set quick|full|f|mf|p] [--tag name] [--plots] [--ab]
    python3 clar_eval.py plugin:reedophone --knobs a,b,c,d [...]

For every reference sample in the set the engine renders the same note (velocity per dynamic layer) and both
are reduced to features:
  harm  : sustained harmonic levels H1..Hk (k*f0 < 10 kHz), dB re. the total harmonic power, floored at -60 dB;
          mean |difference|, weighted towards the stronger harmonics            (timbre, the main term)
  band  : long-term spectrum in 1/3-octave bands 100 Hz..12 kHz incl. noise, dB re. total   (spectral envelope)
  att   : RMS envelope (dB re. sustain) over the first 400 ms after onset, mean |difference| (attack shape)
  hnr   : harmonics-to-noise ratio 0.5..6 kHz, |difference| in dB
  lvl   : sustain level re. the set's mean level, |difference| in dB             (evenness across the range)
  cents : tuning error of the synth vs. the nominal note (reported, not scored)
score = harm + 0.5 band + 0.3 att + 0.1 hnr + 0.2 lvl + tune + lat  (lower is better; dB-like)
  tune  : 0.03 per cent up to 60 cents, +10 beyond (a jump to another mode)
  lat   : time from note-on to -20 dB re. sustain, 1 per 25 ms beyond 80 ms (a soft note must still speak)
"""
import argparse, os, sys, json, subprocess, tempfile, hashlib
import numpy as np, soundfile as sf
from concurrent.futures import ThreadPoolExecutor
from clar_common import *

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), 'analysis')
TMP = os.path.join(tempfile.gettempdir(), 'clar_eval'); os.makedirs(TMP, exist_ok=True)
LAYER_VEL = {'f': 120, 'mf': 85, 'p': 45}
CACHE = True
DUR = 2.6                     # rendered note length (s); release starts at DUR - 0.3
SUS = (0.6, 2.0)              # sustain analysis window (s after onset)


def layer_of(n): return 'f' if n <= 20 else 'mf' if n <= 47 else 'p' if n <= 70 else 's'

def note_sets():
    regs = sfz_regions(); key = {}
    for r in regs: key.setdefault(r['n'], r['key'])
    allf = [(n, key[n], 'f') for n in range(1, 21)]
    allmf = [(n, key[n], 'mf') for n in range(21, 48)]
    allp = [(n, key[n], 'p') for n in range(48, 71)]
    quick = [e for e in allf if e[0] in (1, 3, 5, 7, 8, 9, 11, 14, 16, 18, 19, 20)] + \
            [e for e in allmf if e[0] in (22, 28, 33, 37, 40, 44, 47)] + [e for e in allp if e[0] in (48, 53, 58, 62, 65, 69, 70)]
    return {'quick': quick, 'full': allf + allmf + allp, 'f': allf, 'mf': allmf, 'p': allp}


# ---------------------------------------------------------------- engines
class Proto:
    exe = os.path.join(HERE, 'proto.exe')
    def __init__(self, params, src='proto.c', exe=None):
        self.params = params
        if exe: self.exe = exe
        src = os.path.join(HERE, src)
        if not os.path.exists(self.exe) or os.path.getmtime(self.exe) < os.path.getmtime(src):
            subprocess.run(['gcc', '-O2', '-o', self.exe, src, '-lm'], check=True)
    def render(self, midi, vel, dur=DUR):
        h = hashlib.md5(f'{self.exe}{midi}{vel}{dur}{self.params}{os.path.getmtime(self.exe)}'.encode()).hexdigest()[:16]
        p = os.path.join(TMP, h + '.wav')
        if not os.path.exists(p):
            subprocess.run([self.exe, p, str(midi), str(vel), str(dur), *[f'{k}={v}' for k, v in self.params.items()]], check=True)
        x = sf.read(p)[0]
        if not CACHE: os.remove(p)
        return x


class Plugin:
    """A synth plugin rendered through tools/render_notes.c (fixed-point code as it runs on the OP-1, with the
    firmware's envelope multiply)."""
    def __init__(self, name, knobs, extra=None):
        self.name, self.knobs, self.extra = name, knobs, extra or []
        sdk = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
        d = os.path.join(sdk, 'plugins', name); pj = json.load(open(os.path.join(d, 'plugin.json')))
        if self.knobs is None: self.knobs = pj['defaults']
        self.cflags = os.environ.get('RV_CFLAGS', '').split()   # experiments: RV_CFLAGS='-DX=1' python clar_eval.py ...
        self.exe = os.path.join(TMP, f'render_{name}' + (hashlib.md5(' '.join(self.cflags).encode()).hexdigest()[:8] if self.cflags else '') + '.exe')
        srcs = [os.path.join(d, s) for s in pj['sources']]
        harness = os.path.join(HERE, 'render_notes.c')
        hdrs = [os.path.join(d, f) for f in os.listdir(d) if f.endswith('.h')]
        newest = max(os.path.getmtime(s) for s in srcs + hdrs + [harness])
        if not os.path.exists(self.exe) or os.path.getmtime(self.exe) < newest:
            subprocess.run(['gcc', '-O2', *self.cflags, '-DOP1_HOST', f'-DENGINE={pj["symbol"]}', '-I', sdk, harness, *srcs, '-lm',
                            '-o', self.exe], check=True)
    def render(self, midi, vel, dur=DUR):
        h = hashlib.md5(f'{self.exe}{midi}{vel}{dur}{self.knobs}{self.extra}{os.path.getmtime(self.exe)}'.encode()).hexdigest()[:16]
        p = os.path.join(TMP, h + '.wav')
        if not os.path.exists(p):
            subprocess.run([self.exe, p, str(midi), str(vel), str(dur), *map(str, self.knobs), *self.extra], check=True)
        x = sf.read(p)[0]
        if not CACHE: os.remove(p)
        return x


def make_engine(spec, params, knobs, extra=None):
    if spec == 'proto': return Proto(params)
    if spec.startswith('proto:'): return Proto(params, spec[6:] + '.c', os.path.join(HERE, spec[6:] + '.exe'))
    if spec.startswith('plugin:'): return Plugin(spec[7:], knobs, extra)
    raise SystemExit('engine: proto | proto:<file> | plugin:<name>')


# ---------------------------------------------------------------- features
def onset_of(x):
    e = rms_env(x, 64, 256); ref = np.median(e[len(e)//4: len(e)//2]) if len(e) > 8 else e.max()
    i = np.argmax(e > ref * 10 ** (-30 / 20))
    return i * 64

def features(x, nominal_midi):
    on = onset_of(x)
    seg = x[on + int(SUS[0] * SR): on + int(SUS[1] * SR)]
    if len(seg) < 8192 or np.sqrt(np.mean(seg ** 2)) < 1e-5: return None
    f0 = f0_yin(seg[:4096], hz_of(nominal_midi) * 0.8, hz_of(nominal_midi) * 1.25)
    K = int(10000 // f0)
    h = harmonics(seg, f0, K); hl = 10 ** (h / 20)
    tot = 10 * np.log10(np.nansum(hl ** 2) + 1e-30)
    if not np.isfinite(f0) or f0 <= 0: return None
    hrel = np.maximum(h - tot, -60)
    # 1/3-octave LTAS
    nfft = 8192; win = np.hanning(nfft); S = np.zeros(nfft // 2 + 1); cnt = 0
    for s in range(0, len(seg) - nfft, nfft // 2):
        S += np.abs(np.fft.rfft(seg[s:s + nfft] * win)) ** 2; cnt += 1
    S /= cnt; fr = np.fft.rfftfreq(nfft, 1 / SR)
    edges = 100 * 2 ** (np.arange(0, 22) / 3)          # 100 Hz .. ~12.7 kHz
    band = np.array([S[(fr >= edges[i]) & (fr < edges[i + 1])].sum() + 1e-20 for i in range(len(edges) - 1)])
    band = np.maximum(10 * np.log10(band / band.sum()), -70)
    # HNR 0.5..6 kHz
    ph = (fr / f0) % 1; b = (fr > 500) & (fr < 6000)
    hnr = 10 * np.log10(S[b & ((ph < 0.1) | (ph > 0.9))].mean() / (S[b & (ph > 0.35) & (ph < 0.65)].mean() + 1e-30))
    # attack envelope: dB re. sustain RMS, 5 ms hop, 0..400 ms after onset
    sus = np.sqrt(np.mean(seg ** 2))
    env = rms_env(x[on:on + int(0.45 * SR)], 220, 441)[:80]
    att = np.maximum(20 * np.log10(env / sus + 1e-9), -40)
    t3 = np.argmax(att > -3) * 5.0
    e0 = rms_env(x[:int(1.0 * SR)], 220, 441)                     # from t = 0 (note on), for the latency
    lat = (np.argmax(e0 > sus * 0.1) * 5.0) if np.any(e0 > sus * 0.1) else 1000.0
    return dict(f0=f0, cents=1200 * np.log2(f0 / hz_of(nominal_midi)), harm=hrel, band=band, hnr=hnr, att=att,
                t3=t3, lat=lat, lvl=20 * np.log10(sus), K=K)

_ref_cache = {}
def ref_features(n, midi):
    if n not in _ref_cache: _ref_cache[n] = features(load(ref_path(n)), midi)
    return _ref_cache[n]

def harm_dist(r, s):
    k = min(len(r), len(s)); r, s = r[:k], s[:k]
    w = np.clip((np.maximum(r, s) + 60) / 60, 0.05, 1)
    return float(np.sum(w * np.abs(r - s)) / np.sum(w))

def compare(rf, sf_):
    if sf_ is None: return dict(harm=60, band=60, att=40, hnr=30, cents=200, score=200)
    d = dict(harm=harm_dist(rf['harm'], sf_['harm']), band=float(np.mean(np.abs(rf['band'] - sf_['band']))),
             att=float(np.mean(np.abs(rf['att'] - sf_['att']))), hnr=abs(rf['hnr'] - sf_['hnr']), cents=sf_['cents'])
    return d


def evaluate(engine, notes, vel_map=LAYER_VEL, threads=16):
    def one(e):
        n, midi, layer = e
        x = engine.render(midi, vel_map[layer])
        return n, midi, layer, x
    with ThreadPoolExecutor(threads) as ex: rendered = list(ex.map(one, notes))
    rows = []
    for n, midi, layer, x in rendered:
        rf = ref_features(n, midi); sfeat = features(x, midi)
        d = compare(rf, sfeat); d.update(n=n, midi=midi, layer=layer, rf=rf, sf=sfeat, x=x)
        rows.append(d)
    # level evenness within each layer: synth and ref levels re. their own layer means
    for layer in ('f', 'mf', 'p'):
        rs = [r for r in rows if r['layer'] == layer and r['sf'] is not None]
        if not rs: continue
        mr = np.mean([r['rf']['lvl'] for r in rs]); ms = np.mean([r['sf']['lvl'] for r in rs])
        for r in rs: r['lvl'] = abs((r['sf']['lvl'] - ms) - (r['rf']['lvl'] - mr))
    for r in rows:
        r.setdefault('lvl', 20)
        c = abs(r['cents'])
        r['tune'] = 0.03 * min(c, 60) + (10 if c > 60 else 0)       # small: calibrated later; a mode hop is fatal
        r['lat'] = max(0.0, (r['sf']['lat'] if r['sf'] else 1000) - 80) / 25    # note-on to -20 dB: 1 per 25 ms over 80 ms
        r['score'] = r['harm'] + 0.5 * r['band'] + 0.3 * r['att'] + 0.1 * r['hnr'] + 0.2 * r['lvl'] + r['tune'] + r['lat']
    return rows


def summary(rows, label=''):
    keys = ('score', 'harm', 'band', 'att', 'hnr', 'lvl', 'lat')
    tot = {k: float(np.mean([r[k] for r in rows])) for k in keys}
    tot['|cents|'] = float(np.mean([abs(r['cents']) for r in rows]))
    return tot


def print_rows(rows):
    print(' n  midi lay  score  harm  band   att  hnr   lvl  cents  t3(ref/syn)')
    for r in rows:
        t3 = f"{r['rf']['t3']:4.0f}/{r['sf']['t3']:4.0f}  lat {r['sf']['lat']:4.0f}" if r['sf'] else '-'
        print(f"{r['n']:2d} {r['midi']:4d}  {r['layer']:>2s} {r['score']:6.2f} {r['harm']:5.2f} {r['band']:5.2f} {r['att']:5.2f} "
              f"{r['hnr']:4.1f} {r['lvl']:5.2f} {r['cents']:6.1f}  {t3}")


def plots(rows, path):
    import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
    rs = [r for r in rows if r['sf'] is not None]
    nr = (len(rs) + 1) // 2
    fig, ax = plt.subplots(nr, 6, figsize=(26, 2.4 * nr), gridspec_kw=dict(width_ratios=[3, 2, 2, 3, 2, 2]))
    ax = np.atleast_2d(ax)
    ed = 100 * 2 ** (np.arange(0, 21) / 3 + 1 / 6)
    for i, r in enumerate(rs):
        c = (i % 2) * 3; a = ax[i // 2, c]; k = min(len(r['rf']['harm']), len(r['sf']['harm']), 30)
        a.bar(np.arange(1, k + 1) - 0.2, r['rf']['harm'][:k] + 60, 0.4, bottom=-60, label='ref')
        a.bar(np.arange(1, k + 1) + 0.2, r['sf']['harm'][:k] + 60, 0.4, bottom=-60, label='syn')
        a.set_ylim(-60, 0); a.set_title(f"#{r['n']} midi {r['midi']} {r['layer']}  harm {r['harm']:.1f}  {r['cents']:+.0f}c", fontsize=9)
        if i == 0: a.legend(fontsize=7)
        a = ax[i // 2, c + 1]
        a.plot(np.arange(len(r['rf']['att'])) * 5, r['rf']['att']); a.plot(np.arange(len(r['sf']['att'])) * 5, r['sf']['att'])
        a.set_title(f"attack dB/ms  {r['att']:.1f}", fontsize=9); a.set_ylim(-40, 5)
        a = ax[i // 2, c + 2]; a.semilogx(ed, r['rf']['band']); a.semilogx(ed, r['sf']['band'])
        a.set_title(f"1/3 oct  {r['band']:.1f}  hnr {r['rf']['hnr']:.0f}/{r['sf']['hnr']:.0f}", fontsize=9); a.set_ylim(-70, 0)
    plt.tight_layout(); plt.savefig(path, dpi=45); plt.close()


def ab_wav(rows, path, seg=1.6):
    out = []
    for r in rows:
        ref = load(ref_path(r['n'])); on = onset_of(ref); ref = ref[on:on + int(seg * SR)]
        s = r['x']; on2 = onset_of(s); s = s[on2:on2 + int(seg * SR)]
        g = np.sqrt(np.mean(ref[int(0.5*SR):] ** 2)) / (np.sqrt(np.mean(s[int(0.5*SR):] ** 2)) + 1e-9)
        fade = np.minimum(1, np.minimum(np.arange(len(ref)), len(ref) - np.arange(len(ref))) / 400)
        out += [ref * fade, np.zeros(int(0.15 * SR))]
        fade = np.minimum(1, np.minimum(np.arange(len(s)), len(s) - np.arange(len(s))) / 400)
        out += [s * g * fade, np.zeros(int(0.5 * SR))]
    y = np.concatenate(out); y *= 0.8 / max(1e-9, np.abs(y).max())
    sf.write(path, y.astype(np.float32), SR, subtype='PCM_16')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('engine'); ap.add_argument('params', nargs='*')
    ap.add_argument('--set', default='quick'); ap.add_argument('--tag'); ap.add_argument('--knobs')
    ap.add_argument('--plots', action='store_true'); ap.add_argument('--ab', action='store_true')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--att', type=float, help='stock envelope attack, ms (render_notes default 5; reedophone matches the clarinet at 135)')
    a = ap.parse_args()
    params = dict(p.split('=') for p in a.params)
    knobs = [int(v) for v in a.knobs.split(',')] if a.knobs else None
    eng = make_engine(a.engine, params, knobs, [str(a.att)] if a.att else None)
    rows = evaluate(eng, note_sets()[a.set])
    if not a.quiet: print_rows(rows)
    s = summary(rows)
    for L in ('f', 'mf', 'p'):
        rl = [r for r in rows if r['layer'] == L]
        if rl: print(f"  {L:>2s}: " + '  '.join(f"{k} {v:.2f}" for k, v in summary(rl).items()))
    print('TOTAL: ' + '  '.join(f"{k} {v:.2f}" for k, v in s.items()))
    tag = a.tag or a.engine.replace(':', '_')
    if a.plots: plots(rows, os.path.join(OUT, f'eval_{tag}.png')); print('plots ->', os.path.join(OUT, f'eval_{tag}.png'))
    if a.ab: ab_wav(rows, os.path.join(OUT, f'ab_{tag}.wav')); print('A/B ->', os.path.join(OUT, f'ab_{tag}.wav'))


if __name__ == '__main__':
    main()
