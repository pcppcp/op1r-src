#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
"""SISTER screen host checks: states (defaults, ends, VESPER, the key page), eased motion after knob jumps, flame
flicker, the live parts clipped clear of the still art (ring, nun); every stroke and glyph inside x 0..320, y 0..139 (the OLED lower target, docs/screen-art.md); previews.
Host model, not emulator or OLED proof.

    python3 synth_sdk/plugins/sister/screen_check.py [OUT]
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

out = Path(sys.argv[1]) if len(sys.argv) > 1 else SDK / 'harness/build/sister-screen'
out.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='sister-screen-') as d:
    exe = Path(d) / 'screen'
    subprocess.run(['cc', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=undefined', f'-I{SDK}', f'-I{HERE}', str(HERE / 'sister_screen_test.c'), '-o', str(exe)],
                   check=True)
    frames = []
    for sc in ('states', 'motion', 'flicker', 'quiet', 'clip'):
        r = subprocess.run([str(exe), sc], capture_output=True, text=True, check=True)
        print(sc + ':', r.stderr.strip() or 'ok')
        frames += [json.loads(l) for l in r.stdout.splitlines() if l.strip()]
for f in frames:
    f['svg'] = str(HERE / 'sister.svg')
    res = layout.check_bounds(f, {'allowed': [[0, 0, 320, 139]], 'protected': []})
    assert res['status'] == 'pass', (f['name'], res)
    oprender.render(f, scale=3, stock=True).png(out / (f['name'] + '.png'))
print(f'SISTER screen: {len(frames)} frames inside x 0..320, y 0..139 (strokes and glyphs); motion eased; flames move; '
      f'previews in {out} (host model, not emulator proof)')
