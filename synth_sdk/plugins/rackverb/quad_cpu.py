#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""rackverb's worst block per program with bcore's calibrated cache / SDRAM model (tier2.py stall_pct): builds
quad_cpu_guest.c with the whole FX (rackverb.c, rv_mv.c, rv_kern.S, rv_quad.c, rv_quad_kern.S) and runs it on the
cycle-counting bcore with --memmodel (cold-noalloc: both caches flushed every block, the calibrated variant). The FX
scratch sits at the unit's address for the position (master 0x01B0EE48), QUAD's state in the position's fast lease
(op1_fx_fast: heap 0, SDRAM bank 0, 0x00800000 + pos x 224 KiB) or with --no-fast in its big-buffer lease
(op1_fx_big: 0x02C00000 + pos MiB, modelled write-back as R246's CPLB page is: remapped to 0x00C00000), the L1 pool's
stand-in counted as L1 (--no-l1: no pool). Prints per unit and QUAD configuration the worst steady block and the worst
program-change block as the unit's own share (cycles / 5,805 %), and the program behind each.

Banks: the model prices every SDRAM fill alike (calibrated on the heap-0 arena, bank 0); the unit measured a clean
fill in bank 2 (the pool, framebuffer A's bank) at 354 cycles against bank 0's 193 (K1b). --bank2 [K] runs each job a
second time with the pool counted as L1 and also prints the bank-2 view: free + K x (total - free) per program, K =
1.83 by default. An approximation: per program maxima, not per block, and K is K1b's ratio for clean fills.

    python3 synth_sdk/plugins/rackverb/quad_cpu.py [--units 3] [--pos 2] [--step 1] [--steady 300] [--size]
                                                   [--no-l1] [--no-fast] [--bank2 [K]] [--variant cold-noalloc]
                                                   [--json OUT] [--compare OLD]
"""
import argparse
import json
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

P = Path(__file__).resolve().parent
SDK = P.parents[1]
sys.path.insert(0, str(SDK / 'harness'))
import tier2 as T                                     # noqa: E402

FLAGS = ['-O2', '-mcpu=bf524', '-ffreestanding', '-fno-builtin', '-nostdlib', '-fno-common', '-Wall']
SOURCES = ['quad_cpu_guest.c', 'rackverb.c', 'rv_mv.c', 'rv_kern.S', 'rv_quad.c', 'rv_quad_kern.S']
COUNT = {0: 63, 1: 100, 2: 63, 3: 90}
NAMES = {0: 'VERB I', 1: 'VERB II', 2: 'FEX', 3: 'QUAD'}
POOL_RANGE = '02c00000-03000000'                       # kn_mem.h KN_POOL_LO..HI, the guest's op1_fx_big
QCFG = [(1, 0, 29), (2, 30, 41), (3, 42, 53), (4, 54, 65), (5, 66, 89)]   # rv_quad.h's configurations


def build(t, env, tmp, d):
    objs = []
    for src in SOURCES:
        o = tmp / (Path(src).stem + '.o')
        T.sh([t['gcc'], *FLAGS, *d, f'-I{SDK}', f'-I{P}', '-c', str(P / src), '-o', str(o)], env=env)
        objs.append(str(o))
    o = tmp / 'crt.o'
    T.sh([t['as'], '-mcpu=bf524', str(T.HERE / 'tier2/crt.S'), '-o', str(o)])
    libgcc = T.sh([t['gcc'], '-mcpu=bf524', '-print-libgcc-file-name'], env=env).strip()
    elf = tmp / 'qc.elf'
    T.sh([t['ld'], '--no-warn-rwx-segments', '--defsym=T2_STAGE=0x02000000', '-T', str(T.HERE / 'tier2/guest.ld'),
          str(o), *objs, libgcc, '-o', str(elf)])
    return elf


def run_one(found, t, env, a, unit, first, last, extra_free=()):
    with tempfile.TemporaryDirectory(prefix='quad-cpu-') as tmp:
        tmp = Path(tmp)
        d = [f'-DQC_POS={a.pos}', f'-DQC_UNIT={unit}', f'-DQC_FIRST={first}', f'-DQC_LAST={last}',
             f'-DQC_STEP={a.step}', f'-DQC_STEADY={a.steady}', f'-DQC_SIZE={int(a.size)}', f'-DQC_L1={int(not a.no_l1)}',
             f'-DQC_FAST={int(not a.no_fast)}', *a.cflag]
        elf = build(t, env, tmp, d)
        syms = T.nm_sizes(t['nm'], elf)
        l1 = syms['_l1_mem']
        free = list(a.free) + list(extra_free) + [f'{syms[n][0]:08x}-{syms[n][0] + syms[n][1]:08x}' for n in a.free_sym if n in syms]
        args = [f'--memmodel={a.variant}', '--memmodel-flush', f'{syms["_blk_end"][0]:08x}',
                '--memmodel-remap', '03000000-03400000:01000000+07f00000-08000000:01f00000+' + a.pool,
                '--memmodel-l1', f'{l1[0]:08x}-{l1[0] + l1[1]:08x}' + ''.join('+' + f for f in free)]
        r = subprocess.run([found['emu_cycles'], '--cycles', '--fastmem', *args, str(elf)], capture_output=True,
                           text=True, timeout=7200)
    rows = []
    for ln in r.stdout.splitlines():
        p = ln.split()
        if p and p[0] == 'P':
            rows.append({'unit': int(p[1]), 'prog': int(p[2]), 'chg': int(p[3]), 'max': int(p[4]), 'avg': int(p[5]),
                         'hash': int(p[6])})
    if 'END' not in r.stdout:
        raise SystemExit(f'quad_cpu: guest failed: {r.stdout[-300:]} {r.stderr[-500:]}')
    return rows


def pct(c):
    return 100.0 * c / T.UNIT_BLOCK_CYCLES


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--units', default='3', help='comma list: 0 VERB I, 1 VERB II, 2 FEX, 3 QUAD')
    ap.add_argument('--pos', type=int, default=2)
    ap.add_argument('--step', type=int, default=1)
    ap.add_argument('--steady', type=int, default=300)
    ap.add_argument('--size', action='store_true', help='SIZE swinging between its ends')
    ap.add_argument('--no-l1', action='store_true')
    ap.add_argument('--no-fast', action='store_true', help='no fast lease: QUAD\'s state in op1_fx_big (the pool)')
    ap.add_argument('--bank2', nargs='?', type=float, const=1.83, default=None,
                    help='also print the bank-2 view: the pool\'s stalls x K (default 1.83 = 354 / 193, K1b)')
    ap.add_argument('--variant', default=T.MEMMODEL_CALIBRATED)
    ap.add_argument('--pool', default='02c00000-03000000:00c00000', help='remap of the big-buffer pool ("" for none)')
    ap.add_argument('--free', action='append', default=[], help='LO-HI (hex) counted as L1 too: what a range costs')
    ap.add_argument('--free-sym', action='append', default=[], help='a symbol (e.g. _render) counted as L1: its cost')
    ap.add_argument('--cflag', action='append', default=[], help='extra compiler flag (e.g. -DRV_QD_CLEAR=213600)')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--json', type=Path)
    ap.add_argument('--compare', type=Path, help='an earlier --json run (same options): every output hash must match')
    a = ap.parse_args(argv)
    if not a.pool:
        a.pool = '7f000000-7f000001:7f000000'
    found = T.setup_env()
    if 'toolchain' not in found or 'emu_memmodel' not in found:
        print('quad_cpu: SKIP (toolchain or bcore with --memmodel missing)')
        return 0
    t = T.tools(found['toolchain'])
    env = dict(T.os.environ, PATH=t['path'])
    jobs = []
    for u in (int(x) for x in a.units.split(',')):
        if u == 3:
            jobs += [(u, f, l) for _, f, l in QCFG]
        else:
            n = COUNT[u]
            first = 1 if u != 1 else 0
            jobs += [(u, first + k, min(first + k + 15, n - 1)) for k in range(0, n - first, 16)]
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        res = [r for rows in ex.map(lambda j: run_one(found, t, env, a, *j), jobs) for r in rows]
        if a.bank2 is not None:                           # the same jobs with the pool free: its stalls = total - free
            free = {(r['unit'], r['prog']): r for rows in ex.map(lambda j: run_one(found, t, env, a, *j, (POOL_RANGE,)), jobs)
                    for r in rows}
            for r in res:
                f = free[(r['unit'], r['prog'])]
                for k in ('max', 'avg', 'chg'):
                    r[k + '_b2'] = int(f[k] + a.bank2 * (r[k] - f[k]))
    groups = {}
    for r in res:
        key = NAMES[r['unit']] + (f' config {next(c for c, f, l in QCFG if f <= r["prog"] <= l)}' if r['unit'] == 3 else '')
        groups.setdefault(key, []).append(r)
    print(f'rackverb per-block cost, pos {a.pos}, {a.variant}, L1 {"off" if a.no_l1 else "on"}, '
          f'QUAD state {"op1_fx_big (bank 2)" if a.no_fast else "op1_fx_fast (heap 0)"}, SIZE '
          f'{"swinging" if a.size else "still"} (own share of the unit block: cycles / 5,805 %)')
    out = {}
    for k, rows in groups.items():
        w = max(rows, key=lambda r: r['max'])
        c = max(rows, key=lambda r: r['chg'])
        av = max(rows, key=lambda r: r['avg'])
        out[k] = {'worst': w['max'], 'worst_prog': w['prog'], 'chg': c['chg'], 'chg_prog': c['prog'],
                  'avg_max': av['avg'], 'avg_prog': av['prog'], 'rows': rows}
        print(f'  {k:16s} worst steady {pct(w["max"]):5.1f} % ({w["max"]:,} cy, prog {w["prog"]}), '
              f'avg {pct(av["avg"]):5.1f} % (prog {av["prog"]}), change {pct(c["chg"]):5.1f} % (prog {c["prog"]})')
        if a.bank2 is not None:
            w2 = max(rows, key=lambda r: r['max_b2'])
            c2 = max(rows, key=lambda r: r['chg_b2'])
            a2 = max(rows, key=lambda r: r['avg_b2'])
            out[k].update(worst_b2=w2['max_b2'], worst_b2_prog=w2['prog'], chg_b2=c2['chg_b2'], avg_b2=a2['avg_b2'])
            print(f'  {"":16s} bank-2 view  {pct(w2["max_b2"]):5.1f} % (prog {w2["prog"]}), '
                  f'avg {pct(a2["avg_b2"]):5.1f} %, change {pct(c2["chg_b2"]):5.1f} %')
    if a.json:
        a.json.write_text(json.dumps(out, indent=1))
    if a.compare:
        old = {(r['unit'], r['prog']): r['hash'] for g in json.loads(a.compare.read_text()).values() for r in g['rows']}
        bad = [(r['unit'], r['prog']) for r in res if (r['unit'], r['prog']) in old and old[(r['unit'], r['prog'])] != r['hash']]
        same = sum(1 for r in res if (r['unit'], r['prog']) in old) - len(bad)
        print(f'compare: {same} programs the same, {len(bad)} differ' + (f': {bad[:12]}' if bad else ''))
        return 1 if bad else 0
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
