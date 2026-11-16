/* Parameter-lock recording (seq/plk.c, docs/87). */
#ifndef PLK_H
#define PLK_H
#include <stdint.h>

#define PLK_LANES 14                      /* engine words 0..3, 5..7 (lanes 0..6), FX words 0..3, 5..7 (lanes 7..13) */
#define PLK_EV    256                     /* events per lane */
#define PLK_NLEN  7                       /* lengths: 1/4, 1/2, 1, 2, 4, 8, 16 bars */
enum { PLK_IDLE, PLK_ARMED, PLK_REC };

typedef struct { uint16_t t; int16_t v; } plk_ev_t;               /* tick in the clip, word value */
typedef struct { int32_t type; int16_t start; uint16_t n; plk_ev_t ev[PLK_EV]; } plk_lane_t;
typedef struct { uint8_t len_i, sync, trk; plk_lane_t lane[PLK_LANES]; } plk_clip_t;
/* trk: 0 free; k (1..16): restarts whenever track k loops; PLK_START + k: restarts only when track k starts (the clip
 * then runs its whole length, e.g. 4 bars over a 1-bar pattern, in phase with it) */
#define PLK_START 32
#define PLK_FIRST 0x100                     /* plk_track_restart: the track's first step since it was started */
typedef struct {
    volatile uint8_t state, running, halted;
    int32_t pos;                                                  /* tick in the clip */
    uint16_t seen, based;                                         /* lanes changed in this pass; lanes with a base kept */
    int16_t last[PLK_LANES], base[PLK_LANES];
    uint16_t cur[PLK_LANES];                                      /* playback cursors */
} plk_run_t;

extern plk_clip_t plk_clip[2];
extern plk_run_t plk_run[2];
extern const uint16_t plk_len_ticks[PLK_NLEN];
int plk_lane_group(int lane);
int plk_lane_word(int lane);
uint16_t plk_lanes(int mode);
void plk_start(int mode), plk_stop(int mode), plk_tick(int mode), plk_rec(int mode);
void plk_cut(int mode, int lane);
void plk_halt(int on);
void plk_changed(void);
extern volatile uint8_t plk_tracks[2];      /* tracks of the current sequencer plugin (plk_set_tracks), 0: none */
void plk_set_tracks(int mode, int n);        /* exported to sequencer plugins */
void plk_track_restart(int mode, int track); /* exported: track (0..) is back at its first step (sequencer thread) */
int plk_serialize(int id, char *out);
int plk_parse(int id, const char *p, int size);
void plk_set_len(int mode, int len_i);
/* page (firmware) */
extern volatile uint8_t plk_view;
int plk_page_key(int mode, int code, int pressed, int knob, int shift);
void plk_poll(void);                         /* GUI thread: a finished recording / CUT -> saved (DB table plk) */
void plk_page_turn(int mode, int knob, int delta);
void *plk_player(void *inner);               /* the clock proxy around a sequencer plugin's player */
#endif
