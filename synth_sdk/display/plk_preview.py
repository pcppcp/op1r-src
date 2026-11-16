#!/usr/bin/env python3
"""Design preview of the parameter-lock page (docs/87), for approval. Header: the state (a white triangle playing, a
red dot recording, a red ring armed), the clip length (green encoder), sync / free (red encoder), the selected lane's
name and value (white encoder). Below: one row per declared lane (engine page 1 / 2 in blue, FX in green; blue
encoder selects, the selected row bracketed), each with its recorded curve over the clip and the playhead; empty
lanes a dim line; more lanes than rows scroll with the selection (a bar at the right edge).
Text in Iosevka (harness/iosevka.py; on the device the stock font), butt line caps.
    python3 display/plk_preview.py [OUT.png]"""
import math, os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); SDK = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(SDK, 'harness'))
import cairocffi as cairo, iosevka

BLUE, GREEN, WHITE, RED, GREY = (0x4D, 0x9E, 0xFF), (0x00, 0xED, 0x95), (0xFF, 0xFF, 0xFF), (0xFF, 0x3A, 0x5D), (0x6F, 0x71, 0x73)
DIM = (0x40, 0x42, 0x44)
ROWS, ROW_H, Y0, X0, X1 = 8, 12, 40, 44, 296

def line(ctx, x1, y1, x2, y2, c, w=1.5):
    ctx.set_source_rgb(*(v / 255 for v in c)); ctx.set_line_width(w); ctx.set_line_cap(cairo.LINE_CAP_BUTT)
    ctx.move_to(x1, y1); ctx.line_to(x2, y2); ctx.stroke()

def text(ctx, s, x, y, size, c, align='left'):
    em = size * 0.78
    if align == 'right': x -= iosevka.width(s, em)
    elif align == 'center': x -= iosevka.width(s, em) / 2
    ctx.set_source_rgb(*(v / 255 for v in c)); ctx.move_to(x, y); iosevka.show(ctx, s, em); ctx.fill()

def circle(ctx, x, y, r, c, fill=True):
    ctx.set_source_rgb(*(v / 255 for v in c)); ctx.set_line_width(1.5); ctx.arc(x, y, r, 0, 6.2832)
    ctx.fill() if fill else ctx.stroke()

def triangle(ctx, x, y, h, c):
    ctx.set_source_rgb(*(v / 255 for v in c)); ctx.move_to(x, y); ctx.line_to(x + h * 0.87, y + h / 2); ctx.line_to(x, y + h)
    ctx.close_path(); ctx.fill()

# lanes: (name, colour, curve: f(t 0..1) -> 0..1 or None = empty)
ENC = [BLUE, GREEN, WHITE, RED]
# lanes: (group 'e' / 'fx', knob 0..3, page 1 / 2, curve f(t) -> 0..1 or None = empty); label: a dot (page 1) or a
# colon (page 2) in the knob's encoder colour, the curve in that colour too
LANES = [('e', 0, 1, lambda t: 0.5 + 0.4 * math.sin(t * 6.283)),
         ('e', 1, 1, None),
         ('e', 2, 1, lambda t: min(1, t * 1.6) if t < 0.7 else 0.2),
         ('e', 3, 1, None),
         ('e', 1, 2, None),
         ('e', 2, 2, lambda t: 0.25 + 0.5 * (int(t * 8) % 2)),
         ('e', 3, 2, None),
         ('fx', 0, 1, lambda t: 0.8 - 0.6 * t),
         ('fx', 1, 1, None),
         ('fx', 2, 1, lambda t: 0.3 + 0.3 * math.sin(t * 18.85) ** 2),
         ('fx', 3, 1, None)]

def dot(ctx, x, y, c, r=1.75):                                 # two crossing bars (no zero-length lines on the device)
    line(ctx, x - r, y, x + r, y, c, 2 * r); line(ctx, x, y - r, x, y + r, c, 2 * r)

def marker(ctx, x, y, knob, page):                             # page 1: a dot; page 2: a colon (fits a 12 px row)
    c = ENC[knob]
    if page == 1: dot(ctx, x, y, c)
    else: dot(ctx, x, y - 2.5, c, 1); dot(ctx, x, y + 2.5, c, 1)

def frame(ctx, state, sel, head, length='2 bars', sync=True, upto=1.0):
    """state: 'play' / 'rec' / 'armed'; head: playhead 0..1; upto: recorded part so far (rec: the lanes being
    recorded end at the head)"""
    x = 8
    if state == 'rec': circle(ctx, x + 5, 15, 5, RED)
    elif state == 'armed': circle(ctx, x + 5, 15, 4.5, RED, fill=False)
    else: triangle(ctx, x, 9, 11, WHITE)
    text(ctx, 'plk', 24, 20, 13.3, GREY)
    text(ctx, length, 120, 20, 16, GREEN, 'center')
    text(ctx, 'sync' if sync else 'free', 186, 20, 13.3, RED if sync else GREY, 'center')
    g, kn, pg, f = LANES[sel]
    v = f(head) if f else 0.5
    text(ctx, g, 250, 20, 13.3, GREY, 'right')
    marker(ctx, 258, 15, kn, pg)
    text(ctx, '%d' % round(v * 100), 300, 20, 16, WHITE, 'right')
    line(ctx, 8, 30, 312, 30, DIM, 1)
    first = max(0, min(sel - ROWS // 2 + 1, len(LANES) - ROWS))   # scroll with the selection
    for r in range(ROWS):
        i = first + r
        if i >= len(LANES): break
        g, kn, pg, f = LANES[i]
        c = ENC[kn]
        yc = Y0 + r * ROW_H + ROW_H / 2
        on = i == sel
        if i and LANES[i - 1][0] != g: line(ctx, 14, yc - ROW_H / 2, X1, yc - ROW_H / 2, GREY, 0.8)   # engine | fx
        if r == 0 or LANES[i - 1][0] != g: text(ctx, g, 18, yc + 4, 9.3, GREY, 'right')                 # the group's first row
        marker(ctx, 30, yc, kn, pg)
        if not f or (state == 'armed' and False):
            line(ctx, X0, yc, X1, yc, DIM, 1)
        else:
            rec_lane = state == 'rec' and on
            end = head if rec_lane else 1.0
            pts = [(X0 + (X1 - X0) * k / 200, yc + 4.5 - 9 * f(k / 200)) for k in range(201) if k / 200 <= end]
            ctx.set_source_rgb(*(v / 255 for v in (RED if rec_lane else c))); ctx.set_line_width(1.2)
            ctx.set_line_cap(cairo.LINE_CAP_BUTT); ctx.set_line_join(cairo.LINE_JOIN_MITER)
            ctx.move_to(*pts[0])
            for p in pts[1:]: ctx.line_to(*p)
            ctx.stroke()
            if rec_lane: line(ctx, X0 + (X1 - X0) * end, yc, X1, yc, DIM, 1)
        if on:                                                 # brackets around the selected row
            for xa, d in ((X0 - 4, 3), (X1 + 4, -3)):
                line(ctx, xa, yc - 5, xa, yc + 5, WHITE, 1.2)
                line(ctx, xa, yc - 5, xa + d, yc - 5, WHITE, 1.2); line(ctx, xa, yc + 5, xa + d, yc + 5, WHITE, 1.2)
    # bar markers (the clip's bars) and the playhead
    for b in range(1, 2):
        xb = X0 + (X1 - X0) * b / 2
        line(ctx, xb, Y0 - 3, xb, Y0, GREY, 1)
    if state != 'armed':
        xh = X0 + (X1 - X0) * head
        line(ctx, xh, Y0 - 4, xh, Y0 + ROWS * ROW_H, RED if state == 'rec' else WHITE, 1.2)
    if len(LANES) > ROWS:                                      # scroll bar
        h = ROWS * ROW_H
        line(ctx, 314, Y0, 314, Y0 + h, DIM, 2)
        line(ctx, 314, Y0 + h * first / len(LANES), 314, Y0 + h * (first + ROWS) / len(LANES), GREY, 2)

def main(out):
    S = 2
    states = [('playing, 2 bars synced', dict(state='play', sel=0, head=0.37)),
              ('recording e3 (lane selected), head at 55 %', dict(state='rec', sel=2, head=0.55)),
              ('armed, waiting for the first step; fx lanes', dict(state='armed', sel=8, head=0.0, length='4 bars')),
              ('playing, free, 1/2 bar', dict(state='play', sel=9, head=0.8, length='1/2 bar', sync=False))]
    W, H, pad, lab = 320 * S, 160 * S, 12, 20
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, 2 * (W + pad) + pad, 2 * (H + pad + lab) + pad)
    ctx = cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
    for n, (label, kw) in enumerate(states):
        x0, y0 = pad + (n % 2) * (W + pad), pad + (n // 2) * (H + pad + lab)
        ctx.save(); ctx.set_source_rgb(1, 1, 1); ctx.move_to(x0, y0 + 14); iosevka.show(ctx, label, 12); ctx.fill(); ctx.restore()
        ctx.save(); ctx.translate(x0, y0 + lab); ctx.rectangle(0, 0, W, H); ctx.clip()
        ctx.set_source_rgb(0, 0, 0); ctx.paint(); ctx.scale(S, S)
        frame(ctx, **kw)
        ctx.restore()
    surf.write_to_png(out); print('->', out)

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '/tmp/plk_preview1.png')
