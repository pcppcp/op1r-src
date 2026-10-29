"""Plugin discovery: every plugins/<folder>/plugin.json is one synth engine, FX or sequencer.

plugin.json fields (paths relative to the plugin folder):
  name           type name shown on the OP-1: lowercase, <= 15 chars, no clash with built-in types
  kind           "synth", "fx", "seq" (sequencer, docs/44: op1_seq_t), "drum" (drum engine, docs/43: op1_drum_t) or
                 "tool" (op1_tool_t: keys, main loop, an overlay); seq / drum / tool are always plugin files
  author         (optional) who made it: shown in the web patcher (stored in the plugin file)
  order          (optional) the number of a plugin that was compiled into earlier releases (knobs[4] = 0x5300 + order,
                 1..31): kept so its old sounds work, never change it. Needed only to compile a plugin in (--builtin).
                 New plugins leave it out: sounds find a plugin file by its name (docs/37). Taken: plugins/ORDERS.md
  symbol         the op1_engine_t / op1_fx_t object in the sources
  sources        C files; the first one defines `symbol`
  defaults       initial 4 knob positions (0..32767)
  page2_defaults (fx, optional) initial page-2 knob positions
  uses_sample    (synth, optional) engine reads the slot's sampler recording (synth_<slot>.raw loaded at boot)
  stereo         (fx, optional) op1_fx_t.render_stereo is set: the master position renders left / right itself
  coarse         (synth, optional) knob numbers 0..3 turned in 8 steps over the range (both pages), e.g. a selector
  screen         (optional) {"svg": file name, must not clash with stock content/display, "symbol": op1_screen_t}
  source_cflags  (optional) {"file.c": ["-O3"]}: extra compiler flags for one source, keyed by its file name and
                 added after the default -O2. Only -O0 .. -O3 and -Os are accepted. For a hot loop that -O3 speeds
                 up; check that the output stays the same.
Orders 1..31 per kind (OP1_MAX_ORDER in op1_plugin.h).
"""
import json, os

HERE = os.path.dirname(os.path.abspath(__file__))
DIR = os.path.join(HERE, 'plugins')


def load():
    """-> (engines, effects) sorted by order; paths relative to the SDK root (as make_firmware expects)."""
    engs, fxs = [], []
    for d in sorted(os.listdir(DIR)):
        pj = os.path.join(DIR, d, 'plugin.json')
        if not os.path.isfile(pj):
            continue
        p = json.load(open(pj))
        rel = lambda f: os.path.join('plugins', d, f)
        e = {k: v for k, v in p.items() if k not in ('kind', 'sources')}
        e['source'], e['extra_sources'] = rel(p['sources'][0]), [rel(s) for s in p['sources'][1:]]
        if p.get('screen'):
            e['screen'] = dict(p['screen'], svg=rel(p['screen']['svg']))
        assert p['kind'] in ('synth', 'fx', 'seq', 'drum', 'sampler', 'tool'), pj
        e['dir'] = d
        if p['kind'] in ('seq', 'drum', 'sampler', 'tool'):
            continue
        (engs if p['kind'] == 'synth' else fxs).append(e)
    for lst in (engs, fxs):
        lst.sort(key=lambda e: (e.get('order') or 99, e['name']))
        orders = [e['order'] for e in lst if e.get('order')]
        assert len(set(orders)) == len(orders), f'duplicate plugin order: {orders}'
    return engs, fxs


def source_flags(e, srcs):
    """-> {source path: [flags]} for the sources `srcs` of plugin entry `e` (plugin.json "source_cflags", keyed by
    file name); a key that names no source, or a flag other than -O0..-O3 / -Os, is an error"""
    sf = e.get('source_cflags', {})
    names = {os.path.basename(s) for s in srcs}
    assert isinstance(sf, dict) and set(sf) <= names, f'{e["name"]}: source_cflags names a file that is not a source'
    for flags in sf.values():
        assert isinstance(flags, list) and all(f in ('-O0', '-O1', '-O2', '-O3', '-Os') for f in flags), \
            f'{e["name"]}: source_cflags takes -O0 .. -O3 and -Os only'
    return {s: sf.get(os.path.basename(s), []) for s in srcs}


def load_seqs(kind='seq'):
    """-> plugins of kind "seq" (or "drum", "tool"), sorted by folder; always built as plugin files"""
    out = []
    for d in sorted(os.listdir(DIR)):
        pj = os.path.join(DIR, d, 'plugin.json')
        if not os.path.isfile(pj):
            continue
        p = json.load(open(pj))
        if p['kind'] != kind:
            continue
        rel = lambda f: os.path.normpath(os.path.join('plugins', d, f))
        e = {k: v for k, v in p.items() if k not in ('kind', 'sources')}
        e['source'], e['extra_sources'], e['dir'] = rel(p['sources'][0]), [rel(s) for s in p['sources'][1:]], d
        if p.get('screen'):
            e['screen'] = dict(p['screen'], svg=rel(p['screen']['svg']))
        out.append(e)
    return out


if __name__ == '__main__':
    for e in sum(load(), []):
        print(e['order'], e['name'], e['source'], e.get('screen', {}).get('svg', '-'))
