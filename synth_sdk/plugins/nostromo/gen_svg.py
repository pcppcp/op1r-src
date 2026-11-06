#!/usr/bin/env python3
"""Generate nostromo.svg + nostromo_geom.h: the screen of the "nostromo" FX (nostromo.c), one page: the Lexicon 224's
LED readout, a scope of the reverb's impulse response (drawn by the screen code), the Nostromo riding the tail, each
knob's two page labels and a MU/TH/UR caption of the program. Lettering in a chamfered 4 x 6 stroke font, 1 unit per
grid step (the title 2 units).

Static: the title, frames, the scope's rules and axis, lettering. Groups the screen code switches on / off (node +0x50),
moves (op1_node_set_transform) or recolours (op1_node_line_rgba):
  s0a..s2g dp0 dp1 LED segments of 3 digits, decimal points after digit 0 / 1;  u0..u3  SEC MS HZ KHZ (lit only)
  nm0..6           the readout's control (mid, treble, pre-delay, mix, bass, clock, program) in its knob's colour
  l0a.. l6a..      the labels MID BASS TREB CLOCK PRE-DLAY PROGRAM RATE, one stroke an element, recoloured bright / dim
                   by page;  lp lr  PRE-DLAY, RATE (white's page 1 in LOFI: the clock);  nm7  RATE on the readout
  cp               the caption, the program in words: wa0..2 wb0..2 wc0..2 wd0..2 type, diffusion, tank mod, clock
  bt e0 e1         messages in the caption's place: MU/TH/UR 6000, SPECIAL ORDER 937, CREW EXPENDABLE
  cv cu            the cover that types the caption out (scaled from its right edge), the cursor (moved)
  xh1 xt0..9 xo0..9  the mix percent's digits
  sh               the ship, its top-left corner at 0, 0;  sr0..5  stars, at 0, 0 (sf0..5: their fills, recoloured)
For the OP-1: inline attributes only, no opacity, no dashes, no exponents, dim parts as darker colours; no stroke
under 1 px and no path without width or height (checked by harness/nostromo_svg_check.py).

    python3 plugins/nostromo/gen_svg.py
"""
import math, os

HERE = os.path.dirname(os.path.abspath(__file__))
BLUE, GREEN, WHITE, RED, GREY, BLACK = '#4d9eff', '#00ed95', '#ffffff', '#ff3a5d', '#6f7173', '#000000'
F = {                                                    # polylines on a 4 x 6 grid, y down; advance 5
    'A': [[(0, 6), (0, 1), (1, 0), (3, 0), (4, 1), (4, 6)], [(0, 3), (4, 3)]],
    'B': [[(0, 0), (3, 0), (4, 1), (4, 2), (3, 3), (0, 3)], [(3, 3), (4, 4), (4, 5), (3, 6), (0, 6), (0, 0)]],
    'C': [[(4, 1), (3, 0), (1, 0), (0, 1), (0, 5), (1, 6), (3, 6), (4, 5)]],
    'D': [[(0, 0), (3, 0), (4, 1), (4, 5), (3, 6), (0, 6), (0, 0)]],
    'E': [[(4, 0), (0, 0), (0, 6), (4, 6)], [(0, 3), (3, 3)]],
    'F': [[(4, 0), (0, 0), (0, 6)], [(0, 3), (3, 3)]],
    'G': [[(4, 1), (3, 0), (1, 0), (0, 1), (0, 5), (1, 6), (3, 6), (4, 5), (4, 3), (2, 3)]],
    'H': [[(0, 0), (0, 6)], [(4, 0), (4, 6)], [(0, 3), (4, 3)]],
    'I': [[(1, 0), (3, 0)], [(2, 0), (2, 6)], [(1, 6), (3, 6)]],
    'K': [[(0, 0), (0, 6)], [(4, 0), (1, 3), (0, 3)], [(1, 3), (4, 6)]],
    'L': [[(0, 0), (0, 6), (4, 6)]],
    'M': [[(0, 6), (0, 0), (2, 3), (4, 0), (4, 6)]],
    'N': [[(0, 6), (0, 0), (4, 6), (4, 0)]],
    'O': [[(1, 0), (3, 0), (4, 1), (4, 5), (3, 6), (1, 6), (0, 5), (0, 1), (1, 0)]],
    'P': [[(0, 6), (0, 0), (3, 0), (4, 1), (4, 2), (3, 3), (0, 3)]],
    'R': [[(0, 6), (0, 0), (3, 0), (4, 1), (4, 2), (3, 3), (0, 3)], [(2, 3), (4, 6)]],
    'S': [[(4, 1), (3, 0), (1, 0), (0, 1), (0, 2), (1, 3), (3, 3), (4, 4), (4, 5), (3, 6), (1, 6), (0, 5)]],
    'T': [[(0, 0), (4, 0)], [(2, 0), (2, 6)]],
    'V': [[(0, 0), (0, 4), (2, 6), (4, 4), (4, 0)]],
    'W': [[(0, 0), (0, 6), (2, 4), (4, 6), (4, 0)]],
    'X': [[(0, 0), (4, 6)], [(4, 0), (0, 6)]],
    'Y': [[(0, 0), (2, 3), (4, 0)], [(2, 3), (2, 6)]],
    'U': [[(0, 0), (0, 5), (1, 6), (3, 6), (4, 5), (4, 0)]],
    'Z': [[(0, 0), (4, 0), (0, 6), (4, 6)]],
    '0': [[(1, 0), (3, 0), (4, 1), (4, 5), (3, 6), (1, 6), (0, 5), (0, 1), (1, 0)]],
    '1': [[(1, 1), (2, 0), (2, 6)], [(1, 6), (3, 6)]],
    '2': [[(0, 1), (1, 0), (3, 0), (4, 1), (4, 2), (0, 6), (4, 6)]],
    '3': [[(0, 0), (4, 0), (2, 3), (3, 3), (4, 4), (4, 5), (3, 6), (1, 6), (0, 5)]],
    '4': [[(3, 6), (3, 0), (0, 4), (4, 4)]],
    '5': [[(4, 0), (0, 0), (0, 3), (3, 3), (4, 4), (4, 5), (3, 6), (0, 6)]],
    '6': [[(3, 0), (1, 0), (0, 1), (0, 5), (1, 6), (3, 6), (4, 5), (4, 4), (3, 3), (0, 3)]],
    '7': [[(0, 0), (4, 0), (1, 6)]],
    '8': [[(1, 3), (0, 2), (0, 1), (1, 0), (3, 0), (4, 1), (4, 2), (3, 3), (1, 3), (0, 4), (0, 5), (1, 6), (3, 6),
           (4, 5), (4, 4), (3, 3)]],
    '9': [[(4, 3), (1, 3), (0, 2), (0, 1), (1, 0), (3, 0), (4, 1), (4, 5), (3, 6), (1, 6)]],
    '-': [[(1, 3), (3, 3)]], '/': [[(0, 6), (4, 0)]],
    '%': [[(0, 6), (4, 0)], [(0, 0), (1, 0), (1, 1), (0, 1), (0, 0)], [(3, 5), (4, 5), (4, 6), (3, 6), (3, 5)]],
    ' ': [],
}
TYPES, LEVELS, CLOCKS = ('ROOM', 'PLATE', 'HALL'), ('LOW', 'MED', 'HIGH'), ('HIFI', 'STD', 'LOFI')
LED_X, LED_Y, LED_W, LED_H = (228, 242, 256), 12, 9, 18
SCOPE = (8.5, 36.5, 311.5, 116.5)                        # the scope's frame
BASE, TOP, XD, X1, PRE_PX = 108, 61, 18, 304, 56         # its axis, full scale; the dry hit, the tail's end, pre-delay span
SHIP_Y = 44                                              # the ship's top, at rest: room for 4 px either way
LABEL_Y, CAP_Y = 119, 130                                # the label row, the caption: everything above y 140
LABELS = ((10, 'MID', BLUE), (32, 'BASS', BLUE), (84, 'TREB', GREEN), (112, 'CLOCK', GREEN), (170, 'PRE-DLAY', WHITE),
          (222, 'PROGRAM', WHITE), (170, 'RATE', WHITE))   # 6: white's page 1 in LOFI, as the CXM 1978
CAP = {'type': 10, 's0': 46, 'diff': 56, 'dl': 86, 's1': 116, 'mod': 126, 'ml': 150, 's2': 180, 'clock': 190}
COVER_R = 282                                            # the cover's right edge: clear of the mix percent
MESSAGES = ('MU/TH/UR 6000', 'SPECIAL ORDER 937', 'CREW EXPENDABLE')
HULL = [(0, 11), (0, 0), (8, 0), (8, 3), (11, 3), (11, 1), (16, 1), (16, 3), (23, 3), (23, 1), (27, 1), (29, 3),
        (34, 4), (40, 7), (36, 9), (30, 10), (10, 10), (8, 11), (0, 11)]   # the Nostromo in profile, nose right, 40 x 12
DETAIL = (((1, 4), (7, 4)), ((1, 7), (7, 7)), ((30, 5), (34, 5)), ((13, 1), (13, -3)), ((14, 10), (13, 12)),
          ((28, 10), (29, 12)))                          # engine panels, bridge, mast, landing legs
NOZZLES = (6, 2, 9)                                      # the engines' rows: the core streak first


def tinv(i):                                             # the scope's log time axis: 0 .. 10 s, 250 ms .. 10 s a decade-ish
    return 250 * (2 ** (i / 64 * math.log2(41)) - 1)


def dim(col, f):
    return '#%02x%02x%02x' % tuple(round(int(col[i:i + 2], 16) * f) for i in (1, 3, 5))


def n(v):
    return ('%.2f' % v).rstrip('0').rstrip('.')       # no exponents


def width(s, adv=6, u=1):                                # pixels a text covers
    return adv * (len(s) - 1) + 4 * u + 1


def text(x, y, s, col, u=1, adv=6, w=1, anchor='start', idp=None):
    """u px per grid unit, top-left pixel (x, y): strokes on pixel centres. One element per stroke: the OP-1 draws
    a path's later M as a line to that point. idp: each element gets an id, idp + a, b, c ... (recoloured by the
    screen code: the colour must be one no other element has)"""
    if anchor == 'middle':
        x = round(x - width(s, adv, u) / 2)
    o = []
    for i, ch in enumerate(s):
        ox = x + i * adv
        for pl in F[ch]:
            p = [(ox + px * u + .5, y + py * u + .5) for px, py in pl]
            e = (line(*p[0], *p[1], col, w) if len(p) == 2 else
                 f'<path fill="none" stroke="{col}" stroke-width="{n(w)}" stroke-linejoin="miter" '
                 f'd="M{" L".join(f"{n(a)},{n(b)}" for a, b in p)}"/>')
            if idp: e = e.replace('<line ', f'<line id="{idp}{chr(97 + len(o))}" ', 1).replace('<path ', f'<path id="{idp}{chr(97 + len(o))}" ', 1)
            o.append(e)
    return ''.join(o)


def strokes(s):                                          # elements text() makes for s
    return sum(len(F[ch]) for ch in s)


def line(x1, y1, x2, y2, col, w=1):
    return (f'<line fill="none" stroke="{col}" stroke-width="{n(w)}" x1="{n(x1)}" y1="{n(y1)}" x2="{n(x2)}" '
            f'y2="{n(y2)}"/>')


def rect(x, y, w, h, col, fill='none'):
    s = f'stroke="{col}" stroke-width="1"' if col else 'stroke="none"'
    return f'<rect fill="{fill}" {s} x="{n(x)}" y="{n(y)}" width="{n(w)}" height="{n(h)}"/>'


def circle(cx, cy, r, col=None, fill='none'):
    s = f'stroke="{col}" stroke-width="1"' if col else 'stroke="none"'
    return f'<circle fill="{fill}" {s} cx="{n(cx)}" cy="{n(cy)}" r="{n(r)}"/>'


def chamfer(x0, y0, x1, y1, col, k=4):
    p = ((x0 + k, y0), (x1 - k, y0), (x1, y0 + k), (x1, y1 - k), (x1 - k, y1), (x0 + k, y1), (x0, y1 - k),
         (x0, y0 + k), (x0 + k, y0))
    return f'<polyline fill="none" stroke="{col}" stroke-width="1" points="{" ".join(f"{n(a)},{n(b)}" for a, b in p)}"/>'


def group(gid, parts):
    return f'<g id="{gid}">' + ''.join(parts) + '</g>'


def seg(s, x, y, w, h):
    m = h / 2
    return {'a': (x + 1, y, x + w - 1, y), 'b': (x + w, y + 1, x + w, y + m - 1), 'c': (x + w, y + m + 1, x + w, y + h - 1),
            'd': (x + 1, y + h, x + w - 1, y + h), 'e': (x, y + m + 1, x, y + h - 1), 'f': (x, y + 1, x, y + m - 1),
            'g': (x + 1, y + m, x + w - 1, y + m)}[s]


def main():
    o = [text(10, 15, 'NOSTROMO', WHITE, 2, 14, 1.5)]
    names = (('MID', BLUE), ('TREB', GREEN), ('PRE-DLAY', WHITE), ('MIX', RED), ('BASS', BLUE), ('CLOCK', GREEN), ('PROGRAM', WHITE))
    o += [group(f'nm{i}', [text(216 - width(s), 18, s, c)]) for i, (s, c) in enumerate(names)]   # the readout's control
    o.append(group('nm7', [text(216 - width('RATE'), 18, 'RATE', WHITE)]))                        # pre-delay's knob in LOFI
    o.append(rect(222.5, 8.5, 88, 26, dim(RED, .55)))       # LED readout: unlit segments faint, lit units only
    for x in LED_X:
        o += [line(*seg(s, x, LED_Y, LED_W, LED_H), dim(RED, .12), 1.6) for s in 'abcdefg']
    for d, x in enumerate(LED_X):
        o += [group(f's{d}{s}', [line(*seg(s, x, LED_Y, LED_W, LED_H), RED, 1.6)]) for s in 'abcdefg']
    for i in range(2):
        o.append(group(f'dp{i}', [circle(LED_X[i] + LED_W + 2.5, LED_Y + LED_H, 1.3, fill=RED)]))
    for i, u in enumerate(('SEC', 'MS', 'HZ', 'KHZ')):
        o.append(group(f'u{i}', [text((270, 292)[i % 2], (12, 22)[i // 2], u, RED)]))
    o.append(chamfer(*SCOPE, dim(WHITE, .35)))              # the scope: frame, -20 / -40 dB rules, the time axis
    H = BASE - TOP
    o += [line(80, round(BASE - H * k / 3) + .5, 306, round(BASE - H * k / 3) + .5, dim(GREY, .3)) for k in (1, 2)]
    o.append(line(14, BASE + .5, 306, BASE + .5, dim(GREY, .7)))
    o.append(group('sh', [f'<polyline fill="none" stroke="{WHITE}" stroke-width="1" points="'
                          + ' '.join(f'{n(a + .5)},{n(b + .5)}' for a, b in HULL) + '"/>']
                   + [line(a + .5, b + .5, c + .5, d + .5, WHITE) for (a, b), (c, d) in DETAIL]))
    o += [group(f'sr{i}', [rect(0, 0, 2, 2, None, '#f0f0f%x' % i).replace('<rect ', f'<rect id="sf{i}" ')]) for i in range(6)]   # recoloured: twinkle
    counts = []                                              # the labels: one copy, a colour of their own
    for i, (x, s, c) in enumerate(LABELS):
        r, g, b = (int(c[k:k + 2], 16) for k in (1, 3, 5))
        t = text(x, LABEL_Y, s, '#%02x%02x%02x' % (r, g, b - 1 - i), idp=f'l{i}')
        o.append(group({4: 'lp', 6: 'lr'}[i], [t]) if i in (4, 6) else t)   # PRE-DLAY / RATE: by the clock
        counts.append(strokes(s))
    o.append(text(310 - width('MIX'), LABEL_Y, 'MIX', RED))
    cp = [text(CAP[k], CAP_Y, '/', GREY) for k in ('s0', 's1', 's2')]
    cp += [text(CAP['diff'], CAP_Y, 'DIFF', dim(WHITE, .8)), text(CAP['mod'], CAP_Y, 'MOD', dim(WHITE, .8))]
    for gid, k, words, c in (('wa', 'type', TYPES, WHITE), ('wb', 'dl', LEVELS, WHITE), ('wc', 'ml', LEVELS, WHITE),
                             ('wd', 'clock', CLOCKS, GREEN)):
        cp += [group(f'{gid}{i}', [text(CAP[k], CAP_Y, w, c)]) for i, w in enumerate(words)]
    o.append(group('cp', cp))
    o += [group(gid, [text(10, CAP_Y, m, WHITE)]) for gid, m in zip(('bt', 'e0', 'e1'), MESSAGES)]
    p0 = 310 - width('100%')                                 # the mix percent, right-aligned
    o += [group('xh1', [text(p0, CAP_Y, '1', dim(RED, .7))]), text(p0 + 18, CAP_Y, '%', dim(RED, .7))]
    for d in range(10):
        o += [group(f'xt{d}', [text(p0 + 6, CAP_Y, str(d), dim(RED, .7))]),
              group(f'xo{d}', [text(p0 + 12, CAP_Y, str(d), dim(RED, .7))])]
    o.append(group('cv', [rect(10, CAP_Y - 1, COVER_R - 10, 9, None, BLACK)]))      # last: over the caption
    o.append(group('cu', [rect(0, CAP_Y, 4, 7, None, dim(WHITE, .8))]))
    o = ['<?xml version="1.0" encoding="utf-8"?>',
         '<svg version="1.1" xmlns="http://www.w3.org/2000/svg" x="0px" y="0px" width="320px" height="160px" '
         'viewBox="0 0 320 160">',
         '<!-- generated by plugins/nostromo/gen_svg.py - the nostromo FX screen, animated by nostromo.c -->',
         *o, '</svg>']
    open(os.path.join(HERE, 'nostromo.svg'), 'w').write('\n'.join(o) + '\n')
    ends = [CAP['clock'] + width(w) + 2 for w in CLOCKS] + [10 + width(m) + 2 for m in MESSAGES]
    h = ['/* generated by plugins/nostromo/gen_svg.py - geometry of nostromo.svg */',
         f'#define NS_BASE {BASE}', f'#define NS_TOP {TOP}', f'#define NS_XD {XD}', f'#define NS_X1 {X1}',
         f'#define NS_PRE_PX {PRE_PX}', f'#define NS_SHIP_Y {SHIP_Y}', f'#define NS_SHIP_W {max(a for a, _ in HULL)}',
         f'#define NS_CAP_X 10', f'#define NS_COVER_R {COVER_R}', f'#define NS_LABELS {len(LABELS)}', f'#define NS_LAB_MAX {max(counts)}',
         'static const uint8_t ns_lab_n[NS_LABELS] = { ' + ', '.join(map(str, counts)) + ' };   /* elements per label */',
         'static const uint8_t ns_lab_rgb[NS_LABELS][3] = { ' + ', '.join(
             '{ 0x%s, 0x%s, 0x%s }' % (c[1:3], c[3:5], c[5:7]) for _, _, c in LABELS) + ' };',
         'static const uint16_t ns_cap_end[6] = { ' + ', '.join(map(str, ends)) + ' };   /* the cursor after HIFI, STD, LOFI; the messages */',
         'static const uint8_t ns_nozzle[3] = { ' + ', '.join(map(str, NOZZLES)) + ' };',
         '/* the scope\'s log time axis: ms x 8 at 64ths of its width (0 .. 10 s) */',
         'static const int32_t ns_tinv[65] = { ' + ', '.join(str(round(8 * tinv(i))) for i in range(65)) + ' };']
    open(os.path.join(HERE, 'nostromo_geom.h'), 'w').write('\n'.join(h) + '\n')
    print('-> nostromo.svg, nostromo_geom.h')


if __name__ == '__main__':
    main()
