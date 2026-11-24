#!/usr/bin/env python3
"""drumtrim (docs/89): synthetic self-test; with the stock tree present also every factory kit, and the patcher's
copy of apply() (op1r/patch_op1.py drumtrim) must give the same bytes."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import drumtrim as D

D._self_test()
tree = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'op1_246')
if os.path.exists(os.path.join(tree, 'content', 'op1_factory.db')):
    pat = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))), 'op1r')
    P = None
    if os.path.exists(os.path.join(pat, 'patch_op1.py')):
        sys.path.insert(0, pat); import patch_op1 as P
    for _, kit in D.kits(os.path.join(tree, 'content', 'op1_factory.db')):
        raw = open(os.path.join(tree, 'content', 'audio', 'factory_drum', kit['name'] + '.raw'), 'rb').read()
        segs, nk = D.plan(raw, kit)
        new = D.apply(raw, segs)
        D.check(raw, kit, new, nk)
        assert P is None or P.drumtrim(raw, segs) == new, kit['name']
print('drumtrim: ok')
