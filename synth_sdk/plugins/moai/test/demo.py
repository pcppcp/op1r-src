#!/usr/bin/env python3
"""Render a demo WAV of a few presets through the real engine (one voice at a time, no OP-1 envelope: a short
amplitude fade stands in for it).   python3 demo.py out.wav"""
import sys, wave, numpy as np
from stats import render
FS = 44100
# name: (setup word (n * 1024 + 512), cutoff, emphasis, contour knob, detune, drive, drift), then notes (midi, seconds, release_at)
P = [
 ('bass',   (1536, 6000, 12000, 9000, 12000, 6000, 0), [(36, .45, .3), (36, .22, .15), (39, .45, .3), (43, .45, .3), (34, .9, .7)]),
 ('lead',   (10752, 12000, 16000, 8000, 6000, 0, 0),  [(60, .3, .22), (63, .3, .22), (67, .3, .22), (70, .3, .22), (72, 1.0, .8)]),
 ('pad',    (20992, 14000, 6000, 32000, 20000, 0, 20000), [(48, 1.8, 1.4), (55, 1.8, 1.4)]),
 ('acid',   (5632, 5000, 27000, 9000, 0, 16000, 0),          [(36, .14, .1), (36, .14, .1), (48, .14, .1), (36, .14, .1), (39, .14, .1), (36, .14, .1), (51, .14, .1), (46, .28, .2)]),
 ('zap',    (512, 20000, 32767, 6000, 0, 0, 0),         [(60, .5, .35), (67, .5, .35), (72, .9, .7)]),
 ('hollow', (15872, 16000, 10000, 20000, 9000, 0, 0), [(55, .6, .45), (58, .6, .45), (62, 1.2, 1.0)]),
 ('jestofunk', (1013, 3932, 26213, 21626, 8847, 8191, 0), [(51, .75, .6), (51, .75, .6), (49, .4, .3), (54, .75, .6)]),   # 000, the initial sound
 ('chase',  (2037, 13106, 19660, 8191, 6553, 6553, 0),   [(48, .24, .12)] * 4 + [(43, .24, .12)] * 4 + [(46, .24, .12)] * 4 + [(41, .24, .12)] * 4),   # 001
]
out = [np.zeros(FS // 5)]
for name, (st, cut, emp, con, det, drv, dri), notes in P:
    for midi, dur, rel in notes:
        x = render([midi, st, cut, emp, con, det, drv, dri, dur + 0.35, rel, 110])
        n = int(0.004 * FS); x[:n] *= np.linspace(0, 1, n)                      # stand-in for the OP-1 amp envelope
        t = np.arange(len(x)) / FS; x *= np.clip(1 - (t - rel) / 0.3, 0, 1)
        out.append(x)
    out.append(np.zeros(FS // 2))
y = np.concatenate(out); y = np.clip(y * 1.4, -32768, 32767).astype(np.int16)
with wave.open(sys.argv[1], 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS); w.writeframes(y.tobytes())
print(sys.argv[1], f'{len(y) / FS:.1f} s')
