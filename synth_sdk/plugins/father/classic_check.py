#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""CLASSIC against hundekraft 3.1.4's Father: builds classic_run.h's run (host C) once with this father_dsp.c in
CLASSIC and once with kennel bd899531's father_dsp.c / father_dsp.h / father_lines.h / father_tables.h (taken from
git; that Father has only the one mode), prints both hashes and exit 1 when they differ. A match is the same output
sample for sample for that run (mono and stereo, every control, the gate). father_test.c asserts the hash printed
here (CLASSIC_HASH in classic_run.h). Prints SKIP when the commit is not in this clone.

    python3 synth_sdk/plugins/father/classic_check.py
"""
import subprocess
import sys
import tempfile
from pathlib import Path

P = Path(__file__).resolve().parent
OLD = 'bd899531'                                       # hundekraft 3.1.4's kennel pin
FILES = ('father_dsp.c', 'father_dsp.h', 'father_lines.h', 'father_tables.h')
DRIVER = '#include <stdio.h>\n#include "father_dsp.h"\n#include "%s"\n' \
         'int main(void){printf("%%016llx\\n",(unsigned long long)classic_run());return 0;}\n'


def build_run(tmp, name, inc, src, defs):
    d = tmp / (name + '.c')
    d.write_text(DRIVER % (P / 'classic_run.h'))
    exe = tmp / (name + '.exe')
    subprocess.run(['cc', '-O1', *defs, f'-I{inc}', str(d), str(src), '-o', str(exe)], check=True)
    return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.strip()


def main():
    root = subprocess.run(['git', 'rev-parse', '--show-toplevel'], cwd=P, capture_output=True, text=True).stdout.strip()
    rel = P.relative_to(Path(root).resolve()) if root else None
    with tempfile.TemporaryDirectory(prefix='father-classic-') as tmp:
        tmp = Path(tmp)
        old = tmp / 'old'
        old.mkdir()
        for f in FILES:
            r = subprocess.run(['git', 'show', f'{OLD}:{rel}/{f}'], cwd=P, capture_output=True)
            if r.returncode:
                print(f'father classic: SKIP ({OLD} not in this clone)')
                return 0
            (old / f).write_bytes(r.stdout)
        a = build_run(tmp, 'now', P, P / 'father_dsp.c', [])
        b = build_run(tmp, 'old', old, old / 'father_dsp.c', ['-DCLASSIC_ON(st)=((void)0)'])
    print(f'father classic: CLASSIC {a}, {OLD} {b}', 'same' if a == b else 'DIFFER')
    return 0 if a == b else 1


if __name__ == '__main__':
    sys.exit(main())
