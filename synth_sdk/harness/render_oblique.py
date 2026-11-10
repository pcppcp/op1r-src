#!/usr/bin/env python3
"""Preview of the Oblique Strategies card (screens/oblique.c via harness/oblique_test.c). The firmware text is
drawn in Iosevka (harness/iosevka.py) at the mock's per-character x.
usage: render_oblique.py <out.png> [card indices...]"""
import json, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); SDK = os.path.dirname(HERE)
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(SDK, 'display'))
import render as R, gen_oblique as G

def main(out, cards):
    exe = os.path.join(HERE, 'build', 'oblique_test')
    frames = [json.loads(l) for l in subprocess.run([exe, 'render'] + cards, check=True, capture_output=True, text=True).stdout.splitlines()]
    cairo = R.cairo; S, W, H, pad = 2, 640, 320, 12
    cols = 2; rows = (len(frames) + 1) // 2
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, cols * (W + pad) + pad, rows * (H + pad) + pad)
    ctx = cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
    for n, fr in enumerate(frames):
        x0, y0 = pad + (n % cols) * (W + pad), pad + (n // cols) * (H + pad)
        ctx.save(); ctx.translate(x0, y0); ctx.rectangle(0, 0, W, H); ctx.clip(); ctx.set_source_rgb(0, 0, 0); ctx.paint(); ctx.scale(S, S)
        ctx.set_line_cap(cairo.LINE_CAP_BUTT)
        for t in fr['text']:                          # stock font on the device: Iosevka here, at the mock's per-character x
            ctx.set_source_rgb(*(v / 255 for v in t['c']))
            for ch, x in zip(t['s'].upper(), t['pos']):
                ctx.move_to(x, t['y'] + G.TEXT_H); R.iosevka.show(ctx, ch, G.TEXT_H / 0.735)
        for a, b, c, d, w, col in fr['lines']:
            ctx.set_source_rgba(*(v / 255 for v in col)); ctx.set_line_width(w / 1024)
            ctx.move_to(a / 1024, b / 1024); ctx.line_to(c / 1024, d / 1024); ctx.stroke()
        ctx.restore()
    surf.write_to_png(out); print('->', out)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2:])
