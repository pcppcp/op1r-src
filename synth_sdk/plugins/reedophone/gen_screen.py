#!/usr/bin/env python3
"""Write reedscr.svg (reedophone's screen) from the hand-drawn reedophone.svg.

The OP-1's SVG parser (DEV_GUIDE 15) wants rounded coordinates and no CSS, so the drawing is flattened here:
polylines, polygons, rects, circles and ellipses become paths, numbers get 2 decimals, the colours move to the OP-1
palette (the wind keeps its purple hue, as bright as the dark grey), the background reference rect and the editor's metadata go.

Parts reedscr.c moves or switches get their own group (leaf shapes are wrapped):
  dial (blue), saw + blade (green; blade spins), vent (orange), mic (white), trigger_on / trigger_off,
  body_none / body_vent (vibrate when the saw touches them), o1..o24 / c1..c24 (the keys, open / closed),
  dw1..dw7, vw1..vw5, bell-winds (the wind lines).
A moving part inside another moving group (vent in body_vent, blade in saw) is lifted out of it: the group is split
around it (the part after it is <id>2) and reedscr.c gives the parts the same motion. The firmware is not known to
combine nested transforms, so none are nested."""
import math, os, re, xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
SRC, OUT = os.path.join(HERE, 'reedophone.svg'), os.path.join(HERE, 'reedscr.svg')
NS = '{http://www.w3.org/2000/svg}'
COL = {'#0071bc': '#4d9eff', '#39b54a': '#00ed95', '#f15a24': '#ff3a5d', '#666666': '#6f7173',
       '#3a1a49': '#9b50be'}                      # the wind's purple, brightened to the dark grey's luminance
MOVING = {'dial', 'saw', 'blade', 'vent', 'mic', 'body_none', 'body_vent'}


def n(v): return ('%.2f' % v).rstrip('0').rstrip('.') if abs(v) >= 0.005 else '0'


def ident(el):
    """the id reedscr.c uses, or None. _13o -> o13, _13c -> c13"""
    i = el.get('id')
    if not i: return None
    m = re.fullmatch(r'_(\d+)([oc])', i)
    if m: return m.group(2) + m.group(1)
    if i in MOVING or i in ('trigger_on', 'trigger_off', 'bell-winds') or re.fullmatch(r'[dv]w\d+', i): return i
    return None


def path_d(el):
    tag = el.tag.replace(NS, '')
    f = lambda k: float(el.get(k, 0))
    if tag in ('polyline', 'polygon'):
        p = [float(v) for v in re.findall(r'-?\d*\.?\d+(?:e-?\d+)?', el.get('points'))]
        d = 'M' + ' L'.join(f'{n(p[i])} {n(p[i + 1])}' for i in range(0, len(p), 2))
        return d + (' Z' if tag == 'polygon' else '')
    if tag == 'rect':
        x, y, w, h = f('x'), f('y'), f('width'), f('height')
        return f'M{n(x)} {n(y)} L{n(x + w)} {n(y)} L{n(x + w)} {n(y + h)} L{n(x)} {n(y + h)} Z'
    if tag in ('circle', 'ellipse'):
        cx, cy = f('cx'), f('cy'); rx = f('r') or f('rx'); ry = f('r') or f('ry'); k = 0.5523
        return (f'M{n(cx + rx)} {n(cy)} C{n(cx + rx)} {n(cy + k * ry)} {n(cx + k * rx)} {n(cy + ry)} {n(cx)} {n(cy + ry)} '
                f'C{n(cx - k * rx)} {n(cy + ry)} {n(cx - rx)} {n(cy + k * ry)} {n(cx - rx)} {n(cy)} '
                f'C{n(cx - rx)} {n(cy - k * ry)} {n(cx - k * rx)} {n(cy - ry)} {n(cx)} {n(cy - ry)} '
                f'C{n(cx + k * rx)} {n(cy - ry)} {n(cx + rx)} {n(cy - k * ry)} {n(cx + rx)} {n(cy)} Z')
    if tag == 'path':                                    # already absolute M / L / C / Z: just round
        return re.sub(r'-?\d*\.?\d+(?:e-?\d+)?', lambda m: n(float(m.group())), el.get('d'))
    raise ValueError(tag)


def style(el):
    c = lambda v: COL.get(v.lower(), v.lower()) if v and v != 'none' else 'none'
    s = f' fill="{c(el.get("fill", "#000000"))}"'
    if el.get('stroke'): s += f' stroke="{c(el.get("stroke"))}" stroke-width="{el.get("stroke-width", "1")}"'
    return s


def leaf(el):
    tag = el.tag.replace(NS, '')
    if tag == 'line':
        a = ' '.join(f'{k}="{n(float(el.get(k)))}"' for k in ('x1', 'y1', 'x2', 'y2'))
        return f'<line {a}{style(el)}/>'
    return f'<path d="{path_d(el)}"{style(el)}/>'


def emit(el, out, depth, moving_parent=None):
    """write el; returns nothing. Moving children of a moving group are written after closing it (split)."""
    tag = el.tag.replace(NS, '')
    if tag == 'title' or el.get('id') == 'BG_ref': return
    i = ident(el)
    pad = '  ' * depth
    if tag in ('g', 'svg'):
        kids = list(el)
        if i in MOVING:                                  # split around moving children
            parts, cur = [], []
            for ch in kids:
                if ident(ch) in MOVING: parts.append(cur); parts.append(ch); cur = []
                else: cur.append(ch)
            parts.append(cur)
            k = 0
            for p in parts:
                if isinstance(p, list):
                    if not p: continue
                    gid = i if k == 0 else f'{i}2'
                    assert k < 2, f'{i}: more than one split'
                    k += 1
                    out.append(f'{pad}<g id="{gid}">')
                    for ch in p: emit(ch, out, depth + 1)
                    out.append(f'{pad}</g>')
                else:
                    emit(p, out, depth)
            return
        out.append(f'{pad}<g id="{i}">' if i else f'{pad}<g>')
        for ch in kids: emit(ch, out, depth + 1)
        out.append(f'{pad}</g>')
        return
    s = leaf(el)
    out.append(f'{pad}<g id="{i}">{s}</g>' if i else pad + s)


def main():
    root = ET.parse(SRC).getroot()
    out = []
    for ch in root: emit(ch, out, 1)
    svg = ('<?xml version="1.0" encoding="UTF-8"?>\n<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 320 160">\n'
           + '\n'.join(out) + '\n</svg>\n')
    ids = re.findall(r'id="([^"]+)"', svg)
    assert len(ids) == len(set(ids)), 'duplicate ids'
    # the firmware's parser rewrites ids (Parser.cpp, Illustrator names): "_xHH_" is decoded as a character, and an
    # id with two or more '_' is cut at the first one ("body_vent_b" -> "body"); a lookup of the full name then
    # fails, which is fatal ("Can't find SVG object"). Keep ids to letters, digits, '-' and at most one '_'.
    bad = [x for x in ids if x.count('_') > 1 or '_x' in x or not re.fullmatch(r'[A-Za-z0-9_-]+', x)]
    assert not bad, f'ids the firmware would rewrite: {bad}'
    need = ['dial', 'saw', 'blade', 'vent', 'mic', 'body_none', 'body_vent', 'body_vent2', 'trigger_on', 'trigger_off',
            'bell-winds'] + [f'{c}{k}' for c in 'oc' for k in range(1, 25)] + [f'dw{k}' for k in range(1, 8)] \
           + [f'vw{k}' for k in range(1, 6)]
    miss = [x for x in need if x not in ids]
    assert not miss, miss
    open(OUT, 'w', newline='\n').write(svg)
    print(OUT, len(svg), 'B,', len(ids), 'ids')
    # rotation centres for reedscr.c, from the drawing: the dial's circle, the saw's round hub
    el = {e.get('id'): e for e in root.iter()}
    dial = next(c for c in el['dial'] if c.tag == NS + 'circle')
    hub = next(c for c in el['saw'] if c.tag in (NS + 'ellipse', NS + 'circle'))
    q = lambda e, k: round(float(e.get(k)) * 1024)
    open(os.path.join(HERE, 'reedscr_geom.h'), 'w', newline='\n').write(
        '/* made by gen_screen.py from reedophone.svg: rotation centres, Q10 px */\n'
        f'#define DIAL_X_Q10 {q(dial, "cx")}\n#define DIAL_Y_Q10 {q(dial, "cy")}\n'
        f'#define HUB_X_Q10  {q(hub, "cx")}\n#define HUB_Y_Q10  {q(hub, "cy")}\n')


if __name__ == '__main__':
    main()
