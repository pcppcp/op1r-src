#!/usr/bin/env python3
"""Render the perc drum kit's pages from plugins/percdrum/perc_drum.c (host build, percdrum_kit_test render) to a PNG.
Usage: render_percdrum.py <out.png> (text in Iosevka, harness/iosevka.py). The grey frame marks the corners the
bezel may hide: nothing should need them."""
import json, os, subprocess, sys
import cairocffi as cairo
HERE = os.path.dirname(os.path.abspath(__file__)); SDK = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import iosevka
TEXT_H = 8.0

def main(out):
    exe = os.path.join(HERE, 'build', 'percdrum_kit_test')
    subprocess.run(['cc', '-O1', '-DOP1_HOST', '-I', SDK, '-I', HERE, '-I', os.path.join(SDK, 'plugins', 'percsyn'),
                    os.path.join(HERE, 'percdrum_kit_test.c'), os.path.join(SDK, 'plugins', 'percsyn', 'perc_dsp.c'),
                    '-lm', '-o', exe], check=True, stderr=subprocess.DEVNULL)
    lines = subprocess.run([exe, 'render'], check=True, capture_output=True, text=True).stdout.splitlines()
    frames = [json.loads(l) for l in lines if l.startswith('{')]
    S, W, H, pad, lab, cols = 2, 640, 320, 12, 20, 2
    rows = (len(frames) + cols - 1) // cols
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, cols * (W + pad) + pad, rows * (H + pad + lab) + pad)
    c = cairo.Context(surf); c.set_source_rgb(0.25, 0.25, 0.27); c.paint()
    for n, fr in enumerate(frames):
        x0, y0 = pad + (n % cols) * (W + pad), pad + (n // cols) * (H + pad + lab)
        c.save(); c.set_source_rgb(1, 1, 1); c.set_font_size(14); c.move_to(x0, y0 + 15); c.show_text(fr['title']); c.restore()
        c.save(); c.translate(x0, y0 + lab); c.rectangle(0, 0, W, H); c.clip(); c.set_source_rgb(0, 0, 0); c.paint(); c.scale(S, S)
        c.set_line_cap(cairo.LINE_CAP_BUTT)
        for a, b, x1, y1, w, col in fr['lines']:
            c.set_source_rgb(*(v / 255 for v in col[:3])); c.set_line_width(w / 1024)
            c.move_to(a / 1024, b / 1024); c.line_to(x1 / 1024, y1 / 1024); c.stroke()
        for t in fr['text']:
            c.set_source_rgb(*(v / 255 for v in t['c'])); c.move_to(t['x'] / 1024, t['y'] + TEXT_H)
            iosevka.show(c, t['s'].upper(), TEXT_H / 0.735)
        c.restore()
    surf.write_to_png(out); print('->', out)

if __name__ == '__main__':
    main(sys.argv[1])
