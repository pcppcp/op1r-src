"""Compare audio against a release baseline and check sun colors/geometry."""
from pathlib import Path
import argparse,json,xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser()
ap.add_argument('baseline_source',type=Path)
ap.add_argument('baseline_validation',type=Path)
ap.add_argument('output',type=Path)
a=ap.parse_args();out=a.output
files=['vapora.c','vapora_dsp.c','vapora_dsp.h','vapora_params.h',
       'vapora_tables.h','vapora_mailbox.h','vapora_characters.h']
files+=sorted(str(p.relative_to(ROOT)) for p in (ROOT/'presets').glob('*') if p.is_file())
for f in files:
    assert (ROOT/f).read_bytes()==(a.baseline_source/f).read_bytes(),f
pcms=sorted(out.glob('*.pcm'));assert len(pcms)==19
for p in pcms:
    assert p.read_bytes()==(a.baseline_validation/p.name).read_bytes(),p.name
colors=['#ffffff','#ffa09f','#ff423f']
for design in range(1,8):
    shapes=[]
    for step in range(3):
        xml=ET.parse(out/f'pulse-{design:02d}-{step}.svg').getroot();paint=[]
        for e in xml.iter():
            key='stroke' if 'stroke' in e.attrib else 'fill' if 'fill' in e.attrib else None
            if key and e.get(key)!='#000000':paint.append(e.attrib.pop(key))
        assert paint and set(paint)=={colors[step]},(design,step,paint)
        shapes.append(ET.tostring(xml))
    assert len(set(shapes))==1,design
report=dict(baseline=json.loads((a.baseline_source/'plugin.json').read_text())['version'],unchanged_files=files,
            exact_pcm_fixtures=[p.name for p in pcms],
            all_seven_sun_colors_checked=True,depth_peak_colors=colors,
            sun_geometry_unchanged_across_depth=True,native_hardware_tested=False)
(out/'sun-pulse-regression.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS: audio sources/data and presets unchanged; all 19 PCM fixtures exact; all seven sun colors and invariant shapes checked.')
