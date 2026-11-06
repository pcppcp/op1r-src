#!/usr/bin/env python3
"""Writes eva.svg and eva_geom.h, the EVA screen: the artist's astronaut (eva-art.svg) in firmware primitives.

    python3 synth_sdk/plugins/eva/gen_svg.py [--state rest|low|high|design] [-o FILE]

eva-art.svg is the artist's vector drawing, already 320 x 160 screen units, so it is converted 1:1: its curves
(cubic paths) and ellipses are flattened into polylines and closed M/L/Z paths, opacity is mixed into the colour
over black, black fills become #010101 (R246 hides with that). Its full-screen black rect is left out (the screen is
black). Its text (PRESSURE, FREQ, OXYGEN, COM; Flama 12 px) is drawn in a stroke font made for this screen, and its
meter ticks are rebuilt as moving parts at the same places (one even tick spacing; the artist's is within 0.11 px of it).
R246 draws only line, circle, path (M/L/Z), polyline and g; at most two decimals per number.
Moving parts (ids eva.c's screen code looks up; their numbers go to eva_geom.h):
  m0..m3  meter masks: a black box with 16 dim ticks; eva.c slides it right one tick (TICK_DX px) per lit tick;
          a black cover right of the meters hides the dim ticks pushed past tick 15
  cross   the visor crosshair (FREQ): eva.c moves it from the left eye (0 %, the artist's spot) to the right eye
          (100 %). A black disc inside its ring hides the left eye under it (the artist's crosshair is that eye; the
          eye drawn here shows once the crosshair moves away)
  fog     visor fog over the lower face (OXYGEN), scaled up from the visor's bottom edge (FOG_PIVOT)
  strip   the red radio strip on the visor (COM)
  bar0..3 iter's hidden bars (the firmware's own screen code looks them up)
--state writes a static preview for the PNGs, moving parts placed as eva.c would: rest = the default knobs,
low = all 0, high = all at max, design = the lit ticks of the artist's drawing (8, 4, 13, 10) to compare with it.
Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""
import argparse
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'display'))
from svgart import W, art, element, ellipse, f2, mix, shape, subpaths   # noqa: E402
ART = HERE / 'eva-art.svg'
BLUE, GREEN, WHITE, RED, FACE, BLACK = '#4D9EFF', '#00ED95', '#FFFFFF', '#FF3A5D', '#434645', '#010101'
NAVY = '#2F2C5F'                          # the artist's stars: #4E4A9E at 60 % over black
# meters (the artist's): first tick x, spacing, first tick top, row pitch, tick height; labels: x of the first stem
# (its left edge on the ticks' left edge), cap height (Flama 12 px), first baseline
TICK_X, TICK_DX, TICK_TOP, ROW_PITCH, TICK_H = 208.46, (293.36 - 208.46) / 15, 25.44, 36.85, 9.33
LABEL_X, LABEL_H, LABEL_BASE = 208.3, 8.5, 20.75
ROWS = [(BLUE, 'PRESSURE'), (GREEN, 'FREQ'), (WHITE, 'OXYGEN'), (RED, 'COM')]
TICKS = 16
CROSS, CROSS_R, CROSS_ARM = (101.85, 71.14), 5.42, 6.77     # the artist's crosshair: ring radius, arm length
RIGHT_EYE_X = 121.53                                        # the crosshair's spot at FREQ 100 % (0 %: the artist's)
FOG_PIVOT = (110.6, 101.4)                                  # the bottom of the visor's inner edge


def is_full_rect(el):
    return el.tag.endswith('rect') and el.get('width') == '320' and el.get('height') == '160'


def in_cross(geo):
    x, y = geo[0][0][0]
    return math.hypot(x - CROSS[0], y - CROSS[1]) < 9 and not (x < 92)    # not the HUD lines left of it


# ---- stroke font (cap height 10, y down) ----

def earc(cx, cy, rx, ry, a0, a1, n=None):
    n = n or max(3, int(abs(a1 - a0) / 15))
    return [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + ry * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]


GLYPHS = {
    'T': (6.4, [[(0,0),(6.4,0)],[(3.2,0),(3.2,10)]]),
    'P': (6.4, [[(0, 10), (0, 0), (3.6, 0)] + earc(3.6, 2.7, 2.8, 2.7, -90, 90)[1:] + [(0, 5.4)]]),
    'R': (6.6, [[(0, 10), (0, 0), (3.6, 0)] + earc(3.6, 2.7, 2.8, 2.7, -90, 90)[1:] + [(0, 5.4)],
                [(3.4, 5.4), (6.4, 10)]]),
    'E': (5.8, [[(5.8, 0), (0, 0), (0, 10), (5.8, 10)], [(0, 5), (5, 5)]]),
    'S': (6.2, [earc(3.1, 2.7, 3.0, 2.7, -25, -270) + earc(3.1, 7.6, 3.1, 2.4, -90, 155)[1:]]),
    'U': (6.4, [[(0, 0)] + earc(3.2, 6.8, 3.2, 3.2, 180, 0) + [(6.4, 0)]]),
    'F': (5.6, [[(5.6, 0), (0, 0), (0, 10)], [(0, 5), (4.8, 5)]]),
    'Q': (6.8, [earc(3.4, 5, 3.4, 5, 0, 360, 24), [(4.2, 7.6), (6.9, 10.6)]]),
    'O': (6.8, [earc(3.4, 5, 3.4, 5, 0, 360, 24)]),
    'X': (6.4, [[(0, 0), (6.4, 10)], [(6.4, 0), (0, 10)]]),
    'Y': (6.4, [[(0, 0), (3.2, 5.2), (6.4, 0)], [(3.2, 5.2), (3.2, 10)]]),
    'G': (6.8, [earc(3.4, 5, 3.4, 5, -40, -360, 22) + [(3.8, 5)]]),
    'N': (6.4, [[(0, 10), (0, 0), (6.4, 10), (6.4, 0)]]),
    'C': (6.6, [earc(3.4, 5, 3.4, 5, -40, -320, 20)]),
    'M': (7.8, [[(0, 10), (0, 0), (3.9, 7.2), (7.8, 0), (7.8, 10)]]),
}
ADVANCE_GAP = 2.2


def label(text, x, base, color):
    """text at x, baseline (screen px), cap height LABEL_H"""
    k = LABEL_H / 10
    out = []
    for ch in text:
        w, strokes = GLYPHS[ch]
        out += [shape([(x + px * k, base - LABEL_H + py * k) for px, py in s], color, 1.2) for s in strokes]
        x += (w + ADVANCE_GAP) * k
    return out


# ---- the moving parts ----

def cross():
    """the artist's crosshair, its black cut-outs (a ring through the arms, gaps in the ring beside each arm) drawn as
    gaps instead, so it hides nothing when it moves; plus a black disc inside the ring over the left eye"""
    cx, cy = CROSS
    out = [f'<circle cx="{f2(cx)}" cy="{f2(cy)}" r="4.6" fill="{BLACK}" stroke="{BLACK}" stroke-width="0.2"/>']
    for ux, uy in ((1, 0), (0, 1)):                       # the centre plus: inside the black ring (r 1.91 .. 3.41)
        out.append(shape([(cx - 1.16 * ux, cy - 1.16 * uy), (cx + 1.16 * ux, cy + 1.16 * uy)], GREEN))
    for ux, uy in ((1, 0), (-1, 0), (0, 1), (0, -1)):     # the arms outside it
        out.append(shape([(cx + 4.16 * ux, cy + 4.16 * uy), (cx + CROSS_ARM * ux, cy + CROSS_ARM * uy)], GREEN))
    # ring: cut straight where the artist's black rects (out to 2.11 px either side of an arm) cover it, so each
    # quarter is a filled band: outer arc, the cut, inner arc, the cut
    ro, ri, c = CROSS_R + W / 2, CROSS_R - W / 2, 2.11
    arc = lambda r: [(r * math.cos(math.radians(a)), r * math.sin(math.radians(a)))
                     for a in (math.degrees(math.asin(c / r)) + (90 - 2 * math.degrees(math.asin(c / r))) * i / 8
                               for i in range(9))]
    for sx, sy in ((1, 1), (-1, 1), (-1, -1), (1, -1)):
        band = arc(ro) + arc(ri)[::-1]
        out.append(shape([(cx + sx * x, cy + sy * y) for x, y in band], GREEN, 0.2, True, GREEN))
    return out


def cross_dx(k):
    """crosshair offset in screen px for FREQ 0..1 (eva.c does the same): from the left eye (the artist's spot) to
    the right one"""
    return (RIGHT_EYE_X - CROSS[0]) * k


def left_eye():
    """the artist's right eye, moved to the left one's place under the crosshair"""
    dx, dy = -20, 1.5
    out = [shape(ellipse(121.53 + dx, 68.02 + dy, 1.27, 1.27), FACE, W, True)]
    for d in ('M118.81,67.62s2.31-1.88,5.35-.14', 'M120.04,70.94s.87.94,2.75.43'):
        out += [shape([(x + dx, y + dy) for x, y in pts], FACE) for pts, _ in subpaths(d)]
    return out


def fog_shape():
    """a misty band along the inner visor's bottom (the artist's green edge), wavy top"""
    bottom = [p for pts, _ in subpaths('M75.73,83.09c0,0,13.37,19.16,34.92,19.16s18.58-5.86,18.58-5.86') for p in pts]
    bottom = [(x, y - 1.6) for x, y in bottom[::2] if 79 <= x <= 124.5]    # inside the green and the white rims
    n = len(bottom)
    top = []
    for i, (x, y) in enumerate(bottom):
        t = i / (n - 1)
        h = (1.5 + 9 * math.sin(math.pi * t) ** 0.6) + 1.3 * math.sin(t * 6 * math.pi)
        top.append((x, y - h))
    return bottom + top[::-1]


def svg(state=None):
    lit = {None: [8, 4, 0, 4], 'rest': [8, 4, 0, 4], 'low': [0] * 4, 'high': [TICKS] * 4,
           'design': [8, 4, 13, 10]}[state]
    fog_v = {None: 0.0, 'rest': 0.0, 'low': 0.0, 'high': 1.0, 'design': 0.0}[state]
    freq = {None: 0.0, 'rest': 0.25, 'low': 0.0, 'high': 1.0, 'design': 0.0}[state]
    pre = state is not None   # previews place moving parts with <g transform> (the device file has none)

    out = ['<svg version="1.1" id="Layer_1" xmlns="http://www.w3.org/2000/svg" x="0px" y="0px" width="320px" '
           'height="160px" viewBox="0 0 320 160">']
    cross_done = False
    for el, stroke, fill, width, geo in art(ART):
        if is_full_rect(el) or (el.get('id') or '').startswith('path6808'):
            continue                                      # the black background; the meter ticks (rebuilt below)
        d = el.get('d') or ''
        if d.startswith('M105.2,44.66'):                  # the red radio strip on the visor
            out.append('<g id="strip">' + ''.join(element(stroke, fill, width, geo)) + '</g>')
            continue
        if d.startswith('M75.51,58.37'):                  # the visor's green inner edge: the fog goes under it
            fx, fy = FOG_PIVOT
            f = max(0.02, fog_v)
            a = 0.15 + 0.85 * f
            tr = f' transform="matrix({a:.3f} 0 0 {f:.3f} {fx - a * fx:.2f} {fy - f * fy:.2f})"' if pre else ''
            out.append(f'<g id="fog"{tr}>' + shape(fog_shape(), NAVY, 1.4, True, NAVY) + '</g>')
        if in_cross(geo):
            if not cross_done:
                out += left_eye()
                tr = f' transform="translate({cross_dx(freq):.2f} 0)"' if pre else ''
                out.append(f'<g id="cross"{tr}>' + ''.join(cross()) + '</g>')
                cross_done = True
            continue
        out += element(stroke, fill, width, geo)
    assert cross_done

    # meters: labels, bright ticks, masks (black box + dim ticks), cover
    for i, (color, text) in enumerate(ROWS):
        top = TICK_TOP + i * ROW_PITCH
        shapes=label(text, LABEL_X, LABEL_BASE + i * ROW_PITCH, color)
        if i==3:
            out.append('<g id="com-label">'+''.join(shapes)+'</g>')
            if not pre:                         # page 2: the label reads OCT, the ticks show the beep octave
                out.append('<g id="oct-label">' + ''.join(label('OCT', LABEL_X, LABEL_BASE + i * ROW_PITCH, color)) + '</g>')
        else: out += shapes
        tick = lambda k, c: shape([(TICK_X + k * TICK_DX, top), (TICK_X + k * TICK_DX, top + TICK_H)], c)
        out += [tick(k, color) for k in range(TICKS)]
        bx0, bx1 = TICK_X - TICK_DX / 2, TICK_X + (TICKS - 0.5) * TICK_DX
        box = [(bx0, top - 1.5), (bx1, top - 1.5), (bx1, top + TICK_H + 1.5), (bx0, top + TICK_H + 1.5)]
        mask = [shape(box, BLACK, 0.2, True, BLACK)] + [tick(k, mix(color, 0.3)) for k in range(TICKS)]
        n = lit[i]
        tr = f' transform="translate({f2(n * TICK_DX if n < TICKS else 200)} 0)"' if pre else ''
        out.append(f'<g id="m{i}"{tr}>' + ''.join(mask) + '</g>')
    cover_x = TICK_X + (TICKS - 0.5) * TICK_DX
    out.append(shape([(cover_x, TICK_TOP - 2), (320, TICK_TOP - 2), (320, 160), (cover_x, 160)], BLACK, 0.2, True,
                     BLACK))
    out += [f'<line id="bar{i}" x1="{1.5 + 2 * i}" y1="2" x2="{1.5 + 2 * i}" y2="1" stroke="{BLACK}" stroke-width="1"/>'
            for i in range(4)]
    out.append('</svg>')
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--state', choices=['rest', 'low', 'high', 'design'])
    ap.add_argument('-o', '--output', type=Path)
    a = ap.parse_args()
    out = a.output or HERE / 'eva.svg'
    out.write_text(svg(a.state))
    if a.output is None:
        k = lambda v: round(v * 1024)
        (HERE / 'eva_geom.h').write_text(
            '/* generated by gen_svg.py: eva.svg geometry for the screen code, Q10 screen px */\n'
            f'#define EVA_TICK_DX {k(TICK_DX)}\n'
            f'#define EVA_CROSS_SPAN {k(cross_dx(1))}   /* crosshair offset at FREQ 100 % (0 at FREQ 0) */\n'
            f'#define EVA_FOG_X {k(FOG_PIVOT[0])}\n#define EVA_FOG_Y {k(FOG_PIVOT[1])}\n')
    print(f'{out}: {out.stat().st_size} bytes; TICK_DX {TICK_DX:.4f} px, cross x {CROSS[0]:.2f}, '
          f'fog pivot {FOG_PIVOT[0]:.1f},{FOG_PIVOT[1]:.1f}')
