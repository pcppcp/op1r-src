#!/usr/bin/env python3
"""Preview an engine screen on the PC: build the host harness (real engine + screen C code, mocked
firmware calls), run it for several knob/note states and render SVG + node transforms + raster lines.

  harness/render.py fmbell [out.png]        -> display/<engine>_preview.png   (engine or FX name)

Only the SVG subset used by the SDK screens is drawn: g, line, circle, ellipse, rect, polyline,
polygon, path (absolute/relative M L H V C Q Z). Colours as #rrggbb.
"""
import json, os, re, subprocess, sys, xml.etree.ElementTree as ET
try:
    import cairo                                  # pycairo
except ImportError:
    import cairocffi as cairo                     # pyenv: pip install cairocffi (no cairo headers needed)

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(HERE)
SCALE = 3
# (label, k0..k3, note, frames, release_frame[, pages bitmask, w5, w6, w7])
STATES_BY_NAME = {
    'granular': [('defaults, note', None, 1, 8, -1),
                 ('red page 2: sample gain 1x (unset)', None, 1, 8, -1, 0b1000, 0, 0, 0),
                 ('red page 2: gain 3x', None, 1, 8, -1, 0b1000, 0, 0, 24576),
                 ('page 1 again, gain 3x kept', None, 1, 8, -1, 0, 0, 0, 24576),
                 ('far position, long grains, wide spray, dense', (26000, 30000, 20000, 32767), 1, 8, -1),
                 ('start, short grains, no spray, sparse', (0, 0, 0, 0), 1, 8, -1)],
}
STATES_BY_NAME['dddd'] = [
    ('defaults, note held', None, 1, 40, -1),
    ('silent: the wasp at rest', (0, 0, 0, 0), 0, 10, -1),
    ('feedback max: stretched', (6000, 32767, 0, 0), 1, 40, -1),
    ('destroy max: jagged, coarse', (6000, 12000, 32767, 0), 1, 40, -1),
    ('source metal: angular, larger wings', (32767, 12000, 0, 0), 1, 40, -1),
    ('hands max: jumps, torn', (16000, 26000, 26000, 32767), 1, 50, -1),
    ('everything max', (32767, 32767, 32767, 32767), 1, 50, -1),
    ('page 2 on blue / green / white', (6000, 24000, 20000, 8000), 1, 40, -1, 0b0111, 20000, 16000, 12000),
]
STATES_BY_NAME['aether'] = [                     # HOST_ACCEL: sensor X,Y,Z after the note-on (home = flat)
    ('flat (home), note held', None, 1, 40, -1),
    ('front edge up 20 deg: pitch up', None, 1, 60, -1, 0, 0, 0, 0, {'HOST_ACCEL': '0,-5604,15396'}),
    ('back 15 deg, left side up 20 deg', None, 1, 60, -1, 0, 0, 0, 0, {'HOST_ACCEL': '5604,4240,14500'}),
    ('right side up 25 deg: volume down', None, 1, 60, -1, 0, 0, 0, 0, {'HOST_ACCEL': '-6924,0,14849'}),
    ('blue + white page 2 (dead zone, rate)', None, 1, 40, -1, 0b0101, 20000, 0, 26000),
    ('all knobs max, front up 40 deg', (32767,) * 4, 1, 60, -1, 0, 0, 0, 0, {'HOST_ACCEL': '0,-10531,12551'}),
]
FX_STATES_BY_NAME = {
    'reverb': [('defaults', None, 0, 4, -1),
               ('all min: 1 arm, dim skulls, no fire', (0, 0, 0, 0), 0, 4, -1),
               ('all max: 6 arms, mix full, bright skulls, fire', (32767,) * 4, 0, 4, -1),
               ('time 40%, tone 30%, diff 60%, mix 50%', (13100, 9830, 19660, 16384), 0, 4, -1),
               ('time 75%, tone 80%, diff 20%, mix 20%', (24575, 26214, 6553, 6553), 0, 4, -1),
               ('time 100%, tone 50%, diff 100%, mix 80%', (32767, 16384, 32767, 26214), 0, 4, -1)],
    'steve': [('no loop yet', (16384, 11000, 0, 32767), 0, 10, -1),
              ('capturing (white press)', (16384, 11000, 0, 32767), 6, 14, -1, 0b0100, 0, 8192, 0),
              ('just captured: unison', (16384, 11000, 0, 32767), 6, 40, -1, 0b0100, 0, 8192, 0),
              ('drift +2 %, 2 copies', (32767, 11000, 0, 32767), 6, 160, -1, 0b0100, 0, 8192, 0),
              ('drift +1 %, 4 copies (Come Out)', (24800, 11000, 32767, 32767), 6, 220, -1, 0b0100, 0, 8192, 0),
              ('steps: 12 units, 1 %, hold 2 (Piano Phase)', (21000, 11000, 0, 32767), 6, 300, -1, 0b0100, 16500, 4000, 0),
              ('page 2 on blue + green (steps)', (21000, 11000, 16384, 20000), 6, 120, -1, 0b0111, 16500, 4000, 0),
              ('frozen, no loop', (16384, 11000, 0, 32767), 0, 10, -1, 0b1000, 0, 8192, 0),
              ('synced length: 1 bar', (20000, 30000, 0, 32767), 0, 10, -1)],
    'nostromo': [('defaults (program 13: plate / med / low, HIFI): mid decay', None, 4, 160, -1, 0, 17824, 1024, 15170),
                 ('blue, green, white on page 2; clock STD, program 14', (16645, 23494, 6553, 11468), 4, 160, -1, 0b0111, 26000, 3072, 16384),
                 ('LOFI hall / high / high, wet: the white knob is the clock', (19660, 26214, 16384, 26214), 4, 160, -1, 0, 9800, 5120, 32767),
                 ('all min: room / low / low, dry', (0, 0, 0, 0), 4, 160, -1, 0, 0, 0, 0),
                 ('room, short, dark, thin bass, tank mod MED', (5000, 3300, 3300, 16384), 4, 160, -1, 0, 0, 1024, 1300),
                 ('hall, long, bright, fat bass, tank mod HIGH', (26000, 31000, 6553, 20000), 4, 160, -1, 0, 32767, 1024, 23000),
                 ('plate, diffusion HIGH: the pre-delay regenerated', (16645, 23494, 22000, 16384), 4, 160, -1, 0, 17824, 1024, 18811),
                 ('hall, diffusion LOW: the sparse cloud', (20000, 23494, 16000, 16384), 4, 160, -1, 0, 20000, 1024, 22452),
                 ('the screen opens: MU/TH/UR 6000', None, 4, 20, -1, 0, 17824, 1024, 15170),
                 ('an input hit: the engines flare', None, 40, 122, -1, 0, 17824, 1024, 15170)],
    'clouds': [('defaults, silent', None, 0, 20, -1),
               ('defaults, input bursts', None, 12, 60, -1),
               ('dense grains, small', (8000, 6000, 31000, 12000), 6, 60, -1),
               ('page 2 on blue+green (pitch +12, wet)', None, 12, 60, -1, 0b0011, 24576, 32767, 0),
               ('frozen (red press)', None, 12, 60, -1, 0b1000, 16384, 22938, 0),
               ('all page 2 + frozen', (30000, 25000, 26000, 30000), 6, 60, -1, 0b1111, 4000, 20000, 20000)],
}
FX_STATES = [('defaults, silent', None, 0, 20, -1),
             ('defaults, input bursts', None, 12, 50, -1),
             ('all knobs min, bursts', (0, 0, 0, 0), 6, 50, -1),
             ('all knobs max, bursts', (32767,) * 4, 12, 50, -1),
             ('short time, high feedback', (6000, 30000, 3000, 8000), 6, 50, -1),
             ('long time, input stopped', (16000, 20000, 30000, 16000), 10, 80, 60)]
STATES = [('defaults, idle', None, 0, 10, -1),
          ('defaults, note struck', None, 1, 3, -1),
          ('all knobs min, note', (0, 0, 0, 0), 1, 8, -1),
          ('all knobs max, note', (32767,) * 4, 1, 8, -1),
          ('mid knobs, note held', (16384,) * 4, 1, 40, -1),
          ('mid knobs, released', (16384,) * 4, 1, 40, 20)]


def colour(s):
    if not s or s == 'none': return None
    s = s.strip().lstrip('#')
    return tuple(int(s[i:i + 2], 16) / 255 for i in (0, 2, 4))


def path_ops(d):
    toks = re.findall(r'[MmLlHhVvCcSsQqZz]|-?\d*\.?\d+(?:e-?\d+)?', d)
    i, cmd, x, y, sx, sy, out = 0, None, 0.0, 0.0, 0.0, 0.0, []
    c2 = None                                      # last cubic control point (for S)
    num = lambda: float(toks[i])
    while i < len(toks):
        if toks[i].isalpha(): cmd = toks[i]; i += 1
        if cmd in 'Zz': out.append(('Z',)); x, y = sx, sy; continue
        rel = cmd.islower(); c = cmd.upper()
        n = {'M': 2, 'L': 2, 'H': 1, 'V': 1, 'C': 6, 'S': 4, 'Q': 4}[c]
        v = [float(t) for t in toks[i:i + n]]; i += n
        if c == 'H': v = [v[0] + (x if rel else 0), y]; c = 'L'
        elif c == 'V': v = [x, v[0] + (y if rel else 0)]; c = 'L'
        elif rel: v = [a + (x if k % 2 == 0 else y) for k, a in enumerate(v)]
        if c == 'Q':        # quadratic -> cubic
            qx, qy, ex, ey = v
            v = [x + 2 / 3 * (qx - x), y + 2 / 3 * (qy - y), ex + 2 / 3 * (qx - ex), ey + 2 / 3 * (qy - ey), ex, ey]; c = 'C'
        if c == 'S':        # smooth cubic: first control = reflection of the previous second control
            rx, ry = (2 * x - c2[0], 2 * y - c2[1]) if c2 else (x, y)
            v = [rx, ry] + v; c = 'C'
        c2 = (v[2], v[3]) if c == 'C' else None
        out.append((c, *v)); x, y = v[-2], v[-1]
        if c == 'M': sx, sy = x, y; cmd = 'l' if rel else 'L'
    return out


STYLES = {}                                        # node id -> rgb from the screen code (op1_node_line/fill_rgba)


def draw_svg(ctx, el, nodes, hidden=()):
    tag = el.tag.split('}')[-1]
    nid = el.get('id')
    if nid in hidden: return
    ctx.save()
    if el.get('transform'):                         # matrix(a b c d e f) / translate(x y)
        v = [float(x) for x in re.findall(r'-?\d*\.?\d+(?:e-?\d+)?', el.get('transform'))]
        ctx.transform(cairo.Matrix(*v) if el.get('transform').startswith('matrix') else cairo.Matrix(1, 0, 0, 1, *v[:2]))
    if nid in nodes:
        a, b, _, c, d, _, tx, ty, _ = [v / 1024 for v in nodes[nid]]
        ctx.transform(cairo.Matrix(a, b, c, d, tx, ty))
    f = lambda k, dflt=0: float(el.get(k, dflt))
    if tag in ('svg', 'g'):
        for ch in el: draw_svg(ctx, ch, nodes, hidden)
        ctx.restore(); return
    if tag == 'line':
        ctx.move_to(f('x1'), f('y1')); ctx.line_to(f('x2'), f('y2'))
    elif tag == 'circle':
        ctx.new_sub_path(); ctx.arc(f('cx'), f('cy'), f('r'), 0, 6.2832)
    elif tag == 'ellipse':
        ctx.save(); ctx.translate(f('cx'), f('cy')); ctx.scale(f('rx'), f('ry'))
        ctx.new_sub_path(); ctx.arc(0, 0, 1, 0, 6.2832); ctx.restore()
    elif tag == 'rect':
        ctx.rectangle(f('x'), f('y'), f('width'), f('height'))
    elif tag in ('polyline', 'polygon'):
        p = [float(v) for v in re.findall(r'-?\d*\.?\d+', el.get('points', ''))]
        ctx.move_to(p[0], p[1])
        for k in range(2, len(p), 2): ctx.line_to(p[k], p[k + 1])
        if tag == 'polygon': ctx.close_path()
    elif tag == 'path':
        for op in path_ops(el.get('d', '')):
            {'M': lambda *v: ctx.move_to(*v), 'L': lambda *v: ctx.line_to(*v),
             'C': lambda *v: ctx.curve_to(*v), 'Z': lambda: ctx.close_path()}[op[0]](*op[1:])
    else:
        ctx.restore(); return
    fill, stroke = colour(el.get('fill', '#000000')), colour(el.get('stroke'))
    if nid in STYLES:                                # recoloured by the screen code
        rgb = tuple(v / 255 for v in STYLES[nid])
        if stroke: stroke = rgb
        elif fill: fill = rgb
    if fill: ctx.set_source_rgb(*fill); ctx.fill_preserve() if stroke else ctx.fill()
    if stroke:
        ctx.set_source_rgb(*stroke); ctx.set_line_width(float(el.get('stroke-width', 1)))
        ctx.set_line_cap({'round': cairo.LINE_CAP_ROUND, 'square': cairo.LINE_CAP_SQUARE}.get(
            el.get('stroke-linecap'), cairo.LINE_CAP_BUTT))
        ctx.set_line_join(cairo.LINE_JOIN_ROUND); ctx.stroke()
    ctx.new_path(); ctx.restore()


def build(engine):
    sys.path.insert(0, SDK); import plugins
    engs, fxs = plugins.load()
    fx = next((x for x in fxs if x['name'] == engine), None)
    e = fx or next(x for x in engs if x['name'] == engine)
    kind = f'-DFX={e["symbol"]}' if fx else f'-DENGINE={e["symbol"]}' 
    exe = os.path.join(HERE, 'build', engine)
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    subprocess.run(['cc', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    '-DOP1_HOST', kind, f'-DSCREEN={e["screen"]["symbol"]}',
                    '-I', SDK, os.path.join(HERE, 'screen_harness.c'), *(os.path.join(SDK, x) for x in [e['source']] + e['extra_sources']),
                    '-lm', '-o', exe], check=True)
    return e, exe, bool(fx)


def main(engine, out=None):
    e, exe, is_fx = build(engine)
    states = FX_STATES_BY_NAME.get(engine, FX_STATES) if is_fx else STATES_BY_NAME.get(engine, STATES)
    svg = ET.parse(os.path.join(SDK, e['screen']['svg'])).getroot()
    W, H, pad, lab = 320 * SCALE, 160 * SCALE, 12, 22
    cols = 2; rows = (len(states) + 1) // 2
    sc = SCALE
    if os.environ.get('SINGLE'):                     # one frame, no label, 3x (README screenshots)
        states = [states[int(os.environ['SINGLE'])]]
        sc = 3; W, H, pad, lab = 960, 480, 0, 0; cols = rows = 1
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, cols * (W + pad) + pad, rows * (H + pad + lab) + pad)
    ctx = cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
    for n, (label, knobs, note, frames, rel, *extra) in enumerate(states):
        knobs = knobs or e['defaults']
        env = dict(os.environ, **extra.pop()) if extra and isinstance(extra[-1], dict) else None   # e.g. HOST_ACCEL
        res = subprocess.run([exe, *map(str, knobs), str(note), str(frames), str(rel), *map(str, extra)],
                             check=True, capture_output=True, text=True, env=env)
        if res.stderr: print(res.stderr, file=sys.stderr)
        fr = json.loads(res.stdout)
        x0, y0 = pad + (n % cols) * (W + pad), pad + (n // cols) * (H + pad + lab)
        if lab:
            ctx.save(); ctx.set_source_rgb(1, 1, 1); ctx.set_font_size(14)
            ctx.move_to(x0, y0 + 15); ctx.show_text(f'{label}   knobs {list(knobs)}'); ctx.restore()
        ctx.save(); ctx.translate(x0, y0 + lab); ctx.rectangle(0, 0, W, H); ctx.clip()
        ctx.set_source_rgb(0, 0, 0); ctx.paint(); ctx.scale(sc, sc)
        STYLES.clear(); STYLES.update(fr.get('styles', {}))
        draw_svg(ctx, svg, fr['nodes'], set(fr.get('hidden', ())))
        for x1, y1, x2, y2, w, c in fr['lines']:
            ctx.set_source_rgba(*(v / 255 for v in c)); ctx.set_line_width(w / 1024)
            ctx.set_line_cap(cairo.LINE_CAP_BUTT)   # as the OP-1 rasteriser
            ctx.move_to(x1 / 1024, y1 / 1024); ctx.line_to(x2 / 1024, y2 / 1024); ctx.stroke()
        for x, y, size, c, txt in fr.get('texts', []):     # stock font on the device: a stand-in here
            ctx.set_source_rgb(*(v / 255 for v in c)); ctx.select_font_face('DejaVu Sans', 0, 1)
            ctx.set_font_size(size * 0.78); ctx.move_to(x / 1024, y + size * 0.72); ctx.show_text(txt)
        ctx.restore()
        print(f'{label:24s} nodes={len(fr["nodes"])} lines={len(fr["lines"])}')
    out = out or os.path.join(SDK, 'display', f'{engine}_preview.png')
    surf.write_to_png(out); print('->', out)


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'fmbell', sys.argv[2] if len(sys.argv) > 2 else None)
