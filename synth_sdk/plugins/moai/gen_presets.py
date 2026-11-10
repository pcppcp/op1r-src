#!/usr/bin/env python3
"""Generate moai_presets.h (and docs/PRESETS.md) from docs/modern_patch_library.csv
(run: python3 gen_presets.py; `make tables` runs it too).

The presets are stored after the 32 setups: first EXTRA (made here), then the library shuffled once with SEED; a
saved sound holds that entry. The home view's blue knob numbers them: EXTRA first (000: the initial sound, plugin.json's
defaults; 001 .. 004: Chase), then the setups and the library together in a second fixed shuffle (ORDER_SEED). The
plugin shows only the number; docs/PRESETS.md is the key (names from the CSV and from the setups' comments in
moai.h).

What a CSV patch becomes:
  - osc1..osc3 "16' saw", "LO square", "" (off); intervals in semitones (beyond +-8 the range moves an octave);
  - cutoff, emphasis, contour, detune, drive, saturation, contour amount: 0..100 = the knob's percent;
  - volumes (not in the CSV): osc 1 full, osc 2 full in the same range and 3/4 otherwise, osc 3 3/4; a few patches
    whose description asks for a quiet layer get 1/2 (VOL below);
  - osc 3 "audio-rate" modulation (FM) is not in this engine: that osc 3 is off; "optional vibrato" / "slow OSC3 ->
    pitch" and the LO patch get osc 3 in LO as the modulator (MOD below);
  - glide is not in this engine (ignored).

The second pass (v3.14), measured by rendering each patch (A-weighted level at C3 for basses, C4 for leads):
  - a patch whose osc 1 is 32' sounded at 16 .. 33 Hz in its own play range, below what headphones (let alone the
    OP-1's speaker) give: its oscillators move up an octave, and its cutoff up as much (+10: the same tone);
  - the darkest ones then get their cutoff raised until they reach about the level of the Chase basses (LIFT; the
    round / sub / dub ones less, Pure Sub not at all: a sub is the point);
  - near-duplicates are left out (DROP).
"""
import csv, os, random, re

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, 'docs', 'modern_patch_library.csv')
OUT = os.path.join(HERE, 'moai_presets.h')
DOC = os.path.join(HERE, 'docs', 'PRESETS.md')
SEED = 1970                                    # the Minimoog's year: the order never changes
ORDER_SEED = 1991                              # Can We Live's year: the numbers on the knob never change
SRC = os.path.join(HERE, 'moai.h')

RANGES = ["LO", "32'", "16'", "8'", "4'", "2'"]
RNAME = {"LO": 'R_LO', "32'": 'R32', "16'": 'R16', "8'": 'R8', "4'": 'R4', "2'": 'R2'}
WAVES = {'triangle': 'W_TRI', 'saw': 'W_SAW', 'square': 'W_SQUARE', 'wide pulse': 'W_WIDE', 'narrow pulse': 'W_NARROW'}
VOL = {1: (4, 2, 0), 9: (4, 2, 0), 43: (4, 2, 0)}          # patch id -> volumes in quarters (0: from the rule)
# patch id -> (osc 3 as (range, wave, semitones, volume) or None, mod osc %, mod filter %)
MOD = {
    59: (("LO", 'triangle', 1, 0), 15, 0),    # optional vibrato: ~5 Hz at C4, +-27 cents
    66: (None, 0, 45),                        # subsonic motor: osc 3 LO square, audible, and into the cutoff
    69: (("LO", 'triangle', 1, 0), 12, 0),    # fusion lead: the mod wheel's vibrato
}

# presets not from the CSV, stored first and numbered 000 ..: (name, category, oscillators, volumes, knobs as in k below)
EXTRA = [
    # Jestofunk, "Can We Live" (1991): the bass that sweeps on each bar, matched against the record (the resonance
    # from ~1 kHz down to ~270 Hz in 0.6 s on D#1) with the Imaginando tutorial's layout: two 16' saws (osc 2 ~3 cents
    # up), a 32' square sub at full. The plugin's initial sound (plugin.json defaults).
    ('Jestofunk - Can We Live bass (the initial sound)', 'Bass',
     [("16'", 'saw', 0), ("16'", 'saw', 0), ("32'", 'square', 0)], [4, 3, 4], (12, 80, 66, 27, 25, 0, 70, 0, 0)),
    # Giorgio Moroder, "Chase" (1978): Minimoog basses (the lead was a Roland SH-2000, the strings a Solina). The
    # basses matched against the record's intro (C3 / C2 notes on 8ths): the left one an 8' saw over a 16' triangle with
    # a little 4' saw, a resonant blip around 500 Hz that closes in ~0.1 s; the right one darker and rounder.
    ('Chase - bass (left)', 'Bass',
     [("8'", 'saw', 0), ("16'", 'triangle', 0), ("4'", 'saw', 0)], [4, 3, 2], (40, 60, 25, 20, 20, 0, 30, 0, 0)),
    ('Chase - bass (right, rounder)', 'Bass',
     [("8'", 'triangle', 0), ("16'", 'saw', 0), None], [4, 3, 0], (40, 20, 40, 20, 20, 0, 40, 0, 0)),
    ('Chase - lead (bright saws, slow vibrato)', 'Lead',
     [("8'", 'saw', 0), ("8'", 'saw', 0), ('LO', 'triangle', -1)], [4, 3, 0], (62, 25, 45, 35, 30, 10, 35, 10, 0)),
    ('Chase - strings (Solina-like swell)', 'Pad',
     [("8'", 'saw', 0), ("8'", 'saw', 0), ("4'", 'saw', 0)], [4, 4, 2], (58, 10, 90, 60, 0, 0, 20, 0, 0)),
]

# CSV patch id -> a preset made here in its place (the same entry and number on the knob): (name, category,
# oscillators, volumes, knobs as in EXTRA)
REPLACE = {
    # 036, was Siren Glide (an FX nobody plays twice): French touch, the filtered disco chords of Stardust, Alan Braxe,
    # Cassius. Two 8' saws spread by the detune and a 16' saw under them, the cutoff low with a firm emphasis, a short
    # contour that opens each stab, drive and saturation for the grit of a sampled loop. Play chords on the off-beats.
    65: ('French touch - filter house chords', 'Keys',
         [("8'", 'saw', 0), ("8'", 'saw', 0), ("16'", 'saw', 0)], [4, 4, 2], (46, 50, 38, 22, 30, 15, 50, 0, 0)),
}

def read_setups():
    """the setups' names and groups, from their comments in moai.h (the plugin has no names)"""
    src = open(SRC, encoding='utf-8').read()
    body = src[src.index('const setup_t SETUPS[N_SETUPS] = {'):]
    body = body[:body.index('\n};')]
    out, group = [], ''
    for l in body.split('\n')[1:]:
        g = re.match(r'^    /\* (.*) \*/$', l)
        if g:
            group = g.group(1)
            continue
        m = re.search(r'/\* ([A-Z0-9 ]+?)(:.*)? \*/$', l)
        assert m, l
        out.append((m.group(1), group))
    n = int(re.search(r'#define N_SETUPS (\d+)', src).group(1))
    assert len(out) == n, (len(out), n)
    return out

# CSV patch id -> its twin's: too close to keep both (the rendered spectra over the note within about 3 dB, the same
# oscillators or nearly): left out
DROP = {
    34: 72,     # Motorik Sequence        ~ Kraftwerk-ish Sequence
    2: 44,      # Warehouse Punch         ~ Fat Moog Bass
    76: 55,     # Classic Portamento Lead ~ Portamento Saw Lead
    52: 78,     # Filtered Drone Bass     ~ Classic Root-Fifth Drone
    12: 50,     # Hardgroove Bass         ~ Bass Guitar-ish
    42: 1,      # Sub + Bite              ~ Berlin Sub
    79: 22,     # Classic Square Bass     ~ Funky Square Bass
    83: 41,     # Clean Mono Sub          ~ Pure Sub
    9: 43,      # Dub Techno Round        ~ Round Triangle Bass
    51: 7,      # Growl Bass              ~ Industrial Growl
}
# patch id -> its cutoff (after the octave rule): raised until it is heard (bass at C3 about -27 dB(A) at velocity
# 110, the round / sub / dub ones -31, as the Chase basses -24 / -27; MOOG BS at its starting point -28)
LIFT = {
    1: 46,      # Berlin Sub 24 (+10 octave)
    4: 35,      # Dark Roller 28
    5: 51,      # Rumble Seed 21 (+10 octave)
    8: 37,      # Resonant Pump 30
    10: 54,     # Minimal Click Bass 26
    16: 40,     # Overloaded Drone 32
    17: 35,     # Deep House Round 29
    22: 52,     # Funky Square Bass 48
    23: 35,     # Garage Sub Pluck 22 (+10 octave)
    28: 40,     # Lo-Fi House Moog 31
    39: 49,     # Reese-ish Mono 39
    40: 38,     # Soft Detuned Bass 33
    43: 51,     # Round Triangle Bass 27
    44: 42,     # Fat Moog Bass 37
    49: 47,     # Muted Bass 23
    85: 55,     # Rolling Psy Bass 39
    89: 38,     # Dub Rounder 20 (+10 octave)
}

def osc(text, semi):
    """'16\\' saw' -> (range, wave, semitones) with the interval folded into +-8 semitones; None = off"""
    text = text.strip()
    if not text:
        return None
    rng, wave = text.split(' ', 1)
    r = RANGES.index(rng)
    while semi > 8 and r < len(RANGES) - 1:
        semi -= 12; r += 1
    return RANGES[r], wave, semi

def main():
    rows = list(csv.DictReader(open(CSV, encoding='utf-8')))
    presets = []
    for row in rows:
        pid = int(row['patch_id'])
        if pid in DROP:
            continue
        if pid in REPLACE:
            name, cat, o, vols, k = REPLACE[pid]
            presets.append(dict(id=0, name=name, cat=cat, osc=o, vol=vols, k=k))
            continue
        o = [osc(row['osc1'], 0), osc(row['osc2'], int(row['osc2_interval_semitones'] or 0)),
             osc(row['osc3'], int(row['osc3_interval_semitones'] or 0))]
        if 'audio-rate' in row['modulation_or_lo_use']:
            o[2] = None                                              # FM: not here
        vols = [4, 4 if o[1] and o[0] and o[1][0] == o[0][0] else 3, 3]
        if pid in VOL:
            vols = [v or d for v, d in zip(VOL[pid], vols)]
        pit = flt = 0
        if pid in MOD:
            m3, pit, flt = MOD[pid]
            if m3:
                o[2] = m3[:3]; vols[2] = m3[3]
            elif o[2] and o[2][0] == 'LO':
                vols[2] = 2                                          # the motor: heard, and moving the filter
        p = lambda k: int(row[k] or 0)
        cut = p('cutoff_0_100')
        if o[0] and o[0][0] == "32'":                                # an octave up, the cutoff with it
            o = [(RANGES[RANGES.index(x[0]) + 1],) + x[1:] if x and x[0] != 'LO' else x for x in o]
            cut += 10
        cut = LIFT.get(pid, cut)
        presets.append(dict(id=pid, name=row['patch_name'], cat=row['category'], osc=o, vol=vols,
                            k=(cut, p('emphasis_0_100'), p('contour_0_100'), p('detune_0_100'),
                               p('drive_0_100'), p('saturation_0_100'), p('contour_amount_0_100'), pit, flt)))
    random.Random(SEED).shuffle(presets)
    presets = [dict(id=0, name=name, cat=cat, osc=o, vol=vols, k=k) for name, cat, o, vols, k in EXTRA] + presets

    # the numbers on the knob: EXTRA first, then the setups and the library shuffled together
    setups = read_setups()
    ent = [dict(kind='setup', id=0, name=n, cat=c) for n, c in setups] + presets
    first = len(setups)                                            # EXTRA[0], the initial sound (plugin.json)
    rest = [e for e in range(len(ent)) if not first <= e < first + len(EXTRA)]
    random.Random(ORDER_SEED).shuffle(rest)
    order = list(range(first, first + len(EXTRA))) + rest
    pos = [0] * len(ent)
    for n, e in enumerate(order):
        pos[e] = n

    lines = ['/* generated by gen_presets.py from docs/modern_patch_library.csv: do not edit.',
             ' * The presets, stored after the setups (entries %d ..). Per preset: oscillators (range, wave, interval, volume' % len(setups),
             ' * in quarters), then in % of the knob: cutoff, emphasis, contour, detune, drive, saturation, contour amount, mod',
             ' * osc, mod filter (osc 3 in LO is the source). The comment: its number on the knob. */',
             '#define N_PRESETS %d' % len(presets),
             '#define N_ORDER %d' % len(order),
             '#define MM_INITIAL %d   /* 000, the initial sound (plugin.json) */' % first,
             '/* the numbers on the knob (000 ..): MM_ORDER[number] = entry, MM_POS[entry] = number */',
             'extern const preset_t PRESETS[N_PRESETS];',
             'extern const uint8_t MM_ORDER[N_ORDER], MM_POS[N_ORDER];',
             '#ifdef MM_DEFINE_TABLES                         /* the one copy: moai.c (moai.h) */',
             'const preset_t PRESETS[N_PRESETS] = {']
    for n, pr in enumerate(presets):
        os_ = []
        for o, v in zip(pr['osc'], pr['vol']):
            os_.append('{ %s, %s, %d, %d }' % (RNAME[o[0]], WAVES[o[1]], o[2], v) if o else 'OFF_')
        src = 'CSV %d, %s, %s' % (pr['id'], pr['cat'], pr['name']) if pr['id'] else pr['name']
        lines.append('    { { %s }, %s },   /* %03d: %s */' % (', '.join(os_), ', '.join(map(str, pr['k'])), pos[len(setups) + n], src))
    lines.append('};')
    for name, a in (('MM_ORDER', order), ('MM_POS', pos)):
        lines.append('const uint8_t %s[N_ORDER] = {' % name)
        for k in range(0, len(a), 20):
            lines.append('    ' + ', '.join(map(str, a[k:k + 20])) + ',')
        lines.append('};')
    lines.append('#endif')
    open(OUT, 'w').write('\n'.join(lines) + '\n')

    doc = ['# Sounds', '',
           'The home view\'s blue knob: %d sounds, 000 .. %03d, in a fixed shuffled order (it wraps around at the ends). The' % (len(order), len(order) - 1),
           'plugin shows only the number; this is the key (generated by `plugin/gen_presets.py`). Each is a whole sound',
           '(since v3.19 a setup too: its values are in plugin/README.md). A **setup** is one of the 32 oscillator setups; a',
           '**preset** comes from [the library](modern_patch_library.csv) (the CSV has each patch\'s description',
           'and play range), or was made here.', '',
           '| # | name | category | kind |', '|---|---|---|---|']
    for n, e in enumerate(order):
        x = ent[e]
        kind = 'setup' if x.get('kind') == 'setup' else ('preset, CSV %d' % x['id'] if x['id'] else 'preset, made here')
        doc.append('| %03d | %s | %s | %s |' % (n, x['name'], x['cat'], kind))
    open(DOC, 'w').write('\n'.join(doc) + '\n')
    print('-> %s (%d setups + %d presets), %s' % (OUT, len(setups), len(presets), DOC))

if __name__ == '__main__':
    main()
