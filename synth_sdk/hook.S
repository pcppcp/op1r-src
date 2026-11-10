/* 4-byte JUMP.L placed at 0x019B975C (instr_types name compare) -> instr_name_eq */
	.section .hook_namecmp,"ax"
	.global __hook_namecmp
__hook_namecmp:
	jump.l _instr_name_eq;

/* --dev only: CALL access("/yaffs2/te-boot.op1") at boot (0x019F01B8) -> du_boot_access (devupd/du_fw.c) */
	.section .hook_devrx,"ax"
	.global __hook_devrx
__hook_devrx:
	call _du_rx_poll;
	.section .hook_devboot,"ax"
	call _du_boot_access;

/* CALL at 0x01A01844 (patch::sample_path "sampler" check) -> sample_type_eq */
	.section .hook_samplecheck,"ax"
	.global __hook_samplecheck
__hook_samplecheck:
	call _sample_type_eq;

/* JUMP.L at 0x019AAD8E (fx_types name compare) -> fx_name_eq */
	.section .hook_fxnamecmp,"ax"
	.global __hook_fxnamecmp
__hook_fxnamecmp:
	jump.l _fx_name_eq;

/* ScreenId::for_fx jump-table case 5 target. Entered with the for_fx frame (LINK 0xc) still active,
 * like the original case code: compute R0, then  P0=[FP+4]; UNLINK; JUMP (P0). */
	.text
	.global _fx_screen_case5
	.align 2
_fx_screen_case5:
	call _fx_screen_select;
	P0 = [FP + 4];
	unlink;
	jump (P0);

	.section .hook_iterread,"ax"
	call _iter_read_hook;
	.section .hook_screenctor,"ax"
	call _screens_ctor_hook;
	.section .hook_plugboot,"ax"
	call _plug_boot_hook;

	.section .hook_btopen,"ax"
	call _bt_db_open;
	.section .hook_btfat,"ax"
	call _bt_fat_init;
	.section .hook_btsnd,"ax"
	call _bt_sounds;

	.section .hook_masterload,"ax"
	call _plug_master_load;
	.section .hook_pbxf1,"ax"
	call _fg_xform;
	.section .hook_pbxf2,"ax"
	call _fg_xform;
	.section .hook_pbname,"ax"
	call _fg_rowtext;
	.section .hook_apsynth1,"ax"
	call _plug_apply_synth;
	.section .hook_apsynth2,"ax"
	call _plug_apply_synth;
	.section .hook_apsynth3,"ax"
	call _plug_apply_synth;

/* MIDI sequencer device (seqdev/ms_fw.c): VoiceCtl key_down 0x01A85750 / key_up 0x01A85840 entries (LINK 0x10 /
 * LINK 0x20) -> JUMP.L here: the note is offered to the recorder (R0 = record, R1 = param; the functions read no
 * other argument registers, checked), then the original LINK and on. (Stubs from the MIDI track PR.) */
	.section .hook_vcdown,"ax"
	jump.l _ms_vc_down_stub;
	.section .hook_vcup,"ax"
	jump.l _ms_vc_up_stub;
	.text
	.align 2
_ms_vc_down_stub:
	[--SP] = RETS;			/* scale mode (scale/sc_fw.c): R0 = the record to play, 0 = skip */
	[--SP] = R1;
	R1 = RETS;
	SP += -12;
	call _sc_vc_down;
	SP += 12;
	R1 = [SP++];
	RETS = [SP++];
	CC = R0 == 0;
	IF CC JUMP 1f;
	[--SP] = RETS;
	[--SP] = R0;
	[--SP] = R1;
	R2 = RETS;			/* caller: the input source (ms_fw.c src_of) */
	SP += -12;
	call _ms_vc_down;
	SP += 12;
	R1 = [SP++];
	R0 = [SP++];
	RETS = [SP++];
	LINK 16;
	jump.l _vc_down_body;
1:	rts;
	.align 2
_ms_vc_up_stub:
	[--SP] = RETS;			/* scale mode: R0 = the record to release */
	R1 = RETS;
	SP += -12;
	call _sc_vc_up;
	SP += 12;
	RETS = [SP++];
	[--SP] = RETS;
	[--SP] = R0;
	R1 = RETS;
	SP += -12;
	call _ms_vc_up;
	SP += 12;
	R0 = [SP++];
	RETS = [SP++];
	LINK 32;
	jump.l _vc_up_body;

	.section .hook_screenload,"ax"
	call _screens_load_hook;

/* ScreenId::for_synth jump-table case 14 (iter) target, same frame convention as fx_screen_case5 */
	.text
	.global _synth_screen_case14
	.align 2
_synth_screen_case14:
	call _synth_screen_select;
	P0 = [FP + 4];
	unlink;
	jump (P0);

/* ---- optional OP-Z controller mode (make_firmware.py --opz), docs/12 ----
 * Flag 0x018B6016 = OP-Z host mode. Stock fw clears it on COM->CTRL entry (midi_send_identity_request)
 * and on every TE host-mode SysEx F0 00 20 76 00 01 <m> F7, but never sets or reads it.
 * midi_opz_mode_enabled (0x019E0CFC, stub "return 0") -> return the flag. */
	.section .hook_opzget,"ax"
	jump.l _opz_mode_enabled;
	.text
	.align 2
	.global _opz_mode_enabled
_opz_mode_enabled:
	P1.L = 0x6016;
	P1.H = 0x018B;
	R0 = B[P1] (Z);
	rts;

/* midi_rx_parse, host-mode SysEx branch for m != 1,2 (0x019E3540, was B[P4+9] = R5).
 * Entry: P4 = 0x018B600C (+8 ableton, +9 reason, +A opz), R0 = m. m == 3 -> OP-Z mode.
 * (stock code stores R5 here, assumed 0 - but a preceding 00 04 SysEx can leave R5 = 1) */
	.section .hook_opzset,"ax"
	jump.l _opz_hostmode_other;
	.text
	.align 2
_opz_hostmode_other:
	CC = R0 == 3;
	R0 = CC;
	B[P4 + 10] = R0;
	R0 = 0;
	B[P4 + 9] = R0;
	jump.l _midi_rx_parse_next;

/* every CALL read_fx_param (0x01A28DA0) in the firmware -> read_fx_param_safe (dispatcher.c):
 * clamps custom-FX knob words into the filter ranges first, so the range assert can never fire. */
	.section .hook_rfp019427b8,"ax"
	call _read_fx_param_safe;
	.section .hook_rfp01984f60,"ax"
	call _read_fx_param_safe;
	.section .hook_rfp019a74d4,"ax"
	call _read_fx_param_safe;
	.section .hook_rfp019b05f8,"ax"
	call _read_fx_param_safe;
	.section .hook_rfp019e6a48,"ax"
	call _read_fx_param_safe;
	.section .hook_rfp019e832c,"ax"
	call _read_fx_param_safe;
	.section .hook_rfp01a1cfd2,"ax"
	call _read_fx_param_safe;
	.section .hook_rfp01a33fb2,"ax"
	call _read_fx_param_safe;

/* KeyHandler::process_queue dispatch -> encoder press / turn handling for custom FX pages (dispatcher.c) */
	.section .hook_keyev,"ax"
	call _key_event_hook;
	.section .hook_knobev,"ax"
	call _knob_event_hook;
/* midi_rx_parse FA (MIDI START) branch: CALL 0x01A5E7D0 (reset beat counters) -> midi_start_hook (dispatcher.c) */
	.section .hook_midistart,"ax"
	call _midi_start_hook;
/* VoiceCtl::on_midi_rx 0xB0 branch: CALL midi_message (0x01A84F8C) -> midi_cc_hook (dispatcher.c) */
	.section .hook_midicc,"ax"
	call _midi_cc_hook;

/* ---- input engines (input/in_host.c, after Hundekraft kennel 3.2.2 kennel/kn_input.S) ----
 * synth_bus_dc_block 0x019674F8: its 6-byte prologue ([--SP] = (R7:6); P0.L = 0xAC14) -> in_highpass_entry, which
 * passes &(caller's has-input flag at SP+0x34) and the return address to in_bus. PmSynth keeps its frame pointer in M1
 * across this call (0xFFA02896: P1 = M1) and C treats M1 as scratch: saved around in_bus. */
	.section .hook_inputbus,"ax"
	jump.l _in_highpass_entry;
	nop;
	.text
	.align 2
	.global _in_highpass_entry
_in_highpass_entry:
	R0 = SP;
	R0 += 0x34;
	R1 = RETS;
	[--SP] = RETS;
	[--SP] = M1;
	SP += -12;
	call _in_bus;
	SP += 12;
	M1 = [SP++];
	RETS = [SP++];
	rts;
	.global _in_highpass_stock
_in_highpass_stock:                     /* the stock prologue, then on at 0x019674FE */
	[--SP] = (R7:6);
	P0.L = 0xAC14;
	P1.L = 0x74FE;
	P1.H = 0x0196;
	jump (P1);

/* ---- quad LFO (lfo/quadlfo.c) ---- */
/* lfo_types name compares (from_name 0x019C9C3A, is_valid 0x019C9BE2): "midi" also matches "quad" */
	.section .hook_lfoname1,"ax"
	call _lfo_name_eq;
	.section .hook_lfoname2,"ax"
	call _lfo_name_eq;
/* every CALL read_lfo_param (0x01A293BC) -> read_lfo_param_safe (quad words would trip the range assert) */
	.section .hook_rlp0193de2a,"ax"
	call _read_lfo_param_safe;
	.section .hook_rlp01968c34,"ax"
	call _read_lfo_param_safe;
	.section .hook_rlp01981eda,"ax"
	call _read_lfo_param_safe;
	.section .hook_rlp019e4a18,"ax"
	call _read_lfo_param_safe;
	.section .hook_rlp01a2d388,"ax"
	call _read_lfo_param_safe;
	.section .hook_rlp01a30c3c,"ax"
	call _read_lfo_param_safe;
	.section .hook_rlp01a48fe8,"ax"
	call _read_lfo_param_safe;

/* ScreenId::for_lfo case 4 (midi) target, same frame convention as fx_screen_case5 */
	.text
	.global _lfo_screen_case4
	.align 2
_lfo_screen_case4:
	call _lfo_screen_select;
	P0 = [FP + 4];
	unlink;
	jump (P0);

/* LfoMidi vtable apply entries (synth 0x01910840, drum 0x01910860, FX 0x01910880) -> here.
 * Entry R0 = this, R1 = LFO param words, R2 = pass. Quad words -> our code, anything else -> tail jump to
 * the original LfoMidi method with registers and stack untouched. */
.macro QL_STUB name, orig, ours
	.global \name
	.align 2
\name:
	[--SP] = RETS;
	[--SP] = R0;
	[--SP] = R1;
	[--SP] = R2;
	SP += -12;
	R0 = R1;
	call _ql_is_quad;
	SP += 12;
	CC = R0 == 0;
	R2 = [SP++];
	R1 = [SP++];
	R0 = [SP++];
	RETS = [SP++];
	IF !CC JUMP 1f;
	jump.l \orig;
1:
	jump.l \ours;
.endm
	QL_STUB _ql_vt_synth, _midi_apply_synth, _ql_apply_synth
	QL_STUB _ql_vt_fx, _midi_apply_fx, _ql_apply_fx
	QL_STUB _ql_vt_drum, _midi_apply_drum, _ql_apply_drum
	.global _ql_apply_none
_ql_apply_none:
	R0 = 0;
	rts;

/* parse_patch_json LFO word range check (0x01A042A2) -> lfo_range_ok (accepts quad words, dispatcher.c) */
	.section .hook_lforange,"ax"
	call _lfo_range_ok;

/* PlayerManager::create_player case 3 (CALL 0x019ED460 finger singleton @0x01A0D150) -> finger, euclid or dots */
	.section .hook_euplayer,"ax"
	call _seq_player_get;

/* SeqIds::from_name CALLs -> eu_seq_from_name ("euclid" = type 3 + flag) */
	.section .hook_seqname1,"ax"
	call _eu_seq_from_name;
	.section .hook_seqname2,"ax"
	call _eu_seq_from_name;
	.section .hook_seqname3,"ax"
	call _eu_seq_from_name;
/* SeqIds::name(type) entry -> eu_seq_name (returns "euclid" for type 3 while euclid is active) */
	.section .hook_seqtoname,"ax"
	jump.l _eu_seq_name;

/* input key "sampler?" checks -> rec_type_ok (dispatcher.c). Each replaces "CC = Rx == R(6); IF !CC JUMP no"
 * (4 bytes) with a CALL; the stub keeps all registers, returns to the next instruction (yes) or to `no`. */
	.section .hook_recA,"ax"
	call _rec_check_a;
	.section .hook_recB,"ax"
	call _rec_check_b;
	.section .hook_recend,"ax"
	call _rec_end_patch;
	.text
	.align 2
_rec_check_a:                       /* on_key @0x019AC8B4: type in R0, no -> 0x019AC8CC */
	[--SP] = RETS;
	[--SP] = (R7:0, P5:0);
	SP += -12;
	call _rec_type_ok;
	SP += 12;
	CC = R0 == 0;
	(R7:0, P5:0) = [SP++];
	RETS = [SP++];
	if CC jump 1f;
	rts;
1:	[--SP] = P0;
	P0.L = 0xC8CC;
	P0.H = 0x019A;
	RETS = P0;
	P0 = [SP++];
	rts;
_rec_check_b:                       /* on_key_shift @0x019ACC7C: type in R1, no -> 0x019ACC8C */
	[--SP] = RETS;
	[--SP] = (R7:0, P5:0);
	SP += -12;
	R0 = R1;
	call _rec_type_ok;
	SP += 12;
	CC = R0 == 0;
	(R7:0, P5:0) = [SP++];
	RETS = [SP++];
	if CC jump 1f;
	rts;
1:	[--SP] = P0;
	P0.L = 0xCC8C;
	P0.H = 0x019A;
	RETS = P0;
	P0 = [SP++];
	rts;

/* L1 tape resampler 0xFFA06194 prologue call-out (make_firmware.py L1_CALLS): replaces P1 = 0xFF8029DC;
 * P2 = 0xFF802154. Entered after LINK 0 + push (R7:6, P5:3); R0 = position (1/64 samples), R1 = increment,
 * R2 = gain increment pointer: all kept. ASTAT kept: the tape code sets CC (lp_index < 0) before the call and tests it
 * after (0xFFA0724E / 0xFFA0725C, assert tape_c.cpp:1039); the stock resampler never touches CC. The resampler sets every other register it uses afterwards, so the C call
 * (track outs, seq/... usbtrack.c) may clobber the C caller-saved registers. Caller frame: [FP] = tape FP,
 * [FP + 0x40] = tape [SP + 0x38] = samples read per block. */
	.text
	.global _l1_rs_probe
	.align 2
_l1_rs_probe:
	[--SP] = ASTAT;                           /* CC is live across the resampler call (tape code tests it after) */
	[--SP] = RETS;
	[--SP] = R0;
	[--SP] = R1;
	[--SP] = R2;
	SP += -12;
	call _tr_resample;
	SP += 12;
	R2 = [SP++];
	R1 = [SP++];
	R0 = [SP++];
	RETS = [SP++];
	ASTAT = [SP++];
	P1.L = 0x29dc; P1.H = 0xff80;
	P2.L = 0x2154; P2.H = 0xff80;
	rts;

/* Tape mix loops (L1): the per-track peak-meter MAX instruction is replaced by CALL to an L1 trampoline, which calls
 * these. They do the MAX and log {src, dest, track, N} for the track outs. Must preserve every register but R0.
 * Stack on entry: trampoline pushed RETS + P1 (8 bytes) -> tape SP = SP + 8. */
	.global _mix_hook_fwd
	.align 2
_mix_hook_fwd:                                /* loop 1 @0xFFA0709C: src P4, dest P3, track R1 */
	R0 = MAX (R2, R0) (V);
	[--SP] = P0; [--SP] = P5; [--SP] = R7; [--SP] = R6;
	R6 = P4; R7 = P3;
	[--SP] = R1;
	jump.s _mix_rec_common;
	.global _mix_hook_rev
	.align 2
_mix_hook_rev:                                /* loop 2 @0xFFA0712C: src P1 (+2), dest I1, track R3 */
	R0 = MAX (R0, R1) (V);
	[--SP] = P0; [--SP] = P5; [--SP] = R7; [--SP] = R6;   /* 16 bytes: the trampoline's saved P1 is now at SP + 16 */
	R6 = [SP + 16];                           /* the loop's P1: pushed by the trampoline (P1 = hook address here) */
	R6 += 2; R7 = I1;
	[--SP] = R3;
_mix_rec_common:                              /* stack: track, R6, R7, P5, P0 (20 bytes) + 8 -> tape SP = SP + 28 */
	[--SP] = ASTAT;                           /* CC is live in the mix loop (IF CC JUMP right after) */
	[--SP] = R5;
	[--SP] = R4;                              /* + 12 -> tape SP = SP + 40 */
	P5.L = _tr_rec; P5.H = _tr_rec;
	R5 = [P5];
	R4 = 16;
	CC = R5 < R4;
	if !CC jump _mix_rec_full;
	P0 = R5;
	R5 += 1;
	[P5] = R5;
	P0 = P0 << 2;
	P0 = P0 << 2;
	P5 = P5 + P0;
	[P5 + 4] = R6;                            /* src */
	[P5 + 8] = R7;                            /* dest */
	R5 = [SP + 12];                           /* track */
	[P5 + 12] = R5;
	R5 = [SP + 0x60];                         /* tape [SP + 0x38] = N: SP + 40 + 0x38 */
	[P5 + 16] = R5;
_mix_rec_full:
	R4 = [SP++]; R5 = [SP++]; ASTAT = [SP++];
	SP += 4;
	R6 = [SP++]; R7 = [SP++]; P5 = [SP++]; P0 = [SP++];
	rts;

/* L1 trampolines (plain LDR block at 0xFFA0BFB4, the free end of L1 instruction SRAM): pc-relative CALL range */
	.section .l1tramp,"ax"
	.global _l1_tramp_fwd
	.align 2
_l1_tramp_fwd:
	[--SP] = RETS; [--SP] = P1;
	P1.L = _mix_hook_fwd; P1.H = _mix_hook_fwd;
	call (P1);
	P1 = [SP++]; RETS = [SP++];
	rts;
	.global _l1_tramp_rev
	.align 2
_l1_tramp_rev:
	[--SP] = RETS; [--SP] = P1;
	P1.L = _mix_hook_rev; P1.H = _mix_hook_rev;
	call (P1);
	P1 = [SP++]; RETS = [SP++];
	rts;

/* USB 6 channels (screens/usbtrack.c) */
	.section .hook_usbdesc,"ax"
	.global __hook_usbdesc
__hook_usbdesc:
	call _usb6_desc_init;
	.section .hook_usbiso1,"ax"
	.global __hook_usbiso1
__hook_usbiso1:
	call _usb6_iso_in_silent;
	.section .hook_ciservice,"ax"
	.global __hook_ciservice
__hook_ciservice:
	call _ci_service;
	.section .hook_taperase,"ax"
	.global __hook_taperase
__hook_taperase:
	call _fast_tape_erase;
	.section .hook_lander1,"ax"
	call _lander_stub;
	.section .hook_lander2,"ax"
	call _lander_stub;
	.section .hook_lander3,"ax"
	call _lander_stub;
	.section .hook_pparse1,"ax"
	call _patch_parse_safe;
	.section .hook_pparse2,"ax"
	call _patch_parse_safe;
	.section .hook_pparse3,"ax"
	call _patch_parse_safe;
	.section .hook_pparse4,"ax"
	call _patch_parse_safe;
	.section .hook_tapesave,"ax"
	.global __hook_tapesave
__hook_tapesave:
	call _tape_safe_write;
	.section .hook_tapeflush,"ax"
	.global __hook_tapeflush
__hook_tapeflush:
	call _tape_service;
/* sampler+ (docs/46): JUMP.L at the Adsr_render entry -> sp_adsr (same arguments, 4th on the stack); the stock
 * function = the two replaced instructions, then on at 0x0191212C */
	.section .hook_spadsr,"ax"
	jump.l _sp_adsr_entry;
	.text
/* fast path: straight to the stock envelope unless sp_hook_live (a sampler+ / sampler slot is current or a tool fade
 * runs; sp_core.c, set from the main loop). The C path for every voice of every engine made the audio crackle on
 * hardware with FX on and 4+ voices (op1r 38 / 39, not in the emulator). R3 / P1 are scratch at this entry. */
	.global _sp_adsr_entry
	.align 2
_sp_adsr_entry:
	P1.L = _sp_hook_live; P1.H = _sp_hook_live;
	R3 = B[P1] (Z);
	CC = R3 == 0;
	IF !CC JUMP 1f;
	[--SP] = (R7:4, P5:3);
	P5 = R2;
	jump.l _sp_adsr_cont;
1:	jump.l _sp_adsr;
	.global _sp_adsr_stock
	.align 2
_sp_adsr_stock:
	[--SP] = (R7:4, P5:3);
	P5 = R2;
	jump.l _sp_adsr_cont;                        /* link.ld: 0x0191212C */

/* master FX source (screens/fxroute.c): the LFO / seq tick 0x019C9D04 (only caller: L1 after tape_play_mix) -> fr_snap,
 * which also does the tick's stock body */
	.section .hook_frsnap,"ax"
	jump.l _fr_snap;

/* delayed patch saves (bgsave.c): the SaveSlotJob queue call goes to bgs_queue_slot; bgs_queue_stock = the original */
/* sampler presets over R246's limit (patchjob.c, after kennel): the loader's panic branch (0x01A05984; the count [P3]
 * already limit + 1, R1 = the limit) -> count back to the limit, continue with the next row (0x01A0596A) */
	.section .hook_smpover,"ax"
	jump.l _pj_sampler_over;
	.text
	.align 2
	.global _pj_sampler_over
_pj_sampler_over:
	[P3] = R1;
	P0.L = _pj_skipped; P0.H = _pj_skipped;
	R0 = [P0]; R0 += 1; [P0] = R0;
	P0.L = 0x596a; P0.H = 0x01a0;
	jump (P0);
/* disk-mode export (0x0199DA6C), sampler row: CALL yaffs_open at 0x0199DD7A, never closed -> pj_probe_open */
	.section .hook_diskprobe,"ax"
	call _pj_probe_open;
	.section .hook_crash,"ax"
	jump.l _crash_print;                     /* LINK 0x10 of the FP-chain printer 0x0191995C: our crash lines */
	.section .hook_bgsgc,"ax"
	call _bgs_idle_gc;
	.section .hook_bgsqueue,"ax"
	jump.l _bgs_queue_slot;
	.text
	.global _bgs_queue_stock
	.align 2
_bgs_queue_stock:
	LINK 0;
	jump.l _bgs_queue_cont;

	.section .hook_dpkeep,"ax"
	.global __hook_dpkeep
__hook_dpkeep:
	call _dp_keep_op1r;
	.section .hook_dpbrowse,"ax"
	.global __hook_dpbrowse
__hook_dpbrowse:
	call _dp_browser_id;
	.section .hook_dpload,"ax"
	.global __hook_dpload
__hook_dpload:
	call _dp_load_parse;
	.section .hook_sqlcache,"ax"
	.global __hook_sqlcache
__hook_sqlcache:
	call _sqlite_cache_hook;
	.section .hook_usbiso2,"ax"
	.global __hook_usbiso2
__hook_usbiso2:
	call _usb6_iso_in;
	.section .hook_usbiso3,"ax"
	.global __hook_usbiso3
__hook_usbiso3:
	call _usb6_iso_in_silent;
	.section .hook_usbbuf1,"ax"
	.global __hook_usbbuf1
__hook_usbbuf1:
	R2 = 0x33c (X);
	.section .hook_lpsyn1,"ax"
	call _lp_set_synth;
	.section .hook_lpsyn2,"ax"
	call _lp_set_synth;
	.section .hook_lpdrm1,"ax"
	call _lp_set_drum;
	.section .hook_lpdrm2,"ax"
	call _lp_set_drum;
	.section .hook_lpgrow,"ax"		/* db2::load_presets: new capacity = cap + (cap << 5) (stock: cap >> 1), docs/79 */
	.global __hook_lpgrow
__hook_lpgrow:
	R4 = R3 << 0x5;
	.section .hook_usbbuf2,"ax"
	.global __hook_usbbuf2
__hook_usbbuf2:
	R2 = 0x33c (X);

/* ---- L1 vector: fixed entry points for the calls patched into the encrypted L1 code ----
 * Placed first in .text (link.ld), so its addresses (CODE_BASE + 4 * i) never move: the encrypted L1 patches are
 * then constant and ship as precomputed bytes (l1_patches.json); plugins can change without regenerating them.
 * JUMP.L keeps all registers, RETS and ASTAT: the target returns straight to the L1 caller. Append only. */
	.section .l1vec, "ax"
	.global _l1v_metro, _l1v_push_lr, _l1v_push_interleaved, _l1v_rs_probe
_l1v_metro:            JUMP.L _ci_metro_render
_l1v_push_lr:          JUMP.L _usb6_push_lr
_l1v_push_interleaved: JUMP.L _usb6_push_interleaved
_l1v_rs_probe:         JUMP.L _l1_rs_probe

/* Exact GUI frame skip (screens/guiskip.c, docs/74; hooked only with OP1R_GUISKIP=1). Entry stubs call the C
 * recorder with the original arguments; a non-zero result skips the body (RTS to the caller), else the replaced
 * first instruction(s) run and the body continues. */
	.section .hook_gsmove,"ax"
	jump.l _gs_move_stub;
	.section .hook_gsline,"ax"
	jump.l _gs_line_stub;
	.section .hook_gsframe,"ax"
	call _gs_frame;
	.section .hook_gstext,"ax"
	jump.l _gs_text_stub;
	.section .hook_gsdim,"ax"
	jump.l _gs_dim_stub;
	.section .hook_gsfade,"ax"
	jump.l _gs_fade_stub;
	.section .hook_gsdot,"ax"
	jump.l _gs_dot_stub;
	.section .hook_gssw1,"ax"
	P1.L = _gs_sweep_stub; P1.H = _gs_sweep_stub;
	.section .hook_gssw2,"ax"
	P1.L = _gs_sweep_stub; P1.H = _gs_sweep_stub;
	.section .hook_gssw3,"ax"
	P1.L = _gs_sweep_stub; P1.H = _gs_sweep_stub;
	.section .hook_gssw4,"ax"
	P1.L = _gs_sweep_stub; P1.H = _gs_sweep_stub;
	.section .hook_gssw5,"ax"
	P1.L = _gs_sweep_stub; P1.H = _gs_sweep_stub;
	.text
	.align 2
/* save R0..R2 / RETS, call C (args prepared by the macro user), CC = result != 0, restore */
.macro GS_CALL fn
	[--SP] = RETS; [--SP] = R0; [--SP] = R1; [--SP] = R2;
	SP += -12;
	call \fn;
	SP += 12;
	CC = R0 == 0;
	R2 = [SP++]; R1 = [SP++]; R0 = [SP++]; RETS = [SP++];
.endm
_gs_move_stub:
	[--SP] = P0; P0.L = _gs_caller; P0.H = _gs_caller; R3 = RETS; [P0] = R3; P0 = [SP++];
	GS_CALL _gs_point
	IF !CC JUMP 1f;
	LINK 0;
	jump.l _rs_move_body;
_gs_line_stub:
	[--SP] = P0; P0.L = _gs_caller; P0.H = _gs_caller; R3 = RETS; [P0] = R3; P0 = [SP++];
	GS_CALL _gs_point
	IF !CC JUMP 1f;
	LINK 0;
	jump.l _rs_line_body;
_gs_text_stub:
	GS_CALL _gs_text
	IF !CC JUMP 1f;
	LINK 0x24;
	jump.l _txt_draw_body;
_gs_dot_stub:
	[--SP] = R0; R0 = 2;
	GS_CALL _gs_misc_dot
	R0 = [SP++];
	IF !CC JUMP 1f;
	LINK 0x18;
	jump.l _dot_body;
_gs_dim_stub:
	GS_CALL _gs_misc_dim
	IF !CC JUMP 1f;
	P0 = R0;
	[--SP] = (R7:4, P5:5);
	jump.l _dim_body;
_gs_fade_stub:				/* Raster::fade(R0 raster, R1 amount) */
	GS_CALL _gs_misc_fade
	IF !CC JUMP 1f;
	P0 = R0;
	[--SP] = (R7:4, P5:5);
	jump.l _fade_body;
_gs_sweep_stub:
	[--SP] = RETS; [--SP] = R0; [--SP] = R1; [--SP] = R2;
	SP += -12;
	R1 = R2;
	call _gs_sweep;
	SP += 12;
	CC = R0 == 0;
	R2 = [SP++]; R1 = [SP++]; R0 = [SP++]; RETS = [SP++];
	IF !CC JUMP 1f;
	P1.L = 0x55b4; P1.H = 0xffa0;
	jump (P1);
1:	RTS;

/* lazy stock screens (dispatcher.c lz_guard, docs/79): Screen::find_node 0x01A3B580 entry (LINK 0) -> parse an
 * unparsed screen first (R0 = the screen object, R1 = the node id), then the original LINK and on */
	.section .hook_lzfind,"ax"
	jump.l _lz_find_stub;
	.text
	.align 2
_lz_find_stub:
	[--SP] = RETS; [--SP] = R0; [--SP] = R1; [--SP] = R2;
	SP += -12;
	call _lz_guard;
	SP += 12;
	R2 = [SP++]; R1 = [SP++]; R0 = [SP++]; RETS = [SP++];
	LINK 0;
	jump.l _find_node_body;
