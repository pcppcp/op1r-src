#!/usr/bin/env python3
"""Preview of the USB out page (screens/usbout.c via harness/usbout_test.c) -> display/usbout_preview.png.
Text in a plain font as a stand-in for the firmware font (position = top left)."""
import json, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); SDK = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import render as R
sys.path.insert(0, os.path.join(SDK, 'display'))
def main(out=None, name='usbout'):
    exe = os.path.join(HERE, 'build', name + '_test')
    subprocess.run(['cc', '-O1', '-Wall', '-DOP1_HOST', '-fsanitize=address,undefined', '-I', SDK,
                    os.path.join(HERE, name + '_test.c'), '-o', exe, '-lm'], check=True)
    print(subprocess.run([exe], check=True, capture_output=True, text=True).stdout.strip())
    frames = [json.loads(l) for l in subprocess.run([exe, 'render'], check=True, capture_output=True, text=True).stdout.splitlines() if l.startswith('{')]
    if os.environ.get('SINGLE'):                       # one frame, no label, 3x (README screenshots)
        frames = [frames[int(os.environ['SINGLE'])]]
    cairo = R.cairo; S, W, H, pad, lab = R.SCALE, 320 * R.SCALE, 160 * R.SCALE, 12, 22
    if os.environ.get('SINGLE'): S, W, H, pad, lab = 3, 960, 480, 0, 0
    cols, rows = len(frames), 1
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, cols * (W + pad) + pad, rows * (H + pad + lab) + pad)
    ctx = cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
    for n, fr in enumerate(frames):
        x0, y0 = pad + (n % cols) * (W + pad), pad + (n // cols) * (H + pad + lab)
        if lab: ctx.save(); ctx.set_source_rgb(1, 1, 1); ctx.set_font_size(14); ctx.move_to(x0, y0 + 15); ctx.show_text(fr['label']); ctx.restore()
        ctx.save(); ctx.translate(x0, y0 + lab); ctx.rectangle(0, 0, W, H); ctx.clip()
        ctx.set_source_rgb(0, 0, 0); ctx.paint(); ctx.scale(S, S)
        for x1, y1, x2, y2, w, c in fr['lines']:
            ctx.set_source_rgba(*(v / 255 for v in c)); ctx.set_line_width(w / 1024); ctx.set_line_cap(cairo.LINE_CAP_BUTT)
            ctx.move_to(x1 / 1024, y1 / 1024); ctx.line_to(x2 / 1024, y2 / 1024); ctx.stroke()
        for t in fr['text']:
            x, y, c, s = t[:4]
            ctx.set_source_rgb(*(v / 255 for v in c))
            if len(t) > 4 and t[4] == 1: x -= R.iosevka.width(s.upper(), 9) * 1024 / 2   # centred on x
            ctx.move_to(x / 1024, y + 9); R.iosevka.show(ctx, s.upper(), 9)   # stock font on the device: Iosevka here
        ctx.restore()
    out = out or os.path.join(SDK, 'display', 'usbout_preview.png')
    surf.write_to_png(out); print('->', out)

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else None, sys.argv[2] if len(sys.argv) > 2 else 'usbout')
