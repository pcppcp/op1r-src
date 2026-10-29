#!/usr/bin/env python3
"""Check selected dry sounds against the previous production DSP, at fixed seeds."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
def check(baseline,out):
    old=json.loads((baseline/'data/presets.json').read_text());old={p['name']:p for p in old}
    new=[p for p in json.loads((ROOT/'data/presets.json').read_text()) if p.get('retained_from_r6')]
    # Render through ps_decode/ps_start/ps_render, independent of adapter note serials.
    harness=r'''
#include "vapora_dsp.h"
#include <stdlib.h>
#include <stdio.h>
int main(int argc,char **argv){
 if(argc!=9)return 1;
 int16_t words[8];for(int i=0;i<8;i++)words[i]=(int16_t)strtol(argv[i+1],0,10);
 for(int note=0;note<3;note++)for(int morph=0;morph<3;morph++){
  ps_params p=ps_decode(words);if(morph)p.q[PS_MORPH]=morph==1?0:32767;
  ps_voice v;ps_start(&v,&p,0x12345678u+(unsigned)note,1);
  int32_t pitch[128];int16_t pcm[128];
  for(int n=0;n<128;n++)pitch[n]=(int32_t)((uint64_t)(note==0?131:note==1?262:523)*4294967296ull/44100);
  for(int b=0;b<1200;b++){ps_render(&v,pcm,pitch,32767,&p);if(fwrite(pcm,sizeof *pcm,128,stdout)!=128)return 2;}
 }
 return 0;
}
'''
    results=[]
    with tempfile.TemporaryDirectory(prefix='vapora-retained-') as tmp:
        t=Path(tmp);(t/'compare.c').write_text(harness)
        for label,source in [('old',baseline),('new',ROOT)]:
            subprocess.run(['clang','-std=c11','-O2','-I'+str(source),str(t/'compare.c'),str(source/'vapora_dsp.c'),'-o',str(t/label)],check=True)
        for p in new:
            a=subprocess.check_output([t/'old',*map(str,old[p.get('retained_name_r6',p['name'])]['knobs'])])
            b=subprocess.check_output([t/'new',*map(str,p['knobs'])])
            assert a==b,p['name']
            results.append({'name':p['name'],'dry_pcm_sha256':hashlib.sha256(b).hexdigest(),'bytes':len(b),'identical':True})
    assert (ROOT/'vapora_dsp.c').read_bytes()==(baseline/'vapora_dsp.c').read_bytes()
    report={'retained':results,'production_dsp_source_identical':True,'playback_heads_per_voice':2,'polyphony':6,
            'conditions':'Three pitches, authored Morph and both endpoints, fixed seeds; 1200 blocks each. Dry output before stock T2/FX. New Character words select the same source pairs.',
            'exception':'Deep Pressure adds saved T3 Reverb; dry engine is unchanged.'}
    (out/'retained-equivalence.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'{len(new)} retained presets: exact dry PCM at three pitches and three Morph positions; production two-head DSP source unchanged.')
if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('baseline',type=Path);ap.add_argument('output',type=Path);a=ap.parse_args();check(a.baseline,a.output)
