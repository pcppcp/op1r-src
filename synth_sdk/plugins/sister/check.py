#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
"""SISTER's host checks (source-only: the DSP alone at 44.1 kHz, not the R246 chain):

  test      sister_test.c under ASan/UBSan (MIX 0 exactly dry, silence, tails end in silence, every corner on
            full-scale input, per-position state, cached knob conversion)
  clicks    every knob jumped 0 <-> max and swept fast, the PIPES key stepped and jumped, on a held chord and on
            drums: no click (mbira_check.py's detector: a jump of the ~1.6 kHz low-passed signal far above its
            neighbourhood) beyond what renders with still knobs show (on drums the hits themselves count, and a
            reversed hit's landing at the end of its swell is as sharp as the hit)
  pipes     noise through VOICE 99: the strongest partials sit on the root's and the fifth's harmonics (C3, and G3
            for key +7), within 0.6 %
  vesper    an impulse at SWELL 99: the reversed tail rises to its window's end
  levels    full wet loudness across VOICE and SWELL stays within 9 dB per source; wet peaks below full scale;
            stereo wet decorrelated

    python3 synth_sdk/plugins/sister/check.py [OUT]      (default build/sister-check; WAVs and result.json there)
"""
import json
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np

P = Path(__file__).resolve().parent
SDK = P.parents[1]
FS = 44100
out = Path(sys.argv[1]) if len(sys.argv) > 1 else SDK / 'harness/build/sister-check'
out.mkdir(parents=True, exist_ok=True)
fails = []
report = {'scope': 'source-only DSP at 44.1 kHz; not the R246 decimator, envelope, LFO or mixer'}


def check(ok, what):
    print(('PASS ' if ok else 'FAIL ') + what)
    if not ok:
        fails.append(what)


def q(v):
    return round(v * 32767 / 99)


def material():
    rng = np.random.default_rng(7)
    t = np.arange(int(6 * FS)) / FS
    pad = np.zeros_like(t)
    for f in (130.81, 164.81, 196.0, 246.94):
        for k in range(1, 9):                            # soft partials: a pad, not a saw's per-period edge
            pad += (np.sin(2 * np.pi * f * k * t * 1.002) + np.sin(2 * np.pi * f * k * t * 0.998)) / (k * k)
    env = np.minimum(1, t / 0.3)
    pad *= env * 0.08
    drums = np.zeros_like(t)
    for i in range(int(6 / 0.125)):
        p = int(i * 0.125 * FS)
        n = min(FS // 2, len(t) - p)
        tt = np.arange(n) / FS
        if i % 8 in (0, 3):
            drums[p:p + n] += np.sin(2 * np.pi * np.cumsum(50 + 90 * np.exp(-tt * 30)) / FS) * np.exp(-tt * 7) * 0.6
        if i % 8 == 4:
            drums[p:p + n] += rng.standard_normal(n) * np.exp(-tt * 18) * 0.35
        if i % 2 == 0:
            drums[p:p + n] += np.diff(np.r_[0, rng.standard_normal(n)]) * np.exp(-tt * 60) * 0.15
    noise = np.zeros_like(t)
    noise[:int(0.3 * FS)] = rng.standard_normal(int(0.3 * FS)) * 0.3
    imp = np.zeros(int(5 * FS))
    imp[int(0.2 * FS)] = 0.9
    return {'pad': pad, 'drums': drums, 'noise': noise, 'impulse': imp}


def put(name, x):
    p = tmp / (name + '.raw')
    np.round(np.stack([x, x], 1) * 32767).clip(-32768, 32767).astype('<i2').tofile(p)
    return p


def render(src, k, key=0, auto=None, mono=0):
    o = tmp / 'o.raw'
    args = [str(exe), str(src), str(o), *map(str, k), str(key), str(mono)]
    if auto:
        a = tmp / 'auto.txt'
        a.write_text(''.join(f'{b} {kn} {v}\n' for b, kn, v in auto))
        args.append(str(a))
    subprocess.run(args, check=True)
    return np.fromfile(o, '<i2').reshape(-1, 2).astype(float)


def wav(name, x):
    with wave.open(str(out / (name + '.wav')), 'wb') as w:
        w.setparams((2, 2, FS, 0, 'NONE', 'not compressed'))
        w.writeframes(np.round(x).astype('<i2').tobytes())


def clicks(x):
    """mbira_check.py's click detector on one channel: a sample-to-sample jump of the signal low-passed at
    ~1.6 kHz far above its 256-sample neighbourhood"""
    y = np.zeros(len(x))
    acc = 0.0
    for i in range(len(x)):
        acc += 0.25 * (x[i] - acc)
        y[i] = acc
    d2 = np.abs(y[2:] - 2 * y[1:-1] + y[:-2])
    hits = []
    for s0 in range(FS // 10, len(d2) - 256, 256):
        seg = d2[s0:s0 + 256]
        j = int(np.argmax(seg))
        if seg[j] > 8 * (np.median(seg) + 0.5 * np.mean(seg) + 2) and seg[j] > 200:
            hits.append(s0 + j + 2)
    return hits


with tempfile.TemporaryDirectory(prefix='sister-check-') as d:
    tmp = Path(d)
    t = tmp / 'sister_test'
    subprocess.run(['cc', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=undefined', f'-I{SDK}', f'-I{P}', str(P / 'sister_test.c'), str(P / 'sister_dsp.c'),
                    str(P / 'sister_kern.c'), '-o', str(t)], check=True)
    r = subprocess.run([str(t)], capture_output=True, text=True)
    print(r.stdout, end='')
    check(r.returncode == 0 and 'SISTER host test PASS' in r.stdout, 'sister_test (sanitizers)')
    exe = tmp / 'render'
    subprocess.run(['cc', '-O2', '-Wall', f'-I{SDK}', f'-I{P}', str(P / 'sister_render.c'), str(P / 'sister_dsp.c'),
                    str(P / 'sister_kern.c'), '-o', str(exe)], check=True)
    mats = {k: put(k, v) for k, v in material().items()}
    D = [q(60), q(20), 0, q(35)]                         # plugin.json defaults

    # ---- clicks ----
    blocks = int(6 * FS) // 64
    cl = {}
    for src in ('pad', 'drums'):
        base = {}
        for label, k in (('defaults', D), ('choir wet', [q(60), 0, 0, q(70)]), ('pipes wet', [q(60), q(99), 0, q(70)]),
                         ('vesper wet', [q(60), q(20), q(99), q(70)]), ('all wet', [q(60), q(20), 0, q(99)]),
                         ('all wet, small', [0, q(20), 0, q(99)])):
            x = render(mats[src], k)
            base[label] = max(len(clicks(x[:, 0])), len(clicks(x[:, 1])))
        allowed = max(base.values())
        for knob, name in enumerate(('SPACE', 'VOICE', 'SWELL', 'MIX')):
            jumps = []
            for i, b in enumerate(range(400, blocks - 300, 260)):
                jumps.append((b, knob, 32767 if i % 2 == 0 else 0))
            sweep = [(b, knob, int(32767 * abs(((b - 400) % 300) / 150.0 - 1))) for b in range(400, blocks - 200, 2)]
            for how, auto in (('jumps 0<->max', jumps), ('fast sweeps (0.4 s per pass)', sweep)):
                k = list(D)
                if knob != 3:
                    k[3] = q(70)
                x = render(mats[src], k, auto=auto)
                h = [len(clicks(x[:, 0])), len(clicks(x[:, 1]))]
                cl[f'{src} {name} {how}'] = h
                check(max(h) <= allowed, f'clicks, {src}, {name} {how}: {max(h)} (still knobs: {allowed})')
                if src == 'pad' and how.startswith('fast'):
                    wav(f'sweep-{name.lower()}', x)
        keyj = [(b, 4, (100 * ((b // 97) % 25 - 12))) for b in range(400, blocks - 200, 97)]
        x = render(mats[src], [q(60), q(99), 0, q(70)], auto=keyj)
        h = max(len(clicks(x[:, 0])), len(clicks(x[:, 1])))
        cl[f'{src} key steps'] = h
        check(h <= allowed, f'clicks, {src}, PIPES key stepped and jumped -12..+12: {h} (still knobs: {allowed})')
    report['clicks'] = cl

    # ---- pipes tuning ----
    for key, root, fifth in ((0, 130.81, 196.0), (700, 196.0, 293.66), (-1200, 65.41, 98.0)):
        x = render(mats['noise'], [q(30), q(99), 0, q(99)], key=key)[:, 0] / 32768
        seg = x[int(0.6 * FS):int(4.5 * FS)]
        sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
        f = np.fft.rfftfreq(len(seg), 1 / FS)
        sel = (f > 60) & (f < 1500)
        ff, ss = f[sel], sp[sel]
        picks = []
        for i in np.argsort(ss)[::-1]:
            if all(abs(ff[i] - p) > 5 for p in picks):
                picks.append(ff[i])
            if len(picks) >= 8:
                break
        harm = sorted({root * k for k in range(1, 25)} | {fifth * k for k in range(1, 17)})
        dev = [min(abs(p / h - 1) for h in harm) for p in picks]
        report.setdefault('pipes', {})[str(key)] = {'peaks_hz': [round(p, 1) for p in sorted(picks)], 'max_dev': max(dev)}
        check(max(dev) < 0.006, f'pipes key {key / 100:+.0f} st: 8 strongest partials on root/fifth harmonics '
              f'(worst {100 * max(dev):.2f} %): {", ".join("%.1f" % p for p in sorted(picks))}')

    # ---- vesper: a reversed tail rises ----
    x = render(mats['impulse'], [q(40), q(20), q(99), q(99)])
    m = x.mean(1) / 32768
    env = np.array([np.sqrt(np.mean(m[i:i + 1024] ** 2)) for i in range(0, len(m) - 1024, 1024)])
    i = int(np.argmax(env))
    rise = 20 * np.log10(env[i] / max(env[max(0, i - 12)], 1e-9))
    report['vesper_rise_db_over_0.28s'] = rise
    check(rise > 6, f'VESPER: the reversed tail rises {rise:.1f} dB over the 0.28 s before its peak')
    wav('vesper-impulse', x)

    # ---- levels ----
    lv = {}
    for src in ('pad', 'drums'):
        d = np.fromfile(mats[src], '<i2').reshape(-1, 2).astype(float)
        ref = 20 * np.log10(np.sqrt(np.mean(d[2 * FS:6 * FS] ** 2)))
        vals = []
        for v in (0, 25, 50, 75, 99):
            for sw in (0, 50, 99):
                x = render(mats[src], [q(60), q(v), q(sw), q(99)])
                vals.append(20 * np.log10(np.sqrt(np.mean(x[2 * FS:6 * FS] ** 2))) - ref)   # after VESPER's first window
                check(np.max(np.abs(x)) < 32000, f'{src} VOICE {v} SWELL {sw}: wet peak {np.max(np.abs(x)) / 32768:.2f} FS')
        lv[src] = [round(v, 1) for v in vals]
        check(max(vals) - min(vals) < 9, f'{src}: wet loudness over VOICE x SWELL spans {max(vals) - min(vals):.1f} dB')
    report['levels_wet_minus_dry_db'] = lv
    x = render(mats['pad'], [q(60), q(20), 0, q(99)])[FS:4 * FS]
    c = float(np.corrcoef(x[:, 0], x[:, 1])[0, 1])
    report['stereo_correlation'] = c
    check(abs(c) < 0.9, f'stereo wet correlation {c:.2f}')
    wav('pad-defaults', render(mats['pad'], D))

report['status'] = 'FAIL' if fails else 'PASS'
report['fails'] = fails
(out / 'result.json').write_text(json.dumps(report, indent=1) + '\n')
print(f'sister check: {"FAIL " + str(len(fails)) if fails else "PASS"} (source-only; {out})')
sys.exit(1 if fails else 0)
