#!/usr/bin/env python3
"""Four original stock-Phase sounds that show SISTER's characters: CHOIR, PIPES, VESPER and a blend.
Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.

    python3 synth_sdk/plugins/sister/presets.py OUT        -> OUT/sister-presets.zip (folder sister/, README.txt)

The files hold no audio (the OP-1 renders their previews) and no stock data. They use Father's preset form
(plugins/father/presets.py): stock Phase, fx_type "sister", fx_params as the filter slot's raw words.
"""
import argparse
import json
from pathlib import Path
import struct
import zipfile

# The filter slot's raw knob ranges (dispatcher.c fx_rng_default), as Father's presets use them.
FILTER_RANGES = ((64, 14528), (0, 24576), (0, 22528), (8192, 32767))
SELECTOR = 0x5300           # neutral file-plugin selector; the OP-1 resolves the effect by fx_type
KEY_C3 = 16384              # page-2 green (word 6): the PIPES key, OP1_PRESS_PAGE_SEMI centre = C3


def word(i, v99):
    lo, hi = FILTER_RANGES[i]
    return lo + round((hi - lo) * v99 / 99)


def key_word(semitones):
    """op1_plugin.h op1_semi_word for whole semitones (centre 16384 +- dead band 600, then 15784 per 24 st)"""
    if not semitones:
        return KEY_C3
    u = 600 + -(-(abs(semitones) * 100 * 15784) // 2400)
    return KEY_C3 - u if semitones < 0 else min(32767, KEY_C3 + u)


PRESETS = [
    # name, phase knobs, adsr, (space, voice, swell, mix) 0..99, key semitones from C3
    ('sister choir', [6000, 9000, 2000, 6000], [9000, 16320, 26000, 16320, 9000, 9000, 4000, 4000], (70, 8, 0, 55), 0),
    ('sister pipes', [2000, 14000, 0, 3000], [64, 4000, 6000, 9000, 2048, 5568, 4000, 4000], (55, 99, 0, 45), 0),
    ('sister vesper', [10000, 4000, 6000, 12000], [64, 9000, 14000, 12000, 2048, 5568, 4000, 4000], (60, 20, 99, 60), 0),
    ('sister nave', [8000, 7000, 3000, 8000], [3000, 12000, 20000, 14000, 4000, 6000, 4000, 4000], (85, 50, 40, 50), -5),
]


def metadata(name, knobs, adsr, macros, key):
    fx = [word(i, v) for i, v in enumerate(macros)] + [SELECTOR, 0, key_word(key), 0]
    return dict(name=name, type='phase', synth_version=2, octave=0, knobs=knobs + [0, 0, 0, 0], adsr=adsr,
                fx_type='sister', fx_active=True, fx_params=fx,
                lfo_type='tremolo', lfo_active=False, lfo_params=[16000, 0, 0, 16000, 0, 0, 0, 0])


def chunk(name, data):
    return name + struct.pack('>I', len(data)) + data + (b'\0' if len(data) & 1 else b'')


def aifc(meta):
    rate = bytes.fromhex('400eac44000000000000')      # 80-bit 44100 Hz; zero frames: the OP-1 renders the preview
    comm = struct.pack('>hIh', 1, 0, 16) + rate + b'sowt' + b')Signed integer (little-endian) linear PCM'
    js = json.dumps(meta, sort_keys=True, separators=(',', ':')).encode() + b'\n'
    body = chunk(b'FVER', bytes.fromhex('a2805140')) + chunk(b'COMM', comm) + chunk(b'APPL', b'op-1' + js) + chunk(b'SSND', bytes(8))
    return b'FORM' + struct.pack('>I', len(body) + 4) + b'AIFC' + body


def parse(data):
    assert data[:4] == b'FORM' and data[8:12] == b'AIFC' and int.from_bytes(data[4:8], 'big') == len(data) - 8
    pos = 12
    while pos < len(data):
        n = int.from_bytes(data[pos + 4:pos + 8], 'big')
        body = data[pos + 8:pos + 8 + n]
        if data[pos:pos + 4] == b'APPL':
            assert body[:4] == b'op-1'
            return json.loads(body[4:])
        pos += 8 + n + (n & 1)
    raise ValueError('missing patch metadata')


README = """SISTER - four stock Phase sounds that show the SISTER effect

Needs Hundekraft 3.x with sister.knl installed. Copy the sister folder into synth/ on the OP-1 disk, eject and let
it sync. In synth mode, Shift+1 opens the preset browser; pick SISTER and one of:

  sister choir   a slow pad; the tail sings it back on drifting vowels (VOICE near 0)
  sister pipes   a short pluck; the tail rings on C, G and the octave (VOICE 99)
  sister vesper  each note swells up backwards out of its own tail (SWELL 99)
  sister nave    a long hall halfway between choir and pipes, keyed to G, with some swell

T3 opens the effect page. Press green to set the PIPES key (one semitone per step, two octaves either way of C3). The files hold no audio; the
OP-1 makes their previews.
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('out', type=Path)
    a = ap.parse_args()
    folder = a.out / 'sister'
    folder.mkdir(parents=True, exist_ok=True)
    for name, knobs, adsr, macros, key in PRESETS:
        meta = metadata(name, knobs, adsr, macros, key)
        data = aifc(meta)
        assert parse(data) == meta
        assert all(lo <= v <= hi for v, (lo, hi) in zip(meta['fx_params'], FILTER_RANGES))
        assert 0 < meta['fx_params'][6] <= 32767
        (folder / (name.replace(' ', '-') + '.aif')).write_bytes(data)
    (a.out / 'README.txt').write_text(README)
    with zipfile.ZipFile(a.out / 'sister-presets.zip', 'w', zipfile.ZIP_DEFLATED) as z:
        for f in sorted(folder.glob('*.aif')):
            z.write(f, 'sister/' + f.name)
        z.writestr('README.txt', README)
    print(a.out / 'sister-presets.zip')


if __name__ == '__main__':
    main()
