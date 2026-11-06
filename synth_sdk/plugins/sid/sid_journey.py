#!/usr/bin/env python3
"""Targeted real-key SID selection, six voices, release and native-font capture.
Caller supplies a private fixture already containing the exact SID file. Run serially
through the coordinated queue. One inline process, no forks. No selector or note-state writes.
"""
import argparse,hashlib,json,struct,subprocess,sys,time
from pathlib import Path
HERE=Path(__file__).resolve().parent;SDK=HERE.parents[1]
sys.path.insert(0,str(SDK/'harness/emu'))
from forkemu import Parent,find_up,EMU
from userflow import Journey,hold,file_hash
from sequencer_persistence import isolate_music
import kn_mem
sys.path.insert(0,str(SDK.parent/'tools/kennel'));import pack,reloc
ap=argparse.ArgumentParser();ap.add_argument('nand',type=Path);ap.add_argument('out',type=Path);ap.add_argument('--elf',type=Path,required=True);ap.add_argument('--plugin',type=Path,required=True);ap.add_argument('--content',type=Path,required=True);ap.add_argument('--prepare-only',action='store_true');a=ap.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
symbols={}
for line in subprocess.check_output([str(find_up('toolchain/bfin/bin/bfin-elf-nm')),str(a.elf)],text=True).splitlines():
 w=line.split()
 if len(w)==3:symbols[w[2]]=int(w[0],16)
metadata=out/'metadata/sid';metadata.mkdir(parents=True,exist_ok=True)
data,info=pack.pack_dir(HERE,metadata/'sid.knl',find_up('toolchain/bfin/bin'),metadata)
if data!=a.plugin.read_bytes():raise RuntimeError('SID source rebuild differs from supplied installed file')
ro=pack.Elf((metadata/'sid.ro').read_bytes());offsets={s['name']:info['layout'][ro.sh[s['shndx']]['name']][0]+s['value'] for s in ro.syms if 0<s['shndx']<len(ro.sh) and ro.sh[s['shndx']]['name'] in info['layout']}
voice_size=next(s['size']//6 for s in ro.syms if s['name']=='_voices')
abi=metadata/'abi.c';abi.write_text('#include <stddef.h>\n#include "'+str(HERE/'sid_dsp.h')+'"\nconst unsigned observation[] __attribute__((section(".observer")))={sizeof(sid_state),offsetof(sid_state,step)};\n')
subprocess.run([str(find_up('toolchain/bfin-gcc/bin/bfin-elf-gcc')),'-mcpu=bf524','-ffreestanding','-c',str(abi),'-o',str(metadata/'abi.o')],check=True)
obj=pack.Elf((metadata/'abi.o').read_bytes());size,step_offset=struct.unpack('<II',obj.contents(next(x for x in obj.sh if x['name']=='.observer')))
if size!=voice_size:raise RuntimeError('SID observation ABI mismatch')


def prepare_music():
 result={};pack.SDK=a.elf.resolve().parent.parent
 for name in ('scale','chord'):
  folder=out/'metadata'/name;folder.mkdir(parents=True,exist_ok=True)
  blob,info=pack.pack_dir(pack.SDK/'plugins'/name,folder/(name+'.knl'),find_up('toolchain/bfin/bin'),folder)
  original=a.content/(name+'.knl')
  if blob!=original.read_bytes():raise RuntimeError(name+' source does not match supplied installed content')
  obj=pack.Elf((folder/(name+'.ro')).read_bytes())
  offsets={sym['name']:info['layout'][obj.sh[sym['shndx']]['name']][0]+sym['value'] for sym in obj.syms if 0<sym['shndx']<len(obj.sh) and obj.sh[sym['shndx']]['name'] in info['layout']}
  result[name]={'offsets':offsets,'cached_file':str(original.resolve()),'sha256':file_hash(original)}
 return result

r={'status':'running','checks':{},'actions':[],'audio':{},'browsers':{},'symbols':symbols,'metadata':{'music':prepare_music()},'source_hashes':{str(p):file_hash(p) for p in HERE.glob('*') if p.is_file()},'elf_sha256':file_hash(a.elf),'plugin_sha256':file_hash(a.plugin),'emulator_sha256':file_hash(EMU),'fixture':'caller-supplied private same-core NAND with exact SID file; no release assertion','limitations':['32-sample pitch cadence','six physical input keys; no remote MIDI claim','actual emulator packet timing is not hardware CPU']}
if a.prepare_only:print('SID metadata and exact-source file checks PASS');raise SystemExit(0)
t0=time.monotonic();source_hash=file_hash(a.nand/'nand.img')
try:
 with Parent(out,a.nand/'nand.img',a.nand,jit=a.nand/'jit') as parent:
  with parent.inline('sid') as f:
   wait=f.wait
   f.wait=lambda path,timeout=180:wait(path,min(timeout,60))
   j=Journey(f,r)
   try:
    isolate_music(j,True)
    j.action('synth-engine',hold('synth')+'\n'+hold('t1'));j.select('t1','sid')
    identity=j.identity('engine',j.context());j.check('selected-sid',identity['name']=='sid',identity)
    entry=struct.unpack('<I',f.dump(symbols['_op1_engines']+identity['marker']*4,4,'engine-entry.bin'))[0];base=entry-offsets['_ENGINE_sid']
    if not kn_mem.ARENA_LO<=base<kn_mem.ARENA_HI:raise AssertionError('SID runtime base outside plugin arena')
    plugin=reloc.parse(data);expected=reloc.relocate(plugin,base,symbols)[:plugin['text_size']]
    j.check('exact-loaded-sid-code',f.dump(base,len(expected),'sid-text.bin')==expected,{'base':base,'plugin_sha256':r['plugin_sha256']})
    j.action('fx-page',hold('t3'))
    if j.context()['enabled']:j.action('setup-bypass-synth-fx',hold('t3'),setup=True)
    j.check('synth-fx-bypassed',not j.context()['enabled'])
    def native(label):
     raw=f.snapshot_many([(0x01DFF134,4,label+'-held-count.bin'),(0x01DFE734,20*128,label+'-held.bin'),(0xFF8000D4,24*6,label+'-envelopes.bin'),(0xFF80105C,6,label+'-active.bin'),(0x01B60EF0+j.context()['slot']*16,16,label+'-adsr.bin')])
     count=struct.unpack('<I',raw[0])[0]
     if count>128:raise AssertionError('native held-note count invalid')
     held=[{'note':struct.unpack_from('<i',raw[1],i*20)[0],'voice':struct.unpack_from('<i',raw[1],i*20+16)[0]} for i in range(count)]
     if any(not 0<=x['note']<128 or not -2<=x['voice']<6 for x in held):raise AssertionError('native held-note ABI invalid')
     env=[dict(zip(['state','age','level','pitch','velocity','key_word'],struct.unpack_from('<6i',raw[2],i*24))) for i in range(6)]
     snap={'held':held,'count':count,'envelopes':env,'active':list(raw[3]),'adsr':list(struct.unpack('<8h',raw[4]))}
     r.setdefault('native',{})[label]=snap;return snap
    native('before-envelope-setup')
    # Real envelope page: bounded release makes note-off/silence a meaningful gate.
    j.action('setup-short-release',hold('t2')+'\nturn enc4 cw 256 2\nwait-frames 24\npress shift\nwait-frames 12\nturn enc1 ccw 32 3\nrelease shift\nwait-frames 12\n'+hold('t1'),setup=True)
    mode=int.from_bytes(f.dump(0x01B60EF0+j.context()['slot']*16+8,2,'native-playmode.bin'),'little')>>12
    j.check('native-poly-mode',mode==0,mode)
    envelope=native('after-envelope-setup')
    j.check('native-short-release-readback',envelope['adsr'][3]==16320,envelope['adsr'])
    addr=base+offsets['_voices']
    j.action('single-c4-settle','press c4\nwait-frames 30');mark=f.mark();j.action('single-c4-audio','wait-frames 60');end=f.mark()
    state=f.dump(addr,voice_size*6,'single-note-voices.bin');active=[i for i in range(6) if any(state[i*voice_size:i*voice_size+12])]
    j.check('single-note-one-sid-voice',len(active)==1,active)
    one=native('single-held');j.check('single-native-held-note',one['count']==1,one['held'])
    base_note=one['held'][0]['note']
    inc=struct.unpack_from('<I',state,active[0]*voice_size+step_offset)[0];expected_hz=inc*44100/(1<<28)
    from pitch_check import observe
    pitch=observe(f.samples(mark,end),expected_hz)
    stats=f.audio_since(mark,end);r['audio']['single-c4']={**stats,**pitch}
    j.check('actual-note-pitch-matches-sid-phase',pitch['root_present'] and stats['rms']>.0001,r['audio']['single-c4'])
    j.action('single-note-release','release c4\nwait-frames 180')
    released=native('single-released');j.check('single-key-native-released',released['count']==0,released)
    keys=['c4','e4','g4','b4','d5','e5']
    mark=f.mark();press_counts=[]
    for key in keys:
     j.action('press-'+key,'press '+key+'\nwait-frames 4');press_counts.append(native('held-'+key)['count'])
    held=native('six-held')
    j.check('six-distinct-native-held-notes',press_counts==[1,2,3,4,5,6] and sorted(x['note'] for x in held['held'])==[base_note+i for i in (0,4,7,11,14,16)] and len(set(x['voice'] for x in held['held']))==6,{'counts':press_counts,'held':held['held']})
    j.action('six-held-settle','wait-frames 40')
    first=f.dump(addr,voice_size*6,'voices-first.bin');j.action('held-six-voices','wait-frames 40');second=f.dump(addr,voice_size*6,'voices-second.bin');end=f.mark()
    moved=[first[i*voice_size:i*voice_size+12]!=second[i*voice_size:i*voice_size+12] for i in range(6)]
    stats=f.audio_since(mark,end);r['audio']['six-held']=stats;j.check('six-independent-oscillators-advance',all(moved),moved);j.check('six-voice-audio',stats['peak']>.0001 and stats['rms']>0,stats)
    release_counts=[]
    for key in keys:
     j.action('release-'+key,'release '+key+'\nwait-frames 4');release_counts.append(native('released-'+key)['count'])
    j.check('all-six-native-keys-released',release_counts==[5,4,3,2,1,0],release_counts)
    j.action('native-release-tail','wait-frames 180');native('after-release-tail')
    mark=f.mark();quiet=f.dump(addr,voice_size*6,'voices-after-release.bin');j.action('post-release-tail','wait-frames 60');end=f.mark();quiet2=f.dump(addr,voice_size*6,'voices-after-tail.bin')
    stats=f.audio_since(mark,end);r['audio']['released']=stats;native('after-quiet-window');j.check('native-adsr-release-silence',stats['peak']<.0001,stats);j.check('no-voice-render-after-release',quiet==quiet2)
    f.screenshot('native-sid-default-screen')
   finally:
    original=sys.exc_info()[1]
    try:f.send('\n'.join('release '+k for k in ['c4','e4','g4','b4','d5','e5'])+'\nrelease shift\n'+hold('stop'))
    except Exception as e:
     r['cleanup_error']=str(e)
     if original is None:raise
 r['status']='pass'
except Exception as e:r['status']='fail';r['error']=f'{type(e).__name__}: {e}'
r['source_nand_unchanged']=file_hash(a.nand/'nand.img')==source_hash;r['seconds']=time.monotonic()-t0
(out/'result.json').write_text(json.dumps(r,indent=2));print(json.dumps({'status':r['status'],'checks':r['checks'],'error':r.get('error')},indent=2));raise SystemExit(r['status']!='pass' or not r['source_nand_unchanged'])
