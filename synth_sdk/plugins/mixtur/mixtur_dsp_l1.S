/* OP1R_L1SLOT test build (docs/56): mixtur's two passes a second time, in section .l1pulse (linked at Pulse's L1
 * render slot 0xFFA04B88; boottime.c writes them there after moving Pulse to SDRAM). */
#define MX_L1SLOT
#include "mixtur_dsp.S"
