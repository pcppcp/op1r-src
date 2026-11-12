#!/usr/bin/env python3
"""Compare the fixed-point Clouds port (plugins/clouds/clouds_dsp.c) with the original Mutable Instruments code.

  python3 harness/clouds_compare.py            # builds both, runs all scenarios, writes harness/out/

Both run at 32 kHz, mono, 32-sample blocks, on the same test signal (plucked melody + chirp + noise bursts)
with identical, Q15-representable parameters. Metrics per scenario:
  SNR   sample-wise signal-to-error ratio (granular grains are seeded by a shared RNG sequence, so even the
        stochastic modes are expected to match closely unless a seeding decision flips)
  ENV   correlation of the 10 ms RMS envelopes
  LSD   log-spectral distance (dB, STFT 1024, frames with signal, bins floored at -60 dB)
Pass: SNR >= 25 dB, or ENV >= 0.97 and LSD <= 2.5 dB.
Built with -DCLOUDS_EMULATE_F32_DELAY: reproduces the original's float32 stall of the looping-delay glide
(~20 samples short of the target), which the OP-1 build intentionally does not.
CLOUDS_FLAGS="..." adds compile flags to the port (e.g. the OP-1 speed options).
Needs the pyenv python (numpy, matplotlib).
"""
import os, subprocess, sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(HERE)
SRC = os.path.join(SDK, 'third_party', 'eurorack')
BUILD = os.path.join(HERE, 'build')
OUT = os.path.join(HERE, 'out')
SR = 32000

# (name, mode, position, size, pitch, density, texture, dry_wet, feedback, reverb, freeze_period, freeze_on)
SCENARIOS = [
    ('granular default', 'g', .5, .5, 0, .75, .5, 1, 0, 0, 0, 0),
    ('granular small dense', 'g', .1, .1, 0, .95, .2, 1, 0, 0, 0, 0),
    ('granular +12 diffuse', 'g', .3, .4, 12, .65, .9, 1, 0, 0, 0, 0),
    ('granular -12 large', 'g', .6, .9, -12, .8, .6, 1, 0, 0, 0, 0),
    ('granular deterministic', 'g', .4, .3, 0, .2, .5, 1, 0, 0, 0, 0),
    ('granular freeze', 'g', .5, .5, 0, .75, .5, 1, 0, 0, 1024, 512),
    ('granular fb + reverb', 'g', .5, .5, 0, .75, .5, .8, .5, .5, 0, 0),
    ('looping delay', 'l', .3, .5, 0, .2, .5, 1, 0, 0, 0, 0),
    ('looping freeze +7', 'l', .4, .3, 7, .3, .5, 1, 0, 0, 1024, 400),
    ('looping LP fb rev', 'l', .3, .5, 0, .5, .2, .7, .5, .4, 0, 0),
    ('looping HP', 'l', .3, .5, -5, .5, .9, 1, .2, 0, 0, 0),
]


EXTRA = ['-DCLOUDS_ORIGINAL_DRY'] + os.environ.get('CLOUDS_FLAGS', '').split()   # CLOUDS_FLAGS e.g. "-DCLOUDS_LINEAR_READ"; the
# original's dry gain: the port's dry end is unity on purpose (op1r-src PR #40)


def build():
    os.makedirs(BUILD, exist_ok=True)
    ref = os.path.join(BUILD, 'clouds_ref')
    srcs = ['clouds/dsp/granular_processor.cc', 'clouds/dsp/correlator.cc', 'clouds/dsp/mu_law.cc',
            'clouds/resources.cc', 'clouds/dsp/pvoc/phase_vocoder.cc', 'clouds/dsp/pvoc/stft.cc',
            'clouds/dsp/pvoc/frame_transformation.cc', 'stmlib/utils/random.cc', 'stmlib/dsp/units.cc',
            'stmlib/dsp/atan.cc']
    subprocess.run(['g++', '-O2', '-DTEST', '-w', '-I', SRC, os.path.join(SDK, 'clouds_ref', 'clouds_ref.cc')] +
                   [os.path.join(SRC, s) for s in srcs] + ['-o', ref], check=True)
    port = os.path.join(BUILD, 'clouds_port')
    subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', '-DCLOUDS_SR=32000', '-DCLOUDS_EMULATE_F32_DELAY', '-fsanitize=address,undefined'] + EXTRA + [
                    os.path.join(HERE, 'clouds_port_run.c'), os.path.join(SDK, 'plugins', 'clouds', 'clouds_dsp.c'), '-o', port],
                   check=True)
    return ref, port


def test_signal(seconds=8.0):
    """plucked melody (Karplus-Strong) + chirp + noise bursts, int16"""
    rng = np.random.default_rng(1)
    n = int(seconds * SR)
    x = np.zeros(n)
    notes = [220, 277, 330, 440, 392, 330, 294, 247, 196, 262, 349, 523]
    t0 = 0
    for f in notes:
        L = int(SR / f)
        buf = rng.uniform(-1, 1, L)
        dur = int(0.45 * SR)
        y = np.zeros(dur)
        for i in range(dur):
            y[i] = buf[i % L]
            buf[i % L] = 0.996 * 0.5 * (buf[i % L] + buf[(i + 1) % L])
        x[t0:t0 + dur] += y[:max(0, min(dur, n - t0))]
        t0 += int(0.5 * SR)
    t = np.arange(int(1.5 * SR)) / SR
    chirp = 0.4 * np.sin(2 * np.pi * (100 * t + 0.5 * 2000 * t * t / 1.5))
    x[t0:t0 + len(chirp)] += chirp
    nb = int(0.1 * SR)
    for k in range(4):
        s = t0 + len(chirp) + k * int(0.25 * SR)
        if s + nb < n:
            x[s:s + nb] += 0.3 * rng.uniform(-1, 1, nb) * np.hanning(nb)
    x = x / max(1e-9, np.abs(x).max()) * 0.6
    return (x * 32767).astype(np.int16)


def qparam(v):
    return repr(round(float(v) * 32768) / 32768)


def run(exe, sc, inp, out):
    name, mode, pos, size, pitch, dens, tex, dw, fb, rev, fp, fo = sc
    args = [mode, inp, out] + [qparam(pos), qparam(size), str(pitch / 1.0)] + \
           [qparam(v) for v in (dens, tex, dw, fb, rev)] + [str(fp), str(fo)]
    subprocess.run([exe] + args, check=True)
    return np.fromfile(out, np.int16).astype(np.float64)


def env(x, w=320):
    m = len(x) // w
    return np.sqrt((x[:m * w].reshape(m, w) ** 2).mean(1) + 1e-9)


def stft_db(x, n=1024, hop=256):
    win = np.hanning(n)
    frames = [np.abs(np.fft.rfft(win * x[i:i + n])) for i in range(0, len(x) - n, hop)]
    return 20 * np.log10(np.array(frames).T + 1e-3)


def metrics(r, p):
    snr = 10 * np.log10((r ** 2).sum() / max(1e-9, ((r - p) ** 2).sum()))
    er, ep = env(r), env(p)
    envc = np.corrcoef(er, ep)[0, 1] if er.std() > 0 and ep.std() > 0 else 1.0
    R, P = stft_db(r), stft_db(p)
    floor = R.max() - 60                                                  # ignore bins below -60 dB
    R, P = np.maximum(R, floor), np.maximum(P, floor)
    act = R.max(0) > R.max() - 40                                         # frames with signal
    lsd = float(np.sqrt(((R[:, act] - P[:, act]) ** 2).mean(0)).mean()) if act.any() else 0.0
    ok = snr >= 25 or (envc >= 0.97 and lsd <= 2.5)
    return snr, envc, lsd, ok, R, P


def main():
    ref, port = build()
    os.makedirs(OUT, exist_ok=True)
    x = test_signal()
    inp = os.path.join(OUT, 'input.raw')
    x.tofile(inp)
    rows, fails = [], 0
    fig, ax = plt.subplots(len(SCENARIOS), 3, figsize=(15, 2.1 * len(SCENARIOS)))
    for k, sc in enumerate(SCENARIOS):
        r = run(ref, sc, inp, os.path.join(OUT, 'ref.raw'))
        p = run(port, sc, inp, os.path.join(OUT, 'port.raw'))
        snr, envc, lsd, ok, R, P = metrics(r, p)
        fails += not ok
        rows.append((sc[0], snr, envc, lsd, ok))
        print(f'{sc[0]:24s} SNR {snr:6.1f} dB   ENV {envc:6.4f}   LSD {lsd:5.2f} dB   {"PASS" if ok else "FAIL"}')
        tt = np.arange(len(env(r))) * 0.01
        ax[k, 0].plot(tt, env(r), lw=0.8, label='original')
        ax[k, 0].plot(tt, env(p), lw=0.8, ls='--', label='port')
        ax[k, 0].set_title(f'{sc[0]}   SNR {snr:.1f} dB  ENV {envc:.3f}  LSD {lsd:.2f} dB', fontsize=8, loc='left')
        ax[k, 0].legend(fontsize=6, loc='upper right')
        for j, (S, nm) in enumerate(((R, 'original'), (P, 'port'))):
            ax[k, j + 1].imshow(S[:256], origin='lower', aspect='auto', vmin=S.max() - 80, vmax=S.max(), cmap='magma')
            ax[k, j + 1].set_title(nm + ' (0-8 kHz)', fontsize=8)
            ax[k, j + 1].set_xticks([]); ax[k, j + 1].set_yticks([])
    fig.tight_layout()
    png = os.path.join(OUT, 'clouds_compare.png')
    fig.savefig(png, dpi=70)
    print(f'\n{len(SCENARIOS) - fails}/{len(SCENARIOS)} scenarios pass -> {png}')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
