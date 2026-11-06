#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""Generate wavetable.svg + wavetable_geom.h: WAVETABLE's screen, an oscilloscope stack of the engine's wavetable.

    python3 gen_svg.py

The design is the owner's pick, concept A "oscilloscope stack" (2026-10-31; OP1-Emulator
docs/design/wavetable-v2/build_a.py, native vector art at 320 x 160), with the owner's additions of the same day
(perspective floor, drop lines, playhead, harmonics, ghost curve, curve ticks, rail marks: build_a2.py there draws the
concept with them from this file's geometry). No art file is traced: the concept is code, so its numbers are used
as they are, with these changes:
- The whole drawing is lifted 15 px (LIFT): the concept's axis sits at y 152 and its curve floor at 151, below the
  OLED bottom margin (docs/screen-art.md: ink <= 138, stroke and animation extents included). Lifted, the axis is at
  y 137; the highest moving part (the bracket at the back frame) at y 5.6. main() refuses the art outside x 2..318,
  y 3..138 at every extreme.
- The stack shows the engine's own wavetable: frame k (0..9) is the morph the voices play at WAVE k / 9
  (wavetable_dsp_shape, the same integer interpolation, 64 samples a period, simplified within 0.05 px) instead of
  the concept's sine -> triangle -> saw -> square stand-in. 10 frames over 8 tables, so the frames between table
  rows are morphs.
- R246 has no dash arrays, opacity or arcs: dotted lines are written as dashes (1.5 on, 3 off), one <line> each
  (a stroked path of many subpaths hangs R246's GUI thread), the fainter green copy is drawn in #00ED95 at 60 % over black (#008E59), the red arcs are polylines.

Still (wavetable.svg, drawn through the static-art cache, back to front): the floor grid (navy dotted cross-lines
under each frame and three lengthwise lines), the two grey dotted rails, the lavender marks on the front rail (one a
frame), the ten frames (navy 6..9, plum 3..5, grey 0..2), the grey axis and ticks, and the navy ghost of the filter
curve at CUTOFF half, RESONANCE 0.

Moving (wavetable_screen.c, numbers from wavetable_geom.h):
  green  the frame at WAVE (continuous: between frames it shows the morph there, at its depth), 1.6 px, and a fainter
         copy 1.5 px right, phase-shifted by 0.04 + 0.12 x DETUNE turns plus the oscillators' phase drift; both swell
         up to 15 % with the level; the fundamental's bar in the harmonics readout
  blue   the bracket at the frame's left edge and a tick on the front rail; the playhead dot running along the frame
         while notes play (at its left end in silence)
  white  the low-pass curve (the concept's 2-pole response: 0.9 px a dB, knee at x 46 + 240 x CUTOFF, Q 0.7 + 7 x
         RESONANCE^1.5), computed in integers on a 1 px grid; four short ticks hanging under it round the knee
  red    1 to 4 arcs over the resonant peak (radius 5 + 4i px), RESONANCE
  lav.   drop lines from the frame's ends to the floor; the harmonics readout (top right): harmonics 2..10 of the
         frame at WAVE, from the table (WT_HARM, 64 positions, 40 dB range)
"""
import math
import re
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
BLUE, GREEN, WHITE, RED = '#698EFF', '#00ED95', '#FFFFFF', '#FF3A5D'
LAV, NAVY, PLUM, GREY = '#DED7FF', '#383572', '#514959', '#656579'
GREEN_DIM = '#008E59'                       # green at 60 % over black: the concept's stroke-opacity 0.6

LIFT = 15
N = 10                                      # frames in the stack
X0, Y0 = 24, 80 - LIFT                      # front frame's left baseline
W, H = 140, 14                              # frame width, half-height (the table's peak, 30000)
DX, DY = 13.0, -4.6                         # step back per frame
NW = 64                                     # samples a period (the screen code's grid)
FLOOR = 4                                   # the rails / floor below a frame's baseline
SWELL = 0.15
COPY_DX, COPY_PH0, COPY_PHD = 1.5, 0.04, 0.12
# the filter panel
PX0, PX1 = 10, 310
FLAT_Y, TOP_Y, BOT_Y = 126 - LIFT, 106 - LIFT, 151 - LIFT     # 0 dB, the top clamp, the floor
AXIS_Y = 152 - LIFT
KNEE0, KNEE_SPAN = 10 + 300 * 0.12, 300 * 0.8                  # knee x at CUTOFF 0, its travel
CI = 126                                    # curve grid: x offsets -126 .. +114 px from the knee, 1 px apart
CN = 241
TICKS = [(-48, 1.0), (-24, 0.9), (0, 0.8), (12, 0.6)]          # curve ticks: offset from the knee, width
TICK_LEN = 6
# the harmonics readout
HB_X0, HB_DX, HB_Y, HB_H, NH = 238, 7.6, 21, 14, 10
HB_SWELL = 0.1
HB_DB = 40
NPOS = 64
# widths
W_GREEN, W_COPY, W_BLUE, W_WHITE, W_RED, W_LAV, W_BAR, W_DOT = 1.6, 1.0, 1.4, 1.4, 1.2, 0.8, 1.2, 2.2
W_FRAME, W_DOTTED, W_AXIS = 1.0, 0.8, 0.8
# op1_raster_line's width is in display pixels (400 x 200), not screen units as SVG strokes are (native op1emu
# 2026-10-31: a 1.4 width drew 1.4 px where an SVG stroke of 0.8 drew 1.0 px). The live widths above are the
# concept's, in units; the header gets them x 1.25 so they draw as heavy as the concept. harness/oprender.py draws
# raster lines in units, so host frames show the live lines 25 % heavier than the device.
RPX = 1.25


def tables():
    t = (HERE / 'wavetables.h').read_text()
    body = t[t.index('wt_waves'):t.index('#define WT_CUTOFFS')]
    rows = re.findall(r'\{([-0-9, ]+)\}', body)
    return [[int(v) for v in r.split(',')] for r in rows]


WAVES = tables()


def shape(k0, ph):
    """wavetable_dsp_shape: the morph at knob k0 (0..32767), phase ph (2^32 a turn), C integer arithmetic"""
    pos = max(k0, 0) * 7
    wa = WAVES[pos >> 15]
    wb = WAVES[(pos >> 15) + ((pos >> 15) < 7)]
    frac = pos & 0x7FFF
    def rd(w):
        i, f = ph >> 24, (ph >> 9) & 0x7FFF
        return w[i] + (((w[i + 1] - w[i]) * f) >> 15)
    a = rd(wa)
    return a + (((rd(wb) - a) * frac) >> 15)


def frame_knob(k):
    return (k * 32767 + 4) // 9


def frame_vals(k0):
    v = [shape(k0, (j * (0x100000000 // NW)) & 0xFFFFFFFF) for j in range(NW)]
    return v + [v[0]]


def dp(pts, tol):
    """Douglas-Peucker"""
    if len(pts) < 3:
        return pts
    (x0, y0), (x1, y1) = pts[0], pts[-1]
    L = math.hypot(x1 - x0, y1 - y0) or 1e-9
    d = [abs((x1 - x0) * (y0 - y) - (x0 - x) * (y1 - y0)) / L for x, y in pts[1:-1]]
    i = max(range(len(d)), key=d.__getitem__)
    if d[i] <= tol:
        return [pts[0], pts[-1]]
    return dp(pts[:i + 2], tol)[:-1] + dp(pts[i + 1:], tol)


def frame_pts(k0, f, scale=1.0, phase=0.0, dx=0.0, tol=0.05):
    """frame k0 placed at depth f (frames), as screen points"""
    ox, oy = X0 + f * DX + dx, Y0 + f * DY
    v = frame_vals(k0)
    pts = []
    for j in range(NW + 1):
        t = j / NW
        u = (t + phase) % 1.0
        jj = u * NW
        a = int(jj)
        val = v[a] + (v[min(a + 1, NW)] - v[a]) * (jj - a)
        pts.append((ox + W * t, oy - H * scale * val / 30000))
    return dp(pts, tol)


# --- the filter curve, the screen code's integer arithmetic (curve_shape in wavetable_screen.c) ---
LOG_C = [2847, -12304, 26788, -46250, 94455, 2]           # log2(1 + f) on [0, 1), Q16, Horner
EXP_C = [896, 3386, 15841, 45412, 65536]                   # 2^f on [0, 1), Q16


def clz(v):
    return 32 - v.bit_length()


def log2_q16(x):
    """log2(x / 65536), Q16, x > 0"""
    n = 31 - clz(x)
    m = (x << (30 - n)) & 0x3FFFFFFF if n <= 30 else x >> (n - 30)
    f = (m & 0x3FFFFFFF) >> 14                       # Q16 mantissa fraction
    acc = LOG_C[0]
    for c in LOG_C[1:]:
        acc = ((acc * f) >> 16) + c
    return (n - 16) * 65536 + acc


def exp2_q16(e):
    """2^(e / 65536) in Q16"""
    n, f = e >> 16, e & 0xFFFF
    acc = EXP_C[0]
    for c in EXP_C[1:]:
        acc = ((acc * f) >> 16) + c
    return acc << n if n >= 0 else acc >> -n


def isqrt(v):
    return math.isqrt(v)


def curve_y(k3):
    """the curve's y (Q10 px) on the 1 px grid round the knee, for RESONANCE k3; and the peak index"""
    r15 = k3 * isqrt(k3 << 15) >> 15                 # res^1.5, Q15
    q16 = 45875 + ((7 * r15) << 1)                   # 0.7 + 7 res^1.5, Q16
    invq = 0xFFFFFFFF // q16                         # 1 / Q, Q16
    invq2 = invq * invq >> 16
    ys = []
    for i in range(CN):
        e = (i - CI) * 3193                          # 14.616 x d (d = offset / 300 px): log2 of r^2, Q16
        r2 = exp2_q16(e)
        a = 65536 - r2
        den = ((a * a) >> 16) + ((r2 * invq2) >> 16)
        if den < 1:
            den = 1
        y = FLAT_Y * 1024 + ((2774 * log2_q16(den)) >> 16)   # 9 x log10 = 2.7093 x log2 px; Q10
        ys.append(min(max(y, TOP_Y * 1024), BOT_Y * 1024))
    pk = min(range(CN), key=ys.__getitem__) if k3 > 655 else CI
    return ys, pk


def knee_x(k2):
    """Q10 px"""
    return int(KNEE0 * 1024) + (k2 * 15 >> 1)


def clip_x(pts, x0, x1):
    out = []
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        if bx <= ax or bx <= x0 or ax >= x1:
            continue
        if ax < x0:
            ay += (by - ay) * (x0 - ax) / (bx - ax); ax = x0
        if bx > x1:
            by = ay + (by - ay) * (x1 - ax) / (bx - ax); bx = x1
        if not out or out[-1] != (ax, ay):
            out.append((ax, ay))
        out.append((bx, by))
    return out


def curve_pts(k2, k3):
    """the white curve (screen points, clipped to the panel) and its peak (the arcs' anchor)"""
    ys, pk = curve_y(k3)
    kx = knee_x(k2) / 1024
    pts = [(kx + i - CI, y / 1024) for i, y in enumerate(ys)]
    pts = [(min(PX0, pts[0][0]) - 1, pts[0][1])] + pts + [(max(PX1, pts[-1][0]) + 1, BOT_Y)]
    px = min(max(kx + pk - CI, PX0), PX1)
    return clip_x(pts, PX0, PX1), (px, ys[pk] / 1024)


def arcs(px, py, n):
    out = []
    for i in range(n):
        r = 5 + 4 * i
        cy = py + 0.3 * r
        a0, a1 = math.radians(143.13), math.radians(36.87)
        out.append([(px + r * math.cos(a0 + (a1 - a0) * s / 6), cy - r * math.sin(a0 + (a1 - a0) * s / 6))
                    for s in range(7)])
    return out


# --- harmonics ---
def harmonics():
    """per WAVE position j / 63: harmonics 1..10 as bar heights 0..255 (40 dB under the sine's fundamental)"""
    ref = None
    out = []
    for j in range(NPOS):
        k0 = j * 32767 // (NPOS - 1)
        v = np.array([shape(k0, i << 24) for i in range(256)], float)
        m = np.abs(np.fft.rfft(v))[1:NH + 1]
        if ref is None:
            ref = m[0]
        db = 20 * np.log10(np.maximum(m / ref, 1e-9))
        out.append([int(round(255 * min(1.0, max(0.0, (d + HB_DB) / HB_DB)))) for d in db])
    return out


# --- the SVG ---
def f2(v):
    s = f'{v:.2f}'.rstrip('0').rstrip('.')
    return '0' if s == '-0' else s


def poly(pts, col, w):
    return (f'<polyline points="{" ".join(f"{f2(x)},{f2(y)}" for x, y in pts)}" fill="none" stroke="{col}" '
            f'stroke-width="{w}" stroke-linecap="round" stroke-linejoin="round"/>')


def line(x0, y0, x1, y1, col, w):
    return (f'<line x1="{f2(x0)}" y1="{f2(y0)}" x2="{f2(x1)}" y2="{f2(y1)}" stroke="{col}" stroke-width="{w}" '
            f'stroke-linecap="round"/>')


def dotted(x0, y0, x1, y1, col, w, on=1.5, off=3.0):
    """a dotted line as one <line> per dash: R246 hangs drawing a stroked path of many subpaths (native, 2026-10-31:
    a 32-subpath dotted path stopped the GUI thread on entering the screen)"""
    L = math.hypot(x1 - x0, y1 - y0)
    ux, uy = (x1 - x0) / L, (y1 - y0) / L
    out, s = [], 0.0
    while s < L - 0.3:
        e = min(s + on, L)
        out.append(f'<line x1="{f2(x0 + ux * s)}" y1="{f2(y0 + uy * s)}" x2="{f2(x0 + ux * e)}" y2="{f2(y0 + uy * e)}" '
                   f'stroke="{col}" stroke-width="{w}" stroke-linecap="round"/>')
        s += on + off
    return '\n'.join(out), out


def still():
    el, ink = [], []
    def seg(x0, y0, x1, y1, w):
        ink.append((min(x0, x1) - w / 2, max(x0, x1) + w / 2, min(y0, y1) - w / 2, max(y0, y1) + w / 2))
    fy = Y0 + FLOOR
    for k in range(N):                                       # floor: a cross-line under each frame
        x, y = X0 + k * DX, fy + k * DY
        el.append(dotted(x, y, x + W, y, NAVY, W_DOTTED)[0]); seg(x, y, x + W, y, W_DOTTED)
    for j in (1, 2, 3):                                      # and three lengthwise lines
        x = X0 + W * j / 4
        el.append(dotted(x, fy, x + (N - 1) * DX, fy + (N - 1) * DY, NAVY, W_DOTTED)[0])
    for x in (X0, X0 + W):                                   # the rails
        el.append(dotted(x, fy, x + (N - 1) * DX, fy + (N - 1) * DY, GREY, W_DOTTED)[0])
        seg(x, fy, x + (N - 1) * DX, fy + (N - 1) * DY, W_DOTTED)
    for k in range(N):                                       # marks on the front rail
        x, y = X0 + k * DX, fy + k * DY
        el.append(line(x, y + 1, x, y + 3, LAV, W_LAV)); seg(x, y + 1, x, y + 3, W_LAV)
    for k in range(N - 1, -1, -1):                           # the frames, back to front
        col = NAVY if k > N // 2 else PLUM if k > 2 else GREY
        p = frame_pts(frame_knob(k), k)
        el.append(poly(p, col, W_FRAME))
        for a, b in zip(p, p[1:]):
            seg(*a, *b, W_FRAME)
    el.append(line(PX0, AXIS_Y, PX1, AXIS_Y, GREY, W_AXIS)); seg(PX0, AXIS_Y, PX1, AXIS_Y, W_AXIS)
    for i in range(1, 10):
        x = PX0 + 300 * i / 10
        el.append(line(x, AXIS_Y - 4, x, AXIS_Y, GREY, W_AXIS))
    g, _ = curve_pts(16384, 0)                               # the ghost: CUTOFF half, RESONANCE 0
    g = dp(g, 0.05)
    el.append(poly(g, NAVY, 1.0))
    for a, b in zip(g, g[1:]):
        seg(*a, *b, 1.0)
    return el, ink


def live_extents():
    """ink boxes of the moving parts at their extremes"""
    box = []
    def add(pts, w):
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        box.append((min(xs) - w / 2, max(xs) + w / 2, min(ys) - w / 2, max(ys) + w / 2))
    for k0 in range(0, 32768, 512):
        f = k0 * 9 / 32767
        for ph in (0.0, 0.25, 0.5, 0.75):
            add(frame_pts(k0, f, 1 + SWELL, ph, COPY_DX, 0.2), W_COPY)
        add(frame_pts(k0, f, 1 + SWELL), W_GREEN)
        ox, oy = X0 + f * DX, Y0 + f * DY
        add([(ox - 5, oy - H - 4), (ox - 2, oy + H + 4)], W_BLUE)             # bracket
        add([(ox, oy + FLOOR), (ox, oy + FLOOR + 4)], W_BLUE)                 # rail tick
    for k2 in (0, 16384, 32767):
        for k3 in (0, 16384, 32767):
            pts, (px, py) = curve_pts(k2, k3)
            add(pts, W_WHITE)
            for a in arcs(px, py, 1 + k3 * 3 // 32767):
                add(a, W_RED)
    add([(HB_X0, HB_Y - HB_H * (1 + HB_SWELL)), (HB_X0 + (NH - 1) * HB_DX, HB_Y)], W_BAR)
    xs0 = min(b[0] for b in box); xs1 = max(b[1] for b in box)
    ys0 = min(b[2] for b in box); ys1 = max(b[3] for b in box)
    return xs0, xs1, ys0, ys1


def q10(v):
    return int(round(v * 1024))


def header(harm):
    L = ['/* generated by gen_svg.py: the moving parts of WAVETABLE\'s screen (the oscilloscope stack). Q10 screen px '
         'unless named. */']
    d = {'WT_X0': q10(X0), 'WT_Y0': q10(Y0), 'WT_W': q10(W), 'WT_H': q10(H), 'WT_DX': q10(DX), 'WT_DY': q10(DY),
         'WT_NF': N, 'WT_NW': NW, 'WT_FLOOR': q10(FLOOR), 'WT_SWELL': q10(SWELL), 'WT_COPY_DX': q10(COPY_DX),
         'WT_COPY_PH0': int(COPY_PH0 * 65536), 'WT_COPY_PHD': int(COPY_PHD * 65536), 'WT_PEAK': 30000,
         'WT_PX0': q10(PX0), 'WT_PX1': q10(PX1), 'WT_FLAT_Y': FLAT_Y, 'WT_TOP_Y': TOP_Y, 'WT_BOT_Y': BOT_Y,
         'WT_KNEE0': q10(KNEE0), 'WT_CI': CI, 'WT_CN': CN, 'WT_TICK_LEN': q10(TICK_LEN),
         'WT_HB_X0': q10(HB_X0), 'WT_HB_DX': q10(HB_DX), 'WT_HB_Y': q10(HB_Y), 'WT_HB_H': q10(HB_H),
         'WT_HB_SWELL': q10(HB_SWELL), 'WT_NH': NH, 'WT_NPOS': NPOS,
         'WT_W_GREEN': q10(W_GREEN * RPX), 'WT_W_COPY': q10(W_COPY * RPX), 'WT_W_BLUE': q10(W_BLUE * RPX),
         'WT_W_WHITE': q10(W_WHITE * RPX), 'WT_W_RED': q10(W_RED * RPX), 'WT_W_LAV': q10(W_LAV * RPX),
         'WT_W_BAR': q10(W_BAR * RPX), 'WT_W_DOT': q10(W_DOT * RPX)}
    L += [f'#define {k} {v}' for k, v in d.items()]
    L.append(f'static const int16_t WT_TICK_OFF[{len(TICKS)}] = {{ {", ".join(str(o) for o, _ in TICKS)} }};')
    L.append(f'static const int16_t WT_TICK_W[{len(TICKS)}] = {{ {", ".join(str(q10(w * RPX)) for _, w in TICKS)} }};')
    L.append(f'static const int32_t WT_LOG_C[6] = {{ {", ".join(map(str, LOG_C))} }};')
    L.append(f'static const int32_t WT_EXP_C[5] = {{ {", ".join(map(str, EXP_C))} }};')
    L.append(f'/* harmonics 1..{NH} of the morph at WAVE j / {NPOS - 1}: bar height 0..255 over {HB_DB} dB under the '
             f'sine\'s fundamental */')
    L.append(f'static const uint8_t WT_HARM[{NPOS}][{NH}] = {{')
    for r in harm:
        L.append('    { ' + ', '.join(map(str, r)) + ' },')
    L.append('};')
    return '\n'.join(L) + '\n'


def svg(el):
    return ('<?xml version="1.0" encoding="utf-8"?>\n<svg version="1.1" id="Layer_1" xmlns="http://www.w3.org/2000/svg" '
            'x="0px" y="0px"\n\t width="320px" height="160px" viewBox="0 0 320 160">\n'
            '<!-- generated by gen_svg.py: the still parts of the oscilloscope stack; the moving parts by code -->\n'
            + '\n'.join(el) + '\n</svg>\n')


def main():
    el, ink = still()
    ix0, ix1 = min(b[0] for b in ink), max(b[1] for b in ink)
    iy0, iy1 = min(b[2] for b in ink), max(b[3] for b in ink)
    bx0, bx1, by0, by1 = live_extents()
    if not (2 <= bx0 and bx1 <= 318 and 3 <= by0 and by1 <= 138):
        raise SystemExit(f'moving parts out of bounds: x {bx0:.1f}..{bx1:.1f} y {by0:.1f}..{by1:.1f}')
    if not (2 <= ix0 and ix1 <= 318 and 3 <= iy0 and iy1 <= 138):
        raise SystemExit(f'still drawing out of bounds: x {ix0:.1f}..{ix1:.1f} y {iy0:.1f}..{iy1:.1f}')
    (HERE / 'wavetable.svg').write_text(svg(el))
    (HERE / 'wavetable_geom.h').write_text(header(harmonics()))
    print(f'ok: {len(el)} still elements, still ink x {ix0:.1f}..{ix1:.1f} y {iy0:.1f}..{iy1:.1f}, '
          f'moving extents x {bx0:.1f}..{bx1:.1f} y {by0:.1f}..{by1:.1f}')


if __name__ == '__main__':
    main()
