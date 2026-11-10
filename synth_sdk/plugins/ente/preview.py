#!/usr/bin/env python3
"""Screen previews of ENTE: builds harness/ente_test.c and draws its screen states (the real screen code's lines) with
render.py's SVG painter.
    python3 plugins/ente/preview.py [out.png]       -> plugins/ente/ente_preview.png"""
import json, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(SDK, 'harness'))
import render                                                    # noqa: E402
import xml.etree.ElementTree as ET                               # noqa: E402

STATES = ['T1, no reduction', 'SELF, -9 dB', 'BEAT 1/8, full duck (24 dB)', 'IN, -5 dB, white page 2 (HP)',
          'TAPE, -15 dB, blue page 2 (attack)', 'T3, green page 2 (makeup)']

def main(out=None):
    exe = os.path.join(SDK, 'harness', 'build', 'ente_prev')
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    subprocess.run(['cc', '-O1', '-DOP1_HOST', '-I', SDK, '-I', os.path.join(SDK, 'harness'),
                    os.path.join(SDK, 'harness', 'ente_test.c'), '-lm', '-o', exe], check=True)
    svg = ET.parse(os.path.join(HERE, 'ente.svg')).getroot()
    sc, pad, lab = render.SCALE, 12, 22
    W, H, cols = 320 * sc, 160 * sc, 2
    rows = (len(STATES) + 1) // 2
    cairo = render.cairo
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, cols * (W + pad) + pad, rows * (H + pad + lab) + pad)
    ctx = cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
    for n, label in enumerate(STATES):
        fr = json.loads(subprocess.run([exe, 'screen', str(n)], check=True, capture_output=True, text=True).stdout)
        x0, y0 = pad + (n % cols) * (W + pad), pad + (n // cols) * (H + pad + lab)
        ctx.save(); ctx.set_source_rgb(1, 1, 1); ctx.set_font_size(14); ctx.move_to(x0, y0 + 15)
        ctx.show_text(f'{label}   ({len(fr["lines"])} lines)'); ctx.restore()
        ctx.save(); ctx.translate(x0, y0 + lab); ctx.rectangle(0, 0, W, H); ctx.clip()
        ctx.set_source_rgb(0, 0, 0); ctx.paint(); ctx.scale(sc, sc)
        render.draw_svg(ctx, svg, {}, set())
        for x1, y1, x2, y2, w, c in fr['lines']:
            ctx.set_source_rgba(*(v / 255 for v in c)); ctx.set_line_width(w / 1024)
            ctx.set_line_cap(cairo.LINE_CAP_BUTT)
            ctx.move_to(x1 / 1024, y1 / 1024); ctx.line_to(x2 / 1024, y2 / 1024); ctx.stroke()
        for x, y, size, c, txt in fr.get('texts', []):           # stock font on the device: a stand-in here
            ctx.set_source_rgb(*(v / 255 for v in c[:3])); ctx.move_to(x / 1024, y + size * 0.72)
            render.iosevka.show(ctx, txt.upper(), size * 0.78)   # stock font on the device: Iosevka here
        ctx.restore()
    out = out or os.path.join(HERE, 'ente_preview.png')
    surf.write_to_png(out); print('->', out)

if __name__ == '__main__':
    main(*sys.argv[1:])
