import sys, numpy as np, matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from clar_common import *
ns = [int(a) for a in sys.argv[1:]] or [1, 5, 28, 10, 16, 48]
fig, ax = plt.subplots(len(ns), 4, figsize=(20, 3.2 * len(ns)))
for row, n in enumerate(ns):
    x = load(ref_path(n)); L = len(x)
    st = int(1.0 * SR) if L > 2*SR else L // 3
    f0 = f0_yin(x[st:st+4096], 130, 1900)
    P = SR / f0
    a = ax[row, 0]; seg = x[st:st + int(3 * P)]; a.plot(seg); a.set_title(f"#{n} f0={f0:.1f} midi={midi_of(f0):.2f}")
    a = ax[row, 1]; e = rms_env(x, 256, 512); a.plot(np.arange(len(e)) * 256 / SR, 20*np.log10(e+1e-9)); a.set_ylim(-80, 0); a.set_title('RMS dB')
    a = ax[row, 2]; h = harmonics(x[st:st + 16384], f0, 30); h -= np.nanmax(h); a.bar(np.arange(1, 31), h + 80, bottom=-80); a.set_ylim(-80, 0); a.set_title('harmonics dB (sustain)')
    a = ax[row, 3]; seg = x[:int(0.6 * SR)]
    a.specgram(seg, NFFT=1024, Fs=SR, noverlap=896, vmin=-140); a.set_ylim(0, 10000); a.set_title('onset spectrogram')
plt.tight_layout(); out = '../analysis/ref_' + '_'.join(map(str, ns)) + '.png'; plt.savefig(out, dpi=60); print(out)
