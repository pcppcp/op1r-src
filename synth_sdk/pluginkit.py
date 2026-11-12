#!/usr/bin/env python3
"""Plugin kit: start a plugin from the template and check it on the PC (C compiler and Python 3; no toolchain).

    python3 pluginkit.py new <kind> <name> [--dir DIR] [--author NAME]
        kind: synth, fx, seq or tool. -> DIR/<name>/ (default plugins/<name>/): plugin.json, <name>.c, a screen
        (<name>.svg; tools draw an overlay instead) and a host test <name>_test.c. It builds and passes as it is.
    python3 pluginkit.py check <plugin> [-o DIR]
        plugin: a name in plugins/ or a folder. Builds the plugin's test with its sources (ASan + UBSan) and runs it,
        then writes the previews to DIR (default harness/out/<name>/): <name>.wav (synth / FX, harness/audio.py) and
        <name>_screen.png (the frames the test prints with the argument "render"; needs pycairo or cairocffi).
    python3 pluginkit.py check --all
        every plugin in plugins/: its own test if it has one (<folder>/<name>_test.c), and a sanitizer build and
        render of every synth / FX (harness/audio.py); lists the plugins whose only test is in harness/ or none.

Then the plugin file: python3 make_plugin.py <folder> (Docker toolchain, see PLUGINS.md).
"""
import argparse, datetime, json, os, re, shutil, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS = os.path.join(HERE, 'harness')
KINDS = {'synth': 'mysynth', 'fx': 'myfx', 'seq': 'myseq', 'tool': 'mytool'}   # kind -> template folder token
CC = ['cc', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers',
      '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-DOP1_HOST']


def plugin_dirs():
    d = os.path.join(HERE, 'plugins')
    return sorted(os.path.join(d, f) for f in os.listdir(d) if os.path.isfile(os.path.join(d, f, 'plugin.json')))


def find(p):
    """name (in plugins/) or folder -> (folder, plugin.json dict)"""
    if os.path.isfile(os.path.join(p, 'plugin.json')):
        return os.path.abspath(p), json.load(open(os.path.join(p, 'plugin.json')))
    for d in plugin_dirs():
        j = json.load(open(os.path.join(d, 'plugin.json')))
        if p in (os.path.basename(d), j['name']):
            return d, j
    sys.exit(f'no plugin {p!r} (give a name from plugins/ or a folder with plugin.json)')


def new(kind, name, parent, author):
    if not re.fullmatch(r'[a-z][a-z0-9_]{0,14}', name):
        sys.exit(f'{name!r}: lowercase letters, digits and _, at most 15 characters, a letter first')
    taken = {os.path.basename(d) for d in plugin_dirs()} | {json.load(open(os.path.join(d, 'plugin.json')))['name'] for d in plugin_dirs()}
    if name in taken or name in KINDS.values():
        sys.exit(f'{name!r} is taken (plugins/ or a template name)')
    dst = os.path.join(parent, name)
    if os.path.exists(dst):
        sys.exit(f'{dst} exists')
    src, tok = os.path.join(HERE, 'template', kind), KINDS[kind]
    os.makedirs(dst)
    for f in sorted(os.listdir(src)):
        text = open(os.path.join(src, f)).read().replace(tok, name)
        text = text.replace('<year>', str(datetime.date.today().year))
        if author:
            text = text.replace('<you>', author).replace('"your name"', json.dumps(author))
        open(os.path.join(dst, f.replace(tok, name)), 'w').write(text)
    print(f'{dst}: {kind} plugin "{name}" ({", ".join(sorted(os.listdir(dst)))})\n'
          f'next: python3 pluginkit.py check {os.path.relpath(dst)}')
    return dst


def test_of(d, p):
    """-> (the plugin's kit test <folder>/<name>_test.c, which includes harness/plugin_test.h, or None; other tests)"""
    own = sorted(f for f in os.listdir(d) if f.endswith('_test.c'))
    for n in (os.path.basename(d), p['name']):
        t = os.path.join(d, n + '_test.c')
        if os.path.isfile(t) and 'harness/plugin_test.h' in open(t, errors='replace').read():
            return t, [f for f in own if f != n + '_test.c']
    return None, own


def run_test(d, p, test, render=False):
    """build <folder>/<name>_test.c with the plugin's sources and run it -> (ok, output, frames)"""
    exe = os.path.join(HARNESS, 'build', 'pluginkit', p['name'] + '_test')
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    b = subprocess.run(CC + ['-I', HERE, test, *(os.path.join(d, s) for s in p['sources']), '-lm', '-o', exe],
                       capture_output=True, text=True)
    if b.returncode:
        return False, b.stderr, []
    r = subprocess.run([exe] + (['render'] if render else []), capture_output=True, text=True, timeout=600)
    out = r.stdout + r.stderr
    frames = [json.loads(l) for l in r.stdout.splitlines() if l.startswith('{')]
    text = '\n'.join(l for l in out.splitlines() if not l.startswith('{'))
    return r.returncode == 0 and 'FAIL' not in text and 'runtime error' not in text, text, frames


def png(d, p, frames, out):
    """frames (plugin_test.h pt_frame) over the plugin's SVG -> PNG, 2 per row; False without cairo"""
    sys.path.insert(0, HARNESS)
    try:
        import render as R                                          # render.py: SVG + frame drawing, cairo
    except ImportError:
        return False
    import xml.etree.ElementTree as ET
    svg = ET.parse(os.path.join(d, p['screen']['svg'])).getroot() if p.get('screen') else None
    S, W, H, pad, lab = 3, 960, 480, 12, 22
    rows = (len(frames) + 1) // 2
    surf = R.cairo.ImageSurface(R.cairo.FORMAT_RGB24, 2 * (W + pad) + pad, rows * (H + pad + lab) + pad)
    ctx = R.cairo.Context(surf); ctx.set_source_rgb(0.25, 0.25, 0.27); ctx.paint()
    for n, fr in enumerate(frames):
        x0, y0 = pad + (n % 2) * (W + pad), pad + (n // 2) * (H + pad + lab)
        ctx.save(); ctx.set_source_rgb(1, 1, 1); ctx.set_font_size(14); ctx.move_to(x0, y0 + 15)
        ctx.show_text(fr.get('label', '')); ctx.restore()
        ctx.save(); ctx.translate(x0, y0 + lab); ctx.rectangle(0, 0, W, H); ctx.clip()
        g = 0.12 if fr.get('dim') else 0                             # overlay over a darkened page
        ctx.set_source_rgb(g, g, g); ctx.paint(); ctx.scale(S, S)
        R.draw_frame(ctx, svg, fr)
        ctx.restore()
    surf.write_to_png(out)
    return True


def check(plugin, out_dir=None, previews=True):
    """-> list of problems (empty: passed); prints what it did"""
    d, p = find(plugin)
    name, bad = p['name'], []
    if not re.fullmatch(r'[a-z0-9_ +\-]{1,15}', name):
        bad.append(f'name {name!r}: lowercase, at most 15 characters')
    if p.get('kind') not in ('synth', 'fx', 'seq', 'drum', 'sampler', 'tool'):
        bad.append(f'kind {p.get("kind")!r}')
    files = list(p.get('sources', [])) + ([p['screen']['svg']] if p.get('screen') else [])
    missing = [f for f in files if not os.path.isfile(os.path.join(d, f))]
    if missing or not p.get('sources'):
        return bad + [f'missing files: {missing or "sources"}']
    out_dir = out_dir or os.path.join(HARNESS, 'out', name)
    test, other = test_of(d, p)
    if test:
        ok, text, frames = run_test(d, p, test, render=previews)
        print(f'{name}: test {os.path.basename(test)}: {"ok" if ok else "FAIL"}')
        if not ok:
            bad.append('test failed:\n' + '\n'.join('    ' + l for l in text.strip().splitlines()[-15:]))
        elif previews and frames:
            os.makedirs(out_dir, exist_ok=True)
            f = os.path.join(out_dir, f'{name}_screen.png')
            print(f'{name}: screen -> {f}' if png(d, p, frames, f) else f'{name}: screen: no pycairo / cairocffi, no PNG')
    elif os.path.isfile(os.path.join(HARNESS, os.path.basename(d) + '_test.c')):
        print(f'{name}: test harness/{os.path.basename(d)}_test.c (run by harness/run_all.py)')
    elif other:
        print(f'{name}: own tests {", ".join(other)} (built their own way: not run here)')
    else:
        print(f'{name}: no {os.path.basename(d)}_test.c here or in harness/ (pluginkit.py new writes one to start from)')
    if p['kind'] in ('synth', 'fx') and not p.get('input_engine'):  # host build + render with sanitizers
        wav = os.path.join(out_dir if previews else tempfile.mkdtemp(), f'{name}.wav')
        os.makedirs(os.path.dirname(wav), exist_ok=True)
        r = subprocess.run([sys.executable, os.path.join(HARNESS, 'audio.py'), d, '-o', wav], capture_output=True, text=True)
        print(f'{name}: audio {"-> " + wav if previews else "ok"}' if not r.returncode else f'{name}: audio FAIL')
        if r.returncode:
            bad.append('audio.py failed:\n' + '\n'.join('    ' + l for l in (r.stdout + r.stderr).strip().splitlines()[-10:]))
        if not previews:
            shutil.rmtree(os.path.dirname(wav), ignore_errors=True)
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    n = sub.add_parser('new'); n.add_argument('kind', choices=sorted(KINDS)); n.add_argument('name')
    n.add_argument('--dir', default=os.path.join(HERE, 'plugins')); n.add_argument('--author')
    c = sub.add_parser('check'); c.add_argument('plugin', nargs='?'); c.add_argument('-o', '--out')
    c.add_argument('--all', action='store_true')
    a = ap.parse_args()
    if a.cmd == 'new':
        new(a.kind, a.name, a.dir, a.author)
        return
    if a.all:
        with ThreadPoolExecutor(os.cpu_count()) as pool:
            res = dict(zip(plugin_dirs(), pool.map(lambda d: check(d, previews=False), plugin_dirs())))
        bad = {d: b for d, b in res.items() if b}
        for d, b in bad.items():
            print(f'FAIL  {os.path.basename(d)}: ' + '\n      '.join(b))
        print(f'{len(res) - len(bad)} ok / {len(bad)} failed' if bad else f'all ok ({len(res)} plugins)')
        sys.exit(1 if bad else 0)
    if not a.plugin:
        ap.error('check: give a plugin, or --all')
    bad = check(a.plugin, a.out)
    for b in bad:
        print('FAIL  ' + b)
    print('ok' if not bad else f'{len(bad)} problem(s)')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
