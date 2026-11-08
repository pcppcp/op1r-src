#!/usr/bin/env python3
"""Appends the voice box (amp.svg) after the amp art: box_n (normal outline), box_h (blue, thick, arrows; page 2) and the
five voice icons icon0..icon4 (CLEAN, CRUNCH, LEAD, BASS, FUZZ), line art in OP-1 colours from the owner's mockups
(run/amp-voice-mockups-2026-10-28/icons_B.png). The box sits below the input cable (y 62..124, x 8..76), the name under it
is drawn by amp.c (blue pixel capitals). Idempotent: re-running replaces the block after the marker. Run: python3 gen_ui.py
Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE)."""
import math, pathlib
W, B, R, G, L = '#f5f5f5', '#698eff', '#ff5949', '#00ed95', '#a4a6f0'
SW = 1.2
def attrs(stroke, w=SW, fill='none'):
    return f'fill="{fill}" stroke="{stroke}" stroke-width="{w}" stroke-linecap="round" stroke-linejoin="round"'
def num(v):                                             # at most 3 decimals (R246's parser refuses more than 4)
    t = f'{v:.3f}'.rstrip('0').rstrip('.')
    return '0' if t == '-0' else t
def corner(px, py, cx, cy, x, y):                       # the quadratic corner (control cx, cy) as the exact cubic
    return (f'C{num(px + 2 * (cx - px) / 3)},{num(py + 2 * (cy - py) / 3)},'
            f'{num(x + 2 * (cx - x) / 3)},{num(y + 2 * (cy - y) / 3)},{num(x)},{num(y)}')
def rrect(x0, y0, x1, y1, r):                           # R246's SVG parser has no Q or A (Parser.cpp:525 panics at boot)
    return (f'M{num(x0+r)},{num(y0)}H{num(x1-r)}{corner(x1-r, y0, x1, y0, x1, y0+r)}V{num(y1-r)}'
            f'{corner(x1, y1-r, x1, y1, x1-r, y1)}H{num(x0+r)}{corner(x0+r, y1, x0, y1, x0, y1-r)}'
            f'V{num(y0+r)}{corner(x0, y0+r, x0, y0, x0+r, y0)}z')
def arc(cx, cy, r, a0, a1):                             # circle arc from angle a0 to a1 (degrees, y down) as cubics
    n = max(1, math.ceil(abs(a1 - a0) / 90)); step = math.radians(a1 - a0) / n
    k = 4 / 3 * math.tan(step / 4)
    a = math.radians(a0); d = f'M{num(cx + r * math.cos(a))},{num(cy + r * math.sin(a))}'
    for _ in range(n):
        b = a + step
        d += (f'C{num(cx + r * (math.cos(a) - k * math.sin(a)))},{num(cy + r * (math.sin(a) + k * math.cos(a)))},'
              f'{num(cx + r * (math.cos(b) + k * math.sin(b)))},{num(cy + r * (math.sin(b) - k * math.cos(b)))},'
              f'{num(cx + r * math.cos(b))},{num(cy + r * math.sin(b))}')
        a = b
    return d
def path(d, stroke, w=SW, fill='none'): return f'<path d="{d}" {attrs(stroke, w, fill)}/>'
def rect(x0, y0, x1, y1, stroke, r=1.2, w=SW): return path(rrect(x0, y0, x1, y1, r), stroke, w)
def circ(cx, cy, r, stroke, w=SW): return f'<circle cx="{cx}" cy="{cy}" r="{r}" {attrs(stroke, w)}/>'
def speaker(cx, cy, r, c, inner=1.3, w=1.1): return circ(cx, cy, r, c, w) + circ(cx, cy, inner, c, w)
def hexa(cx, cy, r, c):
    pts = [(cx + r * math.cos(math.radians(60 * k + 30)), cy + r * math.sin(math.radians(60 * k + 30))) for k in range(6)]
    return path('M' + 'L'.join(f'{x:.1f},{y:.1f}' for x, y in pts) + 'z', c)

def clean():
    o = [rect(26, 80, 58, 109, W), rect(36, 76.5, 48, 80, W, 1), path('M26,86.5H58', W)]
    o += [circ(x, 83.3, 1.3, B, 1) for x in (31, 37, 43, 49)]
    o += [circ(42, 98.5, 8.2, B), circ(42, 98.5, 1.8, B), path('M29,109v2M55,109v2', W)]
    return o
def crunch():
    o = [rect(29, 75, 55, 85, W), rect(26, 87, 58, 112, W), path('M26,112v1.5M58,112v1.5', W)]
    o += [circ(x, 80, 1.1, W, 1) for x in (34, 39, 44, 49.5)] + [path('M52.2,78.6h2v2.8h-2z', R, 1, 'none')]
    o += [speaker(x, y, 4.8, R) for x in (34.5, 49.5) for y in (94, 105.5)]
    return o
def lead():
    o = [path('M42,72.2l-3,3.7h2.6l-2,3.4', R), rect(33, 80, 51, 85.5, W, 1), path('M33,85.5v1.2M51,85.5v1.2', W)]
    o += [rect(33, 87, 51, 99.5, W, 1), rect(33, 100.5, 51, 113, W, 1)]
    for y0 in (87, 100.5):
        o += [speaker(x, y0 + y, 2.4, R, 0.6, 0.9) for x in (38, 46) for y in (3.5, 9)]
    o += [circ(x, 82.7, 0.7, W, 0.8) for x in (36, 39, 42)] + [path('M48.5,81.7h1.6v2h-1.6z', R, 0.8)]
    return o
def bass():
    o = [rect(23, 77, 61, 112, B, 1.4, 1.5)]
    o += [path(f'M{x},{y}h2v2h-2z', B, 0.9) for x, y in ((24.5, 78.5), (57.5, 78.5), (24.5, 107.5), (57.5, 107.5))]
    o += [speaker(x, y, 6.6, L, 1.5, 1.2) for x in (33.5, 50.5) for y in (87.5, 102)]
    return o
POW_DY = math.sqrt(3.6 ** 2 - 2.6 ** 2)                 # the power ring: was A3.6 from x-2.6 to x+2.6 at y 89.2 (large, sweep 0)
POW_A0 = math.degrees(math.atan2(-POW_DY, -2.6))       # start, upper left of the centre
POW_SPAN = 360 - 2 * math.degrees(math.atan2(2.6, POW_DY))   # the long way round, the gap at the top
def fuzz():
    o = [circ(42, 95, 16.5, G, 1.5), path('M39,77.5h6v-2.2h-6z', G, 1.2)]
    for x in (33.5, 50.5):
        o += [path(arc(x, 89.2 + POW_DY, 3.6, POW_A0, POW_A0 - POW_SPAN), R, 1.2), path(f'M{x},{87}v3.4', R, 1.2)]
    o += [hexa(42, 103.5, 4.2, W), circ(42, 103.5, 1.6, W, 1)]
    return o

def build():
    out = ['<!-- voice ui (gen_ui.py) -->', '<g id="box_n">', path(rrect(8, 62, 76, 124, 6), '#a0a6ae', 1.2), '</g>',
           '<g id="box_h">', path(rrect(8.5, 62.5, 75.5, 123.5, 6), B, 2.4),
           f'<path d="M42,65.2L38.4,70.6H45.6z" fill="{B}"/>', f'<path d="M42,121.4L38.4,116H45.6z" fill="{B}"/>', '</g>']
    for i, f in enumerate((clean, crunch, lead, bass, fuzz)):
        out += [f'<g id="icon{i}">', *f(), '</g>']
    return '\n'.join(out) + '\n'

p = pathlib.Path(__file__).with_name('amp.svg')
s = p.read_text()
s = s.split('<!-- voice ui')[0].replace('</svg>', '').rstrip('\n') + '\n'
p.write_text(s + build() + '</svg>\n')
