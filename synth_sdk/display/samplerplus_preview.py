#!/usr/bin/env python3
"""Host mock-up of the sampler+ pages (docs/46) for approval: T1, T1 + Shift, T2, T2 + Shift, sample browser.
Usage: samplerplus_preview.py <out.png>   (text in Iosevka via drumplus_preview.text)"""
import math, os, sys
import cairocffi as cairo
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from drumplus_preview import BLUE, GREEN, WHITE, RED, GREY, LILAC, PALE, W, H, S, rgb, line, dot
import drumplus_preview as dpv

def text(c, s, x, y, col, h=7.0): return dpv.text(c, s, x, y, col, h)

X0, X1, MID, AMP = 14, 306, 58, 30                   # waveform: the whole 6 s buffer

def sample(i, n):                                     # a sustained tone with an attack, then silence
    t = i / n
    if t > 0.78: return 0.0
    env = min(1.0, t * 30) * (0.75 + 0.25 * math.cos(t * 9))
    return env * (0.6 + 0.4 * abs(math.sin(i * 0.9) * math.cos(i * 0.23)))

def wave(c, start, end):
    n = (X1 - X0) // 2
    for i in range(n):
        x = X0 + i * 2; a = AMP * sample(i, n)
        col = GREY if start <= x <= end else '#3A3B3D'
        line(c, [(x, MID - max(a, 0.6)), (x, MID + max(a, 0.6))], col, 1.0)

def header(c, shift, title='acidrone'):
    text(c, title, 8, 13, WHITE)
    text(c, 'sampler+', 252, 13, PALE)
    if shift: text(c, 'shift', 120, 13, GREY)

def labels(c, items):
    cols = (BLUE, GREEN, WHITE, RED)
    for i, (lab, val, press) in enumerate(items):
        x = 8 + i * 78
        line(c, [(x, 112), (x + 70, 112)], cols[i], 2)
        text(c, lab, x, 124, cols[i]); text(c, val, x, 136, WHITE)
        if press: text(c, press, x + 38, 136, GREY)

def t1(c, shift):
    header(c, shift)
    st, ls, le, en = 30, 120, 196, 236                # region and loop (edited in the sample editor: read-only here)
    wave(c, st, en)
    line(c, [(st, MID - 34), (st, MID + 34)], WHITE, 1)          # start / end
    line(c, [(en, MID - 34), (en, MID + 34)], WHITE, 1)
    line(c, [(ls, MID + 34), (le, MID + 34)], LILAC, 2)          # loop span (lilac, as the editor)
    line(c, [(ls, MID - 34), (ls, MID + 34)], LILAC, 1)
    line(c, [(le, MID - 34), (le, MID + 34)], LILAC, 1)
    line(c, [(le - 22, MID + 30), (le, MID + 30)], BLUE, 3)      # crossfade zone before the loop end
    line(c, [(ls + 4, MID + 40), (le - 4, MID + 40)], BLUE, 1)   # ping-pong: arrows both ways
    line(c, [(ls + 4, MID + 40), (ls + 8, MID + 37)], BLUE, 1); line(c, [(ls + 4, MID + 40), (ls + 8, MID + 43)], BLUE, 1)
    line(c, [(le - 4, MID + 40), (le - 8, MID + 37)], BLUE, 1); line(c, [(le - 4, MID + 40), (le - 8, MID + 43)], BLUE, 1)
    for x in (148, 171):                                        # playing voices (red, as the sample editor)
        line(c, [(x, MID - 30), (x, MID + 30)], RED, 1)
    text(c, '1 bar', 200, 13, WHITE)                            # tempo sync (white)
    pts = [(st + i * 2, MID - 32 + 26 * min(1, i / 18)) for i in range(0, 20)]   # pitch envelope (green)
    line(c, pts, GREEN, 1.5)
    line(c, [(308, 100), (308, 60)], '#3A2228', 3); line(c, [(308, 100), (308, 86)], RED, 3)   # drive meter
    if not shift:
        labels(c, [('xfade', '40ms', 'pong'), ('p env', '+12', 'rel'), ('sync', '1 bar', 'file'), ('drive', '40', 'rev')])
    else:
        labels(c, [('rnd st', '20', ''), ('p time', '80ms', ''), ('vel>st', '30', ''), ('gain', '-3db', '')])

def t2(c, shift):
    header(c, shift)
    x0, base, top = 20, 100, 30
    a, h, d, sus, s_len, r = 18, 10, 50, 0.55, 90, 60
    ys = base - (base - top) * sus
    line(c, [(x0, base), (x0 + a, top)], BLUE, 1.5)                                # attack
    line(c, [(x0 + a, top), (x0 + a + h, top)], LILAC, 1.5)                        # hold
    dec = [(x0 + a + h + i * 2, ys + (top - ys) * math.exp(-4 * i * 2 / d)) for i in range(0, d // 2 + 1)]
    line(c, dec, GREEN, 1.5)                                                       # decay (exp)
    xs = dec[-1][0]
    line(c, [(xs, ys), (xs + s_len, ys)], WHITE, 1.5)                              # sustain (held)
    rel = [(xs + s_len + i * 2, ys * 0 + base - (base - ys) * math.exp(-4 * i * 2 / r)) for i in range(0, r // 2 + 1)]
    line(c, rel, RED, 1.5)                                                         # release
    line(c, [(xs + s_len, ys - 6), (xs + s_len, ys + 6)], GREY, 1)                 # key-up
    # filter (right): low pass + resonance, cutoff moved by the envelope (dotted: envelope peak)
    fx0, fx1, fy = 236, 306, 50
    def resp(fc, col, w):
        pts = []
        for i in range(0, 36):
            f = i / 35; x = fx0 + f * (fx1 - fx0)
            g = 1 / math.sqrt((1 - (f / fc) ** 2) ** 2 + (f / fc / 2.0) ** 2)
            pts.append((x, fy - min(16, 8 * g)))
        line(c, pts, col, w)
    resp(0.35, GREY, 1); resp(0.6, PALE, 1.5)
    text(c, 'lp', fx0, 28, PALE)
    if not shift:
        labels(c, [('attack', '6ms', 'exp'), ('decay', '240ms', 'hold'), ('sustain', '55', 'lp'), ('release', '300ms', 'vel')])
    else:
        labels(c, [('cutoff', '1.2k', ''), ('reso', '60', ''), ('env>cut', '+40', ''), ('vel>cut', '+20', '')])

def browser(c, shift):
    header(c, False, 'samples')
    names = ['acidrone', 'designadrum', 'dist a log', 'knorr', 'leader', 'm-glide']
    for i, n in enumerate(names):
        y = 30 + i * 13
        if i == 2:
            c.set_source_rgb(*rgb('#00593A')); c.rectangle(18, y - 9, 150, 12); c.fill()
        text(c, n, 22, y, GREEN if i != 2 else WHITE)
    text(c, 'factory', 186, 30, GREY); text(c, 'user', 186, 43, GREY)
    line(c, [(180, 22), (180, 96)], GREY, 1); line(c, [(180, 44), (180, 58)], GREEN, 2)   # scroll bar
    labels(c, [('sample', '3/24', 'load'), ('', '', ''), ('', '', ''), ('', '', 'back')])

def main(out):
    pad, lab = 12, 20
    views = [('t1 play', t1, False), ('t1 + shift', t1, True), ('t2 envelope and filter', t2, False),
             ('t2 + shift', t2, True), ('t1 white press: sample browser (sampler presets)', browser, False)]
    rows = (len(views) + 1) // 2
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, 2 * (W * S + pad) + pad, rows * (H * S + pad + lab) + pad)
    c = cairo.Context(surf); c.set_source_rgb(0.25, 0.25, 0.27); c.paint()
    for n, (title, fn, sh) in enumerate(views):
        x0, y0 = pad + (n % 2) * (W * S + pad), pad + (n // 2) * (H * S + pad + lab)
        c.save(); c.set_source_rgb(1, 1, 1); c.set_font_size(14); c.move_to(x0, y0 + 15); c.show_text(title); c.restore()
        c.save(); c.translate(x0, y0 + lab); c.rectangle(0, 0, W * S, H * S); c.clip()
        c.set_source_rgb(0, 0, 0); c.paint(); c.scale(S, S); fn(c, sh); c.restore()
    surf.write_to_png(out); print('->', out)

if __name__ == '__main__':
    main(sys.argv[-1])
