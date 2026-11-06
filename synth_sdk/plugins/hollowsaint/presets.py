#!/usr/bin/env python3
"""Cathedral of the Hollow Saint: 8 sound presets as OP-1 patch files.
    python3 plugin/hollowsaint/presets.py [OUT_DIR]      -> OUT_DIR/*.aif (default: presets/ next to this script)

An OP-1 synth patch is an AIFC file (16-bit mono, 44.1 kHz, 'sowt') with an APPL chunk holding "op-1" + JSON and an
empty SSND chunk (the firmware renders its own preview when it syncs). Saved sounds find the engine by its saved type
name, "hollowsaint" (the plugin id; kennel docs/preset-identity.md); knobs[4] holds
0x5300 (OP1_ENGINE_MAGIC alone): a file plugin has no fixed number, kennel finds it by name, and without the plugin
the sound falls back to the stock iter and keeps its name.
Every value stays inside the ranges the factory presets use (measured by Hundekraft, tools/op1r_presets.py): values
outside them make the firmware import the file as a plain sample.
Install: copy the presets folder to the OP-1's synth/ folder in disk mode (Shift + COM, T3), e.g. as synth/hollowsaint/.
Adapted from Hundekraft's MBIRA presets.py (GPL-3.0-or-later with the OP-1 firmware permission); presets and this
version copyright (C) 2026 the hollow_saint authors, same licence.
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

# page-2 words: blue (word 5) = the drone's key in 13 zones, FOLLOW then C .. B; green (word 6) = the Lo-Fi Relic's age
# (the relic itself is switched by the green press, which is not saved); white (word 7) = the stroke rate in 7 zones,
# steady / 1 bar / 1/2 / 1/4 / 1/8 / 1/8T / 1/16 (0.7.0; 0.6 had 5). Each zone value is the middle of its zone.
KEYS = ['follow', 'C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
RATES = ['steady', '1 bar', '1/2', '1/4', '1/8', '1/8T', '1/16']


def key_word(k):
    return int((KEYS.index(k) + 0.5) * 32768 / 13)


def rate_word(r):
    return int((RATES.index(r) + 0.5) * 32768 / len(RATES))


def pct(p):
    return round(p * 32767 / 100)


# name: (DRONE, STRINGS, WHEEL BUZZ, CATHEDRAL) in %, drone key, stroke rate, ADSR, FX, LFO, octave; then the relic's
# age in % (RELIC_AGE below). 0.7.0: midnight vigil breathes once a bar, crypt drone and drowned bell every half bar
PRESETS = [
    ("hollow saint",   (50, 50, 45, 40), 'follow', '1/8',  [1400, 9000, 32767, 5200],
     ("spring", [7000, 12000, 2500, 7000]), None, 0),
    ("midnight vigil", (70, 25, 10, 85), 'D',      '1 bar', [3600, 9000, 32767, 11000],
     ("delay", [9000, 12000, 5000, 9000]), None, 0),
    ("howling dog",    (55, 72, 86, 22), 'follow', '1/16', [700, 9000, 32767, 3000],
     None, None, 0),
    ("ash procession", (62, 55, 60, 50), 'G',      '1/4',  [1600, 9000, 32767, 6000],
     ("spring", [8000, 11000, 2000, 6000]), None, 0),
    ("bone flute",     (28, 6, 0, 62),   'follow', '1/8',  [2400, 9000, 30000, 8000],
     ("delay", [8000, 10000, 4000, 7000]), ("tremolo", [9000, 0, 0, 6000]), 1),
    ("crypt drone",    (100, 38, 30, 72), 'C',     '1/2', [4200, 9000, 32767, 12000],
     None, None, -1),
    ("plague dance",   (45, 86, 66, 14), 'follow', '1/8T', [500, 9000, 32767, 2400],
     ("spring", [6000, 10500, 1500, 5000]), None, 0),
    ("drowned bell",   (40, 35, 18, 100), 'A',     '1/2',  [2000, 9000, 32767, 14000],
     ("delay", [10500, 16000, 6000, 12000]), None, 0),
]
RELIC_AGE = {"hollow saint": 25, "midnight vigil": 60, "howling dog": 45, "ash procession": 50, "bone flute": 35,
             "crypt drone": 70, "plague dance": 85, "drowned bell": 95}


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


def knob_words(p):
    name, k, key, rate = p[0], p[1], p[2], p[3]
    return [pct(x) for x in k], [key_word(key), max(1, pct(RELIC_AGE[name])), rate_word(rate)]


def meta(p):
    name, k, key, rate, adsr, fx, lfo, octave = p
    knobs, words = knob_words(p)
    check(name, "knobs", knobs + words, [(0, 32767)] * 7)
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
        "knobs": knobs + [SELECTOR] + words,
        "lfo_active": lfo is not None,
        "lfo_params": (lfo or ("tremolo", [16000, 0, 0, 16000]))[1] + [0, 0, 0, 0],
        "lfo_type": (lfo or ("tremolo", None))[0],
        "name": name,
        "octave": octave,
        "synth_version": 2,
        "type": "hollowsaint",
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
