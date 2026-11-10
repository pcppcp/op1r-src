"""Random knobs, session parameters (as the views set them, modulation too), pitches up to the top key / velocities /
pitch bends through the sanitizer build: no UB, no crash, no clipping, no DC. Exits 1 on a bad render."""
import random, numpy as np
from stats import render
random.seed(7)
bad = 0; worst = 0
# the top keys with osc 3 in LO as the source (square, saw) and the pitch / filter modulation at full, bent both ways
EDGE = [(m, f'15:1,16:{wv},27:{mx},28:32767,29:32767', b) for m in (120, 127) for wv in (2, 4) for mx in (0, 32767)
        for b in (0, 1200, -1200)]
CASES = [(m, [1536, 20000, 0, 0], [0, 0, 0], 127, b, ov) for m, ov, b in EDGE]
for t in range(300):
    midi = random.choice([12, 24, 36, 48, 60, 72, 84, 96, 108, 120, 127]) + (random.random() if t % 4 else 0)
    k = [random.choice([0, 32767, random.randint(0, 32767)]) for _ in range(4)]
    w = [random.choice([0, 32767, random.randint(0, 32767)]) for _ in range(3)]
    vel = random.choice([1, 64, 127]); bend = random.choice([0, 0, 1200, -1200])
    ov = ','.join(f'{i}:{random.choice([0, 32767, random.randint(-900, 32767)])}' for i in random.sample(range(7, 30), 7))
    CASES.append((min(midi, 127), k, w, vel, bend, ov))
for midi, k, w, vel, bend, ov in CASES:
    x = render([midi, *k, *w, 0.25, 0.15, vel, bend, ov])
    pk = np.abs(x).max(); worst = max(worst, pk)
    dc = abs(x[2000:].mean())
    if pk >= 32767 or dc > 3000:
        bad += 1; print('BAD', round(midi, 1), k, w, vel, bend, ov, 'peak', pk, 'dc', round(dc))
print('fuzz done,', len(CASES), 'renders, bad', bad, 'worst peak', worst)
raise SystemExit(1 if bad else 0)
