#!/usr/bin/env python3
"""Generate reactor.svg + reactor_geom.h: REACTOR's screen, a reactor plant and its control desk, from the owner's
image.

    python3 gen_svg.py [--frame-png OUT.png]

The art is reactor-art.webp: the owner's image (2026-11-03), 800 x 400. No vector file exists (the owner: no SVG), so
the still parts are traced with display/art_trace.py (docs/screen-art.md, "From a PNG"). It is a lossy WebP at 2.5x
the screen, under the 4x the doc asks for, so lines may sit up to about a pixel off.

The fit (the owner, 2026-11-03: the art smaller, with the readouts under it). At the image's own scale (0.4) the
drawing (y 24..365, vessel top to the desk's legs) plus the label row (to y 379) can't stay inside the OLED's bottom
margin (screen-art.md: ink <= 138). So one uniform scale, S = 320 / 888 = 0.360, centred: the frame is the 2:1 window
X -44..844, Y 17..461 of the image (black beyond its edges). The drawing lands on x 19.8..298.6, y 2.2..125.0; the
readouts go under it in the stroke font (y 128..134). Every coordinate below is in image px unless named screen px.

Colours: the image's measured colours, each written as its nearest stock / encoder colour (the owner's palette
rule): blue #1494DA -> #698EFF, green #00E078 -> #00ED95, red #FD4B28 -> #FF3A5D, white #F6F7F3 -> #FFFFFF; the
vessel's dim rails #4E606C -> #656579.

Traced 1:1 (reactor.svg, still): the plant (dome and its blue line, the stack's outline, the cooling tower and its
blue lines, the buildings, the ground), the vessel's shell, top bar, rod sockets, basket and feet, both pipes, the
desk's outline and dividers, the button's bracket. Straight runs are simplified within 0.3 screen px.

Drawn here from measured geometry (still, crisper than a trace of a soft WebP): the trees (circle fits), the eight
lamps, the four dial rings (circle fits, r 29), the four gauge frames (their corners), the vessel's five rails. The
gauge arcs (circle fits: centre under the window, r 72, 47..133 degrees) and their five ticks (90 and 90 +- 22.5,
+- 38.5 degrees) are drawn live by reactor.c from the same numbers: in the SVG they cost about 7 KB of TE heap.
Touching traced pieces of one colour are joined into one polyline (TE heap: about 200 bytes an element, 30 a vertex;
the first build measured 49,048 bytes natively). Two
fixes of the image's glitches: gauge 1's frame is drawn whole (the image breaks its bottom and right edges where its
needle smears), and every gauge window is empty but for the arc and ticks (the image has ghost needles in them).

reactor.c draws the moving parts (reactor_geom.h, from the same numbers):
  dials     a short thick needle per knob, -135..+135 degrees from up (COOLING 99 points down-right, as the image)
  gauges    a needle per gauge from the window's bottom centre to the arc (as the image): coolant (COOLING), radiation
            (RADIATION and the clicks), rods (their mean level), core temperature (critical at 80 % of the scale)
  rods      rods 2..4 rise with RODS (reactor.c rod levels), the capsules as SVG groups moved up, stems and T bars
            drawn; rod 1 stays where the image has it
  steam     two wavy strands over the tower, taller and faster with the hottest voice's temperature
  stack     its two red bands, which blink while a voice is critical
  button    the red cap (SVG node "cap"), which blinks while a voice is critical
  text      the four readouts, name and value (0..99) in the encoder colours, at the image's left x. The image's
            title REACTOR is left out (the owner, 2026-11-03)
the shake (2026-11-08): the still drawing goes into SVG groups "s0".., one part each (reactor.c moves them); elements
            move between groups only past ones of their own colour or ones they don't come within 2 px of
main() refuses the art when the still drawing or any moving part at its extremes leaves x 2..318, y 2..138.
Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""
import argparse
import math
import os
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'display'))
import art_trace  # noqa: E402
from svgart import f2, shape  # noqa: E402

ART = HERE / 'reactor-art.webp'
FX0, FY0, FW = -44, 17, 888                     # the uniform frame in image px (2:1, so 444 tall)
S = 320 / FW
INK_BOX = (2.0, 318.0, 2.0, 138.0)              # nothing drawn outside this (x0, x1, y0, y1), live parts included
MEASURED = ['#F6F7F3', '#1494DA', '#00E078', '#FD4B28']
WHITE, BLUE, GREEN, RED, RAIL = '#FFFFFF', '#698EFF', '#00ED95', '#FF3A5D', '#656579'
STOCK = {'#F6F7F3': WHITE, '#1494DA': BLUE, '#00E078': GREEN, '#FD4B28': RED}
LW = 0.85                                       # screen px: the code-drawn still lines (the trace measures ~0.95)

# the moving parts and code-drawn still parts, measured in the image (px)
DIALS = [(171.9, 311.9), (305.1, 310.6), (460.8, 311.3), (621.6, 308.7)]
DIAL_R = 29.0
DIAL_NEEDLE = (3.0, 21.0, 5.0)                  # needle from r, to r, width (image px)
GAUGE_FRAMES = [((115, 228), (240, 228), (241, 269), (98, 269)),
                ((262, 228), (389, 228), (389, 269), (255, 269)),
                ((410, 228), (535, 228), (543, 269), (410, 269)),
                ((559, 228), (682, 228), (697, 269), (567, 269))]
GAUGE_C = [(172.5, 309.7), (323.4, 309.6), (474.2, 309.9), (625.1, 310.0)]
GAUGE_R, GAUGE_A0, GAUGE_A1 = 72.2, 47.0, 133.0
GAUGE_TICKS, GAUGE_TICK = (51.5, 67.5, 90, 112.5, 128.5), 5.0   # measured: 90 +-22.5, +-38.5 degrees
NEEDLE_R1 = 69.0                                # gauge needle's outer end (image px from the centre)
NEEDLE_YMAX = 267.0                             # its pivot: the window's bottom centre, just above the frame
TREES = [(43.5, 188.4, 8.1), (72.5, 190.5, 6.0), (362.5, 189.4, 7.0)]
GROUND_Y = 209.0
LAMPS = [  # (x0, y0, x1, y1, colour, filled)
    (109, 298, 122, 310, BLUE, True), (109, 315, 122, 328, BLUE, False),
    (356, 298, 368, 310, GREEN, True), (356, 315, 369, 328, GREEN, False),
    (510, 297, 522, 310, WHITE, True), (510, 315, 523, 328, BLUE, False),
    (674, 298, 686, 309, RED, True), (674, 315, 687, 328, BLUE, False)]
RAILS_X, RAIL_Y = (488, 535, 580, 623.5, 669), (57.0, 160.0)
RODS_X = (513.5, 557.5, 602.5, 647.0)
ROD_W, ROD_TOP_H, ROD_GAP, ROD_BOT_H = 19.0, 29.0, 3.0, 34.0
ROD1_TOP = 88.0                                 # rod 1 stays here (the image)
ROD_IN, ROD_OUT = 110.0, 64.0                   # capsule top: rod fully in / fully out
STEM_Y0, T_HALF, T_GAP = 52.0, 5.5, 2.5         # stems hang from the top bar; T bar half width, gap to the capsule
STEAM_X, STEAM_Y0, STEAM_LMAX = (291.0, 300.0), 90.0, 42.0
STACK_BANDS = (217.0, 227.5, (85.5, 93.0))
CAP = (64.0, 302.5, 15.0, 8.5)                  # the button's cap: centre, radii
LABELS = [(110, 'COOLING', BLUE), (263, 'RADIATION', GREEN), (439, 'RODS', WHITE), (600, 'HEAT', RED)]
LABEL_Y, LABEL_S = 128.0, 0.8                   # screen px: top, stroke-font scale (4 x 6 grid units)
# erased before tracing (image px): what the code draws, and the text
ERASE_BOXES = [(15, 12, 200, 42), (100, 356, 700, 400), (482, 57, 678, 167), (276, 44, 320, 93)]
ERASE_COLOURED = [(212, 80, 232, 98), (40, 288, 88, 316), (540, 160, 576, 180)]   # non-white in these, then the
ERASE_WHITE = [(540, 160, 576, 180)]                                              # rod 2 tail under the basket


def sc(X, Y):
    """image px -> screen px"""
    return ((X - FX0) * S, (Y - FY0) * S)


def cleaned():
    a = np.asarray(Image.open(ART).convert('RGB')).copy()
    r, g, b = (a[..., i].astype(int) for i in range(3))
    white = (r > 150) & (g > 150) & (b > 150)
    for x0, y0, x1, y1 in ERASE_BOXES:
        a[y0:y1, x0:x1] = 0
    for x0, y0, x1, y1 in ERASE_COLOURED:
        sub = a[y0:y1, x0:x1]
        sub[~white[y0:y1, x0:x1]] = 0
    for x0, y0, x1, y1 in ERASE_WHITE:
        sub = a[y0:y1, x0:x1]
        sub[white[y0:y1, x0:x1]] = 0
    yy, xx = np.mgrid[0:a.shape[0], 0:a.shape[1]]
    for cx, cy in DIALS:                                         # dials: ring and needle (code draws both)
        a[(xx - cx) ** 2 + (yy - cy) ** 2 < 32 ** 2] = 0
    for fr in GAUGE_FRAMES:                                      # gauges: frame, arc, ticks, needles
        xs, ys = [p[0] for p in fr], [p[1] for p in fr]
        a[min(ys) - 3:max(ys) + 3, min(xs) - 3:max(xs) + 3] = 0
    a[269:284, 165:225][~white[269:284, 165:225]] = 0           # gauge 1's needle tail across the desk's edge
    for x0, y0, x1, y1, _, _ in LAMPS:
        a[y0 - 2:y1 + 3, x0 - 2:x1 + 3] = 0
    for X, Y, R in TREES:
        a[int(Y - R - 3):int(GROUND_Y - 1), int(X - R - 3):int(X + R + 4)][~white[int(Y - R - 3):int(GROUND_Y - 1),
                                                                             int(X - R - 3):int(X + R + 4)]] = 0
    # the desk's top edge under gauge 1's needle: the needle was drawn over it, so close the line again
    row = a[272:279, 160]
    for dy in range(7):
        if row[dy].sum() > 450:
            a[272 + dy, 160:230] = np.maximum(a[272 + dy, 160:230], row[dy])
    return Image.fromarray(a)


def framed(im):
    out = Image.new('RGB', (FW, FW // 2), (0, 0, 0))
    out.paste(im, (-FX0, -FY0))
    return out


def trace():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'framed.png')
        framed(cleaned()).save(p)
        r = art_trace.trace(p, MEASURED)
    widths = {q['color']: q['width'] for q in r['palette']}
    return r['strokes'], widths


def rdp(pts, eps):
    if len(pts) < 3:
        return pts
    (ax, ay), (bx, by) = pts[0], pts[-1]
    dx, dy = bx - ax, by - ay
    n = math.hypot(dx, dy)
    best, bi = -1.0, 0
    for i in range(1, len(pts) - 1):
        px, py = pts[i]
        d = abs(dy * (px - ax) - dx * (py - ay)) / n if n else math.hypot(px - ax, py - ay)
        if d > best:
            best, bi = d, i
    if best <= eps:
        return [pts[0], pts[-1]]
    return rdp(pts[:bi + 1], eps)[:-1] + rdp(pts[bi:], eps)


def plen(pts):
    return sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(pts, pts[1:]))


def join(pieces, tol=0.35):
    """open pieces of one colour that meet end to end become one polyline (R246's TE heap: ~200 bytes an element)"""
    pieces = [list(p) for p in pieces]
    merged = True
    while merged:
        merged = False
        for i in range(len(pieces)):
            for j in range(len(pieces)):
                if i == j or not pieces[i] or not pieces[j]:
                    continue
                a, b = pieces[i], pieces[j]
                for ra, rb in ((False, False), (False, True), (True, False), (True, True)):
                    pa, pb = (a[::-1] if ra else a), (b[::-1] if rb else b)
                    if math.hypot(pa[-1][0] - pb[0][0], pa[-1][1] - pb[0][1]) <= tol:
                        pieces[i], pieces[j] = pa + pb[1:], []
                        merged = True
                        break
    return [p for p in pieces if p]


def still(strokes):
    """traced pieces kept: specks dropped, open pieces joined per colour, polylines simplified"""
    out, open_ = [], {}
    for c, w, piece in strokes:
        if piece[0] == 'circle':
            continue                                              # the trace's circles are lamp / tree remnants
        pts, closed = piece[1], piece[2]
        if plen(pts) < 2.0:
            continue
        if closed:
            out.append((STOCK[c], max(w, 0.7), rdp(pts, 0.3), True))
        else:
            open_.setdefault((STOCK[c], max(w, 0.7)), []).append(pts)
    for (col, w), ps in open_.items():
        out += [(col, w, rdp(p, 0.3), False) for p in join(ps)]
    return out


def arc_pts(cx, cy, r, a0, a1, step=4.0):
    n = max(2, int(abs(a1 - a0) / step) + 1)
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / (n - 1))),
             cy - r * math.sin(math.radians(a0 + (a1 - a0) * i / (n - 1)))) for i in range(n)]


def rod_shapes(x, top):
    """a rod's capsule (image px): the upper part (round top) and the lower part (round bottom)"""
    r = ROD_W / 2
    up = arc_pts(x, top + r, r, 180, 0, 20) + [(x + r, top + ROD_TOP_H), (x - r, top + ROD_TOP_H)]
    y2 = top + ROD_TOP_H + ROD_GAP
    lo = [(x - r, y2), (x + r, y2)] + arc_pts(x, y2 + ROD_BOT_H - r, r, 0, -180, 20)
    return up, lo


def fill(pts, col):
    q = [sc(*p) for p in pts]
    d = 'M' + ' L'.join(f'{f2(x)} {f2(y)}' for x, y in q) + ' Z'
    return f'<path d="{d}" fill="{col}" stroke="none"/>'


# ---- the shake (2026-11-08, the owner's pick after the meltdown was dropped: docs/design/reactor-meltdown/ in
# OP1-Emulator) ----
# The plant's elements go into SVG groups, one part each, that reactor.c moves (transforms) while the core runs away.
# Elements are reordered so each part's elements sit together, but never past one of another colour that they come
# within 2 px of (so at rest the pixels are exactly the old drawing's); each run of one part is a group "s0"..
PARTS = ['desk', 'ground', 'dome', 'chimney', 'tower', 'pipes', 'vessel']


def part_of(pts):
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    if max(ys) > 74 or cy > 72:
        return 'desk'
    if 160 <= cx <= 192 or cx >= 262:
        return 'pipes'
    if 186 < cx < 262:
        return 'vessel'
    if 88 <= cx <= 104 and max(ys) - min(ys) > 20:
        return 'chimney'
    if 104 < cx < 145 and cy < 62:
        return 'tower'
    if cx < 90 and cy < 62:
        return 'dome'
    return 'ground'


def elements(traced):
    """the still drawing in its order: (svg text, colour, points for the part and the overlap test)"""
    e = []
    for x in RAILS_X:                                            # behind the rods
        e.append((shape([sc(x, RAIL_Y[0]), sc(x, RAIL_Y[1])], RAIL, 0.6), RAIL, [sc(x, RAIL_Y[0]), sc(x, RAIL_Y[1])]))
    for col, w, pts, closed in traced:
        e.append((shape(pts, col, w, closed=closed), col, list(pts) + ([pts[0]] if closed else [])))
    for X, Y, R in TREES:
        (x, y), r = sc(X, Y), R * S
        ring = [(x + r * math.cos(t / 8 * math.pi), y + r * math.sin(t / 8 * math.pi)) for t in range(17)]
        e.append((f'<circle cx="{f2(x)}" cy="{f2(y)}" r="{f2(r)}" fill="none" stroke="{GREEN}" stroke-width="{f2(LW)}"/>',
                  GREEN, ring))
        e.append((shape([sc(X, Y + R), sc(X, GROUND_Y)], GREEN, LW), GREEN, [sc(X, Y + R), sc(X, GROUND_Y)]))
    for x0, y0, x1, y1, col, filled in LAMPS:
        box = [sc(x0, y0), sc(x1, y0), sc(x1, y1), sc(x0, y1), sc(x0, y0)]
        if filled:
            e.append((fill([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], col), col, box))
        else:
            e.append((shape([sc(x0 + 1, y0 + 1), sc(x1 - 1, y0 + 1), sc(x1 - 1, y1 - 1), sc(x0 + 1, y1 - 1)], col, LW,
                            closed=True), col, box))
    for cx, cy in DIALS:
        (x, y), r = sc(cx, cy), DIAL_R * S
        ring = [(x + r * math.cos(t / 8 * math.pi), y + r * math.sin(t / 8 * math.pi)) for t in range(17)]
        e.append((f'<circle cx="{f2(x)}" cy="{f2(y)}" r="{f2(r)}" fill="none" stroke="{WHITE}" '
                  f'stroke-width="{f2(LW)}"/>', WHITE, ring))
    for fr in GAUGE_FRAMES:
        pts = [sc(*p) for p in fr]
        e.append((shape(pts, BLUE, 1.1, closed=True), BLUE, pts + [pts[0]]))   # arcs and ticks: reactor.c (TE heap)
    return e


def dense(pts, d=0.5):
    out = [pts[0]]
    for a, b in zip(pts, pts[1:]):
        n = max(1, math.ceil(math.hypot(b[0] - a[0], b[1] - a[1]) / d))
        out += [(a[0] + (b[0] - a[0]) * i / n, a[1] + (b[1] - a[1]) * i / n) for i in range(1, n + 1)]
    return out


def grouped(traced):
    """[(part, [svg text, ...])]: the elements in an order that keeps every pixel, gathered into runs of one part"""
    e = elements(traced)
    parts = [part_of(p) for _, _, p in e]
    dn = [dense(p) for _, _, p in e]
    bb = [(min(x for x, _ in p) - 2, min(y for _, y in p) - 2, max(x for x, _ in p) + 2, max(y for _, y in p) + 2) for p in dn]

    def touch(i, j):
        if e[i][1] == e[j][1]:
            return False                                         # one colour over itself blends the same either way
        a, b = bb[i], bb[j]
        if a[0] > b[2] or b[0] > a[2] or a[1] > b[3] or b[1] > a[3]:
            return False
        return any(math.hypot(px - qx, py - qy) < 2.0 for px, py in dn[i] for qx, qy in dn[j])
    before = {j: {i for i in range(j) if touch(i, j)} for j in range(len(e))}
    done, order, cur = set(), [], None
    while len(order) < len(e):
        ready = [j for j in range(len(e)) if j not in done and before[j] <= done]
        pick = [j for j in ready if parts[j] == cur] or ready
        if pick is ready and cur is not None:                    # a new run: the part with the most ready
            best = max(PARTS, key=lambda p: sum(parts[j] == p for j in ready))
            pick = [j for j in ready if parts[j] == best]
        j = pick[0]
        order.append(j); done.add(j); cur = parts[j]
    runs = []
    for j in order:
        if runs and runs[-1][0] == parts[j]:
            runs[-1][1].append(e[j][0]); runs[-1][2].extend(e[j][2])
        else:
            runs.append((parts[j], [e[j][0]], list(e[j][2])))
    return runs


def svg(traced):
    o = ['<?xml version="1.0" encoding="utf-8"?>',
         '<svg version="1.1" id="Layer_1" xmlns="http://www.w3.org/2000/svg" x="0px" y="0px"',
         '\t width="320px" height="160px" viewBox="0 0 320 160">',
         '<!-- generated by gen_svg.py from reactor-art.webp (traced + measured); needles, steam, bands and text by '
         'code; groups s0.. shake -->']
    for i, (part, els, _) in enumerate(grouped(traced)):
        o.append(f'<g id="s{i}">' + ''.join(els) + '</g>')
    for i, x in enumerate(RODS_X):                               # rods: 2..4 drawn fully in, reactor.c lifts them
        up, lo = rod_shapes(x, ROD1_TOP if i == 0 else ROD_IN)
        body = fill(up, WHITE) + fill(lo, WHITE)
        o.append(f'<g id="rod{i + 1}">{body}</g>')
    o.append(f'<g id="cap">{fill(ellipse(*CAP), RED)}</g>')
    o.append('</svg>')
    return '\n'.join(o) + '\n'


def ellipse(cx, cy, rx, ry, n=20):
    return [(cx + rx * math.cos(2 * math.pi * i / n), cy + ry * math.sin(2 * math.pi * i / n)) for i in range(n)]


def q10(v):
    return int(round(v * 1024))


def header(traced):
    h = ['/* generated by gen_svg.py from reactor-art.webp: the moving parts of REACTOR\'s screen. Screen px in Q10 '
         'unless named. */',
         '#ifndef REACTOR_GEOM_H', '#define REACTOR_GEOM_H', '#include <stdint.h>']

    def arr(name, vals):
        h.append(f'static const int32_t {name}[{len(vals)}] = {{ ' + ', '.join(str(v) for v in vals) + ' };')
    arr('RG_DIAL_X', [q10(sc(x, y)[0]) for x, y in DIALS])
    arr('RG_DIAL_Y', [q10(sc(x, y)[1]) for x, y in DIALS])
    h.append(f'#define RG_DIAL_R0 {q10(DIAL_NEEDLE[0] * S)}')
    h.append(f'#define RG_DIAL_R1 {q10(DIAL_NEEDLE[1] * S)}')
    h.append(f'#define RG_DIAL_W {q10(DIAL_NEEDLE[2] * S)}')
    arr('RG_GAUGE_X', [q10(sc(x, y)[0]) for x, y in GAUGE_C])
    arr('RG_GAUGE_Y', [q10(sc(x, y)[1]) for x, y in GAUGE_C])
    h.append(f'#define RG_GAUGE_R1 {q10(NEEDLE_R1 * S)}')
    h.append(f'#define RG_GAUGE_R {q10(GAUGE_R * S)}   /* the arc (drawn live, as the ticks: no TE heap) */')
    h.append(f'#define RG_GAUGE_TICK {q10(GAUGE_TICK * S)}')
    h.append(f'#define RG_ARC_A0 {round(GAUGE_A0 / 360 * 8192)}')
    h.append(f'#define RG_ARC_A1 {round(GAUGE_A1 / 360 * 8192)}')
    arr('RG_TICK_A', [round(a / 360 * 8192) for a in GAUGE_TICKS])
    h.append(f'#define RG_GAUGE_YMAX {q10(sc(0, NEEDLE_YMAX)[1])}   /* the needle\'s pivot y (x = the arc\'s centre) */')
    h.append(f'#define RG_GAUGE_A0 {round(GAUGE_A1 / 360 * 8192)}   /* value 0: 133 degrees (8192 = a turn) */')
    h.append(f'#define RG_GAUGE_A1 {round(GAUGE_A0 / 360 * 8192)}   /* value 1: 47 degrees */')
    arr('RG_ROD_X', [q10(sc(x, 0)[0]) for x in RODS_X])
    h.append(f'#define RG_ROD1_TOP {q10(sc(0, ROD1_TOP)[1])}')
    h.append(f'#define RG_ROD_IN {q10(sc(0, ROD_IN)[1])}   /* capsule top, fully in (as drawn in the SVG) */')
    h.append(f'#define RG_ROD_OUT {q10(sc(0, ROD_OUT)[1])}')
    h.append(f'#define RG_STEM_Y0 {q10(sc(0, STEM_Y0)[1])}')
    h.append(f'#define RG_T_HALF {q10(T_HALF * S)}')
    h.append(f'#define RG_T_GAP {q10(T_GAP * S)}')
    arr('RG_STEAM_X', [q10(sc(x, 0)[0]) for x in STEAM_X])
    h.append(f'#define RG_STEAM_Y0 {q10(sc(0, STEAM_Y0)[1])}')
    h.append(f'#define RG_STEAM_LMAX {q10(STEAM_LMAX * S)}')
    h.append(f'#define RG_STEAM_AMP {q10(3.0 * S)}')
    x0, x1, ys = STACK_BANDS
    h.append(f'#define RG_BAND_X0 {q10(sc(x0, 0)[0])}')
    h.append(f'#define RG_BAND_X1 {q10(sc(x1, 0)[0])}')
    arr('RG_BAND_Y', [q10(sc(0, y)[1]) for y in ys])
    arr('RG_LABEL_X', [q10(sc(x, 0)[0]) for x, _, _ in LABELS])
    h.append(f'#define RG_LABEL_Y {q10(LABEL_Y)}')
    h.append(f'#define RG_LABEL_S {q10(LABEL_S)}')
    runs = grouped(traced)
    h.append(f'/* the shake: SVG groups "s0".."s{len(runs) - 1}", the part each moves with ({", ".join(PARTS)}) */')
    h.append(f'#define RS_NGROUP {len(runs)}')
    h.append('static const uint8_t RS_GROUP_PART[RS_NGROUP] = { ' + ', '.join(str(PARTS.index(p)) for p, _, _ in runs) + ' };')
    h.append('/* each group\'s ink box, Q10 x0, y0, x1, y1 (strokes included: the host test checks where it can shake to) */')
    h.append('static const int32_t RS_GROUP_BOX[RS_NGROUP][4] = { ' + ', '.join(
        '{{ {}, {}, {}, {} }}'.format(q10(min(x for x, _ in pts) - 0.7), q10(min(y for _, y in pts) - 0.7),
                                      q10(max(x for x, _ in pts) + 0.7), q10(max(y for _, y in pts) + 0.7))
        for _, _, pts in runs) + ' };')
    h.append('#endif')
    return '\n'.join(h) + '\n'


def bounds(traced):
    """every ink extent in screen px: the still art, and the moving parts at their extremes"""
    pts = []
    for col, w, p, closed in traced:
        pts += [(x - w / 2, y - w / 2) for x, y in p] + [(x + w / 2, y + w / 2) for x, y in p]
    for cx, cy in GAUGE_C:                                       # needle at both ends of its travel
        for a in (GAUGE_A0, GAUGE_A1):
            pts.append(sc(cx + NEEDLE_R1 * math.cos(math.radians(a)), cy - NEEDLE_R1 * math.sin(math.radians(a))))
    for x in RODS_X:
        pts += [sc(x - ROD_W / 2, ROD_OUT - T_GAP - 1), sc(x + ROD_W / 2, ROD_IN + ROD_TOP_H + ROD_GAP + ROD_BOT_H)]
    for x in STEAM_X:
        pts += [sc(x - 4, STEAM_Y0 - STEAM_LMAX - 1), sc(x + 4, STEAM_Y0)]
    for x, name, _ in LABELS:
        n = len(name) + 3                                          # "NAME 99"
        pts += [(sc(x, 0)[0], LABEL_Y - 0.5), (sc(x, 0)[0] + (6 * n - 2) * LABEL_S + 0.5, LABEL_Y + 6 * LABEL_S + 0.5)]
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return min(xs), max(xs), min(ys), max(ys)


def labels_fit():
    """each readout ends before the next one starts (widest value, 99)"""
    for (x, name, _), (nx, _, _) in zip(LABELS, LABELS[1:]):
        end = sc(x, 0)[0] + (6 * (len(name) + 3) - 2) * LABEL_S
        if end > sc(nx, 0)[0] - 2:
            sys.exit(f'gen_svg: readout {name} 99 runs into the next one ({end:.1f} > {sc(nx, 0)[0] - 2:.1f})')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--frame-png')
    a = ap.parse_args()
    if a.frame_png:
        framed(Image.open(ART).convert('RGB')).save(a.frame_png)
    strokes, widths = trace()
    traced = still(strokes)
    x0, x1, y0, y1 = bounds(traced)
    if x0 < INK_BOX[0] or x1 > INK_BOX[1] or y0 < INK_BOX[2] or y1 > INK_BOX[3]:
        sys.exit(f'gen_svg: ink at x {x0:.1f}..{x1:.1f}, y {y0:.1f}..{y1:.1f} leaves {INK_BOX}')
    labels_fit()
    (HERE / 'reactor.svg').write_text(svg(traced))
    (HERE / 'reactor_geom.h').write_text(header(traced))
    print(f'reactor.svg: {len(traced)} traced pieces, widths {widths}; ink x {x0:.1f}..{x1:.1f}, y {y0:.1f}..{y1:.1f}')


if __name__ == '__main__':
    main()
