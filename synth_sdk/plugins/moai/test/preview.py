#!/usr/bin/env python3
"""Screen previews of every view (needs pycairo): python3 preview.py [out.png [only the states whose label has this]]
or python3 preview.py --screens DIR: one image per state (the OP-1's visible 320 x 146, 2x) and DIR/README.md.
Runs the real engine, UI and screen code (build/mm_views, see mm_views.c) and draws the frames with the SDK's
harness/render.py (SVG + raster lines), several states per view."""
import json, os, re, subprocess, sys
import xml.etree.ElementTree as ET
HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(SDK, 'harness'))
import render
cairo = render.cairo
EXE = os.path.join(HERE, 'build', 'mm_views')
W = lambda n: n * 1024 + 512
# (label, view, k0..k3 (setup, cutoff, emphasis, contour), w5..w7 (detune, drive, drift), note, frames[, osc3 off, highlights])
V3 = '10:30000,14:30000,18:30000'                         # all three oscillators sounding
STATES = [
    ('home: MOOG BS (108), idle', 0, (W(1), 15000, 9000, 12000), (0, 0, 0), 0, 4),
    ('home: cutoff open, little emphasis', 0, (W(1), 30000, 3000, 12000), (0, 0, 0), 0, 4),
    ('home: the initial sound (000, Jestofunk bass), idle', 0, (1013, 3932, 26213, 21626), (8847, 8191, 0), 0, 4),
    ('home: 000 at rest (the number faded)', 0, (1013, 3932, 26213, 21626), (8847, 8191, 0), 0, 300),
    ('home: the four knobs shown, at rest (knobs shown)', 0, (W(1), 15000, 9000, 12000), (0, 0, 0), 0, 4),
    ('home: all four knobs turning (turning 0123)', 0, (W(1), 15000, 9000, 12000), (0, 0, 0), 0, 4),
    ('home: 000, a note: the contour opens the filter', 0, (1013, 3932, 26213, 21626), (8847, 8191, 0), 1, 12),
    ('home: cutoff 45 %, emphasis high', 0, (W(1), 15000, 29000, 12000), (0, 0, 0), 0, 4),
    ('home: cutoff closed', 0, (W(1), 1500, 9000, 12000), (0, 0, 0), 0, 4),
    ('home: detune, drift, noise (wind)', 0, (W(1), 22000, 16000, 20000), (30000, 0, 30000), 0, 8, 0, '', '', '19:26000'),
    ('home + Shift: DET DRV AMT SAT', 0, (W(1), 15000, 9000, 12000), (14000, 9000, 0), 0, 4, 0, '', 'shift', '26:12000'),
    ('home: cutoff knob (turning 1)', 0, (W(1), 12000, 9000, 12000), (0, 0, 0), 0, 4),
    ('home: emphasis knob (turning 2)', 0, (W(1), 15000, 20000, 12000), (0, 0, 0), 0, 4),
    ('home: contour knob (turning 3): a slow swell', 0, (W(1), 15000, 9000, 27000), (0, 0, 0), 0, 4),
    ('home: contour knob (turning 3): a pluck', 0, (W(1), 15000, 9000, 6000), (0, 0, 0), 0, 4),
    ('home: cutoff at the top, emphasis high', 0, (W(1), 32767, 26000, 12000), (0, 0, 0), 0, 4),
    ('home: 5 s untouched (the number faded)', 0, (W(1), 15000, 9000, 12000), (0, 0, 0), 0, 300),
    ('home: contour 5 %', 0, (W(1), 15000, 9000, 1600), (0, 0, 0), 0, 4),
    ('home: contour 15 %', 0, (W(1), 15000, 9000, 4900), (0, 0, 0), 0, 4),
    ('home: contour 25 %', 0, (W(1), 15000, 9000, 8200), (0, 0, 0), 0, 4),
    ('home: contour 40 %', 0, (W(1), 15000, 9000, 13100), (0, 0, 0), 0, 4),
    ('home: emphasis 30 %, cutoff 30 %', 0, (W(1), 10000, 9800, 12000), (0, 0, 0), 0, 4),
    ('home: emphasis full, cutoff 70 %', 0, (W(1), 23000, 32767, 12000), (0, 0, 0), 0, 4),
    ('advanced (default highlights)', 1, (W(1), 15000, 9000, 12000), (14000, 9000, 0), 0, 4),
    ('advanced: highlights moved', 1, (W(10), 20000, 9000, 12000), (14000, 9000, 0), 0, 4, 0, '9,17,24,5'),
    ('osc 1 (MOOG BS), idle: the ideal shape', 2, (W(1), 15000, 9000, 12000), (0, 0, 0), 0, 6),
    ('osc 1 (MOOG BS), a note: the real output', 2, (W(1), 9000, 14000, 16000), (0, 0, 0), 1, 20),
    ('osc 2 (5TH LEAD: 8\' saw +7), a note', 3, (W(10), 15000, 9000, 12000), (0, 0, 0), 1, 20),
    ('osc 3 resting (poly)', 4, (W(1), 15000, 9000, 12000), (0, 0, 0), 0, 6, 1),
    ('osc 3 (CLARINET: 16\' triangle)', 4, (W(15), 15000, 9000, 12000), (0, 0, 0), 0, 6),
    ('filter, note held', 5, (W(1), 9000, 24000, 20000), (0, 0, 0), 1, 12),
    ('filter: cutoff on a note (A4)', 5, (W(1), 14613, 24000, 20000), (0, 0, 0), 0, 4),
    ('contour (from the contour knob)', 6, (W(1), 15000, 9000, 27000), (0, 0, 0), 0, 4),
    ('contour: decay on the tempo (1/8)', 6, (W(1), 15000, 9000, 12000), (0, 0, 0), 0, 4, 0, '', 'sync'),
    ('analog: detune, drift, drive up', 7, (W(29), 15000, 9000, 12000), (24000, 26000, 30000), 0, 10),
    ('analog: MOOG BS as set (three saws, 16\' 16\' 32\')', 7, (W(1), 15000, 9000, 12000), (0, 0, 0), 0, 10),
    ('analog: saturation up', 7, (W(0), 15000, 9000, 12000), (0, 9000, 0), 0, 10, 0, '', '', '26:26000'),
    ('noise row (BREATH), a note', 8, (W(29), 15000, 9000, 12000), (0, 0, 0), 1, 20),
    ('osc 3 in LO (silent: the modulator), a note', 4, (W(1), 15000, 9000, 12000), (0, 0, 0), 1, 20, 0, '', '', '15:1,16:4,17:300,18:0,28:8000,29:16000'),
    ('modulation row: osc 3 LO, vibrato + wah', 8, (W(1), 15000, 9000, 12000), (0, 0, 0), 1, 20, 0, '', '', '15:1,16:4,18:0,27:6000,28:8000,29:16000'),
    ('modulation row: no source (osc 3 at 32\')', 8, (W(1), 15000, 9000, 12000), (0, 0, 0), 0, 4, 0, '', '', '29:16000'),
]

def frame(st):
    if '(knobs shown)' in st[0]:        # home at rest with its four knobs as Shift shows its layer's (composed: on the
        fr = frame((st[0].replace('(knobs shown)', ''),) + st[1:])        # OP-1 they show while turning, which also
        fa = frame((st[0].replace('(knobs shown)', '(turning 0123)'),) + st[1:])   # lights the number and the moai)
        fr['lines'] += [l for l in fa['lines'] if min(l[0], l[2]) > 200 * 1024 and max(l[1], l[3]) < 45 * 1024]
        return fr
    label, view, k, w, note, frames, *rest = st
    args = [EXE, str(view), *map(str, k), *map(str, w), str(note), str(frames)]
    if rest: args.append(str(rest[0]))
    if len(rest) > 1: args.append(rest[1] or '-')
    if len(rest) > 2: args.append(rest[2] or '-')
    if len(rest) > 3: args.append(rest[3])
    env = dict(os.environ)
    if label.startswith('home') and '(turning ' in label: env['MM_TURN'] = label.split('(turning ')[1].split(')')[0]
    r = subprocess.run(args, check=True, capture_output=True, text=True, env=env)
    return json.loads(r.stdout)

def draw(ctx, svg, fr):
    """one frame at 1 unit per pixel, on black"""
    ctx.set_source_rgb(0, 0, 0); ctx.paint()
    render.draw_svg(ctx, svg, fr['nodes'], set(fr.get('hidden', ())))
    for x1, y1, x2, y2, w, c in fr['lines']:
        ctx.set_source_rgba(*(v / 255 for v in c)); ctx.set_line_width(w / 1024)
        ctx.set_line_cap(cairo.LINE_CAP_BUTT); ctx.move_to(x1 / 1024, y1 / 1024); ctx.line_to(x2 / 1024, y2 / 1024); ctx.stroke()

def screens(out_dir):
    """one PNG per state, the rows the OP-1 shows (0 .. 145), and an index"""
    svg = ET.parse(os.path.join(HERE, '..', 'moai.svg')).getroot()
    os.makedirs(out_dir, exist_ok=True)
    S, VIEWS = 2, ['home', 'advanced', 'osc 1', 'osc 2', 'osc 3', 'filter', 'contour', 'analog', 'noise / modulation row']
    md = ['# moai screens', '',
          'Every view in a few states, drawn by the plugin\'s own engine, UI and screen code on the host',
          '(`make screens`: plugin/test/preview.py, mm_views.c). The sound number is drawn in line digits here; the OP-1',
          'draws it in its own font. 320 x 146 pixels (what the OP-1 shows), at 2x.', '']
    last = None
    for n, st in enumerate(sorted(STATES, key=lambda st: st[1])):     # grouped by view (a stable sort)
        fr = frame(st)
        name = '%02d-%s.png' % (n + 1, re.sub(r'[^a-z0-9]+', '-', st[0].lower()).strip('-')[:60].rstrip('-'))
        surf = cairo.ImageSurface(cairo.FORMAT_RGB24, 320 * S, 146 * S)
        ctx = cairo.Context(surf); ctx.scale(S, S); draw(ctx, svg, fr)
        surf.write_to_png(os.path.join(out_dir, name))
        if st[1] != last:
            md += ['## ' + VIEWS[st[1]], '']; last = st[1]
        md += ['**%s**' % st[0], '', '![%s](%s)' % (st[0], name), '']
        print(f'{name:70s} lines={len(fr["lines"])}')
    open(os.path.join(out_dir, 'README.md'), 'w').write('\n'.join(md))
    print('->', out_dir)

def main(out, only=''):
    states = [st for st in STATES if only in st[0]]
    svg = ET.parse(os.path.join(HERE, '..', 'moai.svg')).getroot()
    S, pad, lab = 3, 12, 22
    Wd, Hd = 320 * S, 160 * S
    cols, rows = 2, (len(states) + 1) // 2
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, cols * (Wd + pad) + pad, rows * (Hd + pad + lab) + pad)
    ctx = cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
    for n, st in enumerate(states):
        fr = frame(st)
        x0, y0 = pad + (n % cols) * (Wd + pad), pad + (n // cols) * (Hd + pad + lab)
        ctx.save(); ctx.set_source_rgb(1, 1, 1); ctx.set_font_size(14); ctx.move_to(x0, y0 + 15); ctx.show_text(st[0]); ctx.restore()
        ctx.save(); ctx.translate(x0, y0 + lab); ctx.rectangle(0, 0, Wd, Hd); ctx.clip()
        ctx.scale(S, S); draw(ctx, svg, fr)
        ctx.set_source_rgba(1, 0.2, 0.2, 0.6); ctx.set_line_width(0.5); ctx.move_to(0, 146); ctx.line_to(320, 146); ctx.stroke()   # visible limit
        ctx.restore()
        print(f'{st[0]:40s} lines={len(fr["lines"])}')
    surf.write_to_png(out); print('->', out)

if __name__ == '__main__' and sys.argv[1:2] == ['--screens']:
    screens(sys.argv[2])
elif __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'build', 'preview.png'), sys.argv[2] if len(sys.argv) > 2 else '')
