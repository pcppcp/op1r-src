#!/usr/bin/env python3
"""Twelve normal OP-1 presets for JUNI (written to presets/ next to this file, or the folder given).

    python3 presets.py [out_dir]

knobs: TONE, SHAPE, SWEEP, CHORUS, the engine selector word (0x5300), page 2 RESONANCE, HPF, PWM RATE.
SHAPE's five points sit at 0 (saw + sub), 8192 (saw), 16384 (saw + pulse), 24576 (PWM strings), 32767 (all);
CHORUS: off below 3277, I around 6000..8000, II 16384..24576, I+II at 32767. T2 (adsr) is the amp and the filter's
envelope. Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""
import json, math, struct, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SEL = 0x5300


def ieee_extended(x):
    exp = int(math.floor(math.log2(x)))
    return struct.pack(">HQ", exp + 16383, int(x / 2 ** exp * 2 ** 63))


def chunk(cid, body):
    return cid + struct.pack(">I", len(body)) + body + (b"\0" if len(body) & 1 else b"")


def aifc(meta):
    comm = struct.pack(">hIh", 1, 0, 16) + ieee_extended(44100) + b"sowt" + b")Signed integer (little-endian) linear PCM"
    js = json.dumps(meta, separators=(",", ":"), sort_keys=True).encode() + b"\n"
    if len(js) & 1:
        js += b" "
    body = (chunk(b"FVER", bytes.fromhex("a2805140")) + chunk(b"COMM", comm) + chunk(b"APPL", b"op-1" + js)
            + chunk(b"SSND", struct.pack(">II", 0, 0)))
    return b"FORM" + struct.pack(">I", len(body) + 4) + b"AIFC" + body


# name, (TONE, SHAPE, SWEEP, CHORUS), (RESONANCE, HPF, PWM RATE), adsr (attack, decay, sustain, release)
SOUNDS = [
    ('juni pad',    (17000, 24576,  6000, 20000), (    0,     0,     0), (4500, 11000, 26000, 11000)),
    ('poly brass',  (12500, 16384, 20000,  7000), ( 4000,     0,     0), ( 500,  7000, 22000,  4000)),
    ('sub bass',    (13000,     0, 15000,     0), ( 3000,     0,     0), (  64,  6000, 30000,  2500)),
    ('pluck keys',  (11000,  8192, 24000,  7000), (    0,  3000,     0), (  64,  9000, 10000,  3500)),
    ('ensemble',    (21000, 26000,  3000, 32767), (    0,  6000,  4000), (3000,  9000, 28000,  9000)),
    ('reso sweep',  ( 6000,  8192, 30000, 12000), (20000,     0,     0), (2500, 12000, 20000,  9000)),
    ('organ',       (24000, 12288,     0, 28000), (    0,  4000,     0), (  64,  2000, 26000,  1500)),
    ('big lead',    (19000, 32767,  9000,  6000), ( 6000,     0,  8000), (  64,  7000, 26000,  3000)),
    ('glass pad',   (22000, 22000,  5000, 20000), ( 3000, 18000, 10000), (6000, 12000, 25000, 12000)),
    ('deep bass',   (14000,  2000, 12000,     0), ( 9000,     0,     0), (  64,  5000, 32767,  2000)),
    ('stab',        (11000, 16384, 28000,  7000), (10000,     0,     0), (  64,  7500, 10000,  1800)),
    ('dream pad',   (15000, 30000, 12000, 26000), (    0,     0,  6000), (7000, 12000, 25000, 13000)),
]


def metas():
    for name, k, p2, adsr in SOUNDS:
        yield {'adsr': [*adsr, 2048, 5568, 4000, 4000], 'fx_active': False, 'fx_params': [8000, 8000, 8000, 8000, 0, 0, 0, 0],
               'fx_type': 'delay', 'knobs': [*k, SEL, *p2], 'lfo_active': False,
               'lfo_params': [16000, 0, 0, 16000, 0, 0, 0, 0], 'lfo_type': 'tremolo', 'name': name, 'octave': 0,
               'synth_version': 2, 'type': 'juni'}


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / 'presets'
    out.mkdir(parents=True, exist_ok=True)
    for meta in metas():
        (out / (meta['name'] + '.aif')).write_bytes(aifc(meta))
    print(f'{len(SOUNDS)} JUNI presets in {out}')


if __name__ == '__main__':
    main()
