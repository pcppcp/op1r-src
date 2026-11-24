#!/usr/bin/env python3
"""TE's factory drum kits without their silent tails (docs/89; after op1-sdk's drumtrim, MIT, BertBrandstifter).

Every hit is cut PAD frames after its last sample above -60 dB of its own peak, those PAD frames fade out, silent
gaps between hits are dropped and each key's start / end move to match (positions are frames x UNIT, sub-frame
offsets kept). Keys with a non-default playmode or reverse stay whole (their end can be a loop point). Every key
plays its stock audio bit for bit up to the fade. Pure Python: the patcher (patch_op1.py, also in the browser)
only replays the segment list of plan() through apply(), no TE audio is shipped.

    python3 drumtrim.py <unpacked firmware tree>      self-check on the stock kits (sizes, identity per key)
"""
import json, os, sqlite3, sys
from array import array

UNIT = 4058
PAD = 64
FLOOR = 1000                                             # -60 dB: |x| * FLOOR > peak


def plan(raw, kit):
    """stock kit audio (bytes, int16 LE mono) + kit JSON -> (segments [[f0, f1, cut]], new kit JSON)"""
    x = array('h', raw)
    if sys.byteorder != 'little':
        x.byteswap()
    keys = sorted(range(len(kit['start'])), key=lambda k: kit['start'][k])
    segs, outs, start, end = [], [], list(kit['start']), list(kit['end'])
    out_len = 0
    for k in keys:
        s, e = kit['start'][k], kit['end'][k]
        f0, f1 = s // UNIT, min(len(x) - 1, e // UNIT)
        hit = x[f0:f1 + 1]
        peak = max(max(hit), -min(hit)) if len(hit) else 0
        last = f1
        while last >= f0 and abs(x[last]) * FLOOR <= peak:  # last loud frame (the tail is short to walk back)
            last -= 1
        last = max(last, f0) + PAD
        plain = kit['playmode'][k] == 8192 and kit['reverse'][k] == 8192    # TE's defaults
        if last < f1 and plain:
            e = min(e, last * UNIT + UNIT - 1)
            f1 = last
        cut = int(f1 < min(len(x) - 1, kit['end'][k] // UNIT))
        if segs and f0 <= segs[-1][1]:                   # shares frames with the previous hit
            f0 = segs[-1][1] + 1
        if f0 <= f1:
            segs.append([f0, f1, cut]); outs.append(out_len)
            out_len += f1 - f0 + 1
        fr = s // UNIT                                   # the kept segment holding the key's start (check() proves
        i = max(j for j in range(len(segs)) if segs[j][0] <= fr)   # its whole range is kept)
        shift = segs[i][0] - outs[i]
        start[k], end[k] = s - shift * UNIT, e - shift * UNIT
    nk = dict(kit)
    nk['start'], nk['end'] = start, end
    return segs, nk


def apply(raw, segs):
    """stock kit audio + segments -> trimmed audio (bytes); the only part the patcher needs (same code there)"""
    out = bytearray()
    for f0, f1, cut in segs:
        seg = bytearray(raw[2 * f0:2 * f1 + 2])
        if cut and len(seg) > 2 * PAD:                   # fade the kept near-silence: v * (PAD-1-i) / (PAD-1), rounded
            n = len(seg) // 2 - PAD
            for i in range(PAD):
                v = int.from_bytes(seg[2 * (n + i):2 * (n + i) + 2], 'little', signed=True) * (PAD - 1 - i)
                v = (abs(v) + (PAD - 1) // 2) // (PAD - 1) * (1 if v >= 0 else -1)
                seg[2 * (n + i):2 * (n + i) + 2] = v.to_bytes(2, 'little', signed=True)
        out += seg
    return bytes(out)


def check(raw, kit, new, nk):
    """every key's stock audio identical up to its fade, ends only earlier, offsets kept, inside the file"""
    for k in range(len(kit['start'])):
        a, b = kit['start'][k] // UNIT, nk['start'][k] // UNIT
        n = nk['end'][k] // UNIT - b + 1 - PAD
        assert n > 0 and raw[2 * a:2 * (a + n)] == new[2 * b:2 * (b + n)], f"{kit['name']} key {k}"
        assert nk['end'][k] - nk['start'][k] <= kit['end'][k] - kit['start'][k], f"{kit['name']} key {k} longer"
        assert nk['start'][k] % UNIT == kit['start'][k] % UNIT and 0 <= nk['start'][k] <= nk['end'][k]
    assert max(nk['end']) // UNIT < len(new) // 2


def kits(db):
    """(id, kit JSON) of the factory drum kits in op1_factory.db"""
    c = sqlite3.connect(db)
    rows = [(i, json.loads(p)) for i, p in c.execute("SELECT id, patch FROM drum_presets WHERE folder='drum'")]
    c.close()
    return [(i, k) for i, k in rows if k.get('type') == 'drum']


def _same_width(text, kit, nk, key):
    """the kit's "key":[...] array rewritten with every number right-aligned in its old width (JSON allows the
    spaces): the row keeps its length, so sqlite updates it in place and the db changes only in those bytes"""
    old = '"%s":[%s]' % (key, ','.join(map(str, kit[key])))
    new = '"%s":[%s]' % (key, ','.join(str(v).rjust(len(str(o))) for v, o in zip(nk[key], kit[key])))
    assert len(new) == len(old) and text.count(old) == 1, (kit['name'], key)
    return text.replace(old, new)


def trim_tree(tree):
    """trim the kits of an unpacked firmware tree in place (audio + the start / end of their drum_presets rows);
    returns ({file: segments} for the patcher, stock bytes, trimmed bytes)"""
    dbp = os.path.join(tree, 'content', 'op1_factory.db')
    out, before, after = {}, 0, 0
    c = sqlite3.connect(dbp)
    for pid, text in c.execute("SELECT id, patch FROM drum_presets WHERE folder='drum'").fetchall():
        kit = json.loads(text)
        if kit.get('type') != 'drum':
            continue
        rel = f"content/audio/factory_drum/{kit['name']}.raw"
        p = os.path.join(tree, rel)
        raw = open(p, 'rb').read()
        segs, nk = plan(raw, kit)
        new = apply(raw, segs)
        check(raw, kit, new, nk)
        open(p, 'wb').write(new)
        t2 = _same_width(_same_width(text, kit, nk, 'start'), kit, nk, 'end')
        assert json.loads(t2) == nk
        assert c.execute("UPDATE drum_presets SET patch=? WHERE id=? AND folder='drum'", (t2, pid)).rowcount == 1
        out[rel] = segs
        before += len(raw); after += len(new)
    c.commit(); c.close()
    return out, before, after


def _self_test():
    """synthetic kit: two hits with silent tails, a looped key, a key sharing the first hit"""
    sil = [0] * 3000
    h1 = [1000 * ((-1) ** i) for i in range(500)] + [1] * 200 + sil
    h2 = [-20000] * 300 + [10] * 100 + sil
    raw = array('h', h1 + h2)
    if sys.byteorder != 'little':
        raw.byteswap()
    raw = raw.tobytes()
    n1, n2 = len(h1), len(h2)
    kit = {'name': 't', 'start': [0, n1 * UNIT + 7, 0], 'end': [(n1 - 1) * UNIT, (n1 + n2 - 1) * UNIT, 600 * UNIT],
           'playmode': [8192, 8192, 8192], 'reverse': [8192, 8192, 8192]}
    segs, nk = plan(raw, kit)
    new = apply(raw, segs)
    check(raw, kit, new, nk)
    assert len(new) // 2 == (500 + PAD) + (300 + PAD), len(new) // 2      # tails gone, the -60 dB floor kept
    assert nk['start'][1] % UNIT == 7
    loop = dict(kit, playmode=[8193, 8192, 8192])
    segs, nk = plan(raw, loop)
    check(raw, loop, apply(raw, segs), nk)
    assert nk['end'][0] == kit['end'][0]                  # a looped key keeps its end


if __name__ == '__main__':
    _self_test()
    if len(sys.argv) > 1:
        import shutil, tempfile
        t = tempfile.mkdtemp()
        os.makedirs(os.path.join(t, 'content', 'audio'))
        shutil.copy(os.path.join(sys.argv[1], 'content', 'op1_factory.db'), os.path.join(t, 'content'))
        shutil.copytree(os.path.join(sys.argv[1], 'content', 'audio', 'factory_drum'),
                        os.path.join(t, 'content', 'audio', 'factory_drum'))
        segs, a, b = trim_tree(t)
        print(f'{len(segs)} kits: {a / 1048576:.2f} MB -> {b / 1048576:.2f} MB (-{(a - b) / 1048576:.2f} MB)')
        shutil.rmtree(t)
    print('drumtrim: ok')
