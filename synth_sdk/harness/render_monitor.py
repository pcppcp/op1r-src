#!/usr/bin/env python3
"""Preview of the resource monitor page: builds harness/sysmon_test.c (the real screens/sysmon.c with fake counters,
heaps, disk and MIDI fill), renders display/sysmon.svg + the recorded raster lines and texts
-> display/sysmon_preview.png. Text uses a plain sans font as a stand-in for the firmware font; its position is
taken as the top-left corner (to check on hardware, docs/18).
"""
import json, os, subprocess, sys, xml.etree.ElementTree as ET
HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(HERE)
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(SDK, 'display'))
import render as R
import gen_monitor_svg as G


def main(out=None):
    open(os.path.join(SDK, 'display', 'sysmon.svg'), 'w').write(G.svg())
    exe = os.path.join(HERE, 'build', 'sysmon_test')
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    subprocess.run(['cc', '-O1', '-Wall', '-DOP1_HOST', '-fsanitize=address,undefined', '-I', SDK,
                    os.path.join(HERE, 'sysmon_test.c'), '-o', exe], check=True)
    print(subprocess.run([exe], check=True, capture_output=True, text=True).stdout.strip())
    frames = [json.loads(l) for l in subprocess.run([exe, 'render'], check=True, capture_output=True,
                                                     text=True).stdout.splitlines() if l.startswith('{')]
    svg = ET.parse(os.path.join(SDK, 'display', 'sysmon.svg')).getroot()
    cairo = R.cairo
    S, W, H, pad, lab = R.SCALE, 320 * R.SCALE, 160 * R.SCALE, 12, 22
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W + 2 * pad, len(frames) * (H + pad + lab) + pad)
    ctx = cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
    for n, fr in enumerate(frames):
        x0, y0 = pad, pad + n * (H + pad + lab)
        ctx.save(); ctx.set_source_rgb(1, 1, 1); ctx.set_font_size(14); ctx.move_to(x0, y0 + 15); ctx.show_text(fr['label']); ctx.restore()
        ctx.save(); ctx.translate(x0, y0 + lab); ctx.rectangle(0, 0, W, H); ctx.clip()
        ctx.set_source_rgb(0, 0, 0); ctx.paint(); ctx.scale(S, S)
        R.draw_svg(ctx, svg, {})
        for x1, y1, x2, y2, w, c in fr['lines']:
            ctx.set_source_rgba(*(v / 255 for v in c)); ctx.set_line_width(w / 1024); ctx.set_line_cap(cairo.LINE_CAP_BUTT)
            ctx.move_to(x1 / 1024, y1 / 1024); ctx.line_to(x2 / 1024, y2 / 1024); ctx.stroke()
        for x, y, c, s in fr['text']:                 # stock font on the device: Iosevka here
            ctx.set_source_rgb(*(v / 255 for v in c)); ctx.move_to(x / 1024, y + 9); R.iosevka.show(ctx, s.upper(), 9)
        ctx.restore()
    out = out or os.path.join(SDK, 'display', 'sysmon_preview.png')
    surf.write_to_png(out); print('->', out)


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else None)
