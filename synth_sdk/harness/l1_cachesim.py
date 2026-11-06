#!/usr/bin/env python3
"""Rank plugins by data-cache misses (docs/67): every plugin's host audio phrase (audio_harness.c) runs under valgrind
cachegrind with the OP-1's data cache (16 KB, 4-way, 32 B lines: bank A only, docs/19). Prints, per plugin, the plugin
code's data-cache misses per output sample and the source lines with the most misses: the per-sample data that would
gain from L1. All plugins in parallel.

    python3 harness/l1_cachesim.py [plugin ...]          (default: every synth / FX plugin)

Relative numbers only: x86-64 code (other spills, 8-byte pointers), one voice, and none of the firmware's own cache
traffic (stock voices, mixing, display), which on the device evicts more. valgrind: `brew install valgrind`.
"""
import json, os, re, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__)); SDK = os.path.dirname(HERE)
CACHE = ['--D1=16384,4,32', '--I1=16384,4,32', '--LL=8388608,16,64']


def plugins():
    out = {}
    for f in sorted(os.listdir(os.path.join(SDK, 'plugins'))):
        j = os.path.join(SDK, 'plugins', f, 'plugin.json')
        if os.path.isfile(j):
            p = json.load(open(j))
            if p['kind'] in ('synth', 'fx'): out[p['name']] = (os.path.dirname(j), p)
    return out


def run(name, d, p):
    w = tempfile.mkdtemp(prefix=f'l1cs_{name}_')
    exe = os.path.join(w, 'a')
    kind = f'-DENGINE={p["symbol"]}' if p['kind'] == 'synth' else f'-DFX={p["symbol"]}'
    srcs = [os.path.join(d, s) for s in p['sources'] if s.endswith('.c')]   # .S: Blackfin only (host builds take the C path)
    b = subprocess.run(['cc', '-O2', '-g', '-w', '-DOP1_HOST', kind, '-I', SDK, os.path.join(HERE, 'audio_harness.c'),
                        *srcs, '-lm', '-o', exe], capture_output=True, text=True)
    if b.returncode: return name, None, b.stderr.strip().splitlines()[-1:]
    knobs = [str(k) for k in p['defaults']]
    cg = os.path.join(w, 'cg.out')
    r = subprocess.run(['valgrind', '--tool=cachegrind', '--cache-sim=yes', *CACHE, f'--cachegrind-out-file={cg}', exe,
                        *knobs], capture_output=True)
    if not os.path.exists(cg): return name, None, r.stderr.decode().strip().splitlines()[-2:]
    samples = len(r.stdout) // 2
    # cachegrind out: "fl=" file, "fn=" function, then "line Ir I1mr ILmr Dr D1mr DLmr Dw D1mw DLmw" per line
    ev, fl, per_line, tot = None, None, {}, [0] * 9
    for l in open(cg):
        if l.startswith('events:'): ev = l.split()[1:]
        elif l.startswith('fl='): fl = l[3:].strip()
        elif l[:1].isdigit() and fl and (os.path.realpath(fl).startswith(os.path.realpath(d)) or fl.endswith('op1_plugin.h')):
            v = list(map(int, l.split())); ln, c = v[0], dict(zip(ev, v[1:] + [0] * (len(ev) - len(v) + 1)))
            m = c.get('D1mr', 0) + c.get('D1mw', 0)
            k = (os.path.basename(fl), ln)
            a = per_line.setdefault(k, [0, 0]); a[0] += m; a[1] += c.get('Dr', 0) + c.get('Dw', 0)
            tot[0] += c.get('Ir', 0); tot[1] += c.get('Dr', 0) + c.get('Dw', 0); tot[2] += m
    top = sorted(per_line.items(), key=lambda x: -x[1][0])[:4]
    lines = []
    for (f, ln), (m, acc) in top:
        if not m: break
        try: src = open(os.path.join(SDK if f == 'op1_plugin.h' else d, f)).read().splitlines()[ln - 1].strip()[:90]
        except (OSError, IndexError): src = '?'
        lines.append(f'{f}:{ln} {m / samples:.2f}/smp ({100 * m / max(acc, 1):.0f}% of its {acc / samples:.1f} accesses)  {src}')
    return name, (p['kind'], tot[0] / samples, tot[1] / samples, tot[2] / samples), lines


def main(names):
    ps = plugins()
    names = names or list(ps)
    with ThreadPoolExecutor(os.cpu_count()) as pool:
        res = list(pool.map(lambda n: run(n, *ps[n]), names))
    res.sort(key=lambda r: -(r[1][3] if r[1] else -1))
    print(f'{"plugin":12} kind   instr/smp  data/smp  D1 miss/smp   (OP-1-like 16 KB 4-way data cache, plugin code only)')
    for n, s, lines in res:
        if not s: print(f'{n:12} FAIL  ' + ' '.join(lines)); continue
        print(f'{n:12} {s[0]:5} {s[1]:9.1f} {s[2]:9.1f} {s[3]:11.3f}')
        for l in lines: print('    ' + l)


if __name__ == '__main__':
    main(sys.argv[1:])
