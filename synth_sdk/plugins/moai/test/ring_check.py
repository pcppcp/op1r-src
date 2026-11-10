"""Self-oscillation / ringing: impulse into the ladder at several cutoffs and emphasis; frequency, amplitude, decay."""
import numpy as np
from filter_response import run
print(' Hz    emph | f_osc (err) | amp@1s  amp@2s  amp@3s (peak, 16-bit) | verdict')
for hz in (55, 110, 220, 440, 880, 1760, 3500, 7000, 12000, 18000):
    for emp in (0.9, 1.0, 1.03):
        y = run('ring', hz, emp, 44100 * 3, 6000)
        seg = y[44100 * 2 - 4096:44100 * 2]
        a = [np.abs(y[int(44100 * t) - 2048:int(44100 * t)]).max() for t in (1, 2, 3)]
        w = np.hanning(len(seg)); sp = np.abs(np.fft.rfft(seg * w))
        f = np.argmax(sp) * 44100 / len(seg) if sp.max() > 0 else 0
        verdict = 'sustained' if a[2] > 0.5 * a[0] and a[2] > 200 else ('decaying' if a[2] < 0.5 * a[0] or a[2] < 50 else 'slow')
        print(f'{hz:6d} {emp:4.2f} | {f:7.0f} ({100 * (f / hz - 1):+5.1f}%) | {a[0]:6.0f} {a[1]:6.0f} {a[2]:6.0f} | {verdict}')
