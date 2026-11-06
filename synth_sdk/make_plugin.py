#!/usr/bin/env python3
"""Build plugin files (.op1p) for the dynamic loader (docs/37, format: plugload/op1p.h).

    python3 make_plugin.py <plugin> [...]      -> build/plugins/<name>.op1p   (plugin = name in plugins/, or a folder)
    python3 make_plugin.py --all

The plugin is linked alone at address 0 (libgcc inside). Calls into the core go through generated stubs
(P1 = addr; JUMP (P1): heap and core are > 16 MB apart, out of PCREL24 range). The relocation list is checked by
linking a second time at another base: applying the list to the base-0 image must give that image byte for byte.
"""
import json, os, re, struct, subprocess, sys, zlib
from elftools.elf.elffile import ELFFile
import plugins

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'build', 'plugins')
CFLAGS = '-O2 -mcpu=bf524 -ffreestanding -fno-builtin -nostdlib -fno-common -Wall -I.'
CHECK_BASE = 0x0041A3C0
IMP_FAKE = 0x0E000000                       # import addresses for the test links only (never loaded)
OP1P_API = 1                                # = OP1_PLUGIN_API (op1_plugin.h)
OP1P_ABS32, OP1P_HI16, OP1P_LO16, OP1P_IMPORT = 1, 2, 3, 0x80000000
KINDS = {'synth': 0, 'fx': 1, 'seq': 2, 'drum': 3, 'sampler': 4, 'tool': 5}   # op1p.h OP1P_SYNTH / FX / SEQ / DRUM / SAMPLER / TOOL
R_LUIMM16, R_HUIMM16, R_BYTE4 = 6, 7, 0x12                 # binutils include/elf/bfin.h
R_PCREL = {1, 3, 4, 8, 9, 0xA, 0xD, 0xE, 0x13}           # pc-relative: fine inside the image only
LINK = """SECTIONS {{ . = {base:#x};
  .text : {{ *(.text .text.*) }}  .rodata : {{ *(.rodata .rodata.*) }}  .data : {{ *(.data .data.*) }}
  .bss : {{ *(.bss .bss.* COMMON) }}
  /DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }} }}
"""


def docker(script):
    subprocess.run(['docker', 'run', '--rm', '-u', f'{os.getuid()}:{os.getgid()}', '-v', f'{HERE}:/work',
                    'bfin-elf-toolchain', 'sh', '-ec', 'cd /work; ' + script], check=True)


def elf(path):
    return ELFFile(open(path, 'rb'))


def find(name):
    if os.path.isfile(os.path.join(name, 'plugin.json')):      # a plugin folder anywhere (relative to the SDK)
        p = json.load(open(os.path.join(name, 'plugin.json')))
        d = os.path.relpath(os.path.abspath(name), HERE)
        e = {k: v for k, v in p.items() if k not in ('kind', 'sources')}
        e['source'], e['extra_sources'] = os.path.normpath(os.path.join(d, p['sources'][0])), [os.path.normpath(os.path.join(d, x)) for x in p['sources'][1:]]
        e['dir'] = os.path.basename(os.path.abspath(name))
        if p.get('screen'):
            e['screen'] = dict(p['screen'], svg=os.path.normpath(os.path.join(d, p['screen']['svg'])))
        return KINDS[p['kind']], e
    engs, fxs = plugins.load()
    for kind, lst in ((0, engs), (1, fxs), (2, plugins.load_seqs()), (3, plugins.load_seqs('drum')), (4, plugins.load_seqs('sampler')), (5, plugins.load_seqs('tool'))):
        for e in lst:
            if name in (e['name'], e['dir']):
                return kind, e
    raise SystemExit(f'no plugin {name!r}')


def undefined(path):
    f = elf(path)
    syms = f.get_section_by_name('.symtab')
    und = {s.name for s in syms.iter_symbols() if s['st_shndx'] == 'SHN_UNDEF' and s.name}
    called = set()
    for sec in f.iter_sections():
        if sec.header['sh_type'] == 'SHT_RELA':
            for r in sec.iter_relocations():
                s = syms.get_symbol(r['r_info_sym'])
                if s.name in und and r['r_info_type'] in R_PCREL:
                    called.add(s.name)
    return und, called


def image(path):
    f = elf(path)
    secs = {n: f.get_section_by_name(n) for n in ('.text', '.rodata', '.data', '.bss')}
    base = secs['.text'].header['sh_addr']
    end = max(s.header['sh_addr'] + s.header['sh_size'] for n, s in secs.items() if s and n != '.bss')
    img = bytearray(end - base)
    for n, s in secs.items():
        if s and n != '.bss' and s.header['sh_size']:
            a = s.header['sh_addr'] - base
            img[a:a + s.header['sh_size']] = s.data()
    bss = secs['.bss']
    if bss and bss.header['sh_size']:
        assert bss.header['sh_addr'] - base >= len(img), '.bss inside the image'
        bss_size = bss.header['sh_addr'] - base - len(img) + bss.header['sh_size']
    else:
        bss_size = 0
    syms = {s.name: s['st_value'] - base for s in f.get_section_by_name('.symtab').iter_symbols() if s.name}
    return f, base, img, bss_size, syms


def relocs(f, base, n_img, imports):
    syms = f.get_section_by_name('.symtab')
    out = []
    for sec in f.iter_sections():
        if sec.header['sh_type'] != 'SHT_RELA' or sec.name not in ('.rela.text', '.rela.rodata', '.rela.data'):
            continue
        for r in sec.iter_relocations():
            t, s = r['r_info_type'], syms.get_symbol(r['r_info_sym'])
            off = r['r_offset'] - base
            target = s['st_value'] + r['r_addend']
            imp = s.name if s.name in imports else None
            if t in R_PCREL:
                if imp or not (base <= target < base + n_img):
                    raise SystemExit(f'pc-relative reference out of the plugin: {s.name} @ {off:#x}')
                continue
            kind = {R_BYTE4: OP1P_ABS32, R_HUIMM16: OP1P_HI16, R_LUIMM16: OP1P_LO16}.get(t)
            if kind is None:
                raise SystemExit(f'unsupported relocation type {t:#x} ({s.name} @ {off:#x})')
            if imp:
                out.append((off, kind, OP1P_IMPORT | imports.index(imp), r['r_addend']))
            elif s['st_shndx'] == 'SHN_ABS':
                raise SystemExit(f'absolute symbol {s.name} in a relocation')
            else:
                out.append((off, kind, target - base, 0))
    return sorted(out)


def apply(img, rel, base, imp_addr):
    img = bytearray(img)
    for off, kind, target, add in rel:
        v = (imp_addr[target & 0xFFFF] if target & OP1P_IMPORT else base + target) + add
        v &= 0xFFFFFFFF
        if kind == OP1P_ABS32:
            img[off:off + 4] = struct.pack('<I', v)
        else:
            img[off:off + 2] = struct.pack('<H', v >> 16 if kind == OP1P_HI16 else v & 0xFFFF)
    return img


def build(name, out_dir=OUT):
    kind, e = find(name)
    srcs = [e['source']] + e['extra_sources']
    tag = e.get('dir') or os.path.basename(os.path.dirname(e['source']))
    w = f'build/plugins/{tag}'
    os.makedirs(os.path.join(HERE, w), exist_ok=True)
    objs = ' '.join(f'{w}/{os.path.basename(s)}.o' for s in srcs)
    flags = plugins.source_flags(e, srcs)                         # plugin.json "source_cflags": e.g. -O3 after the default -O2
    cc = ' && '.join(f'bfin-elf-gcc {" ".join([CFLAGS, *flags[s]])} -c {s} -o {w}/{os.path.basename(s)}.o' for s in srcs)
    docker(f'{cc} && bfin-elf-ld -r {objs} $(bfin-elf-gcc -print-libgcc-file-name) -o {w}/all.r.o')
    und, called = undefined(os.path.join(HERE, w, 'all.r.o'))
    # imports: data symbols by their own name, functions via a stub that jumps to "__imp_<name>"
    stub = ''.join(f'\t.global {s}\n{s}:\tP1.L = __imp{s}; P1.H = __imp{s}; JUMP (P1);\n' for s in sorted(called))
    open(os.path.join(HERE, w, 'stubs.S'), 'w').write('\t.text\n' + stub)
    imports = sorted((und - called) | {f'__imp{s}' for s in called})
    defs = ' '.join(f'--defsym {s}={IMP_FAKE + 0x100 * i:#x}' for i, s in enumerate(imports))
    for b, fn in ((0, 'p0'), (CHECK_BASE, 'pb')):
        open(os.path.join(HERE, w, fn + '.ld'), 'w').write(LINK.format(base=b))
    docker(f'bfin-elf-as -mcpu=bf524 {w}/stubs.S -o {w}/stubs.o && '
           f'bfin-elf-ld -q -T {w}/p0.ld {defs} {w}/all.r.o {w}/stubs.o -o {w}/p0.elf 2>/dev/null && '
           f'bfin-elf-ld -q -T {w}/pb.ld {defs} {w}/all.r.o {w}/stubs.o -o {w}/pb.elf 2>/dev/null')
    f0, base0, img0, bss, syms = image(os.path.join(HERE, w, 'p0.elf'))
    rel = relocs(f0, base0, len(img0), imports)
    _, _, imgb, bssb, _ = image(os.path.join(HERE, w, 'pb.elf'))
    fake = [IMP_FAKE + 0x100 * i for i in range(len(imports))]
    assert bssb == bss and apply(img0, rel, CHECK_BASE, fake) == imgb, f'{tag}: relocation self-check failed'
    # header (op1p_hdr, 76 bytes)
    nm = e['name'].encode()
    assert 0 < len(nm) <= 15 and e['name'] == e['name'].lower(), e['name']
    scr = e.get('screen')
    defaults = (e.get('defaults') or [0] * 4)[:4]
    page2 = (e.get('page2_defaults') or [0] * 4)[:4] + [0] * (4 - len((e.get('page2_defaults') or [0] * 4)[:4]))
    hdr = struct.pack('<4I16s4I4h4h3I', 0x5031504F, 1, OP1P_API, kind, nm, len(img0), bss,
                      syms['_' + e['symbol']], syms['_' + scr['symbol']] if scr else 0xFFFFFFFF,
                      *defaults, *page2, (1 if e.get('uses_sample') else 0) | (2 if e.get('stereo') else 0) | (sum(1 << k for k in e.get('coarse', []) if 0 <= k < 4) << 16) | (e['order'] << 8 if 1 <= e.get('order', 0) < 32 else 0),
                      len(rel), len(imports))
    assert len(hdr) == 76
    names = [i[len('__imp_'):] if i.startswith('__imp_') else i.lstrip('_') for i in imports]
    exported = set(re.findall(r'\{ "(\w+)", \(void \*\)', open(os.path.join(HERE, 'plugload', 'plugload.c')).read()))
    assert set(names) <= exported, f'{tag}: imports the core does not export (plugload.c exports[]): {sorted(set(names) - exported)}'
    meta = ''.join(f'{k}={e[k]}\n' for k in ('author',) if e.get(k))   # optional text after the imports (op1p.h)
    assert all(c >= ' ' and c not in '\x7f' for c in meta.replace('\n', '')), 'metadata: printable text only'
    body = hdr + bytes(img0) + b''.join(struct.pack('<IIIi', *r) for r in rel) + \
        b''.join(n.encode() + b'\0' for n in names) + meta.encode()
    head = b''
    if scr:
        head = open(os.path.join(HERE, scr['svg']), 'rb').read()
        assert b'\0' not in head and head.lstrip().startswith(b'<'), scr['svg']
        head += b'\0' * (4 - len(head) % 4 if len(head) % 4 else 4)
    data = head + body + struct.pack('<4I', 0x5431504F, len(head), len(body), zlib.crc32(body))
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, f'{e["name"].replace(" ", "_").replace("+", "plus")}.op1p')
    open(out, 'wb').write(data)
    if out_dir == OUT:                                             # host test: image at CHECK_BASE
        open(out[:-5] + '.ref', 'wb').write(bytes(imgb) + bytes(bss))
    print(f'{out}: {len(data)} B (svg {len(head)}, image {len(img0)} + bss {bss}, {len(rel)} relocs, '
          f'imports {names})')
    return out


if __name__ == '__main__':
    args = sys.argv[1:]
    if args == ['--all']:
        args = [e['dir'] for e in sum(plugins.load(), []) + plugins.load_seqs() + plugins.load_seqs('drum') + plugins.load_seqs('sampler') + plugins.load_seqs('tool')]
    if not args:
        raise SystemExit(__doc__)
    for a in args:
        build(a)
