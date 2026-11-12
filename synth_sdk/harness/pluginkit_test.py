#!/usr/bin/env python3
"""pluginkit.py: a plugin of every kind, scaffolded into a temporary folder, builds and passes its own test (and the
synth / FX render with audio.py), so the templates cannot rot; bad names are refused.   python3 harness/pluginkit_test.py"""
import os, subprocess, sys, tempfile

SDK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KIT = [sys.executable, os.path.join(SDK, 'pluginkit.py')]
fails = 0
with tempfile.TemporaryDirectory() as tmp:
    for kind in ('synth', 'fx', 'seq', 'tool'):
        name = 'pk_' + kind
        r = subprocess.run(KIT + ['new', kind, name, '--dir', tmp, '--author', 'Test Person'], capture_output=True, text=True)
        files = sorted(os.listdir(os.path.join(tmp, name))) if not r.returncode else []
        if r.returncode or f'{name}_test.c' not in files or 'plugin.json' not in files:
            fails += 1; print(f'FAIL new {kind}: {r.stdout}{r.stderr} {files}')
            continue
        r = subprocess.run(KIT + ['check', os.path.join(tmp, name), '-o', os.path.join(tmp, 'out')], capture_output=True, text=True)
        wav = os.path.join(tmp, 'out', name + '.wav')                    # synth / FX: audio.py rendered it
        if r.returncode or (kind in ('synth', 'fx') and not (os.path.isfile(wav) and os.path.getsize(wav) > 44)):
            fails += 1; print(f'FAIL check {kind}:\n{r.stdout}{r.stderr}')
    for bad in ('Big', 'a-b', 'sixteen_chars_xx', '9lives', 'supersaw', 'mysynth'):
        r = subprocess.run(KIT + ['new', 'synth', bad, '--dir', tmp], capture_output=True, text=True)
        if not r.returncode or os.path.exists(os.path.join(tmp, bad)):
            fails += 1; print(f'FAIL: name {bad!r} accepted')
print(f'{fails} failed' if fails else 'pluginkit: ok')
sys.exit(1 if fails else 0)
