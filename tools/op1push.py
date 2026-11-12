#!/usr/bin/env python3
"""Development update over USB MIDI SysEx (docs/35) - host side. Needs a build on the OP-1.

    op1push.py op1_246_custom --port /dev/snd/midiC3D0    # only what differs on the OP-1 -> staged
    op1push.py op1_246_custom --port ... --dry-run        # list what would be sent
    op1push.py op1_246_custom --port ... --force          # every file of the tree, without asking
    op1push.py op1_246_custom --port ... --files content/display/x.svg OP1_vdk.ldr   # these, unconditionally
    op1push.py --dump session.bin op1_246_custom          # the session as SysEx messages (C host test)
    op1push.py --port ... --list                          # plugin files on the OP-1 (protocol 3)
    op1push.py --port ... --delete plugins/x.op1p         # delete plugin files (staged, T1 confirm, at restart)

What differs is asked from the device: STAT gives the size of each content file on the OP-1 and, from protocol 4,
its CRC-32 (missing, other size or other CRC: sent; with protocol 2 / 3 a same-size file with other content is NOT
detected: --files / --force), IDENT gives the running firmware's version text, compared with the tree's OP1_vdk.ldr
boot text ("op1r <n> [build time]"; release builds of one version are taken as equal).

At the end the OP-1 asks: T1 restarts it (the update is applied at boot), T2 cancels.

Never sends te-boot files (the device refuses them too). Message: F0 00 20 76 7D cmd seq_lo seq_hi <7-bit> F7."""
import argparse, binascii, os, struct, sys

HDR = bytes([0xF0, 0x00, 0x20, 0x76, 0x7D])
HELLO, BEGIN, DATA, END, COMMIT, ABORT, STAT, IDENT, DELETE, LIST = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
MAX_MSG, MAX_DATA = 256, 192
WINDOW = 8                                        # DATA messages in flight (device queue: 16, RX ring ~3 KB)


def pack7(b):
    out = bytearray()
    for i in range(0, len(b), 7):
        g = b[i:i + 7]
        out.append(sum(((x >> 7) & 1) << k for k, x in enumerate(g)))
        out += bytes(x & 0x7F for x in g)
    return bytes(out)


def unpack7(b):
    out = bytearray(); i = 0
    while i < len(b):
        hi = b[i]; i += 1
        for k in range(7):
            if i >= len(b):
                break
            out.append(b[i] | ((hi >> k) & 1) << 7); i += 1
    return bytes(out)


def build(cmd, seq, payload=b''):
    m = HDR + bytes([cmd, seq & 0x7F, (seq >> 7) & 0x7F]) + pack7(payload) + b'\xF7'
    assert len(m) <= MAX_MSG, len(m)
    return m


FACTORY_DB = 'content/op1_factory.db'   # goes with every new firmware (a new engine row rarely changes its size)
USER_DATA = ('content/audio/drum/', 'content/audio/synth/', 'content/op1.db', 'content/tape.db')


def user_data(p):
    """the user's sampler slots, settings / patches and tape clips: never sent unless named with --files"""
    return p.startswith(USER_DATA)


def path_ok(p):
    low = p.lower()
    return (all(c.isalnum() or c in '_.-/' for c in p) and 0 < len(p) < 96 and not p.startswith('/')
            and not p.endswith('/') and 'te-boot' not in low and 'te_boot' not in low and 'teboot' not in low
            and '..' not in p and '//' not in p and (p == 'OP1_vdk.ldr' or p.startswith('content/') or
                                                     (p.startswith('plugins/') and p.endswith('.op1p') and
                                                      '/' not in p[8:] and len(p) >= 14)))


def session(files):
    """files: [(path, bytes)] -> list of messages (HELLO, per file BEGIN / DATA... / END, COMMIT)"""
    seq = 0; msgs = []
    def add(cmd, pl=b''):
        nonlocal seq
        msgs.append(build(cmd, seq, pl)); seq = (seq + 1) & 0x3FFF
    add(HELLO)
    for path, data in files:
        assert path_ok(path), path
        add(BEGIN, struct.pack('<II', len(data), binascii.crc32(data)) + path.encode() + b'\0')
        for off in range(0, len(data), MAX_DATA):
            add(DATA, struct.pack('<I', off) + data[off:off + MAX_DATA])
        add(END)
    add(COMMIT)
    return msgs


def parse_reply(m):
    if len(m) < 10 or m[:5] != HDR or m[-1] != 0xF7 or not m[5] & 0x40:
        return None
    pl = unpack7(m[8:-1])
    return m[5] & 0x3F, m[6] | m[7] << 7, pl[0], pl[1:]


class Port:
    """raw ALSA MIDI device (/dev/snd/midiCxDy): no extra dependency"""
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK); self.buf = bytearray()

    def send(self, m):
        os.set_blocking(self.fd, True); os.write(self.fd, m); os.set_blocking(self.fd, False)

    def recv(self, timeout):
        import select, time
        end = time.time() + timeout
        while True:
            i = self.buf.find(0xF0); j = self.buf.find(0xF7, max(i, 0))
            if i >= 0 and j > i:
                m = bytes(self.buf[i:j + 1]); del self.buf[:j + 1]
                r = parse_reply(m)
                if r: return r
                continue
            left = end - time.time()
            if left <= 0 or not select.select([self.fd], [], [], left)[0]:
                return None
            self.buf += bytes(b for b in os.read(self.fd, 4096) if b < 0xF8)   # drop realtime (clock) bytes


ST = {0: 'ok', 1: 'format', 2: 'state', 3: 'path', 4: 'size', 5: 'offset', 6: 'crc', 7: 'io', 8: 'full', 9: 'wait key',
      10: 'no file'}


class UsbipPort:
    """emulator: USB-MIDI straight over USB/IP (tools/usbip_midi.py), no kernel / sudo"""
    def __init__(self, addr):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from usbip_midi import UsbipMidi
        self.u = UsbipMidi(addr)

    def send(self, m):
        self.u.send(m)

    def recv(self, timeout):
        import time
        end = time.time() + timeout
        while time.time() < end:
            rx = self.u.poll(end - time.time())
            i = rx.find(0xF0); j = rx.find(0xF7, max(i, 0))
            if i >= 0 and j > i:
                m = bytes(rx[i:j + 1]); del rx[:j + 1]
                r = parse_reply(m)
                if r: return r
        return None


def ldr_ident(data):
    """boot text in an OP1_vdk.ldr (make_firmware.py boot_text): 'op1r <n> [time]', None if not found"""
    import re
    m = re.search(rb'\n{17} *(op1r [ -~]*)', data)
    return m.group(1).decode() if m else None


def push(port, tree, paths, select=True, dry=False, timeout=2.0, log=print, deletes=(), list_only=False):
    """paths: candidates (tree-relative); select: send only those that differ on the device"""
    import time
    p = UsbipPort(port[6:]) if port.startswith('usbip:') else Port(port); seq = 0
    def ask(cmd, pl=b'', tries=8):
        nonlocal seq
        for _ in range(tries):
            seq = (seq + 1) & 0x3FFF
            p.send(build(cmd, seq, pl))
            while True:
                r = p.recv(timeout)
                if r is None: break                               # lost: send again (new seq)
                if r[0] == cmd and r[1] == seq: return r[2], r[3]
        sys.exit(f'no reply to command {cmd} (dev build on the OP-1? right port?)')
    st, pl = ask(HELLO)
    proto = struct.unpack("<I", pl[:4])[0]
    log(f'OP-1 devupd protocol {proto}')
    if (list_only or deletes) and proto < 3:
        sys.exit('the OP-1 cannot list / delete plugin files (protocol < 3): update its firmware first')
    if list_only:
        i = 0
        while True:
            st, pl = ask(LIST, struct.pack('<H', i))
            if st: break
            log(f'{struct.unpack("<I", pl[:4])[0]:9d}  plugins/{pl[4:].decode(errors="replace")}')
            i += 1
        log(f'{i} plugin files')
        return
    def stat(path, data):
        """STAT -> status, same as data (protocol 4: size + CRC, read DU_CRC_STEP bytes per message; else size)"""
        off = crc = 0
        while True:
            st, pl = ask(STAT, path.encode() + b'\0' + (struct.pack('<II', off, crc) if proto >= 4 else b''))
            if st in (5, 7): return 0, False                        # changed while read: send it
            if st or struct.unpack('<I', pl[:4])[0] != len(data): return st, False
            if proto < 4 or len(pl) < 12: return st, True            # (size only)
            crc, off = struct.unpack('<II', pl[4:12])
            if off >= len(data): return st, crc == binascii.crc32(data)
    if select:
        if proto < 2: sys.exit('the OP-1 cannot report its files (protocol 1): use --files')
        t0 = time.time(); keep = []
        for path in paths:
            data = open(os.path.join(tree, path), 'rb').read()
            if path == 'OP1_vdk.ldr':
                st, pl = ask(IDENT); dev, mine = pl.decode(errors='replace'), ldr_ident(data)
                same = mine is not None and dev == mine
                log(f'  {"same    " if same else "differs "} {path}: OP-1 "{dev}", tree "{mine}"')
            else:
                st, same = stat(path, data)
                if st not in (0, 10): sys.exit(f'{path}: STAT refused ({ST.get(st, st)})')
                if not same: log(f'  {"missing " if st else "differs "} {path}')
            if not same: keep.append(path)
        if proto < 4 and 'OP1_vdk.ldr' in keep and FACTORY_DB in paths and FACTORY_DB not in keep:
            keep.append(FACTORY_DB)                               # engine / FX rows: often the same size
            log(f'  with the firmware: {FACTORY_DB}')
        log(f'{len(keep)} of {len(paths)} files differ (checked in {time.time() - t0:.1f} s)')
        paths = keep
    if dry or not (paths or deletes):
        return
    files = [(q, open(os.path.join(tree, q), 'rb').read()) for q in paths]
    ask(ABORT)                                                    # clean start: staging cleared
    t0 = time.time(); total = sum(len(d) for _, d in files); done = 0
    for path, data in files:
        st, _ = ask(BEGIN, struct.pack('<II', len(data), binascii.crc32(data)) + path.encode() + b'\0')
        if st: sys.exit(f'{path}: BEGIN refused ({ST.get(st, st)})')
        acked = nxt = 0; inflight = {}; last = 0                 # pipelined DATA: up to WINDOW messages in flight
        while acked < len(data):
            while nxt < len(data) and len(inflight) < WINDOW:
                seq = (seq + 1) & 0x3FFF; n = min(MAX_DATA, len(data) - nxt)
                p.send(build(DATA, seq, struct.pack('<I', nxt) + data[nxt:nxt + n])); inflight[seq] = nxt; nxt += n
            r = p.recv(timeout)
            if r is None:                                         # lost: resend everything not confirmed
                inflight.clear(); nxt = acked; continue
            if r[0] != DATA or r[1] not in inflight:
                continue                                          # stale reply (from before a resync)
            inflight.pop(r[1])
            if r[2] in (0, 5):
                w = struct.unpack('<I', r[3][:4])[0]              # device: bytes written so far
                if r[2] == 5:
                    inflight.clear(); nxt = w                     # gap (dropped message): resend from there
                acked = max(acked, w)
            else:
                sys.exit(f'{path}: DATA refused ({ST.get(r[2], r[2])})')
            if acked - last >= 65536 or acked == len(data):
                last = acked; dt = max(time.time() - t0, 1e-3)
                log(f'  {path}: {acked}/{len(data)}  ({(done + acked) / dt / 1e3:.1f} kB/s)')
        done += len(data)
        st, _ = ask(END)
        if st: sys.exit(f'{path}: END refused ({ST.get(st, st)})')
        log(f'{path}: staged ({len(data)} bytes)')
    for path in deletes:
        st, _ = ask(DELETE, path.encode() + b'\0')
        if st: sys.exit(f'{path}: DELETE refused ({ST.get(st, st)})')
        log(f'{path}: delete staged')
    log('confirm on the OP-1: T1 restart, T2 cancel')
    asked = False
    while True:
        st, _ = ask(COMMIT)
        if st == 0: break
        if st == 2 and asked: sys.exit('cancelled on the OP-1 (staging discarded)')
        if st != 9: sys.exit(f'COMMIT refused ({ST.get(st, st)})')
        asked = True
        time.sleep(0.3)
    log(f'sent {len(files)} files ({total} bytes), {len(deletes)} deletes in {time.time() - t0:.0f} s; '
        'the OP-1 restarts and applies the update.')


def selftest():
    import random
    r = random.Random(1)
    for n in list(range(0, 30)) + [191, 192, 196]:
        b = bytes(r.randrange(256) for _ in range(n))
        assert unpack7(pack7(b)) == b and all(x < 0x80 for x in pack7(b))
    assert len(build(DATA, 5, b'\0' * (4 + MAX_DATA))) <= MAX_MSG
    assert user_data('content/audio/drum/drum_1.raw') and user_data('content/op1.db') and not user_data('content/op1_factory.db')
    assert not path_ok('te-boot.ldr') and not path_ok('content/TE-Boot.op1') and not path_ok('../x')
    assert path_ok('OP1_vdk.ldr') and path_ok('content/display/a.svg') and not path_ok('/content/x')
    assert path_ok('plugins/bell.op1p') and not path_ok('plugins/a/b.op1p') and not path_ok('plugins/x.svg')
    assert ldr_ident(b'x' + b'\n' * 17 + b' ' * 20 + b'op1r 31 2026-10-14 20:28\0\0') == 'op1r 31 2026-10-14 20:28'
    assert ldr_ident(b'\n' * 17 + b' ' * 33 + b'R. 00246') is None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('tree', nargs='?')
    ap.add_argument('--dump', help='write the session (length-prefixed messages) to this file')
    ap.add_argument('--port', help='raw MIDI device of the OP-1 (/dev/snd/midiC3D0) or usbip:HOST:PORT (emulator)')
    ap.add_argument('--files', nargs='*', help='send these files (paths in the tree) without asking the device')
    ap.add_argument('--dry-run', action='store_true', help='only list what differs on the device')
    ap.add_argument('--force', action='store_true', help='send every file of the tree without checking the device')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--list', action='store_true', help='list the plugin files on the OP-1 (/yaffs2/plugins)')
    ap.add_argument('--delete', nargs='+', default=[], metavar='PATH',
                    help='delete plugin files on the OP-1 (plugins/<name>.op1p), applied at the restart')
    a = ap.parse_args()
    selftest()
    if a.selftest:
        print('ok'); return
    if (a.list or a.delete) and a.port:
        for d in a.delete:
            if not (path_ok(d) and d.startswith('plugins/')): sys.exit(f'{d}: only plugins/<name>.op1p can be deleted')
        return push(a.port, a.tree or '.', sorted(a.files or []), False, False, deletes=a.delete, list_only=a.list)
    if not a.tree or not (a.dump or a.port):
        sys.exit('tree and --port (or --dump) needed')
    select = a.files is None and not a.force
    if a.files is None:                                           # every file of the tree the device accepts
        a.files = [os.path.relpath(os.path.join(r, f), a.tree).replace(os.sep, '/')
                   for r, _, fs in os.walk(a.tree) for f in fs if not f.startswith('.')]
        a.files = [p for p in a.files if path_ok(p) and not user_data(p)]
    if not select and 'OP1_vdk.ldr' in a.files and FACTORY_DB not in a.files and \
            os.path.exists(os.path.join(a.tree, FACTORY_DB)):
        a.files.append(FACTORY_DB)                                # engine / FX rows belong to the firmware build
    if a.port:
        return push(a.port, a.tree, sorted(a.files), select, a.dry_run)
    files = [(p, open(os.path.join(a.tree, p), 'rb').read()) for p in sorted(a.files)]
    msgs = session(files)
    with open(a.dump, 'wb') as f:
        for m in msgs:
            f.write(struct.pack('<H', len(m)) + m)
    print(f'{a.dump}: {len(msgs)} messages, {sum(map(len, msgs)) / 1e6:.2f} MB SysEx for '
          f'{sum(len(d) for _, d in files) / 1e6:.2f} MB in {len(files)} files')


if __name__ == '__main__':
    main()
