"""CPU guard (moai.h): with many voices sounding the transistor stages rest (from 4), then the sound's quietest
oscillator (from 5), for every voice, each coming back two voices lower; and a note on a voice that is still heard
goes on without a step. Measured on voice 0 (C3): spectral lines of its oscillators and the largest sample step."""
import os, subprocess, numpy as np
EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build', 'mm_poly')
FS = 44100
W = lambda setup: setup * 1024 + 512
CLARINET, SUB_BASS, ORGAN = W(15), W(2), W(22)
def run(nv, secs, word, *more):
    r = subprocess.run([EXE, str(nv), str(secs), str(word), *more], capture_output=True)
    if r.returncode: raise SystemExit(r.stderr.decode() or 'mm_poly failed')
    steps, tr = r.stderr.decode().split(' tr ')
    return np.frombuffer(r.stdout, np.int16).astype(float), steps.strip(), int(tr)
def line(x, t0, t1, hz):                 # the level of the spectral line at hz (+-6 %) between t0 and t1 s
    seg = x[int(t0 * FS):int(t1 * FS)]
    sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))); f = np.fft.rfftfreq(len(seg), 1 / FS)
    return sp[(f > hz * 0.94) & (f < hz * 1.06)].max()
def step(x, t0, t1): return np.abs(np.diff(x[int(t0 * FS):int(t1 * FS)])).max()
ok = True
def check(good, text):
    global ok
    ok &= bool(good)
    print(f'{"ok  " if good else "FAIL"} {text}')

# 1. steady: CLARINET (8' squares, a 16' triangle at the lowest volume: the one to rest, an octave under C3)
for nv in (1, 2, 3, 4, 5, 6):
    x, steps, _ = run(nv, 1.0, CLARINET)
    sub = line(x, 0.5, 1.0, 65.4) / line(x, 0.5, 1.0, 130.8)
    want = 0 if nv < 4 else (1 if nv < 5 else 2)
    check(set(steps) == {str(want)} and (sub < 0.05) == (want == 2),
          f'{nv} voices: guard step {steps[-1]} (want {want}), 16\' line / 8\' line {sub:4.2f}')

# 2. which oscillator: SUB BASS (16' triangle, 16' square at half volume, 32' triangle): the square rests, not osc 3
x1, _, _ = run(1, 1.0, SUB_BASS); x6, _, _ = run(6, 1.0, SUB_BASS)
s1, s6 = (line(x, 0.5, 1.0, 32.7) / line(x, 0.5, 1.0, 65.4) for x in (x1, x6))
h1, h6 = (line(x, 0.5, 1.0, 457.8) / line(x, 0.5, 1.0, 65.4) for x in (x1, x6))     # the 16' 7th harmonic: the square's
check(s6 > s1 and h6 < 0.5 * h1, f'SUB BASS, 6 voices: the 32\' sub stays ({s6:4.2f} of the 16\' line, alone {s1:4.2f}), the quieter '
      f'square rests (7th harmonic {h6:5.3f}, alone {h1:5.3f})')

# 3. transitions, ORGAN (three triangles: any click shows): 3 voices, 5 from 0.5 s, 2 from 1.0 s
x, steps, _ = run(3, 2.0, ORGAN, '0.5+3', '0.5+4', '1.0-2', '1.0-3', '1.0-4')
top = [line(x, a, b, 261.6) / line(x, a, b, 130.8) for a, b in ((0.2, 0.45), (0.7, 0.95), (1.4, 1.9))]
quiet = step(x, 0.1, 0.45)
check(steps[1:4] == '000' and steps[7:10] == '222' and steps[-4:] == '0000' and top[1] < 0.05 and top[0] > 0.1 and top[2] > 0.1,
      f'3 -> 5 -> 2 voices: guard {steps}, osc 3 (4\') {top[0]:4.2f} -> {top[1]:4.2f} -> {top[2]:4.2f}')
worst = max(step(x, 0.48, 0.65), step(x, 0.98, 1.15))
check(worst < 1.25 * quiet, f'... without a click: largest step in the fades {worst:.0f}, steady {quiet:.0f}')
x, steps, _ = run(6, 1.6, ORGAN, '0.4-5', '0.4-4', '0.8-3', '1.2-2')
check(steps[1:4] == '222' and steps[5:8] == '222' and steps[9:12] == '111' and steps[-3:] == '000',
      f'6 -> 4 -> 3 -> 2 voices: guard {steps} (each step ends two voices under where it starts)')

# 4. the transistor stages (drive 75 %, cutoff low): on with 3 voices, resting from 4
(_, s3, tr3), (_, s4, tr4) = run(3, 0.5, W(1), 'cut=9000', 'drive=24576'), run(4, 0.5, W(1), 'cut=9000', 'drive=24576')
check(tr3 == 1 and tr4 == 0, f'driven, 3 voices: transistor stages {tr3} (guard {s3[-1]}); 4 voices: {tr4} (guard {s4[-1]})')

# 5. a new note on a voice: from silence it starts anew; while the OP-1 envelope is open it goes on, no step
x, _, _ = run(1, 1.0, ORGAN, '0.3h1', '0.5n0')
quiet, at = step(x, 0.1, 0.45), step(x, 0.499, 0.52)
check(at < 1.25 * quiet, f'retrigger while heard: largest step {at:.0f} (steady {quiet:.0f})')
anew = []                                # (the new phases are random: one start can land on a small step; op1r's port draws
for t0 in (0.3, 0.4, 0.5, 0.6, 0.7):     # them differently from upstream, so several starts, most must show it)
    x, _, _ = run(1, 1.0, ORGAN, f'{t0}n0')
    i = int(t0 * FS) // 128 * 128
    anew.append(abs(x[i] - x[i - 1]) > 2 * quiet)
check(sum(anew) >= 4, f'a note from silence starts anew (under a closed envelope): {sum(anew)} of 5 starts step')
print('cpu guard', 'ok' if ok else 'FAIL')
raise SystemExit(0 if ok else 1)
