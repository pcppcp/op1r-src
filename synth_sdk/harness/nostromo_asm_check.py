#!/usr/bin/env python3
"""Compile plugins/nostromo/nostromo.c for the Blackfin as make_plugin.py does (Docker, bfin-elf-toolchain) and check
the functions that run per tank sample call no libgcc division or modulo, memset or memcpy (PLUGINS.md: none per
sample). Per-block code, inlined into nt_render, may divide.

    python3 harness/nostromo_asm_check.py
"""
import os, re, subprocess

SDK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFLAGS = re.search(r"^CFLAGS = '([^']*)'", open(os.path.join(SDK, 'make_plugin.py')).read(), re.M).group(1)
PER_SAMPLE = ('_nt_tank', '_nt_run', '_nt_lfo', '_nt_er')
BANNED = re.compile(r'___u?(div|mod)si3|_memset|_memcpy')


def main():
    s = subprocess.run(['docker', 'run', '--rm', '-u', f'{os.getuid()}:{os.getgid()}', '-v', f'{SDK}:/work',
                        'bfin-elf-toolchain', 'sh', '-ec', f'cd /work; bfin-elf-gcc {CFLAGS} -S -o - plugins/nostromo/nostromo.c'],
                       check=True, capture_output=True, text=True).stdout
    parts = re.split(r'(?m)^(_\w+):$', s)
    funcs = dict(zip(parts[1::2], parts[2::2]))
    assert '_nt_tank' in funcs and '_nt_run' in funcs, 'nt_tank / nt_run inlined: check what runs per sample'
    for f in PER_SAMPLE:
        if f in funcs:
            bad = sorted({c for c in re.findall(r'\bcall\s+([\w.]+)', funcs[f]) if BANNED.match(c)})
            assert not bad, f'{f[1:]} calls {bad} per tank sample'
    print('ok asm:', ', '.join(f[1:] for f in PER_SAMPLE if f in funcs), '— no division, modulo, memset or memcpy per sample')


if __name__ == '__main__':
    main()
