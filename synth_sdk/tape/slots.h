/* slot machine (docs/85): shared by tape/tm.c (audio) and screens/tfscreen.c (keys, page) */
#ifndef TM_SLOTS_H
#define TM_SLOTS_H
#include <stdint.h>
#define SL_N    12
#define SL_MAX  (120 * 44100)
typedef struct {
    int32_t len, pos;                                            /* recorded frames (0 empty); play position */
    uint8_t ch, play, gate, lvl;                                 /* channel 0..3; sounding; held (play key); level 0..100 */
    uint8_t a, d, s, r;                                          /* envelope 0..100 (a / d / r: ms = v * v / 2; s: %) */
    uint8_t stage, rev, mode, qlen;                              /* envelope stage 0 a, 1 d, 2 s, 3 r; reverse; 0 loop, 1 once,
                                                                  * 2 ping-pong; record length (sl_qlen) */
    int32_t g;                                                   /* envelope gain Q15 */
    int8_t semi, pdir; uint16_t frac;                            /* pitch -24..24; play direction; Q16 */
    uint8_t pmode, pend, sync, pstop;                            /* pitch 0 tape (speed with it), 1 stretch (length kept);
                                                                  * pend: a start asked for (1 now, 2 at the bar); pstop: a stop at
                                                                  * the bar; sync: recording at the bar, the speed follows the tempo */
    uint32_t gph;                                                /* stretch: grain phase, Q16 frames in [0, SL_G) */
    int32_t bpm;                                                 /* the tempo it was recorded at (Q8; 0 unknown: 1x) */
    int8_t pan; uint8_t pad2[3];                                 /* -50 .. 50 */
} tm_slm;
extern volatile tm_slm tm_sl[SL_N];
extern volatile int8_t tm_sl_rec;
extern volatile int32_t tm_sl_recpos, tm_sl_queue;
extern volatile uint8_t tm_sl_full;
extern volatile int32_t tm_sl_rectgt;
extern volatile uint8_t tm_sl_recwait, tm_sl_od;                 /* + the recording overdubs the playing slot */                           /* a synced recording waits for its bar */
extern volatile int32_t tm_sl_bar, tm_sl_bpm;
extern volatile uint32_t tm_sl_tr[SL_N];
extern volatile uint8_t tm_sl_lfoon[SL_N];                          /* synced: speed now / recorded tempo, Q16 (the GUI: no
                                                                  * 64-bit division in the audio thread; 0 = 1x) */                    /* + R246's tempo now (Q8) */                               /* frames per bar (the GUI, from R246's tempo) */
#define SL_G    2048
/* per slot four LFOs (T4): rate 0..63 free (the quad LFO's 0.05 .. 20 Hz) or a division index (SL_DIVS) when synced;
 * wave 0 sine, 1 triangle, 2 ramp up, 3 ramp down, 4 square, 5 random (sample & hold), 6 / 7 Lorenz x / z (the quad's); type 0 run (free), 1 restart at the
 * slot's start, 2 one cycle from the start; target 0 level, 1 pan, 2 pitch (+-12 st), 3 speed (+-12 st); amount -31..31 */
typedef struct { uint8_t rate, sync, wave, type, tgt; int8_t amt; uint8_t done, pad; uint32_t ph, cnt; int16_t val, pad2; int32_t lx, ly, lz; } tm_sllfo;
#define SL_DIVS 11
#define SL_PKS  11                                               /* waveform: one peak (|x| >> 7) per 2^11 frames */
#define SL_PKN  ((SL_MAX >> SL_PKS) + 1)
extern volatile uint8_t tm_sl_peak[SL_N][SL_PKN];                /* filled while recording and playing (not saved) */
extern volatile tm_sllfo tm_sl_lfo[SL_N][4];
extern volatile uint32_t tm_sl_rinc[64], tm_sl_dinc[SL_DIVS];     /* phase per block: free rates, divisions (the GUI) */                                             /* stretch grain (frames, a power of 2) */                            /* the recording stops at (frames, <= SL_MAX) */
static inline int32_t sl_env_ms(int v) { return v * v / 2; }
#endif
