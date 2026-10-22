from pathlib import Path
import json,xml.etree.ElementTree as ET
from PIL import Image,ImageDraw,ImageFont,ImageChops,ImageStat
import argparse,subprocess,sys
ROOT=Path(__file__).resolve().parents[1]
KENNEL=ROOT.parents[2]
ap=argparse.ArgumentParser(description="Render the idle 1.2 Holy Dust showcase; no note events.")
ap.add_argument('--output',type=Path,default=KENNEL/'build/vapora-1.2-showcase')
root=ap.parse_args().output.resolve();(root/'frames').mkdir(parents=True,exist_ok=True)
subprocess.run(['clang','-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-DVAPORA_HOST',
    '-I'+str(KENNEL/'synth_sdk'),'-I'+str(ROOT),str(ROOT/'tools/showcase.c'),str(ROOT/'vapora.c'),str(ROOT/'vapora_dsp.c'),str(ROOT/'vapora_screen.c'),'-lm','-o',str(root/'render')],check=True)
subprocess.run([str(root/'render'),str(root/'frames')],check=True)
from render_scene import render
frames=[]
for path in sorted((root/'frames').glob('*.svg')):
    im=render(path);im.save(path.with_suffix('.png'));frames.append(im)
# Reversing the return leg makes all screen elements join continuously.
frames=frames+frames[-2:0:-1]
assert len(frames)==160
palette_sheet=Image.new('RGB',(320*8,160*8))
for i in range(64):palette_sheet.paste(frames[i*len(frames)//64],((i%8)*320,(i//8)*160))
palette=palette_sheet.quantize(colors=256,method=Image.Quantize.MEDIANCUT)
indexed=[f.quantize(palette=palette,dither=Image.Dither.NONE) for f in frames]
durations=[60,60,60,70]*40
for scale,name in [(1,'VAPORA-1.2-Holy-Dust-320.gif'),(3,'VAPORA-1.2-Holy-Dust.gif')]:
    output=[f.resize((320*scale,160*scale),Image.Resampling.NEAREST) for f in indexed]
    output[0].save(root/name,save_all=True,append_images=output[1:],duration=durations,loop=0,disposal=1,optimize=False)
    with Image.open(root/name) as gif:
        total=0
        for i in range(gif.n_frames):gif.seek(i);total+=gif.info['duration']
        assert gif.n_frames==160 and total==10000 and gif.info['loop']==0
        print(name,gif.size,gif.n_frames,total,(root/name).stat().st_size)
sheet=Image.new('RGB',(960,480))
for j,idx in enumerate([0,20,40,60,80,120]):sheet.paste(frames[idx],((j%3)*320,(j//3)*160+80))
sheet.save(root/'contact-sheet.png')
(root/'README.txt').write_text('VAPORA 1.2 — Holy Dust screen showcase\n10 seconds, 160 frames, infinite loop.\nProduction screen drawing code; Holy Dust preset with an idle full Brightness sweep and default 01 Solid. No notes are played or held; no DSP note_on/render calls.\nThe return leg reverses the frames for a seamless loop.\nHost rendering with a substitute font, not a physical OP-1 screen capture. GIF is silent.\nBoth native 320x160 and nearest-neighbor 960x480 exports are provided.\nApproved preset parameters are unchanged except the temporary Brightness sweep. Host-only substitute font; native pixels can differ.\n')

(root/'showcase.json').write_text(json.dumps(dict(version='1.2',preset='Holy Dust',sun='01 Solid',played_notes=0,held_notes=0,frames=160,duration_ms=10000,loop=0,host_render=True,substitute_font=True),indent=2)+'\n')
