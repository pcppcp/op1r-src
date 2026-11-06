#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with OP-1 firmware permission.
"""Focused Father integer DSP checks; native UI acceptance is separate."""
import json
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import wave
import numpy as np
P=Path(__file__).resolve().parent
out=Path(sys.argv[1]) if len(sys.argv)>1 else Path('run/father-host')
out.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='father-check-') as tmp:
    exe=Path(tmp)/'dsp'
    subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=undefined',str(P/'father_test.c'),str(P/'father_dsp.c'),str(P/'father_reference.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe),str(out)],check=True)
    wrapper=Path(tmp)/'wrapper'
    subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=undefined',
                    '-I'+str(P),'-I'+str(P.parents[1]),str(P/'father_wrapper_test.c'),
                    str(P/'father_dsp.c'),str(P/'father_reference.c'),'-o',str(wrapper)],check=True)
    subprocess.run([str(wrapper)],check=True)
x=np.fromfile(out/'default.raw',dtype='<i2').reshape(-1,2).astype(float)/32768
energy=[float(np.sum(x[i*22050:(i+1)*22050]**2)) for i in range(12)]
# The retained integer network settles at a roughly one-LSB quantization floor.
assert energy[0]>0 and energy[1]<energy[0]*.01 and energy[8]<=2*22050*(2/32768)**2,energy
assert np.max(abs(x))<.8
assert np.count_nonzero(x[:,0]-x[:,1])>1000
assert abs(float(np.corrcoef(x[:22050].T)[0,1]))<.98
with wave.open(str(out/'impulse.wav'),'wb') as w:
    w.setparams((2,2,22050,0,'NONE','not compressed'));w.writeframes(np.round(x*32768).astype('<i2').tobytes())
near=np.fromfile(out/'near.raw',dtype='<i2').astype(float)/32768
far=np.fromfile(out/'far.raw',dtype='<i2').astype(float)/32768
gated=np.fromfile(out/'gated.raw',dtype='<i2').astype(float)/32768
assert np.sum(far[22050*2:]**2)>np.sum(near[22050*2:]**2)+1e-5
assert np.sum(gated[22050*4:]**2)<np.sum(x.reshape(-1)[22050*4:]**2)
dark=np.fromfile(out/'dark.raw',dtype='<i2').reshape(-1,2).astype(float)
bright=np.fromfile(out/'bright.raw',dtype='<i2').reshape(-1,2).astype(float)
def brightness(v):
    return float(np.sum(np.diff(v,axis=0)**2)/np.sum(v**2))
assert brightness(bright)>brightness(dark)*2
for name in ('chord-dry','chord-mix','chord-wet'):
    raw=(out/(name+'.raw')).read_bytes()
    with wave.open(str(out/(name+'.wav')),'wb') as w:
        w.setparams((2,2,22050,0,'NONE','not compressed'));w.writeframes(raw)
report=dict(status='PASS',reference_output_exact=True,sample_rate=22050,energy_seconds=energy,peak=float(np.max(abs(x))),stereo_correlation=float(np.corrcoef(x[:22050].T)[0,1]),color_brightness=dict(dark=brightness(dark),bright=brightness(bright)),native_runtime='pending')
report['source_sha256']={n:hashlib.sha256((P/n).read_bytes()).hexdigest() for n in ('father_dsp.c','father_dsp.h','father_tables.h','father_reference.c','father_test.c','check.py')}
(out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
