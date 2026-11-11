#!/usr/bin/env python3
"""Design preview v2 of the free tape machine screen (docs/82), for approval: closer to the stock tape screen.
Active track: its number boxed top-left, its head position as a time, its speed (blue) and rev (red) on top; a
timeline window around its own head (clips blue, its loop green, head fixed in the middle); the knob values below in
encoder colours. Bottom strip: all four tracks as thin lanes around their own heads, the active one highlighted.
Text in Iosevka (harness/iosevka.py; on the device the stock font, lowercase), butt line caps.
    python3 display/tapemachine_preview2.py [OUT.png]"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); SDK = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(SDK, 'harness'))
import cairocffi as cairo, iosevka

BLUE, GREEN, WHITE, RED, GREY = (0x4D, 0x9E, 0xFF), (0x00, 0xED, 0x95), (0xFF, 0xFF, 0xFF), (0xFF, 0x3A, 0x5D), (0x6F, 0x71, 0x73)
DIM = (0x40, 0x42, 0x44)
SR = 44100

def line(ctx, x1, y1, x2, y2, c, w=1.5):
    ctx.set_source_rgb(*(v / 255 for v in c)); ctx.set_line_width(w); ctx.set_line_cap(cairo.LINE_CAP_BUTT)
    ctx.move_to(x1, y1); ctx.line_to(x2, y2); ctx.stroke()

def text(ctx, s, x, y, size, c, align='left'):
    em = size * 0.78
    if align == 'right': x -= iosevka.width(s, em)
    elif align == 'center': x -= iosevka.width(s, em) / 2
    ctx.set_source_rgb(*(v / 255 for v in c)); ctx.move_to(x, y); iosevka.show(ctx, s, em); ctx.fill()

def lane(ctx, y, x0, x1, head, clips, loop, win, thick, on, big=False):
    """a track around its own head: clips [(a, b)] and loop (a, b) in seconds, window +-win s"""
    cx = (x0 + x1) / 2
    X = lambda t: cx + (t - head) / win * (x1 - x0) / 2
    clip = lambda v: max(x0, min(x1, v))
    line(ctx, x0, y, x1, y, DIM, 1)
    for a, b in clips:
        if X(b) > x0 and X(a) < x1: line(ctx, clip(X(a)), y, clip(X(b)), y, BLUE if on else GREY, thick)   # active blue, others grey (as stock)
    if loop:
        a, b = loop
        g = GREEN if on else (0x00, 0x77, 0x4A)
        r = 3 if big else 2.5                                   # bottom lanes: end ticks only (no bracket into the next lane)
        for v in (a, b):
            if x0 <= X(v) <= x1: line(ctx, X(v), y - thick / 2 - r, X(v), y + thick / 2 + r, g, 1.2)
        if big: line(ctx, clip(X(a)), y - thick / 2 - 3, clip(X(b)), y - thick / 2 - 3, g, 1.2)

def frame(ctx, tracks, sel):
    head, speed, rev, clips, loop, vals = tracks[sel]
    # boxed track number, top-left (as stock)
    for (a, b, c, d) in ((6, 5, 28, 5), (28, 5, 28, 27), (28, 27, 6, 27), (6, 27, 6, 5)): line(ctx, a, b, c, d, WHITE, 1.2)
    text(ctx, str(sel + 1), 17, 23.5, 22, WHITE, 'center')
    m, s = int(head) // 60, head % 60
    text(ctx, '%d:%02d:%02d' % (m // 60, m % 60, int(s)), 160, 24, 26, WHITE, 'center')
    text(ctx, speed, 312, 15, 13.3, BLUE, 'right')
    if rev: text(ctx, 'rev', 312, 29, 13.3, RED, 'right')
    # active track: timeline around its head, head fixed in the middle
    lane(ctx, 68, 10, 310, head, clips, loop, 8, 9, True, True)
    line(ctx, 160, 48, 160, 88, WHITE, 1.5)
    d = -6 if rev else 6
    line(ctx, 160, 48, 160 + d, 48, RED if rev else WHITE, 1.5)
    # bottom strip: all four tracks around their own heads, active highlighted
    for t, (h, _, r, cl, lp, _) in enumerate(tracks):
        y = 108 + 8 * t
        on = t == sel
        lane(ctx, y, 10, 310, h, cl, lp, 20, 3, on)
        line(ctx, 160, y - 3, 160, y + 3, WHITE if on else GREY, 1)

def main(out=None):
    S, W, H, pad, lab = 3, 320, 160, 12, 22
    clips = [[(0.5, 6.0), (9.0, 14.5)], [(1.0, 3.0), (4.0, 12.0)], [(2.0, 20.0)], [(6.0, 7.5), (11.0, 13.0)]]
    states = [('track 1 active, 1x, no loop',
               [(5.2, '1x', False, clips[0], None, ('1x', '1', '0', 'fwd')), (5.2, '1x', False, clips[1], None, ('1x', '1', '0', 'fwd')),
                (5.2, '1x', False, clips[2], None, ('1x', '1', '0', 'fwd')), (5.2, '1x', False, clips[3], None, ('1x', '1', '0', 'fwd'))], 0),
              ('track 2 active, 1/2x rev, own loops (global loop 1 .. 13 s)',
               [(7.3, '1x', False, clips[0], (1.0, 13.0), ('1x', '1', '0', 'fwd')), (4.6, '1/2x', True, clips[1], (4.0, 10.0), ('1/2x', '1/2', '1/4', 'rev')),
                (11.9, '3/2x', False, clips[2], (1.0, 9.0), ('3/2x', '2/3', '0', 'fwd')), (6.8, '1x', False, clips[3], (5.5, 8.5), ('1x', '1/4', '3/8', 'fwd'))], 1)]
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W * S + 2 * pad, len(states) * (H * S + pad + lab) + pad)
    ctx = cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
    for n, (label, tracks, sel) in enumerate(states):
        x0, y0 = pad, pad + n * (H * S + pad + lab)
        ctx.save(); ctx.translate(x0, y0 + 16); text(ctx, label.upper(), 0, 0, 15, WHITE); ctx.restore()
        ctx.save(); ctx.translate(x0, y0 + lab); ctx.rectangle(0, 0, W * S, H * S); ctx.clip()
        ctx.set_source_rgb(0, 0, 0); ctx.paint(); ctx.scale(S, S)
        frame(ctx, tracks, sel)
        ctx.restore()
    out = out or os.path.join(HERE, 'tapemachine_preview2.png')
    surf.write_to_png(out); print('->', out)

if __name__ == '__main__':
    main(*sys.argv[1:2])
