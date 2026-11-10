#!/usr/bin/env python3
"""Screen previews of "in a room" in states the stock harness/render.py cannot reach (it never presses the latch and
has no beat clock): builds harness/sitting_test.c, which runs the FX on a speech-like input with a modelled OP-1 beat
clock, and draws each state's frame with render.py's SVG painter.

    python -I plugins/sitting/preview.py [out.png]       -> plugins/sitting/sitting_preview.png
"""
import json, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(SDK, 'harness'))
import render                                                    # noqa: E402  (harness/render.py: draw_svg, cairo)
import xml.etree.ElementTree as ET                               # noqa: E402

STATES = ['idle, blue page 2: SYNC 1 BAR', 'free loop GEN 05, armed for 1 BAR (old loop keeps playing)',
          'synced 1 BAR: recording', 'synced 1 BAR at 120 BPM: GEN 08',
          '2 BARS at 120 BPM (11.025 kHz), golden room: GEN 24', 'free 2.6 s loop GEN 03, blue page 2',
          '1/16 (1/4 beat), small cube, green page 2 (absorb)', '1/2 (2 beats), bolt, max erosion: GEN 40',
          'idle at 50 BPM, blue page 2: SYNC 1 BAR LO-FI', 'idle, tape at 25 %: SYNC 1 BAR AS 1/2',
          'free 7.5 s (11.025 kHz) GEN 01, blue page 2']


def main(out=None):
    exe = os.path.join(SDK, 'harness', 'build', 'sitting_test')
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    subprocess.run(['cc', '-O1', '-DOP1_HOST', '-fsanitize=undefined', '-I', SDK,
                    os.path.join(SDK, 'harness', 'sitting_test.c'), '-lm', '-o', exe], check=True)
    svg = ET.parse(os.path.join(HERE, 'lucierroom.svg')).getroot()
    sc, pad, lab = render.SCALE, 12, 22
    W, H, cols = 320 * sc, 160 * sc, 2
    rows = (len(STATES) + 1) // 2
    cairo = render.cairo
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, cols * (W + pad) + pad, rows * (H + pad + lab) + pad)
    ctx = cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
    for n, label in enumerate(STATES):
        fr = json.loads(subprocess.run([exe, 'screen', str(n)], check=True, capture_output=True, text=True).stdout)
        x0, y0 = pad + (n % cols) * (W + pad), pad + (n // cols) * (H + pad + lab)
        ctx.save(); ctx.set_source_rgb(1, 1, 1); ctx.set_font_size(14)
        ctx.move_to(x0, y0 + 15); ctx.show_text(f'{label}   knobs {fr["knobs"]}'); ctx.restore()
        ctx.save(); ctx.translate(x0, y0 + lab); ctx.rectangle(0, 0, W, H); ctx.clip()
        ctx.set_source_rgb(0, 0, 0); ctx.paint(); ctx.scale(sc, sc)
        render.draw_svg(ctx, svg, {}, set())
        for x1, y1, x2, y2, w, c in fr['lines']:
            ctx.set_source_rgba(*(v / 255 for v in c)); ctx.set_line_width(w / 1024)
            ctx.set_line_cap(cairo.LINE_CAP_BUTT)
            ctx.move_to(x1 / 1024, y1 / 1024); ctx.line_to(x2 / 1024, y2 / 1024); ctx.stroke()
        ctx.restore()
        print(f'{label:58s} lines={len(fr["lines"])}')
    out = out or os.path.join(HERE, 'sitting_preview.png')
    surf.write_to_png(out); print('->', out)


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else None)
