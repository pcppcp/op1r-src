#!/usr/bin/env python3
"""Contour original art with the screen placement and line-weight constraints:
move all artwork up 4 screen px; normalize thin ink to the measured 1.6 px SID
bottom-lead weight, preserving broad fills, holes/gaps and the source palette.
Native text is unchanged. Minimum-width processing is deterministic and bounded.
"""
import hashlib,json,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'display'))
from svgart import rounded,shape
from lineweight import minimum_width
import cv2
import numpy as np
from PIL import Image
HERE=Path(__file__).resolve().parent
# Owner 2026-10-26: each lead group is one colour end to end, the colour of the
# oscillator panel whose pins it leaves, and meets the dot of that colour. The
# reference fades the long blue lead to red (traced as a white middle) and crosses
# the colours at the dots. Recolour whole traced paths only, so path data stays
# identical: (source fill, screen box holding the whole path, new fill).
LEAD_COLOURS=(
    ('#dddddd',(108,113,121,117),'#6497eb'),  # fade middle of the long blue lead
    ('#eb454b',(118,105,167,116),'#6497eb'),  # long blue lead's end and bottom dot
    ('#02d1a0',(150,103,160,107),'#6497eb'),  # end of the upper blue lead
    ('#6497eb',(157,94,167,99),'#eb454b'),    # red lead's end and top dot
    ('#6497eb',(151,98,160,101.5),'#eb454b'), # loose end in the red leads' fan
)
def lead_colour(fill,xy):
    """New fill for a top-level path under LEAD_COLOURS (unchanged otherwise)."""
    xs=[p[0] for p in xy];ys=[p[1] for p in xy]
    for i,(old,(x0,y0,x1,y1),new) in enumerate(LEAD_COLOURS):
        if fill==old and x0<=min(xs) and max(xs)<=x1 and y0<=min(ys) and max(ys)<=y1:return new,i
    return fill,None
def generate(source,output,recolour=True):
    a=np.asarray(Image.open(source).convert('RGB')).astype(float)
    if a.shape[:2]!=(887,1774):raise ValueError('expected exact generated 1774x887 canvas')
    palette=np.array([[221,221,221],[100,151,235],[2,209,160],[235,69,75]],float)
    # Assign by chromatic direction: antialiased pixels retain the nearest color,
    # with a half-strength boundary. Palette is measured from bright source ink.
    direction=palette/np.linalg.norm(palette,axis=1)[:,None]
    norms=np.linalg.norm(a,axis=2);unit=a/np.maximum(norms[:,:,None],1)
    labels=np.argmax(np.einsum('...c,kc->...k',unit,direction),axis=2)
    out=['<svg xmlns="http://www.w3.org/2000/svg" width="320" height="160" viewBox="0 0 320 160">']
    counts=[];vertex_counts=[];recoloured=[]
    for i,col in enumerate(palette):
        strength=np.einsum('...c,c->...',a,col)/sum(col*col)
        mask=(labels==i)&(strength>.5)
        # Match the original generator's omitted sub-four-pixel source specks.
        old_contours,_=cv2.findContours(mask.astype(np.uint8)*255,cv2.RETR_TREE,cv2.CHAIN_APPROX_SIMPLE)
        _,components=cv2.connectedComponents(mask.astype(np.uint8))
        for contour in old_contours:
            if abs(cv2.contourArea(contour))<4:
                x,y=contour[0,0];mask[components==components[y,x]]=False
        mask=minimum_width(mask).astype(np.uint8)*255
        contours,hierarchy=cv2.findContours(mask,cv2.RETR_TREE,cv2.CHAIN_APPROX_SIMPLE)
        emitted=0
        def emit(index,depth):
            nonlocal emitted
            contour=contours[index]
            points=cv2.approxPolyDP(contour,1.1,True).reshape(-1,2)
            if len(points)<3:
                x,y,w,h=cv2.boundingRect(contour)
                points=np.array([[x-.5,y-.5],[x+w-.5,y-.5],[x+w-.5,y+h-.5],[x-.5,y+h-.5]])
            xy=rounded([((x+.5)*320/1774,(y+.5)*160/887-4) for x,y in points],True)
            if len(xy)>=3:
                color=('#'+''.join(f'{int(v):02x}' for v in col)) if depth%2==0 else '#010101'
                if recolour and depth%2==0:
                    color,rule=lead_colour(color,xy)
                    if rule is not None:recoloured.append(rule)
                out.append(shape(xy,'none',0,True,color));emitted+=1;vertex_counts.append(len(xy))
            child=hierarchy[0,index,2]
            while child!=-1:
                emit(child,depth+1);child=hierarchy[0,child,0]
        if hierarchy is not None:
            for index,h in enumerate(hierarchy[0]):
                if h[3]==-1:emit(index,0)
        counts.append(emitted)
    assert sum(counts)<=95, 'native path budget exceeded'
    assert sum(vertex_counts)<=3784, 'native vertex budget exceeded'
    if recolour:assert sorted(recoloured)==list(range(len(LEAD_COLOURS))),'each lead rule must match one path'
    out.append('</svg>');Path(output).write_text('\n'.join(out)+'\n')
    print(json.dumps({'source_sha256':hashlib.sha256(Path(source).read_bytes()).hexdigest(),'canvas':[1774,887],'mapping':320/1774,'palette':palette.tolist(),'contours':counts,'vertices':sum(vertex_counts),'minimum_ink_width':1.6,'deviations':'measured flat palette; owner -4px y and 1.6px minimum useful-ink width; broad fills and half-pixel black channels preserved; no added paths'+('; leads one colour per oscillator (LEAD_COLOURS: dots top to bottom red, green, blue)' if recolour else '')}))
if __name__=='__main__':generate(sys.argv[1],sys.argv[2] if len(sys.argv)>2 else HERE/'sid.svg')
