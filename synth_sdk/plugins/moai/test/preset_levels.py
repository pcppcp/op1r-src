"""The library presets are heard: each one on the knob, at velocity 110 (basses and sequences at C3, leads and arps at
C4, the rest at F#3), reaches its floor in dB(A) (gen_presets.py: LIFT). FX, percussion and Pure Sub are what they are."""
import re, os, numpy as np
from concurrent.futures import ThreadPoolExecutor
from stats import render
HERE = os.path.dirname(os.path.abspath(__file__))
SR = 44100
ROUND = re.compile(r'Round|Rounder|Muted|Sub\b|Dub')
def pct(v): return v * 32767 // 100
def word(p): return (p % 31) * 1024 + 1013 + 2 * (p // 31)
def dba(x):
    S = np.abs(np.fft.rfft(x)) ** 2; f = np.fft.rfftfreq(len(x), 1 / SR) + 1e-9; f2 = f * f
    ra = 12194**2 * f2**2 / ((f2 + 20.6**2) * np.sqrt((f2 + 107.7**2) * (f2 + 737.9**2)) * (f2 + 12194**2))
    return 10 * np.log10((S * ra**2).sum() * 2 / len(x) ** 2 + 1e-20) + 2.0 - 20 * np.log10(32768)
h = open(os.path.join(HERE, '..', 'moai_presets.h')).read()
names = {}
jobs = []
for p, m in enumerate(re.finditer(r"\{ \{ .*? \}, ([\d, ]+) \},\s+/\* (.*?) \*/", h)):
    k = [int(x) for x in m[1].split(',')]; c = re.match(r"(\d{3}): CSV (\d+), (.*?), (.*)", m[2])
    if not c: continue                                     # made here (matched to records)
    cat = c[3].lower(); num = int(c[1]); names[num] = c[4]
    if cat.startswith(('bass', 'sequence')): midi, floor = 48, -27.5
    elif cat.startswith(('lead', 'arp')): midi, floor = 60, -21.0
    elif cat.startswith('drone'): midi, floor = 54, -27.5
    else: continue
    if int(c[2]) == 41: continue                           # Pure Sub
    if ROUND.search(names.get(num, '')): floor = -31.5
    jobs.append((num, p, k, midi, floor))
def one(j):
    num, p, k, midi, floor = j
    x = render([midi, word(p), pct(k[0]), pct(k[1]), pct(k[2]), pct(k[3]), max(1, pct(k[4])), 1, 0.6, 0.4, 110, 0])
    return num, dba(x[:int(0.4 * SR)]), floor
with ThreadPoolExecutor(8) as ex: res = list(ex.map(one, jobs))
bad = [r for r in res if r[1] < r[2]]
for num, a, fl in sorted(res, key=lambda r: r[1])[:8]:
    print(f"  {num:03d} {names.get(num, '')[:28]:28s} {a:6.1f} dB(A) (floor {fl})")
print(f"presets heard: {len(res) - len(bad)} of {len(res)} at their floor", 'ok' if not bad else 'FAIL')
for num, a, fl in bad: print(f"FAIL {num:03d} {names.get(num, '')} {a:.1f} < {fl}")
raise SystemExit(1 if bad else 0)
