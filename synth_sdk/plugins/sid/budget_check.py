#!/usr/bin/env python3
"""Bounded real Blackfin six-voice SID packet measurements, no stock firmware.
Run only in a coordinated emulator window. Packet counts are lower bounds on cycles.
"""
import argparse,hashlib,json,shutil,sys
from pathlib import Path
HERE=Path(__file__).resolve().parent;SDK=HERE.parents[1]
sys.path.insert(0,str(SDK/'harness'));import tier2 as T
ap=argparse.ArgumentParser();ap.add_argument('out',type=Path);ap.add_argument('--prepare-only',action='store_true');ap.add_argument('--quick',action='store_true');a=ap.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
found=T.setup_env()
if 'toolchain' not in found:raise SystemExit('Blackfin toolchain missing')
sys.path.insert(0,str(SDK.parent/'tools/kennel'));import pack
knl=out/'sid.knl';pack.pack_dir(HERE,knl)
original=T.HERE;fixture=out/'fixture';(fixture/'tier2').mkdir(parents=True,exist_ok=True)
for name in ('guest.c','crt.S','guest.ld'):shutil.copyfile(original/'tier2'/name,fixture/'tier2'/name)
p=fixture/'tier2/guest.c';source=p.read_text().replace('../../kennel/kn_load.h','kn_load.h').replace('../../op1_feature.h','op1_feature.h').replace('../../op1_input.h','op1_input.h');needle='        params[4] = (int16_t)(OP1_ENGINE_MAGIC + in.legacy_order);';assert source.count(needle)==1
source=source.replace(needle,'''        /* Labelled SID fixture: keep production loader/voice loop, select all six modes. */
        if ((t2_cfg.knobs >> 2)==6) {params[0]=8192;params[1]=16384;params[2]=24576;params[3]=8192;}
        else params[0]=(int16_t)((t2_cfg.knobs >> 2)*5462);
'''+needle);p.write_text(source)
T.HERE=fixture;T.OUT=out/'build';elf,_=T.build_guest(T.tools(found['toolchain']),'sid',knl)
if a.prepare_only:print(elf);raise SystemExit(0)
if 'emu' not in found:raise SystemExit('bcore emulator missing')
rows=[]
for mode in range(7):
 for setting in range(1 if mode==6 else 4):
  if a.quick and (mode,setting) not in [(2,0),(2,1),(3,0),(3,1),(6,0)]:continue
  runs=[T.run_guest(found['emu'],elf,3,n,mode=1,knobs=mode*4+setting) for n in (4,20)]
  if any('error' in r for r in runs):raise RuntimeError(runs)
  packets=(runs[1]['packets']-runs[0]['packets'])/(16*6)
  rows.append({'mode':'defaults' if mode==6 else mode,'endpoint_set':setting,'packets_per_voice':packets,'six_voice_packets':packets*6,'predicted_percent':15.1+.478*packets*6/1000})
report={'limits':'bcore packets exclude cache/SDRAM stalls; calibration is an estimate, not hardware acceptance','knl_sha256':hashlib.sha256(knl.read_bytes()).hexdigest(),'fixture_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'rows':rows,'worst':max(r['packets_per_voice'] for r in rows),'warnings':[r for r in rows if r['predicted_percent']>85]}
(out/'budget.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
assert all(r['packets_per_voice']<387000 and r['predicted_percent']<100 for r in rows)
