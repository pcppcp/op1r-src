#!/usr/bin/env python3
"""Writes bloodmoon.svg and bloodmoon_geom.h, the BLOODMOON screen: the author's scene (bloodmoon-art.svg) placed for
the OLED, with the knob names and values drawn live by moon.c in the OP-1's own font (owner's pick, mockup D,
2026-10-29: the original screen with its blackletter lettering replaced).

    python3 synth_sdk/plugins/bloodmoon/gen_svg.py [--state rest|low|high] [-o FILE]

bloodmoon-art.svg is the author's 320 x 160 vector scene (upstream bloodmoon.svg without its knob panels). It is
converted 1:1 except for these deliberate changes:
- Placement: the scene ends 8 px above the screen's bottom edge (cathedral base y 151.4), below the OLED's safe
  area, so every coordinate is scaled by S = 0.9 about x = 160 and moved to y' = S y + DY: the base lands at y 137.5
  (stroke included), the risen moon's top at y 7.7. Radii scale with S; stroke widths stay. Stars keep their x and
  their size (points, not drawing), take the same y, and those under the text are left out (TEXT_BOXES).
- Colours: the OP-1 palette (blue #698EFF for the author's #4D9EFF); black fills #010101, as R246's stock art hides
  with that. Green, red, white and the dim star grey are the author's.
- The full-screen black rect is left out (the screen is black).
R246's subset: path M L C Z (the author's), circle, g; two decimals; one contour per path (filled: checked; stroked:
split, since the native renderer drew the door's last subpath slanted when it shared a path with the arches).
Moving parts (ids moon.c looks up; pivots and travel go to bloodmoon_geom.h):
  stars0..2  flicker (white, HAUNTING)        moon   rises by MOON_DROP px (red, MOONRISE)
  rose       turns about ROSE (blue, BLOOD)    bellL / bellR swing about BELL_L / BELL_R (green, ECLIPSE)
There is no static-prefix cache: the moon must be drawn before the cathedral that hides its lower part, so the
first static node comes after a moving one; the SVG is small (about 11 KB) instead.
--state writes a static preview with the moving parts placed as moon.c would (rest = the default knobs, low = all 0,
high = all at max); moon.c draws the text, so a preview has none.
Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
The scene is by Nola Drift (Project_Bloodmoon, GPL-3.0-or-later with the OP-1 firmware permission).
"""
import argparse
import math
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
S, CX = 0.9, 160.0
DY = 137.5 - S * (151.38 + 0.75)                     # cathedral base (and half its stroke) at y 137.5
MOON_DROP = 24 * S                                   # upstream moon.c's travel, scaled
# where moon.c draws the names and values (x, y, w, h in screen px, the glyphs' boxes plus 3 px): no stars there
TEXT_BOXES = [(11, 15, 72, 57), (11, 77, 72, 57), (217, 15, 82, 57), (217, 77, 82, 57)]
COLOURS = {'#4D9EFF': '#698EFF', '#000000': '#010101'}
# the defaults (plugin.json) and the extremes, as fractions of the knob: blue, green, white, red
STATES = {'rest': (14000, 20000, 7000, 9000), 'low': (0, 0, 0, 0), 'high': (32767,) * 4}


def X(x):
    return CX + S * (x - CX)


def Y(y):
    return S * y + DY


def num(v):
    s = f'{v:.2f}'.rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


def path_d(d):
    out, pend = [], []
    for tok in re.findall(r'[MLCZ]|-?\d+(?:\.\d+)?', d):
        if tok in 'MLCZ':
            out.append(tok)
        else:
            pend.append(float(tok))
            if len(pend) == 2:
                out.append(f'{num(X(pend[0]))},{num(Y(pend[1]))}')
                pend = []
    assert not pend, d
    s = ''
    for t in out:                                    # commands glued to their first pair, pairs comma separated
        s += t if t in 'MLCZ' or s[-1:] in 'MLCZ' or not s else ',' + t
    return s


def attrs(el):
    return dict(re.findall(r'([\w-]+)="([^"]*)"', el))


def element(tag, a, star=False):
    for k in ('fill', 'stroke'):
        if a.get(k) in COLOURS:
            a[k] = COLOURS[a[k]]
    if tag == 'circle':
        a['cx'] = num(float(a['cx']) if star else X(float(a['cx'])))
        a['cy'] = num(Y(float(a['cy'])))
        if not star:
            a['r'] = num(float(a['r']) * S)
    elif tag == 'path':
        if a.get('fill', 'none') != 'none':
            assert a['d'].count('M') == 1, 'filled path with more than one contour: ' + a['d']
        elif a['d'].count('M') > 1:                  # one path per subpath: R246 drew the door's last subpath slanted
            return ''.join(element(tag, dict(a, d='M' + d)) for d in a['d'].split('M')[1:])
        a['d'] = path_d(a['d'])
    return f'<{tag} ' + ' '.join(f'{k}="{v}"' for k, v in a.items()) + '/>'


def in_text(x, y):
    return any(bx <= x <= bx + w and by <= y <= by + h for bx, by, w, h in TEXT_BOXES)


def build(state=None):
    art = (HERE / 'bloodmoon-art.svg').read_text()
    groups = re.findall(r'<g id="(\w+)">(.*?)</g>', art, re.S)
    assert [g for g, _ in groups] == ['stars0', 'stars1', 'stars2', 'moon', 'cathedral', 'windows', 'rose', 'bellL',
                                      'bellR'], [g for g, _ in groups]
    k = STATES.get(state)
    lines = ['<svg xmlns="http://www.w3.org/2000/svg" width="320" height="160" viewBox="0 0 320 160">',
             '<!-- BLOODMOON (Nola Drift), placed for the OLED by gen_svg.py; moon.c draws the knob names and values -->']
    for gid, body in groups:
        star = gid.startswith('stars')
        els = []
        for tag, a in re.findall(r'<(circle|path)\s([^>]*)/>', body):
            a = attrs(a)
            if star and in_text(float(a['cx']), Y(float(a['cy']))):
                continue
            els.append(element(tag, a, star))
        tr = ''
        if k and gid == 'moon':
            tr = f' transform="translate(0 {num((32767 - k[3]) * MOON_DROP / 32767)})"'
        elif k and gid == 'rose':
            tr = f' transform="rotate({num(180 * k[0] / 32768)} {num(X(160))} {num(Y(103.41))})"'
        elif k and gid in ('bellL', 'bellR'):
            ang = 20 * k[1] / 32767 * (1 if gid == 'bellL' else -1)
            bx = X(126.1 if gid == 'bellL' else 193.61)
            tr = f' transform="rotate({num(ang)} {num(bx)} {num(Y(83.59))})"'
        lines.append(f'<g id="{gid}"{tr}>' + ''.join(els) + '</g>')
    lines.append('</svg>')
    return '\n'.join(lines) + '\n'


def geom():
    return f'''/* BLOODMOON screen geometry, written by gen_svg.py with bloodmoon.svg (SVG units = screen px). Do not edit. */
#ifndef BLOODMOON_GEOM_H
#define BLOODMOON_GEOM_H
#define ROSE_X      {X(160):.2f}
#define ROSE_Y      {Y(103.41):.2f}
#define BELL_L_X    {X(126.1):.2f}
#define BELL_R_X    {X(193.61):.2f}
#define BELL_Y      {Y(83.59):.2f}
#define MOON_DROP_Q10 {round(MOON_DROP * 1024)}       /* the moon sits this much lower with the red knob at 0 */
#endif
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--state', choices=sorted(STATES))
    ap.add_argument('-o', type=Path)
    a = ap.parse_args()
    svg = build(a.state)
    if a.state:
        (a.o or HERE / f'bloodmoon_{a.state}.svg').write_text(svg)
        return
    (a.o or HERE / 'bloodmoon.svg').write_text(svg)
    (HERE / 'bloodmoon_geom.h').write_text(geom())


if __name__ == '__main__':
    main()
