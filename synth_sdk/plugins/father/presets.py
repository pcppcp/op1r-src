#!/usr/bin/env python3
"""Create two original stock-Phase sounds differing only in name and fx_active.
Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with OP-1 firmware permission.
"""
import argparse
import json
from pathlib import Path
import struct
import zipfile

# Existing documented filter-slot ranges, also observed in Father native checks.
FILTER_RANGES=((64,14528),(0,24576),(0,22528),(8192,32767))
SELECTOR=0x5300  # documented neutral file-plugin fallback; resolved by fx_type name

def metadata(enabled):
    return dict(name='father on' if enabled else 'father off',type='phase',synth_version=2,
                octave=0,knobs=[8000,0,0,0,0,0,0,0],
                adsr=[64,16320,32767,16320,2048,5568,4000,4000],
                fx_type='father',fx_active=bool(enabled),fx_params=[7296,0,12390,15564,SELECTOR,0,0,0],
                lfo_type='tremolo',lfo_active=False,lfo_params=[16000,0,0,16000,0,0,0,0])

def chunk(name,data):return name+struct.pack('>I',len(data))+data+(b'\0' if len(data)&1 else b'')

def aifc(meta):
    # Exact80-bit44100Hz; zero preview frames are rendered by the OP-1 on import.
    rate=bytes.fromhex('400eac44000000000000')
    comm=struct.pack('>hIh',1,0,16)+rate+b'sowt'+b')Signed integer (little-endian) linear PCM'
    js=json.dumps(meta,sort_keys=True,separators=(',',':')).encode()+b'\n'
    body=chunk(b'FVER',bytes.fromhex('a2805140'))+chunk(b'COMM',comm)+chunk(b'APPL',b'op-1'+js)+chunk(b'SSND',bytes(8))
    return b'FORM'+struct.pack('>I',len(body)+4)+b'AIFC'+body

def parse(data):
    assert data[:4]==b'FORM' and data[8:12]==b'AIFC' and int.from_bytes(data[4:8],'big')==len(data)-8
    pos=12
    while pos<len(data):
        n=int.from_bytes(data[pos+4:pos+8],'big');body=data[pos+8:pos+8+n]
        if data[pos:pos+4]==b'APPL':assert body[:4]==b'op-1';return json.loads(body[4:])
        pos+=8+n+(n&1)
    raise ValueError('missing patch metadata')

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('out',type=Path);a=ap.parse_args()
    folder=a.out/'father_ab';folder.mkdir(parents=True,exist_ok=True)
    metas=[]
    for enabled in (False,True):
        meta=metadata(enabled);data=aifc(meta);assert parse(data)==meta
        assert all(lo<=v<=hi for v,(lo,hi) in zip(meta['fx_params'],FILTER_RANGES))
        (folder/('father-on.aif' if enabled else 'father-off.aif')).write_bytes(data);metas.append(meta)
    assert {k for k in metas[0] if metas[0][k]!=metas[1][k]}=={'name','fx_active'}
    text='''Father A/B — two identical stock Phase sounds\n\nRequires current Hundekraft 3.0 with father.knl installed.\nCopy the father_ab folder into synth/ on the OP-1 disk, then eject and let it sync.\nIn synth mode, Shift+1 opens the preset browser. Blue selects FATHER_AB; green\nselects FATHER-OFF or FATHER-ON; press 1 to load. Play the same note.\nBoth use identical voice/envelope/FX controls; only the name and saved effect on/off\nsetting differ. Father uses distance ~50%, gate off, color ~55%, mix ~30%.\nT3 first opens the Father effect page; pressing T3 again toggles bypass. Keep master FX off for a clear\ncomparison; synth presets do not save the global master effect setting. The files contain no stock audio; the OP-1 generates their previews.\nPhysical hardware performance is not established by emulator checks.\n'''
    (a.out/'README.txt').write_text(text)
    with zipfile.ZipFile(a.out/'father-ab.zip','w',zipfile.ZIP_DEFLATED) as z:
        for f in sorted(folder.glob('*.aif')):z.write(f,'father_ab/'+f.name)
        z.writestr('README.txt',text)
    print(a.out/'father-ab.zip')
if __name__=='__main__':main()
