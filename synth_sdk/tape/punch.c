/* Punch in / out at tape markers (docs/26). Tape held + REC toggles punch mode. While on, the next marker crossed in
 * the direction of travel (forward or reverse, any speed, across the loop wrap) punches in, the one after punches out
 * and punch mode turns off. PLAY / STOP turn it off. The punches are the stock manual ones (REC held while playing:
 * tape state 10 -> 5, release 5 -> 10), made from the main loop. */
#include "../op1_synth.h"

#define PU_OFF  0
#define PU_ARM  1                            /* waiting for the "in" marker */
#define PU_REC  2                            /* punched in, waiting for the "out" marker */
#define PU_JUMP (64 * 16)                    /* a position step bigger than this (16x speed) is a jump, not travel */

#ifdef OP1_HOST
extern int32_t host_tape_state, host_tape_pos, host_loop_in, host_loop_out, host_loop_on;
extern int host_clips_left;
void host_set_mode(void);
#define TAPE_STATE  host_tape_state
#define TAPE_POS    host_tape_pos
#define LOOP_IN     host_loop_in
#define LOOP_OUT    host_loop_out
#define LOOP_ON     host_loop_on
#define CLIPS_OK()  host_clips_left
#define SET_MODE()  host_set_mode()
static uint32_t irq_off(void) { return 0; }
static void irq_on(uint32_t m) { (void)m; }
#else
#define TAPE_STATE  (*(volatile int32_t *)0x018C17D4)
#define TAPE_POS    (*(volatile int32_t *)0xFF802974)     /* 1/64 samples */
#define LOOP_IN     (*(volatile int32_t *)0x01B4EF34)     /* samples (tape_c 0x01A53DA0) */
#define LOOP_OUT    (*(volatile int32_t *)0x01B4EF38)     /* samples (tape_c 0x01A53D88) */
#define LOOP_ON     (*(volatile uint8_t *)0xFF8029C5)
/* the stock REC-press checks (on_key 0x21): clips left, clipboard idle (also dispatcher.c: Shift + Drop) */
int pu_clips_ok(void)
{
    if (*(int32_t *)((uint8_t *)((void *(*)(void))0x0195BBB4)() + 0x1A00) < 1) return 0;
    return ((int (*)(void))0x0191C750)() != 0 && ((int (*)(void))0x0191554C)() == 0;
}
#define CLIPS_OK()  pu_clips_ok()
#define SET_MODE()  (((void (*)(void))0x01A581F4)())      /* TapeController::set_mode */
static uint32_t irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m) : : "memory"); return m; }
static void irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m) : "memory"); }
#endif

extern int32_t tm_pos[];                     /* tape/markers.c: sorted marker positions (samples) */
extern int tm_n;

volatile uint8_t pu_mode;                    /* PU_OFF / PU_ARM / PU_REC (also read by the marker drawing) */
static volatile uint8_t pu_req;              /* 1 punch in, 2 punch out (main loop) */
static int32_t pu_prev = -1;
static uint8_t pu_hold;                      /* punch mode off by PLAY, the recording continues */                 /* tape position (samples) at the last block */

/* markers in the travelled stretch from a (exclusive) to b (inclusive), forward if a < b */
static int pu_crossed(int32_t a, int32_t b)
{
    for (int i = 0; i < tm_n; i++) {
        int32_t m = tm_pos[i];
        if (a < b ? (m > a && m <= b) : (m < a && m >= b)) return 1;
    }
    return 0;
}

static void pu_cross(void)
{
    if (pu_mode == PU_ARM) { pu_mode = PU_REC; pu_req = 1; }
    else if (pu_mode == PU_REC) { pu_mode = PU_OFF; pu_req = 2; }
}

/* audio thread, once per block (from the metronome wrapper, tape/countin.c) */
void pu_block(void)
{
    if (pu_mode == PU_OFF) return;                              /* (pu_toggle sets pu_prev when arming) */
    int32_t pos = TAPE_POS >> 6, prev = pu_prev;
    pu_prev = pos;
    if (prev < 0 || pos == prev) return;
    int32_t d = pos - prev;
    if (d <= PU_JUMP && d >= -PU_JUMP) { if (pu_crossed(prev, pos)) pu_cross(); return; }
    /* a jump: the loop wrap (travelled to one loop end, continued from the other), else a locate (no crossing) */
    if (!LOOP_ON || LOOP_OUT <= LOOP_IN) return;
    int fwd = d < 0;                                          /* forward travel wraps back down to the loop in */
    int32_t a_end = fwd ? LOOP_OUT : LOOP_IN, b_start = fwd ? LOOP_IN : LOOP_OUT;
    if (pu_crossed(prev, a_end)) { pu_cross(); if (pu_mode == PU_OFF) return; }
    if (pu_crossed(fwd ? b_start - 1 : b_start + 1, pos)) pu_cross();
}

/* main loop (from ci_service): the stock punch state changes. The request is taken and acted on with the interrupts
 * off: a key (pu_toggle / pu_key) or a crossing in between would be undone or lost. */
void pu_service(void)
{
    if (!pu_req) return;
    int ok = pu_req == 1 && CLIPS_OK();                       /* (stock calls: before the interrupts go off) */
    uint32_t m = irq_off();
    uint8_t r = pu_req, mode = 0;
    pu_req = 0;
    if (r == 1 && pu_mode == PU_REC) {
        if (TAPE_STATE == 10 && ok) { TAPE_STATE = 5; mode = 1; }                     /* as REC held while playing */
        else pu_mode = PU_OFF;                                                          /* not playing / no clips */
    }
    if (r == 2 && TAPE_STATE == 5) { TAPE_STATE = 10; mode = 1; }                      /* as REC released */
    irq_on(m);
    if (mode) SET_MODE();
}

/* key thread */
void pu_toggle(void)
{
    pu_hold = 0;
    if (pu_mode == PU_REC && TAPE_STATE == 5) { TAPE_STATE = 10; SET_MODE(); }         /* stop a running punch */
    pu_mode = pu_mode == PU_OFF ? PU_ARM : PU_OFF;
    pu_req = 0;
    pu_prev = TAPE_POS >> 6;                                  /* the current stretch never counts */
}
static void pu_out(void) { if (TAPE_STATE == 5) { TAPE_STATE = 10; SET_MODE(); } }

/* key thread, before the stock key; returns 1 if the key is consumed (press and release).
 * PLAY while punched in: 1st press punch mode off (still recording), 2nd press punch out (still playing).
 * STOP while recording by a punch: punch out first (state 5 = "REC held": the stock STOP skips it), then the stock
 * STOP. STOP while armed and stopped: punch mode off. */
int pu_key(int code)
{
    if (code == 0x22) {
        if (pu_mode == PU_REC) { pu_mode = PU_OFF; pu_req = 0; pu_hold = 1; return 1; }
        if (pu_hold) { pu_hold = 0; pu_out(); return 1; }
        return 0;
    }
    if (code == 0x23) {
        if (pu_mode == PU_REC || pu_hold) { pu_out(); pu_mode = PU_OFF; pu_req = 0; pu_hold = 0; }
        else if (pu_mode == PU_ARM && TAPE_STATE != 10) { pu_mode = PU_OFF; pu_req = 0; }
    }
    return 0;
}
