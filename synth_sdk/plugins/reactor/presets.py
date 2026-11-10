#!/usr/bin/env python3
"""REACTOR: sound presets as OP-1 patch files.
    python3 synth_sdk/plugins/reactor/presets.py [OUT_DIR]     -> OUT_DIR/*.aif (default: presets/ next to this script)

An OP-1 synth patch is an AIFC file (16-bit mono, 44.1 kHz, 'sowt') with an APPL chunk holding "op-1" + JSON and an
empty SSND chunk. REACTOR has order 11 (plugins/ORDERS.md), so knobs[4] = 0x5300 + 11 and the saved type name is
"reactor". Every value stays inside the ranges the factory presets use (tools/op1r_presets.py in the root project):
outside them the firmware imports the file as a plain sample.

Knobs (reactor.c): blue COOLING, green RADIATION, white RODS, red HEAT; word 7 is the hidden preset trim (Q13, 8192 =
1x). The trims come from a level pass: reactor_demo.c --level (one held note at velocity 100, no envelope, 0.5..3 s;
China Syndrome at C2, as it plays an octave down) measured -8.3 .. -6.1 dBFS over these eleven (redone for the
runaway, 2026-11-04), so each is set to -6.6 dBFS, and Meltdown 0.7 dB further down. Source-only: the native envelope, LFO and FX review is open (CLAUDE.md, native audio review).
Adapted from strum's presets.py.
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
}
SELECTOR = 0x5300 + 11                  # order 11
TRIM_ONE = 8192

# name, (COOLING, RADIATION, RODS, HEAT), trim (Q13), ADSR, FX, octave. The comments say what each one shows off.
PRESETS = [
    # 2026-11-04 (the runaway): four new; idle hum, cold start and cherenkov out (the owner: replace the boring ones; those three never go critical)
    ("china syndrome", ( 2500,  4000, 32767, 16000),  8697, (64, 6000, 26000, 3000),   None, -1),
    #   bass with no cooling: critical after about 0.7 s, then the octave-down growl and the pitch sinking under the load
    ("breeder",       ( 9000,  3000, 20000, 13500),  7689, (8000, 12000, 30000, 12000), ("spring", [7000, 12000, 2500, 8000]), 0),
    #   pad on the edge: the rods breathe faster as it heats, and a held chord trips after about 4 s
    ("ignition",      (20000,  1500, 18000,  9000),  8354, (64, 8000, 14000, 4000),   ("delay", [7000, 9000, 2500, 5000]), 0),
    #   keys that stay cool: hard notes bark (velocity drives the attack), soft ones hum
    ("turbine",       ( 6000,  5000, 28000, 20000),  8578, (300, 7000, 26000, 6000),  ("delay", [6500, 11000, 2000, 4500]), 0),
    #   lead that trips in about 0.6 s and dives in pitch while held; it spins back up as it cools in the release
    ("coolant leak",  ( 6000,  3000, 14000, 13000),  7850, (200, 8000, 26000, 5000),  None, 0),
    #   short notes stay clean, a held one heats up and goes critical after a few seconds
    ("geiger lead",   (18000, 26000, 12000, 10000),  7932, (64, 6000, 24000, 2500),   ("delay", [6500, 11000, 2000, 4500]), 0),
    #   clicks and grit on a lead
    ("half-life pad", (12000,  5000, 24000, 16000),  7914, (9000, 12000, 28000, 12000), ("spring", [7000, 12000, 2500, 8000]), 0),
    #   heat just under critical: it trips while held and calms in the release
    ("core bass",     (14000,  2000, 32767, 12000),  7796, (64, 5000, 14000, 1500),   None, -1),
    #   all rods out, short envelope, drive
    ("meltdown",      (    0, 20000, 32767, 32767),  8829, (3000, 10000, 30000, 9000), None, -1),
    #   no cooling, everything else full: a hot overdriven drone, growling an octave down and sagging
    ("control rods",  (24000,     0, 26000,  4000),  9929, (64, 11000, 0, 6000),      ("spring", [6000, 13000, 2000, 7000]), 0),
    #   rods out, cool and calm: a struck FM bell
    ("scram",         ( 8000,  8000, 30000, 24000),  8687, (64, 4000, 6000, 1200),    ("delay", [9000, 14000, 4000, 9000]), 0),
    #   hot, short stabs that flare into critical on the attack
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


def meta(p):
    name, knobs, trim, adsr, fx, octave = p
    check(name, "knobs", list(knobs) + [trim], [(0, 32767)] * 5)
    check(name, "adsr", adsr, RANGE_ADSR)
    if fx:
        check(name, "fx " + fx[0], fx[1], RANGE_FX[fx[0]])
    return {
        "adsr": list(adsr) + [2048, 5568, 4000, 4000],
        "fx_active": fx is not None,
        "fx_params": (fx or ("delay", [8000] * 4))[1] + [0, 0, 0, 0],
        "fx_type": (fx or ("delay", None))[0],
        "knobs": list(knobs) + [SELECTOR, 0, 0, trim],
        "lfo_active": False,
        "lfo_params": [16000, 0, 0, 16000, 0, 0, 0, 0],
        "lfo_type": "tremolo",
        "name": name,
        "octave": octave,
        "synth_version": 2,
        "type": "reactor",
    }


def main():
    if len(sys.argv) > 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1]) if len(sys.argv) == 2 else Path(__file__).resolve().parent / "presets"
    out.mkdir(parents=True, exist_ok=True)
    for p in PRESETS:
        (out / (p[0] + ".aif")).write_bytes(aifc(meta(p)))
    print(f"{len(PRESETS)} presets in {out}")


if __name__ == "__main__":
    main()
