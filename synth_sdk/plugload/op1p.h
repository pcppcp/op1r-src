/*
 * op1r plugin file format (.op1p), docs/37. Shared by the builder (make_plugin.py, same layout in Python) and the
 * loader. All words little-endian.
 *
 *   [SVG text, NUL, zero padding to 4]   optional: the plugin's screen; the stock SVG parser stops at the NUL
 *   op1p_hdr                              at bin_off (0 without SVG)
 *   image[image_size]                     code + rodata + data, linked at address 0
 *   op1p_reloc[n_relocs]
 *   import names                          n_imports NUL-terminated names (core exports, e.g. "sq_line")
 *   metadata (optional)                   UTF-8 "key=value\n" lines up to the end of the binary part, e.g.
 *                                         "author=pcppcp\n" (shown by the web patcher; ignored by the loader)
 *   op1p_trailer                          last 16 bytes of the file
 *
 * Relocation: the 32-bit value v = (target & OP1P_IMPORT ? export_addr[target & 0xFFFF] : base + target) + addend;
 *   ABS32: word at off = v;  HI16 / LO16: halfword at off (the immediate field itself, as R_BFIN_HUIMM16 /
 *   LUIMM16 r_offset) = v >> 16 / v & 0xFFFF.
 */
#ifndef OP1P_H
#define OP1P_H
#include <stdint.h>

#define OP1P_MAGIC    0x5031504Fu   /* "OP1P" */
#define OP1P_TMAGIC   0x5431504Fu   /* "OP1T" */
#define OP1P_FORMAT   1
#define OP1P_SYNTH    0
#define OP1P_FX       1
#define OP1P_SEQ      2              /* op1_seq_t (docs/44) */
#define OP1P_DRUM     3              /* op1_drum_t (docs/43) */
#define OP1P_SAMPLER  4              /* op1_sampler_t (docs/46) */
#define OP1P_TOOL     5              /* op1_tool_t */
#define OP1P_NO_SCREEN 0xFFFFFFFFu
#define OP1P_F_USES_SAMPLE 1u
#define OP1P_F_STEREO      2u      /* fx: op1_fx_t has render_stereo (plugin.json "stereo") */
#define OP1P_F_INPUT       4u      /* synth: the entry is an op1_input_engine_t (plugin.json "input_engine", op1_input.h) */
#define OP1P_COARSE(flags) (((flags) >> 16) & 15)   /* synth: knobs turned in 8 steps (plugin.json "coarse": [k..]) */
#define OP1P_PREF(flags) (((flags) >> 8) & 0xFF)   /* preferred number (plugin.json order 1..31: keeps old sounds), 0 none */

#define OP1P_ABS32 1
#define OP1P_HI16  2
#define OP1P_LO16  3
#define OP1P_IMPORT 0x80000000u

typedef struct {
    uint32_t magic, format, api, kind;
    char     name[16];                  /* type name, NUL-terminated, <= 15 chars */
    uint32_t image_size, bss_size;
    uint32_t entry_off;                 /* op1_engine_t / op1_fx_t in the image */
    uint32_t screen_off;                /* op1_screen_t, or OP1P_NO_SCREEN */
    int16_t  defaults[4], page2_defaults[4];
    uint32_t flags;
    uint32_t n_relocs, n_imports;
} op1p_hdr;                             /* 76 bytes */

typedef struct { uint32_t off, type, target; int32_t addend; } op1p_reloc;

typedef struct { uint32_t magic, bin_off, bin_len, crc32; } op1p_trailer;   /* crc32 over [bin_off, bin_off+bin_len) */

#endif
