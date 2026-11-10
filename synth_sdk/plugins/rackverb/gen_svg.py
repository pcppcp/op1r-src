#!/usr/bin/env python3
"""Writes rackverb.svg, the screen's static art: the owner's approved rack faceplate (OP1-Emulator
docs/design/rackverb-v2/a-v2.svg, concept A revision 2 at screen size, 2026-11-06), its coordinates as they are.
Static: the frame with its rack ears and screws, the LED window, the knob column lines, each knob's ring and seven
ticks in its encoder colour, the strip's line; per unit a logo group (logo0..logo3:
the unit's name in bold slanted stroke letters, kennel's stroke font, and three blue stripes after it;
rackverb_screen.c shows one). Everything that moves is drawn live. Every disconnected stroke is its own element
(R246 draws a stroked path's later M as an L). Run: python3 gen_svg.py
Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE)."""
import math
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
h = (HERE.parents[1] / 'seq/seq_text.h').read_text()
arr = lambda n: [int(x, 0) for x in re.search(n + r'\[\] = \{([^}]*)\}', h).group(1).replace('\n', ' ').split(',') if x.strip()]
FC = re.search(r'sq_fc\[\] = "([^"]*)"', h).group(1)
FO, FN, SEG = arr('sq_fo'), arr('sq_fn'), arr('sq_seg')

G, B, GR, W = '#656579', '#698EFF', '#00ED95', '#FFFFFF'
UNITS = ['VERB I', 'VERB II', 'FEX', 'QUAD']
# the name: concept ink x 20.3.., y 13.7..25.0 (Menlo bold italic 15 px); stroke letters 1.52 px a grid unit (x and y set from the native capture, 2026-11-06)
NAME_X, NAME_Y, NAME_SX, NAME_SY, NAME_W, SLANT = 19.0, 14.8, 1.52, 1.52, 2.2, 0.21
STRIPE_GAP, STRIPE_END = 11.0, 300            # stripes start 11 px after the name's ink (concept: 73 -> 84)
KNOB_X = (147, 209, 271)                       # centres (x0 = 116 + 62 i, x0 + 31); the column lines at x0 - 0.5
KNOB_Y, KNOB_C = 64, (B, GR, W)


def line(x0, y0, x1, y1, c, w, cap=None):
    return (f'<line x1="{x0:.2f}" y1="{y0:.2f}" x2="{x1:.2f}" y2="{y1:.2f}" stroke="{c}" stroke-width="{w}"'
            + (f' stroke-linecap="{cap}"' if cap else '') + '/>')


def name(t):
    """-> (elements, ink right edge) of a unit's name in slanted stroke letters"""
    out, x, right = [], NAME_X, NAME_X
    for ch in t:
        if ch in FC:
            gi = FC.index(ch); g = 2 * FO[gi]
            for j in range(FN[gi]):
                a, b = SEG[g + 2 * j], SEG[g + 2 * j + 1]
                pts = [(x + (v >> 4) * NAME_SX + SLANT * (6 - (v & 15)) * NAME_SY, NAME_Y + (v & 15) * NAME_SY) for v in (a, b)]
                out.append(line(*pts[0], *pts[1], W, NAME_W, 'round'))
                right = max(right, pts[0][0], pts[1][0])
        x += (4 if ch in '1.' else 6) * NAME_SX
    return out, right + NAME_W / 2


def main():
    p = ['<svg xmlns="http://www.w3.org/2000/svg" width="320" height="160" viewBox="0 0 320 160">',
         f'<rect x="1.5" y="6.5" width="317" height="132" rx="2" fill="none" stroke="{G}"/>',
         line(12.5, 6.5, 12.5, 138.5, G, 1), line(307.5, 6.5, 307.5, 138.5, G, 1)]
    p += [f'<circle cx="{cx}" cy="{cy}" r="2.5" fill="none" stroke="{G}"/>' for cx in (7, 313) for cy in (16, 129)]
    p.append(f'<rect x="18.5" y="31.5" width="76" height="52" rx="2" fill="none" stroke="{G}"/>')   # LED window
    for cx, c in zip(KNOB_X, KNOB_C):
        p.append(line(cx - 31.5, 31, cx - 31.5, 98, G, 1))
        for t in range(7):
            a = math.radians(135 + t * 45)
            p.append(line(cx + 15 * math.cos(a), KNOB_Y + 15 * math.sin(a), cx + 18 * math.cos(a),
                          KNOB_Y + 18 * math.sin(a), c, 1))
        p.append(f'<circle cx="{cx}" cy="{KNOB_Y}" r="11.5" fill="none" stroke="{c}" stroke-width="2"/>')
    p.append(line(18, 104.5, 300, 104.5, G, 1))
    for i, u in enumerate(UNITS):
        segs, right = name(u)
        segs += [line(right + STRIPE_GAP, yy, STRIPE_END, yy, B, 1) for yy in (14, 18, 22)]
        p.append(f'<g id="logo{i}">' + ''.join(segs) + '</g>')
    p.append('</svg>')
    (HERE / 'rackverb.svg').write_text('\n'.join(p) + '\n')


if __name__ == '__main__':
    main()
