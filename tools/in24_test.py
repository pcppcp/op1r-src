#!/usr/bin/env python3
"""24-bit input test (docs/29): play test signals from this computer into the OP-1's input and record its USB audio
(6 channels, 24 bit) at the same time. Needs the test firmware built with OP1R_USB_RAWIN=1: USB channels 3 / 4 carry
the codec's raw input words (top 24 bits), 1 / 2 the main mix (the stock 16-bit path, input monitor).

    in24_test.py run [--out /tmp/in24.wav]      play + record, then analyse
    in24_test.py analyse /tmp/in24.wav          analyse a recording again
    in24_test.py --self-test

Segments (3 s each, 1 kHz): silence, -20, -60, -80, -100 dBFS, then a rise -30 .. 0 dBFS (clip point).
Per segment and channel: RMS, the 1 kHz level (FFT), the noise floor around it, and how many of the low 8 bits of the
24-bit words change (0: only 16 bits arrive)."""
import argparse, subprocess, sys, time, wave
import numpy as np

RATE, SEG = 44100, 3.0
LEVELS = [None, -20, -60, -80, -100]                  # None: silence

def signal():
    t = np.arange(int(RATE * SEG)) / RATE
    parts = [np.zeros_like(t) if lv is None else 10 ** (lv / 20) * np.sin(2 * np.pi * 1000 * t) for lv in LEVELS]
    tr = np.arange(int(RATE * 6.0)) / RATE                            # the rise: -30 .. 0 dBFS over 6 s
    parts.append(10 ** ((-30 + 30 * tr / tr[-1]) / 20) * np.sin(2 * np.pi * 1000 * tr))
    x = np.concatenate([np.zeros(int(RATE * 1.0))] + parts + [np.zeros(int(RATE * 1.0))])
    return (np.round(x * 8388607)).astype(np.int32)                   # 24-bit

def write_wav(path, x, ch, width):
    with wave.open(path, 'wb') as w:
        w.setnchannels(ch); w.setsampwidth(width); w.setframerate(RATE)
        b = x.astype('<i4').tobytes()
        if width == 3: b = b''.join(b[i:i + 3] for i in range(0, len(b), 4))
        w.writeframes(b)

def read_wav(path):
    with wave.open(path, 'rb') as w:
        ch, wd, n = w.getnchannels(), w.getsampwidth(), w.getnframes(); raw = w.readframes(n)
    a = np.frombuffer(raw, np.uint8).reshape(-1, wd)
    v = np.zeros(len(a), np.int32)
    for i in range(wd): v |= a[:, i].astype(np.int32) << (8 * i + (32 - 8 * wd))
    return (v >> (32 - 8 * wd)).reshape(-1, ch), wd

def seg_stats(x):
    """x: int 24-bit samples -> rms dBFS, 1 kHz dBFS, noise floor dBFS/bin around it, active low bits (of 8)"""
    f = x.astype(np.float64) / 8388608
    rms = 20 * np.log10(np.sqrt(np.mean(f ** 2)) + 1e-12)
    n = len(f); win = np.hanning(n); sp = np.abs(np.fft.rfft(f * win)) / (np.sum(win) / 2)
    k = int(round(1000 * n / RATE))
    tone = 20 * np.log10(np.sqrt(np.sum(sp[k - 3:k + 4] ** 2)) + 1e-12)
    band = np.r_[sp[k - 400:k - 20], sp[k + 20:k + 400]]
    floor = 20 * np.log10(np.median(band) + 1e-15)
    low = int(np.bitwise_or.reduce((x & 0xFF).astype(np.int64)) != 0) and bin(int(np.bitwise_or.reduce(x & 0xFF))).count('1')
    return rms, tone, floor, low

def find_start(rec):
    """the recording's first sample of the -20 dBFS segment: the first block well above the silence"""
    m = np.abs(rec).max(axis=1).astype(np.float64)
    b = 441; e = np.array([m[i:i + b].max() for i in range(0, len(m) - b, b)])
    thr = max(e[:20].max() * 8, 8388608 * 10 ** (-40 / 20))
    i = int(np.argmax(e > thr)); return i * b

def analyse(path):
    rec, wd = read_wav(path)
    print(f'{path}: {rec.shape[1]} channels, {wd * 8} bit, {len(rec) / RATE:.1f} s')
    s20 = find_start(rec)
    starts = [s20 - int(RATE * SEG)] + [s20 + i * int(RATE * SEG) for i in range(len(LEVELS) - 1)]
    names = ['silence'] + [f'{lv} dBFS' for lv in LEVELS[1:]]
    cut = int(0.3 * RATE)                                              # skip edges
    for ch, label in ((2, 'raw L (ch3)'), (3, 'raw R (ch4)'), (0, 'mix L (ch1)'), (1, 'mix R (ch2)')):
        if ch >= rec.shape[1]: continue
        print(f'\n{label}:   rms     1 kHz   floor/bin  low bits active')
        for nm, s in zip(names, starts):
            if s < 0: continue
            r, t, fl, lb = seg_stats(rec[s + cut:s + int(RATE * SEG) - cut, ch])
            print(f'  {nm:10s} {r:7.1f} {t:8.1f} {fl:9.1f}    {lb}')
        rise = rec[starts[-1] + int(RATE * SEG):starts[-1] + int(RATE * SEG) + int(RATE * 6.0), ch].astype(np.int64)
        if len(rise):
            pk = np.abs(rise).max(); full = (np.abs(rise) >= pk - 2).mean()
            print(f'  rise: peak {20 * np.log10(pk / 8388608 + 1e-12):6.1f} dBFS of 24-bit, at the peak {100 * full:.1f} % of samples (flat = clipping)')

def run(out):
    play = '/tmp/in24_play.wav'
    write_wav(play, np.repeat(signal()[:, None], 2, axis=1).reshape(-1), 2, 3)
    dur = int(len(signal()) / RATE) + 3
    rec = subprocess.Popen(['arecord', '-q', '-D', 'hw:OP1,0', '-c', '6', '-f', 'S24_3LE', '-r', str(RATE), '-d', str(dur), out])
    time.sleep(1.0)
    subprocess.run(['aplay', '-q', play], check=True)                # default output (PipeWire), the headphone jack
    rec.wait()
    if rec.returncode: sys.exit(f'arecord failed ({rec.returncode}): is the OP-1 connected as USB audio, hw:OP1 free?')
    analyse(out)

def self_test():
    x = signal(); assert abs(len(x) / RATE - (2 + SEG * 5 + 6)) < 1e-6
    # a -100 dBFS tone: below the 16-bit LSB, visible in 24 bits
    t = np.arange(int(RATE * 2)) / RATE; v = np.round(10 ** (-100 / 20) * np.sin(2 * np.pi * 1000 * t) * 8388607).astype(np.int32)
    r, tone, fl, lb = seg_stats(v); assert -102 < tone < -98 and lb > 0, (tone, lb)
    v16 = (v >> 8) << 8; assert seg_stats(v16)[3] == 0
    import tempfile, os
    p = tempfile.mktemp(suffix='.wav'); write_wav(p, np.stack([v] * 6, 1).reshape(-1), 6, 3)
    back, wd = read_wav(p); os.unlink(p); assert wd == 3 and (back[:, 2] == v).all()
    print('self-test ok')

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', nargs='?', choices=['run', 'analyse']); ap.add_argument('path', nargs='?')
    ap.add_argument('--out', default='/tmp/in24.wav'); ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test: self_test()
    elif a.cmd == 'run': run(a.out)
    elif a.cmd == 'analyse': analyse(a.path or a.out)
    else: ap.print_help()
