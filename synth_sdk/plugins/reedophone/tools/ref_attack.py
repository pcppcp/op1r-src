import sys, numpy as np, matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from clar_common import *
ns = [int(a) for a in sys.argv[1:]] or [1, 28, 48, 10, 39, 64]
fig, ax = plt.subplots(len(ns), 3, figsize=(20, 3 * len(ns)))
for row, n in enumerate(ns):
    x = load(ref_path(n))
    f0 = f0_yin(x[SR:SR+4096], 130, 1900); P = SR / f0
    on = np.argmax(np.abs(x) > 0.01 * np.abs(x).max())
    a = ax[row, 0]; xp = np.concatenate([np.zeros(500), x]); a.plot(np.arange(-500, 6000) / SR * 1000, xp[on:on+6500]); a.set_title(f'#{n} onset waveform (ms), f0 {f0:.0f}')
    # harmonic trajectories via short-time projection on k*f0 over 2-period windows
    W = int(4 * P); hop = int(P / 2); t = []; H = []
    for s in range(max(0, on - 2000), on + int(0.4 * SR), hop):
        seg = x[s:s + W] * np.hanning(W); tt = np.arange(W) / SR
        H.append([2*abs(np.sum(seg * np.exp(-2j*np.pi*k*f0*tt))) / np.sum(np.hanning(W)) for k in range(1, 8)])
        t.append((s + W/2 - on) / SR * 1000)
    H = 20*np.log10(np.array(H) + 1e-7)
    a = ax[row, 1]
    for k in range(7): a.plot(t, H[:, k], label=f'H{k+1}')
    a.set_ylim(-90, 0); a.legend(fontsize=7, ncol=7); a.set_title('harmonic amplitudes during onset (ms)')
    # pitch over whole note + RMS fluctuations
    hop2 = 1024; fs = []; ts = []
    for s in range(on + 4096, len(x) - 8192, hop2):
        seg = x[s:s + 4096]
        if np.sqrt(np.mean(seg**2)) < 0.01: break
        fs.append(1200*np.log2(f0_yin(seg, f0*0.7, f0*1.4) / f0)); ts.append(s / SR)
    a = ax[row, 2]; a.plot(ts, fs); a.set_title('pitch deviation (cents)'); a.set_ylim(-40, 40)
plt.tight_layout(); out = '../analysis/attack_' + '_'.join(map(str, ns)) + '.png'; plt.savefig(out, dpi=55); print(out)
