"""Host-only SVG command preview; substitute font, not native raster pixels."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import xml.etree.ElementTree as ET
import re,math
S=4
font='/System/Library/Fonts/Menlo.ttc'
def render(path):
 im=Image.new('RGB',(320*S,160*S),'black');draw=ImageDraw.Draw(im)
 def walk(el,offset=(0,0),fill="black"):
  a=el.attrib;tag=el.tag.split('}')[-1];ox,oy=offset;fill=a.get('fill',fill)
  if a.get('transform','').startswith('translate'):
   nums=[float(v) for v in re.findall(r'-?\d+(?:\.\d+)?',a['transform'])];ox+=nums[0];oy+=nums[1]
  def xy(x,y):return ((x+ox)*S,(y+oy)*S)
  if tag=='line':
   pts=[xy(float(a['x1']),float(a['y1'])),xy(float(a['x2']),float(a['y2']))]
   draw.line(pts,fill=a['stroke'],width=max(1,round(float(a['stroke-width'])*S)))
  elif tag=='circle':
   x,y,r=map(float,(a['cx'],a['cy'],a['r']))
   draw.ellipse([xy(x-r,y-r),xy(x+r,y+r)],fill=a['fill'])
  elif tag=='path':
   tokens=re.findall(r'[MLCz]|-?\d+(?:\.\d+)?',a['d']);i=0;pts=[];p=(0,0)
   while i<len(tokens):
    cmd=tokens[i];i+=1
    if cmd in ('M','L'):p=tuple(map(float,tokens[i:i+2]));i+=2;pts.append(xy(*p))
    elif cmd=='C':
     coords=list(map(float,tokens[i:i+6]));i+=6;b=coords[:2];c=coords[2:4];d=coords[4:6]
     for n in range(1,33):
      t=n/32;u=1-t;pts.append(xy(*(u**3*p[j]+3*u*u*t*b[j]+3*u*t*t*c[j]+t**3*d[j] for j in range(2))))
     p=d
    elif cmd!='z':raise ValueError(cmd)
   draw.polygon(pts,fill=fill)
  elif tag=='text':
   size=float(a['font-size']);f=ImageFont.truetype(font,round(size*S))
   draw.text(xy(float(a['x']),float(a['y'])-size),el.text or '',font=f,fill=a['fill'])
  for child in el:walk(child,(ox,oy),fill)
 walk(ET.parse(path).getroot())
 return im.resize((320,160),Image.Resampling.LANCZOS)
