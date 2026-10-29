#!/usr/bin/env python3
"""Build custom synth engines and an installable OP-1 firmware (fw 246).

  ./make_firmware.py [--opz] [--release] [--builtin a,b|all]   -> ../op1_246_custom-repacked.op1
      (synths / FX: plugins/*/plugin.json, by default built as /yaffs2/plugins/<name>.op1p files, docs/37;
       --builtin: those compiled into the firmware instead)
                                              + ../op1_246_custom-delta.op1 (changes since the installed tree, docs/34)

  --release  boot screen shows only "op1r <VERSION>" (use for the patcher); local builds add the build time

  --opz   also enable the hidden OP-Z controller MIDI mode (docs/12). COM -> CTRL (T2) starts in the
          normal CTRL mode; the OP-Z mode is switched on by the host-mode SysEx F0 00 20 76 00 01 03 F7
          (01 = Ableton, 02 = Reason, other = off; flag cleared again on every CTRL entry).

Steps: generate engine table -> compile with dockerised bfin-elf-gcc -> patch OP1_vdk.ldr
(new code block @0x01E00000, iter vtable slots 2-5, JUMP.L hook at 0x019B975C) -> copy unpacked
firmware -> add synth_types rows (iter + one per engine, knobs[4] = 0x5300+n) -> op1repacker repack.
Encrypted LDR blocks: only the precomputed patches in l1_patches.json (see L1_IMM_PATCHES).
"""
import argparse, json, os, shutil, sqlite3, struct, subprocess, sys
import plugins

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC_FW = os.path.join(ROOT, 'op1_246')
OUT_FW = os.path.join(ROOT, 'op1_246_custom')
REPACKER = os.path.join(ROOT, 'op1repacker', '.venv', 'bin', 'op1repacker')
CODE_BASE = 0x01E00000
MAGIC = 0x5300
MAX_ORDER = 31                                  # OP1_MAX_ORDER (op1_plugin.h)
BUILTIN = {'fm','digital','drwave','string','cluster','pulse','sampler','phase','drum','dbox',
           'ls','dna','dsynth','voltage','iter'}
VT_ITER, VT_FILTER, VT_FILTER_ST = 0x019103CC, 0x01910C94, 0x01910CB4   # _ST: master (stereo) filter
VT_DRUM = 0x0190FAC4                                                      # DrumSampler voice (docs/31)
VT_SAMPLER = 0x019119F4                                                   # SynthSampler voice (docs/46)
SLOTS = {'synth_render': (VT_ITER, 2, 0x019B99D4), 'synth_note_on': (VT_ITER, 3, 0x019B9B8A),
         'synth_query': (VT_ITER, 4, 0x019B9980), 'synth_note_off': (VT_ITER, 5, 0x019B996C),
         'fx_render': (VT_FILTER, 2, 0x019E7934), 'fx_init': (VT_FILTER, 3, 0x019E780A),
         'fx_render_stereo': (VT_FILTER_ST, 2, 0x019E7EE4), 'fx_init_stereo': (VT_FILTER_ST, 3, 0x019E78D0),
         'dp_fw_render': (VT_DRUM, 2, 0x019789EC), 'dp_fw_note_on': (VT_DRUM, 3, 0x01978926),
         'dp_fw_note_off': (VT_DRUM, 5, 0x01978898),
         'dp_type_job': (0x0190F34C, 2, 0x0193F86C),                        # drum type browser job (docs/31)
         'sp_fw_render': (VT_SAMPLER, 2, 0x01A51A7C), 'sp_fw_note_on': (VT_SAMPLER, 3, 0x01A518F4),
         'sp_fw_note_off': (VT_SAMPLER, 5, 0x01A51964),
         'sp_type_job': (0x0190F3FC, 2, 0x0193FFAC)}                        # synth type browser job (docs/46)
FX_BUILTIN = {'delay','grid','punch','phone','spring','filter','nitro','cwo'}
FILTER_ROW = (2, 'filter', '[7548, 0, 8272, 19572, 8000, 8000, 8000, 8000]')   # as op1repacker
# Raw knob ranges of fx type 5 (filter), which every custom FX runs in: range table 0x01DE5C50 + 5*0x30,
# filled at boot by 0x01A1E914 (docs/15). read_fx_param asserts "Instr fx param out of range" (and the OP-1
# locks up, also at every boot while the value is stored) if a raw word lies outside. plugin.json FX
# defaults are knob positions 0..32767 and are mapped into these ranges.
FX_RANGES = [(64, 14528), (0, 24576), (0, 22528), (8192, 32767)]
HOOKS = {0x019B975C: ('hook_namecmp', bytes.fromhex('18300b32')),      # instr_types name compare
         0x01A01844: ('hook_samplecheck', bytes.fromhex('ffe328fc')),  # sample_path 'sampler' check
         0x019AAD8E: ('hook_fxnamecmp', bytes.fromhex('18300b32')),    # fx_types name compare
         0x019B9EF8: ('hook_iterread', bytes.fromhex('03e38c71')),     # iter screen: read knobs
         0x01A3BD58: ('hook_screenctor', bytes.fromhex('fbe3d0f2')),   # screen ctor list
         0x019F02EE: ('hook_plugboot', bytes.fromhex('fbe3cd65')),     # boot: CALL Clips::load -> + plugin loader (docs/37)
         0x019F054A: ('hook_masterload', bytes.fromhex('ffe3393a')),   # boot: CALL MasterController::load -> + name lookup
         0x019C2E5E: ('hook_apsynth1', bytes.fromhex('01e34df1')),     # CALL apply_synth (key-handler job) -> name lookup
         0x01A01866: ('hook_apsynth2', bytes.fromhex('ffe349fc')),     # CALL apply_synth (boot patch load)
         0x01A50F1C: ('hook_apsynth3', bytes.fromhex('fde3ee80')),     # CALL apply_synth (synth_controller::load_patch)
         0x01A85750: ('hook_vcdown', bytes.fromhex('00e80400')),         # VoiceCtl key_down entry (LINK): MIDI sequencer recording
         0x01A85840: ('hook_vcup', bytes.fromhex('00e80800')),           # VoiceCtl key_up entry (LINK)
         0x019F07D2: ('hook_screenload', bytes.fromhex('02e33f59')),   # boot: load all screens
         0x019427B8: ('hook_rfp019427b8', bytes.fromhex('07e3f432')),   # CALL read_fx_param
         0x01984F60: ('hook_rfp01984f60', bytes.fromhex('05e3201f')),   # CALL read_fx_param
         0x019A74D4: ('hook_rfp019a74d4', bytes.fromhex('04e3660c')),   # CALL read_fx_param
         0x019B05F8: ('hook_rfp019b05f8', bytes.fromhex('03e3d4c3')),   # CALL read_fx_param
         0x019E6A48: ('hook_rfp019e6a48', bytes.fromhex('02e3ac11')),   # CALL read_fx_param
         0x019E832C: ('hook_rfp019e832c', bytes.fromhex('02e33a05')),   # CALL read_fx_param
         0x01A1CFD2: ('hook_rfp01a1cfd2', bytes.fromhex('00e3e75e')),   # CALL read_fx_param
         0x01A33FB2: ('hook_rfp01a33fb2', bytes.fromhex('ffe3f7a6')),    # CALL read_fx_param
         0x019C2D3E: ('hook_keyev', bytes.fromhex('ffe39b9f')),          # KeyHandler: key event dispatch
         0x019C2E94: ('hook_knobev', bytes.fromhex('ffe3bc9e')),         # KeyHandler: encoder step dispatch
         0x01A042A2: ('hook_lforange', bytes.fromhex('00e37dce')),       # parse_patch_json: LFO word range check
         0x01A0D150: ('hook_euplayer', bytes.fromhex('ffe38801')),       # create_player case 3: finger or euclid
         0x01A42A2E: ('hook_seqname1', bytes.fromhex('ffe38bfc')),       # set_sequencer: SeqIds::from_name
         0x01A42BDC: ('hook_seqname2', bytes.fromhex('ffe3b4fb')),       # load (synth): SeqIds::from_name
         0x01A42CE0: ('hook_seqname3', bytes.fromhex('ffe332fb')),       # load (drum): SeqIds::from_name
         0x01A422EC: ('hook_seqtoname', bytes.fromhex('09e138a6')),      # SeqIds::name(type) entry -> eu_seq_name
         0x019C9C3A: ('hook_lfoname1', bytes.fromhex('ffe391ff')),       # lfo_types::from_name compare
         0x019C9BE2: ('hook_lfoname2', bytes.fromhex('ffe3bdff')),       # lfo_types is_valid compare
         0x0193DE2A: ('hook_rlp0193de2a', bytes.fromhex('07e3c95a')),   # CALL read_lfo_param
         0x01968C34: ('hook_rlp01968c34', bytes.fromhex('06e3c403')),   # CALL read_lfo_param
         0x01981EDA: ('hook_rlp01981eda', bytes.fromhex('05e3713a')),   # CALL read_lfo_param
         0x019E4A18: ('hook_rlp019e4a18', bytes.fromhex('02e3d224')),   # CALL read_lfo_param
         0x01A2D388: ('hook_rlp01a2d388', bytes.fromhex('ffe31ae0')),   # CALL read_lfo_param
         0x01A30C3C: ('hook_rlp01a30c3c', bytes.fromhex('ffe3c0c3')),   # CALL read_lfo_param
         0x01A48FE8: ('hook_rlp01a48fe8', bytes.fromhex('ffe3ea01')),    # CALL read_lfo_param
         0x019AC8B4: ('hook_recA', bytes.fromhex('38080b10')),           # on_key input: type == sampler?
         0x019ACC7C: ('hook_recB', bytes.fromhex('01080710')),           # on_key_shift input: type == sampler?
         0x019DEEF2: ('hook_recend', bytes.fromhex('01e3aa1d')),        # record end: make slot a sampler patch
         0x01A85C8A: ('hook_midicc', bytes.fromhex('ffe381f9')),         # VoiceCtl::on_midi_rx: CALL midi_message (CC)
         0x019E39D6: ('hook_midistart', bytes.fromhex('03e3fdd6')),      # midi_rx_parse FA: CALL reset beat counters
         0x01A6ED96: ('hook_usbdesc', bytes.fromhex('ffe3adda')),        # USB init: CALL usb_audio::desc_init (6 ch)
         0x019E151C: ('hook_usbiso1', bytes.fromhex('04e3ac47')),        # usb_class_event_cb: iso IN, first packet
         0x019E161C: ('hook_usbiso2', bytes.fromhex('04e32c47')),        # .. iso IN (data)
         0x019E1812: ('hook_usbiso3', bytes.fromhex('04e33146')),        # .. iso IN (start-up, zero-filled)
         0x019F0A24: ('hook_ciservice', bytes.fromhex('03e3761e')),      # main loop tape_c::service(0): count-in PLAY
         0x0196F942: ('hook_sqlcache', bytes.fromhex('ffe3b9fe')),       # sqlite init: open all DBs, then cap page caches
         0x0191B9F0: ('hook_taperase', bytes.fromhex('09e38cc8')),       # clipboard cmd 4 (erase tape): only chunks with clips
         0x01A54842: ('hook_tapesave', bytes.fromhex('02e3a12b')),       # tape_c::service: yaffs_write of a dirty clip -> bounce buffer (docs/42)
         0x01A044A8: ('hook_dpkeep', bytes.fromhex('fde38ccc')),         # parse_patch_json: keep "op1r" (drum+, docs/31)
         0x01977900: ('hook_dpbrowse', bytes.fromhex('04e37f53')),       # DrumController Shift+T1: start row (user kit -> DRUM, drum+ -> DRUM+)
         0x01995680: ('hook_pparse1', bytes.fromhex('03e39470')),       # insert_user_patch: parse -> JSON repair first (patch_repair.c)
         0x0199F210: ('hook_pparse2', bytes.fromhex('03e3cc22')),       # preset import (drum): parse -> repair first
         0x0199FA4E: ('hook_pparse3', bytes.fromhex('03e3ad1e')),       # preset import (synth): parse -> repair first
         0x01A051F8: ('hook_pparse4', bytes.fromhex('ffe3d8f2')),       # load_synth_patch: parse -> repair first
         0x01A3BDB0: ('hook_lander1', bytes.fromhex('fce38e48')),       # screen list: LanderScreen::instance -> empty stub (docs/45)
         0x019C35F0: ('hook_lander2', bytes.fromhex('00e36e0c')),       # LanderController key_down: instance -> stub
         0x019C369C: ('hook_lander3', bytes.fromhex('00e3180c')),       # LanderController key_up: instance -> stub
         0x01A04BC0: ('hook_dpload', bytes.fromhex('ffe3f4f5')),         # load_drum_patch: parse, then read drum+ pads
         0x019C9D04: ('hook_frsnap', bytes.fromhex('09e11e59')),         # LFO/seq tick (after tape_play_mix) -> fr_snap (master FX source, docs/54)
         0x019411B8: ('hook_bgsqueue', bytes.fromhex('00e80000')),       # queue SaveSlotJob(is_drum, slot): LINK -> bgs_queue_slot (delayed, bgsave.c)
         0x01912128: ('hook_spadsr', bytes.fromhex('e3052a32')),         # Adsr_render entry -> sp_adsr (sampler+ envelope, docs/46)
         0x01A69730: ('hook_usbbuf1', bytes.fromhex('22e11401')),        # EP 0x86 transfer buffer 0x114 -> 828
         0x01A69740: ('hook_usbbuf2', bytes.fromhex('22e11401'))}        # .. second buffer
# data words: address -> (symbol, expected original value)
DATA_PATCHES = {0x018F7FF8: ('fx_screen_case5', 0x01A3BF26),          # ScreenId::for_fx case 5
                0x018F8074: ('synth_screen_case14', 0x01A3BFD0),      # ScreenId::for_synth case 14
                0x018F80F8: ('lfo_screen_case4', 0x01A3C078),         # ScreenId::for_lfo case 4 (midi)
                0x01910840: ('ql_vt_synth', 0x019CCE68),              # LfoMidi synth vtable: apply
                0x01910860: ('ql_vt_drum', 0x019CD048),               # LfoMidi drum vtable (0x0191084C): apply
                0x01910880: ('ql_vt_fx', 0x019CD104),                 # LfoMidi FX vtable (0x0191086C): apply
                0x01910848: ('ql_trig_synth', 0x019CCE30),            # LfoMidi synth vtable +0x1c: note-on trigger (RTS)
                0x01910868: ('ql_trig_drum', 0x019CCE2E),             # LfoMidi drum vtable +0x1c (RTS)
                0x01910888: ('ql_trig_fx', 0x019CCE2C),               # LfoMidi FX vtable +0x1c (RTS)
                0x01911B98: ('sm_idle_run', 0x01A5E240),              # kTE_Idle vtable run: remember thread (sysmon)
                0x01911B78: ('tm_screen_draw', 0x01A5CF94),           # tape screen draw: + markers (tape/markers.c)
                0x01910FB0: ('plug_apply_synth', 0x01A010F8),         # synth patch vtable: apply_synth -> name lookup (docs/37)
                0x01910FB8: ('plug_apply_synth_fx', 0x01A00DC4),      # synth patch vtable: apply_synth_fx
                0x01910F80: ('plug_apply_drum_fx', 0x01A0010C)}       # drum patch vtable: apply_drum_fx           # tape screen draw: + markers (tape/markers.c)              # kTE_Idle vtable run: remember thread (sysmon)
# 32-bit immediates loaded as Rx.L = lo; Rx.H = hi (two 4-byte instructions): address -> (symbol, original value)
IMM_PATCHES = {0x0191AB52: ('sm_audio_cb', 0xFFA04804)}             # audio::init: DMA callback = audio render (sysmon)
# The same, in the encrypted L1 image. These patches are precomputed: public builds replay the byte differences in
# l1_patches.json; a private maintainer tool (analysis/l1_private.py, not published) regenerates them. Their targets
# are the fixed L1 vector entries (hook.S .l1vec at CODE_BASE), so plugins and other code can change freely.
L1_IMM_PATCHES = {0xFFA01018: ('l1v_metro', 0x019DE548),             # audio callback: metronome render (count-in, accent)
                  0xFFA049FC: ('l1v_push_lr', 0x019E1B3C),           # audio callback: push master L/R to USB (+ tracks)
                  0xFFA0487C: ('l1v_push_interleaved', 0x019E1A88)}  # .. muted output: silent block (+ silent tracks)
L1_VECTOR = ['l1v_metro', 'l1v_push_lr', 'l1v_push_interleaved', 'l1v_rs_probe']   # hook.S order: CODE_BASE + 4 * i
L1_PATCHES_JSON = os.path.join(HERE, 'l1_patches.json')
# L1 call-outs: replace `n` original bytes at addr by  P1.L = sym; P1.H = sym; CALL (P1); NOP...  (the target must
# redo what the replaced instructions did; written in asm in hook.S)
L1_CALLS = {0xFFA0619A: ('l1v_rs_probe', bytes.fromhex('09e1dc2949e180ff0ae154214ae180ff'))}   # tape resampler prologue
#            (P1 = 0xFF8029DC; P2 = 0xFF802154) -> track-out resampling (screens/usbtrack.c), sets P1 / P2
# pc-relative CALLs in L1 to the trampolines in the free end of L1 (.l1tramp, plain LDR block at L1_TRAMP)
L1_PCCALLS = {0xFFA0709C: ('l1_tramp_fwd', bytes.fromhex('06c41000')),   # mix loop 1: R0 = MAX (R2, R0) (V)
              0xFFA0712C: ('l1_tramp_rev', bytes.fromhex('06c40100'))}   # mix loop 2: R0 = MAX (R0, R1) (V)
L1_TRAMP, L1_TRAMP_MAX = 0xFFA0BFB4, 0xFFA0C000 - 0xFFA0BFB4
FX_SCREEN_SVG = 'fxgeneric.svg'
QUAD_LFO_SVG = 'display/quadlfo.svg'
SYSMON_SVG = 'display/sysmon.svg'
USBOUT_SVG = 'display/usbout.svg'
ITERLAB_SVG = 'display/iterlab.svg'   # stock iter screen (op1repacker iter-lab.svg, MIT)
# quad LFO defaults (lfo/quadlfo.c: 128-bit stream, LFO i at bit 30 i: rate 6 | sync 1 | wave 3 | smooth 3 | offset 3 |
# type 3 | target 5 | amount+32 6; marker 0xA5 in bits 120..127): ~0.3 / 1 / 3 / 8 Hz, sine, amount 0, VOIC, knobs b..r
def quad_words(lfos):
    bits = 0
    for i, (rate, tgt) in enumerate(lfos):
        bits |= (rate | tgt << 19 | 32 << 24) << (30 * i)
    bits |= 0xA5 << 120
    return [((bits >> (16 * k)) & 0xFFFF) - (0x10000 if (bits >> (16 * k)) & 0x8000 else 0) for k in range(8)]
QUAD_LFO_DEFAULTS = quad_words([(19, 0), (31, 1), (42, 2), (54, 3)])
# optional: hidden OP-Z controller mode, gated by host-mode SysEx 00 01 03 (docs/12_opz_mode_patch.md)
DEV_HOOKS = {0x019F01B8: ('hook_devboot', bytes.fromhex('05e37c52')),   # boot: access(te-boot.op1) -> apply a staged dev update first (docs/35)
             0x019E3CC8: ('hook_devrx', bytes.fromhex('ffe36efa'))}    # midi_poll: CALL midi_rx_parse -> copy our SysEx first
OPZ_HOOKS = {0x019E0CFC: ('hook_opzget', bytes.fromhex('00601000')),   # midi_opz_mode_enabled: R0=0; RTS
             0x019E3540: ('hook_opzset', bytes.fromhex('a5e60900'))}   # rx host-mode m!=1,2: B[P4+9]=R5

def sh(cmd):
    print('$', cmd); subprocess.run(cmd, shell=True, check=True)

def by_order(lst):
    """plugins indexed by order: [None (0 = stock), plugin with order 1 or None, ...] up to the highest order"""
    out = [None] * (max([e['order'] for e in lst], default=0) + 1)
    for e in lst:
        out[e['order']] = e
    return out

def gen_table(engs, fxs):
    # built-in plugins, indexed by number (order, stored in patches); unused numbers 0 (stock iter / filter). Compact
    # const tables: the plugin loader (plugload/plugload.c) copies them into the RAM tables (OP1_SLOTS entries, .bss)
    # at boot and adds the plugin files.
    eo, fo = by_order(engs), by_order(fxs)
    assert all(e['order'] < 32 for e in engs + fxs), 'built-in orders are 1..31 (32.. = plugin files)'
    ptr = lambda lst, f: ''.join(', ' + f(e) if e else ', 0' for e in lst[1:])
    name = lambda lst: ''.join(json.dumps(e['name'] if e else '') + ', ' for e in lst[1:]) + '0'
    svg = lambda e: json.dumps(os.path.basename(e['screen']['svg'])) if e.get('screen') else '0'
    scr = lambda e: '&' + e['screen']['symbol'] if e.get('screen') else '0'
    lines = ['#include "op1_synth.h"']
    lines += [f'extern const op1_engine_t {e["symbol"]};' for e in engs]
    lines += [f'extern const op1_fx_t {f["symbol"]};' for f in fxs]
    lines += [f'extern const op1_screen_t {e["screen"]["symbol"]};' for e in engs + fxs if e.get('screen')]
    lines.append('const op1_engine_t *const op1_builtin_engines[] = { 0' + ptr(eo, lambda e: '&' + e['symbol']) + ' };')
    lines.append(f'const int op1_builtin_engine_count = {len(eo)};')
    lines.append('const char *const op1_builtin_engine_names[] = { ' + name(eo) + ' };')
    lines.append('const char *const op1_builtin_sample_engine_names[] = { ' +
                 ''.join(json.dumps(e['name']) + ', ' for e in engs if e.get('uses_sample')) + '0 };')
    lines.append('const op1_screen_t *const op1_builtin_engine_screens[] = { 0' + ptr(eo, scr) + ' };')
    lines.append('const char *const op1_builtin_engine_svgs[] = { 0' + ptr(eo, svg) + ' };')
    lines.append('const op1_fx_t *const op1_builtin_fxs[] = { 0' + ptr(fo, lambda f: '&' + f['symbol']) + ' };')
    lines.append(f'const int op1_builtin_fx_count = {len(fo)};')
    lines.append('const char *const op1_builtin_fx_names[] = { ' + name(fo) + ' };')
    lines.append('const op1_screen_t *const op1_builtin_fx_screens[] = { 0' + ptr(fo, scr) + ' };')
    lines.append('const char *const op1_builtin_fx_svgs[] = { 0' + ptr(fo, svg) + ' };')
    open(os.path.join(HERE, 'build', 'engines_table.c'), 'w').write('\n'.join(lines) + '\n')

def build(engs, fxs):
    os.makedirs(os.path.join(HERE, 'build'), exist_ok=True)
    gen_table(engs, fxs)
    srcs = ['dispatcher.c', 'build/engines_table.c', 'lfo/quadlfo.c', 'seq/seq_core.c', 'screens/iterlab.c', 'screens/sysmon.c', 'screens/usbtrack.c', 'screens/usbout.c', 'tape/markers.c', 'screens/oblique.c', 'build/oblique_data.c', 'tape/countin.c', 'seqdev/ms_core.c', 'seqdev/ms_fw.c', 'seqdev/ms_draw.c', 'midi/mx.c', 'screens/overlay.c', 'plugload/plugload.c', 'plugload/op1p_load.c', 'screens/settings.c', 'tape/punch.c', 'screens/sampedit.c', 'tape/erase.c', 'tape/safewrite.c', 'patch_repair.c', 'drumplus/dp_core.c', 'samplerplus/sp_core.c', 'devupd/du.c', 'devupd/du_fw.c', 'screens/duprompt.c', 'screens/sbrowse.c', 'screens/fxroute.c', 'screens/mixaux.c', 'screens/scalepg.c', 'screens/op1r_settings.c', 'bgsave.c', 'scale/sc_core.c', 'scale/sc_fw.c'] + [s for e in engs for s in [e['source']] + e.get('extra_sources', [])] + [s for f in fxs for s in [f['source']] + f.get('extra_sources', [])]
    objs = []
    cmds = []
    flags = {}                                       # plugin.json "source_cflags": e.g. -O3 after the default -O2
    for e in engs + fxs:
        flags.update(plugins.source_flags(e, [e['source']] + e.get('extra_sources', [])))
    for s in srcs:
        o = 'build/' + s.replace('/', '_').replace('.c', '.o'); objs.append(o)
        cmds.append(f'bfin-elf-gcc -O2 -mcpu=bf524 -ffreestanding -fno-builtin -nostdlib -fno-common '
                    f'{" ".join(["-Wall -I.", *flags.get(s, [])])} -c {s} -o {o}')
    cmds.append('bfin-elf-as -mcpu=bf524 hook.S -o build/hook.o'); objs.append('build/hook.o')
    cmds.append(f'bfin-elf-ld -T link.ld {" ".join(objs)} $(bfin-elf-gcc -print-libgcc-file-name) -o build/synth.elf')
    cmds.append('bfin-elf-objcopy -O binary -j .text -j .rodata -j .data build/synth.elf build/synth.bin')
    for sec, _ in list(HOOKS.values()) + list(OPZ_HOOKS.values()) + list(DEV_HOOKS.values()):
        cmds.append(f'bfin-elf-objcopy -O binary -j .{sec} build/synth.elf build/{sec}.bin')
    cmds.append('bfin-elf-objcopy -O binary -j .l1tramp build/synth.elf build/l1tramp.bin')
    cmds.append('bfin-elf-nm build/synth.elf > build/synth.sym')
    cmds.append('bfin-elf-objdump -d build/synth.elf > build/synth.lst && bfin-elf-size build/synth.elf')
    sh(f'docker run --rm -u {os.getuid()}:{os.getgid()} -v "{HERE}":/work bfin-elf-toolchain '
       f'sh -c \'{" && ".join(cmds)}\'')
    syms = {}
    for line in open(os.path.join(HERE, 'build', 'synth.sym')):
        a, t, n = line.split(); syms[n.lstrip('_')] = int(a, 16)
    code = open(os.path.join(HERE, 'build', 'synth.bin'), 'rb').read()
    code += b'\0' * (-len(code) % 4)
    hooks = {}
    for addr, (sec, orig) in list(HOOKS.items()) + list(OPZ_HOOKS.items()) + list(DEV_HOOKS.items()):
        hooks[addr] = open(os.path.join(HERE, 'build', sec + '.bin'), 'rb').read()
        assert len(hooks[addr]) == len(orig), (sec, hooks[addr])
    return code, hooks, syms

def ldr_hdr(flags_dma, addr, cnt, arg):
    h = bytearray(struct.pack('<IIII', 0xAD000000 | flags_dma, addr, cnt, arg)); x = 0
    for b in h: x ^= b
    h[2] ^= x
    return bytes(h)

def ldr_blocks(d):
    off = 0
    while off + 16 <= len(d):
        code, addr, cnt, arg = struct.unpack_from('<IIII', d, off)
        yield off, code, addr, cnt, arg
        off += 16 + (0 if code & 0x0100 else cnt)

def ldr_poke(d, addr, data, expect=None):
    for off, c, a, n, g in ldr_blocks(d):
        if c & 0x1500 or not n: continue            # skip FILL / IGNORE / CALLBACK(encrypted)
        if a <= addr and addr + len(data) <= a + n:
            p = off + 16 + addr - a
            if expect is not None:
                assert bytes(d[p:p+len(expect)]) == expect, f'{addr:#x}: unexpected original bytes'
            d[p:p+len(data)] = data; return
    raise SystemExit(f'address {addr:#x} not in a plain LDR block')

# boot screen text under the TE logo: drawn by 0x01A2FCB8 -> text overlay 0x019416EA(0x018F7358).
# Original: 17 newlines + 33 spaces + "R. 00246" (58 bytes, 0-terminated). Replaced in place by "op1r <VERSION>",
# right-aligned to the same column. VERSION = synth_sdk/VERSION, a counter bumped for every released change.
BOOT_TEXT_ADDR = 0x018F7358
BOOT_TEXT_ORIG = b'\n' * 17 + b' ' * 33 + b'R. 00246'
def boot_text(release=True):
    ver = open(os.path.join(HERE, 'VERSION')).read().strip()
    tag = f'op1r {ver}'.encode()
    if not release:                                               # local build: op1r <n> YYYY-MM-DD HH:MM
        import time
        tag += time.strftime(' %Y-%m-%d %H:%M').encode()
    assert 0 < len(tag) <= 41, tag
    return b'\n' * 17 + b' ' * (41 - len(tag)) + tag

def apply_l1(d, pokes):
    """pokes: [(addr, data, expect)] inside the encrypted L1 blocks. With the private tool: patch and record the byte
    differences in l1_patches.json; without it (public builds): replay the recorded differences."""
    want = [[f'{a:#010x}', x.hex(), e.hex()] for a, x, e in pokes]
    try:
        sys.path.insert(0, os.path.join(ROOT, 'analysis'))
        import l1_private
    except ImportError:
        l1_private = None
    if l1_private:
        before = bytes(d)
        for a, x, e in pokes:
            l1_private.poke(d, ldr_blocks(d), a, x, e)
        ops, i = [], 0
        while i < len(d):                                        # changed byte ranges (same length)
            if d[i] != before[i]:
                j = i
                while j < len(d) and d[j] != before[j]: j += 1
                ops.append([i, before[i:j].hex(), bytes(d[i:j]).hex()]); i = j
            else:
                i += 1
        rec = {'_comment': 'encrypted L1 patch bytes, generated by make_firmware.py with the private tool; '
                           'public builds replay them (offsets in the original OP1_vdk.ldr)',
               'pokes': want, 'ops': ops}
        json.dump(rec, open(L1_PATCHES_JSON, 'w'), indent=1)
        return
    rec = json.load(open(L1_PATCHES_JSON))
    if rec['pokes'] != want:
        raise SystemExit('the L1 patches changed (hook addresses or L1 patch lists): l1_patches.json must be '
                         'regenerated by a maintainer with the private tool')
    for off, old, new in rec['ops']:
        assert bytes(d[off:off + len(old) // 2]).hex() == old, f'L1 bytes at {off:#x} differ from the original'
        d[off:off + len(new) // 2] = bytes.fromhex(new)

KNOB_BYTE = (0x018B62A4, b'\x00', b'\xff')    # boot: volume knob byte starts at 0, not full volume (docs/29)

def patch_ranges():
    """(start, end, label) of every byte the build patches in the firmware image"""
    r = []
    for tbl, name in ((HOOKS, 'HOOKS'), (DEV_HOOKS, 'DEV_HOOKS'), (OPZ_HOOKS, 'OPZ_HOOKS'), (L1_CALLS, 'L1_CALLS'),
                      (L1_PCCALLS, 'L1_PCCALLS')):
        r += [(a, a + len(orig), f'{name} {sym}') for a, (sym, orig) in tbl.items()]
    r += [(a, a + 4, f'DATA_PATCHES {sym}') for a, (sym, _) in DATA_PATCHES.items()]
    for tbl, name in ((IMM_PATCHES, 'IMM_PATCHES'), (L1_IMM_PATCHES, 'L1_IMM_PATCHES')):
        r += [(a + 2, a + 4, f'{name} {sym} lo') for a, (sym, _) in tbl.items()]
        r += [(a + 6, a + 8, f'{name} {sym} hi') for a, (sym, _) in tbl.items()]
    r += [(vt + 8 * slot + 4, vt + 8 * slot + 8, f'SLOTS {n}') for n, (vt, slot, _) in SLOTS.items()]
    r += [(BOOT_TEXT_ADDR, BOOT_TEXT_ADDR + len(BOOT_TEXT_ORIG), 'boot text'),
          (KNOB_BYTE[0], KNOB_BYTE[0] + len(KNOB_BYTE[1]), 'volume knob byte'),
          (L1_TRAMP, L1_TRAMP + L1_TRAMP_MAX, 'L1 trampolines')]
    return r

def check_collisions():
    """two patches touching the same firmware bytes = build error (the later one would silently win)"""
    r = sorted(patch_ranges())
    bad = [(a, b) for a, b in zip(r, r[1:]) if b[0] < a[1]]
    assert not bad, 'patch collision: ' + '; '.join(f'{x[2]} [{x[0]:#x}..{x[1]:#x}) overlaps {y[2]} [{y[0]:#x}..{y[1]:#x})'
                                                   for x, y in bad)

def patch_ldr(src, dst, code, hooks, syms, opz=False, release=True):
    d = bytearray(open(src, 'rb').read())
    if opz:
        for addr, (sec, orig) in OPZ_HOOKS.items():
            ldr_poke(d, addr, hooks[addr], orig)
        print('OP-Z mode enabled (switched on by SysEx F0 00 20 76 00 01 03 F7)')
    for addr, (sec, orig) in DEV_HOOKS.items():           # SysEx update: on in every build
        ldr_poke(d, addr, hooks[addr], orig)
    for name, (vt, slot, orig) in SLOTS.items():
        ldr_poke(d, vt + 8 * slot + 4, struct.pack('<I', syms[name]), struct.pack('<I', orig))
    for addr, data in hooks.items():
        if addr in HOOKS: ldr_poke(d, addr, data, HOOKS[addr][1])
    ldr_poke(d, BOOT_TEXT_ADDR, boot_text(release), BOOT_TEXT_ORIG)   # boot screen: op1r <version> [build time]
    # boot audio burst (stock): volume knob byte starts at 0xFF = full volume until the first TWI knob read
    # (callback 0xFFA0521C); start at 0 so the output ramp (0x01A10DE4) rises from silence to the real knob level
    ldr_poke(d, *KNOB_BYTE)
    for addr, (sym, orig) in DATA_PATCHES.items():
        ldr_poke(d, addr, struct.pack('<I', syms[sym]), struct.pack('<I', orig))
    for addr, (sym, orig) in IMM_PATCHES.items():              # keep the opcodes, swap the 16-bit halves
        v = syms[sym]
        ldr_poke(d, addr + 2, struct.pack('<H', v & 0xFFFF), struct.pack('<H', orig & 0xFFFF))
        ldr_poke(d, addr + 6, struct.pack('<H', v >> 16), struct.pack('<H', orig >> 16))
    for i, name in enumerate(L1_VECTOR):                        # encrypted L1 patches assume these addresses
        assert syms[name] == CODE_BASE + 4 * i, (name, hex(syms[name]))
    pokes = []
    for addr, (sym, orig) in L1_CALLS.items():
        v = syms[sym]
        new = struct.pack('<HHHHH', 0xE109, v & 0xFFFF, 0xE149, v >> 16, 0x0061)
        new += b'\0' * (len(orig) - len(new))                 # NOP (0x0000) fill
        assert len(new) == len(orig)
        pokes.append((addr, new, orig))
    for addr, (sym, orig) in L1_PCCALLS.items():               # CALL pcrel24: E3xx (offset >> 16) + low 16 bits
        o = ((syms[sym] - addr) >> 1) & 0xFFFFFF
        assert -0x800000 <= ((syms[sym] - addr) >> 1) < 0x800000
        pokes.append((addr, struct.pack('<HH', 0xE300 | (o >> 16), o & 0xFFFF), orig))
    for addr, (sym, orig) in L1_IMM_PATCHES.items():           # Rx.L / Rx.H pair in encrypted L1
        v = syms[sym]
        pokes.append((addr + 2, struct.pack('<H', v & 0xFFFF), struct.pack('<H', orig & 0xFFFF)))
        pokes.append((addr + 6, struct.pack('<H', v >> 16), struct.pack('<H', orig >> 16)))
    apply_l1(d, pokes)
    fin = [o for o, c, a, n, g in ldr_blocks(d) if c & 0x8000][0]
    blk = ldr_hdr(0x0006, CODE_BASE, len(code), 0) + code
    bss, bss_end = syms['bss_start'], syms['bss_end']        # .bss: zero FILL block (link.ld), no payload
    assert CODE_BASE + len(code) <= bss, (hex(CODE_BASE + len(code)), hex(bss_end))   # code below .bss
    if bss_end > bss:
        blk += ldr_hdr(0x0106, bss, bss_end - bss, 0)
    tramp = open(os.path.join(HERE, 'build', 'l1tramp.bin'), 'rb').read()
    if tramp:                                                   # L1 trampolines: plain block in free L1 instruction SRAM
        assert len(tramp) <= L1_TRAMP_MAX, len(tramp)
        tramp += b'\0' * (-len(tramp) % 4)
        blk += ldr_hdr(0x0006, L1_TRAMP, len(tramp), 0) + tramp
    d[fin:fin] = blk
    c0, a0, n0, g0 = struct.unpack_from('<IIII', d, 0)
    d[0:16] = ldr_hdr(c0 & 0xffff, a0, n0, g0 + len(blk))
    open(dst, 'wb').write(d)

def patch_db(dbp, engs, fxs):
    c = sqlite3.connect(dbp)
    have = {r[0] for r in c.execute('SELECT type FROM synth_types')}
    nid = c.execute('SELECT MAX(id) FROM synth_types').fetchone()[0] + 1
    if 'iter' not in have:
        c.execute('INSERT INTO synth_types VALUES (?,?,?)', (nid, 'iter', '[1516, 16704, 0, 15168, 0, 0, 0, 0]')); nid += 1
    for e in engs:
        k = list(e['defaults']) + [0] * (4 - len(e['defaults']))
        params = json.dumps(k[:4] + [MAGIC + e['order'], 0, 0, 0])
        if e['name'] in have:
            c.execute('UPDATE synth_types SET default_params=? WHERE type=?', (params, e['name']))
        else:
            c.execute('INSERT INTO synth_types VALUES (?,?,?)', (nid, e['name'], params)); nid += 1
    have = {r[0] for r in c.execute('SELECT type FROM fx_types')}
    if 'filter' not in have:
        fid = FILTER_ROW[0] if not c.execute('SELECT 1 FROM fx_types WHERE id=?', (FILTER_ROW[0],)).fetchone() \
            else c.execute('SELECT MAX(id) FROM fx_types').fetchone()[0] + 1
        c.execute('INSERT INTO fx_types VALUES (?,?,?)', (fid,) + FILTER_ROW[1:])
    nid = c.execute('SELECT MAX(id) FROM fx_types').fetchone()[0] + 1
    for f in fxs:
        k = list(f['defaults']) + [0] * (4 - len(f['defaults']))
        raw = [lo + (min(max(v, 0), 32767) * (hi - lo)) // 32767 for v, (lo, hi) in zip(k[:4], FX_RANGES)]
        assert all(lo <= v <= hi for v, (lo, hi) in zip(raw, FX_RANGES)), (f['name'], raw)
        p2 = list(f.get('page2_defaults', [])) + [0] * 3                 # words 5..7: page-2 params, 0..32767
        params = json.dumps(raw + [MAGIC + f['order']] + [min(max(int(v), 0), 32767) for v in p2[:3]])
        if f['name'] in have:
            c.execute('UPDATE fx_types SET default_params=? WHERE type=?', (params, f['name']))
        else:
            c.execute('INSERT INTO fx_types VALUES (?,?,?)', (nid, f['name'], params)); nid += 1
    # quad LFO (lfo/quadlfo.c): runs in the midi LFO class; words 0..3 frequency, 4..7 0x4000|free|dest|group|amount
    if not c.execute("SELECT 1 FROM lfo_types WHERE type='quad'").fetchone():
        lid = c.execute('SELECT MAX(id) FROM lfo_types').fetchone()[0] + 1
        c.execute('INSERT INTO lfo_types VALUES (?,?,?)', (lid, 'quad', json.dumps(QUAD_LFO_DEFAULTS)))
    # sequencers (docs/44) are plugin files: the loader adds / removes their seq_types rows
    # drum+ is a plugin file (docs/43): the loader adds / removes its drum_types row (id 2)
    c.commit()
    for r in c.execute('SELECT * FROM synth_types ORDER BY id'): print('  ', r)
    for r in c.execute('SELECT * FROM fx_types ORDER BY id'): print('  fx', r)
    c.close()

def main(opz=False, release=False, plugin_files='all', builtin=''):
    check_collisions()
    engs, fxs = plugins.load()
    seqs = plugins.load_seqs() + plugins.load_seqs('drum') + plugins.load_seqs('sampler') + plugins.load_seqs('tool')   # sequencers / drum+ / sampler+ / tools: plugin files
    # plugin files (docs/37, default: all) are built into the tree's plugins/ instead of being compiled in;
    # --builtin compiles the named ones (or all) into the firmware
    names = {e['name'] for e in engs + fxs + seqs}
    pick = lambda arg: names if arg == 'all' else {n.strip() for n in arg.split(',') if n.strip()}
    as_files, comp = pick(plugin_files), pick(builtin)
    assert as_files <= names and comp <= names, f'unknown plugins: {sorted((as_files | comp) - names)}'
    comp -= {e['name'] for e in seqs}                       # (sequencers cannot be compiled in)
    as_files -= comp
    missing = sorted(e['name'] for e in engs + fxs if e['name'] not in as_files and not e.get('order'))
    assert not missing, f'compiled-in plugins need an order (plugin.json): {missing}'
    file_plugins = [e for e in engs + fxs + seqs if e['name'] in as_files]
    engs = [e for e in engs if e['name'] not in as_files]
    fxs = [f for f in fxs if f['name'] not in as_files]
    # order = the number stored in patches (MAGIC + n): never renumber;
    for lst in (engs, fxs):                     # gaps are fine (an order no plugin has plays as stock iter / filter)
        assert all(1 <= e['order'] <= MAX_ORDER for e in lst), [e['order'] for e in lst]
    for f in fxs:
        assert f['name'] not in FX_BUILTIN and f['name'].islower() and len(f['name']) <= 15, f['name']
    for e in engs:
        assert e['name'] not in BUILTIN and e['name'].islower() and len(e['name']) <= 15, e['name']
    stock = set(os.listdir(os.path.join(SRC_FW, 'content', 'display')))
    for f in (QUAD_LFO_SVG, SYSMON_SVG, USBOUT_SVG):  # (oblique reuses usbout.svg: empty)
        assert os.path.basename(f) not in stock, f'{f} clashes with a stock content/display file'
    for e in engs + fxs + seqs:                  # never replace a firmware screen (boot assert "Can't find SVG object")
        if e.get('screen'):
            assert os.path.basename(e['screen']['svg']) not in stock, \
                f'{e["name"]}: {e["screen"]["svg"]} clashes with a stock content/display file, rename it'
    os.makedirs(os.path.join(HERE, 'build'), exist_ok=True)   # fresh checkout
    sys.path.insert(0, os.path.join(HERE, 'display'))
    import gen_oblique                           # cards wrapped with the stock font's widths (build time only)
    ob_cards, _ = gen_oblique.build(os.path.join(SRC_FW, 'content', 'display', 'opfont.svg'),
                                    os.path.join(HERE, 'screens', 'oblique', 'cards.txt'))
    gen_oblique.emit(ob_cards, os.path.join(HERE, 'build', 'oblique_data.c'))
    code, hooks, syms = build(engs, fxs)
    assert len(code) < 0x100000
    shutil.rmtree(OUT_FW, ignore_errors=True); shutil.copytree(SRC_FW, OUT_FW)
    if file_plugins:
        import make_plugin                       # plugin files keep their order as preferred number (old sounds)
        for e in file_plugins:
            make_plugin.build(e['name'], os.path.join(OUT_FW, 'plugins'))
    patch_ldr(os.path.join(SRC_FW, 'OP1_vdk.ldr'), os.path.join(OUT_FW, 'OP1_vdk.ldr'), code, hooks, syms, opz, release)
    print(f'code {len(code)} bytes @{CODE_BASE:#x}; ' + ', '.join(f'{k}={syms[k]:#x}' for k in SLOTS))
    patch_db(os.path.join(OUT_FW, 'content', 'op1_factory.db'), engs, fxs)
    disp = os.path.join(OUT_FW, 'content', 'display')
    for e in engs + fxs:
        if e.get('screen'):
            shutil.copy(os.path.join(HERE, e['screen']['svg']), os.path.join(disp, os.path.basename(e['screen']['svg'])))
    shutil.copy(os.path.join(HERE, QUAD_LFO_SVG), os.path.join(disp, os.path.basename(QUAD_LFO_SVG)))
    shutil.copy(os.path.join(HERE, SYSMON_SVG), os.path.join(disp, os.path.basename(SYSMON_SVG)))
    shutil.copy(os.path.join(HERE, USBOUT_SVG), os.path.join(disp, os.path.basename(USBOUT_SVG)))
    shutil.copy(os.path.join(HERE, ITERLAB_SVG), os.path.join(disp, os.path.basename(ITERLAB_SVG)))
    src_svg = os.path.join(HERE, 'display', FX_SCREEN_SVG)
    shutil.copy(src_svg if os.path.exists(src_svg) else os.path.join(disp, 'iter.svg'),
                os.path.join(disp, FX_SCREEN_SVG))
    sh(f'"{REPACKER}" repack "{OUT_FW}"')
    # development delta (tools/op1delta.py, docs/34): LDR + files changed since the tree recorded as installed
    delta = os.path.join(ROOT, os.path.basename(OUT_FW) + '-delta.op1')
    if os.path.exists(OUT_FW + '.delta-state.json'):
        sh(f'python3 "{os.path.join(ROOT, "tools", "op1delta.py")}" "{OUT_FW}" -o "{delta}"')
    else:
        print(f'no delta: record the installed tree first (tools/op1delta.py {os.path.basename(OUT_FW)} --init)')

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--opz', action='store_true', help='enable hidden OP-Z controller mode (via SysEx 00 01 03)')
    ap.add_argument('--release', action='store_true', help='boot screen without build time (patcher builds)')
    ap.add_argument('--plugin-files', default='all', metavar='NAMES',
                    help='plugins built as plugin files (tree plugins/), comma-separated or "all" (default)')
    ap.add_argument('--builtin', default='', metavar='NAMES',
                    help='plugins compiled into the firmware instead, comma-separated or "all"')
    a = ap.parse_args()
    main(a.opz, a.release, a.plugin_files, a.builtin)
