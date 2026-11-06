#!/usr/bin/env python3
"""Dry representative WAVs and numerical audibility/fundamental checks; no ADSR claim."""
import json,subprocess,sys,tempfile,wave
from pathlib import Path
import numpy as np
HERE=Path(__file__).resolve().parent
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='sid-audio-') as tmp:
    exe=Path(tmp)/'audio'
    subprocess.run(['cc','-O2',str(HERE/'sid_audio.c'),str(HERE/'sid_dsp.c'),'-o',str(exe)],check=True)
    stats={}
    for name,mode,hz,mix in [('bass',1,110,0),('pulse',2,220,10000),('sync',4,220,16000),('ring',5,220,16000),('noise',3,440,16000)]:
        raw=subprocess.check_output([str(exe),str(mode*5462),str(round(hz/44100*2**32)),str(mix)])
        with wave.open(str(out/(name+'.wav')),'wb') as w:
            w.setparams((1,2,44100,0,'NONE','not compressed'));w.writeframes(raw)
        a=np.frombuffer(raw,dtype='<i2').astype(float)/32768;b=a[44100:]
        freq=np.fft.rfftfreq(len(b),1/44100);power=abs(np.fft.rfft(b*np.hanning(len(b))));i=power[1:].argmax()+1
        stats[name]={'peak':float(abs(a).max()),'rms':float(np.sqrt(np.mean(b*b))),'settled_dc':float(b.mean()),'dominant_hz':float(freq[i])}
    assert abs(stats['bass']['dominant_hz']-110)<2
    assert all(v['rms']>.001 and abs(v['settled_dc'])<.01 and v['peak']<.99 for v in stats.values()),stats
    (out/'audio-metrics.json').write_text(json.dumps(stats,indent=2))
    print(json.dumps(stats,indent=2))
