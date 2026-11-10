#!/usr/bin/env python3
"""Host mock-up of the drum+ pages (docs/31) for approval: T1, T1 + Shift, T2, T2 + Shift.
Usage: drumplus_preview.py <out.png>   (text in Iosevka, harness/iosevka.py)"""
import math, os, sys
import cairocffi as cairo
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'harness'))
import iosevka                                      # preview font (Iosevka, OFL)

BLUE, GREEN, WHITE, RED, GREY, LILAC, PALE = '#4D9EFF', '#00ED95', '#FFFFFF', '#FF3A5D', '#6F7173', '#DFD9FF', '#AEB1DC'
W, H, S = 320, 160, 2

def rgb(h): return tuple(int(h[i:i + 2], 16) / 255 for i in (1, 3, 5))

def line(c, pts, col, w=1.0):
    c.set_source_rgb(*rgb(col)); c.set_line_width(w); c.set_line_cap(cairo.LINE_CAP_BUTT)
    c.move_to(*pts[0])
    for p in pts[1:]: c.line_to(*p)
    c.stroke()

def dot(c, x, y, col, r=1.5):                      # the rasteriser drops zero-length lines: two crossing segments
    line(c, [(x - r, y), (x + r, y)], col, 1.5); line(c, [(x, y - r), (x, y + r)], col, 1.5)



def text(c, s, x, y, col, h=7.0):                  # y = baseline, h = cap height; stock font on the device: Iosevka here (capitals)
    s = s.upper(); em = h / 0.735
    c.set_source_rgb(*rgb(col)); c.move_to(x, y); iosevka.show(c, s, em); return x + iosevka.width(s, em) + 2.2

def wave(c, x0, x1, y, amp):                        # the pad's region (read-only): a decaying hit
    for i, x in enumerate(range(x0, x1, 2)):
        t = i / ((x1 - x0) / 2)
        a = amp * math.exp(-4 * t) * (0.55 + 0.45 * abs(math.sin(i * 1.7) * math.cos(i * 0.37)))
        line(c, [(x, y - max(a, 0.6)), (x, y + max(a, 0.6))], GREY, 1.0)

def header(c, shift):
    text(c, 'pad 8', 8, 13, WHITE)
    text(c, 'drum+', 268, 13, PALE)
    if shift: text(c, 'shift', 140, 13, GREY)

def labels(c, items):                               # bottom row: 4 encoders (label, value, press state)
    cols = (BLUE, GREEN, WHITE, RED)
    for i, (lab, val, press) in enumerate(items):
        x = 8 + i * 78
        line(c, [(x, 112), (x + 70, 112)], cols[i], 2)
        text(c, lab, x, 124, cols[i]); text(c, val, x, 136, WHITE)
        if press: text(c, press, x + 40, 136, GREY)

X0, X1, MID = 20, 300, 62                           # waveform area: x 20..300, centre line y 62 (+-36)

def t1(c, shift):
    header(c, shift); wave(c, X0, X1, MID, 34)
    # pitch envelope (green): +12 st at the start, gliding to 0 over the glide time
    pts = [(X0 + i * 2, MID - 30 * max(0.0, 1 - i * 2 / 90)) for i in range(0, 60)]
    line(c, [(X0, MID), (X1, MID)], '#2A2B2D', 1)
    line(c, pts, GREEN, 1.5)
    # retrig: 4 hits at 1/16 (white ticks under the waveform, falling level)
    for k in range(4):
        x = X0 + k * 64; hgt = 10 * (0.79 ** k)
        line(c, [(x, 104 - hgt), (x, 104)], WHITE, 2)
    # choke group + mono, drive meter (red, right edge)
    text(c, 'c1', 278, 30, WHITE); text(c, 'mono', 270, 44, RED)
    line(c, [(306, 98), (306, 98 - 40)], '#3A2228', 3); line(c, [(306, 98), (306, 98 - 16)], RED, 3)
    if not shift:
        labels(c, [('pitch', '+3', 'rev'), ('p env', '+12', 'loop'), ('retrig', '4', 'c1'), ('drive', '40', 'mono')])
    else:
        labels(c, [('fine', '+12c', ''), ('p time', '80ms', ''), ('rate', '1/16', ''), ('volume', '80', '')])

def t2(c, shift):
    header(c, shift); wave(c, X0, X1, MID, 34)
    # envelope over the region: attack (blue), hold (lilac), decay (green, exp), release (white, after key-up)
    a, h, d = 14, 10, 150
    base, top = MID + 36, MID - 36
    line(c, [(X0, base), (X0 + a, top)], BLUE, 1.5)
    line(c, [(X0 + a, top), (X0 + a + h, top)], LILAC, 1.5)
    dec = [(X0 + a + h + i * 3, base - (base - top) * math.exp(-3.2 * i * 3 / d)) for i in range(0, 34)]
    line(c, dec, GREEN, 1.5)
    rel0 = dec[-1]
    line(c, [rel0, (rel0[0] + 40, base)], WHITE, 1.5)
    # filter (red): small response at the right, low pass with a resonance bump; cutoff marker
    fx0, fx1, fy = 236, 300, 98
    resp = []
    for i in range(0, 33):
        f = i / 32; x = fx0 + f * (fx1 - fx0)
        g = 1 / math.sqrt((1 - (f / 0.55) ** 2) ** 2 + (f / 0.55 / 2.2) ** 2) if f < 1 else 0
        resp.append((x, fy - min(18, 9 * g if f < 0.55 else 9 * g * (0.55 / f) ** 2)))
    line(c, resp, RED, 1.5)
    if not shift:
        labels(c, [('attack', '2ms', 'env'), ('decay', '240ms', 'exp'), ('release', '80ms', 'lp'), ('cutoff', '1.2k', 'v>st')])
    else:
        labels(c, [('vel>pitch', '+2', ''), ('human', '20', ''), ('reso', '60', ''), ('vel>cut', '+30', '')])

def main(out):
    pad, lab = 12, 20
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, 2 * (W * S + pad) + pad, 2 * (H * S + pad + lab) + pad)
    c = cairo.Context(surf); c.set_source_rgb(0.25, 0.25, 0.27); c.paint()
    views = [('t1 play', t1, False), ('t1 + shift', t1, True), ('t2 envelope and tone', t2, False), ('t2 + shift', t2, True)]
    for n, (title, fn, sh) in enumerate(views):
        x0, y0 = pad + (n % 2) * (W * S + pad), pad + (n // 2) * (H * S + pad + lab)
        c.save(); c.set_source_rgb(1, 1, 1); c.set_font_size(14); c.move_to(x0, y0 + 15); c.show_text(title); c.restore()
        c.save(); c.translate(x0, y0 + lab); c.rectangle(0, 0, W * S, H * S); c.clip()
        c.set_source_rgb(0, 0, 0); c.paint(); c.scale(S, S); fn(c, sh); c.restore()
    surf.write_to_png(out); print('->', out)

if __name__ == '__main__':
    main(sys.argv[-1])
