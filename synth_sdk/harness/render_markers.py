#!/usr/bin/env python3
"""Preview of the tape markers over the stock tape screen (stock tape.svg from the unpacked firmware: keep the
output out of the repo). Usage: render_markers.py <op1_246/content/display/tape.svg> <out.png>"""
import json, os, subprocess, sys, xml.etree.ElementTree as ET
HERE = os.path.dirname(os.path.abspath(__file__)); SDK = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import render as R

def main(svg_path, out):
    exe = os.path.join(HERE, 'build', 'markers_test')
    subprocess.run(['cc', '-O1', '-Wall', '-DOP1_HOST', '-I', SDK, os.path.join(HERE, 'markers_test.c'), '-o', exe], check=True)
    frames = [('markers, punch off', 335160, 40, 0), ('punch armed: red lines, [o]', 335160, 40, 1),
              ('punched in', 335160, 40, 2), ('cursor on a marker, punch armed', 264600, 40, 1)]
    svg = ET.parse(svg_path).getroot()
    cairo = R.cairo; S, W, H, pad, lab = 2, 640, 320, 12, 22
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, 2 * (W + pad) + pad, 2 * (H + pad + lab) + pad)
    ctx = cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
    for n, (label, cur, zoom, pm) in enumerate(frames):
        lines = json.loads(subprocess.run([exe, 'render', str(cur), str(zoom), str(pm)], check=True, capture_output=True, text=True).stdout)
        x0, y0 = pad + (n % 2) * (W + pad), pad + (n // 2) * (H + pad + lab)
        ctx.save(); ctx.set_source_rgb(1, 1, 1); ctx.set_font_size(14); ctx.move_to(x0, y0 + 15); ctx.show_text(label); ctx.restore()
        ctx.save(); ctx.translate(x0, y0 + lab); ctx.rectangle(0, 0, W, H); ctx.clip()
        ctx.set_source_rgb(0, 0, 0); ctx.paint(); ctx.scale(S, S)
        R.draw_svg(ctx, svg, {})
        for a, b, c, d, w, col in lines:
            ctx.set_source_rgba(*(v / 255 for v in col)); ctx.set_line_width(w / 1024); ctx.set_line_cap(cairo.LINE_CAP_BUTT)
            ctx.move_to(a / 1024, b / 1024); ctx.line_to(c / 1024, d / 1024); ctx.stroke()
        ctx.restore()
    surf.write_to_png(out); print('->', out)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
