#!/usr/bin/env python3
"""Generate the hollow_saint screen: plugin/hollow_saint/hollow_saint.svg and hollow_saint_geom.h.

    python3 tools/gen_screen.py            (in kennel: python3 synth_sdk/plugins/hollowsaint/gen_screen.py)

A hurdy-gurdy in line art, seen from above, standing in a pointed gothic arch, in the OP-1's line style and palette:
blue #4D9EFF (DRONE: the drone strings), green #00ED95 (STRINGS: the melody strings and the keys), white #FFFFFF
(WHEEL BUZZ: the wheel, the crank, the dog), red #FF3A5D (CATHEDRAL: the saint's halo, the rose window, the bell),
grey #6F7173 for the architecture and details. Only what R246's renderer draws: groups (ids, no paint), lines,
polylines, circles and M/L/C paths, each element with its own stroke; no fills, text, opacity or CSS; at most two
decimals. Moving parts are groups with ids; the C screen code moves them (hollow_saint_geom.h has their pivots).
Vibrating strings are two arcs that part (never a closed lens: its tips would turn back 180 degrees, which R246's
stroke join draws as a spike).
Copyright (C) 2026 the hollow_saint authors. GPL-3.0-or-later with the OP-1 firmware permission. Original artwork.

kennel adaptation (2026-10-29, Hundekraft): the OLED's lower edge (kennel docs/screen-art.md "Placement and OLED
bottom margin"). Upstream's ink reached y 156; here the scene (arch, saint, instrument, dust) is drawn at SCENE_S
about the arch's apex and the bottom row (meters and page-2 icons) moved up by ROW_DY, so all ink and animation
stays within y 3..138. Pivots of moving parts are snapped to whole units (each cluster shifted < 0.5) so the C code's
integer pivots stay exact. Writes next to this file.
"""
import math
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = HERE                                         # kennel: the plugin folder itself

# ---- kennel placement: scene scale about the apex (160, 3), the bottom row's lift, a per-cluster snap offset
SCENE_S, SCENE_CX, SCENE_CY, ROW_DY = 0.85, 160.0, 3.0, -19
xf = ['scene', 0.0, 0.0]                           # mode ('scene', 'row', 'none'), snap dx, dy


def P(x, y):
    m = xf[0]
    if m == 'scene':
        return SCENE_CX + (x - SCENE_CX) * SCENE_S + xf[1], SCENE_CY + (y - SCENE_CY) * SCENE_S + xf[2]
    if m == 'row':
        return x, y + ROW_DY
    return x, y


def RS(r):
    return r * SCENE_S if xf[0] == 'scene' else r


def snap(px, py):
    """shift what follows so that the pivot (px, py) lands on whole units; returns the pivot"""
    xf[1] = xf[2] = 0.0
    x, y = P(px, py)
    xf[1], xf[2] = round(x) - x, round(y) - y
    return round(x), round(y)


def unsnap():
    xf[1] = xf[2] = 0.0


def xd(d):
    """a path's absolute x,y pairs through P (the paths here are M / L / C / Z with absolute pairs only)"""
    def one(m):
        x, y = P(float(m.group(1)), float(m.group(2)))
        return f'{f(x)},{f(y)}'
    return re.sub(r'(-?[\d.]+),(-?[\d.]+)', one, d)
B, G, W, R, GR, DK = '#4D9EFF', '#00ED95', '#FFFFFF', '#FF3A5D', '#6F7173', '#3A3C40'

out = []
batch = {}                                         # (colour, width) -> [subpath strings], static art only
static = [True]                                    # inside a group: draw elements directly


def f(v):
    s = f'{v:.2f}'.rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


def st(c, w=1.2):
    return f' fill="none" stroke="{c}" stroke-width="{f(w)}" stroke-linecap="round" stroke-linejoin="round"'


def line(x1, y1, x2, y2, c, w=1.2):
    (x1, y1), (x2, y2) = P(x1, y1), P(x2, y2)
    seg = f'M{f(x1)},{f(y1)} L{f(x2)},{f(y2)}'
    if static[0]:
        batch.setdefault((c, w), []).append(seg)
    else:
        gbuf.setdefault((c, w), []).append(seg)


def poly(pts, c, w=1.2):
    out.append(f'<polyline points="{" ".join(f"{f(x)},{f(y)}" for x, y in (P(*q) for q in pts))}"{st(c, w)}/>')


def path(d, c, w=1.2):
    d = xd(d)
    if static[0]:
        batch.setdefault((c, w), []).append(d)
    else:
        gbuf.setdefault((c, w), []).append(d)


def circle(cx, cy, r, c, w=1.2):
    (cx, cy), r = P(cx, cy), RS(r)
    out.append(f'<circle cx="{f(cx)}" cy="{f(cy)}" r="{f(r)}"{st(c, w)}/>')


def dot(x, y, c, s=0.7, w=1.2):
    """a dot: two short crossing lines (R246 draws no zero-length line)"""
    line(x - s, y, x + s, y, c, w); line(x, y - s, x, y + s, c, w)


gbuf = {}


def flush(parts_by_style):
    """kennel: one <path> per subpath. R246 joins a path's subpaths (each further M draws a line from the last point,
    stroked as well as filled), which drew diagonals across the pegs and keys on the emulator (2026-10-29)"""
    for (c, w), parts in parts_by_style.items():
        for d in parts:
            for sub in re.split(r' (?=M)', d):
                out.append(f'<path d="{sub}"{st(c, w)}/>')


def group(gid):
    flush(batch); batch.clear()                    # keep the drawing order: static art drawn so far goes first
    out.append(f'<g id="{gid}">')
    static[0] = False


def end():
    flush(gbuf); gbuf.clear()
    out.append('</g>')
    static[0] = True


geom = {}

# ---------------------------------------------------------------- the cathedral: a pointed arch, a floor, the bell
path('M4,136 L4,66 C4,40 120,22 160,3 C200,22 316,40 316,66 L316,136', GR, 1)
path('M9,136 L9,68 C9,46 118,30 160,10 C202,30 311,46 311,68 L311,136', DK, 1)
line(4, 136, 316, 136, GR, 1)
for x in range(24, 300, 34):                       # floor stones
    line(x, 136, x - 6, 141, DK, 1)
geom['BELL_X'], geom['BELL_Y'] = snap(160, 11)
group('bell')                                      # swings around its rope's top
line(160, 11, 160, 15, R)
path('M155.5,22 C155.5,15 164.5,15 164.5,22 L166,25 L154,25 Z', R)
dot(160, 27.5, R, 0.8)
end()
unsnap()

# ---------------------------------------------------------------- the saint: hooded head, hollow eyes, halo
geom['HALO_X'], geom['HALO_Y'] = snap(27, 78)
group('halo')
circle(27, 78, 15, R, 1)
end()
unsnap()
path('M18,95 C18,80 21,70 27,63 C33,70 36,80 36,95', W)           # the hood, a lancet arch
path('M22,92 C22,86 24,81 27,81 C30,81 32,86 32,92', GR)          # the face
dot(25, 86, W, 0.5); dot(29, 86, W, 0.5)                          # hollow eyes
line(36, 80, 44, 80, W)                                           # neck to the pegbox

# ---------------------------------------------------------------- pegbox and pegs
path('M44,72 L82,72 M44,88 L82,88 M44,72 L44,88', W)
for x, top in ((54, True), (64, False), (72, True), (60, True), (76, False)):
    y0, y1 = (72, 62) if top else (88, 98)
    line(x, y0, x, y1, W)
    yk = y1 - 2 if top else y1 + 2
    path(f'M{x - 2},{yk} L{x + 2},{yk}', W, 2.4)                  # peg heads
for y, x in ((76, 54), (80, 64), (84, 72)):                       # strings to their pegs
    line(82, y, x, y, GR, 0.8)

# ---------------------------------------------------------------- the keybox and its keys (24 chromatic keys)
path('M82,66 L182,66 L182,94 L82,94 Z', W)
line(86, 70, 178, 70, DK, 1); line(86, 90, 178, 90, DK, 1)       # the lid
for x in (100, 164):                                               # lid hinges
    line(x - 3, 66, x + 3, 66, GR, 2)
for x in (94, 110, 126, 142, 158):                                  # a gothic arcade carved in the lid
    path(f'M{x},86 L{x},79 C{x},76 {x + 3},74 {x + 4},73 C{x + 5},74 {x + 8},76 {x + 8},79 L{x + 8},86', GR, 1)
KEY0, KEYDX = 88.0, 6.0                                            # naturals: one per KEYDX
nat = [0, 2, 4, 5, 7, 9, 11]                                       # pitch classes of the naturals from G: G A B C D E F#?
# a hurdy-gurdy keyboard from G: naturals G A B C D E F (as on a piano) and sharps between: 24 keys over two octaves
names = []
i_nat = 0
keys = []
for s in range(24):                                                # semitones above G3
    pc = (s + 7) % 12                                              # pitch class (C = 0)
    is_nat = pc in (0, 2, 4, 5, 7, 9, 11)
    if is_nat:
        x = KEY0 + i_nat * KEYDX
        i_nat += 1
        keys.append((s, x, True))
    else:
        keys.append((s, x + KEYDX / 2, False))                    # between the previous natural and the next
for s, x, nat_ in keys:                            # static: pressed keys are lit by the markers below
    if nat_:
        path(f'M{f(x)},95.5 L{f(x)},103 M{f(x - 1.5)},103 L{f(x + 1.5)},103', G)
    else:
        line(x, 95.5, x, 99.5, G, 2)
geom['KEY_X'] = [round((P(x, 0)[0] - P(0, 0)[0]) * 4) for _, x, _ in keys]  # x4 (quarter units): moves from x 0
for i in range(6):                                  # one lit key per voice, moved by the C code
    group(f'kp{i}')
    line(0, 96, 0, 102.5, W, 1.6)
    end()

# ---------------------------------------------------------------- the body: a guitar-shaped vielle, purfling, C-holes
path('M182,62 C188,46 196,42 206,42 C216,42 220,50 230,50 C240,50 246,32 264,32 C282,32 296,48 296,80'
     ' C296,112 282,128 264,128 C246,128 240,110 230,110 C220,110 216,118 206,118 C196,118 188,114 182,98', W)
path('M187,61 C192,50 198,47 206,47 C214,47 218,54 230,54', DK, 1)
path('M187,99 C192,110 198,113 206,113 C214,113 218,106 230,106', DK, 1)
path('M262,37 C278,37 291,50 291,80 C291,110 278,123 262,123', DK, 1)
path('M197,52 C193,52 193,58 197,58', GR, 1); path('M197,108 C193,108 193,102 197,102', GR, 1)

# the rose window (rosette) on the upper bout: rings, tracery, 8 petals that the CATHEDRAL knob lights
RX, RY = 210, 80
circle(RX, RY, 11, R, 1)
circle(RX, RY, 3, R, 1)
for k in range(8):
    a = math.pi * k / 4
    line(RX + 3 * math.cos(a), RY + 3 * math.sin(a), RX + 11 * math.cos(a), RY + 11 * math.sin(a), R, 0.8)
for k in range(8):
    a = math.pi * (k + 0.5) / 4
    group(f'p{k}')
    circle(RX + 7.5 * math.cos(a), RY + 7.5 * math.sin(a), 1.8, R, 0.9)
    end()

# ---------------------------------------------------------------- the wheel (seen through the cover) and its cover
WX, WY = 247, 80
geom['WHEEL_X'], geom['WHEEL_Y'] = snap(WX, WY)
circle(WX, WY, 26, W)
circle(WX, WY, 22.5, GR, 1)
group('wheel')
for k in range(6):
    a = math.pi * k / 3
    line(WX + 4 * math.cos(a), WY + 4 * math.sin(a), WX + 22 * math.cos(a), WY + 22 * math.sin(a), W, 1)
for k in range(12):                                # rosin marks on the rim: you see them roll
    a = math.pi * k / 6 + 0.26
    line(WX + 23.6 * math.cos(a), WY + 23.6 * math.sin(a), WX + 25 * math.cos(a), WY + 25 * math.sin(a), W, 1)
end()
circle(WX, WY, 4, W)
unsnap()

# ---------------------------------------------------------------- strings, bridges, the dog, tailpiece
TAIL = 293
STRINGS = [('d0', 58, B, 84), ('d1', 63, B, 84), ('m0', 77, G, 182), ('m1', 83, G, 182), ('t0', 108, W, 84)]
geom['STRING_Y'] = []
for i, (sid, y, c, x0) in enumerate(STRINGS):      # two arcs per string, parted (scaled about the string) to vibrate
    xf[1] = xf[2] = 0.0
    yr = P(0, y)[1]
    xf[2] = round(yr) - yr
    geom['STRING_Y'].append(round(yr))
    group(f's{i}')
    for sgn in (-1, 1):
        path(f'M{x0},{y} C{f(x0 + (TAIL - x0) / 3)},{f(y + sgn * 0.6)} {f(TAIL - (TAIL - x0) / 3)},{f(y + sgn * 0.6)} {TAIL},{y}', c, 1)
    end()
unsnap()
for y in (58, 63):                                  # nuts for the drones at the keybox
    line(84, y - 1.5, 84, y + 1.5, W)
line(281, 73, 281, 87, W, 2)                        # the melody bridge
line(283, 55, 283, 66, W, 2)                        # the drone bridge
geom['DOG_X'], geom['DOG_Y'] = (round(v) for v in P(280, 108))
group('dog')                                        # the buzzing bridge on the trompette
path('M275,113 L279,107.5 L286,107.5', W, 1.4)
end()
line(286, 108, 291, 114, GR, 0.8)                   # its tuning thread (tirant)
line(TAIL, 55, TAIL, 110, GR, 1.4)                  # tailpiece pins

# ---------------------------------------------------------------- the crank
CX, CY = 301, 80
geom['CRANK_X'], geom['CRANK_Y'] = snap(CX, CY)
line(296, 80, CX, 80, W, 1.6)
group('crank')
line(CX, CY, CX, CY - 11, W, 1.6)
circle(CX, CY - 11, 2.4, W, 1.2)
end()
circle(CX, CY, 1.6, W, 1)
unsnap()

# ---------------------------------------------------------------- meters (blue, green, white, red) and page-2 icons
xf[0] = 'row'                                       # the bottom row: moved up by ROW_DY, not scaled
MX = [24, 100, 176, 252]
ML, MY = 44, 150
geom['METER_X'], geom['METER_LEN'], geom['METER_Y'] = MX, ML, MY + ROW_DY
for i, c in enumerate((B, G, W, R)):
    line(MX[i], MY, MX[i] + ML, MY, DK, 1.6)
    group(f'm{i}')
    line(MX[i], MY, MX[i] + ML, MY, c, 1.6)
    end()
    line(MX[i], MY - 3, MX[i], MY + 3, c, 1)
# blue page 2: one octave of keys (C .. B) and the FOLLOW arrow; a dot marks the drone's key
KX, KW, KY = 34, 5, 142
geom['KBD_X'], geom['KBD_W'], geom['KBD_Y'] = KX, KW, KY + ROW_DY
group('kbd')
path(f'M{KX},{KY} L{KX + 7 * KW},{KY} L{KX + 7 * KW},{KY + 12} L{KX},{KY + 12} Z', B, 1)
for i in range(1, 7):
    line(KX + i * KW, KY + 6, KX + i * KW, KY + 12, B, 0.8)
for i in (1, 2, 4, 5, 6):                            # black keys after C D F G A
    line(KX + i * KW, KY, KX + i * KW, KY + 6, B, 2)
path(f'M{KX - 8},{KY + 1} L{KX - 8},{KY + 10} M{KX - 11},{KY + 7} L{KX - 8},{KY + 10} L{KX - 5},{KY + 7}', B, 1)   # follow
end()
xf[0] = 'none'                                       # kdot: at the origin, placed by the C code (translate)
group('kdot')
circle(0, 0, 1.6, W, 1.2)
end()
xf[0] = 'row'
# green page 2 (the Lo-Fi Relic is on while it is open): a worn cassette whose reels turn with the tape's wobble, and
# the relic's age as a short meter beside it
CX0, CY0 = MX[1], 141
geom['CAS_X'], geom['CAS_Y'] = CX0, CY0 + ROW_DY
group('relic')
path(f'M{CX0},{CY0} L{CX0 + 26},{CY0} L{CX0 + 26},{CY0 + 15} L{CX0},{CY0 + 15} Z', G, 1)
path(f'M{CX0 + 5},{CY0 + 15} L{CX0 + 7},{CY0 + 11} L{CX0 + 19},{CY0 + 11} L{CX0 + 21},{CY0 + 15}', G, 1)   # tape guide
line(CX0 + 9, CY0 + 6, CX0 + 17, CY0 + 6, G, 0.8)                                                      # the window
path(f'M{CX0 + 26},{CY0 + 3} L{CX0 + 23},{CY0 + 5} L{CX0 + 24.5},{CY0 + 7} L{CX0 + 22},{CY0 + 9}', GR, 0.8)   # a crack
end()
geom['REEL_X'] = [CX0 + 6, CX0 + 20]
geom['REEL_Y'] = CY0 + 6 + ROW_DY
for i, x in enumerate(geom['REEL_X']):                # the reels: hubs with three teeth, turned by the C code
    group(f'rl{i}')
    circle(x, CY0 + 6, 3, G, 1)
    for k in range(3):
        a = 2 * math.pi * k / 3
        line(x + 1 * math.cos(a), CY0 + 6 + 1 * math.sin(a), x + 2.4 * math.cos(a), CY0 + 6 + 2.4 * math.sin(a), G, 0.8)
    end()
AGX, AGL = CX0 + 30, 14
geom['AGE_X'], geom['AGE_LEN'] = AGX, AGL
group('rage0')                                        # the age meter's track
line(AGX, MY, AGX + AGL, MY, DK, 1.6)
end()
group('rage')                                         # the age, scaled from AGE_X
line(AGX, MY, AGX + AGL, MY, G, 1.6)
end()
# dust in the cathedral while the relic plays: specks the C code shows and jitters
xf[0] = 'scene'                                      # the dust is in the cathedral
group('dust')
for x, y in ((30, 52), (74, 40), (118, 24), (196, 26), (238, 38), (286, 50), (52, 116), (150, 118), (226, 124),
             (300, 112)):
    dot(x, y, GR, 0.5, 0.9)
end()
xf[0] = 'row'

# white page 2: the stroke rate as a beat cut in parts (steady, 1 bar, 1/2, 1/4, 1/8, 1/8T, 1/16); the slow rates
# as one stroke under a slur over the bar (1 bar) or half of it (1/2)
RX0, RL, RY0 = 176, 44, 146
line_ids = []
for zone, n in enumerate((0, -4, -2, 1, 2, 3, 4)):
    group(f'r{zone}')
    line(RX0, RY0 + 8, RX0 + RL, RY0 + 8, W, 1)
    if n == 0:
        line(RX0, RY0 + 3, RX0 + RL, RY0 + 3, W, 1.6)
    if n < 0:
        span = (RL - 4) * (-n) / 4
        line(RX0 + 2, RY0, RX0 + 2, RY0 + 6, W, 1.6)
        path(f'M{RX0 + 2},{RY0 - 1} C{f(RX0 + 2 + span / 3)},{RY0 - 5} {f(RX0 + 2 + 2 * span / 3)},{RY0 - 5} '
             f'{f(RX0 + 2 + span)},{RY0 - 1}', W, 0.8)
    for j in range(max(n, 0)):
        x = RX0 + 2 + j * (RL - 4) / n
        line(x, RY0, x, RY0 + 6, W, 1.6)
    if n == 3:
        path(f'M{RX0 + 2},{RY0 - 2} L{RX0 + 2},{RY0 - 4} L{RX0 + RL - 6},{RY0 - 4} L{RX0 + RL - 6},{RY0 - 2}', W, 0.8)
    end()

flush(batch); batch.clear()
svg = ('<svg version="1.1" xmlns="http://www.w3.org/2000/svg" x="0px" y="0px" width="320px" height="160px" '
       'viewBox="0 0 320 160">\n' + '\n'.join(out) + '\n</svg>\n')
open(os.path.join(OUT, 'hollow_saint.svg'), 'w').write(svg)

h = ['/* generated by tools/gen_screen.py: positions in hollow_saint.svg (SVG units) */', '#ifndef HOLLOW_SAINT_GEOM_H',
     '#define HOLLOW_SAINT_GEOM_H']
for k, v in geom.items():
    if isinstance(v, list):
        h.append(f'static const int16_t HS_{k}[{len(v)}] = {{ {", ".join(str(int(x)) for x in v)} }};')
    else:
        h.append(f'#define HS_{k} {int(v)}')
h.append(f'#define HS_RX0 {RX0}')
h.append('#endif')
open(os.path.join(OUT, 'hollow_saint_geom.h'), 'w').write('\n'.join(h) + '\n')
print(f'hollow_saint.svg: {len(svg)} bytes, {len(out)} elements; hollow_saint_geom.h')
