#!/usr/bin/env python3
"""Preview of the MIDI sequencer view (seqdev/ms_draw.c via harness/mseq_render.c) -> display/mseq_preview.png.
Lines with butt caps (as the OP-1 rasteriser); text in the stock font (opfont.svg of a local firmware tree; a plain
stand-in without one)."""
import json, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); SDK = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import render as R
sys.path.insert(0, os.path.join(SDK, 'display'))
def text(ctx, x, y, c, s, align):
    """stock-font text (on the device; Iosevka here, capitals): x Q10, y px top, align 0 / 1 / 2"""
    ctx.set_source_rgb(*(v / 255 for v in c)); s = s.upper()
    w = R.iosevka.width(s, 9); x0 = x / 1024 - (w / 2 if align == 1 else w if align == 2 else 0)
    ctx.move_to(x0, y + 9); R.iosevka.show(ctx, s, 9)

def main(out=None):
    exe = os.path.join(HERE, 'build', 'mseq_render')
    subprocess.run(['cc', '-O1', '-Wall', '-DOP1_HOST', '-fsanitize=address,undefined', '-I', SDK,
                    os.path.join(HERE, 'mseq_render.c'), os.path.join(SDK, 'seqdev', 'ms_draw.c'),
                    os.path.join(SDK, 'seqdev', 'ms_core.c'), '-o', exe, '-lm'], check=True)
    frames = [json.loads(l) for l in subprocess.run([exe], check=True, capture_output=True, text=True).stdout.splitlines()]
    cairo = R.cairo; S = 3; W, H, pad, lab = 320 * S, 160 * S, 12, 22
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W + 2 * pad, len(frames) * (H + pad + lab) + pad)
    ctx = cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
    for n, fr in enumerate(frames):
        x0, y0 = pad, pad + n * (H + pad + lab)
        ctx.save(); ctx.set_source_rgb(1, 1, 1); ctx.set_font_size(15); ctx.move_to(x0, y0 + 16); ctx.show_text(fr['label']); ctx.restore()
        ctx.save(); ctx.translate(x0, y0 + lab); ctx.rectangle(0, 0, W, H); ctx.clip()
        ctx.set_source_rgb(0, 0, 0); ctx.paint(); ctx.scale(S, S)
        for x1, y1, x2, y2, w, c in fr['lines']:
            ctx.set_source_rgba(*(v / 255 for v in c)); ctx.set_line_width(w / 1024); ctx.set_line_cap(cairo.LINE_CAP_BUTT)
            ctx.move_to(x1 / 1024, y1 / 1024); ctx.line_to(x2 / 1024, y2 / 1024); ctx.stroke()
        for x, y, c, st, al in fr.get('text', []): text(ctx, x, y, c, st, al)
        ctx.restore()
    out = out or os.path.join(SDK, 'display', 'mseq_preview.png')
    surf.write_to_png(out); print('->', out)

if __name__ == '__main__':
    main(*sys.argv[1:2])
