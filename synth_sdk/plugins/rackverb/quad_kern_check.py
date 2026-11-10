#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""QUAD's Blackfin kernels (rv_quad_kern.S) against rv_quad.c's C references on bcore: builds quad_kern_guest.c with
rv_quad.c and rv_quad_kern.S, and rv_quad.c again with -DQD_REF (the C loops, its API renamed qdr_*), runs it, exit 1
on any difference. --cost also runs it on the cycle-counting bcore (BCORE_EMU_CYCLES or OP1-Emulator
build/bcore-cycles-bin/emu) and prints the worst qd_render call per config in packets. Prints SKIP without the
toolchain or bcore.

    python3 synth_sdk/plugins/rackverb/quad_kern_check.py [--cost]
"""
import subprocess
import sys
import tempfile
from pathlib import Path

P = Path(__file__).resolve().parent
SDK = P.parents[1]
sys.path.insert(0, str(SDK / 'harness'))
import tier2 as T                                     # noqa: E402

FLAGS = ['-O2', '-mcpu=bf524', '-ffreestanding', '-fno-builtin', '-nostdlib', '-fno-common', '-Wall']
REF = ['-DQD_REF', '-Dqd_select=qdr_select', '-Dqd_render=qdr_render', '-Dqd_render_w=qdr_render_w', '-Dqd_name=qdr_name', '-Dqd_config=qdr_config']


def main():
    found = T.setup_env()
    if 'toolchain' not in found or 'emu' not in found:
        print('quad kern: SKIP (toolchain or bcore missing)')
        return 0
    t = T.tools(found['toolchain'])
    env = dict(T.os.environ, PATH=t['path'])
    with tempfile.TemporaryDirectory(prefix='quad-kern-') as tmp:
        tmp = Path(tmp)
        objs = []
        for src, extra, name in (('quad_kern_guest.c', [], 'guest'), ('rv_quad.c', ['-DQD_EXPORT_C'], 'quad'),
                                 ('rv_quad_kern.S', [], 'kern'), ('rv_quad.c', REF, 'ref')):
            o = tmp / (name + '.o')
            T.sh([t['gcc'], *FLAGS, *extra, f'-I{SDK}', f'-I{P}', '-c', str(P / src), '-o', str(o)], env=env)
            objs.append(str(o))
        o = tmp / 'crt.o'
        T.sh([t['as'], '-mcpu=bf524', str(T.HERE / 'tier2/crt.S'), '-o', str(o)])
        libgcc = T.sh([t['gcc'], '-mcpu=bf524', '-print-libgcc-file-name'], env=env).strip()
        elf = tmp / 'kern.elf'
        T.sh([t['ld'], '--no-warn-rwx-segments', '--defsym=T2_STAGE=0x02000000', '-T', str(T.HERE / 'tier2/guest.ld'),
              str(o), *objs, libgcc, '-o', str(elf)])
        emu = found['emu']
        if '--cost' in sys.argv:
            if 'emu_cycles' not in found:
                print('quad kern: no cycle-counting bcore for --cost')
                return 1
            emu = found['emu_cycles']
        cmd = [emu, '--cycles', '--fastmem', str(elf)] if emu != found['emu'] else [emu, '--fastmem', str(elf)]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=6000)
    out = r.stdout.strip()
    print('quad kern:', out or r.stderr[-300:])
    return 0 if out.startswith('KERN ok') else 1


if __name__ == '__main__':
    sys.exit(main())
