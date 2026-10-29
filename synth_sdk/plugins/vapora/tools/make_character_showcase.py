"""Fourteen selected Character styles and real-adapter note/chord impacts."""
from pathlib import Path
import json,subprocess,xml.etree.ElementTree as ET
from PIL import Image,ImageDraw
from render_scene import render
ROOT=Path(__file__).resolve().parents[1];KENNEL=ROOT.parents[2]
version=json.loads((ROOT/'plugin.json').read_text())['version']
out=KENNEL/f'build/vapora-{version}-character';out.mkdir(parents=True,exist_ok=True)
subprocess.run(['clang','-std=c11','-O1','-g','-fsanitize=address,undefined',
 '-fno-sanitize-recover=all','-DVAPORA_HOST','-I'+str(KENNEL/'synth_sdk'),'-I'+str(ROOT),
 str(ROOT/'tools/character_showcase.c'),str(ROOT/'vapora.c'),str(ROOT/'vapora_dsp.c'),
 str(ROOT/'vapora_screen.c'),'-lm','-o',str(out/'render')],check=True)
subprocess.run([str(out/'render'),str(out)],check=True)
names=['Original','Ripples','Roll','Fold','Orbit','Rings','Rain','Pulse','Tide','Ribbons','Weave','Dunes','Echo','Jelly']
sound_names=['Warm','Swarm','Breath','Flute','Pressure','Surf','Porcelain','Sub','Velvet','Cathedral','Nebula','Aurora','Oceanic','Glassveil']
all_frames=[];fingerprints=[]
for i,name in enumerate(names,1):
    frames=[];shapes=[]
    for n in range(64):
        path=out/f'mode-{i:02d}-{n:03d}.svg';xml=ET.parse(path).getroot()
        text=[e.text for e in xml.iter() if e.tag.endswith('text')]
        assert text==[f'CHARACTER  {i:02d} {sound_names[i-1].upper()}'],text
        shapes.append(tuple(tuple(sorted(e.attrib.items())) for e in xml.iter()
            if e.tag.endswith('line') and e.get('data-particle-layer')=='-1' and e.get('stroke-width')=='1.9043' and float(e.get('y1'))<126))
        frames.append(render(path))
    assert len(set(shapes))>30,(name,'not animated')
    fingerprint=tuple(shapes);assert fingerprint not in fingerprints;fingerprints.append(fingerprint)
    frames[0].save(out/f'{i:02d}-{name.lower()}.gif',save_all=True,append_images=frames[1:],duration=65,loop=0,disposal=2)
    frames[24].save(out/f'{i:02d}-{name.lower()}.png');all_frames.append(frames)
sheet=Image.new('RGB',(1320,784),(16,18,22));draw=ImageDraw.Draw(sheet)
for i,name in enumerate(names):
    x=5+(i%4)*330;y=5+(i//4)*196
    draw.text((x,y),f'{i+1:02d} {name.upper()}',fill='white');sheet.paste(all_frames[i][24],(x,y+23))
sheet.save(out/'all-characters.png')
# Five four-mode comparison reels keep native pixels legible.
for group in range(4):
    frames=[]
    for n in range(64):
        im=Image.new('RGB',(660,390),(16,18,22));d=ImageDraw.Draw(im)
        for slot in range(4):
            i=group*4+slot
            if i>=len(names):continue
            x=5+(slot%2)*330;y=5+(slot//2)*195
            d.text((x,y),f'{i+1:02d} {names[i].upper()}',fill='white');im.paste(all_frames[i][n],(x,y+25))
        frames.append(im)
    frames[0].save(out/f'compare-{group+1}.gif',save_all=True,append_images=frames[1:],duration=65,loop=0,disposal=2)
frames=[]
for i in range(96):
    im=Image.new('RGB',(320,184),(16,18,22));d=ImageDraw.Draw(im)
    label='ONE NOTE' if 8<=i<40 else 'ONE CHORD' if 40<=i<72 else 'SETTLED / HELD NOTES' if i>=72 else 'READY'
    d.text((5,5),label,fill='white');im.paste(render(out/f'trampoline-{i:03d}.svg'),(0,24));frames.append(im)
frames[0].save(out/'trampoline.gif',save_all=True,append_images=frames[1:],duration=65,loop=0,disposal=2)
footer=[]
for i in range(96):
    footer.append(render(out/f'footer-{i:03d}.svg'))
footer[0].save(out/'shift-toggle.gif',save_all=True,append_images=footer[1:],duration=65,loop=0,disposal=2)
for i in (0,31,32,63,64):footer[i].save(out/f'footer-{i:03d}.png')
(out/'preview.json').write_text(json.dumps(dict(version=version,characters=sound_names,wave_styles=names,morph_percent=75,footer_default=False,shift_toggle=True,
    frames_per_mode=64,production_commands=True,substitute_font='Menlo',hardware_tested=False,
    trampoline_note_events=2,trampoline_ambient_phase_fixed=True),indent=2)+'\n')
print(out)
