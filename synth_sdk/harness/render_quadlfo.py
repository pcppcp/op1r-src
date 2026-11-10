#!/usr/bin/env python3
"""Render the quad LFO screen states from harness/quadlfo_test.c (render mode) -> display/quadlfo_preview.png"""
import json, os, subprocess, sys, xml.etree.ElementTree as ET
HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import render as R                                               # draw_svg + cairo import

def main(out=None):
    exe = os.path.join(HERE, 'build', 'quadlfo_test')
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    subprocess.run(['cc', '-O1', '-Wall', '-DOP1_HOST', '-fsanitize=address,undefined', '-I', SDK, '-I', HERE,
                    os.path.join(HERE, 'quadlfo_test.c'), '-lm', '-o', exe], check=True)
    chk = subprocess.run([exe], capture_output=True, text=True); print(chk.stdout.strip())
    frames = [json.loads(l) for l in subprocess.run([exe, 'render'], check=True, capture_output=True,
                                                     text=True).stdout.splitlines() if l.startswith('{')]
    svg = ET.parse(os.path.join(SDK, 'display', 'quadlfo.svg')).getroot()
    pond = ET.parse(os.path.join(SDK, 'display', 'pondlfo.svg')).getroot()
    empty = ET.parse(os.path.join(SDK, 'display', 'usbout.svg')).getroot()   # master LFO type list: drawn in code
    cairo = R.cairo
    S, W, H, pad, lab = R.SCALE, 320 * R.SCALE, 160 * R.SCALE, 12, 22
    cols, rows = 2, (len(frames) + 1) // 2
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, cols * (W + pad) + pad, rows * (H + pad + lab) + pad)
    ctx = cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
    for n, fr in enumerate(frames):
        x0, y0 = pad + (n % cols) * (W + pad), pad + (n // cols) * (H + pad + lab)
        ctx.save(); ctx.set_source_rgb(1, 1, 1); ctx.set_font_size(14); ctx.move_to(x0, y0 + 15); ctx.show_text(fr['label']); ctx.restore()
        ctx.save(); ctx.translate(x0, y0 + lab); ctx.rectangle(0, 0, W, H); ctx.clip()
        ctx.set_source_rgb(0, 0, 0); ctx.paint(); ctx.scale(S, S)
        lb = fr['label']
        R.draw_svg(ctx, pond if lb.startswith(('POND', 'MASTER POND')) else empty if lb.startswith('MASTER LIST') else svg, fr['nodes'])
        for x1, y1, x2, y2, w, c in fr['lines']:
            ctx.set_source_rgba(*(v / 255 for v in c)); ctx.set_line_width(w / 1024); ctx.set_line_cap(cairo.LINE_CAP_BUTT)   # as the OP-1 rasteriser
            ctx.move_to(x1 / 1024, y1 / 1024); ctx.line_to(x2 / 1024, y2 / 1024); ctx.stroke()
        for x, y, size, c, txt in fr.get('texts', []):           # stock font on the device: a stand-in here
            ctx.set_source_rgb(*(v / 255 for v in c[:3])); ctx.move_to(x / 1024, y + size * 0.72)
            R.iosevka.show(ctx, txt.upper(), size * 0.78)        # stock font on the device: Iosevka here
        ctx.restore()
    out = out or os.path.join(SDK, 'display', 'quadlfo_preview.png')
    surf.write_to_png(out); print('->', out)

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else None)
