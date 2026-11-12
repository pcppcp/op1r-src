#!/usr/bin/env python3
"""Symbol map for the crash screen (crash.c): op1r core functions, plugin functions (offsets in each plugin file's
image, as crash.c prints them) and the R246 addresses this source names. make_firmware.py writes it next to the
firmware (op1_246_custom-symbols.json); tools/op1crash.py and the patcher's web/crash.html read it.

    python3 symmap.py [-o out.json]           map of the last make_firmware.py build (build/synth.nms, build/plugins)
    python3 symmap.py --stock [-o out.json]   only the R246 names (no build needed)

Format: {"op1r": VERSION, "image_end": a, "core": [[addr, size, name], ...], "plugins": {file: [[offset, size, name],
...]}, "stock": [[addr, name], ...]}, sorted by address; size 0 = unknown (asm labels)."""
import argparse, glob, json, os, re

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, 'build')
STOCK = (0x01900000, 0x01B00000)                  # R246 code in SDRAM (crash.c code_addr)
L1 = (0xFFA00000, 0xFFA14000)


def parse_nm(text):
    """bfin-elf-nm -S -n output -> ([(addr, size, name)] code symbols, __image_end or 0)"""
    out, end = [], 0
    for line in text.splitlines():
        f = line.split()
        if len(f) == 4: a, s, t, n = f
        elif len(f) == 3: (a, t, n), s = f, '0'
        else: continue
        if n == '__image_end': end = int(a, 16)
        if t in 'tTwW' and not n.startswith(('.', '$')) and not STOCK[0] <= int(a, 16) < STOCK[1]:   # (hook.S labels in R246: stock())
            out.append((int(a, 16), int(s, 16), n[1:] if n.startswith('_') else n))   # C names: leading _
    return sorted(set(out)), end


def _code(a):
    return STOCK[0] <= a < STOCK[1] or L1[0] <= a < L1[1]


def stock(sdk=HERE):
    """R246 addresses the source names: function pointer macros, link.ld symbols, Class::method (0x...) in comments,
    hook sites (make_firmware.py *HOOKS, with their comment)"""
    names = {}
    def add(a, n):
        if _code(a) and n not in names.setdefault(a, []): names[a].append(n)
    hexa = r'0x0?(1[89AB][0-9A-Fa-f]{5}|FFA[0-9A-Fa-f]{5})\b'
    files = [p for p in glob.glob(os.path.join(sdk, '**', '*.[ch]'), recursive=True)
             if not os.path.relpath(p, sdk).startswith(('build', 'harness'))]
    for p in sorted(files):
        with open(p, errors='replace') as f: lines = [l for l in f if '0x' in l]
        for line in lines:
            m = re.match(r'\s*#define\s+(\w+)(?:\([^)]*\))?\s.*\(\*\)[^;]*?\)\s*' + hexa, line)
            if m: add(int(m.group(2), 16), m.group(1))
            for m in re.finditer(r'\b([A-Za-z_]\w*(?:::~?\w+)+)\s*\(?' + hexa, line):
                add(int(m.group(2), 16), m.group(1))
    with open(os.path.join(sdk, 'link.ld')) as f: ld = f.read()
    for m in re.finditer(r'^\s*_(\w+) = ' + hexa + r';\s*/\*\s*(.*?)\s*\*/', ld, re.M):
        add(int(m.group(2), 16), f'{m.group(1)} ({m.group(3)})')
    with open(os.path.join(sdk, 'make_firmware.py')) as f: mf = f.read()
    for m in re.finditer(r'(0x[0-9A-F]{8}): \(\'(\w+)\', bytes\.fromhex\(\'[0-9a-f]+\'\)\)[,}]*\s*(?:#\s*([^#\n]*))?', mf):
        add(int(m.group(1), 16), f'{(m.group(3) or "").strip() or "hook site"} [{m.group(2)}]')
    return [[a, ' / '.join(n)] for a, n in sorted(names.items())]


def make(version, nm_text='', plugins=None, sdk=HERE):
    core, end = parse_nm(nm_text)
    return {'op1r': version, 'image_end': end, 'core': [list(s) for s in core],
            'plugins': {k[:15]: sorted(map(list, v)) for k, v in sorted((plugins or {}).items())}, 'stock': stock(sdk)}


def version(sdk=HERE):
    with open(os.path.join(sdk, 'VERSION')) as f: return f.read().strip()


def from_build(stems):
    """the map of the last build: core from build/synth.nms, plugins from build/plugins/<file>.funcs.json
    (make_plugin.py), stems = plugin file names without .op1p"""
    pl = {s: json.load(open(os.path.join(BUILD, 'plugins', s + '.funcs.json'))) for s in stems}
    return make(version(), open(os.path.join(BUILD, 'synth.nms')).read(), pl)


def write(path, m):
    with open(path, 'w') as f: json.dump(m, f, separators=(',', ':'))
    print(f'{path}: {os.path.getsize(path)} B ({len(m["core"])} core, {sum(map(len, m["plugins"].values()))} plugin, '
          f'{len(m["stock"])} R246 symbols)')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--stock', action='store_true', help='R246 names only')
    ap.add_argument('-o', default=os.path.join(os.path.dirname(HERE), 'op1_246_custom-symbols.json'))
    a = ap.parse_args()
    if a.stock: write(a.o, make(None))                    # (the same for every op1r version)
    else: write(a.o, from_build(sorted(os.path.basename(p)[:-11] for p in glob.glob(os.path.join(BUILD, 'plugins', '*.funcs.json')))))
