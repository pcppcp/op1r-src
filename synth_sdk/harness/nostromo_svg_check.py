#!/usr/bin/env python3
"""Check plugins/nostromo/nostromo.svg against what the OP-1's SVG parser reads: the elements the SDK screens use,
inline #rrggbb colours, plain numbers (no exponents), M / L paths only, no opacity or dashes; every node
nostromo.c looks up, each id once; knob colours only, above y 140 (as op1r's screens keep); each recoloured label in a colour of its own (the
parser may share equal styles); the typing cover and cursor drawn last. And against what its renderer draws badly at 320 x 160: strokes under 1 px
(they fade into the background), paths whose box has no width or height (drawn as a line across the screen) and
paths of more than one subpath (it draws each later M as a line to that point, joining the strokes); and line art
below y 140 (op1r's screens stay above it).

    python3 harness/nostromo_svg_check.py
"""
import os, re, xml.etree.ElementTree as ET

SDK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SVG = os.path.join(SDK, 'plugins', 'nostromo', 'nostromo.svg')
TAGS = {'svg', 'g', 'path', 'line', 'polyline', 'rect', 'circle'}
NUMS = {'x', 'y', 'x1', 'y1', 'x2', 'y2', 'width', 'height', 'cx', 'cy', 'r', 'stroke-width'}
ATTRS = NUMS | {'id', 'fill', 'stroke', 'stroke-linejoin', 'd', 'points'}
IDS = (['s%d%s' % (d, g) for d in range(3) for g in 'abcdefg'] + ['dp0', 'dp1'] + ['u%d' % i for i in range(4)] +
       ['nm%d' % i for i in range(7)] +
       ['%s%d' % (p, i) for p in ('wa', 'wb', 'wc', 'wd') for i in range(3)] + ['cp', 'bt', 'e0', 'e1', 'cv', 'cu', 'xh1', 'lp', 'lr', 'nm7'] +
       ['%s%d' % (p, d) for p in ('xt', 'xo') for d in range(10)] + ['sh'] + ['sr%d' % i for i in range(6)])
LABELS = 7                                                   # l0a.., the labels recoloured by page


def hue(c):                                                  # degrees; None for greys and 'none'
    if not c.startswith('#'):
        return None
    r, g, b = (int(c[i:i + 2], 16) / 255 for i in (1, 3, 5))
    mx, mn = max(r, g, b), min(r, g, b)
    if mx - mn < .05:
        return None
    h = (g - b) / (mx - mn) % 6 if mx == r else (b - r) / (mx - mn) + 2 if mx == g else (r - g) / (mx - mn) + 4
    return 60 * h


def main():
    root = ET.parse(SVG).getroot()
    assert root.get('viewBox') == '0 0 320 160'
    ids = set()
    for el in root.iter():
        tag = el.tag.split('}')[-1]
        assert tag in TAGS, tag
        if tag == 'svg':
            continue
        for k, v in el.attrib.items():
            assert k in ATTRS, (tag, k)
            if k in NUMS: assert re.fullmatch(r'-?\d+(\.\d+)?', v), (k, v)
            if k in ('fill', 'stroke'): assert v == 'none' or re.fullmatch('#[0-9a-f]{6}', v), (k, v)
            if k == 'd': assert re.fullmatch(r'[ML0-9., -]+', v), v[:60]
            if k == 'points': assert re.fullmatch(r'[0-9., -]+', v), v[:60]
        if tag in ('path', 'line', 'polyline'): assert el.get('fill') == 'none', (tag, el.attrib)
        if el.get('stroke-width'): assert float(el.get('stroke-width')) >= 1, (tag, el.get('stroke-width'))
        if tag in ('line', 'path', 'polyline'):
            ys = [float(el.get('y1')), float(el.get('y2'))] if tag == 'line' else \
                 [float(v) for v in re.findall(r'-?\d+(?:\.\d+)?', el.get('d') or el.get('points'))[1::2]]
            assert max(ys) <= 140, ('below y 140', tag, max(ys))
        if tag == 'path':
            v = [float(x) for x in re.findall(r'-?\d+(?:\.\d+)?', el.get('d'))]
            assert max(v[0::2]) > min(v[0::2]) and max(v[1::2]) > min(v[1::2]), ('flat path', el.get('d')[:60])
            assert el.get('d').count('M') == 1, ('subpaths joined on the OP-1', el.get('d')[:60])
        if el.get('id'):
            assert el.get('id') not in ids, el.get('id')
            ids.add(el.get('id'))
    n = sum(1 for _ in root.iter())
    assert n <= 1100, ('elements', n)                     # the user's OP-1 runs 1092; the stock maximum is 1111
    missing = [i for i in IDS if i not in ids]
    assert not missing, missing
    gone = [i for i in ids if re.fullmatch(r'(cb|cd|lb|ld|mx|sw|qt|qo|pt|po|gty|gdf|gmd|gck|st)\d+|kp|nd|sb|sd|fpb|fpd|fwb|fwd', i)]
    assert not gone, ('the knob page and the ship screen are gone', gone)
    for el in root.iter():                                   # knob colours only (no amber)
        for k in ('stroke', 'fill'):
            h = hue(el.get(k) or 'none')
            assert h is None or not 20 <= h <= 60, ('a colour no knob has', el.get(k))
        if el.tag.split('}')[-1] == 'rect':
            assert float(el.get('y')) + float(el.get('height')) <= 140, ('below y 140', el.attrib)
    colours = {}                                             # each label's strokes: one colour, no other element's
    for el in root.iter():
        c = el.get('stroke')
        if c and c != 'none': colours.setdefault(c, set()).add(el.get('id') or '')
    for i in range(LABELS):
        strokes = [e for e in root.iter() if re.fullmatch(r'l%d[a-z]' % i, e.get('id') or '')]
        assert strokes and [e.get('id') for e in strokes] == ['l%d%s' % (i, chr(97 + k)) for k in range(len(strokes))]
        cs = {e.get('stroke') for e in strokes}
        assert len(cs) == 1, ('a label in one colour', i, cs)
        assert colours[cs.pop()] == {e.get('id') for e in strokes}, ('a label colour another element has', i)
    order = [e.get('id') for e in root if e.get('id')]
    assert order[-2:] == ['cv', 'cu'] and order.index('cp') < order.index('cv') and order.index('bt') < order.index('cv'), \
        'the cover and cursor over the caption'
    print('ok svg:', len(ids), 'nodes,', n, 'elements,', os.path.getsize(SVG), 'bytes')


if __name__ == '__main__':
    main()
