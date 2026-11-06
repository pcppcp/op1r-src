#!/usr/bin/env python3
"""Compare source/raster boundaries after undoing the deliberate translation.
The configured 1.6px ink permits up to 0.8px boundary growth, plus contour
simplification; retain the source-art continuity bar of 99% within one pixel.
The lead recolour (gen_svg.LEAD_COLOURS) changes fills only: sid.svg must equal
the generator's output without it apart from fill, and that output is measured.
"""
import json,re,sys,tempfile
from pathlib import Path
import numpy as np
from PIL import Image
from scipy.ndimage import binary_erosion,distance_transform_edt,shift
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE))
sys.path.insert(0,str(HERE.parents[1]/'harness'));import oprender
source=Path(sys.argv[1]);a=np.asarray(Image.open(source).convert('RGB')).astype(float)
import gen_svg
traced=Path(tempfile.mkdtemp())/'traced.svg';gen_svg.generate(source,traced,recolour=False)
def no_fill(p):return re.sub(r'fill="#[0-9a-f]+"','',p.read_text())
assert no_fill(HERE/'sid.svg')==no_fill(traced),'sid.svg path data differs from the trace'
render=oprender.render({'svg':str(traced)},scale=1774/320,stock=False).array().astype(float)
# Undo the configured four-pixel screen translation for shape comparison.
render=shift(render,(4*1774/320,0,0),order=0,mode='constant')
palette=np.array([[221,221,221],[100,151,235],[2,209,160],[235,69,75]],float)
def masks(x):
 d=palette/np.linalg.norm(palette,axis=1)[:,None];u=x/np.maximum(np.linalg.norm(x,axis=2)[:,:,None],1)
 labels=np.argmax(np.einsum('...c,kc->...k',u,d),axis=2)
 return [(labels==i)&(np.einsum('...c,c->...',x,col)/sum(col*col)>.5) for i,col in enumerate(palette)]
report={}
for color,ref,test in zip(palette,masks(a),masks(render)):
 r=ref^binary_erosion(ref);t=test^binary_erosion(test)
 dt=distance_transform_edt(~r)[t]*320/1774;dr=distance_transform_edt(~t)[r]*320/1774
 def stats(v):return {'samples':len(v),'p95':float(np.percentile(v,95)),'within_1':float(np.mean(v<=1)),'within_half':float(np.mean(v<=.5))}
 report[str(color.tolist())]={'placement':stats(dt),'missing':stats(dr)}
print(json.dumps(report,indent=2))
assert all(s['p95']<1.0 and s['within_1']>=.99 for c in report.values() for s in c.values())
