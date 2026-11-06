#!/usr/bin/env python3
"""Generate juni.svg + juni_geom.h: JUNI's screen, a midsummer night on a fjord, traced from the owner's mockup.

    python3 gen_svg.py [--frame-png OUT.png]

The art is juni-art.png: the owner's pick, mockup A (2026-10-27), a 1536 x 1024 image-model PNG. No vector file
exists, so it is traced with display/art_trace.py (docs/screen-art.md, "From a PNG").

The fit (one uniform scale, then a sideways spread; 2026-10-28, the owner: fill the screen's width, as TAFFY and
WAVETABLE). The mockup is 3:2 and its drawing sits in a band about 1460 x 700 px (x 39..1498, y 143..849 with the
strokes' width). The height limits the scale: the stars' top must stay at y >= 3 and the shore's bottom inside the
OLED's bottom margin (screen-art.md: bottom ink <= 138, strokes and animation included). So the frame is the 2:1
window X -67..1603, Y 126..961 of the mockup (1670 x 835 px, black beyond the PNG's edges), scale 320 / 1670 = 0.192
(K = 1.065 times the first fit's 0.180, frame X -121..1657, Y 83..972): uniform, the drawing lands on x 20.6..299.7,
y 3.5..137.8. Live sizes in px scale with K (the sun's travel, the ridges' waveform height, period and fade; the
sparks' top clip is a mockup y), so the moving parts keep their proportions and the ridges their tooth counts.
The spread fills the width (place()): the middle (the middle cliff, the ranges behind it, the sun and its horizon)
stays at the uniform fit. The left group (shore, stones, grass, trees, left mountain and its ridge, the bonfire and
its sparks, the left cloud and star) and the right group (the right cliffs and their ridges, the navy range behind
them with its inner lines, the right cloud, stars and dots) each move out sideways, whole, until their outermost ink
is on x 2.2 / 317.8 (18.4 px left, 18.1 px right). The green waterline in front of the middle cliff runs on left to
where the fire cuts it, the right one back to the blue horizon's end. The reflection's dashes outside the column under
the sun move out on a straight ramp, as far as their worst-case slide and stretch allow at the right edge (lengths and
rows unchanged). main() refuses the art when the still drawing or any moving part at its extremes (bounds()) leaves
x 2..318, y 3..138.
The left mountain's ridge is now its real outline (up to the peak, down the far side): at the first fit the piece
search's 7 px tolerance took an inner line instead, with a stray connector line back to the outline.

Colours: traced with the mockup's measured colours, then each one written as its nearest stock / encoder colour
(the owner's palette rule): blue #3D8BF8 -> #698EFF, green #00D781 -> #00ED95, red #F63555 -> #FF3A5D,
white -> #FFFFFF, the far purple range #2D2247 -> #1D2040, the navy range #1E357C -> #383572, rocks and clouds
#5F5B82 -> #656579, the stars #A796F2 -> #DED7FF. Stroke widths as measured (green 1.38 px etc.).

What is traced 1:1 into juni.svg (still): the mountain ranges behind (navy, far purple), the rocky shore, the fire's
stones and the clouds (grey), the pine trees, grass tufts, the cliffs' inner lines and the green waterline, and the
blue horizon under the sun. Dropped: the tracer's lavender pieces (only the grey lines' antialiased edges) and
specks under about 1.5 px. The stars are under three stroke widths, so they are measured in the PNG (centre, size)
and drawn as small crosses and dots.

What juni.c draws (moving, geometry from the same trace in juni_geom.h):
  blue  TONE    the sun: the disc, nine rays and the two broken halo arcs are fitted (circle centre and radius,
                ray angles and lengths, arc angles) and drawn about the sun's centre, which TONE moves: the mockup's
                place at the default TONE, wholly under the water at 0 (sun_down()), as high as the halo allows at
                the top (sun_up()); dim to bright, and the halo grows with it
  green SHAPE   the three cliffs' ridge lines (left mountain, middle cliff, right cliffs) are resampled every
                1.5 px with their outward normals; juni.c pushes each point out by the DCO's waveform (saw, pulse
                with the PWM, sub an octave down), 0 .. 5.9 px, a saw cycle every 12.8 px, fading out at the ridge
                ends. The ridge reads as the mockup's outline with the waveform as its teeth
  page 2        RESONANCE widens, whitens and grows the halo, ringing near the top; HPF thins and dims the green
                waterlines (taken out of the SVG for it, waterlines()); PWM RATE drives the PWM LFO the teeth follow
  white SWEEP   the bonfire: the traced flame strokes (white, red inside) scaled about the fire's base, the mockup's
                size at the default SWEEP, smaller below and taller above, flaring with the voices' envelope;
                each tongue flickers and sways, the sparks rise
  red   CHORUS  the reflection: the traced dashes, each shifted and stretched by the chorus LFOs (rows in
                opposite phase), and by the voices' envelope, so it moves whenever notes sound
--frame-png writes the framed mockup (the PNG the tracer sees) for art_compare (the uniform fit, before the spread:
compare the middle with it).
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

ART = HERE / 'juni-art.png'
FX0, FY0, FW = -67, 126, 1670                   # the uniform frame in mockup px (2:1, so 835 tall)
S = 320 / FW
K = S / (320 / 1778)                            # this fit against the first one (2026-10-27, frame -121, 83, 1778)
LEFT_X, RIGHT_X = 380, 1090                     # mockup x: the side groups (see side())
WATER_Y = 615                                   # mockup y: below this, the shore (the left group, x < 800)
DASH_BLOCK = (800, 1150)                        # mockup x: the reflection's column under the sun, never moved
EDGE = (2.2, 317.8)                             # screen x: where the side groups' outermost ink goes
INK_BOX = (2.0, 318.0, 3.0, 138.0)              # nothing drawn outside this (x0, x1, y0, y1), live parts included
SPARK_CLIP_Y = 155.2                            # mockup y: sparks above this aren't drawn (screen y 13 in the first fit)
MEASURED = ['#2D2247', '#1E357C', '#5F5B82', '#F63555', '#3D8BF8', '#00D781', '#F6F6F6']
STOCK = {'#2D2247': '#1D2040', '#1E357C': '#383572', '#5F5B82': '#656579', '#A796F2': '#DED7FF',
         '#F63555': '#FF3A5D', '#3D8BF8': '#698EFF', '#00D781': '#00ED95', '#F6F6F6': '#FFFFFF'}
BLUE, GREEN, WHITE, RED, LAV = '#3D8BF8', '#00D781', '#F6F6F6', '#F63555', '#A796F2'


def sc(X, Y):
    """mockup px -> screen px"""
    return ((X - FX0) * S, (Y - FY0) * S)


# stars, measured in the PNG (blob centres, mockup px): crosses (half arm), dots, a small white sparkle
CROSSES = [(184.8, 162.9, 16), (667.6, 244.0, 16), (1357.0, 307.6, 16)]
SPARKLES = [(558.1, 327.5, 9)]
DOTS = [(1149.2, 183.5), (456.9, 217.9), (1461.3, 246.8)]
# the moving parts, picked by where their traced strokes start or end (mockup px), not by index
RIDGES = [  # (outward direction, pieces in order)
    ((0, -1), [(44, 394), (142, 341), (194, 372)]),                 # left mountain: up to the peak and down
    ((1, -1), [(366, 423), (496, 525)]),                            # middle cliff and its shelf to the water
    ((-1, -1), [(1493, 355), (1425, 433), (1352, 549)]),            # right cliff, the high face
    ((-1, -1), [(1357, 481), (1271, 558)]),                         # right cliff, the low face
]
FIRE = (200, 330, 420, 745)                     # the bonfire's box (mockup px): white and red strokes in it move
FIRE_BASE = (310, 730)                          # its base, the pivot of the flames
STONES = (170, 690, 440, 790)                   # the fire's stones: their dark shading is drawn grey
SPARK_ABOVE = 540                               # white pieces wholly above this y (mockup) and short are sparks
RIDGE_STEP = 1.5                                # ridge resampling, screen px
RIDGE_PERIOD = 12.0 * K                         # screen px per saw cycle along a ridge
RIDGE_FADE = 8.0 * K                            # screen px over which the waveform fades in / out at the ends
RIDGE_AMP = 5.5 * K                             # screen px, the waveform's full height (juni.c); x 1.2 at most with
RIDGE_BREATH = 1.2                              # the PWM LFO's breathing (juni.c draw_ridges())
# the sun (juni.c draw_sun()): at TONE 0 the disc is wholly under the horizon; at the top it rises as far as the halo
# at its largest allows (top ink >= SUN_TOP). The halo: x 0.75 at TONE 0, 1.0 at the default, 1.04 at the top, then
# x 1.06 at RESONANCE's top and a ringing wobble of 0.015 above 3/4 of it; its stroke up to 1.6 x as wide
HALO_MAX = 1.04 * 1.06 + 0.015
HALO_WMAX = 1.6
SUN_TOP = 3.6


def framed():
    """the mockup in the frame: the PNG the tracer sees (black outside the mockup)"""
    im = Image.open(ART).convert('RGB')
    out = Image.new('RGB', (FW, FW // 2), (0, 0, 0))
    out.paste(im, (-FX0, -FY0))
    return out


def trace():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'framed.png')
        framed().save(p)
        r = art_trace.trace(p, MEASURED)
    widths = {p['color']: p['width'] for p in r['palette']}
    return [(c, w, piece[1]) for c, w, piece in r['strokes'] if piece[0] == 'poly'], widths


def bbox(pts):
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)


def plen(pts):
    return sum(math.dist(a, b) for a, b in zip(pts, pts[1:]))


def cross(u, v):
    return u[0] * v[1] - u[1] * v[0]


def rdp(pts, eps):
    """Douglas-Peucker, screen px"""
    if len(pts) < 3:
        return list(pts)
    a, b = np.array(pts[0]), np.array(pts[-1])
    ab = b - a
    n = np.hypot(*ab)
    d = [abs(cross(ab, np.array(p) - a)) / n if n else math.dist(p, pts[0]) for p in pts[1:-1]]
    i = int(np.argmax(d)) + 1
    if d[i - 1] <= eps:
        return [pts[0], pts[-1]]
    return rdp(pts[:i + 1], eps)[:-1] + rdp(pts[i:], eps)


def near(pts, key, tol=7.0):
    """-> 0 when the stroke starts at key, 1 when it ends there, else None (screen px)"""
    k = sc(*key)
    d0, d1 = math.dist(pts[0], k), math.dist(pts[-1], k)
    if min(d0, d1) > tol:
        return None
    return 0 if d0 <= d1 else 1


def inbox(pts, box):
    (x0, y0), (x1, y1) = sc(box[0], box[1]), sc(box[2], box[3])
    a = bbox(pts)
    return x0 <= a[0] and a[2] <= x1 and y0 <= a[1] and a[3] <= y1


def split(strokes, widths_grey):
    """-> parts: static [(colour, width, pts)], sun, ridges, fire, sparks, dashes"""
    fx0, fy0 = sc(FIRE[0], FIRE[1])
    fx1, fy1 = sc(FIRE[2], FIRE[3])
    used, ridges = set(), []
    greens = [i for i, s in enumerate(strokes) if s[0] == GREEN]
    for direction, keys in RIDGES:
        chain = []
        for key in keys:
            hits = [(i, near(strokes[i][2], key)) for i in greens if i not in used]
            hits = [(i, e) for i, e in hits if e is not None]
            if not hits:
                raise SystemExit(f'ridge piece at {key} not found in the trace')
            i, e = min(hits, key=lambda h: -plen(strokes[h[0]][2]))   # the longest stroke there
            used.add(i)
            pts = list(strokes[i][2])
            chain += pts if e == 0 else pts[::-1]
        ridges.append((direction, chain))
    static, blue, fire, sparks, dashes = [], [], [], [], []
    for i, (c, w, pts) in enumerate(strokes):
        if i in used:
            continue
        x0, y0, x1, y1 = bbox(pts)
        tiny = max(x1 - x0, y1 - y0) < 1.5
        infire = fx0 <= x0 and x1 <= fx1 and fy0 <= y0 and y1 <= fy1
        if any(abs((x0 + x1) / 2 - sc(X, Y)[0]) < 3.5 and abs((y0 + y1) / 2 - sc(X, Y)[1]) < 3.5
               for X, Y, *_ in CROSSES + SPARKLES + DOTS):
            continue                            # a star: drawn below as a primitive
        if c == BLUE:
            if y1 - y0 < 1.0 and x1 - x0 > 40:
                static.append((c, w, pts))      # the horizon under the sun
            elif not tiny:
                blue.append(pts)
        elif c == WHITE:
            if not infire:
                continue                        # the white star dots: drawn below
            if y1 < sc(0, SPARK_ABOVE)[1] and y1 - y0 < 50 * S:
                sparks.append(pts)
            elif not tiny:
                fire.append((c, pts))
        elif c == RED:
            if infire:
                fire.append((c, pts))
            elif not tiny:
                dashes.append(((x0 + x1) / 2, (x1 - x0) / 2, sum(p[1] for p in pts) / len(pts)))
        elif not tiny:
            if c == '#2D2247' and inbox(pts, STONES):
                c, w = '#5F5B82', widths_grey
            static.append((c, w, pts))
    return static, blue, ridges, fire, sparks, dashes


def circle_fit(pts):
    P = np.array(pts)
    A = np.c_[2 * P, np.ones(len(P))]
    b = (P ** 2).sum(1)
    cx, cy, k = np.linalg.lstsq(A, b, rcond=None)[0]
    return cx, cy, math.sqrt(k + cx * cx + cy * cy)


def angle(cx, cy, p):
    """degrees, 0 = up, clockwise (juni.c's arc())"""
    return math.degrees(math.atan2(p[0] - cx, cy - p[1]))


def sun(blue):
    """-> centre, disc radius, the disc's cut (y), rays [(deg, r0, r1)], arcs [(r, a0, a1)] (screen px)"""
    disc = min(blue, key=lambda p: abs(circle_fit(p)[2] - 87.5 * S) if len(p) > 8 else 1e9)
    cx, cy, rd = circle_fit(disc)
    rays, arcs = [], []
    for p in blue:
        if p is disc:
            continue
        a, b = p[0], p[-1]
        ra, rb = math.dist(a, (cx, cy)), math.dist(b, (cx, cy))
        if abs(angle(cx, cy, a) - angle(cx, cy, b)) < 6:                 # radial: a ray
            inner, outer = (a, b) if ra < rb else (b, a)
            rays.append((angle(cx, cy, outer), min(ra, rb), max(ra, rb)))
        else:
            r = float(np.mean([math.dist(q, (cx, cy)) for q in p]))
            a0, a1 = sorted((angle(cx, cy, a), angle(cx, cy, b)))
            arcs.append((r, a0, a1))
    rays.sort()
    arcs.sort()
    return (cx, cy), rd, max(disc[0][1], disc[-1][1]), rays, arcs


def resample(chain, step):
    P = np.array(chain, float)
    seg = np.hypot(*np.diff(P, axis=0).T)
    s = np.r_[0, np.cumsum(seg)]
    n = max(2, int(round(s[-1] / step)) + 1)
    t = np.linspace(0, s[-1], n)
    return np.c_[np.interp(t, s, P[:, 0]), np.interp(t, s, P[:, 1])], s[-1]


def ridge_points(direction, chain):
    """-> [(x, y, nx, ny, phase16, win8)]: the half-rate phase (bit 15 = the sub) and the end fade"""
    P, total = resample(chain, RIDGE_STEP)
    cycles = max(2, round(total / RIDGE_PERIOD))
    out = []
    for i, (x, y) in enumerate(P):
        a, b = P[max(i - 2, 0)], P[min(i + 2, len(P) - 1)]
        tx, ty = (b - a) / max(np.hypot(*(b - a)), 1e-9)
        nx, ny = -ty, tx
        if nx * direction[0] + ny * direction[1] < 0:
            nx, ny = -nx, -ny
        s = total * i / (len(P) - 1)
        ph = int(round(s / total * cycles * 32768)) % 65536            # half rate: two saw cycles per sub cycle
        win = min(1.0, s / RIDGE_FADE, (total - s) / RIDGE_FADE)
        out.append((x, y, nx, ny, ph, int(round(255 * max(0.0, win)))))
    return out


def mk(x, y):
    """screen px (uniform fit) -> mockup px"""
    return (x / S + FX0, y / S + FY0)


def side(c, pts):
    """which group a still stroke moves with: 'L' (the left shore, trees, left mountain, fire, left cloud and star),
    'R' (the right cliffs, the navy range behind them with its inner lines, the right cloud, stars) or 'C' (the middle
    cliff and the ranges behind it, the sun's horizon: never moved). By the stroke's mockup box"""
    (X0, Y0), (X1, Y1) = mk(*bbox(pts)[:2]), mk(*bbox(pts)[2:])
    cx = (X0 + X1) / 2
    if Y0 > WATER_Y:
        return 'L' if cx < 800 else 'R'
    if cx < LEFT_X:
        return 'L'
    if c != BLUE and X0 >= RIGHT_X:
        return 'R'
    return 'C'


def star_side(X):
    return 'L' if X < LEFT_X else 'R' if X >= RIGHT_X else 'C'


def star_ink(dx):
    """the stars' ink (screen px) per side, with the move dx {'L', 'R', 'C'}: [(side, x0, x1, y0, y1)]"""
    out = []
    for X, Y, h in CROSSES + SPARKLES:
        (x, y), r = sc(X, Y), h * S
        x += dx[star_side(X)]
        out.append((star_side(X), x - r - 0.5, x + r + 0.5, y - r - 0.5, y + r + 0.5))
    for X, Y in DOTS:
        x, y = sc(X, Y)
        x += dx[star_side(X)]
        out.append((star_side(X), x - 1.0, x + 1.0, y - 0.6, y + 0.6))
    return out


def ink(pieces):
    """[(colour, width, pts)] -> x0, x1, y0, y1 (strokes included)"""
    xs = [q[0] - w / 2 for _, w, pts in pieces for q in pts] + [q[0] + w / 2 for _, w, pts in pieces for q in pts]
    ys = [q[1] - w / 2 for _, w, pts in pieces for q in pts] + [q[1] + w / 2 for _, w, pts in pieces for q in pts]
    return min(xs), max(xs), min(ys), max(ys)


def place(static, ridges, fire, sparks, dashes, widths):
    """the spread (2026-10-28, the owner: fill the screen's width). The uniform fit is limited by the height (the
    stars' top, the shore's bottom), so the two side groups move out sideways, each whole, until their outermost ink is
    on EDGE: the left by dL (shore, stones, trees, grass, left mountain and its ridge, the fire and its sparks, the left
    cloud and star), the right by dR (right cliffs and their ridges, the navy range behind them, right cloud, stars).
    The middle (the middle cliff, the ranges behind it, the sun, its horizon) stays. The green waterline in front of
    the middle cliff runs on left to where it was cut by the fire (by dL), the right one back to the horizon's end.
    The reflection's dashes move out on a ramp (dash_delta()). Sizes are all the uniform fit's.
    -> static, ridges, fire, sparks, dashes, dx {'L', 'R', 'C'} (screen px)"""
    sides = [side(c, pts) for c, w, pts in static]
    stars = star_ink({'L': 0, 'R': 0, 'C': 0})
    lx = min(min(ink([st for st, sd in zip(static, sides) if sd == 'L'])[0], min(a[1] for a in stars if a[0] == 'L')),
             min(q[0] - widths[GREEN] / 2 for q in ridges[0][1]))
    rx = max(max(ink([st for st, sd in zip(static, sides) if sd == 'R'])[1], max(a[2] for a in stars if a[0] == 'R')),
             max(q[0] + widths[GREEN] / 2 for d, ch in ridges[2:] for q in ch))
    dx = {'L': EDGE[0] - lx, 'R': EDGE[1] - rx, 'C': 0.0}
    out = []
    for (c, w, pts), sd in zip(static, sides):
        q = [(x + dx[sd], y) for x, y in pts]
        x0, y0, x1, y1 = bbox(pts)
        if c == GREEN and y1 - y0 < 1.5 and x1 - x0 > 40 * S:          # a waterline
            j = min(range(len(q)), key=lambda i: q[i][0])
            if sd == 'C' and mk(x0, 0)[0] < 500:
                q[j] = (q[j][0] + dx['L'], q[j][1])                     # in front of the middle cliff: on to the fire
            elif sd == 'R':
                q[j] = (q[j][0] - dx['R'], q[j][1])                     # the right one: back to the horizon's end
        out.append((c, w, q))
    ridges = [(d, [(x + dx[sd], y) for x, y in ch]) for (d, ch), sd in zip(ridges, 'LCRR')]
    fire = [(c, [(x + dx['L'], y) for x, y in p]) for c, p in fire]
    sparks = [[(x + dx['L'], y) for x, y in p] for p in sparks]
    return out, ridges, fire, sparks, dash_place(dashes, dx['L'], widths), dx


# the reflection's motion in juni.c draw_water(), its worst case: a slide of 8 px (slow chorus LFO) + 2 px (fast) +
# 6 px (the voices' envelope); the slow and the fast share one depth (8 (1 - u) + 2 u <= 8), so 14 px; a stretch of
# 1 + 460 / 1024 of the half length
DASH_SLIDE, DASH_STRETCH = 14.0, 1 + 460 / 1024


def dash_place(dashes, dl, widths):
    """the dashes outside the column under the sun move out, each by its outer edge on a straight ramp: 0 at the
    column, dl at the leftmost dash (as far as the shore moved), on the right as far as the motion allows at the
    screen's edge. Lengths and rows unchanged. dashes [(cx, half, y)] -> moved"""
    b0, b1 = sc(DASH_BLOCK[0], 0)[0], sc(DASH_BLOCK[1], 0)[0]
    l = min(cx - h for cx, h, _ in dashes)
    r = max(cx + h for cx, h, _ in dashes)
    wr = widths[RED] / 2
    # the right edge's move: the farthest any dash reaches (its slide and stretch) lands on INK_BOX's right
    dr = min((INK_BOX[1] - 0.3 - (cx + h * DASH_STRETCH + DASH_SLIDE + wr)) / ((cx + h - b1) / (r - b1))
             for cx, h, _ in dashes if cx + h > b1)
    out = []
    for cx, h, y in dashes:
        if cx + h < b0 and cx < (b0 + b1) / 2:
            d = dl * (b0 - (cx - h)) / (b0 - l)
        elif cx - h > b1 or (cx + h > b1 and cx > (b0 + b1) / 2):
            d = dr * ((cx + h) - b1) / (r - b1)
        else:
            d = 0.0
        out.append((cx + d, h, y))
    return out


def bounds(static, dx, centre, rd, rays, arcs, ridges, fire, sparks, dashes, widths):
    """the worst-case extents (screen px, strokes included) of the still drawing and of every moving part at its
    extremes, from juni.c's maps; main() refuses the art outside INK_BOX. -> {name: (x0, x1, y0, y1)}"""
    b = {'still': ink(static)}
    st = star_ink(dx)
    b['stars'] = (min(a[1] for a in st), max(a[2] for a in st), min(a[3] for a in st), max(a[4] for a in st))
    wb = widths[BLUE] / 2
    rr = max(max(r1 * 1.18 for _, _, r1 in rays), max(r for r, _, _ in arcs), rd)
    up = sun_up(centre, rd, rays, arcs, widths)
    rh = max(max(r1 * 1.18 for _, _, r1 in rays), max(r for r, _, _ in arcs) * HALO_MAX, rd)
    b['sun'] = (centre[0] - rh - wb * HALO_WMAX, centre[0] + rh + wb * HALO_WMAX,
                centre[1] - up - rh - wb * HALO_WMAX, centre[1] + wb)            # clipped at the horizon (above the centre)
    wg = widths[GREEN] / 2                      # ridge points (ridge_points()) at rest and pushed out in full
    pts = [(x + a * nx * w / 255, y + a * ny * w / 255) for ch in ridges for x, y, nx, ny, _, w in ch
           for a in (0, RIDGE_AMP * RIDGE_BREATH)]
    b['ridges'] = (min(q[0] for q in pts) - wg, max(q[0] for q in pts) + wg, min(q[1] for q in pts) - wg,
                   max(q[1] for q in pts) + wg)
    bx, by = fire_base(dx)
    hs = 1.3 * 1.18 * (1 + 3 * 64 / 1024 / 2)    # SWEEP top, the flare, the flicker (two terms, 64 / 1024 at most)
    ws = 0.5 + hs / 2
    fp = [q for _, p in fire for q in p]
    ww = widths[WHITE] / 2
    b['fire'] = (bx + min(min(x - bx for x, _ in fp) * ws, 0) - 1.6 - ww, bx + max(x - bx for x, _ in fp) * ws + 1.6 + ww,
                 by + min(y - by for _, y in fp) * hs - ww, by + max(max(y - by for _, y in fp) * hs, 0) + ww)
    sp = [q for p in sparks for q in p]
    b['sparks'] = (bx + min(x - bx for x, _ in sp) * ws - 1.5 - ww, bx + max(x - bx for x, _ in sp) * ws + 1.5 + ww,
                   sc(0, SPARK_CLIP_Y)[1] - ww, by + max(y - by for _, y in sp) * 0.55 + 10 + ww)
    wr = widths[RED] / 2
    b['dashes'] = (min(cx - h * DASH_STRETCH - DASH_SLIDE for cx, h, _ in dashes) - wr,
                   max(cx + h * DASH_STRETCH + DASH_SLIDE for cx, h, _ in dashes) + wr,
                   min(y for _, _, y in dashes) - wr, max(y for _, _, y in dashes) + wr)
    return b


def sun_up(centre, rd, rays, arcs, widths):
    """the sun's rise at TONE's top (screen px): the halo at its largest stays at y >= SUN_TOP"""
    rh = max(max(r1 * 1.18 for _, _, r1 in rays), max(r for r, _, _ in arcs) * HALO_MAX, rd)
    return math.floor((centre[1] - rh - widths[BLUE] / 2 * HALO_WMAX - SUN_TOP) * 4) / 4


def sun_down(centre, rd, clip, widths):
    """the sun's sinking at TONE 0 (screen px): the disc's top (stroke included) under the horizon's clip line"""
    return clip + rd + widths[BLUE] / 2 + 0.5 - centre[1]


def waterlines(static):
    """the green waterlines (the cliffs' and the shore's feet, mockup y about 604): drawn live by juni.c, thinned by
    HPF. -> still strokes without them, [pts]"""
    keep, wl = [], []
    for c, w, pts in static:
        x0, y0, x1, y1 = bbox(pts)
        if c == GREEN and y1 - y0 < 1.5 and x1 - x0 > 4 and 598 < mk(0, (y0 + y1) / 2)[1] < 610:
            wl.append(pts)
        else:
            keep.append((c, w, pts))
    return keep, wl


def fire_base(dx):
    bx, by = sc(*FIRE_BASE)
    return bx + dx['L'], by


def svg(static, dx):
    o = ['<?xml version="1.0" encoding="utf-8"?>',
         '<svg version="1.1" id="Layer_1" xmlns="http://www.w3.org/2000/svg" x="0px" y="0px"',
         '\t width="320px" height="160px" viewBox="0 0 320 160">',
         '<!-- generated by gen_svg.py from juni-art.png (traced): the still parts; sun, ridges, fire and reflection by code -->']
    for c, w, pts in static:
        o.append(shape(rdp(pts, 0.12), STOCK[c], w))
    lw = 1.0
    for X, Y, h in CROSSES + SPARKLES:
        (x, y), r = sc(X, Y), h * S
        x += dx[star_side(X)]
        col = STOCK[LAV] if h > 10 else '#FFFFFF'
        o.append(shape([(x - r, y), (x + r, y)], col, lw))
        o.append(shape([(x, y - r), (x, y + r)], col, lw))
    for X, Y in DOTS:
        x, y = sc(X, Y)
        x += dx[star_side(X)]
        o.append(shape([(x - 0.4, y), (x + 0.4, y)], '#FFFFFF', 1.2))
    o.append('</svg>')
    return '\n'.join(o) + '\n'


def q4(v):
    return int(round(v * 16))


def arr(ctype, name, vals, per=16):
    rows = [', '.join(str(v) for v in vals[i:i + per]) for i in range(0, len(vals), per)]
    return f'static const {ctype} {name}[{len(vals)}] = {{\n    ' + ',\n    '.join(rows) + '\n};'


def header(widths, centre, rd, clip, rays, arcs, ridges, fire, sparks, dashes, dx, wl):
    bx, by = fire_base(dx)
    h = ['/* generated by gen_svg.py from juni-art.png: the moving parts of JUNI\'s screen. Coordinates Q4 screen px '
         'unless named Q10. */',
         f'#define JS_W_BLUE {int(widths[BLUE] * 1024)}', f'#define JS_W_GREEN {int(widths[GREEN] * 1024)}',
         f'#define JS_W_WHITE {int(widths[WHITE] * 1024)}', f'#define JS_W_RED {int(widths[RED] * 1024)}',
         '/* the sun: centre at the default TONE, its clip line (the disc\'s cut above the horizon), disc radius */',
         f'#define JS_SUN_X {int(round(centre[0] * 1024))}', f'#define JS_SUN_Y {int(round(centre[1] * 1024))}',
         f'#define JS_SUN_CLIP {int(round(clip * 1024))}', f'#define JS_SUN_R {int(round(rd * 1024))}',
         f'#define JS_SUN_DOWN {int(round(sun_down(centre, rd, clip, widths) * 1024))}   /* Q10 px: the sinking at TONE 0 */',
         f'#define JS_SUN_UP {int(round(sun_up(centre, rd, rays, arcs, widths) * 1024))}   /* Q10 px: the rise at TONE max */',
         f'#define JS_NRAYS {len(rays)}', f'#define JS_NARCS {len(arcs)}',
         '/* rays: angle (0.1 degree, 0 = up, clockwise), inner and outer radius (Q10) */',
         arr('int16_t', 'JS_RAY_A', [int(round(a * 10)) for a, _, _ in rays]),
         arr('int32_t', 'JS_RAY_R0', [int(round(r0 * 1024)) for _, r0, _ in rays]),
         arr('int32_t', 'JS_RAY_R1', [int(round(r1 * 1024)) for _, _, r1 in rays]),
         '/* halo arcs: radius (Q10), from / to angle (0.1 degree) */',
         arr('int32_t', 'JS_ARC_R', [int(round(r * 1024)) for r, _, _ in arcs]),
         arr('int16_t', 'JS_ARC_A0', [int(round(a0 * 10)) for _, a0, _ in arcs]),
         arr('int16_t', 'JS_ARC_A1', [int(round(a1 * 10)) for _, _, a1 in arcs])]
    pts = [p for ch in ridges for p in ch]
    starts = [0]
    for ch in ridges:
        starts.append(starts[-1] + len(ch))
    h += ['/* ridges: points, outward normal (Q7), half-rate waveform phase (bit 15 = the sub), end fade (Q8) */',
          f'#define JS_NRIDGES {len(ridges)}', f'#define JS_RIDGE_AMP {int(RIDGE_AMP * 1024)}   /* Q10 px */',
          arr('uint16_t', 'JS_RIDGE_START', starts),
          arr('int16_t', 'JS_RX', [q4(p[0]) for p in pts]), arr('int16_t', 'JS_RY', [q4(p[1]) for p in pts]),
          arr('int8_t', 'JS_RNX', [int(round(p[2] * 127)) for p in pts]),
          arr('int8_t', 'JS_RNY', [int(round(p[3] * 127)) for p in pts]),
          arr('uint16_t', 'JS_RPH', [p[4] for p in pts]), arr('uint8_t', 'JS_RWIN', [p[5] for p in pts])]
    # fire: strokes relative to the base, simplified within 0.25 px
    fpts, fstart, fred = [], [0], []
    for c, p in fire:
        q = rdp(p, 0.25)
        fpts += [(x - bx, y - by) for x, y in q]
        fstart.append(len(fpts))
        fred.append(1 if c == RED else 0)
    top = -min(y for _, y in fpts)
    h += ['/* the bonfire: base (Q10), strokes relative to it (Q4), the flames\' height (Q4), which are red */',
          f'#define JS_FIRE_X {int(round(bx * 1024))}', f'#define JS_FIRE_Y {int(round(by * 1024))}',
          f'#define JS_FIRE_H {q4(top)}', f'#define JS_NFIRE {len(fire)}',
          arr('uint16_t', 'JS_FIRE_START', fstart), arr('uint8_t', 'JS_FIRE_RED', fred),
          arr('int16_t', 'JS_FX', [q4(x) for x, _ in fpts]), arr('int16_t', 'JS_FY', [q4(y) for _, y in fpts])]
    sp = [(p[0], p[-1]) for p in sparks]
    h += ['/* sparks: one short line each, relative to the fire\'s base (Q4); none drawn above JS_SPARK_TOP (Q10) */',
          f'#define JS_NSPARKS {len(sp)}', f'#define JS_SPARK_TOP {int(round(sc(0, SPARK_CLIP_Y)[1] * 1024))}',
          arr('int16_t', 'JS_SX0', [q4(a[0] - bx) for a, _ in sp]), arr('int16_t', 'JS_SY0', [q4(a[1] - by) for a, _ in sp]),
          arr('int16_t', 'JS_SX1', [q4(b[0] - bx) for _, b in sp]), arr('int16_t', 'JS_SY1', [q4(b[1] - by) for _, b in sp])]
    dashes.sort(key=lambda d: (d[2], d[0]))
    rows, last = [], None
    for d in dashes:
        if last is None or d[2] - last > 1.2:
            rows.append(0)
        rows[-1] += 1
        last = d[2]
    row_of = [i for i, n in enumerate(rows) for _ in range(n)]
    h += ['/* the reflection: dash centre, half length, y (Q4), row (rows count down the water) */',
          f'#define JS_NDASH {len(dashes)}', f'#define JS_DASH_ROWS {len(rows)}',
          arr('int16_t', 'JS_DX', [q4(d[0]) for d in dashes]), arr('int16_t', 'JS_DH', [q4(d[1]) for d in dashes]),
          arr('int16_t', 'JS_DY', [q4(d[2]) for d in dashes]), arr('uint8_t', 'JS_DROW', row_of)]
    wp = [q for p in wl for q in rdp(p, 0.12)]
    ws = [0]
    for p in wl:
        ws.append(ws[-1] + len(rdp(p, 0.12)))
    h += ['/* the green waterlines (still in the art, drawn here so HPF can thin them): points (Q4) */',
          f'#define JS_NWL {len(wl)}', arr('uint16_t', 'JS_WL_START', ws),
          arr('int16_t', 'JS_WLX', [q4(x) for x, _ in wp]), arr('int16_t', 'JS_WLY', [q4(y) for _, y in wp])]
    return '\n'.join(h) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--frame-png', type=Path)
    a = ap.parse_args()
    if a.frame_png:
        framed().save(a.frame_png)
    strokes, widths = trace()
    static, blue, ridges, fire, sparks, dashes = split(strokes, widths['#5F5B82'])
    centre, rd, clip, rays, arcs = sun(blue)
    ux = ink(static)
    static, ridges, fire, sparks, dashes, dx = place(static, ridges, fire, sparks, dashes, widths)
    ridges = [ridge_points(d, ch) for d, ch in ridges]
    still, wl = waterlines(static)
    b = bounds(static, dx, centre, rd, rays, arcs, ridges, fire, sparks, dashes, widths)
    x0, x1, y0, y1 = INK_BOX
    bad = {k: v for k, v in b.items() if not (x0 <= v[0] and v[1] <= x1 and y0 <= v[2] and v[3] <= y1)}
    if bad:
        raise SystemExit('out of bounds: ' + ', '.join(f'{k} x {v[0]:.1f}..{v[1]:.1f} y {v[2]:.1f}..{v[3]:.1f}'
                                                         for k, v in bad.items()))
    (HERE / 'juni.svg').write_text(svg(still, dx))
    (HERE / 'juni_geom.h').write_text(header(widths, centre, rd, clip, rays, arcs, ridges, fire, sparks, dashes, dx,
                                             wl))
    print(f'uniform still ink x {ux[0]:.1f}..{ux[1]:.1f}; moved left {dx["L"]:.1f}, right {dx["R"]:.1f} px; extents ' +
          ', '.join(f'{k} x {v[0]:.1f}..{v[1]:.1f} y {v[2]:.1f}..{v[3]:.1f}' for k, v in b.items()))
    print(f'sun: down {sun_down(centre, rd, clip, widths):.1f} px at TONE 0, up {sun_up(centre, rd, rays, arcs, widths):.1f} '
          f'px at the top; {len(wl)} waterlines live')
    print(f'ok: {len(still)} still strokes, sun r {rd:.1f} at {centre[0]:.1f},{centre[1]:.1f}, {len(rays)} rays, '
          f'{len(arcs)} arcs, ridges {[len(r) for r in ridges]} points, {len(fire)} flame strokes, {len(sparks)} sparks, '
          f'{len(dashes)} dashes, widths {widths}')


if __name__ == '__main__':
    main()
