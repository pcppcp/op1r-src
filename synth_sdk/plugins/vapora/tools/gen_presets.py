#!/usr/bin/env python3
"""AIFC writer adapted from Hundekraft EVA/MBIRA presets.py.
Copyright (C) 2026 Hundekraft and VAPORA contributors.
GPL-3.0-or-later with OP-1 firmware permission.
Format checked against the pinned SDK; import on hardware still pending.
"""
import csv
import json
import struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def chunk(cid,body):
    return cid+struct.pack(">I",len(body))+body+(b"\0" if len(body)&1 else b"")
def generate():
    out=ROOT/"presets";out.mkdir(exist_ok=True)
    definitions=json.loads((ROOT/"data/presets.json").read_text())
    core=json.loads((ROOT/"data/core_set.json").read_text())
    assert [p["name"] for p in definitions]==core["preset_names"]
    assert len({p["name"] for p in definitions})==len(definitions)
    characters=json.loads((ROOT/"data/voices.json").read_text())["characters"]
    assert [c["name"] for c in characters]==core["character_order"],"Do not remap saved Character assignments"
    for d in definitions:
        assert d["knobs"][5]==8192+256*core["character_order"].index(d["character"].lower())
    # Retire only known factory patches; preserve unknown user files.
    for name in ["blue hour","distant choir","sunlit voices"]+json.loads((ROOT/"data/retired_presets.json").read_text()):
        (out/(name+".aif")).unlink(missing_ok=True)
    recipes=[]
    for definition in definitions:
        name,knobs,adsr=(definition[k] for k in ("name","knobs","adsr"))
        assert all(0<=x<=32767 for x in knobs)
        assert all(1<=x<=32767 for x in knobs[5:])
        assert all(64<=adsr[k]<=16320 for k in (0,1,3))
        p=dict(type="vapora",name=name,knobs=knobs,adsr=adsr+[2048,5568,4000,4000],
               fx_active=False,fx_type="delay",fx_params=[8000]*4+[0]*4,
               lfo_active=False,lfo_type="tremolo",lfo_params=[16000,0,0,16000,0,0,0,0],
               octave=definition.get("octave",0),synth_version=2)
        if not definition.get("retained_from_r6"):assert len(name)<=10
        if definition.get("fx"):
            effect=definition["fx"];assert effect["type"] in ("reverb","plate")
            # Custom FX use the stock filter's raw ranges. Selector is repaired by name.
            ranges=[(64,14528),(0,24576),(0,22528),(8192,32767)]
            assert len(effect["q15"])==4 and all(0<=q<=32767 for q in effect["q15"])
            p.update(fx_active=True,fx_type=effect["type"],
                     fx_params=[lo+round(q*(hi-lo)/32767) for q,(lo,hi) in zip(effect["q15"],ranges)]+[0x5300,0,0,0])
        # 80-bit IEEE extended sample rate: 44100 = 1.3458251953125 * 2^15.
        rate=struct.pack(">HQ",16383+15,44100<<48)
        comm=struct.pack(">hIh",1,0,16)+rate+b"sowt"+b")Signed integer (little-endian) linear PCM"
        js=json.dumps(p,separators=(",",":"),sort_keys=True).encode()+b"\n"
        body=chunk(b"FVER",bytes.fromhex("a2805140"))+chunk(b"COMM",comm)+chunk(b"APPL",b"op-1"+js)+chunk(b"SSND",bytes(8))
        data=b"FORM"+struct.pack(">I",len(body)+4)+b"AIFC"+body
        (out/(name+".aif")).write_bytes(data)
        # Parse the container again; catch alignment/length/JSON identity errors.
        assert struct.unpack_from(">I",data,4)[0]+8==len(data)
        pos=12;found=None
        while pos<len(data):
            tag=data[pos:pos+4];n=struct.unpack_from(">I",data,pos+4)[0]
            part=data[pos+8:pos+8+n];assert len(part)==n
            if tag==b"APPL":assert part[:4]==b"op-1";found=json.loads(part[4:])
            pos+=8+n+(n&1)
        assert pos==len(data) and found==p
        recipes.append(p)
    (out/"recipes.json").write_text(json.dumps(recipes,indent=2)+"\n")
    (ROOT/"tests/preset_words.h").write_text(
        "/* Generated from the same recipes as the AIFC files; host fixtures only. */\n"
        f"enum {{ PRESET_COUNT={len(recipes)} }};\nstatic const int16_t preset_words[PRESET_COUNT][8]={{\n"+
        "\n".join("{"+",".join(map(str,p["knobs"]))+"}," for p in recipes)+"\n};\n"+
        "static const int8_t preset_octave[PRESET_COUNT]={"+",".join(str(d.get("octave",0)) for d in definitions)+"};\n"+
        "static const int32_t preset_preview[PRESET_COUNT][4]={\n"+
        "\n".join("{"+",".join(map(str,d.get("preview_envelope_ms",[1500,0,32767,2500])))+"}," for d in definitions)+"\n};\n"+
        "static const char *const preset_names[PRESET_COUNT]={\n"+
        ",\n".join(json.dumps(p["name"].replace(" ","-")) for p in recipes)+"\n};\n")
    with (ROOT/"PRESET_CATALOG.csv").open("w",newline="") as f:
        writer=csv.writer(f,lineterminator="\n")
        writer.writerow(["Number","Name","Group","Character","Interval semitones","Description","Rating 1-5","Notes"])
        for i,p in enumerate(definitions,1):
            writer.writerow([i,p["name"],p["group"],p.get("character","WARM"),p["interval_semitones"],p["description"],"",""])
    print(f"{len(recipes)} AIFC presets generated and round-trip checked")
if __name__=="__main__":generate()
