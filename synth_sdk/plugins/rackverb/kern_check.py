#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""rackverb's block engine (rv_mv.c with rv_kern.S) against rv_mv_run_ref on bcore: builds kern_guest.c with
rv_mv.c and rv_kern.S (pack.py's flags), runs it, exit 1 on any difference. Covers wrapping spans,
odd call lengths, SIZE and LFO patching. Prints SKIP without the toolchain
or bcore.

    python3 synth_sdk/plugins/rackverb/kern_check.py
"""
import subprocess
import sys
import tempfile
from pathlib import Path

P = Path(__file__).resolve().parent
SDK = P.parents[1]
sys.path.insert(0, str(SDK / 'harness'))
import tier2 as T                                     # noqa: E402


def main():
    found = T.setup_env()
    if 'toolchain' not in found or 'emu' not in found:
        print('rackverb kern: SKIP (toolchain or bcore missing)')
        return 0
    t = T.tools(found['toolchain'])
    env = dict(T.os.environ, PATH=t['path'])
    with tempfile.TemporaryDirectory(prefix='rackverb-kern-') as tmp:
        tmp = Path(tmp)
        objs = []
        for src in ('kern_guest.c', 'rv_mv.c', 'rv_kern.S'):
            o = tmp / (src.replace('.', '_') + '.o')
            T.sh([t['gcc'], '-O2', '-mcpu=bf524', '-ffreestanding', '-fno-builtin', '-nostdlib', '-fno-common', '-Wall',
                  f'-I{SDK}', '-c', str(P / src), '-o', str(o)], env=env)
            objs.append(str(o))
        o = tmp / 'crt.o'
        T.sh([t['as'], '-mcpu=bf524', str(T.HERE / 'tier2/crt.S'), '-o', str(o)])
        libgcc = T.sh([t['gcc'], '-mcpu=bf524', '-print-libgcc-file-name'], env=env).strip()
        elf = tmp / 'kern.elf'
        T.sh([t['ld'], '--no-warn-rwx-segments', '--defsym=T2_STAGE=0x02000000', '-T', str(T.HERE / 'tier2/guest.ld'),
              str(o), *objs, libgcc, '-o', str(elf)])
        r = subprocess.run([found['emu'], '--fastmem', str(elf)], capture_output=True, text=True, timeout=3000)
    out = r.stdout.strip()
    print('rackverb kern:', out or r.stderr[-300:])
    return 0 if out.startswith('KERN ok') else 1


if __name__ == '__main__':
    sys.exit(main())
