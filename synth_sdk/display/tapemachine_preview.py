#!/usr/bin/env python3
"""Design preview of the free tape machine screen (docs/82), for approval before building: four track lanes (the
grey line = the global loop, the bright segment = the track's own loop, a tick = its playhead), the selected track's
values in the bottom row in encoder colours. Text in Iosevka (harness/iosevka.py), butt line caps.
    python3 display/tapemachine_preview.py [OUT.png]"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); SDK = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(SDK, 'harness'))
import cairocffi as cairo, iosevka

BLUE, GREEN, WHITE, RED, GREY = (0x4D, 0x9E, 0xFF), (0x00, 0xED, 0x95), (0xFF, 0xFF, 0xFF), (0xFF, 0x3A, 0x5D), (0x6F, 0x71, 0x73)
DIM = (0x40, 0x42, 0x44)
X0, X1 = 44, 268                     # the global loop, left to right
LANE_Y = (18, 40, 62, 84)

def line(ctx, x1, y1, x2, y2, c, w=1.5):
    ctx.set_source_rgb(*(v / 255 for v in c)); ctx.set_line_width(w); ctx.set_line_cap(cairo.LINE_CAP_BUTT)
    ctx.move_to(x1, y1); ctx.line_to(x2, y2); ctx.stroke()

def text(ctx, s, x, y, size, c, align='left'):
    em = size * 0.78
    if align == 'right': x -= iosevka.width(s, em)
    elif align == 'center': x -= iosevka.width(s, em) / 2
    ctx.set_source_rgb(*(v / 255 for v in c)); ctx.move_to(x, y); iosevka.show(ctx, s, em); ctx.fill()

def frame(ctx, tracks, sel):
    for i, (speed, loop, off, rev, phase) in enumerate(tracks):
        y = LANE_Y[i]
        on = i == sel
        text(ctx, str(i + 1), 18, y + 4.5, 13.3, WHITE if on else GREY)
        line(ctx, X0, y, X1, y, DIM, 1)                                   # the global loop
        a = X0 + (X1 - X0) * off
        b = a + (X1 - X0) * loop
        line(ctx, a, y - 3, a, y + 3, WHITE if on else GREY, 1)             # own loop start / end marks
        line(ctx, b, y - 3, b, y + 3, WHITE if on else GREY, 1)
        line(ctx, a, y, b, y, WHITE if on else GREY, 2)                   # own loop
        p = a + (b - a) * phase
        line(ctx, p, y - 7, p, y + 7, RED if on else WHITE, 2)              # playhead
        if rev: line(ctx, p - 5, y - 7, p, y - 7, RED if on else WHITE, 2)  # reverse: flag to the left
        else: line(ctx, p, y - 7, p + 5, y - 7, RED if on else WHITE, 2)
        text(ctx, speed, 306, y + 4.5, 13.3, BLUE if on else GREY, 'right')
    speed, loop, off, rev, _ = tracks[sel]
    frac = lambda v: {1: '1', .5: '1/2', 1 / 3: '1/3', .25: '1/4', 2 / 3: '2/3', .75: '3/4', 0: '0'}.get(v, '%.2f' % v)
    cols = (('speed', speed, BLUE), ('loop', frac(loop), GREEN), ('offset', frac(off), WHITE), ('dir', 'rev' if rev else 'fwd', RED))
    for k, (name, val, c) in enumerate(cols):
        x = 18 + k * 76
        text(ctx, name.upper(), x, 108, 13.3, c)
        text(ctx, val.upper(), x, 128, 20, c)

def main(out=None):
    S, W, H, pad, lab = 3, 320, 160, 12, 22
    states = [('default: stock-like, all tracks 1x, full loop', [('1x', 1, 0, False, .35)] * 4, 0),
              ('free: own speed / loop / offset per track (track 3 selected)',
               [('1x', 1, 0, False, .35), ('1/2x', .5, 0, False, .7), ('1x', 2 / 3, 1 / 3, True, .4), ('1.37x', .75, .25, False, .15)], 2)]
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W * S + 2 * pad, len(states) * (H * S + pad + lab) + pad)
    ctx = cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
    for n, (label, tracks, sel) in enumerate(states):
        x0, y0 = pad, pad + n * (H * S + pad + lab)
        ctx.save(); ctx.translate(x0, y0 + 16); text(ctx, label.upper(), 0, 0, 15 / 0.78 * 0.78, WHITE); ctx.restore()
        ctx.save(); ctx.translate(x0, y0 + lab); ctx.rectangle(0, 0, W * S, H * S); ctx.clip()
        ctx.set_source_rgb(0, 0, 0); ctx.paint(); ctx.scale(S, S)
        frame(ctx, tracks, sel)
        ctx.restore()
    out = out or os.path.join(HERE, 'tapemachine_preview.png')
    surf.write_to_png(out); print('->', out)

if __name__ == '__main__':
    main(*sys.argv[1:2])
