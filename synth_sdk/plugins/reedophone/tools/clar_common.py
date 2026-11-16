"""Shared analysis helpers for comparing clarinet synthesis against the Crisis clarinet samples."""
import numpy as np, soundfile as sf, re, os

SR = 44100
REF_DIR = r"D:/Sounds/sfzs/Crisis/samples"
SFZ = r"D:/Sounds/sfzs/Crisis/064-071 Reed/CrisisClarinet.sfz"

def load(path):
    x, sr = sf.read(path, dtype='float64')
    if x.ndim > 1: x = x.mean(1)
    assert sr == SR, (path, sr)
    return x

def ref_path(n): return os.path.join(REF_DIR, f"CrsClarinet{n:02d}.wav")

def sfz_regions():
    txt = open(SFZ).read()
    regs = []
    for blk in txt.split('<region>')[1:]:
        d = dict(re.findall(r'(\w+)=([^\s]+)', blk))
        m = re.search(r'CrsClarinet(\d+)', d['sample'])
        regs.append(dict(n=int(m.group(1)), key=int(d['pitch_keycenter']), lokey=int(d['lokey']),
                         hikey=int(d['hikey']), lovel=int(d['lovel']), hivel=int(d['hivel']),
                         offset=int(d.get('offset', 0))))
    return regs

def f0_yin(x, fmin=100, fmax=2000, thresh=0.12):
    """YIN f0 over one frame (with parabolic refinement)."""
    N = len(x); tmax = int(SR / fmin); tmin = int(SR / fmax)
    W = N - tmax
    d = np.zeros(tmax + 1)
    for t in range(1, tmax + 1):
        e = x[:W] - x[t:t + W]; d[t] = np.dot(e, e)
    cm = d.copy(); cm[0] = 1
    cm[1:] = d[1:] * np.arange(1, tmax + 1) / np.maximum(np.cumsum(d[1:]), 1e-20)
    t = tmin
    while t < tmax:
        if cm[t] < thresh:
            while t + 1 < tmax and cm[t + 1] < cm[t]: t += 1
            break
        t += 1
    else:
        t = tmin + int(np.argmin(cm[tmin:tmax]))
    if 1 <= t < tmax:
        a, b, c = cm[t - 1], cm[t], cm[t + 1]
        den = a - 2 * b + c
        if den != 0: t = t + 0.5 * (a - c) / den
    return SR / t

def midi_of(f): return 69 + 12 * np.log2(f / 440.0)
def hz_of(m): return 440.0 * 2 ** ((m - 69) / 12.0)

def rms_env(x, hop=256, win=1024):
    n = max(1, (len(x) - win) // hop + 1)
    return np.array([np.sqrt(np.mean(x[i*hop:i*hop+win] ** 2)) for i in range(n)])

def harmonics(x, f0, nh=30, nfft=None):
    """Harmonic amplitudes (dB re. the strongest) via a Hann-windowed FFT peak pick at k*f0."""
    N = len(x); w = np.hanning(N)
    nfft = nfft or 1 << int(np.ceil(np.log2(N * 4)))
    X = np.abs(np.fft.rfft(x * w, nfft)) / (np.sum(w) / 2)
    fr = SR / nfft; out = []
    for k in range(1, nh + 1):
        fk = k * f0
        if fk > SR / 2 - 200: out.append(np.nan); continue
        c = int(round(fk / fr)); r = max(2, int(0.25 * f0 / fr))
        out.append(X[max(0, c - r):c + r + 1].max())
    out = np.array(out)
    return 20 * np.log10(np.maximum(out, 1e-9))
