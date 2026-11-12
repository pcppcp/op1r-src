/* Custom code in SDRAM 0x01E00000.. (free: image+bss end 0x01DFFF34, heap0 0x100..0x1000000,
   heap1 0x03040000.., ICPLB maps 0..64MB). .hook_* sections are patched into the original image. */
ENTRY(_synth_render)
SECTIONS {
  . = 0x01E00000;
  .text   : { KEEP(*(.l1vec)) *(.text*) }       /* .l1vec first: fixed L1 entry points (hook.S) */
  .rodata : { *(.rodata*) }
  .data   : { *(.data*) }
  __image_end = .;
  /* zero-initialised data right after the image, as an LDR FILL block (no bytes in the image) */
  .bss ALIGN(4) (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; }
  .l1eq 0xFFA030F0 : { KEEP(*(.l1eq.0)) KEEP(*(.l1eq.1)) KEEP(*(.l1eq.2)) }   /* dsp/eq_l1.c + fir_l1.S: in place of master_eq (docs/81) */
  .l1dec 0xFFA005D4 : { KEEP(*(.l1dec)) }                                /* dsp/dec_l1.S: in place of synth_bus_decimate (docs/81) */
  .l1master 0xFFA00E50 : { KEEP(*(.l1master.0)) KEEP(*(.l1master.1)) KEEP(*(.l1master.2)) KEEP(*(.l1master.3)) KEEP(*(.l1master.9)) }   /* our master section in place of Core_mix_master (docs/81) */
  .l1mix 0xFFA00E58 : { KEEP(*(.l1mix)) KEEP(*(.l1mix.1)) }                                /* dsp/mix_l1.S: in place of Core_mix_master's mix loops (docs/81) */
  .l1out 0xFFA04280 : { KEEP(*(.l1out.0)) KEEP(*(.l1out.1)) }           /* dsp/outstage_l1.c: in place of master_output_stage (docs/81) */
  .l1tape 0xFFA01D60 : { KEEP(*(.l1tape)) }      /* tape/tm_l1.S: in the dead Lander audio range (docs/56, docs/82) */
  _l1_lander_end = 0xFFA020A8;
  .l1tramp 0xFFA0BFB4 : { KEEP(*(.l1tramp)) }    /* free end of L1 instruction SRAM (image ends 0xFFA0BFB4) */
  .hook_namecmp 0x019B975C : { KEEP(*(.hook_namecmp)) }
  .hook_samplecheck 0x01A01844 : { KEEP(*(.hook_samplecheck)) }
  .hook_fxnamecmp 0x019AAD8E : { KEEP(*(.hook_fxnamecmp)) }
  .hook_iterread 0x019B9EF8 : { KEEP(*(.hook_iterread)) }
  .hook_screenctor 0x01A3BD58 : { KEEP(*(.hook_screenctor)) }
  .hook_plugboot 0x019F02EE : { KEEP(*(.hook_plugboot)) }
  .hook_btopen 0x019F02E0 : { KEEP(*(.hook_btopen)) }
  .hook_btfat 0x019F02F4 : { KEEP(*(.hook_btfat)) }
  .hook_btsnd 0x019F0456 : { KEEP(*(.hook_btsnd)) }
  .hook_masterload 0x019F054A : { KEEP(*(.hook_masterload)) }
  .hook_pbxf1 0x01A16936 : { KEEP(*(.hook_pbxf1)) }
  .hook_pbxf2 0x01A16D7C : { KEEP(*(.hook_pbxf2)) }
  .hook_pbname 0x01A16048 : { KEEP(*(.hook_pbname)) }
  .hook_apsynth1 0x019C2E5E : { KEEP(*(.hook_apsynth1)) }
  .hook_apsynth2 0x01A01866 : { KEEP(*(.hook_apsynth2)) }
  .hook_apsynth3 0x01A50F1C : { KEEP(*(.hook_apsynth3)) }
  .hook_vcdown 0x01A85750 : { KEEP(*(.hook_vcdown)) }
  .hook_gsmove 0x019149C4 : { KEEP(*(.hook_gsmove)) }
  .hook_gsline 0x01914904 : { KEEP(*(.hook_gsline)) }
  .hook_gsframe 0x01A3BBE0 : { KEEP(*(.hook_gsframe)) }
  .hook_lzfind 0x01A3B580 : { KEEP(*(.hook_lzfind)) }
  .hook_gstext 0x01A6259C : { KEEP(*(.hook_gstext)) }
  .hook_gsfade 0x01A22F3C : { KEEP(*(.hook_gsfade)) }
  .hook_gsdim 0x01A2357A : { KEEP(*(.hook_gsdim)) }
  .hook_gsdot 0x01A24CBC : { KEEP(*(.hook_gsdot)) }
  .hook_gssw1 0x01A24CA6 : { KEEP(*(.hook_gssw1)) }
  .hook_gssw2 0x01A254DC : { KEEP(*(.hook_gssw2)) }
  .hook_gssw3 0x01A27316 : { KEEP(*(.hook_gssw3)) }
  .hook_gssw4 0x01A2749A : { KEEP(*(.hook_gssw4)) }
  .hook_gssw5 0x01A274FE : { KEEP(*(.hook_gssw5)) }
  .hook_vcup 0x01A85840 : { KEEP(*(.hook_vcup)) }
  _vc_down_body = 0x01A85754;      /* VoiceCtl key_down after its LINK 0x10 (sequencer hook) */
  _vc_up_body = 0x01A85844;        /* VoiceCtl key_up after its LINK 0x20 */
  _rs_move_body = 0x019149C8;      /* rasterizer move_to after its LINK 0 (guiskip) */
  _rs_line_body = 0x01914908;      /* rasterizer line_to after its LINK 0 */
  _mix_ret = 0xFFA00FDC;          /* Core_mix_master after its mix loops (docs/81) */
  _core_mix_master = 0xFFA00E50;   /* R246's master section (docs/81) */
  _find_node_body = 0x01A3B584;    /* Screen::find_node after its LINK 0 (lazy stock screens) */
  _txt_draw_body = 0x01A625A0;     /* Text::draw after its LINK 0x24 (guiskip) */
  _dot_body = 0x01A24CC0;          /* Raster::dotted_hline after its LINK 0x18 */
  _fade_body = 0x01A22F40;         /* Raster::fade after P0 = R0; [--SP] = (R7:4, P5:5) */
  _dim_body = 0x01A2357E;          /* Raster::dim_all after P0 = R0; [--SP] = (R7:4, P5:5) */
  _bgs_queue_cont = 0x019411BC;    /* SaveSlotJob queue after its LINK 0 (bgsave.c) */
  _sp_adsr_cont = 0x0191212C;      /* Adsr_render after the two instructions the sampler+ hook replaces */
  .hook_screenload 0x019F07D2 : { KEEP(*(.hook_screenload)) }
  .hook_rfp019427b8 0x019427B8 : { KEEP(*(.hook_rfp019427b8)) }
  .hook_rfp01984f60 0x01984F60 : { KEEP(*(.hook_rfp01984f60)) }
  .hook_rfp019a74d4 0x019A74D4 : { KEEP(*(.hook_rfp019a74d4)) }
  .hook_rfp019b05f8 0x019B05F8 : { KEEP(*(.hook_rfp019b05f8)) }
  .hook_rfp019e6a48 0x019E6A48 : { KEEP(*(.hook_rfp019e6a48)) }
  .hook_rfp019e832c 0x019E832C : { KEEP(*(.hook_rfp019e832c)) }
  .hook_rfp01a1cfd2 0x01A1CFD2 : { KEEP(*(.hook_rfp01a1cfd2)) }
  .hook_rfp01a33fb2 0x01A33FB2 : { KEEP(*(.hook_rfp01a33fb2)) }
  .hook_keyev 0x019C2D3E : { KEEP(*(.hook_keyev)) }
  .hook_midicc 0x01A85C8A : { KEEP(*(.hook_midicc)) }
  .hook_inputbus 0x019674F8 : { KEEP(*(.hook_inputbus)) }
  .hook_midistart 0x019E39D6 : { KEEP(*(.hook_midistart)) }
  .hook_knobev 0x019C2E94 : { KEEP(*(.hook_knobev)) }
  .hook_lforange 0x01A042A2 : { KEEP(*(.hook_lforange)) }
  .hook_euplayer 0x01A0D150 : { KEEP(*(.hook_euplayer)) }
  .hook_seqname1 0x01A42A2E : { KEEP(*(.hook_seqname1)) }
  .hook_seqname2 0x01A42BDC : { KEEP(*(.hook_seqname2)) }
  .hook_seqname3 0x01A42CE0 : { KEEP(*(.hook_seqname3)) }
  .hook_seqtoname 0x01A422EC : { KEEP(*(.hook_seqtoname)) }
  .hook_lfoname1 0x019C9C3A : { KEEP(*(.hook_lfoname1)) }
  .hook_lfoname2 0x019C9BE2 : { KEEP(*(.hook_lfoname2)) }
  .hook_rlp0193de2a 0x0193DE2A : { KEEP(*(.hook_rlp0193de2a)) }
  .hook_rlp01968c34 0x01968C34 : { KEEP(*(.hook_rlp01968c34)) }
  .hook_rlp01981eda 0x01981EDA : { KEEP(*(.hook_rlp01981eda)) }
  .hook_rlp019e4a18 0x019E4A18 : { KEEP(*(.hook_rlp019e4a18)) }
  .hook_rlp01a2d388 0x01A2D388 : { KEEP(*(.hook_rlp01a2d388)) }
  .hook_rlp01a30c3c 0x01A30C3C : { KEEP(*(.hook_rlp01a30c3c)) }
  .hook_rlp01a48fe8 0x01A48FE8 : { KEEP(*(.hook_rlp01a48fe8)) }
  _midi_apply_synth = 0x019CCE68;   /* LfoMidi::apply_synth / apply_fx / apply_drum (quad LFO stubs) */
  _midi_apply_fx = 0x019CD104;     /* FX object (vtable 0x0191086C) */
  _midi_apply_drum = 0x019CD048;   /* drum object (vtable 0x0191084C) */
  .hook_usbdesc 0x01A6ED96 : { KEEP(*(.hook_usbdesc)) }
  .hook_usbiso1 0x019E151C : { KEEP(*(.hook_usbiso1)) }
  .hook_usbiso2 0x019E161C : { KEEP(*(.hook_usbiso2)) }
  .hook_usbiso3 0x019E1812 : { KEEP(*(.hook_usbiso3)) }
  .hook_ciservice 0x019F0A24 : { KEEP(*(.hook_ciservice)) }
  .hook_sqlcache 0x0196F942 : { KEEP(*(.hook_sqlcache)) }
  .hook_taperase 0x0191B9F0 : { KEEP(*(.hook_taperase)) }
  .hook_tapesave 0x01A54842 : { KEEP(*(.hook_tapesave)) }
  .hook_tapeflush 0x019F0BE2 : { KEEP(*(.hook_tapeflush)) }
  .hook_lander1 0x01A3BDB0 : { KEEP(*(.hook_lander1)) }
  .hook_lander2 0x019C35F0 : { KEEP(*(.hook_lander2)) }
  .hook_lander3 0x019C369C : { KEEP(*(.hook_lander3)) }
  .hook_pparse1 0x01995680 : { KEEP(*(.hook_pparse1)) }
  .hook_pparse2 0x0199F210 : { KEEP(*(.hook_pparse2)) }
  .hook_pparse3 0x0199FA4E : { KEEP(*(.hook_pparse3)) }
  .hook_pparse4 0x01A051F8 : { KEEP(*(.hook_pparse4)) }
  .hook_dpkeep 0x01A044A8 : { KEEP(*(.hook_dpkeep)) }
  .hook_spadsr 0x01912128 : { KEEP(*(.hook_spadsr)) }
  .hook_frsnap 0x019C9D04 : { KEEP(*(.hook_frsnap)) }
  .hook_bgsqueue 0x019411B8 : { KEEP(*(.hook_bgsqueue)) }
  .hook_bgsgc 0x019F0C46 : { KEEP(*(.hook_bgsgc)) }
  .hook_crash 0x0191995C : { KEEP(*(.hook_crash)) }
  .hook_smpover 0x01A05984 : { KEEP(*(.hook_smpover)) }
  .hook_diskprobe 0x0199DD7A : { KEEP(*(.hook_diskprobe)) }
  .hook_dpload 0x01A04BC0 : { KEEP(*(.hook_dpload)) }
  .hook_dpbrowse 0x01977900 : { KEEP(*(.hook_dpbrowse)) }
  .hook_usbbuf1 0x01A69730 : { KEEP(*(.hook_usbbuf1)) }
  .hook_lpsyn1 0x01A05766 : { KEEP(*(.hook_lpsyn1)) }
  .hook_lpsyn2 0x01A058D4 : { KEEP(*(.hook_lpsyn2)) }
  .hook_lpdrm1 0x01A050A2 : { KEEP(*(.hook_lpdrm1)) }
  .hook_lpdrm2 0x01A05184 : { KEEP(*(.hook_lpdrm2)) }
  .hook_lpgrow 0x0197008E : { KEEP(*(.hook_lpgrow)) }
  .hook_usbbuf2 0x01A69740 : { KEEP(*(.hook_usbbuf2)) }
  .hook_opzget 0x019E0CFC : { KEEP(*(.hook_opzget)) }
  .hook_recA 0x019AC8B4 : { KEEP(*(.hook_recA)) }
  .hook_recB 0x019ACC7C : { KEEP(*(.hook_recB)) }
  .hook_recend 0x019DEEF2 : { KEEP(*(.hook_recend)) }
  .hook_opzset 0x019E3540 : { KEEP(*(.hook_opzset)) }
  _midi_rx_parse_next = 0x019E348E;   /* end of the SysEx dispatch in midi_rx_parse */
  .hook_devboot 0x019F01B8 : { KEEP(*(.hook_devboot)) }
  .hook_devrx 0x019E3CC8 : { KEEP(*(.hook_devrx)) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
