#!/usr/bin/env python3
"""engine+ fm preview (docs/90): the extra parameters in the side columns the fm screen leaves free (its cubes move
with the knobs but stay inside x 61..263 at y > 98). Backgrounds: emulator screenshots (local only, never committed); text in
Iosevka (harness/iosevka.py) for the stock font.
    python3 display/engineplus_preview.py BG.ppm OUT.png STYLE VALUES     STYLE bar|value|dim, VALUES e.g. 5,-,9,0
    ('-' = knob not flipped)"""
import sys, os, cairocffi as cairo
from PIL import Image
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'harness'))
import iosevka

COL = [(0x4D, 0x9E, 0xFF), (0x00, 0xED, 0x95), (0xFF, 0xFF, 0xFF), (0xFF, 0x3A, 0x5D)]
GREY = (0x6F, 0x71, 0x73)
LAB = ['drive', 'filter', 'reso', 'crush']
S = 2
# blue / green: left column, white / red: right column (the knobs' order), bottom aligned
POS = [(8, 112), (8, 134), (268, 112), (268, 134)]
W = 44


def render(bg, out, style, vals):
    im = Image.open(bg).convert('RGB').resize((320 * S, 160 * S), Image.NEAREST)
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, 320 * S, 160 * S)
    ctx = cairo.Context(surf)
    ctx.set_source_surface(cairo.ImageSurface.create_for_data(
        bytearray(im.convert('RGBA').tobytes('raw', 'BGRA')), cairo.FORMAT_RGB24, 320 * S, 160 * S), 0, 0)
    ctx.paint()
    ctx.scale(S, S)
    ctx.set_line_cap(cairo.LINE_CAP_BUTT)
    for k, v in enumerate(vals):
        if v == '-':
            continue
        v = int(v); x, y = POS[k]
        c = [a / 255 for a in COL[k]]; g = [a / 255 for a in GREY]
        if style == 'dim':                                # brightness: grey (0) .. full colour (9)
            t = v / 9
            ctx.set_source_rgb(*[g[i] * 0.5 * (1 - t) + c[i] * t for i in range(3)])
            ctx.move_to(x, y); iosevka.show(ctx, LAB[k], 9)
            continue
        ctx.set_source_rgb(*c); ctx.move_to(x, y); iosevka.show(ctx, LAB[k], 9)
        if style == 'value':
            n = str(v * 11)
            ctx.move_to(x + W - iosevka.width(n, 9), y); iosevka.show(ctx, n, 9)
        else:
            ctx.set_line_width(1)
            ctx.set_source_rgb(*g); ctx.move_to(x, y + 5.5); ctx.line_to(x + W, y + 5.5); ctx.stroke()
            if v:
                ctx.set_source_rgb(*c); ctx.move_to(x, y + 5.5); ctx.line_to(x + W * v / 9, y + 5.5); ctx.stroke()
    surf.write_to_png(out)


if __name__ == '__main__':
    render(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4].split(','))
