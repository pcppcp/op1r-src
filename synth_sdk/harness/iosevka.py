"""Iosevka 34.9.0 (SIL OFL 1.1, fonts/Iosevka-OFL.md) for the host previews: the op1-design skill's open preview font
in place of the stock font (never in previews) and of DejaVu. Glyph outlines from fonts/iosevka_glyphs.json
(fonts/gen_iosevka.py), drawn as cairo paths.
    show(ctx, text, em)  at the current point (baseline), as cairo's show_text with set_font_size(em)
    width(text, em)      advance width
The weight follows the skill's size mapping (PREVIEW-FONT.md): the stock text size em / 0.78 (the renderers' old
stand-in scale) -> XS Regular 400, S Light 300, M / L ExtraLight 200, XL / XXL Thin 100."""
import json, os

_G = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fonts', 'iosevka_glyphs.json')))
_UPM = _G['unitsPerEm']

def weight(em):
    s = em / 0.78                                     # the stock text size (op1_text size) the preview stands for
    return 400 if s < 15 else 300 if s < 25 else 200 if s < 40 else 100

def width(text, em, w=None):
    g = _G['weights'][str(w or weight(em))]
    return sum(g.get(c, g['?'])[0] for c in text) * em / _UPM

def show(ctx, text, em, w=None):
    g = _G['weights'][str(w or weight(em))]
    k = em / _UPM
    x, y = ctx.get_current_point()
    ctx.new_path()
    for c in text:
        adv, ops = g.get(c, g['?'])
        px = py = 0.0
        for o in ops:
            op, v = o[0], o[1:]
            P = lambda i: (x + v[i] * k, y - v[i + 1] * k)
            if op == 'M': ctx.move_to(*P(0)); px, py = P(0)
            elif op == 'L': ctx.line_to(*P(0)); px, py = P(0)
            elif op == 'Q':                            # quadratic -> cubic
                (cx, cy), (ex, ey) = P(0), P(2)
                ctx.curve_to(px + 2 / 3 * (cx - px), py + 2 / 3 * (cy - py), ex + 2 / 3 * (cx - ex), ey + 2 / 3 * (cy - ey), ex, ey)
                px, py = ex, ey
            elif op == 'C': ctx.curve_to(*P(0), *P(2), *P(4)); px, py = P(4)
            elif op == 'Z': ctx.close_path()
        x += adv * k
    ctx.fill()
    ctx.move_to(x, y)

if __name__ == '__main__':                            # self-check: widths scale, weights by size, a PNG renders
    assert abs(width('AB', 20) - 2 * width('A', 20)) < 1e-9 and width('A', 40) == 2 * width('A', 20)
    assert weight(12 * 0.78) == 400 and weight(16 * 0.78) == 300 and weight(30 * 0.78) == 200 and weight(48 * 0.78) == 100
    try: import cairo
    except ImportError: import cairocffi as cairo
    s = cairo.ImageSurface(cairo.FORMAT_RGB24, 300, 60); c = cairo.Context(s); c.set_source_rgb(1, 1, 1)
    c.move_to(4, 24); show(c, 'PULSE SYNC 120.0', 12 * 0.78); c.move_to(4, 56); show(c, '120.0', 30 * 0.78)
    s.write_to_png('/tmp/iosevka_selftest.png'); print('iosevka: ok -> /tmp/iosevka_selftest.png')
