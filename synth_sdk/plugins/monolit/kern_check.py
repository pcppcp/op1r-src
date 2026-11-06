#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""MONOLIT's Blackfin kernels (monolit_dsp.S) against its own C loop on bcore: builds kern_guest.c with monolit.c twice
(with monolit_dsp.S, and with MONOLIT_C_LOOP and the symbols renamed ref_*), runs it, exit 1 on any difference. Covers
pitch words (the whole 32-bit range), velocities, knob words and render orders that exact_check.py's regimes do
not reach. Prints SKIP without the toolchain or bcore.

    python3 synth_sdk/plugins/monolit/kern_check.py
"""
import subprocess
import sys
import tempfile
from pathlib import Path

P = Path(__file__).resolve().parent
SDK = P.parents[1]
sys.path.insert(0, str(SDK / 'harness'))
import tier2 as T                                     # noqa: E402

REF = ['-DMONOLIT_C_LOOP'] + [f'-D{n}=ref_{n}' for n in ('ENGINE_monolit', 'SCREEN_monolit', 'monolit_live', 'monolit_env')]


def main():
    found = T.setup_env()
    if 'toolchain' not in found or 'emu' not in found:
        print('monolit kern: SKIP (toolchain or bcore missing)')
        return 0
    t = T.tools(found['toolchain'])
    env = dict(T.os.environ, PATH=t['path'])
    with tempfile.TemporaryDirectory(prefix='monolit-kern-') as tmp:
        tmp = Path(tmp)
        objs = []
        for src, name, extra in (('kern_guest.c', 'guest', []), ('monolit.c', 'monolit', []), ('monolit_dsp.S', 'dsp', []),
                                 ('monolit.c', 'ref', REF)):
            o = tmp / (name + '.o')
            T.sh([t['gcc'], '-O2', '-mcpu=bf524', '-ffreestanding', '-fno-builtin', '-nostdlib', '-fno-common', '-Wall',
                  *extra, f'-I{SDK}', '-c', str(P / src), '-o', str(o)], env=env)
            objs.append(str(o))
        o = tmp / 'crt.o'
        T.sh([t['as'], '-mcpu=bf524', str(T.HERE / 'tier2/crt.S'), '-o', str(o)])
        libgcc = T.sh([t['gcc'], '-mcpu=bf524', '-print-libgcc-file-name'], env=env).strip()
        elf = tmp / 'kern.elf'
        T.sh([t['ld'], '--no-warn-rwx-segments', '--defsym=T2_STAGE=0x02000000', '-T', str(T.HERE / 'tier2/guest.ld'),
              str(o), *objs, libgcc, '-o', str(elf)])
        r = subprocess.run([found['emu'], '--fastmem', str(elf)], capture_output=True, text=True, timeout=3000)
    out = r.stdout.strip()
    print('monolit kern:', out or r.stderr[-300:])
    return 0 if out.startswith('KERN ok') else 1


if __name__ == '__main__':
    sys.exit(main())
