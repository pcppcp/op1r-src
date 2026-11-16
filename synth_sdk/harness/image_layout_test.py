#!/usr/bin/env python3
"""make_firmware.py layout_errors: our code + .bss at CODE_BASE must end by R246's frame buffer (0x02000000) and not
overlap an R246 SDRAM LDR block; R246's L1 blocks and empty blocks don't count.   python3 harness/image_layout_test.py"""
import os, sys

SDK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, SDK)
import make_firmware as mf

def ldr(*blocks):                                                  # (flags, addr, count): payload for non-FILL blocks
    return b''.join(mf.ldr_hdr(f, a, n, 0) + (b'' if f & 0x0100 else b'\0' * n) for f, a, n in blocks)

stock = ldr((0x5006, 0xFFA00000, 0), (0x0006, 0x01000000, 0x1000), (0x0106, 0x01001000, 0x01DFEF34 - 0x01000000),
            (0x0106, 0x02000000, 0xDF80), (0x0006, 0xFFA04280, 0x100), (0x8006, 0xFFA00000, 0))
fails = 0
def want(d, end, n, what):
    global fails
    err = mf.layout_errors(list(mf.ldr_blocks(d)), end)
    if len(err) != n:
        fails += 1; print(f'FAIL {what}: {err}')
want(stock, 0x01EC5754, 0, 'op1r 68 end')
want(stock, 0x02000000, 0, 'up to the frame buffer')
want(stock, 0x02000004, 2, 'into the frame buffer')                # past FRAME_BUF + overlaps its block
want(ldr((0x0106, 0x01F00000, 0x10)), 0x01EC5754, 0, 'block above the end')
want(ldr((0x0006, 0x01E80000, 0x10)), 0x01EC5754, 1, 'block inside')
want(ldr((0x0106, 0x01DFFF00, 0x200)), 0x01E00100, 1, 'block across CODE_BASE')
want(ldr((0x0106, 0x01DFFF00, 0x100)), 0x01E00100, 0, 'block ending at CODE_BASE')
want(ldr((0x0006, 0x01E00010, 0)), 0x01E00100, 0, 'empty block')
fw = os.path.join(mf.SRC_FW, 'OP1_vdk.ldr')                         # the real R246 image, if unpacked here
if os.path.exists(fw):
    want(open(fw, 'rb').read(), 0x01EC5754, 0, 'R246 OP1_vdk.ldr')
print(f'{fails} failed' if fails else 'image_layout: ok')
sys.exit(1 if fails else 0)
