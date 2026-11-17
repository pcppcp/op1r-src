/* Hang detector (on by default, OP1R_WDOG=0 builds without it; docs/18): the BF524 watchdog raises an NMI when neither the audio interrupt (sm_audio_cb,
 * every block) nor the GUI (every frame) has reloaded it for ~4-5 s, i.e. the whole OP-1 froze (device: a looping
 * buzz, no error screen; the stock error screen can not be drawn from there). The NMI handler (wdog_nmi.S) saves
 * where the core was (RETN ...) and the stack into a record at the top of SDRAM, then resets via the watchdog; the
 * next boot takes the record (sysmon pages "hang"). Off at power-off (wd_shutdown, when the main loop ends); held ~35 s on the crash screen (crash.c, wd_crash: a key restarts sooner).
 * Armed from the GUI frame (dispatcher.c lz_frame, via gs_frame): OP1R_GUISKIP=0 builds never arm it. TE uses the
 * watchdog only to reboot (0x01A88794: CTL 0x0AD0, CNT 1, CTL 0x0FF0), which reprograms it. */
#include <stdint.h>
#ifdef OP1_HOST                                               /* host test (harness/wdog_test.c): registers as variables */
uint16_t wd_host_ctl; uint32_t wd_host_cnt, wd_host_stat, wd_host_evt2, wd_host_rec[64], wd_host_set;
#define WDOG_CTL  wd_host_ctl
#define WDOG_CNT  wd_host_cnt
#define WDOG_STAT wd_host_stat
#define EVT2      wd_host_evt2
#define REC       wd_host_rec
#define NMI_ADDR  1u
static uint32_t wd_host_setter(uint32_t v) { uint32_t o = wd_host_set; wd_host_set = v; return o; }
#define SETTER    wd_host_setter
#else
#define WDOG_CTL  (*(volatile uint16_t *)0xFFC00200)
#define WDOG_CNT  (*(volatile uint32_t *)0xFFC00204)
#define WDOG_STAT (*(volatile uint32_t *)0xFFC00208)
#define EVT2      (*(volatile uint32_t *)0xFFE02008)          /* NMI vector */
#define REC       ((volatile uint32_t *)0x03FFFE00)          /* 64 words: above heap 1 (ends 0x03FFFDB0), below TE's 0x03FFFF02 */
#define NMI_ADDR  ((uint32_t)wd_nmi)
#define SETTER    ((uint32_t (*)(uint32_t))0x01A2FAC4)      /* the stock setter the shutdown hook replaces (stores R0, returns the old value) */
#endif
#define MAGIC     0x48414E47u                                 /* "HANG" */
volatile uint32_t wd_last[48];                                /* the record of the last hang (sysmon), wd_have: one was there */
volatile uint8_t wd_have, wd_armed;
void wd_nmi(void);                                            /* wdog_nmi.S */
void wd_boot(void)                                            /* early boot: a record left by the NMI before its reset */
{
    if (REC[0] != MAGIC) return;
    for (int i = 0; i < 48; i++) wd_last[i] = REC[i];
    REC[0] = 0;
    wd_have = 1;
}
void wd_kick(void) { if (wd_armed) WDOG_STAT = 0; }           /* reload the count (audio block, GUI frame) */
void wd_arm(void)                                             /* GUI thread, once the GUI and the audio run */
{
    if (wd_armed) return;
    EVT2 = NMI_ADDR;
    WDOG_CTL = 0x0AD0;                                        /* disabled, expired flag cleared */
    WDOG_CNT = 0x20000000;                                    /* SCLK ticks: ~4..5 s */
    WDOG_STAT = 0;
    WDOG_CTL = 0x0002;                                        /* enabled, event NMI */
    wd_armed = 1;
}
void wd_off(void) { WDOG_CTL = 0x0AD0; wd_armed = 0; }        /* power-off flush: no NMI while the OP-1 goes down */
uint32_t wd_shutdown(uint32_t v)                               /* CALL @0x019F0B9E, the first call after the main loop ends: off
                                                               * here, not at the flush. The steps before the flush stop the
                                                               * audio (no kicks) and wait unbounded for the background file
                                                               * worker: a long save there tripped the NMI, a reset instead
                                                               * of power-off, the tape not flushed. */
{
    wd_off();
    return SETTER(v);
}
void wd_crash(void)                                           /* crash screen (crash.c): the longest count, ~35 s to photograph
                                                               * it, then the NMI record and the reset as for a hang (a key
                                                               * restarts sooner: crash_wait) */
{
    if (!wd_armed) return;
    WDOG_CTL = 0x0AD0;
    WDOG_CNT = 0xFFFFFFFFu;
    WDOG_STAT = 0;
    WDOG_CTL = 0x0002;
    wd_armed = 0;                                             /* no kicks any more (nothing runs) */
}
