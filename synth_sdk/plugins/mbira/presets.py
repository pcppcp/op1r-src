#!/usr/bin/env python3
"""MBIRA sound presets as OP-1 patch files: python3 synth_sdk/plugins/mbira/presets.py [OUT_DIR] -> OUT_DIR/*.aif
(default: presets/ next to this script; the generated files are kept there for the user to install)

An OP-1 synth patch is an AIFC file (16-bit mono, 44.1 kHz, 'sowt') with an APPL chunk holding "op-1" + JSON and an
empty SSND chunk (the firmware renders its own preview when it syncs). Saved sounds find MBIRA by its type name
("type": "mbira", docs/preset-identity.md); knobs[4] holds 0x5300 (OP1_ENGINE_MAGIC alone): a file plugin has no fixed
number, kennel rewrites the selector from the type name when mbira is loaded, and without mbira the sound falls back to
the stock iter instead of whichever file engine holds a guessed number.
Every value stays inside the ranges the factory presets use: values outside them make the firmware import the file
as a plain sample (tools/op1r_presets.py, where these ranges were measured).
Copy the presets folder to the OP-1's synth/ folder in disk mode (Shift + COM, T3), e.g. as synth/mbira/.
"marimba steps" uses the STUTTER effect (fx_type "stutter", page-2 word 5 = 4 parts): it needs stutter.knl too.
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
    # kennel's STUTTER runs in the filter FX slot: its raw ranges (fx_params[4] = 0x5300, found by name like the engine)
    "stutter": [(64, 14528), (0, 24576), (0, 22528), (8192, 32767)],
}
RANGE_LFO = {
    "tremolo": [(2289, 29816), (-32767, 32767), (-18663, 32767), (0, 32767)],
}
SELECTOR = 0x5300                 # OP1_ENGINE_MAGIC: no fixed number (see above)

# name: knobs (STRIKE, TINE, BOX, BUZZ), page-2 words (RING, DOUBLE, WAH), ADSR, FX, LFO, octave
PRESETS = [
    ("kalimba", [14000, 16384, 12000, 0], [0, 0, 0], [64, 9000, 30000, 9000],
     ("spring", [8000, 12000, 3000, 8000]), None, 0),
    ("thumb wah", [16000, 16384, 14000, 6000], [8000, 6000, 20000], [64, 9000, 30000, 9000],
     ("delay", [7000, 14000, 6000, 10000]), None, 0),
    ("glass rain", [26000, 30000, 6000, 0], [16000, 12000, 0], [64, 12000, 26000, 12000],
     ("delay", [9000, 18000, 9000, 14000]), None, 1),
    ("marimba steps", [12000, 2000, 20000, 9000], [0, 0, 0], [64, 7000, 24000, 6000],
     ("stutter", [7635, 1728, 5500, 20192]), None, 0),
    ("muted wood", [9000, 2000, 5000, 0], [0, 0, 0], [64, 9000, 24000, 2000],
     None, None, 0),
    ("steel duet", [14500, 16384, 9000, 0], [5000, 6000, 0], [64, 8000, 25000, 5000],
     None, None, 0),
    ("bottle caps", [19000, 16000, 17000, 16000], [0, 0, 0], [64, 6500, 22000, 3000],
     None, None, 0),
    ("glass dusk", [10000, 30000, 4000, 0], [10000, 0, 0], [64, 10000, 30000, 6500],
     ("delay", [7000, 10000, 5000, 5000]), None, 0),
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


def meta(name, knobs, words, adsr, fx, lfo, octave):
    check(name, "knobs", knobs + words, [(0, 32767)] * 7)
    check(name, "adsr", adsr, RANGE_ADSR)
    if fx:
        check(name, "fx " + fx[0], fx[1], RANGE_FX[fx[0]])
    if lfo:
        check(name, "lfo " + lfo[0], lfo[1], RANGE_LFO[lfo[0]])
    return {
        "adsr": list(adsr) + [2048, 5568, 4000, 4000],
        "fx_active": fx is not None,
        "fx_params": (fx or ("delay", [8000] * 4))[1] + ([SELECTOR, 13568, 0, 0] if fx and fx[0] == "stutter" else [0, 0, 0, 0]),
        "fx_type": (fx or ("delay", None))[0],
        "knobs": list(knobs) + [SELECTOR] + list(words),
        "lfo_active": lfo is not None,
        "lfo_params": (lfo or ("tremolo", [16000, 0, 0, 16000]))[1] + [0, 0, 0, 0],
        "lfo_type": (lfo or ("tremolo", None))[0],
        "name": name,
        "octave": octave,
        "synth_version": 2,
        "type": "mbira",
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
