"""CPU guard: osc 3 (setup CLARINET: 8' squares and a 16' triangle, one octave under voice 0's C3) must sound with 1-2 voices and fade out (no click)
from 4 voices. Measured on voice 0: the level of the sub-octave line (65.4 Hz) and the largest sample step."""
import os, subprocess, numpy as np
EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build', 'mm_poly')
def run(nv):
    r = subprocess.run([EXE, str(nv), '1.0', str(15 * 1024 + 512)], capture_output=True)
    if r.returncode: raise SystemExit(r.stderr.decode())
    return np.frombuffer(r.stdout, np.int16).astype(float), r.stderr.decode().strip()
ok = True
for nv in (1, 2, 3, 4, 6):
    x, flags = run(nv)
    seg = x[22050:]
    sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))); f = np.fft.rfftfreq(len(seg), 1 / 44100)
    sub = sp[(f > 60) & (f < 71)].max() / sp[(f > 125) & (f < 137)].max()
    jump = np.abs(np.diff(x)).max()
    expect_off = nv >= 4
    good = (sub < 0.05) == expect_off
    ok &= good
    print(f'{nv} voices: guard {flags}  sub-octave / fundamental {sub:5.2f}  max step {jump:5.0f}  {"ok" if good else "FAIL"}')
print('cpu guard', 'ok' if ok else 'FAIL')
raise SystemExit(0 if ok else 1)
