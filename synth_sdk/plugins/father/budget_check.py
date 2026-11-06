#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with OP-1 firmware permission.
"""Father-only mono/master costs. Run in a coordinated emulator queue slot."""
import argparse
import json
from pathlib import Path
import shutil
import sys
P=Path(__file__).resolve().parent
SDK=P.parents[1]
sys.path.insert(0,str(SDK/'harness'))
import fx_budget as B
T=B.T
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('out',type=Path)
ap.add_argument('--prepare-only',action='store_true')
ap.add_argument('--short',action='store_true')
ap.add_argument('--limit',type=int,help='stop after this many separate cases')
ap.add_argument('--source',type=Path,default=P,help='immutable plugin source snapshot')
a=ap.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
found=T.setup_env()
if 'toolchain' not in found:raise SystemExit('Blackfin toolchain missing')
fixture=out/'fixture';(fixture/'tier2').mkdir(parents=True,exist_ok=True)
for name in ('guest.c','crt.S','guest.ld'):shutil.copyfile(T.HERE/'tier2'/name,fixture/'tier2'/name)
p=fixture/'tier2/guest.c';s=p.read_text().replace('../../kennel/kn_load.h','kn_load.h').replace('../../op1_feature.h','op1_feature.h').replace('../../op1_input.h','op1_input.h')
start=s.index('        } else if (in.kind == KN_KIND_FX) {');end=s.index('        } else {',start)
p.write_text(s[:start]+B.FX_LOOP+s[end:]);T.HERE=fixture;T.OUT=out/'build'
knl=out/'father.knl';B.pack.pack_dir(a.source.resolve(),knl)
elf,_=T.build_guest(T.tools(found['toolchain']),'father',knl)
report=dict(knl_sha256=B.digest(knl),fixture_sha256=B.digest(p),elf_sha256=B.digest(elf),rows=[],limits='bcore packets exclude cache/SDRAM stalls; no hardware CPU acceptance')
if a.prepare_only:
    (out/'prepared.json').write_text(json.dumps(report,indent=2)+'\n');print(elf);raise SystemExit(0)
if 'emu' not in found:raise SystemExit('bcore missing')
windows=(4,20) if a.short else (2756,3012);report['windows']=windows
for pos in (0,2):
    for setting,label in ((4,'defaults'),(0,'min'),(1,'max')):
        counts=[B.run(found['emu'],elf,n,pos*8+setting) for n in windows]
        cost=(counts[1]-counts[0])/(windows[1]-windows[0])
        row=dict(position=pos,setting=label,packets_per_callback=cost,raw_packets=counts)
        report['rows'].append(row);print(json.dumps(row),flush=True)
        (out/'cost.json').write_text(json.dumps(report,indent=2)+'\n')
        if a.limit and len(report['rows'])>=a.limit:raise SystemExit(0)
report['status']='PASS' if all(r['packets_per_callback']<=35040 for r in report['rows']) else 'OVER_TARGET'
(out/'cost.json').write_text(json.dumps(report,indent=2)+'\n')
