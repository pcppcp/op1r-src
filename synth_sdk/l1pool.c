/* L1 pool (docs/67). The idle Lander L1 range (0xFF900820..0xFF906908, unused in op1r: the Lander is never built):
 *   0xFF900820  R246's whole sine table (8192 x int16, 16 KB): op1_sin() (op1_plugin.h) reads it in one cycle
 *               (device, fmbell 6 voices: 12.8 % -> 5.6 % of the time). Its first 2,049 entries are the quarter
 *               kennel's op1_l1_sine_quarter() returns.
 *   0xFF904820  32 B of zeros: the stock readers reach entries 8192 / 8193 (punch), which are 0 after the SDRAM table
 *   0xFF904840  OP1_L1_SYNTH  8,192 B   a lease for the synth engine (kennel's API, op1_l1.h)
 *   0xFF907E00  OP1_L1_DRUM     512 B   L1 data B's top, which the LDR never loads
 *   OP1_L1_FX / OP1_L1_MASTER: no room left (their claims return 0: the plugin works on its SDRAM copy).
 * The lease logic is Hundekraft kennel 3.1.4's kn_l1.c (GPL-3.0-or-later with the OP-1 firmware permission, NOTICE),
 * without its Lander check and page-7 switch: a plugin claims its partition every block it renders and keeps it while
 * it does; another owner gets 0 until the holder has not claimed for L1_IDLE blocks. Takeovers and state copies only
 * inside the audio callback (l1_begin .. l1_end, sm_audio_cb); a synth note_on on the key / sequencer thread is
 * bracketed with l1_thread_enter / _leave (dispatcher.c), and no partition moves while one runs. */
#include "op1_synth.h"

#define L1_SINE     0xFF900820u
#define FW_SINE     ((const int16_t *)0x0101E5E8)
#define L1_IDLE     2u
#ifndef OP1_HOST
const int16_t *op1_sine_q = FW_SINE;                   /* the firmware table until l1_begin has copied it */
#endif

#ifdef OP1_HOST                                         /* host test (harness/l1pool_test.c): plain arrays */
static uint32_t host_synth[2048], host_drum[128];
static uint32_t *const L1_AT[OP1_L1_PARTS] = { host_synth, 0, 0, host_drum };
#else
static uint32_t *const L1_AT[OP1_L1_PARTS] = { (uint32_t *)0xFF904840u, 0, 0, (uint32_t *)0xFF907E00u };
#endif
static const uint32_t L1_CAP[OP1_L1_PARTS] = { 8192, 0, 0, 512 };
_Static_assert(0xFF904840u + 8192 <= 0xFF906908u, "SYNTH partition inside the Lander range");

static uint32_t l1_cb, l1_tick;
static volatile uint32_t l1_busy;                      /* plugin calls running on another thread right now */
static const void *l1_holder[OP1_L1_PARTS];
static uint32_t l1_last[OP1_L1_PARTS], *l1_home[OP1_L1_PARTS], l1_words[OP1_L1_PARTS];

void l1_sine_init(void)                                /* boot (boottime.c bt_db_open), before anything reads the
                                                         * table: the stock code reads the L1 copy (make_firmware.py
                                                         * IMM_PATCHES l1_sine, docs/84) */
{
#ifndef OP1_HOST
    if (op1_sine_q != FW_SINE) return;
    volatile uint32_t *d = (volatile uint32_t *)L1_SINE;
    const uint32_t *s = (const uint32_t *)FW_SINE;
    for (int i = 0; i < 4096; i++) d[i] = s[i];
    for (int i = 4096; i < 4104; i++) d[i] = 0;          /* as after the SDRAM table (entries 8192..) */
    op1_sine_q = (const int16_t *)L1_SINE;
#endif
}
void l1_begin(void)                                    /* audio callback start (sm_audio_cb) */
{
    l1_cb = 1;
    l1_sine_init();                                    /* (done at boot already) */
}
void l1_end(void) { l1_cb = 0; l1_tick++; }            /* audio callback end */

#ifdef __bfin__
static uint32_t irq_off(void) { uint32_t m; __asm__ volatile("cli %0;" : "=d"(m)); return m; }
static void irq_on(uint32_t m) { __asm__ volatile("sti %0;" : : "d"(m) : "memory"); }
#else
static uint32_t irq_off(void) { return 0; }
static void irq_on(uint32_t m) { (void)m; }
#endif
uint32_t l1_thread_enter(void)
{
    if (l1_cb) return 0;                               /* inside the callback: nothing preempts it */
    uint32_t m = irq_off(); l1_busy++; irq_on(m);
    return 1;
}
void l1_thread_leave(uint32_t t) { if (t) { uint32_t m = irq_off(); l1_busy--; irq_on(m); } }

static void put_back(int p)                            /* the holder's state home (the only copy back) */
{
    for (uint32_t i = 0; i < l1_words[p]; i++) l1_home[p][i] = L1_AT[p][i];
    l1_home[p] = 0; l1_words[p] = 0;
}
static void bring_in(int p, uint32_t *home, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) L1_AT[p][i] = home[i];
    l1_home[p] = n ? home : 0; l1_words[p] = n;
}
static void *take(int part, const void *owner, uint32_t *home, uint32_t state, uint32_t bytes, int *fresh)
{
    if (fresh) *fresh = 0;
    if (part < 0 || part >= OP1_L1_PARTS || bytes > L1_CAP[part] || !owner) return 0;
    if (state > bytes || (state & 3u) || ((uintptr_t)home & 3u)) return 0;
    if (l1_holder[part] != owner) {
        if (!l1_cb || l1_busy) return 0;               /* no takeover outside the callback or under a thread's call */
        if (l1_holder[part] && l1_tick - l1_last[part] < L1_IDLE) return 0;   /* someone else's, still in use */
        if (l1_words[part]) put_back(part);
        l1_holder[part] = owner;
        bring_in(part, home, state >> 2);
        if (fresh) *fresh = 1;
    } else if (home && l1_words[part] != state >> 2) { /* the holder's state changed size: home and in again */
        if (!l1_cb || l1_busy) return 0;
        if (l1_words[part]) put_back(part);
        bring_in(part, home, state >> 2);
        if (fresh) *fresh = 1;
    }
    l1_last[part] = l1_tick;
    return L1_AT[part];
}
void *op1_l1_claim(int part, const void *owner, uint32_t bytes, int *fresh) { return take(part, owner, 0, 0, bytes, fresh); }
void *op1_l1_claim_state(int part, void *home, uint32_t state_bytes, uint32_t bytes, int *fresh)
{
    if (fresh) *fresh = 0;
    if (!home || !state_bytes) return 0;
    return take(part, home, home, state_bytes, bytes, fresh);
}
#ifndef OP1_HOST
const int16_t *op1_l1_sine_quarter(void) { return op1_sine_q == FW_SINE ? 0 : op1_sine_q; }
#endif
