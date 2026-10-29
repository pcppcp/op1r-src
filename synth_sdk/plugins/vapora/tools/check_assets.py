#!/usr/bin/env python3
"""Data checks and previews of actual C-generated line commands.
Pillow rasterization is a HOST APPROXIMATION, not the OP-1 renderer.
"""
from pathlib import Path
import hashlib,json,re,wave,xml.etree.ElementTree as ET
import numpy as np
from PIL import Image,ImageDraw,ImageFont
ROOT=Path(__file__).resolve().parents[1]

def check(out,render_previews=True):
    text=(ROOT/"vapora_tables.h").read_text()
    arrays={}
    for name,n,body in re.findall(r"int16_t (ps_t\d+_\d+)\[(\d+)\] = \{(.*?)\};",text,re.S):
        a=np.fromstring(body.replace("\n",""),sep=",",dtype=np.int16)
        assert len(a)==int(n)
        arrays[name]=a
    cfg=json.loads((ROOT/"data/voices.json").read_text())
    assert len(arrays)==(2+len(cfg["textures"]))*8
    preset_count=len(json.loads((ROOT/"data/presets.json").read_text()))
    fundamental=cfg["reference_cycles"]
    result=[]
    for name,a in arrays.items():
        x=a.astype(float);spec=np.fft.rfft(x);energy=abs(spec)**2
        assert abs(x.mean())<=.5 and abs(x).max()<=23000
        leak=float(energy[int(len(x)*.4)+1:].sum()/max(1,energy.sum()))
        quantization_rms=float(np.sqrt(2*energy[int(len(x)*.4)+1:].sum())/len(x))
        assert leak<2e-7 or quantization_rms<=.5,(name,leak,quantization_rms)
        if name.endswith("_7"):assert not a.any() # top safety level is silent
        result.append(dict(table=name,dc=float(x.mean()),alias_band_energy_ratio=leak,alias_band_rms_lsb=quantization_rms))
    spectra=[]
    for t in range(2):
        spectrum=np.abs(np.fft.rfft(arrays[f"ps_t{t}_0"].astype(float)))
        bins=np.arange(fundamental-24,fundamental+25);power=spectrum[bins]**2
        center=float(np.sum(bins*power)/power.sum())
        width=float(np.sqrt(np.sum((bins-center)**2*power)/power.sum()))
        support=int(np.sum(spectrum[bins]>.1*spectrum[bins].max()))
        assert abs(center-fundamental)<1 and width>1.2 and support>=9
        spectra.append(dict(endpoint=t,fundamental_center_bin=center,
                            fundamental_power_std_bins=width,bins_above_10pct_amplitude=support))
    for t,e in enumerate(cfg["textures"],2):
        if e.get("gain_group")!="cinema":continue
        spectrum=abs(np.fft.rfft(arrays[f"ps_t{t}_0"].astype(float)))
        f0=e["master_length"]//64
        bins=np.arange(f0-16,f0+17);power=spectrum[bins]**2
        center=float(np.sum(bins*power)/power.sum())
        width=float(np.sqrt(np.sum((bins-center)**2*power)/power.sum()))
        support=int(np.sum(spectrum[bins]>.1*spectrum[bins].max()))
        assert width>1.1 and support>=7,(e["name"],width,support)
        spectra.append(dict(endpoint=t,name=e["name"],fundamental_center_bin=center,
                            fundamental_power_std_bins=width,bins_above_10pct_amplitude=support))
    morph=[]
    for f in range(1):
        a=arrays[f"ps_t{2*f}_0"].astype(float);b=arrays[f"ps_t{2*f+1}_0"].astype(float)
        ratio=float(np.sqrt(np.mean(((a+b)/2)**2))/np.sqrt(np.mean(a*a)))
        assert .8<ratio<=1.01
        morph.append(ratio)
    audio=[]
    for pcm in sorted(out.glob("*.pcm")):
        data=pcm.read_bytes()
        with wave.open(str(pcm.with_suffix(".wav")),"wb") as w:
            w.setnchannels(1);w.setsampwidth(2);w.setframerate(44100);w.writeframes(data)
        x=np.frombuffer(data,dtype="<i2").astype(float)
        peak=int(np.max(np.abs(x)));rms=float(np.sqrt(np.mean(x*x)))
        assert peak>100 and rms>10,(pcm.name,"silent or near-silent fixture")
        # Approximate correlation at the nominal long-loop period at C4.
        lag=round(fundamental*44100/261.625565)
        held=x[4410:4*44100]
        corr=float(np.corrcoef(held[:-lag],held[lag:])[0,1]) if np.std(held[lag:])>1e-9 and np.std(held[:-lag])>1e-9 else None
        spec=np.abs(np.fft.rfft(held*np.hanning(len(held))))**2
        freq=np.fft.rfftfreq(len(held),1/44100)
        image_ratio=float(spec[freq>11025].sum()/max(spec.sum(),1))
        assert image_ratio<.002,(pcm.name,image_ratio)
        audio.append(dict(file=pcm.with_suffix(".wav").name,peak=peak,rms=rms,
                          above_internal_nyquist_energy_ratio=image_ratio,
                          nominal_c4_loop_correlation=corr,
                          sha256=hashlib.sha256(pcm.with_suffix(".wav").read_bytes()).hexdigest()))
    assert len(audio)==preset_count+4
    report=dict(tables=result,measured_spectral_profiles=spectra,morph_midpoint_rms_relative_to_endpoint=morph,audio=audio,
                note="WAVs use production DSP and simulated per-preset attack/decay/sustain/release. No stock T2 renderer or FX: Deep Pressure's saved Reverb and Stone Hall's saved Plate are not rendered in these dry fixtures. No per-file normalization. Correlation is diagnostic, not an audition.")
    (out/"assets.json").write_text(json.dumps(report,indent=2)+"\n")
    diversity(out)
    visual_checks(out)
    if render_previews:render(out)
    print(f"{len(arrays)} tables: DC/peaks/spectral limits checked; coherent morph midpoint RMS",morph)
    print(f"{len(audio)} WAV fixtures wrapped without normalization; scene/sun drawing commands checked")
    return report

def diversity(out):
    features=[];names=[];raw=[]
    for f in sorted(out.glob("preset-*.pcm")):
        x=np.frombuffer(f.read_bytes(),dtype="<i2").astype(float)
        # Equal-power log-frequency fingerprints expose near-duplicate timbres.
        power=np.zeros(8193)
        for start in range(0,min(len(x)-16384,6*44100),8192):
            power+=abs(np.fft.rfft(x[start:start+16384]*np.hanning(16384)))**2
        freq=np.fft.rfftfreq(16384,1/44100);edges=np.geomspace(45,10000,97)
        bands=np.array([power[(freq>=lo)&(freq<hi)].sum() for lo,hi in zip(edges[:-1],edges[1:])])
        bands=np.sqrt(bands/max(bands.sum(),1));bands/=max(np.linalg.norm(bands),1e-12)
        features.append(bands);names.append(f.stem);raw.append(hashlib.sha256(x.tobytes()).hexdigest())
    count=len(json.loads((ROOT/"data/presets.json").read_text()))
    assert len(set(raw))==count
    similarities=np.array(features)@np.array(features).T
    pairs=sorted([(float(similarities[i,j]),names[i],names[j]) for i in range(count) for j in range(i)],reverse=True)
    report={"method":"Cosine similarity of 96 equal-power log-frequency fingerprints from the first six seconds of dry fixtures. 1 means identical distribution; this is not a listening verdict.",
            "unique_pcm_fixtures":count,"closest_pairs":[{"similarity":a,"a":b,"b":c} for a,b,c in pairs[:20]]}
    (out/"preset-diversity.json").write_text(json.dumps(report,indent=2)+"\n")
    print("Nearest spectral pairs:",pairs[:5])

def visual_checks(out):
    def lines(name,width,layer=None):
        root=ET.parse(out/name).getroot()
        return [e.attrib for e in root.iter() if e.tag.split("}")[-1]=="line"
                and abs(float(e.get("stroke-width","0"))-width/1024)<.0001
                and (layer is None or e.get("data-particle-layer")==str(layer))]
    for name in ("minimum.svg","default.svg"):
        assert len(lines(name,3200,0))<=8
        assert len(lines(name,1900,0))<=24
        assert len(lines(name,2500,0))<=16
        dots=lines(name,1900,0)+lines(name,2500,0)+lines(name,3200,0)
        xs=[float(d["x1"]) for d in dots];ys=[float(d["y1"]) for d in dots]
        assert max(xs)-min(xs)>270 and max(ys)-min(ys)>30
        mesh=lines(name,1950)
        xs=[float(d[k]) for d in mesh for k in ("x1","x2")]
        # Rightmost pointed vertices now sit 8px in from their former x=318.
        assert 1.9<=min(xs)<=2.1 and 309.9<=max(xs)<=310.1
    assert lines("idle-00.svg",1950)!=lines("idle-23.svg",1950),"idle surface did not move"
    ids=range(1,8)
    for style in range(2,8):
        normal=[]
        for cy in range(30,57):
            aa=lines(f"sunfixed-{style:02d}-{cy:02d}.svg",4096 if style==2 else 2200)
            normal.append([tuple(round(float(a[k])-(cy if k.startswith("y") else 0),6)
                         for k in ("x1","y1","x2","y2")) for a in aa])
        assert all(g==normal[0] for g in normal),"sun shape changed during translation"
    fill=[]
    for cy in range(30,57):
        aa=lines(f"sunmove-02-{cy:02d}.svg",4096)
        fill.append(sum(float(a["x2"])-float(a["x1"]) for a in aa))
        assert all(a["y1"]==a["y2"] and (float(a["y1"])-cy)%6==0 for a in aa)
        root=ET.parse(out/f"sunfixed-01-{cy:02d}.svg").getroot()
        groups=list(root)
        assert len(groups)==1 and groups[0].get('transform')==f'translate(140.0000 {cy-20:.4f})'
        assert groups[0][0].get('fill')=='#ffffff'
    assert fill[0]>150 and fill[-1]==0 and all(a>=b for a,b in zip(fill,fill[1:]))
    full=lines("sunfixed-02-30.svg",4096)
    assert len(full)==7 and sorted(float(a['y1'])-30 for a in full)==[-18,-12,-6,0,6,12,18]
    assert all((float(a['x1'])+float(a['x2']))/2==160 for a in full)
    for style,circles in [(3,[(0,0,15)]),(5,[(0,0,15)]),(6,[(0,0,12)]),
                          (7,[(-17,0,10),(0,0,14),(17,0,10)])]:
        aa=lines(f"sunfixed-{style:02d}-30.svg",2200)
        for n,(cx,yy,radius) in enumerate(circles):
            for a in aa[n*32:(n+1)*32]:
                for x,y in [(float(a["x1"])-160-cx,float(a["y1"])-30-yy),
                            (float(a["x2"])-160-cx,float(a["y2"])-30-yy)]:
                    assert abs((x*x+y*y)**.5-radius)<.1,"Notched circle geometry"
    retro=lines("sunmove-04-30.svg",2200)
    assert max(float(a["y1"])-30 for a in retro)==14,"Retro bottom stripe returned"
    names=['SOLID','LINES','HALO','RETRO','BANDS','SATURN','TRIPLE']
    for style,name in enumerate(names,1):
        root=ET.parse(out/f"sun-{style:02d}.svg").getroot()
        text=" ".join(e.text or "" for e in root.iter() if e.tag.split("}")[-1]=="text")
        assert f"SUN {style:02d} {name}" in " ".join(text.split()),(style,text)
    for e in ET.parse(ROOT/'vapora.svg').getroot().iter():
        if e.tag.split('}')[-1]=='path':assert e.get('fill') not in (None,'inherit')
    (out/"sun-motion.json").write_text(json.dumps(dict(styles=7,display_ids=list(ids),names=names,default=1,
        geometry_test_heights=27,brightness_center_low=56,brightness_center_high=36,
        travel_scale=0.75,translation_invariant=list(ids),solid_radius=20,lines_stroke_pixels=4,
        lines_count=7,lines_pitch_pixels=6,lines_width_sum_high_to_low=fill,
        limitation="Host command geometry, not an OP-1 raster capture."),indent=2)+"\n")
    print("Visual contract: seven suns in approved order; four-pixel Lines; explicit native paint; idle water moves")

def render(out):
    from render_scene import render as render_frame
    for svg in sorted(out.glob("*.svg")):
        render_frame(svg).save(svg.with_suffix('.png'))
    suns=Image.new("RGB",(990,600),(20,23,30));sd=ImageDraw.Draw(suns)
    for i,style in enumerate(range(1,8)):
        name=f"sun-{style:02d}";x=10+(i%3)*330;y=10+(i//3)*198
        sd.text((x,y),name+" / HOST FONT SUBSTITUTE",fill=(180,185,200))
        suns.paste(Image.open(out/(name+".png")),(x,y+20))
    suns.save(out/"sun-contact-sheet.png")
    for style in (1,2,4,7):
        frames=[Image.open(out/f"sunmove-{style:02d}-{cy:02d}.png") for cy in range(56,29,-1)]
        frames=frames+frames[-2:0:-1]
        frames[0].save(out/f"sun-{style:02d}-geometry-stress-host.gif",save_all=True,append_images=frames[1:],duration=100,loop=0)
    names=["minimum","default","maximum","character","interval","speed","all-alternates","voice-onset","single-low","single-semitone","single-high"]
    sheet=Image.new("RGB",(660,1200),(20,23,30));d=ImageDraw.Draw(sheet)
    for i,n in enumerate(names):
        x=10+(i%2)*330;y=10+(i//2)*198
        d.text((x,y),n+" / HOST",fill=(180,185,200))
        sheet.paste(Image.open(out/(n+".png")),(x,y+20))
    sheet.save(out/"contact-sheet.png")
    for prefix,name in [("animation","ocean-host"),("idle","idle-host"),("default-idle","default-idle-host"),("single-low","single-low-host"),("single-semitone","single-semitone-host"),("single-high","single-high-host"),("secret","secret-host")]:
        frames=[Image.open(p).convert("RGB") for p in sorted(out.glob(prefix+"-*.png"))]
        if frames:frames[0].save(out/(name+".gif"),save_all=True,append_images=frames[1:],duration=63,loop=0)
if __name__=="__main__":
    import sys
    check(Path(sys.argv[1]))
