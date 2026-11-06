#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""TAFFY's screen after the owner's chosen mockup A, "bubblegum" (OP1-Emulator
run/screen-mockups-2026-10-28/tf_A_bubblegum.png: a 1536 x 1024 image-generator mockup, no vector exists).
Writes taffy.svg (the static part, drawn through the static-prefix cache: the girl, clouds, stars, dots, lips) and
taffy_geom.h (what screen.c draws live: the bubble, its red highlights, the green gum strands, the white sparkles).

    python3 gen_svg.py --trace MOCKUP.png    trace the mockup's static line art again into taffy_art.json
    python3 gen_svg.py                       taffy_art.json + the measurements below -> taffy.svg, taffy_geom.h

The fit (docs/screen-art.md; one uniform scale, chosen once). The trace and every measurement below are in the
trace frame, x = (mockup_x + 106) * 20/111, y = (mockup_y - 77) * 20/111 (the first fit, 2026-10-28: it fitted the
whole 3:2 canvas, so the mockup's empty border became empty bands, art only in x 27.7..292.4, y 15.5..136.4). The
screen is that frame scaled once more, around the mockup's ink rather than its canvas:
    screen x = trace x * 1.107 - 17.32,  screen y = trace y * 1.107 - 13.28
  i.e. screen = (mockup - (-19.17, 143.58)) * 0.19946. The ink band (mockup x 48..1517, y 163..834, strokes included)
  is as tall as it can be: centre lines y 4.65..137.21, ink 4.0..137.9 (the OLED's lower target y <= 138); its
  width is centred (ink x 13.4..306.4). The girl, the lips, the strands, the bubble and every live size scale with
  it (taffy_geom.h: TF_RSIL .. TF_RIPPLE), so their placement is the mockup's.
  The one sideways move: the 2:1 screen is wider than the band at this height, so the outer decorations (the stars,
  clouds and white dots left of trace x 100 and right of 240) move out by the same amount on each side until their
  ink spans x 2..318, as the mockup's art spans its own canvas (11.5 px each). The star and dots between the girl and
  the bubble stay. Strokes keep their screen widths (1.3 px lavender, 1.25 lips, 1.5 bubble); the lips' gap is 1.4
  screen px. Douglas-Peucker thinning (0.25 px) runs after the fit.
Traced (synth_sdk/display/art_trace.py, --colors lavender and white, on the mockup with the moving parts blanked:
everything between x 662 and 1325 except the star there, the green strands; the face's lighter profile
shading counts as lavender, only the lips are white):
  girl      hair, face, ear, eye and lashes, neck and shoulders: lavender #9A98E1 as measured, 1.05 px measured,
            drawn 1.3 px (forced: thinner lines don't show evenly on the OLED). Pieces under 2.2 px are dropped
            (specks of the mockup's shading; the brief: no hair detail thinner than the OLED can show), except the
            eyelashes. Each line is thinned to the points within 0.25 px of it (Douglas-Peucker), to keep
            the screen's entry cost down.
  stars     the same trace, recoloured to their measured purple #A899FC (the mockup draws them brighter than the
            hair); clouds to their measured #888AC1.
  dots      four white dots, measured centres, r 0.6.
  lips      all white (owner, 2026-10-28): two small arcs (upper and lower lip) at their measured place (the trace of
            a 3 px pucker is a blob), the traced pucker line inside them drawn white too; the lavender profile
            line ends 1.4 px (centre lines) before the lips and resumes after them.
Measured for the live parts (trace frame units; taffy_geom.h has them at the screen fit; see screen.c):
  bubble    centre (211.2, 75.6), mean radius 45.8, its outline's strongest harmonic the 6th (3.06 px, peaks at 29
            degrees + k 60): six cycles of the live wave around the circle, the first starting at 29 degrees. Stroke 1.4 px measured, drawn 1.5 in the encoder blue #698EFF.
  strands   from the lips' notch (134.4, 84.3) to the outline at 190.6 (top), 165.2 (middle, starting 35 % out) and
            149.8 degrees (bottom); two branches leave the outer strands 80 / 86 % along for 180.5 and 157.9
            degrees. Quadratic curves with the bows measured on the trace. Encoder green #00ED95, 1.3 px.
  highlight red arcs at 0.80 of the outline radius: -71..-30 degrees, a dash at -23..-17, a dot at -80; 21..55 with
            a white glint at 28.5..34, a dash at 60..65. Encoder red #FF3A5D, 1.45 px.
  sparkles  white radial dashes 7..17 px outside the mean radius at -56, -46, 51 and 60 degrees; four by the lips
            (measured ends). 1.3 px.
Forced changes, written down: the bubble's outline is the live wave, so its six lobes follow SHAPE and DISTORTION
rather than the mockup's hand-drawn ones (at the defaults they come close: lobe depth 6 % of the radius, the
mockup's 6.7 %); the colours of the four knob-driven parts are the encoder colours (the mockup's bubble blue is
#3287F8, its red #F93E5B); the white dots and lips are primitives. No firmware data.
"""
from pathlib import Path
import json
import math
import sys

HERE = Path(__file__).resolve().parent
SDK = HERE.parents[1]

S = 20 / 111                 # the trace frame: taffy_art.json and every measurement below
X0, Y0 = -106, 77
K = 1.107                    # the screen fit: screen = trace frame x K + (FX, FY), one uniform scale
FX, FY = -17.32, -13.28
EDGE = (2.0, 318.0)          # the outer stars, clouds and dots move sideways until their ink reaches these columns
OUTER = (100.0, 240.0)       # ... the ones left of the first / right of the second (trace frame centres)
LAV, WHITE = '#9A98E1', '#FFFFFF'   # stars and clouds: measured in --trace (taffy_art.json)
LAV_W = 1.3

# measured (see the header); trace frame units
BUBBLE = (211.2, 75.6)
BUBBLE_R = 45.8
LOBE0 = 29.0                 # the mockup's first lobe peak (its outline's 6th harmonic), degrees
MOUTH = (134.4, 84.3)
DOTS = [(44.1, 56.2), (140.2, 27.2), (268.8, 73.7), (154.8, 119.1)]
LIPS = [((134.6, 81.0), 2.3, 200, 450), ((134.6, 87.6), 2.3, -90, 160)]   # centre, radius, from..to degrees
# strands: start (None = the mouth, else (parent, t) on a strand), end angle (deg), start along the own line, bow:
# the control point's offset toward the top as a share of the strand's width (quadratic curves; measured on the
# trace: the top strand sags 4 px under its chord, the bottom one bulges 4.4 px over it, the others nearly straight)
STRANDS = [(None, 190.6, 0.0, -0.257), (None, 165.2, 0.35, -0.068), (None, 149.8, 0.0, 0.286),
           ((0, 0.80), 180.5, 0.0, -0.06), ((2, 0.86), 157.9, 0.0, 0.18)]
# highlights: centre angle, half span (deg), colour (0 red, 1 white), resonance threshold (Q15 fraction)
HIGHLIGHTS = [(-50.5, 20.5, 0, 0.0), (43.0, 12.0, 0, 0.02), (25.0, 4.0, 0, 0.05), (31.3, 2.7, 1, 0.07),
              (-20.0, 3.2, 0, 0.09), (62.5, 2.5, 0, 0.12), (-80.0, 1.2, 1, 0.15),
              (-98.0, 6.0, 0, 0.45), (84.0, 5.0, 0, 0.62)]   # all the mockup's are full at the default 0.25
HL_INSET = 0.80
# bubble sparkles: angle (deg), inner and outer offset from the mean radius (px); lip sparkles: two ends each
SPARKS = [(-56.0, 7.1, 16.7), (-46.0, 4.8, 14.2), (51.0, 6.3, 14.2), (60.0, 8.1, 17.7)]
SPARKS_POP = [(-68.0, 6.0, 13.0), (71.0, 6.0, 13.0), (-34.0, 9.0, 15.0), (39.0, 9.0, 15.0)]   # only on a kick
LIP_SPARKS = [((145.2, 65.1), (151.1, 58.8)), ((149.0, 70.4), (156.5, 66.6)), ((139.4, 74.6), (142.6, 70.6)),
              ((142.7, 93.5), (148.1, 99.6))]


def fit(x, y):
    return ((x - X0) * S, (y - Y0) * S)


def scr(x, y):
    """trace frame -> screen"""
    return (x * K + FX, y * K + FY)


def side(x):
    """-1 / 1 for an outer decoration left / right (by its centre, trace frame), else 0"""
    return -1 if x < OUTER[0] else 1 if x > OUTER[1] else 0


def decorations(art):
    """(trace centre x, screen ink x0, x1) of every star, cloud and dot"""
    out = []
    for st in art['strokes']:
        if st['color'] == LAV:
            continue
        xs = [scr(x, y)[0] for x, y in st['points']]
        out.append((sum(x for x, _ in st['points']) / len(st['points']), min(xs) - LAV_W / 2, max(xs) + LAV_W / 2))
    for x, y in DOTS:
        out.append((x, scr(x, y)[0] - 1.1, scr(x, y)[0] + 1.1))
    return out


def shifts(art):
    """the outer decorations' sideways moves (screen px), left and right: their ink spans EDGE"""
    d = decorations(art)
    return (EDGE[0] - min(a for c, a, _ in d if side(c) < 0), EDGE[1] - max(b for c, _, b in d if side(c) > 0))


# ---------------------------------------------------------------------------------------------------- trace mode
def trace(png):
    import numpy as np
    import tempfile
    from PIL import Image
    from scipy import ndimage as nd
    sys.path.insert(0, str(SDK / 'display'))
    import art_trace
    a = np.asarray(Image.open(png).convert('RGB')).astype(float)
    H, W = a.shape[:2]
    G, R = a[..., 1], a[..., 0]
    br = a.max(2)
    yy, xx = np.mgrid[0:H, 0:W]
    keep = np.ones((H, W), bool)
    keep[:, 662:1325] = False                         # the bubble, strands, sparkles, highlights
    keep[260:316, 745:800] = True                     # the star between
    keep &= ~nd.binary_dilation((G > R + 60) & (G > a[..., 2]), iterations=4)      # green
    for x, y in DOTS:                                 # the dots are drawn as circles
        mx, my = round(x / S + X0), round(y / S + Y0)
        keep[my - 12:my + 13, mx - 12:mx + 13] = False
    lips = (xx >= 626) & (xx <= 662) & (yy >= 506) & (yy <= 584) & (a.min(2) > 185)
    lips = nd.binary_dilation(lips, iterations=2)
    ink = keep & (br > 20) & ~lips                    # lips: primitives
    cov = np.clip(br / 225.0, 0, 1)[..., None]
    o = np.zeros_like(a)
    o[ink] = (np.array([0x9A, 0x98, 0xE1], float) * cov)[ink]
    pad = np.zeros((H, W + 250, 3))
    pad[:, 110:110 + W] = o                            # the frame starts left of the mockup: pad, frame x 4..1780
    # measured colours: stars and clouds, their ink's brightest 3 %
    def colour(box):
        x0, y0, x1, y1 = box
        r = a[y0:y1, x0:x1].reshape(-1, 3)
        b = r.max(1)
        return '#' + ''.join(f'{int(v):02X}' for v in r[b >= np.percentile(b, 97)].mean(0))
    with tempfile.TemporaryDirectory() as tmp:
        p = Path(tmp) / 'static.png'
        Image.fromarray(pad.clip(0, 255).astype(np.uint8)).save(p)
        rep = art_trace.trace(p, ['#9A98E1'], (4, 77, 1780, 965))
    stars_px = [(200, 185), (1415, 230), (80, 538), (772, 288), (1390, 635)]
    stars = [fit(x, y) for x, y in stars_px]
    clouds = [(fit(48, 238), fit(330, 295)), (fit(1340, 310), fit(1520, 365)), (fit(1325, 730), fit(1490, 775))]
    eye = (98.0, 66.0, 110.0, 78.0)
    meas = {'star': colour((180, 165, 220, 205)), 'cloud': colour((50, 240, 330, 295)), 'traced_width': rep['palette'][0]['width']}
    out = []
    for colr, width, piece in rep['strokes']:
        pts = [(round(x, 2), round(y, 2)) for x, y in piece[1]]
        n = sum(math.hypot(b[0] - a_[0], b[1] - a_[1]) for a_, b in zip(pts, pts[1:]))
        cx = sum(x for x, _ in pts) / len(pts)
        cy = sum(y for _, y in pts) / len(pts)
        in_eye = eye[0] <= cx <= eye[2] and eye[1] <= cy <= eye[3]
        if n < 2.2 and not in_eye:
            continue
        c = LAV
        if any(math.hypot(cx - sx, cy - sy) < 4 for sx, sy in stars):
            c = meas['star']
        if any(a0[0] - 1 <= cx <= a1[0] + 1 and a0[1] - 1 <= cy <= a1[1] + 1 for a0, a1 in clouds):
            c = meas['cloud']
        out.append({'color': c, 'points': pts, 'closed': bool(piece[2])})
    (HERE / 'taffy_art.json').write_text(json.dumps({'source': 'run/screen-mockups-2026-10-28/tf_A_bubblegum.png',
                                                    'fit': 'x = (mx + 106) * 20/111, y = (my - 77) * 20/111',
                                                    'measured': meas, 'strokes': out}, separators=(',', ':')) + '\n')
    print('traced', len(out), 'pieces; measured', meas)


# ------------------------------------------------------------------------------------------------- generate mode
def num(v):
    s = f'{v:.2f}'.rstrip('0').rstrip('.')
    return '0' if s == '-0' else s


def no_turnback(pts):
    out = []
    for p in pts:
        if out and math.hypot(p[0] - out[-1][0], p[1] - out[-1][1]) < 0.05:
            continue
        if len(out) >= 2:
            ax, ay = out[-1][0] - out[-2][0], out[-1][1] - out[-2][1]
            bx, by = p[0] - out[-1][0], p[1] - out[-1][1]
            if ax * bx + ay * by < 0 and abs(ax * by - ay * bx) < 1e-6:
                out.pop()
        out.append(p)
    return out


def simplify(pts, tol=0.25):
    """Douglas-Peucker within tol px: fewer points for R246 to parse and draw on screen entry"""
    if len(pts) < 3:
        return pts
    (x0, y0), (x1, y1) = pts[0], pts[-1]
    dx, dy = x1 - x0, y1 - y0
    n = math.hypot(dx, dy)
    def dist(p):
        return math.hypot(p[0] - x0, p[1] - y0) if n == 0 else abs(dy * (p[0] - x0) - dx * (p[1] - y0)) / n
    i = max(range(1, len(pts) - 1), key=lambda k: dist(pts[k]))
    if dist(pts[i]) <= tol:
        return [pts[0], pts[-1]]
    return simplify(pts[:i + 1], tol)[:-1] + simplify(pts[i:], tol)


def arc(c, r, a0, a1, step=20):
    n = max(2, int(abs(a1 - a0) / step) + 1)
    return [(c[0] + r * math.cos(math.radians(a0 + (a1 - a0) * i / (n - 1))),
             c[1] + r * math.sin(math.radians(a0 + (a1 - a0) * i / (n - 1)))) for i in range(n)]


PUCKER = (128.5, 79.5, 132.8, 89.0)   # the lips' inner outline (traced lavender, trace frame): drawn white as the lips
LIP_GAP = 1.4                         # the profile line stops this far (screen px, centre lines) from the white lips


def lips_clear(p):
    return all(math.hypot(p[0] - scr(*c)[0], p[1] - scr(*c)[1]) > r * K + LIP_GAP for c, r, _, _ in LIPS)


def svg_doc(art):
    dl, dr = shifts(art)
    els = []
    for s in art['strokes']:
        tr = s['points']
        cx = sum(x for x, _ in tr) / len(tr)
        dx = 0.0 if s['color'] == LAV else {-1: dl, 0: 0.0, 1: dr}[side(cx)]
        pts = [(x + dx, y) for x, y in (scr(x, y) for x, y in tr)]
        pts = no_turnback([(round(x, 2), round(y, 2)) for x, y in simplify(pts)])
        if len(pts) < 2:
            continue
        if all(PUCKER[0] <= x <= PUCKER[2] and PUCKER[1] <= y <= PUCKER[3] for x, y in tr):
            p = ' '.join(f'{num(x)},{num(y)}' for x, y in pts)
            els.append(f'<polyline points="{p}" fill="none" stroke="{WHITE}" stroke-width="1.25" stroke-linecap="round" stroke-linejoin="round"/>')
            continue
        while len(pts) >= 2 and not lips_clear(pts[-1]):      # the face profile ends where the lips start ...
            pts.pop()
        while len(pts) >= 2 and not lips_clear(pts[0]):       # ... and resumes after them
            pts.pop(0)
        if len(pts) < 2:
            continue
        p = ' '.join(f'{num(x)},{num(y)}' for x, y in pts)
        if s['closed']:
            d = 'M' + ' L'.join(f'{num(x)} {num(y)}' for x, y in pts) + ' Z'
            els.append(f'<path d="{d}" fill="none" stroke="{s["color"]}" stroke-width="{num(LAV_W)}" stroke-linejoin="round"/>')
        else:
            els.append(f'<polyline points="{p}" fill="none" stroke="{s["color"]}" stroke-width="{num(LAV_W)}" '
                       f'stroke-linecap="round" stroke-linejoin="round"/>')
    for c, r, a0, a1 in LIPS:
        pts = no_turnback([(round(x, 2), round(y, 2)) for x, y in arc(scr(*c), r * K, a0, a1)])
        p = ' '.join(f'{num(x)},{num(y)}' for x, y in pts)
        els.append(f'<polyline points="{p}" fill="none" stroke="{WHITE}" stroke-width="1.25" stroke-linecap="round" stroke-linejoin="round"/>')
    for x, y in DOTS:
        sx, sy = scr(x, y)
        sx += {-1: dl, 0: 0.0, 1: dr}[side(x)]
        els.append(f'<circle cx="{num(sx)}" cy="{num(sy)}" r="0.6" fill="{WHITE}" stroke="{WHITE}" stroke-width="1"/>')
    return ('<svg version="1.1" id="Layer_1" xmlns="http://www.w3.org/2000/svg" x="0px" y="0px" width="320px" '
            'height="160px" viewBox="0 0 320 160">\n' + '\n'.join(els) + '\n</svg>\n')


def q10(v):
    return int(round(v * 1024))


def ang(deg):                 # degrees -> SVG_SIN units (8192 a turn), 0..8191
    return int(round(deg * 8192 / 360)) & 8191


def header():
    cx, cy = scr(*BUBBLE)
    mx, my = scr(*MOUTH)
    st = []
    for parent, a, t0, bow in STRANDS:
        st.append('{%d,%d,%d,%d,%d}' % (-1 if parent is None else parent[0], 0 if parent is None else q10(parent[1]),
                                        ang(a), q10(t0), q10(bow)))
    hl = ','.join('{%d,%d,%d,%d}' % (ang(c), int(round(h * 8192 / 360)), col, int(round(t * 32767)))
                  for c, h, col, t in HIGHLIGHTS)
    sp = ','.join('{%d,%d,%d}' % (ang(a), q10(r0 * K), q10(r1 * K)) for a, r0, r1 in SPARKS)
    pop = ','.join('{%d,%d,%d}' % (ang(a), q10(r0 * K), q10(r1 * K)) for a, r0, r1 in SPARKS_POP)
    lip = ','.join('{%d,%d,%d,%d}' % (q10(scr(*a)[0]), q10(scr(*a)[1]), q10(scr(*b)[0]), q10(scr(*b)[1]))
                   for a, b in LIP_SPARKS)
    h = ['/* Generated by gen_svg.py from the owner\'s mockup A (the fit and every measurement are in its header).',
         ' * Coordinates Q10 screen units, angles 8192 a turn (SVG_SIN). Original drawing; no firmware data. */',
         '#ifndef TAFFY_GEOM_H', '#define TAFFY_GEOM_H', '#include <stdint.h>',
         f'#define TF_BX {q10(cx)}          /* the bubble at the mockup\'s size: centre, mean radius */',
         f'#define TF_BY {q10(cy)}', f'#define TF_BR {q10(BUBBLE_R * K)}',
         f'#define TF_LOBE0 {ang(LOBE0)}          /* the first lobe\'s peak at rest (the mockup\'s) */',
         f'#define TF_MX {q10(mx)}          /* the lips\' notch: the strands start here */',
         f'#define TF_MY {q10(my)}',
         f'#define TF_HL_INSET {int(round(HL_INSET * 1024))}    /* highlights at this share of the outline radius (Q10) */',
         '/* the live sizes at the fit\'s scale (Q10 px; screen.c): radius silent, at a held note of velocity 100 (the',
         '   mockup\'s mean radius + 1 px), the outline\'s limit; the note\'s kick (swell at ENVELOPE 0, added at full),',
         '   the sparkles\' reach on a kick, the highlights\' ripple at full RESONANCE */',
         f'#define TF_RSIL {q10(30 * K)}', f'#define TF_RLOUD {q10(46.8 * K)}', f'#define TF_RMAX {q10(56 * K)}',
         f'#define TF_KICK0 {q10(K)}', f'#define TF_KICK_ADD {q10(3 * K)}', f'#define TF_PUSH {q10(3 * K)}',
         f'#define TF_RIPPLE {q10(1.6 * K)}',
         '/* strands: parent strand (-1: from the mouth) and where along it (Q10), end angle on the outline, start along',
         '   the own line (Q10), bow (the control point\'s offset toward the top, Q10 of the strand\'s width) */',
         'static const int32_t tf_strand[%d][5]={' % len(STRANDS) + ','.join(st) + '};',
         '/* highlights: centre angle, half span, colour (0 red, 1 white), resonance (Q15) it appears at */',
         'static const int16_t tf_hl[%d][4]={' % len(HIGHLIGHTS) + hl + '};',
         '/* sparkles: angle, inner and outer offset from the mean radius (Q10); pop ones only on a kick */',
         'static const int32_t tf_spark[%d][3]={' % len(SPARKS) + sp + '};',
         'static const int32_t tf_pop[%d][3]={' % len(SPARKS_POP) + pop + '};',
         '/* by the lips: two ends each (Q10), the first the nearer */',
         'static const int32_t tf_lipspark[%d][4]={' % len(LIP_SPARKS) + lip + '};',
         '#endif']
    return '\n'.join(h) + '\n'


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--trace':
        trace(sys.argv[2])
        return
    if len(sys.argv) != 1:
        sys.exit(__doc__)
    art = json.loads((HERE / 'taffy_art.json').read_text())
    (HERE / 'taffy.svg').write_text(svg_doc(art))
    (HERE / 'taffy_geom.h').write_text(header())
    dl, dr = shifts(art)
    print('taffy.svg:', len(art['strokes']), 'traced pieces,', len(LIPS), 'lip arcs,', len(DOTS), 'dots; outer '
          f'decorations moved {dl:+.2f} / {dr:+.2f} px')


if __name__ == '__main__':
    main()
