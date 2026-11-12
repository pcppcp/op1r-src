#!/usr/bin/env python3
"""Crash screen -> function names. After a crash the OP-1 shows (synth_sdk/crash.c)
    op1r <version> sp <stack pointer>
    and up to 10 code addresses from the crashed stack: plugin addresses as <file>+<offset>, the others raw.
Type the lines from a photo into a text file (or stdin) and decode them with the symbol map of that release
(make_firmware.py writes op1_246_custom-symbols.json, synth_sdk/symmap.py):

    op1crash.py [-m op1_246_custom-symbols.json] [screen.txt]

Without -m only the R246 addresses the source names are known (core and plugin addresses: region only). The
patcher's web/crash.html does the same in the browser (keep the two in step)."""
import argparse, bisect, json, os, re, sys

STOCK, L1, CORE = (0x01900000, 0x01B00000), (0xFFA00000, 0xFFA14000), (0x01E00000, 0x02000000)
NEAR = 0x800              # R246: names are entry points / call sites, not sizes: "near" up to this far past one
UNSIZED = 0x1000          # op1r asm labels without a size: up to the next symbol, at most this far


def parse(text):
    """screen text -> {'version', 'sp', 'addrs': [(token, file or None, value)]}"""
    v = re.search(r'op1r\s+(\d+)', text, re.I)
    sp = re.search(r'\bsp\s+(0x[0-9a-f]+)', text, re.I)
    out = []
    for m in re.finditer(r'([A-Za-z][\w-]*)\+(0x[0-9a-f]+)|\b(0x[0-9a-f]+)\b', text, re.I):
        if m.group(1):
            out.append((m.group(0), m.group(1), int(m.group(2), 16)))
        elif not (sp and m.start() == sp.start(1)):
            a = int(m.group(3), 16)
            if any(lo <= a < hi for lo, hi in (STOCK, L1, CORE)) and not a & 1:
                out.append((m.group(0), None, a))
    return {'version': v.group(1) if v else None, 'sp': int(sp.group(1), 16) if sp else None, 'addrs': out}


def lookup(syms, a):
    """syms: sorted [addr, size, name]; -> (name, offset) or None"""
    i = bisect.bisect_right([s[0] for s in syms], a) - 1
    if i < 0: return None
    s0, size, name = syms[i]
    nxt = syms[i + 1][0] if i + 1 < len(syms) else s0 + UNSIZED
    if a - s0 < (size if size else min(nxt - s0, UNSIZED)): return name, a - s0
    return None


def decode(m, file, a):
    """-> (where, text): where = 'plugin', 'op1r', 'R246' or '?'"""
    m = m or {}
    if file:
        pl = m.get('plugins', {})
        syms = pl.get(file[:15]) or next((v for k, v in pl.items() if k.startswith(file)), None)
        hit = syms and lookup(syms, a)
        return 'plugin', f'{file}: ' + (f'{hit[0]}+{hit[1]:#x}' if hit else f'+{a:#x}' + ('' if syms else ' (no symbols for this plugin)'))
    hit = lookup(m.get('core', []), a)
    if hit: return 'op1r', f'{hit[0]}+{hit[1]:#x}'
    if CORE[0] <= a < CORE[1]: return 'op1r', 'op1r core' + ('' if m.get('core') else ' (no symbol map)')
    st = m.get('stock', [])
    i = bisect.bisect_right([s[0] for s in st], a) - 1
    l1 = 'L1, ' if L1[0] <= a < L1[1] else ''
    if i >= 0 and a - st[i][0] <= NEAR: return 'R246', f'{l1}near {st[i][1]} +{a - st[i][0]:#x}'
    return 'R246', l1 + 'unnamed'


def report(text, m):
    p = parse(text)
    lines = [f'op1r {p["version"] or "?"}' + (f', sp {p["sp"]:#x}' if p['sp'] is not None else '')
             + ('' if not m.get('op1r') or m['op1r'] == p['version'] else f' (symbol map is op1r {m.get("op1r")})')]
    for tok, f, a in p['addrs']:
        where, t = decode(m, f, a)
        lines.append(f'{tok:<22} {where:<6} {t}')
    return '\n'.join(lines)


def stock_map():
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'synth_sdk'))
    import symmap
    return symmap.make(None)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('-m', '--map', help='symbol map (make_firmware.py: op1_246_custom-symbols.json)')
    ap.add_argument('screen', nargs='?', help='text from the crash screen (default: stdin)')
    a = ap.parse_args()
    text = open(a.screen).read() if a.screen else sys.stdin.read()
    print(report(text, json.load(open(a.map)) if a.map else stock_map()))
