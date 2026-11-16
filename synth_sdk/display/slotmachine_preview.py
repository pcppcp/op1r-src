#!/usr/bin/env python3
"""Design preview of the slot machine page (docs/85), for approval. Top: the selected slot's settings (its number
boxed, as the tape page; its time as position / length; its channel; loop). Below: the 12 slots as a 3 x 4 grid of
bars, each the slot's 2:00 with its recorded length filled in its channel's colour (channel 1..4 = blue, green, white,
red), the play position as a white tick, the selected slot framed, a slot recording in red.
Text in Iosevka (harness/iosevka.py; on the device the stock font), butt line caps.
    python3 display/slotmachine_preview.py [OUT.png]"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); SDK = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(SDK, 'harness'))
import cairocffi as cairo, iosevka

BLUE, GREEN, WHITE, RED, GREY = (0x4D, 0x9E, 0xFF), (0x00, 0xED, 0x95), (0xFF, 0xFF, 0xFF), (0xFF, 0x3A, 0x5D), (0x6F, 0x71, 0x73)
DIM = (0x40, 0x42, 0x44)
CH = [BLUE, GREEN, WHITE, RED]
MAX = 120.0                                                   # seconds per slot

def line(ctx, x1, y1, x2, y2, c, w=1.5):
    ctx.set_source_rgb(*(v / 255 for v in c)); ctx.set_line_width(w); ctx.set_line_cap(cairo.LINE_CAP_BUTT)
    ctx.move_to(x1, y1); ctx.line_to(x2, y2); ctx.stroke()

def text(ctx, s, x, y, size, c, align='left'):
    em = size * 0.78
    if align == 'right': x -= iosevka.width(s, em)
    elif align == 'center': x -= iosevka.width(s, em) / 2
    ctx.set_source_rgb(*(v / 255 for v in c)); ctx.move_to(x, y); iosevka.show(ctx, s, em); ctx.fill()

def circle(ctx, x, y, r, c):
    ctx.set_source_rgb(*(v / 255 for v in c)); ctx.arc(x, y, r, 0, 6.2832); ctx.fill()

def triangle(ctx, x, y, h, c):                                # play: pointing right, left edge at x, top at y
    ctx.set_source_rgb(*(v / 255 for v in c)); ctx.move_to(x, y); ctx.line_to(x + h * 0.87, y + h / 2); ctx.line_to(x, y + h)
    ctx.close_path(); ctx.fill()

def mmss(t):
    return '%d:%02d' % (int(t) // 60, int(t) % 60)

# slot: (length s, channel 0..3, playing position s or None, recording)
def frame(ctx, slots, sel):
    ln, ch, pos, rec = slots[sel]
    # top: the selected slot's settings
    for (a, b, c, d) in ((6, 5, 30, 5), (30, 5, 30, 27), (30, 27, 6, 27), (6, 27, 6, 5)): line(ctx, a, b, c, d, WHITE, 1.2)
    text(ctx, str(sel + 1), 18, 23.5, 20 if sel < 9 else 16, WHITE, 'center')
    shown = pos if pos is not None else ln
    text(ctx, mmss(shown), 148, 24, 26, RED if rec else WHITE, 'right')
    text(ctx, '/ ' + mmss(ln if not rec else MAX), 154, 24, 13.3, GREY)
    text(ctx, 'ch', 262, 15, 13.3, GREY, 'right')
    text(ctx, str(ch + 1), 274, 15, 13.3, CH[ch], 'right')
    text(ctx, 'loop', 312, 15, 13.3, GREEN, 'right')
    if rec: circle(ctx, 304, 24, 5, RED)                     # record: a red circle
    elif pos is not None: triangle(ctx, 299, 18, 11, WHITE)  # play: a white triangle
    # the 12 slots: 3 rows x 4 columns
    x0, gap, w = 10, 10, (300 - 3 * 10) / 4
    for s, (l, c, p, r) in enumerate(slots):
        row, col = divmod(s, 4)
        x, y = x0 + col * (w + gap), 58 + row * 30
        on = s == sel
        text(ctx, str(s + 1), x, y - 7, 10.7, WHITE if on else GREY)
        line(ctx, x, y, x + w, y, DIM, 1)                     # the 2:00
        if l > 0 or r:
            fill = (p if r else l) / MAX * w
            line(ctx, x, y, x + fill, y, CH[c], 6)           # recording too: its channel (the header shows rec)
        if p is not None and not r:
            px = x + p / MAX * w
            line(ctx, px, y - 6, px, y + 6, WHITE, 1.5)
        if on:                                                # the selected slot: corner brackets
            for (a, b) in ((x - 3, y - 5), (x + w + 3, y - 5)):
                d = 3 if a < x else -3
                line(ctx, a, b, a, b + 10, WHITE, 1.2); line(ctx, a, b, a + d, b, WHITE, 1.2); line(ctx, a, b + 10, a + d, b + 10, WHITE, 1.2)

def main(out=None):
    S, W, H, pad, lab = 3, 320, 160, 12, 22
    idle = [(95, 0, None, False), (32, 1, None, False), (0, 0, None, False), (118, 2, None, False),
            (61, 3, None, False), (44, 1, None, False), (0, 0, None, False), (12, 0, None, False),
            (78, 2, None, False), (0, 0, None, False), (120, 3, None, False), (5, 1, None, False)]
    busy = [(95, 0, 41, False), (32, 1, None, False), (0, 0, None, False), (118, 2, 77, False),
            (61, 3, None, False), (44, 1, 12, False), (0, 0, None, False), (12, 0, 9, False),
            (78, 2, None, False), (0, 0, None, False), (0, 3, 23, True), (5, 1, None, False)]
    states = [('idle, slot 6 selected (channel 2)', idle, 5),
              ('slots 1, 4, 6, 8 playing (loop), slot 11 recording (selected)', busy, 10)]
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W * S + 2 * pad, len(states) * (H * S + pad + lab) + pad)
    ctx = cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
    for n, (label, slots, sel) in enumerate(states):
        x0, y0 = pad, pad + n * (H * S + pad + lab)
        ctx.save(); ctx.translate(x0, y0 + 16); text(ctx, label.upper(), 0, 0, 15, WHITE); ctx.restore()
        ctx.save(); ctx.translate(x0, y0 + lab); ctx.rectangle(0, 0, W * S, H * S); ctx.clip()
        ctx.set_source_rgb(0, 0, 0); ctx.paint(); ctx.scale(S, S)
        frame(ctx, slots, sel)
        ctx.restore()
    out = out or os.path.join(HERE, 'slotmachine_preview.png')
    surf.write_to_png(out); print('->', out)

if __name__ == '__main__':
    main(*sys.argv[1:2])
