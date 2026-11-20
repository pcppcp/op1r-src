#!/usr/bin/env python3
"""Emulator test of the devupd apply phase (docs/35). Needs a build in ../op1_246_custom(-repacked.op1),
an op1emu build and a NAND image with user data. Local paths, firmware-derived data stays in /tmp.

  t1: staged SVG + a different LDR (+ manifest)  -> applied: LDR in blocks 4.., SVG in place, staging gone,
      te-boot blocks 0..3 identical; booting again runs the staged LDR (boot text)
  t2: same with one byte of the staged LDR flipped -> CRC fails: nothing changed, staging gone, normal boot
  t3: no staging                                   -> untouched

  devupd_emu_test.py BASE_NAND OTHER_LDR    (OTHER_LDR: any other op1r OP1_vdk.ldr, e.g. an older build)"""
import binascii, os, shutil, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(HERE))
EMU, EMUDIR = '/tmp/emu-linux/op1emu', os.path.expanduser('~/src/op1emu-hundekraft')
JIT, W = '/tmp/hk_jitA', '/tmp/du_emu'
BLOCK = 64 * 2048


def manifest(files):
    t = 'op1r-stage 1\n' + ''.join(f'F {len(d)} {binascii.crc32(d):08x} {p}\n' for p, d in files)
    return (t + f'N {len(files)} {binascii.crc32(t.encode()):08x}\n').encode()


def boot(img, frames, dumps='', snapshot=False):
    open(f'{W}/s.txt', 'w').write(f'wait-frames {frames}\nquit\n')
    env = dict(os.environ, OP1EMU_USBIP_PORT='3499', OP1_DUMP_MEM=dumps)
    subprocess.run([EMU, img] + (['--nand-snapshot'] if snapshot else []) + ['--headless', '--deterministic',
                   '--jit-cache', JIT, '--input-script', f'{W}/s.txt'], cwd=EMUDIR, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3600)


def main():
    base, other = sys.argv[1], sys.argv[2]
    os.makedirs(W, exist_ok=True)
    inst = [sys.executable, os.path.join(ROOT, 'tools', 'op1nand_install.py')]
    sym = {l.split()[2]: l.split()[0] for l in open(os.path.join(ROOT, 'synth_sdk', 'build', 'synth.sym')) if len(l.split()) == 3}
    res = sym['_du_fw_result']
    shutil.copy(base, f'{W}/base.bin')
    subprocess.run(inst + [f'{W}/base.bin', os.path.join(ROOT, 'op1_246_custom-repacked.op1'), '--fast'], check=True, stdout=subprocess.DEVNULL)
    svg = open(os.path.join(ROOT, 'op1_246_custom', 'content', 'display', 'usbout.svg'), 'rb').read() + b'<!-- devupd test -->\n'
    ldr = open(other, 'rb').read()
    assert ldr != open(os.path.join(ROOT, 'op1_246_custom', 'OP1_vdk.ldr'), 'rb').read(), 'OTHER_LDR equals the installed one'
    bad = bytearray(ldr); bad[100000] ^= 1
    open(f'{W}/svg', 'wb').write(svg); open(f'{W}/manifest', 'wb').write(manifest([('content/display/usbout.svg', svg), ('OP1_vdk.ldr', ldr)]))
    ok = True
    for t, l in (('t1', ldr), ('t2', bytes(bad)), ('t3', None)):
        img = f'{W}/{t}.bin'
        shutil.copy(f'{W}/base.bin', img)
        if l is not None:
            open(f'{W}/ldr', 'wb').write(l)
            subprocess.run(inst + [img, '--fast', '--put', f'op1r_stage/f000={W}/svg', '--put', f'op1r_stage/f001={W}/ldr',
                                   '--put', f'op1r_stage/manifest={W}/manifest'], check=True, stdout=subprocess.DEVNULL)
        before = open(img, 'rb').read()[:132 * BLOCK]
        boot(img, 400, f'{res}:4:{W}/{t}.res')
        after = open(img, 'rb').read()[:132 * BLOCK]
        res_f = f'{W}/{t}.res'                                       # (an applied update restarts: op1emu resets and
        r = struct.unpack('<i', open(res_f, 'rb').read())[0] if os.path.exists(res_f) else None   # leaves no dump: None)
        staged = subprocess.run(inst + [img, '--list'], capture_output=True, text=True).stdout.count('op1r_stage')
        teboot = before[:4 * BLOCK] == after[:4 * BLOCK]
        if t == 't1':
            subprocess.run(inst + [img, '--get', f'content/display/usbout.svg={W}/back.svg'], check=True, stdout=subprocess.DEVNULL)
            boot(img, 60, f'018f7358:40:{W}/bt', snapshot=True)
            bt = open(f'{W}/bt', 'rb').read().replace(b'\0', b'').decode(errors='replace').strip()
            good = (r in (0, None) and teboot and after[4 * BLOCK:4 * BLOCK + len(ldr)] == ldr and staged == 0 and
                    open(f'{W}/back.svg', 'rb').read() == svg and bt.split('..')[0].strip() in ldr.decode('latin-1'))
            print(f't1 applied: result {r}, te-boot identical {teboot}, LDR in blocks 4.. {after[4*BLOCK:4*BLOCK+len(ldr)] == ldr}, '
                  f'SVG in place, staging left {staged}, boots "{bt.split("..")[0].strip()}" -> {"ok" if good else "FAIL"}')
        elif t == 't2':
            good = r == 11 and before == after and staged == 0
            print(f't2 corrupted: result {r}, boot area unchanged {before == after}, staging left {staged} -> {"ok" if good else "FAIL"}')
        else:
            good = r == -1 and before == after
            print(f't3 nothing staged: result {r}, boot area unchanged {before == after} -> {"ok" if good else "FAIL"}')
        ok &= good
    print('OK: devupd apply phase' if ok else 'FAILED')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
