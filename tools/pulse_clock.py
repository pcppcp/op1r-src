#!/usr/bin/env python3
"""Clock pulses on the notebook's audio output, to test the OP-1's pulse clock input (docs/59).

    pulse_clock.py --bpm 120 --ppq 4                  # 4 PPQ at 120 bpm until Ctrl-C
    pulse_clock.py --bpm 90 --ppq 24 --seconds 20     # DIN sync rate, 20 s
    pulse_clock.py --bpm 120 --ppq 4 --run 8 --pause 4   # 8 beats on, 4 beats off, repeated (start / stop test)
    pulse_clock.py --self-test

Positive gates (default 5 ms) on both channels, streamed into `aplay` (no extra Python packages). Turn the notebook
volume up: line level is enough for the OP-1's line input; the OP-1's tempo page flashes on each detected pulse.
"""
import argparse, subprocess, sys

def pulses(rate, bpm, ppq, width_ms, level, run, pause, channel='both'):
    """Yields 16-bit stereo PCM chunks (one pulse period each); the pulse grid is exact over time (no drift)."""
    per = rate * 60.0 / (bpm * ppq)                  # samples per pulse (fractional)
    w = max(1, int(rate * width_ms / 1000))
    v, z = int(32767 * level).to_bytes(2, 'little', signed=True), b'\0\0'
    hi = {'both': v + v, 'left': v + z, 'right': z + v, 'inverted': v + (-int(32767 * level)).to_bytes(2, 'little', signed=True)}[channel]
    lo = z * 2
    k, pos = 0, 0                                     # pulse index, samples written
    while True:
        on = run == 0 or (k // ppq) % (run + pause) < run   # beat on / off for the start / stop test
        end = round((k + 1) * per)
        n = end - pos
        g = min(w, n) if on else 0
        yield hi * g + lo * (n - g)
        pos, k = end, k + 1

def self_test():
    rate, bpm, ppq = 48000, 120, 4
    g = pulses(rate, bpm, ppq, 5, 0.5, 2, 1)
    chunks = [next(g) for _ in range(ppq * 6)]       # 6 beats: 2 on, 1 off, 2 on, 1 off
    assert sum(len(c) for c in chunks) // 4 == rate * 3            # 6 beats at 120 bpm = 3 s, exact
    starts = [len(c) > 0 and c[:4] != b'\0\0\0\0' for c in chunks]
    assert starts == ([True] * 8 + [False] * 4) * 2               # beats on / off
    assert chunks[0][:4] == (16383).to_bytes(2, 'little', signed=True) * 2
    l = next(pulses(rate, bpm, ppq, 5, 0.5, 0, 0, 'left'))
    assert l[:4] == (16383).to_bytes(2, 'little', signed=True) + b'\0\0'
    print('self-test ok')

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bpm', type=float, default=120)
    ap.add_argument('--ppq', type=int, default=4, choices=[1, 2, 4, 24, 48])
    ap.add_argument('--seconds', type=float, default=0, help='0: until Ctrl-C')
    ap.add_argument('--width-ms', type=float, default=5, help='gate length (default 5 ms)')
    ap.add_argument('--level', type=float, default=0.8, help='0..1 of full scale (default 0.8)')
    ap.add_argument('--run', type=int, default=0, help='beats on (with --pause: start / stop test)')
    ap.add_argument('--pause', type=int, default=0, help='beats off between runs')
    ap.add_argument('--channel', default='both', choices=['both', 'left', 'right', 'inverted'],
                    help='pulse on both channels (default), one, or right inverted (to test a cancelling sum)')
    ap.add_argument('--rate', type=int, default=48000)
    ap.add_argument('--device', help='ALSA device for aplay -D (default: system default)')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not 0 < a.level <= 1 or a.bpm <= 0 or a.width_ms <= 0:
        sys.exit('level must be 0..1, bpm and width > 0')
    if a.width_ms * a.bpm * a.ppq / 60000 >= 0.9:
        sys.exit('gate too long for this pulse rate (lower --width-ms)')
    cmd = ['aplay', '-q', '-t', 'raw', '-f', 'S16_LE', '-c', '2', '-r', str(a.rate)] + (['-D', a.device] if a.device else [])
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    left = int(a.seconds * a.rate) if a.seconds else None
    print(f'{a.bpm:g} bpm, {a.ppq} PPQ ({a.bpm * a.ppq / 60:.2f} pulses/s), Ctrl-C to stop', file=sys.stderr)
    try:
        for c in pulses(a.rate, a.bpm, a.ppq, a.width_ms, a.level, a.run, a.pause, a.channel):
            if left is not None:
                if left <= 0: break
                c = c[:left * 4]; left -= len(c) // 4
            p.stdin.write(c)
    except (KeyboardInterrupt, BrokenPipeError):
        pass
    finally:
        try: p.stdin.close()
        except BrokenPipeError: pass
        if p.wait() not in (0, -2):                   # aplay failed (e.g. device busy: PipeWire holds hw devices)
            sys.exit(f'aplay failed (exit {p.returncode}); try without --device to play through the default output')

if __name__ == '__main__':
    main()
