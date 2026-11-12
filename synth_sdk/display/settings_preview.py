#!/usr/bin/env python3
"""Preview of the op1r settings page in the stock font (lowercase; Iosevka stands in here, harness/iosevka.py):
labels left in the encoder colours, values at x 164; tape machine names 'jesper/op1' (stock) / 'stefan' (free).
    python3 display/settings_preview.py [OUT.png] [free]"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); SDK = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(SDK, 'harness'))
import cairocffi as cairo, iosevka
BLUE, GREEN, WHITE = (0x4D, 0x9E, 0xFF), (0x00, 0xED, 0x95), (0xFF, 0xFF, 0xFF)
SIZE, Y0, DY, LX, VX = 16, 40, 28, 24, 164           # stock text size, rows as before (px)

def text(ctx, s, x, y, c):
    em = SIZE * 0.78
    ctx.set_source_rgb(*(v / 255 for v in c)); ctx.move_to(x, y + SIZE * 0.75); iosevka.show(ctx, s, em); ctx.fill()

def render(out, free):
    s = cairo.ImageSurface(cairo.FORMAT_RGB24, 320, 160); ctx = cairo.Context(s)
    ctx.set_source_rgb(0, 0, 0); ctx.paint()
    for i, (l, v, c) in enumerate([('count-in', '8', BLUE), ('velocity', '127', GREEN),
                                   ('tape', 'stefan' if free else 'jesper/op1', WHITE)]):
        text(ctx, l, LX, Y0 + i * DY, c); text(ctx, v, VX, Y0 + i * DY, c)
    s.write_to_png(out)

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'settings_preview.png')
    render(out, len(sys.argv) > 2 and sys.argv[2] == 'free')
