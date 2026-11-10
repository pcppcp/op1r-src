"""Oscillator quality: pitch, level and aliasing of the waves, polyBLEP vs naive."""
import subprocess, os, numpy as np
EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build', 'mm_osc')
def osc(midi, wk, blep, secs=1.0):
    r = subprocess.run([EXE, str(midi), str(wk), str(secs), str(blep)], capture_output=True)
    if r.returncode: raise SystemExit(r.stderr.decode())
    return np.frombuffer(r.stdout, dtype=np.int16).astype(float)
def alias_db(x, f0):
    """energy outside +-3 bins of the harmonics of f0 (relative to total), dB"""
    n = len(x); sp = np.abs(np.fft.rfft(x * np.blackman(n))) ** 2
    f = np.fft.rfftfreq(n, 1 / 44100)
    hb = np.zeros(len(f), bool)
    for h in range(1, int(22050 / f0) + 1):
        hb |= np.abs(f - h * f0) < 8 * 44100 / n * 2
    return 10 * np.log10(sp[~hb & (f > 20)].sum() / sp[hb].sum())
names = {2: 'saw', 3: 'reverse saw', 1: 'shark', 4: 'square', 5: 'pulse 30%', 6: 'pulse 12%', 0: 'triangle'}
print('wave        note   f_meas (cents err)  rms   |  alias rel. dB:  blep  naive')
for wk, nm in names.items():
    for midi in (36, 60, 84, 96):
        f0 = 440 * 2 ** ((midi - 69) / 12)
        x = osc(midi, wk, 1); xn = osc(midi, wk, 0)
        # pitch from the autocorrelation-free approach: zero-crossing-independent -> spectral peak with parabolic interpolation
        sp = np.abs(np.fft.rfft(x * np.hanning(len(x)))); k = np.argmax(sp[:int(2.5 * f0 * len(x) / 44100)])
        if 0 < k < len(sp) - 1:
            a, b, c = np.log(sp[k - 1:k + 2] + 1e-9); k = k + 0.5 * (a - c) / (a - 2 * b + c)
        fm = k * 44100 / len(x)
        print(f'{nm:11s} {midi:4d}  {fm:8.2f} ({1200 * np.log2(fm / f0):+5.2f})  {np.sqrt((x ** 2).mean()):6.0f}  |  {alias_db(x, f0):10.1f} {alias_db(xn, f0):7.1f}')
