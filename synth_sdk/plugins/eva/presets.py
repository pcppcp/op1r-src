#!/usr/bin/env python3
"""EVA sound presets as OP-1 patch files: python3 synth_sdk/plugins/eva/presets.py [OUT_DIR] -> OUT_DIR/*.aif
(default: presets/ next to this script; the generated files are kept there for the user to install)

An OP-1 synth patch is an AIFC file (16-bit mono, 44.1 kHz, 'sowt') with an APPL chunk holding "op-1" + JSON and an
empty SSND chunk (the firmware renders its own preview when it syncs). Saved sounds find EVA by its type name
("type": "eva", docs/preset-identity.md); knobs[4] holds 0x5300 (OP1_ENGINE_MAGIC alone): a file plugin has no fixed
number, kennel rewrites the selector from the type name when eva is loaded, and without eva the sound falls back to
the stock iter instead of whichever file engine holds a guessed number.
Every value stays inside the ranges the factory presets use: values outside them make the firmware import the file
as a plain sample (tools/op1r_presets.py, where these ranges were measured).
Copy the presets folder to the OP-1's synth/ folder in disk mode (Shift + COM, T3), e.g. as synth/eva/.
Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""
import json
import math
import struct
import sys
from pathlib import Path

RANGE_ADSR = [(64, 16320), (64, 16320), (0, 32767), (64, 16320)]
RANGE_FX = {
    "delay": [(3392, 11200), (4969, 32767), (0, 16384), (1332, 32767)],
    "spring": [(1344, 16448), (9920, 16448), (0, 16384), (2047, 26120)],
    "cwo": [(0, 22527), (0, 29055), (0, 32767), (0, 32767)],
}
RANGE_LFO = {
    "tremolo": [(2289, 29816), (-32767, 32767), (-18663, 32767), (0, 32767)],
}
SELECTOR = 0x5300                 # OP1_ENGINE_MAGIC: no fixed number (see above)

# name: knobs (PRESSURE, FREQ (vowel), OXYGEN, COM), ADSR (attack, decay, sustain, release), FX, LFO, octave
PRESETS = [
    ("space choir", [20000, 16384, 12000, 3000], [3000, 12000, 30000, 13000],
     ("spring", [16448, 15000, 2000, 10239]), None, 0),
    ("tears in rain", [11000, 4000, 22000, 6000], [6000, 14000, 30000, 15000],
     ("spring", [16448, 16000, 3000, 12000]), ("tremolo", [2600, 2000, 0, 1500]), 0),
    ("ensemble ah", [24000, 15500, 28000, 0], [2500, 12000, 29000, 12000],
     ("spring", [14000, 14000, 2500, 10000]), None, 0),
    ("ee halo", [26000, 31000, 18000, 2000], [2000, 10000, 26000, 14000],
     ("delay", [9000, 16000, 9000, 14000]), None, 1),
    ("distant oo", [3500, 1500, 15000, 9000], [7000, 14000, 30000, 15000],
     ("spring", [16448, 16448, 4000, 14000]), None, 0),
    ("radio choir", [20000, 14000, 10000, 17000], [3000, 12000, 28000, 11000],
     ("spring", [14000, 13000, 3000, 9000]), None, 0),
    ("houston", [19000, 11000, 6000, 23000], [1500, 9000, 26000, 9000],
     ("delay", [7000, 12000, 6000, 9000]), None, 0),
    ("lost signal", [16000, 21000, 20000, 30000], [2000, 12000, 26000, 12000],
     ("delay", [8000, 14000, 8000, 12000]), None, 0),
]


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


def check(name, what, values, ranges):
    for i, (v, (lo, hi)) in enumerate(zip(values, ranges)):
        if not lo <= v <= hi:
            raise ValueError(f"{name}: {what}[{i}] = {v} is outside {lo}..{hi}")


def meta(name, knobs, adsr, fx, lfo, octave):
    check(name, "knobs", knobs, [(0, 32767)] * 4)
    check(name, "adsr", adsr, RANGE_ADSR)
    if fx:
        check(name, "fx " + fx[0], fx[1], RANGE_FX[fx[0]])
    if lfo:
        check(name, "lfo " + lfo[0], lfo[1], RANGE_LFO[lfo[0]])
    return {
        "adsr": list(adsr) + [2048, 5568, 4000, 4000],
        "fx_active": fx is not None,
        "fx_params": (fx or ("delay", [8000] * 4))[1] + [0, 0, 0, 0],
        "fx_type": (fx or ("delay", None))[0],
        "knobs": list(knobs) + [SELECTOR, 0, 0, 0],
        "lfo_active": lfo is not None,
        "lfo_params": (lfo or ("tremolo", [16000, 0, 0, 16000]))[1] + [0, 0, 0, 0],
        "lfo_type": (lfo or ("tremolo", None))[0],
        "name": name,
        "octave": octave,
        "synth_version": 2,
        "type": "eva",
    }


def main():
    if len(sys.argv) > 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1]) if len(sys.argv) == 2 else Path(__file__).resolve().parent / "presets"
    out.mkdir(parents=True, exist_ok=True)
    for p in PRESETS:
        (out / (p[0] + ".aif")).write_bytes(aifc(meta(*p)))
    print(f"{len(PRESETS)} presets in {out}")


if __name__ == "__main__":
    main()
