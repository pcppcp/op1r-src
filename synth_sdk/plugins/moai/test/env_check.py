"""Contour (filter envelope) timing, keyboard tracking, velocity and the contour knob, white box: reads the engine's
internal envelope and cutoff position."""
import subprocess, os, numpy as np
EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build', 'mm_env')
def env(midi, cut, amt, a, d, s, r, secs, rel, vel=127, macro=-1):
    out = subprocess.run([EXE, *map(str, (midi, cut, amt, a, d, s, r, secs, rel, vel, macro))], capture_output=True, text=True)
    if out.returncode: raise SystemExit(out.stderr)
    return np.array([[float(v) for v in l.split()] for l in out.stdout.splitlines()])
def t_to(a, level, after=0):
    i = np.where((a[:, 0] > after) & (a[:, 1] <= level))[0]
    return a[i[0], 0] if len(i) else float('nan')
def t_up(a, level):
    i = np.where(a[:, 1] >= level)[0]
    return a[i[0], 0] if len(i) else float('nan')
ms = lambda t: 4 * 1.0625 ** ((t * 115) >> 15)
print('decay knob -> time to 37 % (designed tau), release:')
for t in (0, 8000, 16000, 24000, 30000):
    a = env(60, 10000, 32767, 0, t, 0, t, 8.0, 4.0)
    rr = a[a[:, 0] > 4000]; r0 = rr[0, 1]
    tr = rr[np.where(rr[:, 1] <= r0 * 0.368)[0][0], 0] - rr[0, 0] if (rr[:, 1] <= r0 * 0.368).any() else float('nan')
    print(f'  {t:6d}: decay {t_to(a, 0.368, 3):7.0f} ms   release {tr:7.0f} ms   (tau {ms(t):6.0f} ms)')
print('attack knob -> time to 90 %:')
for t in (0, 12000, 20000, 28000):
    a = env(60, 10000, 32767, t, 16000, 32767, 16000, 4.0, 3.0)
    print(f'  {t:6d}: {t_up(a, 0.9):7.0f} ms')
print('sustain 50 %: level after 2 s =', round(env(60, 10000, 32767, 0, 8000, 16384, 8000, 3.0, 2.5)[:, 1][int(2.0 * 344)], 3))
print('cutoff position at note start vs key (cutoff 40 %, no contour, tracking 1/2):')
for m in (36, 60, 84):
    a = env(m, 13000, 0, 0, 10000, 0, 10000, 0.1, 1.0)
    print(f'  midi {m}: {20 * 2 ** a[-1, 2]:7.1f} Hz  (expected {20 * 2 ** (13000 * 10 / 32767) * 2 ** ((m - 60) / 12 / 2):7.1f})')
print('velocity scaling of the contour depth (amount 100 %), peak in octaves at 127 / 64 / 20:')
for v in (127, 64, 20):
    a = env(60, 6553, 32767, 0, 30000, 32767, 30000, 0.5, 1.0, v)
    print(f'  vel {v:3d}: +{a[:, 2].max() - 6553 * 10 / 32767:.2f} oct')
print('contour knob (one knob): amount / decay / sustain / attack it sets, and the envelope it gives:')
for m in (0, 6000, 12000, 20000, 26000, 32767):
    a = env(60, 6553, 0, 0, 0, 0, 0, 3.0, 2.5, 127, m)
    peak = a[:, 2].max() - 6553 * 10 / 32767
    print(f'  {m:6d}: depth +{peak:4.1f} oct   to 37 % {t_to(a, 0.368, 3):6.0f} ms   level at 2 s {a[int(2.0 * 344), 1]:4.2f}   to 90 % {t_up(a, 0.9):5.0f} ms')
