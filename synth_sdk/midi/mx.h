/* MIDI parameter CCs (docs/60): sound / FX / LFO / mixer targets on the params channel (midi/mx.c) */
#ifndef MX_H
#define MX_H
#include <stdint.h>
#define MX_KEY 0x3F                     /* private key code: "apply pending automation" in the KeyHandler thread */
#define MX_KEY_DOWN 0xA5                /* its state byte: R246 pushes key 0x3F state 1 itself (USB disk eject, see dispatcher.c) */

/* target groups; a target id = group base + knob */
enum { MX_SE, MX_SV, MX_SL, MX_SF, MX_DE, MX_DP, MX_DF, MX_DL, MX_DV, MX_MF, MX_VOL, MX_PAN, MX_MUTE, MX_AUX, MX_TAPE, MX_NG };
#define MX_N 74                         /* targets */

int mx_index(int cc);                   /* CC -> target id, -1: not a params CC */
int mx_cc_of(int id);                   /* target id -> CC */
int mx_group(int id, int *knob);        /* target id -> group, knob */
int mx_scale(const int16_t *rg, int v); /* 0..127 over [min, max] */
int mx_unscale(const int16_t *rg, int w);   /* word -> 0..127 (inverse, rounded) */
int mx_cc(int cc, int v);               /* any thread; 1 if the CC is a params CC (stored, applied in the KeyHandler) */
void mx_apply(void);                    /* KeyHandler thread */
int mx_read(int id);                    /* current value 0..127 of a target, -1: not available now (feedback) */
#endif
