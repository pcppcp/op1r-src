import numpy as np
from clar_common import *
np.set_printoptions(linewidth=250)
print("n  midi  " + " ".join(f"H{k:<3d}" for k in range(1, 13)) + "  cent(Hz) HNR")
for n in list(range(1, 21)) + list(range(48, 71)):
    x = load(ref_path(n)); st = int(1.0 * SR); seg = x[st:st + 32768]
    f0 = f0_yin(seg[:4096], 130, 1900)
    h = harmonics(seg, f0, 40); hl = 10 ** (h / 20)
    ks = np.arange(1, 41); ok = ~np.isnan(h)
    cent = np.sum(ks[ok] * f0 * hl[ok] ** 2) / np.sum(hl[ok] ** 2)
    # noise: energy between harmonics in 1-5 kHz relative to harmonics
    X = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2; fr = np.fft.rfftfreq(len(seg), 1 / SR)
    ph = (fr / f0) % 1; band = (fr > 500) & (fr < 6000)
    harm = band & ((ph < 0.1) | (ph > 0.9)); mid = band & (ph > 0.35) & (ph < 0.65)
    hnr = 10 * np.log10(X[harm].mean() / X[mid].mean())
    print(f"{n:02d} {midi_of(f0):5.1f} " + " ".join(f"{v - h[0]:4.0f}" for v in h[:12]) + f"  {cent:6.0f}  {hnr:4.1f}")
