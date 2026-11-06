#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
"""juni chorus screen host checks: the real DSP under the screen (juni_chorus_screen_test.c). States (defaults, each
mode, DEPTH / TONE / MIX ends), every stroke and glyph inside x 0..320, y 0..139 (the OLED lower target,
docs/screen-art.md) at every sway phase; motion: MIX 0 still, the sway follows DEPTH and MODE, the ripple the input,
TONE only the moon; a frame strip; previews. Host model, not emulator or OLED proof.

    python3 synth_sdk/plugins/juni_chorus/screen_check.py [OUT]
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SDK = HERE.parents[1]
sys.path.insert(0, str(SDK / 'harness'))
import layout    # noqa: E402
import oprender  # noqa: E402
import svg_check  # noqa: E402

assert not svg_check.filled_subpaths_in((HERE / 'juni_chorus.svg').read_bytes())
out = Path(sys.argv[1]) if len(sys.argv) > 1 else SDK / 'harness/build/juni-chorus-screen'
out.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='juni-chorus-screen-') as d:
    exe = Path(d) / 'screen'
    subprocess.run(['cc', '-O1', '-g', '-DOP1_HOST', '-Wall', '-Wextra', '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=undefined', f'-I{SDK}', f'-I{HERE}', str(HERE / 'juni_chorus_screen_test.c'),
                    str(HERE / 'juni_chorus.c'), '-lm', '-o', str(exe)], check=True)
    frames = []
    for sc in ('states', 'motion', 'strip'):
        r = subprocess.run([str(exe), sc], capture_output=True, text=True, check=True)
        print(sc + ':', r.stderr.strip() or 'ok')
        frames += [json.loads(l) for l in r.stdout.splitlines() if l.strip()]
for f in frames:
    f['svg'] = str(HERE / 'juni_chorus.svg')
    res = layout.check_bounds(f, {'allowed': [[0, 0, 320, 139]], 'protected': []})
    assert res['status'] == 'pass', (f['name'], res)
    oprender.render(f, scale=3, stock=True).png(out / (f['name'] + '.png'))
n = max(f['nlines'] for f in frames)
print(f'juni chorus screen: {len(frames)} frames inside x 0..320, y 0..139 (strokes and glyphs), at most {n} lines a '
      f'frame; MIX 0 still, sway and ripple follow their knobs; previews in {out} (host model, not emulator proof)')
