#!/usr/bin/env python3
"""Live USB audio monitor for the OP-1 (Linux): what the OP-1 sends to the computer, per channel, in the console.

  python3 tools/usb_vu.py                 # meters until Ctrl-C, then a summary
  python3 tools/usb_vu.py --card 2        # ALSA card number (default: the card named OP-1)
  python3 tools/usb_vu.py --self-test     # check the decoding / metering on synthetic data, no device needed

Per channel (op1r: 1/2 = master L / R, 3..6 = tape tracks 1..4 with USB OUT set to + TRACKS, docs/21): an RMS bar
(-60 .. 0 dBFS, 50 ms), a peak marker held 1.5 s, the peak of the last block, the maximum since the start, and clipped
samples (>= -0.1 dBFS). Status line: the measured frame rate (should be 44100), arecord overruns, time.
Needs arecord (alsa-utils) and numpy. Only reads from the OP-1; nothing is sent to it.
"""
import argparse, math, os, re, select, subprocess, sys, time

import numpy as np

RATE, BLOCK_S, HOLD_S, FLOOR = 44100, 0.05, 1.5, -60.0
NAMES = ['MASTER L', 'MASTER R', 'TRACK 1', 'TRACK 2', 'TRACK 3', 'TRACK 4']
FMT = {'S24_3LE': 3, 'S16_LE': 2, 'S32_LE': 4}
FULL = {3: 1 << 23, 2: 1 << 15, 4: 1 << 31}


def find_card():
    """-> (card, channels, format) of the OP-1's capture stream, from /proc/asound"""
    try:
        cards = open('/proc/asound/cards').read()
    except OSError:
        sys.exit('no /proc/asound (Linux and ALSA needed)')
    m = re.search(r'^\s*(\d+)\s+\[[^\]]*\]:.*\n.*OP-1', cards, re.M) or re.search(r'^\s*(\d+)\s+\[OP1', cards, re.M)
    if not m:
        sys.exit('no OP-1 sound card (is it connected and not in disk mode?)')
    card = int(m.group(1))
    try:
        s = open('/proc/asound/card%d/stream0' % card).read()
        cap = s[s.index('Capture'):]
        ch = int(re.search(r'Channels:\s*(\d+)', cap).group(1))
        fmt = re.search(r'Format:\s*(\S+)', cap).group(1)
    except (OSError, ValueError, AttributeError):
        ch, fmt = 6, 'S24_3LE'
    return card, ch, fmt


def decode(raw, ch, width):
    """raw interleaved little-endian PCM -> float array [frames, ch] in -1 .. 1"""
    n = len(raw) // (ch * width)
    b = np.frombuffer(raw[:n * ch * width], dtype=np.uint8).reshape(n, ch, width).astype(np.int32)
    v = b[..., 0]
    for k in range(1, width):
        v = v | (b[..., k] << (8 * k))
    sign = 1 << (8 * width - 1)
    if width < 4:
        v = np.where(v & sign, v - (sign << 1), v)
    return v.astype(np.float64) / FULL[width]


def db(x):
    return 20 * math.log10(x) if x > 1e-9 else -120.0


class Meter:
    def __init__(self, ch):
        self.ch = ch
        self.rms = [FLOOR] * ch
        self.peak = [FLOOR] * ch
        self.hold = [FLOOR] * ch
        self.hold_t = [0.0] * ch
        self.max = [-120.0] * ch
        self.clip = [0] * ch
        self.live = [False] * ch

    def feed(self, x, now):
        for c in range(self.ch):
            s = x[:, c]
            a = np.abs(s)
            pk = db(float(a.max())) if len(a) else -120.0
            self.rms[c] = db(float(np.sqrt(np.mean(s * s)))) if len(s) else -120.0
            self.peak[c] = pk
            self.max[c] = max(self.max[c], pk)
            self.clip[c] += int(np.count_nonzero(a >= 10 ** (-0.1 / 20)))
            self.live[c] = bool(np.any(s != 0))
            if pk >= self.hold[c] or now - self.hold_t[c] > HOLD_S:
                self.hold[c], self.hold_t[c] = pk, now


def bar(rms, hold, width):
    pos = lambda d: int(round((min(max(d, FLOOR), 0.0) - FLOOR) / -FLOOR * width))
    n, h = pos(rms), pos(hold)
    out = []
    for i in range(width):
        lvl = FLOOR + (i + 0.5) * -FLOOR / width
        colour = '\x1b[32m' if lvl < -12 else '\x1b[33m' if lvl < -3 else '\x1b[31m'
        if i < n:
            out.append(colour + '#')
        elif i == h - 1 and h > n:
            out.append(colour + '|')
        else:
            out.append('\x1b[90m-')
    return ''.join(out) + '\x1b[0m'


def draw(m, names, rate, overruns, t, first):
    w = max(20, min(60, __import__("shutil").get_terminal_size((100, 20)).columns - 62))
    lines = ['OP-1 USB audio out    %6.0f frames/s    overruns %d    %5.1f s    Ctrl-C: quit' % (rate, overruns, t), '']
    for c in range(m.ch):
        state = '\x1b[32mlive  \x1b[0m' if m.live[c] else '\x1b[90msilent\x1b[0m'
        clip = ('\x1b[31m%5d clip\x1b[0m' % m.clip[c]) if m.clip[c] else '        '
        lines.append('%-9s %s rms %6.1f  pk %6.1f  max %6.1f  %s %s' % (
            names[c], bar(m.rms[c], m.hold[c], w), max(m.rms[c], -99.9), max(m.peak[c], -99.9),
            max(m.max[c], -99.9), state, clip))
    if not first:
        sys.stdout.write('\x1b[%dA' % len(lines))
    sys.stdout.write('\n'.join('\x1b[2K' + l for l in lines) + '\n')
    sys.stdout.flush()


def self_test():
    for width, fmt in ((3, 'S24_3LE'), (2, 'S16_LE')):
        full = FULL[width]
        frames = np.array([[0, full - 1, -full, full // 2, -1, 1]], dtype=np.int64)
        raw = b''.join(int(v & ((1 << (8 * width)) - 1)).to_bytes(width, 'little') for v in frames[0])
        x = decode(raw * 4, 6, width)
        assert x.shape == (4, 6), (fmt, x.shape)
        assert x[0, 0] == 0 and abs(x[0, 1] - 1) < 1e-4 and x[0, 2] == -1 and abs(x[0, 3] - 0.5) < 1e-9, (fmt, x[0])
        assert x[0, 4] < 0 < x[0, 5]
    m = Meter(2)
    t = np.arange(4410) / RATE
    m.feed(np.stack([0.5 * np.sin(2 * np.pi * 1000 * t), np.zeros_like(t)], axis=1), 0.0)
    assert abs(m.peak[0] - db(0.5)) < 0.05 and abs(m.rms[0] - db(0.5 / math.sqrt(2))) < 0.05, m.peak
    assert m.live[0] and not m.live[1] and m.clip == [0, 0]
    m.feed(np.ones((10, 2)), 1.0)
    assert m.clip == [10, 10] and m.hold[0] == 0.0
    print('self-test ok')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--card', type=int)
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    card, ch, fmt = find_card()
    if a.card is not None:
        card = a.card
    width = FMT.get(fmt)
    if not width:
        sys.exit('capture format %s not handled' % fmt)
    names = NAMES[:ch] + ['CH %d' % (i + 1) for i in range(len(NAMES), ch)]
    p = subprocess.Popen(['arecord', '-q', '-D', 'hw:%d' % card, '-c', str(ch), '-f', fmt, '-r', str(RATE), '-t', 'raw'],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    m, chunk = Meter(ch), int(RATE * BLOCK_S) * ch * width
    t0, frames, overruns, first, buf = time.time(), 0, 0, True, b''
    print('card %d: %d channels, %s, %d Hz' % (card, ch, fmt, RATE))
    try:
        while True:
            r, _, _ = select.select([p.stdout, p.stderr], [], [], 1.0)
            if p.stderr in r:
                err = os.read(p.stderr.fileno(), 4096).decode(errors='replace')
                overruns += err.count('overrun')
                if 'error' in err.lower() and 'overrun' not in err:
                    sys.exit('arecord: ' + err.strip())
            if p.stdout in r:
                d = os.read(p.stdout.fileno(), chunk)
                if not d:
                    sys.exit('arecord stopped (device gone?)')
                buf += d
            if len(buf) >= chunk:
                x = decode(buf[:chunk], ch, width)
                buf = buf[chunk:]
                frames += len(x)
                now = time.time()
                m.feed(x, now)
                draw(m, names, frames / max(now - t0, 1e-3), overruns, now - t0, first)
                first = False
            if p.poll() is not None:
                sys.exit('arecord exited: ' + p.stderr.read().decode(errors='replace').strip())
    except KeyboardInterrupt:
        pass
    finally:
        p.terminate()
    t = time.time() - t0
    print('\n%.1f s, %d frames (%.0f/s), overruns %d' % (t, frames, frames / max(t, 1e-3), overruns))
    for c in range(ch):
        print('  %-9s max %6.1f dBFS  clipped %d' % (names[c], max(m.max[c], -99.9), m.clip[c]))


if __name__ == '__main__':
    main()
