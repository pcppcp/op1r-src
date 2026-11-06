#!/usr/bin/env python3
"""Generate pew.svg + pew_geom.h: PEW's screen, a jazz club organist at a Hammond with a Leslie, traced from the
owner's mockup.

    python3 synth_sdk/plugins/pew/gen_svg.py [--frame-png OUT.png] [--live-svg OUT.svg]

The art is pew-art.png: the owner's pick, concept 3 "jazz club" (2026-10-29), a 1536 x 1024 image-model PNG. No vector
file exists, so it is traced with display/art_trace.py (docs/screen-art.md, "From a PNG").

The fit: one uniform scale, no offsets picked by hand. The drawing's ink spans the mockup's whole width (x 0..1535)
and y 153..875; the height limits the scale (top ink at y 3, bottom ink at y 138, the OLED's bottom margin), so the
frame is the 2:1 window X -88..1624, Y 137..993 of the mockup (black beyond the PNG's edges), scale 320 / 1712 =
0.187: the mockup lands on x 16.4..303.6. Then the spread (the owner, 2026-10-29: fill the width, as JUNI and
Wavetable; spread()): the left group (window, city, moon, table, chair, glass, candle) and the right group (the Leslie,
its grille and live lines, the plant) move out sideways, whole, to x 2.2 / 317.8 (13.8 px each way); the middle (the
organist, the organ, the lamp, notes and smoke) stays at the uniform fit, and the room's floor and wall lines that
run on under a side group move with it past the boundary. The orange stays Oxbow's ochre (the owner, 2026-10-29).

Colours: traced with the mockup's measured colours, then each one written as its stock / encoder colour (the owner's
palette rule): blue #4C72F3 -> #698EFF, green #01E895 -> #00ED95, red #F33351 -> #FF3A5D, white -> #FFFFFF, the purple
room (window, city, walls, table, chair, floor) #5B45DD -> the dim indigo #383572, the orange wood #F68A3D -> Oxbow's
light ochre #D6A259 and the Leslie's dim grille #773528 -> that ochre at 55 % (#765932). Strokes as measured (about
0.8 px), at least 1.0 px so they stay lit on the OLED.

What is traced 1:1 into pew.svg (all of it still, drawn once through the core's static-prefix cache): the room, the
organist (hat, face, hair, coat, hands, legs, stool), the organ's case and both manuals, the Leslie cabinet, horn and
grille, the lamp's shade and bulb, the table with glass and candle, the plant, the plus sparkles and the white swooshes.
Filled parts (keys, noteheads, the moon, the bulb, the flame) are traced as filled contours (one M per path, as R246's
fill walker needs), the rest as centre lines.

What is live (pew_geom.h, from the same trace):
  drawbars   the 2 x 9 drawbars on the organ's top, drawn by screen.c as bars pulled out to their registration
             (the mockup's bars are measured for their row, spacing, width and longest pull)
  notes      the three green notes (SVG nodes after the cache boundary, ids note0..note2): screen.c moves them up
  smoke      the white smoke from the cigarette (points of its traced line), drawn by screen.c (the owner kept
             the cigarette, 2026-10-29)
  rotor      the red motion lines round the horn and the drum (ellipses fitted to the traced arcs), drawn by screen.c
  lamp       the lamp's bulb centre and the shade's rim, for screen.c's glow
--frame-png writes the framed mockup (what the tracer sees) for art_compare; --live-svg an SVG of the live parts at
the mockup's state, for comparing.
Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""
import argparse
import math
import re
import os
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'display'))
import art_trace  # noqa: E402
from svgart import f2, mix, rounded, shape  # noqa: E402
from svg_lite import lighten  # noqa: E402

ART = HERE / 'pew-art.png'
FX0, FY0, FW = -88, 137, 1712                   # the uniform frame in mockup px (2:1, so 856 tall)
S = 320 / FW
INK_BOX = (2.0, 318.0, 3.0, 138.0)              # nothing drawn outside this (x0, x1, y0, y1), live parts included
MEASURED = ['#773528', '#5B45DD', '#F33351', '#4C72F3', '#F68A3D', '#01E895', '#F7F7F8']
GRILLE, ROOM, RED, BLUE, WOOD, GREEN, WHITE = MEASURED
ORANGE = '#D6A259'
STOCK = {GRILLE: mix(ORANGE, 0.55), ROOM: '#383572', RED: '#FF3A5D', BLUE: '#698EFF', WOOD: ORANGE,
         GREEN: '#00ED95', WHITE: '#FFFFFF'}
MIN_W = 1.0                                     # screen px: the thinnest stroke written
SOLID = 4.0                                     # mockup px: ink thicker than this from its edge is a filled part
RDP = 0.12                                      # screen px: point reduction of the fitted lines
SPECK = 1.2                                     # screen px: shorter strokes are dropped

# the live parts' boxes, mockup px
BARS = [(688, 428), (1000, 428), (1000, 538), (688, 508)]     # the drawbar well (blue, green, white in it are live)
NOTES = (760, 240, 1065, 425)                   # the green notes
SMOKE = [(470, 135), (640, 135), (640, 336), (512, 336), (512, 326), (470, 326)]   # the white smoke (not the cigarette)
ROTOR_X = 1150                                  # red lines centred right of this: the Leslie's motion lines
ROTOR_SPLIT = 500                               # mockup y: horn lines above, drum lines below
LAMP = (680, 160, 800, 245)                     # the lamp (blue shade, orange bulb)
# the two manuals, measured in the mockup (px): the first white key's front-left corner, the step to the next key,
# the key's length (front to back) and how many; each key is drawn where the mockup shows its white (keyboard())
MANUALS = [((587, 532), (14.3, 3.7), (75, -27), 20),        # upper
           ((649, 642), (15.0, 4.8), (85, -27), 9)]         # lower (left of it the hands)
KEY_W = 0.78                                    # a white key's width, in key steps (the rest is the black gap)


def sc(X, Y):
    """mockup px -> screen px"""
    return ((X - FX0) * S, (Y - FY0) * S)


def framed():
    im = Image.open(ART).convert('RGB')
    out = Image.new('RGB', (FW, FW // 2), (0, 0, 0))
    out.paste(im, (-FX0, -FY0))
    return out


def in_poly(x, y, poly):
    inside = False
    for (ax, ay), (bx, by) in zip(poly, poly[1:] + poly[:1]):
        if (ay > y) != (by > y) and x < ax + (y - ay) * (bx - ax) / (by - ay):
            inside = not inside
    return inside


def region_masks(shape_):
    """framed-pixel masks of the live boxes"""
    h, w = shape_
    yy, xx = np.mgrid[0:h, 0:w]
    X, Y = xx + FX0, yy + FY0

    def box(b):
        return (X >= b[0]) & (X < b[2]) & (Y >= b[1]) & (Y < b[3])
    def poly(q):
        m = np.zeros((h, w), bool)
        x0, x1 = min(p[0] for p in q), max(p[0] for p in q)
        y0, y1 = min(p[1] for p in q), max(p[1] for p in q)
        ys, xs = np.nonzero(box((x0, y0, x1, y1)))
        for y, x in zip(ys, xs):
            if in_poly(X[y, x], Y[y, x], q):
                m[y, x] = True
        return m
    bars = poly(BARS)
    keys = np.zeros((h, w), bool)
    for (fx, fy), (px, py), (kx, ky), n in MANUALS:
        q = [(fx - 2, fy + 3), (fx + n * px + 2, fy + n * py + 3), (fx + n * px + kx + 2, fy + n * py + ky - 3),
             (fx + kx - 2, fy + ky - 3)]
        x0, x1 = int(min(p[0] for p in q)), int(max(p[0] for p in q)) + 1
        y0, y1 = int(min(p[1] for p in q)), int(max(p[1] for p in q)) + 1
        ys, xs = np.nonzero(box((x0, y0, x1, y1)))
        for y, x in zip(ys, xs):
            if in_poly(X[y, x], Y[y, x], q):
                keys[y, x] = True
    return {'manuals': keys, 'bars': bars, 'notes': box(NOTES), 'smoke': poly(SMOKE), 'rotor': (X >= ROTOR_X), 'lamp': box(LAMP),
            'X': X, 'Y': Y}


# ---- filled parts: contours ----

def contour(m):
    """outer boundary of one 8-connected component (Moore neighbour trace), pixel centres -> [(x, y)]"""
    m = np.pad(m, 1)
    ys, xs = np.nonzero(m)
    i = np.lexsort((xs, ys))[0]
    start = (int(ys[i]), int(xs[i]))
    nb = [(-1, 0), (-1, 1), (0, 1), (1, 1), (1, 0), (1, -1), (0, -1), (-1, -1)]
    pts, cur, d = [start], start, 6                      # came from the west
    for _ in range(m.size):
        found = False
        for k in range(8):
            dd = (d + 1 + k) % 8
            y, x = cur[0] + nb[dd][0], cur[1] + nb[dd][1]
            if m[y, x]:
                cur, d, found = (y, x), (dd + 4) % 8, True
                break
        if not found or cur == start:
            break
        pts.append(cur)
    return [(x - 1 + 0.5, y - 1 + 0.5) for y, x in pts]


def rdp(pts, eps):
    if len(pts) < 3:
        return list(pts)
    a, b = np.array(pts[0]), np.array(pts[-1])
    P = np.array(pts)
    d = b - a
    n = math.hypot(*d)
    dist = (np.abs(d[0] * (P[:, 1] - a[1]) - d[1] * (P[:, 0] - a[0])) / n) if n else np.hypot(*(P - a).T)
    i = int(np.argmax(dist))
    if dist[i] <= eps:
        return [pts[0], pts[-1]]
    return rdp(pts[:i + 1], eps)[:-1] + rdp(pts[i:], eps)


def rdp_closed(pts, eps):
    P = np.array(pts)
    j = int(np.argmax(np.hypot(*(P - P[0]).T)))
    return rdp(pts[:j + 1], eps)[:-1] + rdp(pts[j:] + [pts[0]], eps)[:-1]


def solids(ms):
    """per colour: the thick (filled) parts -> mask of them, [(colour, contour in screen px)]"""
    out, allm = [], []
    for c, m in zip(MEASURED, ms):
        core = ndimage.distance_transform_edt(m) > SOLID
        lab, n = ndimage.label(core, np.ones((3, 3)))
        big = ndimage.sum(core, lab, range(1, n + 1)) >= 25      # a stroke's junction or cross isn't filled
        core = np.isin(lab, 1 + np.nonzero(big)[0])
        s = ndimage.binary_dilation(core, iterations=int(SOLID) + 2) & m
        lab, n = ndimage.label(s, np.ones((3, 3)))
        for i in range(1, n + 1):
            comp = lab == i
            if comp.sum() < 40:
                continue
            ring = contour(comp)
            ring = [(x * S, y * S) for x, y in ring]
            ring = rdp_closed(ring, 0.22)
            if len(ring) >= 3:
                out.append((c, ring, comp))
        allm.append(s)
    return out, allm


def keyboard(white, R):
    """the white keys as filled parallelograms: each key's centre line sampled in the mockup's white mask, a key
    drawn over each run of white (gaps under 4 px bridged), so keys under the hands are cut where the mockup cuts
    them. -> [(manual, key, t0, t1, contour in screen px)]"""
    out = []
    h, w = white.shape
    grow = ndimage.binary_dilation(white, iterations=1)
    for mi, ((fx, fy), (px, py), (kx, ky), n) in enumerate(MANUALS):
        L = math.hypot(kx, ky)
        for i in range(n):
            cx, cy = fx + (i + KEY_W / 2) * px, fy + (i + KEY_W / 2) * py
            ts = np.linspace(0, 1, int(L * 2))
            on = []
            for t in ts:
                X, Y = cx + t * kx, cy + t * ky
                c, r = int(round(X - FX0)), int(round(Y - FY0))
                on.append(0 <= r < h and 0 <= c < w and bool(grow[r, c]))
            runs, t0, gap = [], None, 0
            for t, v in zip(ts, on):
                if v:
                    if t0 is None:
                        t0 = t
                    t1, gap = t, 0
                elif t0 is not None:
                    gap += 1
                    if gap > 8:
                        runs.append((t0, t1))
                        t0 = None
            if t0 is not None:
                runs.append((t0, t1))
            for t0, t1 in runs:
                if (t1 - t0) * L < 6:
                    continue
                a = (fx + i * px, fy + i * py)
                q = [(a[0] + t * kx + u * KEY_W * px, a[1] + t * ky + u * KEY_W * py)
                     for t, u in ((t0, 0), (t0, 1), (t1, 1), (t1, 0))]
                out.append((mi, i, t0, t1, [sc(*p) for p in q]))
    return out


def rotor_lines(red, R):
    """the red lines whose middle is right of ROTOR_X: the motion lines round the Leslie (the swoosh that reaches
    over from the notes stays still)"""
    lab, n = ndimage.label(ndimage.binary_dilation(red, iterations=1), np.ones((3, 3)))
    xs = ndimage.mean(R['X'], lab, range(1, n + 1))
    return np.isin(lab, 1 + np.nonzero(np.asarray(xs) > ROTOR_X)[0]) & red


def specks(ms, cut, strokes):
    """small marks the tracer leaves out (under about two stroke widths: the city's lit windows, the cigarette's
    ember, eyelashes; not the dim colours' bits or the Leslie's grille, which are shading): every component of 8..400 px that no traced line comes near, drawn as a short line along its
    long axis with its measured length and width. -> [(colour, (p0, p1), width)] in screen px"""
    pts = {}
    for c, w, piece in strokes:
        if piece[0] == 'poly' and poly_len(piece[1]) >= SPECK:
            pts.setdefault(c, []).extend(piece[1])
    out = []
    for c, m in zip(MEASURED, ms):
        if c in (GRILLE, ROOM):
            continue                                     # the dim colours' small bits are shading, not marks
        m = m & ~cut
        lab, n = ndimage.label(m, np.ones((3, 3)))
        P = np.array(pts.get(c, [(-99, -99)]))
        for i, sl in enumerate(ndimage.find_objects(lab), 1):
            comp = lab[sl] == i
            area = int(comp.sum())
            if not 8 <= area <= 400:
                continue
            ys, xs = np.nonzero(comp)
            xs = xs + sl[1].start + 0.5
            ys = ys + sl[0].start + 0.5
            cx, cy = xs.mean() * S, ys.mean() * S
            if to_mock(cx, cy)[0] > ROTOR_X:
                continue                                 # the Leslie's grille: shading
            if np.min(np.hypot(P[:, 0] - cx, P[:, 1] - cy)) < 1.0:
                continue
            Q = np.stack([xs - xs.mean(), ys - ys.mean()], 1)
            ev, evec = np.linalg.eigh(np.cov(Q.T) + 1e-6 * np.eye(2))
            d = evec[:, 1]
            proj = Q @ d
            wd = max(2 * math.sqrt(4 * ev[0]) * S, MIN_W)
            if wd > 1.6:
                continue                                 # not a dash: lines the tracer left (drawn below)
            half = max((proj.max() - proj.min()) * S / 2 - wd / 2, 0.05)
            out.append((c, ((cx - d[0] * half, cy - d[1] * half), (cx + d[0] * half, cy + d[1] * half)), wd))
    return out


GRILLE_BOX = (236.0, 64.0, 277.5, 118.0)        # screen px: the Leslie's grille panel
GRILLE_X0, GRILLE_STEP, GRILLE_N = 238.5, 2.1, 18   # its bars (the traced bars' x, about 2.1 px apart)


def in_grille(P):
    """a short, upright piece inside the grille panel (its bars' brighter runs, not the cabinet's edges or rails)"""
    P = np.asarray(P)
    x0, y0, x1, y1 = GRILLE_BOX
    inside = (P[:, 0] > x0 + 1.5).all() and (P[:, 0] < x1 - 1.5).all() and (P[:, 1] > y0).all() and (P[:, 1] < y1).all()
    d = P[-1] - P[0]
    return inside and abs(d[0]) < 0.4 * abs(d[1]) + 0.5 and poly_len(P) < 30


GRILLE_TOP, GRILLE_FOOT = 68.3, ((238.5, 110.0), (274.2, 115.4))   # the bars' tops, the line of their feet


# ---- the spread (the owner, 2026-10-29: fill the width, as JUNI and Wavetable) ----
SIDE_L, SIDE_L_ROOM, SIDE_R = 53.8, 70.7, 225.8     # screen px at the uniform fit (mockup x 200, 290, 1120)
EDGE = (2.2, 317.8)                                 # where the side groups' outermost ink goes
DX = {'L': 0.0, 'R': 0.0}


def side(col, xs):
    """L: the window, the city and moon, the table, chair, glass and candle (wholly left of mockup x 200, or of x 290
    in the room's purple or the wood's ochre: the organist's coat reaches x 205); R: the Leslie, its grille and
    lines, the plant (wholly right of mockup x 1120: the organ ends at 1110); else C, the middle"""
    if max(xs) < SIDE_L or (max(xs) < SIDE_L_ROOM and col in (STOCK[ROOM], STOCK[WOOD])):
        return 'L'
    return 'R' if min(xs) > SIDE_R else 'C'


def spread(lines, static, rotor_st):
    """the uniform fit is limited by the height, so the two side groups move out sideways, each whole, until their
    outermost ink (strokes, and the Leslie's live lines at any angle) is on EDGE; sizes are the uniform fit's. A
    middle line that runs out under a side group (the floor and wall lines) has its points past the boundary moved
    with that group, so it still reaches the edge (the room's colour only: the organ and the red swoosh stay whole).
    Sets DX. -> lines, static"""
    def xs_of(e):
        return [p[0] for p in (e[2][1] if e[0] == 'path' else [e[2][:2]])]
    ext = {'L': [], 'R': []}
    for e in lines:
        xs = xs_of(e)
        sd = side(e[1], xs)
        if sd in ext:
            w = e[3] / 2 if e[0] == 'path' else e[3] / 2 + e[2][2]
            ext[sd].append(min(xs) - w if sd == 'L' else max(xs) + w)
    for _, col, ring in static:
        xs = [p[0] for p in ring]
        sd = side(col, xs)
        if sd in ext:
            ext[sd].append(min(xs) if sd == 'L' else max(xs))
    for c, w, pc in rotor_st:
        if pc[0] == 'poly' and poly_len(pc[1]) > 4:
            ext['R'].append(max(p[0] for p in pc[1]) + 1.5)       # their ring's far side as they turn: about
    DX['L'] = EDGE[0] - min(ext['L'])
    DX['R'] = EDGE[1] - max(ext['R'])
    # the live lines turn round their ring: the farthest any reaches is its ring's centre plus its radius
    out = []
    for e in lines:
        xs = xs_of(e)
        sd = side(e[1], xs)
        if e[0] == 'circle':
            cx, cy, r = e[2]
            out.append(('circle', e[1], (cx + (DX[sd] if sd in DX else 0), cy, r), e[3]))
            continue
        pts = e[2][1]
        if sd in DX:
            q = [(x + DX[sd], y) for x, y in pts]
        else:
            room = e[1] == STOCK[ROOM]                  # only the floor and wall lines run on under a side group
            q = [(x + (DX['R'] if room and x > SIDE_R else DX['L'] if room and x < SIDE_L_ROOM else 0), y)
                 for x, y in pts]
        out.append(('path', e[1], ('poly', q, e[2][2]) + tuple(e[2][3:]), e[3]))
    st = []
    for kind, col, ring in static:
        sd = side(col, [p[0] for p in ring])
        st.append((kind, col, [(x + DX.get(sd, 0), y) for x, y in ring]))
    return out, st


def grille():
    """the grille's bars as straight lines from under the rail to the slanting bottom rail (the traced bars' ends;
    the mockup's dark shading between them, which traces as scribbles, is left out). -> [((x, y0), (x, y1))]"""
    (xa, ya), (xb, yb) = GRILLE_FOOT
    return [((x, GRILLE_TOP), (x, ya + (yb - ya) * (x - xa) / (xb - xa)))
            for x in (GRILLE_X0 + i * GRILLE_STEP for i in range(GRILLE_N))]


def bridge(lines):
    """pieces of one colour and width that continue each other across a small gap (under 2.4 px, the tracer's cuts
    where a live line crossed or two lines touched) become one; never across a sharp turn"""
    lines = list(lines)
    merged = True
    while merged:
        merged = False
        for i, a in enumerate(lines):
            if a[0] != 'path' or a[2][2]:
                continue
            pa = list(a[2][1])
            for j, b in enumerate(lines):
                if j == i or b[0] != 'path' or b[1] != a[1] or b[3] != a[3] or b[2][2]:
                    continue
                pb = list(b[2][1])
                for qa in (pa, pa[::-1]):
                    for qb in (pb, pb[::-1]):
                        gap = math.dist(qa[-1], qb[0])
                        if gap > 2.4 or len(qa) < 2 or len(qb) < 2:
                            continue
                        ta = np.subtract(qa[-1], qa[-2]); tb = np.subtract(qb[1], qb[0])
                        g = np.subtract(qb[0], qa[-1])
                        na, nb = np.linalg.norm(ta), np.linalg.norm(tb)
                        if not na or not nb or np.dot(ta, tb) / (na * nb) < 0.9:
                            continue
                        if gap > 0.3 and np.dot(ta, g) / (na * gap) < 0.9:
                            continue
                        lines[i] = ('path', a[1], ('poly', qa + qb, False), a[3])
                        del lines[j]
                        merged = True
                        break
                    if merged:
                        break
                if merged:
                    break
            if merged:
                break
    return lines


def recolour(A, c, piece):
    """a blue piece whose pixels are the room's purple, brighter (the floor and wall lines brighten along their length,
    the tracer cuts them into purple and blue runs) goes back to the room's colour: by the hue (unit RGB vectors) at
    its points"""
    if c != BLUE or piece[0] != 'poly':
        return c
    h, w = A.shape[:2]
    cols = [A[min(h - 1, int(y / S)), min(w - 1, int(x / S))] for x, y in piece[1]]
    cols = [q for q in cols if q.max() > 60]
    if not cols:
        return c
    m = np.mean(cols, 0)
    m = m / np.linalg.norm(m)
    b, r = art_trace.rgb(BLUE), art_trace.rgb(ROOM)
    return ROOM if np.dot(m, r / np.linalg.norm(r)) > np.dot(m, b / np.linalg.norm(b)) else c


def poly_len(p):
    return sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(p, p[1:]))


def centroid(p):
    return (sum(x for x, _ in p) / len(p), sum(y for _, y in p) / len(p))


def to_mock(x, y):
    return (x / S + FX0, y / S + FY0)


def trace(img):
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'f.png')
        img.save(p)
        r = art_trace.trace(p, MEASURED)
    widths = {q['color']: q['width'] for q in r['palette']}
    return r['strokes'], widths


def P2(p):
    return f'{f2(p[0])} {f2(p[1])}'


def ends(piece):
    """start and end point and the direction going in / coming out (unit)"""
    p = rdp(list(piece[1]), RDP)
    t0, t1 = np.subtract(p[1], p[0]), np.subtract(p[-1], p[-2])
    return p[0], p[-1], t0 / (np.linalg.norm(t0) or 1), t1 / (np.linalg.norm(t1) or 1)


def joined(lines):
    """one <path> per run of pieces of one colour and width that continue each other (the tracer splits a line at its
    corners): fewer elements, the same geometry. Never across a turn-back (R246 draws one as a spike)"""
    out, cur = [], None
    for e in lines:
        if e[0] == 'circle':
            cx, cy, r = e[2]
            out.append(f'<circle cx="{f2(cx)}" cy="{f2(cy)}" r="{f2(r)}" fill="none" stroke="{e[1]}" '
                       f'stroke-width="{f2(e[3])}"/>')
            cur = None
            continue
        _, col, piece, w = e
        a, b, t0, t1 = ends(piece)
        if (cur and cur[0] == col and cur[1] == w and math.dist(cur[3], a) < 0.35
                and float(np.dot(cur[4], t0)) > -0.9):
            cur[2] += rdp(list(piece[1]), RDP)[1:]
            cur[3], cur[4] = b, t1
            continue
        if cur:
            out.append(emit(cur))
        cur = [col, w, rdp(list(piece[1]), RDP), b, t1, piece[2]]
    if cur:
        out.append(emit(cur))
    return out


def emit(cur):
    col, w = cur[0], cur[1]
    q = rounded(cur[2])
    if len(q) < 2:
        q = [tuple(map(float, cur[2][0])), tuple(map(float, cur[2][-1]))]
    d = 'M' + P2(q[0]) + ''.join('L' + P2(p) for p in q[1:])
    return (f'<path fill="none" stroke="{col}" stroke-width="{f2(w)}" stroke-linecap="round" '
            f'stroke-linejoin="round" d="{d}"/>')


def ellipse_fit(P):
    """ellipse about the points' box centre (a ring's lines are only parts of it, so the centre is not fitted), tilted:
    a u^2 + b u v + c v^2 = 1 by least squares -> cx, cy, rx, ry, tilt (radians); untilted box half sizes if that
    fails"""
    P = np.asarray(P, float)
    cx, cy = (P[:, 0].min() + P[:, 0].max()) / 2, (P[:, 1].min() + P[:, 1].max()) / 2
    u, v = P[:, 0] - cx, P[:, 1] - cy
    a, b, c = np.linalg.lstsq(np.stack([u * u, u * v, v * v], 1), np.ones(len(P)), rcond=None)[0]
    ev, evec = np.linalg.eigh(np.array([[a, b / 2], [b / 2, c]]))
    if ev.min() <= 0:
        return cx, cy, max(abs(u).max(), 1), max(abs(v).max(), 1), 0.0
    k = int(np.argmin(ev))                      # the long axis
    t = math.atan2(evec[1, k], evec[0, k])
    if t > math.pi / 2:
        t -= math.pi
    if t < -math.pi / 2:
        t += math.pi
    return cx, cy, 1 / math.sqrt(ev[k]), 1 / math.sqrt(ev[1 - k]), t


# ---- the live parts' geometry (pew_geom.h) ----

PULSE_KEYS = (8, 10, 12, 14, 16, 18)            # upper manual keys lit per playing voice (right of the hands)
BAR_N = 9                                       # drawbars per manual


def resample(P, step):
    P = np.asarray(P, float)
    d = np.concatenate([[0], np.cumsum(np.hypot(*np.diff(P, axis=0).T))])
    t = np.arange(0, d[-1] + 1e-9, step)
    return np.stack([np.interp(t, d, P[:, 0]), np.interp(t, d, P[:, 1])], 1)


def geometry(bars, smoke_st, rotor_st, keys, notes, bulb, shade):
    g = {}
    # drawbars: the measured bars of each manual (left / right of mockup x 850): 9 evenly from the first to the last,
    # their feet on the line fitted through the measured feet; the longest pull = the tallest measured bar
    g['bars'] = []
    for side in (lambda b: b[0] < 850, lambda b: b[0] >= 850):
        bs = [b for b in bars if side(b)]
        xs = np.array([b[0] for b in bs])
        k, c = np.polyfit(xs, [b[2] for b in bs], 1)
        x0, x1 = xs.min(), xs.max()
        feet = [sc(x, k * x + c) for x in np.linspace(x0, x1, BAR_N)]
        g['bars'].append(feet)
    g['bar_len'] = max(b[2] - b[1] for b in bars) * S
    g['bar_w'] = 6.5 * S                         # a stem's width in the mockup (about 6.5 px)
    # smoke: the traced line from the cigarette up, every 2.5 px
    sm = max((p for c, w, p in smoke_st if p[0] == 'poly'), key=lambda p: poly_len(p[1]))[1]
    sm = list(sm)
    if sm[0][1] < sm[-1][1]:
        sm = sm[::-1]                           # from the cigarette (bottom) up
    g['smoke'] = resample(sm, 2.5)
    # rotor: the motion lines in three rings (above the cabinet, round the horn, round the drum), an ellipse fitted to
    # each ring's lines; each line kept as (ring, start angle, sweep, radius scale), angles 8192 per turn
    arcs = [p[1] for c, w, p in rotor_st if p[0] == 'poly' and poly_len(p[1]) > 4]
    def ring_of(P):
        y = np.mean([q[1] for q in P])
        return 0 if y < 35 else 1 if y < 65 else 2
    g['rings'], g['arcs'] = [], []
    for r in range(3):
        pts = [q for P in arcs if ring_of(P) == r for q in P]
        cx, cy, rx, ry, tl = ellipse_fit(pts)
        g['rings'].append((cx + DX['R'], cy, rx, ry, tl))   # the Leslie's group moved out (spread())
        for P in arcs:
            if ring_of(P) != r:
                continue
            P = np.asarray(P)
            du, dv = P[:, 0] - cx, P[:, 1] - cy
            u = (du * math.cos(tl) + dv * math.sin(tl)) / rx
            v = (-du * math.sin(tl) + dv * math.cos(tl)) / ry
            th = np.unwrap(np.arctan2(v, u))
            a0, a1 = th[0], th[-1]
            if a1 < a0:
                a0, a1 = a1, a0
            g['arcs'].append((r, a0 / (2 * math.pi) * 8192 % 8192, (a1 - a0) / (2 * math.pi) * 8192,
                              float(np.mean(np.hypot(u, v)))))
    # the lamp: the bulb's centre (its orange) and the shade's rim (the blue's lowest row there)
    ys, xs = np.nonzero(bulb)
    g['bulb'] = ((xs.mean() + 0.5) * S, (ys.mean() + 0.5) * S, (xs.max() - xs.min() + 1) * S / 2)
    # the keys lit per voice: their centre lines and width
    g['pulse'] = []
    (fx, fy), (px, py), (kx, ky), n = MANUALS[0]
    for k in PULSE_KEYS:
        t0, t1 = max(((q[2], q[3]) for q in keys if q[0] == 0 and q[1] == k), key=lambda t: t[1] - t[0])
        a = (fx + (k + KEY_W / 2) * px, fy + (k + KEY_W / 2) * py)
        g['pulse'].append((sc(a[0] + t0 * kx, a[1] + t0 * ky), sc(a[0] + t1 * kx, a[1] + t1 * ky)))
    g['key_w'] = KEY_W * math.hypot(px, py) * S * 0.8
    # the notes' centres (their spawn and travel are screen.c's)
    g['notes'] = [centroid([q for prt in parts for q in (prt[2])]) for parts in notes]
    return g


def q10(v):
    return int(round(v * 1024))


def header(g):
    L = ['/* generated by gen_svg.py from pew-art.png: the live parts of the PEW screen, screen px in Q10 */',
         '#define PEW_BAR_N %d' % BAR_N,
         'static const int32_t pew_bar_foot[2][%d][2] = {%s};' % (BAR_N, ', '.join(
             '{' + ', '.join('{%d, %d}' % (q10(x), q10(y)) for x, y in feet) + '}' for feet in g['bars'])),
         '#define PEW_BAR_LEN %d' % q10(g['bar_len']),
         '#define PEW_BAR_W %d' % q10(g['bar_w']),
         '#define PEW_SMOKE_N %d' % len(g['smoke']),
         'static const int32_t pew_smoke[PEW_SMOKE_N][2] = {%s};' % ', '.join(
             '{%d, %d}' % (q10(x), q10(y)) for x, y in g['smoke']),
         'static const int32_t pew_ring[3][6] = {%s};   /* cx, cy, rx, ry (Q10), tilt sin, cos (Q15) */' % ', '.join(
             '{' + ', '.join([str(q10(v)) for v in r[:4]] + [str(round(math.sin(r[4]) * 32767)),
                                                            str(round(math.cos(r[4]) * 32767))]) + '}'
             for r in g['rings']),
         '#define PEW_ARC_N %d' % len(g['arcs']),
         'static const int16_t pew_arc[PEW_ARC_N][4] = {%s};   /* ring, start, sweep (8192 per turn), radius Q10 */'
         % ', '.join('{%d, %d, %d, %d}' % (r, round(a0), round(sw), q10(rs)) for r, a0, sw, rs in g['arcs']),
         'static const int32_t pew_bulb[3] = {%d, %d, %d};   /* centre, half width */' % tuple(q10(v) for v in g['bulb']),
         'static const int32_t pew_pulse[%d][4] = {%s};' % (len(g['pulse']), ', '.join(
             '{%d, %d, %d, %d}' % (q10(a[0]), q10(a[1]), q10(b[0]), q10(b[1])) for a, b in g['pulse'])),
         '#define PEW_KEY_W %d' % q10(g['key_w']),
         'static const int32_t pew_note_home[3][2] = {%s};' % ', '.join(
             '{%d, %d}' % (q10(x), q10(y)) for x, y in g['notes'])]
    return '\n'.join(L) + '\n'


def live_preview(g):
    """the live parts at the mockup's state, as SVG (for comparing with the mockup)"""
    out = []
    for feet in g['bars']:
        for x, y in feet:
            out.append(shape([(x, y), (x, y - g['bar_len'] * 0.6)], '#888888', g['bar_w']))
    out.append(shape([tuple(p) for p in g['smoke']], '#FFFFFF', 1.0))
    for r, a0, sw, rs in g['arcs']:
        cx, cy, rx, ry, tl = g['rings'][r]
        pts = [(cx + rs * (rx * math.cos(t) * math.cos(tl) - ry * math.sin(t) * math.sin(tl)),
                cy + rs * (rx * math.cos(t) * math.sin(tl) + ry * math.sin(t) * math.cos(tl)))
               for t in np.linspace(a0, a0 + sw, 12) * 2 * math.pi / 8192]
        out.append(shape(pts, '#FF3A5D', 0.9))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--frame-png', type=Path)
    ap.add_argument('--live-svg', type=Path)
    a = ap.parse_args()
    img = framed()
    if a.frame_png:
        img.save(a.frame_png)
    A = np.asarray(img).astype(float)
    ink = A.max(2) > art_trace.INK
    pal = [art_trace.rgb(c) for c in MEASURED]
    ms = art_trace.masks(A, ink, pal)
    R = region_masks(ink.shape)
    sol, smasks = solids(ms)
    ci = {c: i for i, c in enumerate(MEASURED)}

    # live pixels: the drawbars (blue / green / white in the well), notes (green), smoke (white), rotor (red)
    live = {
        'bars': (ms[ci[BLUE]] | ms[ci[GREEN]] | ms[ci[WHITE]]) & R['bars'],
        'notes': ms[ci[GREEN]] & R['notes'],
        'smoke': ms[ci[WHITE]] & R['smoke'],
        'rotor': rotor_lines(ms[ci[RED]], R),
        'keys': ms[ci[WHITE]] & R['manuals'],          # the white keys: rebuilt (keyboard())
    }
    # the drawbars, measured: each bar's centre x, top and bottom (mockup px)
    lab, n = ndimage.label(ndimage.binary_dilation(live['bars'], iterations=1), np.ones((3, 3)))
    bars = []
    for i in range(1, n + 1):
        ys, xs = np.nonzero(lab == i)
        if len(ys) < 30:
            continue
        bars.append((float(R['X'][0, int(xs.mean())]), float(R['Y'][ys.min(), 0]), float(R['Y'][ys.max(), 0]),
                     float(xs.max() - xs.min() + 1)))
    bars.sort()

    # the still image for the line trace: live pixels and filled parts blacked out (with their antialiased edge)
    still = A.copy()
    cut = np.zeros(ink.shape, bool)
    for k in live:
        cut |= live[k]
    for s in smasks:
        cut |= s
    # with their antialiased edge, but not the other colours' lines that cross them (the cabinet under the red lines)
    assigned = np.zeros(ink.shape, bool)
    for m in ms:
        assigned |= m
    cut = ndimage.binary_dilation(cut, iterations=2) & ink & ~(assigned & ~cut)
    # keep the antialiased edges of untouched colours: only remove pixels whose own colour was cut nearby
    still[cut] = 0
    strokes, widths = trace(Image.fromarray(still.astype(np.uint8)))
    strokes = [(recolour(A, c, piece), w, piece) for c, w, piece in strokes]

    # the live lines on their own
    def only(m):
        o = np.zeros_like(A)
        mm = ndimage.binary_dilation(m, iterations=2) & ink
        o[mm] = A[mm]
        return Image.fromarray(o.astype(np.uint8))
    smoke_st, _ = trace(only(live['smoke']))
    rotor_st, _ = trace(only(live['rotor']))
    notes_st, _ = trace(only(live['notes']))

    static, notes = [], [[], [], []]
    note_c = []
    for c, ring, comp in sol:
        if c == GREEN and any(R['notes'][comp]) and comp[R['notes']].sum() > comp.sum() / 2:
            note_c.append(centroid(ring))
    # three notes: cluster the noteheads by x (two heads per eighth pair)
    heads = sorted(note_c)

    def which_note(p):
        mx, my = to_mock(*p)
        cs = [(806, 312), (931, 264), (1027, 360)]
        return min(range(3), key=lambda i: (mx - cs[i][0]) ** 2 + (my - cs[i][1]) ** 2)

    for c, ring, comp in sol:
        if c == GREEN and comp[R['notes']].sum() > comp.sum() / 2:
            notes[which_note(centroid(ring))].append(('fill', STOCK[c], ring))
        elif (live['bars'] & comp).sum() > comp.sum() / 2:
            continue
        elif c == WHITE and comp[R['manuals']].sum() > comp.sum() / 2:
            continue
        else:
            static.append(('fill', STOCK[c], ring))
    for c, w, piece in notes_st:
        if piece[0] == 'poly' and poly_len(piece[1]) >= SPECK:
            notes[which_note(centroid(piece[1]))].append(('line', STOCK[c], rdp(piece[1], RDP), max(w, MIN_W)))

    lines = []
    for c, w, piece in strokes:
        if piece[0] == 'circle':
            _, cx, cy, r = piece
            lines.append(('circle', STOCK[c], (cx, cy, r), max(w, MIN_W)))
            continue
        if poly_len(piece[1]) < SPECK:
            continue
        if c == GRILLE:
            continue                                     # the grille is rebuilt (grille())
        if c == WOOD and in_grille(piece[1]):
            continue                                     # the grille's brighter bits
        lines.append(('path', STOCK[c], piece, max(w, MIN_W)))

    # draw order: dim colours first (room, grille), then the lines, the filled parts on top
    lines = bridge(lines)
    order = {STOCK[c]: i for i, c in enumerate(MEASURED)}
    lines.sort(key=lambda e: order[e[1]])
    sp = specks(ms, cut, strokes)
    for c, (p0, p1), wd in sp:
        lines.append(('path', STOCK[c], ('poly', [p0, p1], False), round(wd / 0.05) * 0.05))
    lines, static = spread(lines, static, rotor_st)
    keys = keyboard(ms[ci[WHITE]] & R['manuals'], R)
    dim = [e for e in lines if e[1] in (STOCK[ROOM], STOCK[GRILLE])]
    body = joined(dim)
    for p0, p1 in grille():
        p0, p1 = (p0[0] + DX['R'], p0[1]), (p1[0] + DX['R'], p1[1])
        body.append(f'<line x1="{f2(p0[0])}" y1="{f2(p0[1])}" x2="{f2(p1[0])}" y2="{f2(p1[1])}" stroke="{STOCK[GRILLE]}" '
                    f'stroke-width="0.8" stroke-linecap="round"/>')
    for *_, q in keys:
        q = rounded(q, True)
        body.append('<path d="M' + ' L'.join(f'{f2(x)} {f2(y)}' for x, y in q) + ' Z" fill="#FFFFFF" stroke="none"/>')
    body += joined([e for e in lines if e not in dim])
    for _, col, ring in static:
        ring = rounded(ring, True)
        d = 'M' + ' L'.join(f'{f2(x)} {f2(y)}' for x, y in ring) + ' Z'
        body.append(f'<path d="{d}" fill="{col}" stroke="none"/>')
    live_svg = []
    for i, parts in enumerate(notes):
        g = [f'<g id="note{i}">']
        for p in parts:
            if p[0] == 'fill':
                d = 'M' + ' L'.join(f'{f2(x)} {f2(y)}' for x, y in rounded(p[2], True)) + ' Z'
                g.append(f'<path d="{d}" fill="{p[1]}" stroke="none"/>')
            else:
                g.append(shape(p[2], p[1], p[3]))
        g.append('</g>')
        live_svg.append(''.join(g))
    svg = ('<svg version="1.1" xmlns="http://www.w3.org/2000/svg" width="320px" height="160px" '
           'viewBox="0 0 320 160">\n' + '\n'.join(body + live_svg) + '\n</svg>\n')
    svg = lighten(svg, note='pew gen_svg.py, made lighter by display/svg_lite.py')[0]
    (HERE / 'pew.svg').write_text(svg)

    geom = geometry(bars, smoke_st, rotor_st, keys, notes, ms[ci[WOOD]] & R['lamp'], ms[ci[BLUE]] & R['lamp'])
    (HERE / 'pew_geom.h').write_text(header(geom))
    nodes = len(body) + sum(len(p) for p in notes)
    print(f'pew.svg: {len(svg)} bytes, {nodes} shapes ({len(body)} still), '
          f'{len(re.findall(r"-?[0-9.]+[ ,]-?[0-9.]+", svg))} points')
    if a.live_svg:
        a.live_svg.write_text('<svg version="1.1" xmlns="http://www.w3.org/2000/svg" width="320px" height="160px" '
                              'viewBox="0 0 320 160">\n' + '\n'.join(live_preview(geom)) + '\n</svg>\n')


if __name__ == '__main__':
    main()
