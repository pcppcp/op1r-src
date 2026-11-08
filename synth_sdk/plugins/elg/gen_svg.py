#!/usr/bin/env python3
"""Generate elg.svg + elg_geom.h: ELG's screen, the folk-art moose crown, traced from the owner's approved concept.

    python3 gen_svg.py [--retrace] [--report OUT.json] [--design OUT.svg]

The art is elg-art.png: concept B "fine" at 2:1 (OP1-Emulator docs/design/elg-v2/concepts/B-fine-2to1.png, 1536 x
768, an image-model PNG the owner picked and approved on 2026-11-06; spec docs/design/elg-v2/spec.md). No vector file
exists, so it is traced with display/art_trace.py (docs/screen-art.md, "From a PNG"): one uniform scale 320 / 1536,
no offsets, measured colours and widths. The trace takes about five minutes, so it is kept in
synth_sdk/build/elg-trace-<png sha>.svg (git-ignored; --retrace makes it again).

Colours: traced with the PNG's measured colours, then written as the palette the concept was asked for (the image
model drifted from it; the owner's rule: stock and encoder colours): navy #3B3C90 -> #383572, red #F52750 -> #FF3A5D,
blue #4986F3 -> #698EFF, brown #DC8E51 -> #B07A50, green #02EEA4 -> #00ED95. The tracer measures the lavender and the
white as one colour (#F3F3F6); each of those strokes is split by the PNG's colour along it: the moon ring and the eye
whites are white #FFFFFF, the rest (knotwork, cloud lines, stars, face and beard shading) lavender #DED7FF.
Stroke widths as measured (brown 0.6, blue 0.8, red 0.85, navy 1.0, green 1.1, white 0.45 px; the lavender knotwork
0.6 so its double lines stay apart).

What is kept (the owner: keep the engraved lines that still show at screen size, drop the rest): every traced stroke
of at least MIN_LEN px (2.5 for the knotwork), each simplified within SIMPLIFY px (Douglas-Peucker; R246 draws at
1.25 x, so 0.35 units is under half a display pixel). Shorter pieces are the tracer's specks and the engraving's dots, which blur at
320 x 160. The stars and the knotwork's green berries are under three stroke widths: measured in the PNG (centre,
size) and drawn as four-point sparkles and small circles.

Placement (docs/screen-art.md, "Placement and OLED bottom margin"): the concept's beard reaches y 146.6 and its
knotwork y 141.6, while its aurora starts at y 10.3. One lift, LIFT = 7 px, for everything (SVG and elg_geom.h
together): the aurora's top lands on y 3.3 and the knotwork on 134.6. The beard's tip is then still 2.4 px too low,
so it is trimmed: brown strokes are cut at y BEARD_CUT (ink to 137.5). main() refuses any still or moving ink outside
x 2..318, y 3..138 at the knob extremes.

What is SVG and what is code (TE heap budget, see the README "Screen"): a parsed SVG costs R246's drawing heap about
200 bytes an element plus 29 a vertex (fitted on the catalog's measured screen_bytes). All of the trace as SVG would
be ~520 KB; the parts in CODE are drawn by screen.c with raster lines from elg_geom.h instead, which costs no heap.
"""
import argparse
import hashlib
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
SDK = HERE.parents[1]
sys.path.insert(0, str(SDK / 'display'))
from svgart import art, f2, no_turnback   # noqa: E402

ART = HERE / 'elg-art.png'
SVG = HERE / 'elg.svg'
GEOM = HERE / 'elg_geom.h'
K = 320 / 1536                       # the PNG's px -> screen px

MIN_LEN = 1.6                        # px: shorter traced pieces are dropped
SIMPLIFY = 0.35                      # px: Douglas-Peucker tolerance
MIN_LEN_PART = {'knot': 2.5}          # px: the knotwork's traced double lines leave more specks
LIFT = 7.0                           # px, everything (screen-art.md bottom margin)
BEARD_CUT = 137.2 + LIFT             # reference y: brown strokes are cut here (centre; ink to 137.5 after the lift)
X0, X1, Y0, Y1 = 2.0, 318.0, 3.0, 138.0     # every still or moving ink stays inside

TRACE_COL = {'#3B3C90': 'navy', '#F52750': 'red', '#4986F3': 'blue', '#DC8E51': 'brown', '#02EEA4': 'green',
             '#F3F3F6': 'light'}
STOCK = {'navy': '#383572', 'red': '#FF3A5D', 'blue': '#698EFF', 'brown': '#B07A50', 'green': '#00ED95',
         'lav': '#DED7FF', 'white': '#FFFFFF', 'knot': '#DED7FF'}
WIDTH = {'navy': 1.0, 'red': 0.85, 'blue': 0.8, 'brown': 0.6, 'green': 1.1, 'lav': 0.6, 'white': 0.6, 'knot': 1.1}
KNOT_BOXES = ((0, 505, 560, 720), (976, 505, 1536, 720))     # the PNG's knotwork corners (traced on their own)
KNOT_COLORS = ['#C4C4F2', '#02EEA4', '#B87444', '#383890']

# the nostrils (breath scales about them), the antlers' base (they spread about it): reference px
NOSTRIL_L, NOSTRIL_R = (146.0, 108.5), (174.0, 108.5)
ANTLER_BASE = (160.0, 66.0)
# the most the moving parts reach (screen.c): antler spread x / y, breath scale, aurora drop and fringe stretch
ANT_SX, ANT_SY = 1.07, 1.035
BREATH_MAX = 1.45
AUR_DROP, AUR_STRETCH = 3.0, 1.45


# ---------------------------------------------------------------------------------------------------------------
# the trace

def trace_path():
    sha = hashlib.sha256(ART.read_bytes()).hexdigest()[:12]
    return SDK / 'build' / f'elg-trace-{sha}.svg'


def knot_trace_path():
    sha = hashlib.sha256(ART.read_bytes()).hexdigest()[:12]
    return SDK / 'build' / f'elg-knot-trace-{sha}.svg'


def retrace():
    import art_trace
    import tempfile
    r = art_trace.trace(ART)
    p = trace_path()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(art_trace.svg(r['strokes']))
    # the knotwork on its own (the rest black), with the lavender as its own colour: in the whole trace the tracer
    # measures lavender and white as one colour 0.45 px wide, and the knot's 1.1 px strands come out as their edges
    im = Image.open(ART).convert('RGB')
    knot = Image.new('RGB', im.size)
    for box in KNOT_BOXES:
        knot.paste(im.crop(box), box[:2])
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / 'knot.png'
        knot.save(q)
        r = art_trace.trace(q, KNOT_COLORS)
    knot_trace_path().write_text(art_trace.svg(r['strokes']))
    return p


def knot_strokes():
    out = []
    for el, stroke, fill, w, geo in art(knot_trace_path()):
        if stroke.upper() != KNOT_COLORS[0]:
            continue
        for pts, closed in geo:
            P = np.array(pts, float)
            if closed:
                P = np.vstack([P, P[:1]])
            out.append({'col': 'light', 'P': P, 'closed': closed, 'len': plen(P), 'part': 'knot', 'colour': 'knot'})
    return out


def plen(P):
    return float(np.sum(np.hypot(*np.diff(P, axis=0).T))) if len(P) > 1 else 0.0


def dp(P, eps):
    """Douglas-Peucker"""
    if len(P) < 3:
        return P
    a, b = P[0], P[-1]
    ab = b - a
    n = np.hypot(*ab)
    if n < 1e-9:
        d = np.hypot(*(P - a).T)
    else:
        d = np.abs(ab[0] * (P[:, 1] - a[1]) - ab[1] * (P[:, 0] - a[0])) / n
    i = int(np.argmax(d))
    if d[i] <= eps:
        return np.array([a, b])
    return np.vstack([dp(P[:i + 1], eps)[:-1], dp(P[i:], eps)])


def png():
    return np.asarray(Image.open(ART).convert('RGB')).astype(float)


def sample_colour(im, P):
    cols = []
    for (ax, ay), (bx, by) in zip(P, P[1:]):
        n = max(1, int(math.hypot(bx - ax, by - ay) / 0.3))
        for t in np.linspace(0, 1, n):
            x, y = (ax + t * (bx - ax)) / K, (ay + t * (by - ay)) / K
            c = im[int(min(767, max(0, y))), int(min(1535, max(0, x)))]
            if c.max() > 120:
                cols.append(c)
    return np.median(cols, axis=0) if cols else np.zeros(3)


def strokes():
    p = trace_path()
    if not p.exists() or not knot_trace_path().exists():
        retrace()
    out = []
    for el, stroke, fill, w, geo in art(p):
        for pts, closed in geo:
            P = np.array(pts, float)
            if closed:
                P = np.vstack([P, P[:1]])
            out.append({'col': TRACE_COL[stroke.upper()], 'P': P, 'closed': closed, 'len': plen(P)})
    return out


# ---------------------------------------------------------------------------------------------------------------
# small parts measured in the PNG

def components(mask, lo, hi):
    from scipy import ndimage
    lab, n = ndimage.label(mask, np.ones((3, 3)))
    out = []
    for i, sl in enumerate(ndimage.find_objects(lab)):
        h, w = sl[0].stop - sl[0].start, sl[1].stop - sl[1].start
        if lo <= max(h, w) <= hi:
            ys, xs = np.nonzero(lab[sl] == i + 1)
            out.append(((xs.mean() + sl[1].start + 0.5) * K, (ys.mean() + sl[0].start + 0.5) * K, w * K, h * K))
    return out


def stars(im):
    """the four-point sparkles (lavender, in the sky): centre and arm length"""
    lav = (im[..., 0] > 150) & (im[..., 2] > 200) & (im[..., 2] - im[..., 0] > 20)
    found = []
    for x, y, w, h in components(lav, 18, 60):
        if y < 75 and abs(w - h) < 2.5 and not (118 < x < 202 and 22 < y < 70):
            found.append((round(x, 2), round(y, 2), round(max(w, h) / 2, 2)))
    return sorted(found)


def berries(im):
    """the knotwork's green berries: small filled circles. Two berries that touch read as one larger blob: such a blob
    (over 1.5 x the usual size) is taken as the berries of the mirrored corner (the art is symmetric about x 160) that
    fall inside it"""
    g = (im[..., 1] > 180) & (im[..., 0] < 90) & (im[..., 2] > 100)
    found = [c for c in components(g, 6, 40) if c[1] > 100 and (c[0] < 40 or c[0] > 280)]
    size = float(np.median([max(w, h) for _, _, w, h in found]))
    single = [c for c in found if max(c[2], c[3]) <= 1.5 * size]
    out = [(x, y, (w + h) / 4) for x, y, w, h in single]
    for x, y, w, h in found:
        if max(w, h) > 1.5 * size:
            for mx, my, mw, mh in single:
                mx = 320 - mx
                if abs(mx - x) <= w / 2 and abs(my - y) <= h / 2:
                    out.append((mx, my, (mw + mh) / 4))
    return sorted((round(x, 2), round(y, 2), round(r, 2)) for x, y, r in out)


def pines(im):
    """the pine trees (navy, filled in the concept): each one's silhouette from its rows' left and right edges in the
    PNG, simplified; -> [(polygon in reference px, bbox)]"""
    from scipy import ndimage
    nav = (im[..., 2] > 100) & (im[..., 0] < 90) & (im[..., 1] < 90)
    out = []
    for x0, x1 in ((0, 220), (1316, 1536)):
        y0 = 395
        sub = nav[y0:484, x0:x1]
        lab, n = ndimage.label(sub, np.ones((3, 3)))
        for i, sl in enumerate(ndimage.find_objects(lab)):
            h, w = sl[0].stop - sl[0].start, sl[1].stop - sl[1].start
            if h < 40 or w > 25:
                continue
            m = lab[sl] == i + 1
            left, right = [], []
            for r in range(m.shape[0]):
                xs = np.nonzero(m[r])[0]
                if len(xs):
                    y = (y0 + sl[0].start + r + 0.5) * K
                    left.append(((x0 + sl[1].start + xs[0]) * K, y))
                    right.append(((x0 + sl[1].start + xs[-1] + 1) * K, y))
            ring = np.array(right + left[::-1])
            ring = dp(np.vstack([ring, ring[:1]]), 0.3)[:-1]
            out.append((ring, (ring[:, 0].min(), ring[:, 1].min(), ring[:, 0].max(), ring[:, 1].max())))
    return out


# ---------------------------------------------------------------------------------------------------------------
# parts

def fit_circle(P):
    A = np.c_[2 * P, np.ones(len(P))]
    b = (P ** 2).sum(1)
    cx, cy, c = np.linalg.lstsq(A, b, rcond=None)[0]
    return cx, cy, math.sqrt(c + cx * cx + cy * cy)


def classify(ss, im, star_list, pine_list=()):
    """-> list of strokes with 'part' and 'colour' (stock name)"""
    # the moon ring: long light strokes around the head's top
    # the moon ring: fitted on the longest light stroke (its top arc), then every light piece on that circle
    top = max((s for s in ss if s['col'] == 'light'), key=lambda s: s['len'])
    cx, cy, r = fit_circle(top['P'])
    ring = [s for s in ss if s['col'] == 'light' and s['len'] > 3 and
            np.abs(np.hypot(s['P'][:, 0] - cx, s['P'][:, 1] - cy) - r).max() < 2.5]
    cx, cy, r = fit_circle(np.vstack([s['P'] for s in ring]))
    moon = (cx, cy, r)
    out = []
    for s in ss:
        P, c = s['P'], s['col']
        x, y = P[:, 0].mean(), P[:, 1].mean()
        if c == 'light':
            if any(math.hypot(x - sx, y - sy) < sr + 1.2 for sx, sy, sr in star_list):
                continue                                         # redrawn as a sparkle
            dr = np.abs(np.hypot(P[:, 0] - cx, P[:, 1] - cy) - r)
            if s['len'] > 3 and (dr.max() < 1.5 or (np.mean(dr < 1.2) >= 0.5 and y < 66)):   # ring (a piece may carry a cloud line's end)
                part, colour = 'moon', 'white'
            elif 115 < x < 205 and 38 < y < 52 and np.ptp(P[:, 1]) < 2.5:
                part, colour = 'cloud', 'lav'
            elif y > 104 and (x < 112 or x > 208):
                continue                                         # the knotwork: from its own trace (knot_strokes)
            else:
                mc = sample_colour(im, P)
                part, colour = 'face', ('lav' if mc[2] - mc[0] > 22 else 'white')
        elif c == 'navy' and any(b[0] - 0.6 <= P[:, 0].min() and P[:, 0].max() <= b[2] + 0.6 and b[1] - 0.6 <=
                                 P[:, 1].min() and P[:, 1].max() <= b[3] + 0.6 for _, b in pine_list):
            continue                                             # a pine's centre line: the pine is a filled shape
        elif c == 'green':
            if x < 60 or x > 260:
                continue                                         # berry speckles: redrawn as circles
            part, colour = ('breathL' if x < 160 else 'breathR'), 'green'
        else:
            part, colour = {'navy': 'land', 'red': 'aurora', 'blue': 'antlers', 'brown': 'fur'}[c], c
        out.append(dict(s, part=part, colour=colour))
    return out + knot_strokes(), moon


def prepare(ss):
    """drop the specks, cut the beard, simplify, lift"""
    out = []
    for s in ss:
        if s['len'] < MIN_LEN_PART.get(s['part'], MIN_LEN):
            continue
        P = s['P']
        if s['part'] == 'fur' and P[:, 1].max() > BEARD_CUT - LIFT:
            P = cut_below(P, BEARD_CUT - LIFT)
            if P is None or plen(P) < MIN_LEN:
                continue
        P = dp(P, SIMPLIFY)
        q = no_turnback([(float(f2(px)), float(f2(py - LIFT))) for px, py in P])
        if len(q) < 2:
            continue
        out.append(dict(s, P=np.array(q), len=plen(np.array(q))))
    return out


def cut_below(P, ymax):
    """the part of a polyline above ymax (the longest run), the crossing interpolated"""
    runs, cur = [], []
    for i, p in enumerate(P):
        if p[1] <= ymax:
            if not cur and i > 0:
                a = P[i - 1]
                t = (ymax - a[1]) / (p[1] - a[1])
                cur.append(a + t * (p - a))
            cur.append(p)
        elif cur:
            a = P[i - 1]
            t = (ymax - a[1]) / (p[1] - a[1])
            cur.append(a + t * (p - a))
            runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    if not runs:
        return None
    return np.array(max(runs, key=lambda r: plen(np.array(r))))


# ---------------------------------------------------------------------------------------------------------------
# cost

def te_bytes(n_el, n_vert):
    return 201 * n_el + 29 * n_vert + 3171


def segs(s):
    return len(s['P']) - 1


# ---------------------------------------------------------------------------------------------------------------
# output

def el(s, w=None):
    """one stroke as an R246 element (its own element: R246 draws a stroked path's later M as an L)"""
    q = [(float(x), float(y)) for x, y in s['P']]
    col = STOCK[s['colour']]
    w = w if w is not None else s.get('w', WIDTH[s['colour']])
    attrs = f'stroke="{col}" stroke-width="{f2(w)}" stroke-linecap="round"'
    if len(q) == 2:
        (ax, ay), (bx, by) = q
        return f'<line x1="{f2(ax)}" y1="{f2(ay)}" x2="{f2(bx)}" y2="{f2(by)}" {attrs}/>'
    return f'<polyline points="{" ".join(f"{f2(x)},{f2(y)}" for x, y in q)}" fill="none" {attrs} stroke-linejoin="round"/>'


def sparkle(x, y, r):
    """a four-point star as lines (both arms), reference coordinates lifted"""
    y -= LIFT
    return [((x - r, y), (x + r, y)), ((x, y - r), (x, y + r))]


def preview_svg(pp, star_list, berry_list, path):
    body = [el(s) for s in pp]
    for x, y, r in star_list:
        for (a, b) in sparkle(x, y, r):
            body.append(el({'P': np.array([a, b]), 'colour': 'lav'}))
    for x, y, r in berry_list:
        body.append(f'<circle cx="{f2(x)}" cy="{f2(y - LIFT)}" r="{f2(r)}" fill="{STOCK["green"]}" stroke="none"/>')
    Path(path).write_text('<svg version="1.1" xmlns="http://www.w3.org/2000/svg" width="320px" height="160px" '
                          'viewBox="0 0 320 160">\n' + '\n'.join(body) + '\n</svg>\n')


def measure_widths(im, pp):
    """each antler, fur and face stroke's own width, measured in the PNG (twice the distance to its colour's edge at the
    centre line, median along the stroke, less half a pixel of antialiasing): the engraving tapers, the antlers'
    outline is about 1.2 px and their inner tines about 0.5, the colour's single median (WIDTH) would flatten that"""
    from scipy import ndimage
    masks = {'blue': (im[..., 2] > 150) & (im[..., 2] - im[..., 0] > 60),
             'brown': (im[..., 0] > 110) & (im[..., 0] - im[..., 2] > 35),
             'lav': (im[..., 0] > 150) & (im[..., 2] - im[..., 0] > 20)}
    edt = {c: ndimage.distance_transform_edt(m) for c, m in masks.items()}
    for s in pp:
        e = edt.get(s['colour'])
        if e is None or s['part'] not in ('antlers', 'fur', 'face', 'under'):
            continue
        P = s['P'] + [0, LIFT]
        d = []
        for (ax, ay), (bx, by) in zip(P, P[1:]):
            for t in np.linspace(0, 1, 6):
                xi, yi = int((ax + t * (bx - ax)) / K), int((ay + t * (by - ay)) / K)
                d.append(e[max(0, yi - 1):yi + 2, max(0, xi - 1):xi + 2].max())
        w = (2 * float(np.median(d)) - 0.5) * K
        s['w'] = round(min(1.5, max(0.45, w)) / 0.05) * 0.05


# ---------------------------------------------------------------------------------------------------------------
# the split: what stays SVG, what screen.c draws

def breath_box():
    """the breath clouds' footprint at their largest (BREATH_MAX about each nostril), lifted, grown by 1 px"""
    return BREATH_BOX


def inside(P, box):
    x0, y0, x1, y1 = box
    return bool(((P[:, 0] >= x0) & (P[:, 0] <= x1) & (P[:, 1] >= y0) & (P[:, 1] <= y1)).any())


def scaled(P, c, sx, sy=None):
    sy = sx if sy is None else sy
    return np.c_[c[0] + (P[:, 0] - c[0]) * sx, c[1] + (P[:, 1] - c[1]) * sy]


def split(pp):
    """-> svg parts (draw order) and code parts"""
    global BREATH_BOX
    boxes = []
    for part, c in (('breathL', NOSTRIL_L), ('breathR', NOSTRIL_R)):
        c = (c[0], c[1] - LIFT)
        P = scaled(np.vstack([s['P'] for s in pp if s['part'] == part]), c, BREATH_MAX)
        boxes.append((P[:, 0].min() - 1, P[:, 1].min() - 1, P[:, 0].max() + 1, P[:, 1].max() + 1))
    BREATH_BOX = boxes
    svg = {k: [] for k in ('land', 'knot', 'moon', 'cloud', 'under', 'breathL', 'breathR')}
    code = {k: [] for k in ('ribbon', 'fringe', 'antlers', 'fur', 'face_lav', 'face_white')}
    for s in pp:
        p = s['part']
        if p in ('land', 'knot', 'moon', 'cloud', 'breathL', 'breathR'):
            svg[p].append(s)
        elif p in ('fur', 'face') and any(inside(s['P'], b) for b in boxes):
            svg['under'].append(s)                 # under the breath: SVG, so the clouds draw over it
        elif p == 'fur':
            code['fur'].append(s)
        elif p == 'face':
            code['face_' + s['colour']].append(s)
        elif p == 'antlers':
            code['antlers'].append(s)
        elif p == 'aurora':
            P = s['P']
            fr = s['len'] < 16 and np.ptp(P[:, 0]) < 0.5 * np.ptp(P[:, 1])
            if fr:                                  # a fringe stroke: from its top end down
                s = dict(s, P=P[np.argsort(P[:, 1])] if len(P) == 2 else (P if P[0, 1] <= P[-1, 1] else P[::-1]))
            code['fringe' if fr else 'ribbon'].append(s)
    return svg, code


def ring_ranges(svg_moon, moon):
    """the moon ring's drawn arcs: (from, to) angles in degrees (0 = right, clockwise, y down), lifted centre"""
    cx, cy, r = moon[0], moon[1] - LIFT, moon[2]
    out = []
    for s in svg_moon:
        a = np.degrees(np.unwrap(np.arctan2(s['P'][:, 1] - cy, s['P'][:, 0] - cx)))
        out.append((float(a.min()), float(a.max())))
    return sorted(out), (cx, cy, r)


# ---------------------------------------------------------------------------------------------------------------
# writing

def q2(v):
    return int(round(v * 4))


def write_geom(code, star_list, ranges, moon):
    pts, table, parts = [], [], []
    for name in ('ribbon', 'fringe', 'antlers', 'fur', 'face_lav', 'face_white'):
        first = len(table)
        for s in code[name]:
            table.append((len(pts) // 2, len(s['P']), int(round(s.get('w', WIDTH[s['colour']]) * 1.25 * 1024))))
            for x, y in s['P']:
                pts += [q2(x), q2(y)]
        parts.append((name, first, len(table)))
    L = ['/* generated by gen_svg.py from elg-art.png: the parts of ELG\'s screen that screen.c draws (raster lines).',
         ' * Points in quarter screen px (lifted: the SVG\'s coordinates); a stroke = (first point, point count, width:',
         ' * display px Q10, the stroke\'s measured width x 1.25). */',
         f'#define EG_NPTS {len(pts) // 2}', f'#define EG_NSTROKES {len(table)}']
    for name, a, b in parts:
        L.append(f'#define EG_{name.upper()}_0 {a}')
        L.append(f'#define EG_{name.upper()}_1 {b}')
    L.append('static const int16_t EG_PTS[%d] = {' % len(pts))
    for i in range(0, len(pts), 24):
        L.append('    ' + ', '.join(map(str, pts[i:i + 24])) + ',')
    L.append('};')
    L.append('static const uint16_t EG_STROKE[%d][3] = {' % len(table))
    for i in range(0, len(table), 8):
        L.append('    ' + ', '.join(f'{{{a}, {n}, {w}}}' for a, n, w in table[i:i + 8]) + ',')
    L.append('};')
    cx, cy, r = moon
    L += ['/* the moon ring (white, SVG): centre, radius, Q2 px; its drawn arcs in degrees (0 = right, clockwise) */',
          f'#define EG_MOON_X {q2(cx)}', f'#define EG_MOON_Y {q2(cy)}', f'#define EG_MOON_R {q2(r)}',
          f'#define EG_NARCS {len(ranges)}',
          'static const int16_t EG_ARC[%d][2] = { %s };' % (len(ranges), ', '.join(f'{{{int(round(a))}, {int(round(b))}}}' for a, b in ranges)),
          '/* the stars (lavender sparkles): centre and arm, Q2 px */',
          f'#define EG_NSTARS {len(star_list)}',
          'static const int16_t EG_STAR[%d][3] = { %s };' % (len(star_list), ', '.join(f'{{{q2(x)}, {q2(y - LIFT)}, {q2(r)}}}' for x, y, r in star_list)),
          '/* pivots, Q2 px: the antlers spread about their base, each breath cloud scales about its nostril */',
          f'#define EG_ANT_X {q2(ANTLER_BASE[0])}', f'#define EG_ANT_Y {q2(ANTLER_BASE[1] - LIFT)}',
          f'#define EG_NOSE_LX {q2(NOSTRIL_L[0])}', f'#define EG_NOSE_RX {q2(NOSTRIL_R[0])}', f'#define EG_NOSE_Y {q2(NOSTRIL_L[1] - LIFT)}',
          '/* widths (raster lines are in display px: the SVG unit x 1.25), Q10 */']
    for c in ('red', 'blue', 'brown', 'lav', 'white'):
        L.append(f'#define EG_W_{c.upper()} {int(round(WIDTH[c] * 1.25 * 1024))}')
    GEOM.write_text('\n'.join(L) + '\n')
    return len(pts) // 2


def write_svg(svg, berry_list, pine_list):
    body = [el(s) for s in svg['land']]
    for ring, _ in pine_list:
        d = 'M' + ' L'.join(f'{f2(x)} {f2(y - LIFT)}' for x, y in ring) + ' Z'
        body.append(f'<path d="{d}" fill="{STOCK["navy"]}" stroke="{STOCK["navy"]}" stroke-width="0.3" stroke-linejoin="round"/>')
    body += [el(s) for s in svg['knot']]
    for x, y, r in berry_list:
        body.append(f'<circle cx="{f2(x)}" cy="{f2(y - LIFT)}" r="{f2(r)}" fill="{STOCK["green"]}" stroke="{STOCK["green"]}" stroke-width="0.4"/>')
    for part in ('moon', 'cloud', 'under'):
        body += [el(s) for s in svg[part]]
    for part in ('breathL', 'breathR'):
        body.append(f'<g id="{part}">' + ''.join(el(s) for s in svg[part]) + '</g>')
    # iter's screen code (elg_plugin.c screen_init/update) turns bar0..bar3: kept, hidden in black
    for i in range(4):
        x = 1.5 + 2 * i
        body.append(f'<circle id="base{i}" cx="{f2(x)}" cy="2" r="0.5" fill="#010101" stroke="#010101" stroke-width="0.5"/>')
        body.append(f'<line id="bar{i}" x1="{f2(x)}" y1="2" x2="{f2(x)}" y2="1" stroke="#010101" stroke-width="1"/>')
    SVG.write_text('<svg version="1.1" id="Layer_1" xmlns="http://www.w3.org/2000/svg" x="0px" y="0px" width="320px" '
                   'height="160px" viewBox="0 0 320 160">\n' + '\n'.join(body) + '\n</svg>\n')


def count(svg_text):
    import re
    els = re.findall(r'<(line|polyline|path|circle|g)\b([^>]*)>', svg_text)
    n_el = len(els)
    n_v = 0
    for tag, a in els:
        if tag == 'polyline':
            n_v += len(re.findall(r'[-\d.]+,[-\d.]+', a))
        elif tag == 'path':
            n_v += len(re.findall(r'[ML]', a))
        elif tag == 'line':
            n_v += 2
        elif tag == 'circle':
            n_v += 1
    return n_el, n_v


# ---------------------------------------------------------------------------------------------------------------
# bounds: every still and moving ink at the knob extremes

def check_bounds(svg, code, star_list, moon):
    bad = []

    def chk(name, P, w):
        x0, y0 = P.min(0) - w / 2
        x1, y1 = P.max(0) + w / 2
        if x0 < X0 or x1 > X1 or y0 < Y0 or y1 > Y1:
            bad.append(f'{name}: x {x0:.1f}..{x1:.1f} y {y0:.1f}..{y1:.1f}')
    for part, ss in svg.items():
        for s in ss:
            chk(part, s['P'], WIDTH[s['colour']])
    for part, ss in code.items():
        for s in ss:
            chk(part, s['P'], WIDTH[s['colour']])
    base = (ANTLER_BASE[0], ANTLER_BASE[1] - LIFT)
    for s in code['antlers']:
        chk('antlers at the widest', scaled(s['P'], base, ANT_SX, ANT_SY), WIDTH['blue'])
    for part, c in (('breathL', NOSTRIL_L), ('breathR', NOSTRIL_R)):
        for s in svg[part]:
            chk(part + ' at the largest', scaled(s['P'], (c[0], c[1] - LIFT), BREATH_MAX), WIDTH['green'])
    for s in code['ribbon'] + code['fringe']:
        P = s['P'] + [0, AUR_DROP]
        chk('aurora dropped', P, WIDTH['red'])
    for s in code['fringe']:
        P = s['P'].copy()
        P[-1, 1] = P[0, 1] + (P[-1, 1] - P[0, 1]) * AUR_STRETCH + AUR_DROP
        chk('fringe stretched', P, WIDTH['red'])
    cx, cy, r = moon
    chk('moon glow', np.array([[cx - r - 6, cy - r - 6], [cx + r + 6, cy - r - 6]]), 0.6)
    for x, y, r in star_list:
        chk('star', np.array([[x - r, y - LIFT - r], [x + r, y - LIFT + r]]), 0.6)
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--retrace', action='store_true')
    ap.add_argument('--report', type=Path)
    ap.add_argument('--design', type=Path, help='also write the whole design (SVG + code parts at rest) as one SVG')
    a = ap.parse_args()
    if a.retrace:
        retrace()
    im = png()
    star_list, berry_list, pine_list = stars(im), berries(im), pines(im)
    ss, moon = classify(strokes(), im, star_list, pine_list)
    pp = prepare(ss)
    measure_widths(im, pp)
    svg, code = split(pp)
    ranges, lmoon = ring_ranges(svg['moon'], moon)
    bad = check_bounds(svg, code, star_list, lmoon)
    if bad:
        raise SystemExit('ink outside x %g..%g, y %g..%g:\n  ' % (X0, X1, Y0, Y1) + '\n  '.join(bad))
    npts = write_geom(code, star_list, ranges, lmoon)
    write_svg(svg, berry_list, pine_list)
    n_el, n_v = count(SVG.read_text())
    rep = {'svg': {'elements': n_el, 'vertices': n_v, 'te_estimate': te_bytes(n_el, n_v),
                   'segments': sum(segs(s) for v in svg.values() for s in v),
                   'parts': {k: [len(v), sum(segs(s) for s in v)] for k, v in svg.items()}},
           'code': {'strokes': sum(len(v) for v in code.values()), 'points': npts,
                    'segments': sum(segs(s) for v in code.values() for s in v) + 4 * len(star_list),
                    'parts': {k: [len(v), sum(segs(s) for s in v)] for k, v in code.items()}},
           'all_as_svg_estimate': te_bytes(len(pp) + 2 * len(star_list) + len(berry_list),
                                           sum(len(s['P']) for s in pp) + 4 * len(star_list) + len(berry_list)),
           'stars': star_list, 'berries': berry_list, 'moon': lmoon, 'ring_arcs': ranges}
    print(json.dumps({k: rep[k] for k in ('svg', 'code', 'all_as_svg_estimate')}, indent=1))
    if a.report:
        a.report.write_text(json.dumps(rep, indent=1) + '\n')
    if a.design:
        allp = [s for v in svg.values() for s in v] + [s for v in code.values() for s in v]
        body = SVG.read_text().rsplit('</svg>', 1)[0]
        extra = [el(s) for v in code.values() for s in v]
        for x, y, r in star_list:
            for (p, q) in sparkle(x, y, r):
                extra.append(el({'P': np.array([p, q]), 'colour': 'lav'}))
        a.design.write_text(body + '\n'.join(extra) + '\n</svg>\n')


if __name__ == '__main__':
    main()
