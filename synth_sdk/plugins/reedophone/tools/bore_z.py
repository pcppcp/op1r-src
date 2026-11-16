"""Input impedance of the prototype's discrete bore (same delays, end filter, vent scattering as proto.c)."""
import sys, numpy as np, matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
SR = 44100
def zin(midi, fc=1800, loss=0.01, loss_k=0.5, vy=0.3, vlp=500, va=0.25, reg2=None, f=None):
    f0 = 440 * 2 ** ((midi - 69) / 12); reg2 = midi >= 69.5 if reg2 is None else reg2
    a = np.exp(-2 * np.pi * fc / SR)
    f = np.linspace(20, 8000, 8000) if f is None else f
    z = np.exp(1j * 2 * np.pi * f / SR)
    LP = (1 - a) / (1 - a * z ** -1)
    w0 = 2 * np.pi * f0 / SR; LP0 = (1 - a) / (1 - a * np.exp(-1j * w0)); tau = -np.angle(LP0) / w0
    g = 1 - loss * (f0 / 261.6) ** loss_k
    Rend = -g * LP
    rt = (1.5 if reg2 else 0.5) * SR / f0 - tau
    if not reg2:
        Lb = (rt - 1) / 2; Rin = z ** -(2 * Lb + 1) * Rend
    else:
        La = va * SR / f0 - 1; Lb = (rt - 2) / 2 - La
        RB = z ** -(2 * Lb + 1) * Rend
        if vlp > 0:
            av = np.exp(-2 * np.pi * vlp / SR); Y = vy * (1 - av) / (1 - av * z ** -1)
        else: Y = vy
        P = 2 / ((2 + Y) - 2 * RB / (1 + RB)); RJ = P - 1
        Rin = z ** -(2 * La + 1) * RJ
    Z = (1 + Rin) / (1 - Rin)
    return f, Z, f0
if __name__ == '__main__':
    midis = [int(m) for m in sys.argv[1].split(',')]; kw = dict(p.split('=') for p in sys.argv[2:]); kw = {k: float(v) for k, v in kw.items()}
    fig, ax = plt.subplots(len(midis), 1, figsize=(16, 3 * len(midis)))
    for a, m in zip(np.atleast_1d(ax), midis):
        f, Z, f0 = zin(m, **kw)
        a.semilogy(f / f0, np.abs(Z)); a.set_xlim(0, 8000 / f0); a.grid(); a.set_title(f'midi {m}  |Zin| vs f/f0  {kw}')
        for k in range(1, 12): a.axvline(k, color='r', alpha=0.3)
    plt.tight_layout(); plt.savefig('../analysis/bore_z.png', dpi=50)
