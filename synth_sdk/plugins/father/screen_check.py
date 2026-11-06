#!/usr/bin/env python3
"""Targeted Father native-font model, clipping and observed-distance fixtures."""
from pathlib import Path
import json,subprocess,sys,tempfile,hashlib
HERE=Path(__file__).resolve().parent;SDK=HERE.parents[1]
sys.path.insert(0,str(SDK/'harness'));import layout,oprender,svg_check
assert not svg_check.filled_subpaths_in((HERE/'father.svg').read_bytes())
output=Path(sys.argv[1] if len(sys.argv)>1 else '/tmp/father-screen');output.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='father-screen-') as d:
    exe=Path(d)/'screen'
    subprocess.run(['cc','-O1','-g','-Wall','-Wextra','-fsanitize=address,undefined','-fno-sanitize-recover=undefined',f'-I{SDK}',str(HERE/'screen_test.c'),'-o',str(exe)],check=True)
    cases=[(distance,off,9830) for distance in (0,25,49,80,99) for off in (0,1)]
    cases += [(distance,off,mix) for distance in (0,99) for off in (0,1) for mix in (0,32767)]
    for distance,off,mix in cases:
            frame=json.loads(subprocess.check_output([str(exe),str(distance),str(off),str(mix)]));frame['svg']=str(HERE/'father.svg')
            result=layout.check_bounds(frame,{'allowed':[[0,0,320,160]],'protected':[]})
            if result['status']!='pass':raise AssertionError((distance,off,result))
            (output/f'distance-{distance}-off-{off}-mix-{mix}.json').write_text(json.dumps(frame))
            oprender.render(frame,scale=3,stock=True).png(output/f'distance-{distance}-off-{off}-mix-{mix}.png')
    for distance in (0,99):
        for mod in (0,32767):
            for color in (0,32767):
                for frames in (6,50):
                    frame=json.loads(subprocess.check_output([str(exe),str(distance),'0','9830',str(mod),str(color),str(frames)]))
                    frame['svg']=str(HERE/'father.svg')
                    assert layout.check_bounds(frame,{'allowed':[[0,0,320,160]],'protected':[]})['status']=='pass', (distance,mod,color,frames)
    def state(mod,color,frames):
        return json.loads(subprocess.check_output([str(exe),'49','0','9830',str(mod),str(color),str(frames)]))
    still=state(0,18022,6);still_later=state(0,18022,50)
    assert still==still_later, 'zero modulation must be stationary'
    moving=state(32767,18022,6);moving_later=state(32767,18022,50)
    assert moving!=still, 'green gate threshold must move the small person'
    assert moving==moving_later, 'held-open gate telemetry must not add modulation sway'
    dark=state(0,0,6);bright=state(0,32767,6)
    assert dark!=bright, 'white must change reflection shading'
    assert [op[:6] for op in dark['ops']]==[op[:6] for op in bright['ops']], 'color must not move geometry'
    for name,frame in [('mod-zero',still),('mod-max',moving),('mod-max-later',moving_later),('color-dark',dark),('color-bright',bright)]:
        frame['svg']=str(HERE/'father.svg')
        assert layout.check_bounds(frame,{'allowed':[[0,0,320,160]],'protected':[]})['status']=='pass'
        (output/(name+'.json')).write_text(json.dumps(frame))
        oprender.render(frame,scale=3,stock=True).png(output/(name+'.png'))
manifest={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in output.glob('*.png')}
(output/'preview-hashes.json').write_text(json.dumps({'label_font_size':12,'distance_font_size':30,'images':manifest},indent=2)+'\n')
print('Father 18 distance/mix/bypass and 32 distance/mod/color/motion endpoint fixtures PASS; green motion/zero-still and white shading gates PASS; host model is not emulator proof')
