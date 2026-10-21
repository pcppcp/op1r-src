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
  .l1tramp 0xFFA0BFB4 : { KEEP(*(.l1tramp)) }    /* free end of L1 instruction SRAM (image ends 0xFFA0BFB4) */
  .hook_namecmp 0x019B975C : { KEEP(*(.hook_namecmp)) }
  .hook_samplecheck 0x01A01844 : { KEEP(*(.hook_samplecheck)) }
  .hook_fxnamecmp 0x019AAD8E : { KEEP(*(.hook_fxnamecmp)) }
  .hook_iterread 0x019B9EF8 : { KEEP(*(.hook_iterread)) }
  .hook_screenctor 0x01A3BD58 : { KEEP(*(.hook_screenctor)) }
  .hook_plugboot 0x019F02EE : { KEEP(*(.hook_plugboot)) }
  .hook_masterload 0x019F054A : { KEEP(*(.hook_masterload)) }
  .hook_apsynth1 0x019C2E5E : { KEEP(*(.hook_apsynth1)) }
  .hook_apsynth2 0x01A01866 : { KEEP(*(.hook_apsynth2)) }
  .hook_apsynth3 0x01A50F1C : { KEEP(*(.hook_apsynth3)) }
  .hook_vcdown 0x01A85750 : { KEEP(*(.hook_vcdown)) }
  .hook_vcup 0x01A85840 : { KEEP(*(.hook_vcup)) }
  _vc_down_body = 0x01A85754;      /* VoiceCtl key_down after its LINK 0x10 (sequencer hook) */
  _vc_up_body = 0x01A85844;        /* VoiceCtl key_up after its LINK 0x20 */
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
  .hook_keywait 0x019C2CF4 : { KEEP(*(.hook_keywait)) }
  .hook_keyev 0x019C2D3E : { KEEP(*(.hook_keyev)) }
  .hook_midicc 0x01A85C8A : { KEEP(*(.hook_midicc)) }
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
  .hook_lander1 0x01A3BDB0 : { KEEP(*(.hook_lander1)) }
  .hook_lander2 0x019C35F0 : { KEEP(*(.hook_lander2)) }
  .hook_lander3 0x019C369C : { KEEP(*(.hook_lander3)) }
  .hook_pparse1 0x01995680 : { KEEP(*(.hook_pparse1)) }
  .hook_pparse2 0x0199F210 : { KEEP(*(.hook_pparse2)) }
  .hook_pparse3 0x0199FA4E : { KEEP(*(.hook_pparse3)) }
  .hook_pparse4 0x01A051F8 : { KEEP(*(.hook_pparse4)) }
  .hook_dpkeep 0x01A044A8 : { KEEP(*(.hook_dpkeep)) }
  .hook_spadsr 0x01912128 : { KEEP(*(.hook_spadsr)) }
  .hook_dpload 0x01A04BC0 : { KEEP(*(.hook_dpload)) }
  .hook_dpbrowse 0x01977900 : { KEEP(*(.hook_dpbrowse)) }
  .hook_usbbuf1 0x01A69730 : { KEEP(*(.hook_usbbuf1)) }
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
