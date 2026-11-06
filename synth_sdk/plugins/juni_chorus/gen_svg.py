#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
"""juni chorus's screen after the owner's chosen mockup C, "swaying birches" (juni_chorus-art.png: a 1536 x 1024
image-generator PNG, no vector exists; the owner picked it 2026-10-28). Writes juni_chorus.svg (the static part,
drawn through the static-prefix cache: the ranges, the cliffs, the horizon, the clouds, the stars, the white marks
at the tree feet) and juni_chorus_geom.h (what juni_chorus_screen.c draws live: the moon and its dashed halo, the
five birches with their bark marks, branches and leaves, the red reflections).

    python3 synth_sdk/plugins/juni_chorus/gen_svg.py [ART.png]        (needs numpy, scipy, PIL)

The fit (docs/screen-art.md; one uniform scale, chosen once, no per-part offsets):
    screen x = 40 + art_x * 5/32,  screen y = art_y * 5/32 - 4        (art pixel centres at i + 0.5)
  The art's height maps to the screen's 160 rows; its 3:2 width lands at x 40..280, centred. The drawing's band
  (the halo's top, art y 85, to the lowest reflection dash, art y 890) then sits at y 9.3..135.1 with the strokes,
  inside the OLED's lower target (y <= 138, strokes and glyphs included); the 4-row lift is the only offset
  (without it the lowest dash would end at y 139.6). The art's own labels (MODE 2, DEPTH, TONE, MIX and its bar)
  are blanked before tracing; the screen's labels are native text in the corner layout of the other FX screens,
  in the 40 px bands the 3:2 -> 2:1 fit leaves, not at the art's places.

How each part is taken from the PNG (screen units; numbers printed by this script, the work folder's trace.json):
  traced    art_trace.trace() on the fit at 4x (1280 x 640) with the palette given (the tracer otherwise drops the
            navy ranges as an edge of the moon blue): the navy ranges #264096 and the dim plum lines #372353 at
            their feet, the green cliffs and shore lines (green strokes over the art's large green shapes outside
            the trees), at the tracer's measured widths.
  measured  the horizon (blue runs on art rows 637..642, gaps at the trunks kept), the two curled clouds (row
            profiles: straight runs and half-circle hooks), the three lavender plus stars and five white dot stars
            (component centres and sizes), the white marks under the trunk feet, the red reflection dashes (one
            horizontal line per component, its ends and centre row).
  moon      the blue ring and its dashed halo: least-squares circles over the blue ink, split by radius; each halo
            dash's angles from its component, less half the stroke.
  birches   each trunk's silhouette row by row (white, closed over the black bark marks) from its foot (art row
            647) up to where a branch leaves it; its centre a cubic in y, its width a straight line (the art's taper),
            sampled every 4 rows. Bark marks: the black components inside the silhouette. The upper trunk and the
            branches: the traced white strokes outside the silhouettes, each given to the nearest tree. Leaves: the
            green components that aren't cliff (about 80), each a diamond from its principal axes (tips, widest point), given
            to the tree of the nearest trunk or branch point.
Colours: measured, except the three encoder colours the art means (the leaves and cliffs, the reflections, the
birches): the trace's #00E489 / #F54058 / #F5F5F5 are within 15 of the encoder green #00ED95, red #FF3A5D and white,
so those are written. The moon blue #2E84F5 (the art's MODE label is the same blue) is 59 from the encoder blue
#698EFF and stays as measured.
Movement (juni_chorus_screen.c): every tree point moves sideways by its tree's lean times (height / tree height)^2,
the foot fixed, so leaves ride their branches; the reflection columns follow their tree's lean, mirrored.
Forced changes, written down: the trunks are white lines of the measured width (the art's filled shapes can't
bend), the bark marks black lines on them; leaves are 4-line diamonds (draw cost; 6-point almonds read the same at
1x); static traced lines are simplified within 0.25 px; sub-pixel texture of the clouds is left out.
No firmware data.
"""
from pathlib import Path
import math
import sys
import tempfile

import numpy as np
from PIL import Image
from scipy import ndimage as nd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'display'))
from svgart import shape, f2  # noqa: E402
import art_trace  # noqa: E402

ART = HERE / 'juni_chorus-art.png'
K = 5 / 32                       # the fit's scale
LIFT = 4                         # rows


def X(sx):
    return 40 + sx * K


def Y(sy):
    return sy * K - LIFT


def SX(x):
    return (x - 40) / K


def SY(y):
    return (y + LIFT) / K


LABELS = [(30, 95, 195, 145), (30, 145, 110, 230), (1310, 95, 1510, 150), (30, 865, 190, 915), (1300, 850, 1510, 925)]
PALETTE = ['#264096', '#372353', '#625D8B', '#B7A5F9', '#2E84F5', '#00E489', '#F54058', '#F5F5F5']
NAVY, PLUM, CLOUD, LAVENDER, MOON = '#264096', '#372353', '#625D8B', '#B7A5F9', '#2E84F5'
GREEN, RED, WHITE = '#00ED95', '#FF3A5D', '#FFFFFF'
FEET = (431, 576, 729, 896, 1051)       # art x of each trunk at its foot (row 647), to start the tracking
FOOT_ROW = 647
STATIC_TOL = 0.25                       # static traced lines: points dropped within this of the line (px)
RING_N = 24                             # the moon ring's pieces
CROWN_SPLIT = (525, 705, 895, 1055)     # art x between the crowns (white strokes go to the tree on their side)


def load(path):
    a = np.asarray(Image.open(path).convert('RGB')).copy()
    for x0, y0, x1, y1 in LABELS:
        a[y0:y1, x0:x1] = 0
    return a


def rdp(pts, eps):
    """Ramer-Douglas-Peucker: drop points within eps of the chord"""
    if len(pts) < 3:
        return list(pts)
    (ax, ay), (bx, by) = pts[0], pts[-1]
    L = math.hypot(bx - ax, by - ay) or 1e-9
    d = [abs((bx - ax) * (ay - y) - (ax - x) * (by - ay)) / L for x, y in pts[1:-1]]
    i = int(np.argmax(d)) + 1
    if d[i - 1] <= eps:
        return [pts[0], pts[-1]]
    return rdp(pts[:i + 1], eps)[:-1] + rdp(pts[i:], eps)


def rdp_closed(pts):
    """rdp for a closed polygon: split at the point farthest from the first, simplify both halves"""
    if len(pts) < 5:
        return list(pts)
    p0 = pts[0]
    i = max(range(len(pts)), key=lambda k: math.hypot(pts[k][0] - p0[0], pts[k][1] - p0[1]))
    a = rdp(list(pts[:i + 1]), STATIC_TOL)
    b = rdp(list(pts[i:]) + [p0], STATIC_TOL)
    return a[:-1] + b[:-1]


def clusters(xs, gap):
    out, s, p = [], None, None
    for x in xs:
        if s is None:
            s = p = x
        elif x > p + gap:
            out.append((s, p))
            s = x
        p = x
    if s is not None:
        out.append((s, p))
    return out


# ---------------------------------------------------------------- the traced static art
def traced(a):
    with tempfile.TemporaryDirectory() as d:
        big = Image.fromarray(a).resize((960, 640), Image.LANCZOS)
        fit = Image.new('RGB', (1280, 640))
        fit.paste(big, (160, -16))                      # 4 x the fit: x 160 + art_x * 0.625, y art_y * 0.625 - 16
        p = Path(d) / 'fit.png'
        fit.save(p)
        rep = art_trace.trace(str(p), PALETTE)
    g = a.astype(int)
    green = (g[..., 1] > 120) & (g[..., 1] > g[..., 0] + 60) & (g[..., 1] > g[..., 2] + 20)
    lab, n = nd.label(green, np.ones((3, 3)))
    cliff = np.zeros_like(green)
    leaves = []
    for i, s in enumerate(nd.find_objects(lab)):
        m = lab[s] == i + 1
        ext = max(s[0].stop - s[0].start, s[1].stop - s[1].start)
        cx = (s[1].start + s[1].stop) / 2
        if (ext > 40 or m.sum() > 300) and (cx < 345 or cx > 1235):
            cliff[s] |= m
        elif m.sum() >= 15:
            ys, xs = np.nonzero(m)
            leaves.append(np.stack([xs + s[1].start + 0.5, ys + s[0].start + 0.5], 1))
    near_cliff = nd.distance_transform_edt(~cliff) <= 4
    out = {NAVY: [], PLUM: [], GREEN: []}
    for color, width, piece in rep['strokes']:
        pts = piece[1] if piece[0] == 'poly' else None
        if piece[0] == 'poly' and len(piece[1]) > 2:   # fewer points to draw on entering the page: within 0.25 px
            piece = ('poly', rdp_closed(list(piece[1])) if piece[2] else rdp(list(piece[1]), STATIC_TOL), piece[2])
            pts = piece[1]
        if color in (NAVY, PLUM):
            out[color].append((width, piece))
        elif color == '#00E489' and pts:
            inside = [near_cliff[min(1023, max(0, int(SY(y)))), min(1535, max(0, int(SX(x))))] for x, y in pts]
            if sum(inside) >= 0.6 * len(inside):
                out[GREEN].append((width, piece))
    whites = [(width, piece) for color, width, piece in rep['strokes'] if color == '#F5F5F5' and piece[0] == 'poly']
    widths = {c['color']: c['width'] for c in rep['palette']}
    return out, whites, leaves, widths


def horizon(a):
    g = a.astype(int)
    blue = (g[..., 2] > 200) & (g[..., 0] < 100)
    rows = range(637, 643)
    segs = []
    for x0, x1 in clusters(np.nonzero(blue[637:643].any(0))[0], 3):
        if x1 - x0 > 20:
            segs.append(((X(x0 + 2.5), Y(639.75)), (X(x1 - 1.5), Y(639.75))))
    return segs


def hook(cx, cy, r, a0, a1, n=6):
    return [(cx + r * math.cos(a0 + (a1 - a0) * i / n), cy + r * math.sin(a0 + (a1 - a0) * i / n)) for i in range(n + 1)]


def clouds():
    """the two curled clouds, measured from row profiles (art px): runs at their centre rows, half-circle hooks"""
    up, dn = -math.pi / 2, math.pi / 2
    left = ([(120.5, 279.75), (297.5, 279.75)] + hook(297.5, 289.4, 9.65, up, dn)[1:] + [(104.5, 299.0)]
            + hook(104.5, 308.25, 9.25, up, -3 * math.pi / 2)[1:] + [(229.0, 317.5)])
    right = ([(1445.0, 320.5), (1289.5, 320.5)] + hook(1289.5, 328.75, 8.25, up, -3 * math.pi / 2)[1:]
             + [(1352.5, 337.0)] + hook(1352.5, 345.0, 8.0, up, dn)[1:] + [(1315.5, 353.0)]
             + hook(1315.5, 358.25, 5.25, up, -3 * math.pi / 2)[1:] + [(1467.0, 363.5)])
    return [[(X(x), Y(y)) for x, y in left], [(X(x), Y(y)) for x, y in right]]


def stars(a):
    g = a.astype(int)
    lav = np.abs(g - [0xB7, 0xA5, 0xF9]).sum(2) < 120
    lab, n = nd.label(lav, np.ones((3, 3)))
    plus = []
    for i, s in enumerate(nd.find_objects(lab)):
        if (lab[s] == i + 1).sum() > 20:
            cx, cy = (s[1].start + s[1].stop) / 2, (s[0].start + s[0].stop) / 2
            h = (s[1].stop - s[1].start) / 2 - 2.5          # arm, less the round end
            plus.append((X(cx), Y(cy), h * K))
    w = g.min(2) > 200
    lab, n = nd.label(w, np.ones((3, 3)))
    dots = []
    for i, s in enumerate(nd.find_objects(lab)):
        m = lab[s] == i + 1
        if m.sum() < 60 and s[0].start < 600 and (s[1].stop - s[1].start) <= 9:
            ys, xs = np.nonzero(m)
            dots.append((X(xs.mean() + s[1].start + 0.5), Y(ys.mean() + s[0].start + 0.5)))
    return plus, dots


def feet_marks(a):
    """the white marks under each trunk's foot (art rows 652..666): static, the foot doesn't move"""
    w = a.astype(int).min(2) > 150
    out = []
    lab, n = nd.label(w[652:667], np.ones((3, 3)))
    for i, s in enumerate(nd.find_objects(lab)):
        x0, x1 = s[1].start, s[1].stop
        yc = 652 + (s[0].start + s[0].stop) / 2
        if x1 - x0 > 6:
            out.append(((X(x0 + 2), Y(yc)), (X(x1 - 2), Y(yc))))
    return out


# ---------------------------------------------------------------- the moon
def moon(a):
    g = a.astype(int)
    blue = (g[..., 2] > 180) & (g[..., 0] < 120) & (g[..., 1] > 80)
    blue[500:] = False
    blue[:, :550] = False
    blue[:, 1000:] = False
    ys, xs = np.nonzero(blue)
    xs, ys = xs + 0.5, ys + 0.5

    def fit(px, py):
        A = np.stack([px, py, np.ones_like(px)], 1)
        b = px * px + py * py
        c = np.linalg.lstsq(A, b, rcond=None)[0]
        cx, cy = c[0] / 2, c[1] / 2
        return cx, cy, math.sqrt(c[2] + cx * cx + cy * cy)
    r = np.hypot(xs - 775, ys - 280)
    inner = (r > 120) & (r < 175)
    cx, cy, r0 = fit(xs[inner], ys[inner])
    r = np.hypot(xs - cx, ys - cy)
    outer = (r > r0 + 25) & (r < r0 + 80)
    ox, oy, r1 = fit(xs[outer], ys[outer])
    m = np.zeros_like(blue)
    m[ys[outer].astype(int), xs[outer].astype(int)] = True
    lab, n = nd.label(m, np.ones((3, 3)))
    dashes = []
    half = 2.5 / r1                                      # half the stroke, in radians, off each end
    for i in range(1, n + 1):
        yy, xx = np.nonzero(lab == i)
        if len(xx) < 20:
            continue
        ang = np.unwrap(np.sort(np.arctan2(yy + 0.5 - oy, xx + 0.5 - ox)))
        t = np.arctan2(yy + 0.5 - oy, xx + 0.5 - ox)
        c = math.atan2(np.sin(t).mean(), np.cos(t).mean())
        d = np.angle(np.exp(1j * (t - c)))
        dashes.append((c + d.min() + half, c + d.max() - half))
    dashes.sort()
    # which of the ring's 48 pieces the art shows (the rest is behind the crowns): blue ink within 3 px of the middle
    near = nd.distance_transform_edt(~blue) <= 3
    seen = 0
    for i in range(RING_N):
        t = (i + 0.5) * 2 * math.pi / RING_N
        if near[int(cy + r0 * math.sin(t)), int(cx + r0 * math.cos(t))]:
            seen |= 1 << i
    return (X(cx), Y(cy), r0 * K, seen), (X(ox), Y(oy), r1 * K), dashes


# ---------------------------------------------------------------- the birches
def trunks(a):
    white = nd.binary_closing(a.min(2) > 110, np.ones((11, 1)))
    out = []
    for c0 in FEET:
        c, rows, ws = float(c0), [], []
        for y in range(FOOT_ROW, 150, -1):
            lo = int(c) - 30
            xs = np.nonzero(white[y, lo:int(c) + 31])[0] + lo
            cl = clusters(xs, 16)
            if not cl:
                break
            l, r = min(cl, key=lambda t: abs((t[0] + t[1]) / 2 - c))
            w = r - l + 1
            if len(ws) > 20 and w > np.median(ws[-12:]) + 4:      # a branch leaves: the thick trunk ends here
                break
            if abs((l + r) / 2 - c) > 6 or w < 4:
                break
            rows.append((y, l, r))
            ws.append(w)
            c = (l + r) / 2
        out.append(rows)
    return out


def tree_model(a, whites, leaves):
    rows_all = trunks(a)
    m = a.astype(int).min(2)
    trees = []
    for rows in rows_all:
        ys = np.array([y for y, _, _ in rows], float) + 0.5
        cs = np.array([(l + r + 1) / 2 for _, l, r in rows], float)
        ws = np.array([r - l + 1 for _, l, r in rows], float) - 1.0      # the antialiased edge pixel
        pc = np.polyfit(ys, cs, 3)
        pw = np.polyfit(ys, ws, 1)
        top = ys.min()
        foot = FOOT_ROW + 1.0
        trunk = []
        n = max(2, int(round((foot - top) * K / 4)))
        for i in range(n + 1):
            yy = foot - (foot - top) * i / n
            trunk.append((X(np.polyval(pc, yy)), Y(yy), np.polyval(pw, yy) * K))
        # bark marks: dark components inside the silhouette rows
        sil = np.zeros(m.shape, bool)
        for y, l, r in rows:
            sil[y, l + 2:r - 1] = True
        dark = sil & (m < 90)
        lab, nl = nd.label(dark, np.ones((3, 3)))
        marks = []
        for i, s in enumerate(nd.find_objects(lab)):
            mm = lab[s] == i + 1
            if mm.sum() < 8 or s[0].stop - s[0].start > 7 or s[0].start < rows[-1][0] + 6:
                continue                                 # not a mark: the gap where a branch leaves
            yc = s[0].start + (s[0].stop - s[0].start) / 2
            x0, x1 = s[1].start + 1.0, s[1].stop - 1.0
            marks.append(((X(x0), Y(yc)), (X(x1), Y(yc)), max(0.5, (s[0].stop - s[0].start - 2) * K)))
        trees.append({'foot': (X(np.polyval(pc, foot)), Y(foot)), 'trunk': trunk, 'marks': marks, 'rows': rows,
                      'top_art': top, 'pc': pc, 'pw': pw, 'branches': [], 'leaves': []})
    # the white strokes outside the thick trunks: upper trunk and branches
    runs = []
    for width, piece in whites:
        pts = piece[1]
        if len(pts) < 2:
            continue
        sx = [SX(x) for x, _ in pts]
        sy = [SY(y) for _, y in pts]
        if np.mean(sy) > FOOT_ROW - 2:                    # the foot marks (static)
            continue
        if min(sy) > 600 and max(sy) < 640 and len(pts) < 3:
            continue
        def inside(x, y):
            return any(t['top_art'] - 1 <= y <= FOOT_ROW + 2 and abs(x - np.polyval(t['pc'], y)) <= np.polyval(t['pw'], y) / 2 + 1
                       for t in trees)
        run = []
        for (x, y), ax_, ay_ in zip(pts, sx, sy):     # keep the pieces outside the thick trunks
            if inside(ax_, ay_):
                if len(run) >= 2:
                    runs.append(run)
                run = []
            else:
                run.append((x, y))
        if len(run) >= 2:
            runs.append(run)
    for run in runs:
        mx = SX(np.mean([x for x, _ in run]))
        k = sum(1 for b in CROWN_SPLIT if mx > b)
        trees[k]['branches'].append(rdp(run, 0.3))
    # each trunk's leader: the piece with an end nearest the thick trunk's top goes on from it, its width the trunk's
    # taper carried on (at least a branch's)
    for t in trees:
        tx, ty, _ = t['trunk'][-1]
        best = None
        for i, b in enumerate(t['branches']):
            for rev in (False, True):
                e = b[-1] if rev else b[0]
                d = math.hypot(e[0] - tx, e[1] - ty)
                if (best is None or d < best[0]) and d < 8:
                    best = (d, i, rev)
        t['widths'] = [[0.95] * len(b) for b in t['branches']]
        if best:                                         # the leader joins the trunk: one smooth tapered line
            _, i, rev = best
            lead = t['branches'].pop(i)
            t['widths'].pop(i)
            lead = lead[::-1] if rev else lead
            from scipy.interpolate import UnivariateSpline
            ry = np.array([y for y, _, _ in t['rows']], float) + 0.5
            rx = np.array([(l + r + 1) / 2 for _, l, r in t['rows']], float)
            ly = np.array([SY(y) for _, y in lead])
            lx = np.array([SX(x) for x, _ in lead])
            keep = ly < ry.min() - 2
            yy = np.concatenate([ry, ly[keep]])
            xx = np.concatenate([rx, lx[keep]])
            o = np.argsort(yy)
            yy, xx = yy[o], xx[o]
            yy, ui = np.unique(yy, return_index=True)
            xx = xx[ui]
            sp = UnivariateSpline(yy, xx, s=len(yy) * 1.5)
            foot, jun, top = FOOT_ROW + 1.0, ry.min(), yy.min()
            wj = np.polyval(t['pw'], jun) * K
            n = max(2, int(round((foot - top) * K / 5)))
            trunk = []
            for j in range(n + 1):
                y = foot - (foot - top) * j / n
                w = np.polyval(t['pw'], y) * K if y >= jun else 0.95 + (wj - 0.95) * (y - top) / (jun - top)
                trunk.append((X(float(sp(y))), Y(y), max(0.95, w)))
            t['trunk'] = trunk
    # leaves: diamonds from the principal axes, to the tree of the nearest trunk or branch point
    tp = []
    for k, t in enumerate(trees):
        for x, y, _ in t['trunk']:
            tp.append((x, y, k))
        for b in t['branches']:
            for x, y in b:
                tp.append((x, y, k))
    tp = np.array(tp)
    for L in leaves:
        c = L.mean(0)
        u, sv, vt = np.linalg.svd(L - c, full_matrices=False)
        ax = vt[0]
        pr = (L - c) @ ax
        qr = (L - c) @ vt[1]
        half_len = max(4.0, (pr.max() - pr.min()) / 2 - 2.0)
        half_w = max(2.0, (qr.max() - qr.min()) / 2 - 2.0)
        cx, cy = X(c[0]), Y(c[1])
        a_ = ax * half_len * K
        b_ = vt[1] * half_w * K
        pts = [(a_[0], a_[1]), (b_[0], b_[1])]           # a diamond: the tips and the widest point
        d = np.hypot(tp[:, 0] - cx, tp[:, 1] - cy)
        k = int(tp[d.argmin(), 2])
        trees[k]['leaves'].append(((cx, cy), pts))
    for t in trees:                                      # the tree's height: its foot to its highest point
        ys = [y for _, y, _ in t['trunk']] + [y for b in t['branches'] for _, y in b] + [c[1] for c, _ in t['leaves']]
        t['height'] = t['foot'][1] - min(ys)
    return trees


# ---------------------------------------------------------------- the reflections
def reflections(a, trees):
    g = a.astype(int)
    red = (g[..., 0] > 150) & (g[..., 1] < 120) & (g[..., 2] < 140)
    red[:660] = False
    lab, n = nd.label(red, np.ones((3, 3)))
    out = []
    for i, s in enumerate(nd.find_objects(lab)):
        m = lab[s] == i + 1
        if m.sum() < 10:
            continue
        ys, xs = np.nonzero(m)
        yc = ys.mean() + s[0].start + 0.5
        x0, x1 = s[1].start + 2.5, s[1].stop - 2.5          # less the round end
        xc = (x0 + x1) / 2
        k = min(range(len(trees)), key=lambda j: abs(X(xc) - trees[j]['foot'][0]))
        col = k if abs(X(xc) - trees[k]['foot'][0]) < 50 * K and x1 - x0 < 60 else -1
        out.append((X(x0), X(x1), Y(yc), col))
    out.sort(key=lambda d: (d[3], d[2], d[0]))
    return out


# ---------------------------------------------------------------- output
def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else ART
    a = load(src)
    static, whites, leaves, widths = traced(a)
    trees = tree_model(a, whites, leaves)
    refl = reflections(a, trees)
    (mx, my, mr, seen), (ox, oy, orr), dashes = moon(a)
    plus, dots = stars(a)

    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="320" height="160" viewBox="0 0 320 160">']

    def emit(color, width, piece):
        if piece[0] == 'circle':
            _, cx, cy, r = piece
            out.append(f'<circle cx="{f2(cx)}" cy="{f2(cy)}" r="{f2(r)}" fill="none" stroke="{color}" stroke-width="{f2(width)}"/>')
        elif len(piece[1]) >= 2:
            out.append(shape(piece[1], color, width, piece[2]))
    for color in (PLUM, NAVY, GREEN):
        for width, piece in static[color]:
            emit(color, width, piece)
    for p, q in horizon(a):
        out.append(shape([p, q], MOON, widths.get(MOON, 1.0)))
    for c in clouds():
        out.append(shape(c, CLOUD, 0.8))
    for x, y, h in plus:
        out.append(shape([(x - h, y), (x + h, y)], LAVENDER, 0.6))
        out.append(shape([(x, y - h), (x, y + h)], LAVENDER, 0.6))
    for x, y in dots:
        out.append(shape([(x - 0.3, y), (x + 0.3, y)], WHITE, 1.0))
    for p, q in feet_marks(a):
        out.append(shape([p, q], WHITE, 0.8))
    out.append('</svg>')
    (HERE / 'juni_chorus.svg').write_text('\n'.join(out) + '\n')

    def q8(v):
        return int(round(v * 256))

    def q12(v):
        return int(round(v * 4096))
    h = ['/* Generated by gen_svg.py from the owner\'s mockup (the fit and every measurement are in its header).',
         ' * Coordinates Q8 screen units unless noted; weights Q12 = (height above the foot / tree height)^2.',
         ' * Original drawing; no firmware data. */',
         '#ifndef JUNI_CHORUS_GEOM_H', '#define JUNI_CHORUS_GEOM_H', '#include <stdint.h>',
         f'/* the ring\'s {RING_N} pieces (piece i from i/{RING_N} of a turn) the art shows: bit i; the rest is behind the crowns */',
         f'#define JC_RING_N {RING_N}', f'#define JC_MOON_SEEN 0x{seen:08X}u',
         f'#define JC_MOON_X {q8(mx)}', f'#define JC_MOON_Y {q8(my)}', f'#define JC_MOON_R {q8(mr)}',
         f'#define JC_HALO_X {q8(ox)}', f'#define JC_HALO_Y {q8(oy)}', f'#define JC_HALO_R {q8(orr)}',
         '/* halo dashes: start and end angle, Q16 of a turn (0 = +x, clockwise on screen) */',
         'static const uint16_t jc_dash[%d][2]={' % len(dashes) +
         ','.join('{%d,%d}' % (round((a0 / (2 * math.pi)) % 1 * 65536) & 0xFFFF, round((a1 / (2 * math.pi)) % 1 * 65536) & 0xFFFF)
                  for a0, a1 in dashes) + '};',
         '#define JC_TREES 5']
    # per tree: foot, height, trunk samples (x, y, width, weight), marks, branch points, leaves
    tr, tm, tb, tbs, tl = [], [], [], [], []
    starts = {'trunk': [], 'marks': [], 'br': [], 'brp': [], 'leaves': []}
    for t in trees:
        fx, fy = t['foot']
        H = t['height']

        def wgt(y):
            hh = max(0.0, fy - y) / H
            return q12(min(1.0, hh) ** 2)
        starts['trunk'].append(len(tr))
        for x, y, w in t['trunk']:
            tr.append((q8(x), q8(y), q8(w), wgt(y)))
        starts['marks'].append(len(tm))
        for (x0, y0), (x1, y1), w in t['marks']:
            tm.append((q8(x0), q8(y0), q8(x1), q8(w), wgt(y0)))
        starts['br'].append(len(tbs))
        for b, ws in zip(t['branches'], t['widths']):
            tbs.append(len(tb))
            for (x, y), w in zip(b, ws):
                tb.append((q8(x), q8(y), wgt(y), q8(w)))
        starts['leaves'].append(len(tl))
        for (cx, cy), pts in t['leaves']:
            tl.append((q8(cx), q8(cy), wgt(cy), [(q8(px), q8(py)) for px, py in pts]))
    for k in starts:
        starts[k].append({'trunk': len(tr), 'marks': len(tm), 'br': len(tbs), 'brp': len(tb), 'leaves': len(tl)}[k])
    tbs.append(len(tb))
    h += ['/* tree t: foot, height (Q8) */',
          'static const int32_t jc_foot[5][3]={' + ','.join('{%d,%d,%d}' % (q8(t['foot'][0]), q8(t['foot'][1]), q8(t['height'])) for t in trees) + '};',
          '/* trunk samples, foot first: x, y, width, weight; tree t uses [jc_trunk_at[t], jc_trunk_at[t + 1]) */',
          'static const int32_t jc_trunk[%d][4]={' % len(tr) + ','.join('{%d,%d,%d,%d}' % v for v in tr) + '};',
          'static const uint8_t jc_trunk_at[6]={' + ','.join(map(str, starts['trunk'])) + '};',
          '/* bark marks: x0, y, x1, stroke width, weight */',
          'static const int32_t jc_mark[%d][5]={' % len(tm) + ','.join('{%d,%d,%d,%d,%d}' % v for v in tm) + '};',
          'static const uint8_t jc_mark_at[6]={' + ','.join(map(str, starts['marks'])) + '};',
          '/* branches (and the upper trunks): polylines of x, y, weight, width; branch b is [jc_br_at[b], jc_br_at[b + 1]) */',
          'static const int32_t jc_brp[%d][4]={' % len(tb) + ','.join('{%d,%d,%d,%d}' % v for v in tb) + '};',
          'static const uint16_t jc_br_at[%d]={' % len(tbs) + ','.join(map(str, tbs)) + '};',
          'static const uint8_t jc_tree_br[6]={' + ','.join(map(str, starts['br'])) + '};',
          '/* leaves: centre x, y, weight, then the half axis and the half width as vectors (Q8): a diamond */',
          'static const int32_t jc_leaf[%d][7]={' % len(tl) + ','.join(
              '{%d,%d,%d,%s}' % (cx, cy, w, ','.join('%d,%d' % p for p in pts)) for cx, cy, w, pts in tl) + '};',
          'static const uint8_t jc_leaf_at[6]={' + ','.join(map(str, starts['leaves'])) + '};',
          '/* reflection dashes: x0, x1, y (Q8), tree (-1: a free dash on the water) */',
          f'#define JC_WATER_Y {q8(Y(FOOT_ROW + 1.0))}',
          'static const int32_t jc_refl[%d][4]={' % len(refl) + ','.join('{%d,%d,%d,%d}' % (q8(x0), q8(x1), q8(y), c) for x0, x1, y, c in refl) + '};',
          '#endif']
    (HERE / 'juni_chorus_geom.h').write_text('\n'.join(h) + '\n')
    lines = {'static elements': len(out) - 2, 'trees': [
        {'foot': [round(v, 2) for v in t['foot']], 'height': round(t['height'], 2), 'trunk': len(t['trunk']), 'marks': len(t['marks']),
         'branches': len(t['branches']), 'branch points': sum(len(b) for b in t['branches']), 'leaves': len(t['leaves'])} for t in trees],
        'reflections': len(refl), 'reflections in columns': sum(1 for d in refl if d[3] >= 0),
        'moon': [round(mx, 2), round(my, 2), round(mr, 2), bin(seen).count('1')], 'halo': [round(ox, 2), round(oy, 2), round(orr, 2)], 'dashes': len(dashes),
        'stars': len(plus), 'dots': len(dots), 'widths': widths}
    import json
    print(json.dumps(lines, indent=1))


if __name__ == '__main__':
    main()
