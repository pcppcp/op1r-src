/* Count-in before recording + metronome accent (docs/25).
 * REC held + metronome: the arm is latched (tape state 6, as Shift+REC), the stock metronome counts N beats (4/8/16,
 * settings page), then PLAY is pressed from the main loop: recording runs until STOP. STOP or REC + metronome again
 * cancels. Beats come from the stock metronome's own beat clock, so count-in and metronome clicks never double;
 * the first beat of each 4/4 bar is accented (higher click), also for the normal metronome. */
#include "../op1_synth.h"

/* stock metronome state (metronome.cpp, docs/25) */
#ifdef OP1_HOST
extern uint8_t host_metro[0x40];
extern int32_t host_tape_state, host_tape_run, host_tape_speed;
void host_metro_render(void);
int host_tape_service(int arg);
void host_dispatch_key(int code, int pressed);
#define MB              host_metro
#define TAPE_STATE      host_tape_state
#define TAPE_RUN        host_tape_run
#define TAPE_SPEED      host_tape_speed
extern int32_t host_tape_pos;
#define TAPE_POS        host_tape_pos
#else
#define MB              ((volatile uint8_t *)0x01AADABC)
#define TAPE_STATE      (*(volatile int32_t *)0x018C17D4)
#define TAPE_RUN        (*(volatile int32_t *)0xFF802960)      /* 1 / 2: tape (re)started: the render resets the phase */
#define TAPE_SPEED      (*(volatile int32_t *)0xFF802970)      /* phase step while M_TAPE: (speed >> 21) * 0x4000 */
#define TAPE_POS        (*(volatile int32_t *)0xFF802974)      /* tape position, 1/64 samples (docs/22) */
#endif
#define M_ENV     (*(volatile uint16_t *)(MB + 0x00))          /* click envelope, decays per sample */
#define M_START   (*(volatile uint16_t *)(MB + 0x02))          /* envelope start value */
#define M_LEVEL   (*(volatile int16_t  *)(MB + 0x04))          /* metronome level 0..0x7FFF */
#define M_ON      (*(volatile int32_t  *)(MB + 0x08))          /* clicks on beat wraps only if != 0 */
#define M_HALF    (*(volatile int32_t  *)(MB + 0x0C))          /* square half period (samples): pitch */
#define M_TAPE    (*(volatile uint8_t  *)(MB + 0x14))          /* phase follows the tape speed (0x01AADAD0) */
#define M_RESET   (*(volatile uint8_t  *)(MB + 0x16))          /* phase was reset on tape start (0x01AADAD2) */
#define M_LEN     (*(volatile int32_t  *)(MB + 0x1C))          /* beat length in phase units */
#define M_PHASE   (*(volatile int32_t  *)(MB + 0x20))          /* beat phase, counts down, wraps by the length */

#define CI_LEVEL_MIN  0x3000                 /* count-in level when the metronome level is 0 */
#define ACC_BLOCKS    12                     /* accent pitch held this many blocks (~17 ms, the click's decay) */

uint8_t ci_beats = 4;                        /* setting: 4 / 8 / 16 */
static volatile uint8_t ci_on, ci_play_req;  /* counting (audio), PLAY wanted (main loop) */
static volatile int ci_left;
static int32_t bar_beat;                     /* beat in the bar, 0 = accent */
static int32_t acc_left, acc_half;
static int32_t since_wrap = 0x7FFFFFFF;      /* blocks since the last beat */
static int32_t bar_anchor;                   /* tape position (samples) of a beat 1: 0, or where a count-in recorded */

/* accent from the tape position while the beats follow the tape: the stock phase moves with the tape, so a beat
 * is a tape position; beat length in samples = len / 0x4000 (0x4000 phase units per sample at 1x) */
static int tape_accent(void)
{
    int32_t beat = M_LEN / 0x4000;                                /* (once per beat, not per sample) */
    if (beat <= 0) return 0;
    int32_t d = (TAPE_POS >> 6) - bar_anchor + beat / 2;
    int32_t idx = d >= 0 ? d / beat : -((-d + beat - 1) / beat);
    return (idx & 3) == 0;
}           /* accent: blocks left, the stock half period to restore */
static uint8_t saved_on, saved_level_set;
static int16_t saved_level;


static void restore_metro(void)
{
    M_ON = saved_on;
    if (saved_level_set) { M_LEVEL = saved_level; saved_level_set = 0; }
}

/* L1 audio callback: replaces the call to metronome::render (0x019DE548) */
extern void pu_block(void), pu_service(void);              /* tape/punch.c */
void ci_metro_render(void)
{
    pu_block();                                                   /* punch: marker crossings (audio thread) */
    /* will this block wrap (as the render computes it)? then set the accent pitch before the click starts */
    int restart = (TAPE_RUN == 1 || TAPE_RUN == 2) && !M_RESET;  /* the render resets the phase to 0 first */
    int32_t step = M_TAPE ? (TAPE_SPEED >> 21) * 0x4000 : 0x100000;   /* the render's own phase step */
    if (ci_on && step < 0x100000)                                 /* tape-synced phase stands still while stopped: */
        M_PHASE -= 0x100000 - step;                               /* count in at the tempo (the untied step) */
    int32_t before = M_PHASE;
    int will = (restart ? 0 : before) - step < 0;
    if (restart) bar_beat = 0;
    int tape_bar = M_TAPE && !ci_on;                              /* beats follow the tape: bar from its position */
    int acc_now = tape_bar ? (will && tape_accent()) : (will && bar_beat == 0);
    if (acc_now && (M_ON || ci_on) && !acc_left) {
        acc_half = M_HALF; M_HALF = acc_half > 1 ? acc_half / 2 : 1; acc_left = ACC_BLOCKS + 1;
    }
    /* the count's start beat: silent, the tape start's re-sync click (moments later, where recording really
     * starts) is beat 1 instead (hardware: a double click); the accent pitch is held until that click */
    int mute = will && ci_on && ci_left <= 0;
    int32_t on = M_ON;
    if (mute) { M_ON = 0; if (acc_left) acc_left = (M_LEN >> 21) + ACC_BLOCKS; }
    ((void (*)(void))
#ifdef OP1_HOST
        host_metro_render
#else
        0x019DE548
#endif
    )();
    if (mute) M_ON = on;
    int wrapped = restart || M_PHASE > before;                    /* counts down; a wrap jumps up by the length */
    if (acc_left && !--acc_left) M_HALF = acc_half;               /* accent click over: stock pitch again */
    if (since_wrap < 0x7FFFFFFF) since_wrap++;
    if (!wrapped) return;
    /* a beat less than half a beat after the previous one is the stock re-sync on tape start (the phase is reset
     * to 0): the same beat again, so the bar does not advance (hardware: else the bar after the count had 3 beats) */
    int resync = since_wrap < (M_LEN >> 21);                      /* len / 0x100000 blocks per beat, / 2 */
    since_wrap = 0;
    if (resync) {
        if (acc_left > ACC_BLOCKS) acc_left = ACC_BLOCKS;          /* the held start accent: this click, then stock */
        return;
    }
    bar_beat = restart ? 1 : (bar_beat + 1) & 3;                  /* (counted bar: count-in, untied metronome) */
    if (ci_on) {
        if (ci_left <= 0) {                                       /* the beat after the count: start recording */
            ci_on = 0; ci_play_req = 1;
        } else if (--ci_left == 0) restore_metro();               /* last count click: the start beat is stock */
    }
}

/* key thread: REC held + metronome pressed, tape armed by the held REC (state 3) */
int ci_start(void)
{
    if (ci_on || TAPE_STATE != 3) return 0;
    TAPE_STATE = 6;                                               /* latched arm (Shift+REC): REC can be released */
    saved_on = (uint8_t)M_ON;
    if (M_LEVEL == 0) { saved_level = M_LEVEL; saved_level_set = 1; M_LEVEL = CI_LEVEL_MIN; }
    bar_beat = 0;
    since_wrap = 0x7FFFFFFF;
    bar_anchor = TAPE_POS >> 6;                                   /* recording starts here: beat 1 */
    ci_left = ci_beats;
    M_ON = 1;                                                     /* clicks on the stock beat clock */
    M_PHASE = 0;                                                  /* first click on the next block */
    ci_on = 1;
    return 1;
}
int ci_counting(void) { return ci_on; }
void ci_cancel(void)
{
    if (!ci_on && !ci_play_req) return;
    if (ci_on && ci_left > 0) restore_metro();
    ci_on = 0; ci_play_req = 0;
}

/* main loop: replaces CALL tape_c::service(0) at 0x019F0A24; presses PLAY once the count is over */
extern void tl_service(void);
int ci_service(int arg)
{
    int r =
#ifdef OP1_HOST
        host_tape_service(arg);
#else
        ((int (*)(int))0x01A54710)(arg);
#endif
    pu_service();                                                 /* punch in / out (main loop) */
    extern void du_service(void) __attribute__((weak));
    if (du_service) du_service();                                 /* dev update over SysEx (--dev, docs/35) */
    tl_service();                                                 /* per-track tape loops (tape/tapeloop.c) */
    if (ci_play_req) {
        ci_play_req = 0;
        if (TAPE_STATE == 6) {                                    /* still armed (not cancelled by the stock keys) */
#ifdef OP1_HOST
            host_dispatch_key(0x22, 1); host_dispatch_key(0x22, 0);
#else
            ((void (*)(int, int))0x019B6C74)(0x22, 1);            /* KeyHandler::dispatch_key: as a user PLAY */
            ((void (*)(int, int))0x019B6C74)(0x22, 0);
#endif
        }
    }
    return r;
}
