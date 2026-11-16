#!/usr/bin/env python3
"""op1dev: drive a developer build (local make_firmware.py build, not --release) over USB MIDI SysEx (docs/86).

  op1dev.py PORT peek ADDR|SYMBOL LEN          hex dump
  op1dev.py PORT poke ADDR|SYMBOL HEXBYTES
  op1dev.py PORT key NAME [down|up|tap]        keys as the emulator names them (tape, rec, t1, g4, enc3 ...)
  op1dev.py PORT turn ENC cw|ccw DETENTS       enc1..enc4 (blue .. red)
  op1dev.py PORT script FILE                   the emulator's --input-script format (wait, tap, press, release,
                                               turn ENC cw|ccw EDGES, peek ADDR|SYMBOL LEN PATH, quit)
  op1dev.py PORT perf [SECONDS]                audio callback %, ELSE-MARIE / STEFAN engine %, slot paths
PORT: /dev/snd/midiCxDy or usbip:HOST:PORT (emulator). Symbols: synth_sdk/build/synth.sym (--sym). The firmware
answers only in local builds (commands 12..15); a release build replies "format"."""
import argparse, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from op1push import build, Port, UsbipPort, ST

PEEK, POKE, KEY, TURN, IDENT = 12, 13, 14, 15, 8
MAX = 192
NOTES = ['f3', 'f3#', 'g3', 'g3#', 'a3', 'a3#', 'b3', 'c4', 'c4#', 'd4', 'd4#', 'e4',
         'f4', 'f4#', 'g4', 'g4#', 'a4', 'a4#', 'b4', 'c5', 'c5#', 'd5', 'd5#', 'e5']
KB = [39, 13, 40, 14, 41, 15, 42, 28, 32, 29, 22, 30, 31, 23, 48, 24, 49, 25, 50, 51, 26, 52, 27, 53]   # seqdev/ms_fw.c kb_keys
KEYS = {'help': 0x00, 'tempo': 0x01, 'synth': 0x02, 'drum': 0x03, 'tape': 0x04, 'mixer': 0x05,
        't1': 0x06, 't2': 0x07, 't3': 0x08, 't4': 0x09, 'lift': 0x0A, 'drop': 0x0B, 'split': 0x0C,
        's1': 0x10, 's2': 0x11, 's3': 0x12, 's4': 0x13, 's5': 0x14, 'seq': 0x15, 's6': 0x2D, 's7': 0x2E, 's8': 0x2F,
        'rec': 0x21, 'play': 0x22, 'stop': 0x23, 'left': 0x24, 'right': 0x25, 'shift': 0x26, 'mic': 0x2B, 'com': 0x2C,
        'enc1': 0x3B, 'enc2': 0x3C, 'enc3': 0x3D, 'enc4': 0x3E, **dict(zip(NOTES, KB))}


def load_syms(path):
    syms = {}
    if os.path.exists(path):
        for line in open(path):
            p = line.split()
            if len(p) >= 3 and p[2].startswith('_'):
                syms[p[2][1:]] = int(p[0], 16)
    return syms


class Dev:
    def __init__(self, port, sym=None):
        self.p = UsbipPort(port[6:]) if port.startswith('usbip:') else Port(port)
        self.seq = 0; self.syms = load_syms(sym) if sym else {}

    def call(self, cmd, pl=b'', tries=3, timeout=1.0):
        for _ in range(tries):
            self.seq = (self.seq + 1) & 0x3FFF
            self.p.send(build(cmd, self.seq, pl))
            while True:
                r = self.p.recv(timeout)
                if r is None: break
                if r[0] == cmd and r[1] == self.seq:
                    if r[2]: raise RuntimeError(f'command {cmd}: {ST.get(r[2], r[2])}')
                    return bytes(r[3])
        raise TimeoutError(f'command {cmd}: no reply (a developer build on {self.p}?)')

    def addr(self, a):
        if isinstance(a, int): return a
        if a in self.syms: return self.syms[a]
        return int(a, 0)

    def peek(self, a, n):
        a = self.addr(a); out = b''
        while n > 0:
            k = min(n, MAX); out += self.call(PEEK, struct.pack('<IH', a, k)); a += k; n -= k
        return out

    def poke(self, a, data):
        a = self.addr(a)
        for i in range(0, len(data), MAX - 8):
            self.call(POKE, struct.pack('<I', a + i) + data[i:i + MAX - 8])

    def key(self, name, down):
        self.call(KEY, bytes([KEYS[name], 1 if down else 0]))

    def turn(self, enc, ticks):                                    # enc 0..3, ticks: 2 per detent
        while ticks:
            t = max(-127, min(127, ticks)); self.call(TURN, struct.pack('<Bb', enc, t)); ticks -= t

    def ident(self):
        return self.call(IDENT).decode('latin-1')

    def u32(self, sym, n=1):
        return struct.unpack(f'<{n}I', self.peek(sym, 4 * n))


def run_script(d, path, log=print):
    for raw in open(path):
        w = raw.split('#')[0].split()
        if not w: continue
        c = w[0]
        if c == 'quit': break
        elif c == 'wait': time.sleep(int(w[1]) / 1000)
        elif c == 'tap': d.key(w[1], 1); time.sleep(0.05); d.key(w[1], 0)
        elif c == 'press': d.key(w[1], 1)
        elif c == 'release': d.key(w[1], 0)
        elif c == 'turn':                                          # emulator edges: 4 per detent; device ticks: 2 per detent
            d.turn(int(w[1][3:]) - 1, (1 if w[2] == 'cw' else -1) * int(w[3]) // 2)
        elif c == 'peek':
            data = d.peek(w[1], int(w[2], 0))
            if len(w) > 3: open(w[3], 'wb').write(data)
            else: log(data.hex())
        else: log(f'skipped: {raw.strip()}')


def perf(d, secs=3.0, log=print):
    """cycles per audio block from the counters (screens/sysmon.c sm_aud_tot: blocks, cycles / 256; tape/tm.c)"""
    prof = 'tm_prof' in d.syms
    a0 = d.u32('sm_aud_tot', 2); t0 = d.u32('tm_cyc')[0]; p0 = d.u32('tm_prof', 12) if prof else None
    time.sleep(secs)
    a1 = d.u32('sm_aud_tot', 2); t1 = d.u32('tm_cyc')[0]; p1 = d.u32('tm_prof', 12) if prof else None
    n = (a1[0] - a0[0]) & 0xFFFFFFFF
    if not n: raise RuntimeError('no audio blocks counted')
    blk = 400e6 / 44100 * 64                                       # cycles per 64-frame block at 400 MHz
    cb = ((a1[1] - a0[1]) & 0xFFFFFFFF) * 256 / n; fr = ((t1 - t0) & 0xFFFFFFFF) / n
    log(f'blocks {n}  callback {cb:.0f} cycles = {cb / blk * 100:.1f} %  tape engine {fr:.0f} = {fr / blk * 100:.1f} %')
    if prof:                                                       # tape/tm.c PROFILE builds: the engine's sections per block
        nb = ((p1[7] - p0[7]) & 0xFFFFFFFF) or 1
        names = ('start', 'slots', None, None, 'mix', 'out', 'rec', None, 'fast:top', 'fast:setup', 'fast:part', None)
        log('engine ' + '  '.join(f'{k} {((p1[i] - p0[i]) & 0xFFFFFFFF) / nb:.0f}' for i, k in enumerate(names) if k))
    if 'tm_sl_paths' in d.syms:
        paths = d.peek('tm_sl_paths', 3); pc = d.u32('tm_sl_pcyc', 3)
        log('slots  ' + '  '.join(f'{k} {paths[i]}: {pc[i]} cycles ({pc[i] / blk * 100:.1f} %)'
                                   for i, k in enumerate(('fast', 'general', 'stretch'))))
    return cb / blk * 100, fr / blk * 100


def selftest():
    assert KEYS['tape'] == 4 and KEYS['g4'] == 48 and KEYS['enc3'] == 0x3D and len(NOTES) == len(KB) == 24
    m = build(PEEK, 1, struct.pack('<IH', 0x01DE4C40, 4))
    assert m[:5] == bytes([0xF0, 0, 0x20, 0x76, 0x7D]) and m[5] == PEEK and m[-1] == 0xF7 and all(b < 0x80 for b in m[1:-1])
    print('selftest ok')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port', nargs='?'); ap.add_argument('cmd', nargs='?'); ap.add_argument('args', nargs='*')
    ap.add_argument('--sym', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'synth_sdk', 'build', 'synth.sym'))
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest: return selftest()
    if not a.port or not a.cmd: ap.error('PORT and a command are needed')
    d = Dev(a.port, a.sym)
    print('device:', d.ident())
    if a.cmd == 'peek':
        data = d.peek(a.args[0], int(a.args[1], 0))
        for i in range(0, len(data), 16): print(f'{i:04x}  {data[i:i + 16].hex(" ")}')
    elif a.cmd == 'poke': d.poke(a.args[0], bytes.fromhex(a.args[1]))
    elif a.cmd == 'key':
        how = a.args[1] if len(a.args) > 1 else 'tap'
        if how in ('down', 'tap'): d.key(a.args[0], 1)
        if how == 'tap': time.sleep(0.05)
        if how in ('up', 'tap'): d.key(a.args[0], 0)
    elif a.cmd == 'turn': d.turn(int(a.args[0][3:]) - 1, (1 if a.args[1] == 'cw' else -1) * 2 * int(a.args[2]))
    elif a.cmd == 'script': run_script(d, a.args[0])
    elif a.cmd == 'perf': perf(d, float(a.args[0]) if a.args else 3.0)
    else: ap.error(f'unknown command {a.cmd}')


if __name__ == '__main__':
    main()
