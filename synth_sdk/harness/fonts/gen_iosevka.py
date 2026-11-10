#!/usr/bin/env python3
"""Glyph outlines of Iosevka 34.9.0 (SIL OFL 1.1, harness/fonts/Iosevka-OFL.md) for the host previews (harness/iosevka.py).
Reads the four weights bundled with the op1-design skill (.agents/skills/op1-design/assets/fonts, woff2: needs
fontTools + brotli), applies the skill's character variants (profile.json 'features', GSUB alternates, as its
preview_font.py does) and writes printable ASCII per weight to iosevka_glyphs.json: {weight: {char: [advance,
[[op, coords...], ...]]}}, font units (1000 per em, cap height 735). Run once; the previews need only the JSON."""
import json, os, sys
from fontTools.ttLib import TTFont
from fontTools.pens.recordingPen import DecomposingRecordingPen
HERE = os.path.dirname(os.path.abspath(__file__))
SKILL = os.path.join(HERE, '..', '..', '..', '.agents', 'skills', 'op1-design', 'assets', 'fonts')

def main():
    prof = json.load(open(os.path.join(SKILL, 'profile.json')))
    out = {'version': prof['version'], 'capHeight': prof['capHeight'], 'unitsPerEm': prof['unitsPerEm'], 'weights': {}}
    for weight, asset in prof['fonts'].items():
        f = TTFont(os.path.join(SKILL, asset['file']))
        gs, cmap, sel = f.getGlyphSet(), f.getBestCmap(), {}
        for rec in f['GSUB'].table.FeatureList.FeatureRecord:      # the chosen variants (cvNN = alternate index)
            idx = prof['features'].get(rec.FeatureTag)
            if not idx: continue
            for li in rec.Feature.LookupListIndex:
                for raw in f['GSUB'].table.LookupList.Lookup[li].SubTable:
                    for src, alts in getattr(getattr(raw, 'ExtSubTable', raw), 'alternates', {}).items(): sel[src] = alts[idx - 1]
        g = {}
        for c in map(chr, range(32, 127)):
            if ord(c) not in cmap: continue
            name = sel.get(cmap[ord(c)], cmap[ord(c)])
            pen = DecomposingRecordingPen(gs); gs[name].draw(pen)        # composites resolved
            ops = []
            for op, pts in pen.value:
                if op in ('closePath', 'endPath'): ops.append(['Z']); continue
                if op == 'qCurveTo' and len(pts) > 2:              # implied on-curve points between off-curve pairs
                    for i in range(len(pts) - 2):
                        mid = ((pts[i][0] + pts[i + 1][0]) / 2, (pts[i][1] + pts[i + 1][1]) / 2)
                        ops.append(['Q', *pts[i], *mid])
                    ops.append(['Q', *pts[-2], *pts[-1]]); continue
                ops.append([{'moveTo': 'M', 'lineTo': 'L', 'qCurveTo': 'Q', 'curveTo': 'C'}[op]] + [v for p in pts for v in p])
            g[c] = [f['hmtx'][name][0], [[o[0]] + [round(v, 1) for v in o[1:]] for o in ops]]
        out['weights'][weight] = g
    json.dump(out, open(os.path.join(HERE, 'iosevka_glyphs.json'), 'w'), separators=(',', ':'))
    print('->', os.path.join(HERE, 'iosevka_glyphs.json'), {w: len(v) for w, v in out['weights'].items()})

if __name__ == '__main__':
    main()
