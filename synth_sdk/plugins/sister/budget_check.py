#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
"""SISTER's CPU on bcore: average, init block and worst block per regime, with the predicted unit %.

    python3 synth_sdk/plugins/sister/budget_check.py OUT [--renders N] [--regimes 0,1,..] [--cflags -DX]

Runs the packed plugin in tier 2's guest (Father's budget_check pattern) with its own FX loop: every block's plugin
calls (init included in block 0, as the dispatcher runs init on the first render) are timed with CYCLES on a bcore
build with --cycles (CYCLES counts packets): OP1-Emulator build/bcore-cycles-bin/emu or BCORE_EMU_CYCLES. Without it,
only the average from the plain packet count is reported.

Unit model (docs/cpu-readings-2026-10-27.md): an FX adds 0.478 points per 1,000 packets per block; the base is the
owner's Supersaw 6-voice reading (36 %), and the FX-alone line uses 10.1 + 0.478 / 1,000 packets.
bcore packets omit cache and SDRAM stalls: no hardware acceptance. Not in `make check` (about a minute).
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

P = Path(__file__).resolve().parent
SDK = P.parents[1]
sys.path.insert(0, str(SDK / 'harness'))
import tier2 as T                                     # noqa: E402
sys.path.insert(0, str(SDK.parent / 'tools/kennel'))
import pack                                           # noqa: E402

REGIMES = {
    0: 'defaults, steady noise',
    1: 'all knobs 0',
    2: 'all knobs max',
    3: 'VOICE blend + SWELL max + SPACE max (heaviest steady)',
    4: 'every knob and the key changing every block',
    5: 're-select (init) every 128 blocks, defaults',
    6: 'PIPES, key change every 16 blocks',
    7: 'MIX 0 (dry), defaults otherwise',
    8: 'defaults, noise bursts (8 on / 56 off)',
}

FX_LOOP = r'''        } else if (in.kind == KN_KIND_FX) {
            const op1_fx_stereo_t *f = entry;
            const uint32_t kk = t2_cfg.knobs;
            const int pos = (int)(kk >> 8) & 3, reg = (int)(kk & 255);
            static int16_t right[64];
            uint32_t rng = 0x12345u, blk = 0, b0 = 0, bmax = 0, bat = 0, bsum = 0, bn = 0, t0, hash = 2166136261u;
            #define CYC(v) __asm__ volatile ("%0 = CYCLES;" : "=d"(v))
            for (int i = 0; i < 8; i++) params[i] = i < 4 ? in.defaults[i] : 0;
            params[4] = (int16_t)(OP1_ENGINE_MAGIC + in.legacy_order);
            params[6] = 16384;
            if (reg == 1) for (int i = 0; i < 4; i++) params[i] = 0;
            if (reg == 2) for (int i = 0; i < 4; i++) params[i] = 32767;
            if (reg == 3) { params[0] = 32767; params[1] = 16384; params[2] = 32767; params[3] = 29000; }
            if (reg == 6) params[1] = 32767;
            if (reg == 7) params[3] = 0;
            CYC(t0); f->mono.init(pos); { uint32_t t1; CYC(t1); blk = t1 - t0; }
            for (uint32_t r = 0; stage >= 3 && r < renders; r++) {
                if (reg == 4) { for (int i = 0; i < 4; i++) { rng = rng * 1664525u + 1013904223u; params[i] = (int16_t)(rng >> 17); }
                                rng = rng * 1664525u + 1013904223u; params[6] = (int16_t)(rng >> 17); }
                if (reg == 6 && r % 16 == 0) { rng = rng * 1664525u + 1013904223u; params[6] = (int16_t)(600 + (rng >> 17) % 31000); }
                if (reg == 5 && r && r % 128 == 0) { uint32_t a, b; CYC(a); f->mono.init(pos); CYC(b); blk += b - a; }
                const int on = reg != 8 || (r & 63) < 8;
                for (int i = 0; i < 64; i++) {
                    rng = rng * 1664525u + 1013904223u; out[i] = on ? (int16_t)((int32_t)(rng >> 16) - 32768) / 3 : 0;
                    rng = rng * 1664525u + 1013904223u; right[i] = on ? (int16_t)((int32_t)(rng >> 16) - 32768) / 3 : 0;
                }
                { uint32_t a, b; CYC(a);
                  if (pos == 2) f->render_stereo(pos, out, right, params); else f->mono.render(pos, out, params);
                  CYC(b); blk += b - a; }
                for (int i = 0; i < 64; i++) { hash = (hash ^ (uint16_t)out[i]) * 16777619u; if (pos == 2) hash = (hash ^ (uint16_t)right[i]) * 16777619u; }
                if (r == 0) b0 = blk; else if (blk > bmax) { bmax = blk; bat = r; }
                bsum += blk; bn++; blk = 0;
            }
            put_hex("hash", hash); put_hex("b0", b0);
            #ifdef SI_PROF_GUEST
            { const uint32_t *sc = ((uint32_t **)0x01AB8BA0)[pos]; char k[4] = { 'p', '0', 0, 0 };
              for (int i = 0; i < 9; i++) { k[1] = (char)('0' + i); put_hex(k, sc[i]); } }
            #endif
            put_hex("bmax", bmax); put_hex("bat", bat); put_hex("bsum", bsum); put_hex("bn", bn);
'''


def find_cycles_emu():
    e = os.environ.get('BCORE_EMU_CYCLES') or T.find_up('build/bcore-cycles-bin/emu')
    return str(e) if e and os.access(e, os.X_OK) else None


def run(emu, elf, renders, knobs, cycles):
    off, _ = T.elf_section(elf, '.t2cfg')
    data = bytearray(elf.read_bytes())
    struct.pack_into('<IIIII', data, off, 3, renders, 0, 1, knobs)
    target = elf.with_name(f'b_{renders}_{knobs}.elf')
    target.write_bytes(data)
    r = subprocess.run([emu, '--fastmem', '--count-packets', *(['--cycles'] if cycles else []), str(target)],
                       capture_output=True, text=True, timeout=600)
    line = next((s for s in r.stdout.splitlines() if s.startswith('T2')), '')
    if r.returncode or 'error=' in line or not line:
        raise RuntimeError(f'exit {r.returncode}: {line} {r.stderr[-400:]}')
    res = {'packets': int(re.search(r'packets=(\d+)', r.stderr).group(1))}
    for key in ('hash', 'b0', 'bmax', 'bat', 'bsum', 'bn') + tuple(f'p{i}' for i in range(9)):
        m = re.search(rf' {key}=0x([0-9a-f]+)', line)
        res[key] = int(m.group(1), 16) if m else 0
    return res


def unit(pk, base):
    return round(base + 0.478 * pk / 1000.0, 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('out', type=Path)
    ap.add_argument('--renders', type=int, default=1200)
    ap.add_argument('--regimes', default=','.join(map(str, REGIMES)))
    ap.add_argument('--positions', default='0,2')
    ap.add_argument('--cflags', default='', help='extra flags for every source (-DSI_NO_ASM: the C kernels)')
    ap.add_argument('--source', type=Path, default=P)
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    found = T.setup_env()
    if 'toolchain' not in found:
        raise SystemExit('budget_check: SKIP (Blackfin toolchain missing)')
    emu_c = find_cycles_emu()
    emu = emu_c or found.get('emu')
    if not emu:
        raise SystemExit('budget_check: SKIP (bcore missing)')
    fixture = out / 'fixture'
    (fixture / 'tier2').mkdir(parents=True, exist_ok=True)
    for name in ('guest.c', 'crt.S', 'guest.ld'):
        shutil.copyfile(T.HERE / 'tier2' / name, fixture / 'tier2' / name)
    g = fixture / 'tier2/guest.c'
    s = re.sub(r'#include "\.\./\.\./(kennel/)?', '#include "', g.read_text())   # the fixture builds with -I synth_sdk, -I kennel
    s = s.replace('static char line[512];', 'static char line[1024];')                 # room for the profile words
    start = s.index('        } else if (in.kind == KN_KIND_FX) {')
    end = s.index('        } else {', start)
    g.write_text(('#define SI_PROF_GUEST 1\n' if '-DSI_PROF' in a.cflags else '') + s[:start] + FX_LOOP + s[end:])
    T.HERE, T.OUT = fixture, out / 'build'
    src = out / 'src' / 'sister'
    if src.exists():
        shutil.rmtree(src)
    shutil.copytree(a.source, src, ignore=shutil.ignore_patterns('__pycache__'))
    if a.cflags:
        meta = json.loads((src / 'plugin.json').read_text())
        meta.setdefault('cflags', []).extend(a.cflags.split())
        (src / 'plugin.json').write_text(json.dumps(meta, indent=2))
    knl = out / 'sister.knl'
    pack.pack_dir(src, knl)
    elf, _ = T.build_guest(T.tools(found['toolchain']), 'sister', knl)
    regimes = [int(x) for x in a.regimes.split(',')]
    positions = [int(x) for x in a.positions.split(',')]
    tasks = [(p, r) for p in positions for r in regimes]

    def one(t):
        p, r = t
        return p, r, run(emu, elf, a.renders, p << 8 | r, bool(emu_c))
    rows = []
    with ThreadPoolExecutor(max_workers=int(os.environ.get('TIER2_JOBS', '2'))) as ex:
        for p, r, x in ex.map(one, tasks):
            avg = (x['bsum'] - x['b0']) / max(1, x['bn'] - 1) if emu_c else None
            row = dict(position=p, regime=r, label=REGIMES[r], hash=f"{x['hash']:08x}", avg=round(avg) if avg else None, b0=x['b0'],
                       bmax=x['bmax'], bat=x['bat'],
                       unit_avg=unit(avg, 36.0) if avg else None, unit_worst=unit(max(x['bmax'], x['b0']), 36.0) if emu_c else None,
                       fx_alone_avg=unit(avg, 10.1) if avg else None)
            if '-DSI_PROF' in a.cflags:
                row['stages'] = {n: round(x.get(f'p{i}', 0) / max(1, x['bn'])) for i, n in enumerate(
                    ('params', 'input', 'tank', 'body', 'choir', 'pipes', 'vesper', 'wet', 'out'))}
                print('   stages per block:', row['stages'])
            rows.append(row)
            print(f"pos {p} {row['hash']} {REGIMES[r]:55s} avg {row['avg']:>7,} block0 {row['b0']:>7,} worst {row['bmax']:>7,} (block {row['bat']})"
                  f" -> unit +{0.478 * avg / 1000:.1f} pts avg, {row['unit_avg']} % / worst block {row['unit_worst']} % on Supersaw x6", flush=True)
    report = dict(knl=str(knl), cycles_emu=emu_c, renders=a.renders, cflags=a.cflags, rows=rows,
                  limits='bcore packets exclude cache/SDRAM stalls; the unit model is empirical (cpu-readings-2026-10-27)')
    (out / 'budget.json').write_text(json.dumps(report, indent=1) + '\n')
    worst_avg = max(r['avg'] for r in rows if r['regime'] in (0, 1, 2, 3, 6, 7, 8))
    worst_blk = max(r['bmax'] for r in rows)
    init_blk = max(r['b0'] for r in rows)
    print(f'max steady avg {worst_avg:,} (target 25,000), worst block {worst_blk:,} (target 40,000), init block {init_blk:,}')


if __name__ == '__main__':
    main()
