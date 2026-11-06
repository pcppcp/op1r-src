#!/usr/bin/env python3
"""Targeted sanitizer, production render and unclipped native-font layout checks."""
import itertools,json,subprocess,sys,tempfile
from pathlib import Path
HERE=Path(__file__).resolve().parent;SDK=HERE.parents[1]
sys.path.insert(0,str(SDK/'harness'));import layout,svg_check
assert not svg_check.filled_subpaths_in((HERE/'sid.svg').read_bytes())
with tempfile.TemporaryDirectory(prefix='sid-check-') as tmp:
    tmp=Path(tmp)
    flags=['cc','-O1','-g','-Wall','-Wextra','-fsanitize=address,undefined','-fno-sanitize-recover=undefined',f'-I{SDK}']
    for name in ('sid_test','sid_render_test','sid_screen_test'):
        src=[str(HERE/(name+'.c'))]
        if name!='sid_screen_test':src.append(str(HERE/'sid_dsp.c'))
        subprocess.run(flags+src+['-o',str(tmp/name)],check=True)
        if name!='sid_screen_test':subprocess.run([str(tmp/name)],check=True)
    states=[(8192,16384,24576,8192),*[(mode*5462,*q) for mode in range(6) for q in itertools.product((0,32767),repeat=3)]]
    for q in states:
        frame=json.loads(subprocess.check_output([str(tmp/'sid_screen_test'),*map(str,q)]));frame['svg']=str(HERE/'sid.svg')
        texts=[op[-1] for op in frame['ops'] if op[0]=='text']
        if q[2]==32767:assert texts[5]=='99',texts
        if q[3]==32767:assert texts[7]=='99',texts
        if q[0]>=4*5462 and q[1]==32767:assert texts[3]=='4.00x',texts
        if q[0]==2*5462 and q[1]==32767:assert texts[3]=='97',texts
        result=layout.check_bounds(frame,{'allowed':[[0,0,320,160]],'protected':[]})
        if result['status']!='pass':raise AssertionError((q,result))
    print(f'SID {len(states)} unclipped native-size12 screen fixtures PASS')
