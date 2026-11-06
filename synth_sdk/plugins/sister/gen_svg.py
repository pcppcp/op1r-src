#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission.
"""SISTER's screen after the owner's chosen sketch (run/sister-2026-10-27/nun.png in the full project: a 1536 x 1024
image-generator mockup, no vector exists). Writes sister.svg (the static part, drawn through the static-prefix
cache: the floor grid and the ring, both cut where they run behind the nun, the candle bodies and wicks, the
quatrefoil, the nun's outline) and sister_geom.h (what sister_screen.c draws live: the columns, pointed arches and
lancets that follow SPACE, the petals, the flames, and the veil edge and ring radii it clips them against).

The fit (docs/screen-art.md; one uniform scale, chosen once):
    screen x = 40 + sketch_x * 5/32,  screen y = sketch_y * 5/32 - 9
  The sketch's height maps to the screen's 160 rows; its 3:2 width lands at x 40..280, centred. The art band
  (sketch y 76..929: arch tops to the floor's front) then sits at y 2.9..136.2, inside the OLED's lower target
  (y <= 138, strokes included); the 9-row lift is the only offset. A larger scale can't keep the floor, a smaller
  one only shrinks the art. The sketch's own labels (SPACE 62, MIX and its bar, SHIMMER, TONE) are blanked
  before tracing; the screen's labels are native text at the corner layout of the other FX screens (space with
  its value, mix with its bar, voice or `key c3`, swell), so they sit in the 40 px bands the 3:2 -> 2:1 fit
  leaves, not at the sketch's places.

Measured (screen units, on the fit at 4x, fit.png / trace.svg from synth_sdk/display/art_trace.py; the run folder
run/sister-2026-10-28/work in the full project):
  columns   four pairs; the near one's centre x 88.95, capital (abacus) top y 41.4, plinth on the floor y 115;
            the four sit at x 89 / 106.6 / 117.5 / 125.5 with capitals at 41.4 / 54.1 / 61.4 / 65.4: one
            perspective, vanishing point (160, 89.3), depth step 0.34 between pairs (fitted). SPACE moves the
            step (0.17 .. 0.64); the floor is the same plane, so plinths stay on it at any depth.
  arches    pointed transverse arches spring from each pair's capitals (levels 0..5; 0 and 1 rise off the top as
            in the sketch; 4 and 5 nest toward the rose and spring from the farthest drawn pair, level 3,
            as the sketch's inner vault lines do). Apex y 4.3 / 13.6 / 20.6 / 26.2 at levels 2..5: the
            apexes recede toward their own point, y = APEX_V - APEX_A * s (fitted below). The profile is the two
            clearest traced arches, normalised and averaged.
  lancets   the side arcade: a pointed lancet on each wall between neighbouring pairs, apex 45 % of a step in
            front of the farther column at height 85 above the horizon (traced tips (100.6, 16.8),
            (112.6, 38.6), (120.9, 46.6)).
  rose      centre (160, 52), white ring r 22.3, twelve green petal lobes (stem from r 8, round head centre r 16.3,
            radius 3.6), a white quatrefoil (four lobes r 3.1 at 3.65 from the centre). The petal straight down
            hides behind the veil and isn't drawn.
  nun       traced outline (left half, mirrored): veil top y 63.9, face frame, band at y 74.3, chin 90.8,
            necklace V to y 104.4, cross, shoulders flowing out to the hem at y 130.6 (x 131..189).
  candles   bodies x 53.3..59.1 / 63.8..69.2 / 74..78.5, tops 100.5 / 107.5 / 111.1, foot 120.6; teardrop
            flames 7 / 6.7 / 5.6 tall, 4 / 3.85 / 3 wide; teal wicks on the first two.
  floor     transverse lines at the traced rows, longitudinal lines toward the vanishing point at 17.1, 31.1,
            45.1 and 59 units either side of the axis (the traced crossings meet at y 87..90); its edges run along the column line to y 117.2, the front rows reach x 83..237 (the
            sketch's left side; its right side reaches 242).
Colours: the trace's teal #0B9A9E (architecture, floor, nun), the encoder green #00ED95 for the petals (the
trace's #01F7A7 is 21 away), white ring, quatrefoil and candles.
Forced changes, written down: the sketch's perspective isn't one camera (the floor lines converge a little
higher than the columns, the arch apexes recede toward their own point); the model keeps the columns and floor on
one vanishing point and the apexes on a fitted one, so every part can move with SPACE. Sub-pixel details (the
three ribs on the near capital, the hub circle behind the petals, the far half of the floor tiles' wobble) are
left out. No firmware data.
"""
from pathlib import Path
import math
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'display'))
from svgart import shape  # noqa: E402

TEAL = '#0B9A9E'
WHITE = '#FFFFFF'

VP = (160.0, 89.3)          # columns, capitals, floor
FLOOR_Y = 25.7              # floor below the horizon at s = 1 (the near plinths' foot, y 115)
D0 = 0.34                   # depth step between column pairs at the default SPACE (60)
D_MIN, D_MAX = 0.17, 0.642  # SPACE 0 and 99: d = D_MIN + (D0 - D_MIN) / 0.36 * u^2

# the near (level 0) left column at s = 1, absolute screen coordinates; 'near' parts only on levels 0 and 1
COLUMN = [
    ('all', [(84, 41.4), (94, 41.4), (94, 43.6), (84, 43.6), (84, 41.4)]),       # abacus
    ('near', [(85.1, 43.6), (85.1, 45.8), (93, 45.8), (93, 43.6)]),              # necking
    ('near', [(86, 45.8), (86.8, 49.2), (91.4, 49.2), (92, 45.8)]),              # bell
    ('all', [(87.1, 49.2), (87.1, 97.5)]), ('all', [(90.8, 49.2), (90.8, 97.5)]),  # shaft
    ('all', [(85.6, 100), (85.6, 97.5), (92.4, 97.5), (92.4, 100)]),            # base
    ('near', [(85.2, 100), (92.8, 100)]),                                        # plinth top
    ('all', [(85.2, 100), (85.2, 115)]), ('all', [(92.8, 100), (92.8, 115)]),   # plinth
]
ARCH_X = 70.0               # springing: x from the axis at s = 1 (just inside the column centre 71.05)
CAP_Y = 41.4 - VP[1]        # springing height (the abacus top), from the horizon, at s = 1
APEX_TRACED = {2: 4.3, 3: 13.6, 4: 20.6, 5: 26.2}
# the arch profile: (u, v) from the springing (0, 0) to the apex (1, 1); the traced arches with apex y 4.3 and 13.6,
# normalised, averaged and resampled
PROFILE = [(0, 0), (0.06, 0.34), (0.13, 0.50), (0.24, 0.63), (0.40, 0.76), (0.60, 0.88), (1, 1)]
# (the traced (0.02, 0.15) at the springing is left out: it moves the curve 0.33 px at most, and saves a line per arch)
LANCET_X = 71.05            # the wall plane (column centres)
LANCET_APEX_Y = -85.0       # apex height above the horizon at s = 1
LANCET_APEX_U = -0.45       # apex depth, in steps from the farther column
LANCET_LEGS = [(0, 0), (0.06, 0.34), (0.24, 0.63), (0.60, 0.88), (1, 1)]   # PROFILE thinned for a 1-step lancet

ROSE = (160.0, 52.0)
RING_R = 22.3
PETAL_IN, PETAL_C, PETAL_R, PETAL_STEM = 8.0, 16.3, 3.6, 1.0
QUAT_D, QUAT_R = 3.65, 3.1

# the nun, left half from the top centre, as traced (screen units)
VEIL = [(160, 63.9), (157.6, 64.2), (156.1, 64.8), (154, 66.2), (151.7, 68.9), (150.4, 72), (149.7, 74.8),
        (148.9, 78.4), (147.8, 83.3), (147.1, 86.1), (145.8, 90), (144.4, 94.1), (143.2, 97.7), (141.7, 100.4),
        (140.3, 102.2), (139.2, 104.6), (138.4, 107.5), (137.6, 111.2), (136.6, 115), (135.4, 118.8),
        (133.6, 124.1), (132.1, 127.3), (131.1, 130.6)]
FRAME = [(160, 68.1), (157.9, 68.4), (156.5, 69), (154.8, 70.3), (153.5, 72.1), (152.7, 74), (152.2, 76),
         (152.1, 80.4), (152.2, 84.8), (151.6, 91.1), (151.1, 93.1), (150.4, 95.1), (148.9, 96.2), (147.4, 96.9),
         (145.3, 98), (143.1, 98.6)]          # face frame down to the shoulder line
FOLD = [(146.9, 97.4), (146.2, 100.4), (145.5, 104), (145, 107.9), (143.9, 118.1), (143.2, 125.3), (142.9, 130.6)]
BAND = [(152.4, 75.4), (154.8, 75), (157.2, 74.5), (160, 74.3)]
CHIN = [(154.4, 88.1), (156.1, 89.3), (158, 90.4), (160, 90.8)]
NECK = [(152.4, 84.1), (154.4, 87.9), (154.3, 90.6), (153.2, 93.4), (153.2, 95.4), (154.2, 97.6), (156.5, 101.3),
        (158.7, 103.7), (160, 104.4)]         # face side to the necklace V
CROSS = [[(160, 105.6), (160, 115)], [(157.1, 108.6), (162.9, 108.6)]]
HEM_Y = 130.6

CANDLES = [  # x0, x1, top, flame base x, base y, height, width, wick
    (53.3, 59.1, 100.5, 56.2, 98.6, 7.0, 4.0, True),
    (63.8, 69.2, 107.5, 66.5, 105.6, 6.7, 3.85, True),
    (74.0, 78.5, 111.1, 76.15, 109.6, 5.6, 3.0, False),
]
CANDLE_FOOT = 120.6

FLOOR_ROWS = [101.9, 103.75, 106.25, 108.75, 111.25, 113.75, 116.9, 120.6, 123.1, 126.25, 130.6]
FLOOR_X = [0, 17.1, 31.1, 45.1, 59.0]   # longitudinal lines, across the floor at s = 1 (traced crossings)
FLOOR_FRONT = 77.0          # the edges follow the column line X = 71.05 until this far from the axis, then the
                            # front rows reach this far (the sketch's left side, clear of the candles)
FLOOR_EDGE_Y = 89.3 + 25.7 * FLOOR_FRONT / 71.05
FLOOR_FAR_S = 0.33          # the longitudinal lines start here (behind the farthest column at SPACE 99)
FLOOR_BOTTOM = 136.0


def s_level(i, d=D0):
    return 1.0 / (1.0 + i * d)


def mirror(p):
    return [(320 - x, y) for x, y in p]


def apex_fit():
    """APEX_V, APEX_A: least squares apex_y = V - A s over the traced levels"""
    pts = [(s_level(i), y) for i, y in APEX_TRACED.items()]
    n = len(pts)
    sx = sum(s for s, _ in pts)
    sy = sum(y for _, y in pts)
    sxx = sum(s * s for s, _ in pts)
    sxy = sum(s * y for s, y in pts)
    slope = (n * sxy - sx * sy) / (n * sxx - sx * sx)
    return (sy - slope * sx) / n, -slope


APEX_V, APEX_A = apex_fit()


def floor():
    out = []
    edge_s = (FLOOR_EDGE_Y - VP[1]) / FLOOR_Y

    def half(y):                                    # the floor's half width at row y
        s = (y - VP[1]) / FLOOR_Y
        return LANCET_X * s if y <= FLOOR_EDGE_Y + 0.01 else FLOOR_FRONT
    for y in FLOOR_ROWS:                            # transverse rows
        h = half(y)
        out.append([(VP[0] - h, y), (VP[0] + h, y)])
    y_far = VP[1] + FLOOR_Y * FLOOR_FAR_S
    for X in FLOOR_X:                               # longitudinal lines toward the vanishing point
        for sign in ((1,) if X == 0 else (-1, 1)):
            y_end = FLOOR_BOTTOM
            if X * (FLOOR_BOTTOM - VP[1]) / FLOOR_Y > FLOOR_FRONT:    # leaves the front at its side
                y_end = VP[1] + FLOOR_Y * FLOOR_FRONT / X
            a = (VP[0] + sign * X * FLOOR_FAR_S, y_far)
            b = (VP[0] + sign * X * (y_end - VP[1]) / FLOOR_Y, y_end)
            out.append([a, b])
    for sign in (-1, 1):                            # the edges along the columns
        out.append([(VP[0] + sign * LANCET_X * FLOOR_FAR_S, y_far), (VP[0] + sign * LANCET_X * edge_s, FLOOR_EDGE_Y)])
    return out


def candles():
    out = []
    for x0, x1, top, fx, fy, *_rest, wick in CANDLES:
        out.append(([(x0, CANDLE_FOOT), (x0, top), (x1, top), (x1, CANDLE_FOOT)], WHITE))
        if wick:
            out.append(([(fx, top), (fx, fy)], TEAL))
    return out


def nun_outline():
    """polylines (full, mirrored) in drawing order"""
    left = [VEIL, FRAME, FOLD, BAND, CHIN, NECK]
    lines = []
    for p in left:
        lines.append(p)
        lines.append(mirror(p))
    lines += CROSS
    lines.append([(VEIL[-1][0], HEM_Y), (320 - VEIL[-1][0], HEM_Y)])
    return lines


def veil_half(y):
    """the nun's half width at row y (0 above the veil)"""
    for (x0, y0), (x1, y1) in zip(VEIL, VEIL[1:]):
        if y0 <= y <= y1 and y1 > y0:
            return 160 - (x0 + (x1 - x0) * (y - y0) / (y1 - y0))
    return 0.0


def behind_nun(x, y):
    """inside the nun's silhouette (the outline's centre line): what her black fill used to hide"""
    return VEIL[0][1] <= y <= HEM_Y and abs(x - 160) < veil_half(y)


def outside_nun(a, b):
    """the parts of segment a-b in front of the nun's silhouette (the static art is clipped here, so the SVG needs no
    black fill: the outline is drawn over the cut ends, as it was drawn over the fill)"""
    n = max(1, int(math.hypot(b[0] - a[0], b[1] - a[1]) / 0.05))
    at = lambda t: (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
    out, start = [], None
    for k in range(n + 1):
        t = k / n
        vis = not behind_nun(*at(t))
        if vis and start is None:
            start = t
        if (not vis or k == n) and start is not None:
            end = t if vis else (k - 1) / n
            if end > start:
                out.append((at(start), at(end)))
            start = None
    return out


def ring():
    """the ring's pieces in front of the veil, as runs of points"""
    n = 40
    pts = [(ROSE[0] + RING_R * math.sin(t), ROSE[1] - RING_R * math.cos(t)) for t in (i * 2 * math.pi / n for i in range(n + 1))]
    runs, cur = [], []
    for i in range(n):
        for a, b in outside_nun(pts[i], pts[i + 1]):
            if cur and math.dist(cur[-1], a) < 1e-6:
                cur.append(b)
            else:
                if cur:
                    runs.append(cur)
                cur = [a, b]
    if cur:
        runs.append(cur)
    if len(runs) > 1 and math.dist(runs[-1][-1], runs[0][0]) < 1e-6:          # the run across the top joins up
        runs[0] = runs.pop() + runs[0][1:]
    return runs


def quatrefoil():
    """closed outline: four arcs, each round its lobe between the outer crossings with its neighbours"""
    # outer crossing of two lobes, on the diagonal: rho^2 - sqrt(2) D rho + D^2 - R^2 = 0
    rho = (math.sqrt(2) * QUAT_D + math.sqrt(2 * QUAT_D ** 2 - 4 * (QUAT_D ** 2 - QUAT_R ** 2))) / 2
    pts = []
    for k in range(4):
        c = k * math.pi / 2                                         # lobe direction: up, right, down, left
        cx, cy = QUAT_D * math.sin(c), -QUAT_D * math.cos(c)
        n0 = (rho * math.sin(c - math.pi / 4), -rho * math.cos(c - math.pi / 4))
        n1 = (rho * math.sin(c + math.pi / 4), -rho * math.cos(c + math.pi / 4))
        a0 = math.atan2(n0[0] - cx, -(n0[1] - cy))                  # angle round the lobe centre, 0 = up, clockwise
        a1 = math.atan2(n1[0] - cx, -(n1[1] - cy))
        while a1 < a0:
            a1 += 2 * math.pi
        for j in range(6):
            a = a0 + (a1 - a0) * j / 6
            pts.append((ROSE[0] + cx + QUAT_R * math.sin(a), ROSE[1] + cy - QUAT_R * math.cos(a)))
    return pts


def flame(n=5):
    """a teardrop, base at (0, 0), height 1, half width 0.5 at its widest; one side, base to tip"""
    pts = []
    for i in range(n):
        t = i / (n - 1)
        a = t * math.pi
        y = -(1 - math.cos(a)) / 2
        w = math.sin(a) * (1 - t) ** 0.6 * 0.66
        pts.append((w, y))
    return pts


def main():
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="320" height="160" viewBox="0 0 320 160">']
    for seg in floor():
        for piece in outside_nun(*seg):                 # the floor runs behind the nun
            out.append(shape(piece, TEAL, 1))
    for seg, col in candles():
        out.append(shape(seg, col, 1))
    rg = ring()
    for run in rg:                                      # the rose's ring and quatrefoil, the nun's outline: all still
        out.append(shape(run, WHITE, 1))
    qf = quatrefoil()
    out.append(shape(qf, WHITE, 1, closed=True))
    for poly in nun_outline():
        out.append(shape(poly, TEAL, 1))
    out.append('</svg>')
    (HERE / 'sister.svg').write_text('\n'.join(out) + '\n')

    def q8(v):
        return round(v * 256)

    def q12(v):
        return round(v * 4096)

    def arr(name, p, typ='sister_xy'):
        return f'static const {typ} {name}[{len(p)}]={{' + ','.join('{%d,%d}' % (q8(x), q8(y)) for x, y in p) + '};'

    # columns: segments relative to the vanishing point at s = 1, near-only flag
    col = []
    for kind, poly in COLUMN:
        for a, b in zip(poly, poly[1:]):
            col.append((a[0] - VP[0], a[1] - VP[1], b[0] - VP[0], b[1] - VP[1], kind == 'near'))
    col.sort(key=lambda c: c[4])                   # 'all' first: far levels draw a prefix
    n_far = sum(1 for c in col if not c[4])

    fl = flame()
    arc = [(math.cos(a), math.sin(a)) for a in (math.radians(-120 + 60 * j) for j in range(5))]   # 60 degree steps

    h = ['/* Generated by gen_svg.py from the owner\'s sketch (the fit and every measurement are in its header).',
         ' * Coordinates Q8 screen units unless noted. Original drawing; no firmware data. */',
         '#ifndef SISTER_GEOM_H', '#define SISTER_GEOM_H', '#include <stdint.h>',
         'typedef struct { int32_t x, y; } sister_xy;',
         f'#define SISTER_VPX {q8(VP[0])}', f'#define SISTER_VPY {q8(VP[1])}',
         '/* depth step between column pairs, Q12: D_MIN + D_K * u^2 (u = SPACE, Q15) */',
         f'#define SISTER_D_MIN {q12(D_MIN)}', f'#define SISTER_D_K {q12((D0 - D_MIN) / 0.36)}',
         '/* columns: segments from the vanishing point at s = 1 (left side); the first SISTER_COL_FAR on every level */',
         f'#define SISTER_COL_FAR {n_far}',
         'static const int16_t sister_col[%d][4]={' % len(col) +
         ','.join('{%d,%d,%d,%d}' % (q8(a), q8(b), q8(c), q8(d)) for a, b, c, d, _ in col) + '};',
         '/* transverse arches: springing (from the vanishing point, s = 1), apex y = V - A s, profile (u, v) Q12 */',
         f'#define SISTER_ARCH_X {q8(ARCH_X)}', f'#define SISTER_CAP_Y {q8(CAP_Y)}',
         f'#define SISTER_APEX_V {q8(APEX_V)}', f'#define SISTER_APEX_A {q8(APEX_A)}',
         'static const int16_t sister_profile[%d][2]={' % len(PROFILE) +
         ','.join('{%d,%d}' % (q12(u), q12(v)) for u, v in PROFILE) + '};',
         '/* lancets: wall x, apex height (from the horizon, s = 1), apex depth (steps, Q12), legs profile Q12 */',
         f'#define SISTER_WALL_X {q8(LANCET_X)}', f'#define SISTER_LANCET_Y {q8(LANCET_APEX_Y)}',
         f'#define SISTER_LANCET_U {q12(LANCET_APEX_U)}',
         'static const int16_t sister_legs[%d][2]={' % len(LANCET_LEGS) +
         ','.join('{%d,%d}' % (q12(u), q12(v)) for u, v in LANCET_LEGS) + '};',
         '/* the rose window */',
         f'#define SISTER_ROSE_X {q8(ROSE[0])}', f'#define SISTER_ROSE_Y {q8(ROSE[1])}',
         '/* the live parts stay clear of the still art: the nave outside the ring\'s outer edge (where the black disc',
         '   hid it), the petals inside its inner edge (where the ring was drawn over them), both clear of the nun */',
         f'#define SISTER_NAVE_R {q8(RING_R + 0.5)}', f'#define SISTER_PETAL_MAX_R {q8(RING_R - 0.5)}',
         f'#define SISTER_PETAL_IN {q8(PETAL_IN)}', f'#define SISTER_PETAL_C {q8(PETAL_C)}',
         f'#define SISTER_PETAL_R {q8(PETAL_R)}', f'#define SISTER_PETAL_STEM {q8(PETAL_STEM)}',
         '/* the petal head: unit arc from -120 to +120 degrees in 60 degree steps (cos radial, sin tangential), Q12 */',
         'static const int16_t sister_arc[%d][2]={' % len(arc) + ','.join('{%d,%d}' % (q12(c), q12(s)) for c, s in arc) + '};',
         'static const int16_t sister_petal_dir[12][2]={' + ','.join('{%d,%d}' % (round(16384 * math.sin(t)), round(-16384 * math.cos(t)))
                                                             for t in (i * 2 * math.pi / 12 for i in range(12))) + '};',
         '/* the nun\'s silhouette for the live parts: the veil\'s left edge (top to hem), mirrored about x 160; the',
         '   outline (in the SVG) is 1 px wide, so the live parts stop half a pixel outside it */',
         arr('sister_veil', VEIL), f'#define SISTER_HEM_Y {q8(HEM_Y)}',
         '/* the candles: flame base, rest height and width (Q8); flame side template (x half width, y height; Q8) */',
         'static const int16_t sister_candle[3][4]={' +
         ','.join('{%d,%d,%d,%d}' % (q8(c[3]), q8(c[4]), q8(c[5]), q8(c[6])) for c in CANDLES) + '};',
         arr('sister_flame', fl),
         '#endif']
    (HERE / 'sister_geom.h').write_text('\n'.join(h) + '\n')
    print(f'apex fit: y = {APEX_V:.2f} - {APEX_A:.2f} s; column segments {len(col)} ({n_far} on far levels); '
          f'SVG elements {len(out) - 2}')


if __name__ == '__main__':
    main()
