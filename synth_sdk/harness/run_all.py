#!/usr/bin/env python3
"""Build and run every host test (harness/*_test.c) with sanitizers; exit 1 if any fails; all in parallel.
    python3 harness/run_all.py [name ...]      (names without _test.c; default: all)
A test builds from its own file plus the sources listed in EXTRA; SKIP names tests that need input files or
arguments, with the reason. Run before every release (AGENTS.md)."""
import os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__)); SDK = os.path.dirname(HERE)
EXTRA = {                                   # test -> extra sources (relative to synth_sdk)
    'chord': ['chord/ch_core.c'], 'dddd': ['dddd/dd_core.c'], 'dddd_plugin': ['dddd/dd_core.c'],
    'devupd': ['devupd/du.c'], 'dp_plus': ['drumplus/dp_plus.c', 'drumplus/dp_voice.c'],
    'dp_ui': ['drumplus/dp_plus.c', 'drumplus/dp_voice.c'], 'erase': ['tape/erase.c'],
    'mseq': ['seqdev/ms_core.c'], 'oblique': ['harness/build/oblique_data.c'], 'mx': ['midi/mx.c'], 'ccmap': ['midi/ccmap.c', 'midi/mx.c', 'screens/op1r_settings.c'], 'overlay': ['screens/overlay.c'],
    'rnd': ['rnd/rn_core.c'], 'pclk': ['midi/pclk.c', 'screens/op1r_settings.c'], 'safewrite': ['tape/safewrite.c'], 'steve': ['steve/st_core.c'],
    'usbtrack': ['screens/usbtrack.c'], 'op1p': ['plugload/op1p_load.c'], 'mixdrop': ['tape/mixdrop.c'], 'patchjob': ['patchjob.c'], 'tombola': ['seq/tombola.c'],
}
def _plugins():                                 # op1p_test: every built plugin file (make_plugin.py / make_firmware.py)
    d = os.path.join(SDK, 'build', 'plugins')
    return sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith('.op1p')) if os.path.isdir(d) else []
ARGS = {'op1p': _plugins}
SKIP = {
    'dp_voice': 'needs a sample directory argument', 'sp_voice': 'needs a sample directory and rate',
'vel': 'built per engine (-DENGINE=...)',
    'paulstretch_engine': 'shelved plugin', 'sitting': 'built from synth_sdk with its own include path',
    'sp_plus': 'needs a sample directory argument',
}
if not _plugins(): SKIP['op1p'] = 'no built plugins (build/plugins/*.op1p)'
if not os.path.exists(os.path.join(HERE, 'build', 'oblique_hostw.h')):   # width table made from the stock font (local)
    SKIP['oblique'] = 'needs build/oblique_hostw.h (stock font widths, local only)'

def run(name):
    src = os.path.join(HERE, name + '_test.c')
    exe = os.path.join(HERE, 'build', name + '_test')
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    cmd = ['cc', '-O1', '-DOP1_HOST', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
           '-I', SDK, '-I', HERE, '-I', os.path.join(SDK, 'plugload'), src] + [os.path.join(SDK, s) for s in EXTRA.get(name, [])] + ['-lm', '-o', exe]
    b = subprocess.run(cmd, capture_output=True, text=True)
    if b.returncode: return 'build', b.stderr.strip().splitlines()[-3:]
    args = ARGS.get(name, lambda: [])()
    r = subprocess.run([exe] + args, capture_output=True, text=True, cwd=HERE, timeout=600)
    if r.returncode: return 'run', (r.stdout + r.stderr).strip().splitlines()[-3:]
    return None, None

def main(names):
    names = names or sorted(f[:-7] for f in os.listdir(HERE) if f.endswith('_test.c'))
    bad = 0
    todo = [n for n in names if n not in SKIP]
    with ThreadPoolExecutor(os.cpu_count()) as pool:   # every test builds and runs in parallel (own exe, own outputs)
        res = dict(zip(todo, pool.map(run, todo)))
    for n in names:
        if n in SKIP: print(f'skip  {n}: {SKIP[n]}'); continue
        what, out = res[n]
        print(('ok    ' if not what else f'FAIL  ') + n + ('' if not what else f' ({what})'))
        for l in out or []: print('      ' + l)
        bad += bool(what)
    print(f'{len(names) - bad} ok / {bad} failed' if bad else 'all ok')
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main(sys.argv[1:])
