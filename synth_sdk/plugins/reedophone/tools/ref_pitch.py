import numpy as np
from clar_common import *
regs = sfz_regions(); keyof = {}
for r in regs: keyof.setdefault(r['n'], set()).add((r['key'], r['lovel'], r['hivel']))
for n in range(1, 79):
    x = load(ref_path(n)); L = len(x)
    seg = x[int(0.3*SR):int(0.3*SR)+4096] if L > 0.5*SR else x[L//3:L//3+4096]
    f = f0_yin(seg, 130, 1800)
    m = midi_of(f)
    pk = np.abs(x).max(); e = rms_env(x)
    print(f"{n:02d} len {L/SR:4.2f}s f0 {f:7.2f} midi {m:6.2f} sfz {sorted(keyof.get(n,[]))} peak {20*np.log10(pk):5.1f} rmsmax {20*np.log10(e.max()):5.1f}")
