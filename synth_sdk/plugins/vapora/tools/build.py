#!/usr/bin/env python3
"""Regenerate and validate VAPORA, then build with op1r's standard packer."""
from pathlib import Path
import argparse
import hashlib
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT.parents[1]
sys.path.insert(0, str(Path(__file__).parent))
import gen_tables, gen_art, gen_presets, check_assets


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--toolchain', type=Path, help='native toolchain root containing bfin-gcc/bin and bfin/bin; otherwise use SDK Docker build')
    ap.add_argument('--skip-regenerate', action='store_true')
    ap.add_argument('--skip-previews', action='store_true')
    args = ap.parse_args()
    out = SDK / 'build/vapora-validation'
    out.mkdir(parents=True, exist_ok=True)
    if not args.skip_regenerate:
        gen_tables.generate()
        digest = hashlib.sha256((ROOT / 'vapora_tables.h').read_bytes()).digest()
        gen_tables.generate()
        assert digest == hashlib.sha256((ROOT / 'vapora_tables.h').read_bytes()).digest(), 'nondeterministic tables'
        print('Table generator repeatability: PASS')
    gen_art.generate()
    gen_presets.generate()
    with (out / 'checks.log').open('w') as log:
        def run(command, env=None):
            p = subprocess.run(list(map(str, command)), cwd=SDK, env=env, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            print(p.stdout, end='')
            log.write(p.stdout)
            log.flush()
            p.check_returncode()
        run(['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unknown-pragmas',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-DVAPORA_HOST',
             '-I' + str(SDK), '-I' + str(ROOT), ROOT / 'tests/host.c',
             ROOT / 'vapora.c', ROOT / 'vapora_dsp.c', ROOT / 'vapora_screen.c',
             '-lm', '-o', out / 'check'])
        run([out / 'check', out])
        check_assets.check(out, render_previews=not args.skip_previews)
        sys.path.insert(0, str(SDK))
        import make_plugin
        if args.toolchain:
            toolchain = args.toolchain.resolve()
            env = dict(os.environ, PATH=os.pathsep.join([
                str(toolchain / 'bfin-gcc/bin'), str(toolchain / 'bfin/bin'), os.environ['PATH']]))
            # Run the unmodified SDK packer's compile/link commands locally.
            make_plugin.docker = lambda script: run(['sh', '-ec', script], env=env)
        target = make_plugin.build('vapora')
    print('op1r build candidate (hardware validation still required):', target)


if __name__ == '__main__':
    main()
