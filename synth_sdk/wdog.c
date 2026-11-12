/* Hang detector (OP1R_WDOG=1, docs/18): the BF524 watchdog raises an NMI when neither the audio interrupt (fr_snap,
 * every block) nor the GUI (every frame) has reloaded it for ~4-5 s, i.e. the whole OP-1 froze (device: a looping
 * buzz, no error screen; the stock error screen can not be drawn from there). The NMI handler (wdog_nmi.S) saves
 * where the core was (RETN ...) and the stack into a record at the top of SDRAM, then resets via the watchdog; the
 * next boot takes the record (sysmon pages "hang"). Off at power-off (tape flush). TE uses the watchdog only to
 * reboot (0x01A88794: CTL 0x0AD0, CNT 1, CTL 0x0FF0), which reprograms it. */
#include <stdint.h>
#define WDOG_CTL  (*(volatile uint16_t *)0xFFC00200)
#define WDOG_CNT  (*(volatile uint32_t *)0xFFC00204)
#define WDOG_STAT (*(volatile uint32_t *)0xFFC00208)
#define EVT2      (*(volatile uint32_t *)0xFFE02008)          /* NMI vector */
#define REC       ((volatile uint32_t *)0x03FFFE00)          /* 64 words: above heap 1 (ends 0x03FFFDB0), below TE's 0x03FFFF02 */
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
    EVT2 = (uint32_t)wd_nmi;
    WDOG_CTL = 0x0AD0;                                        /* disabled, expired flag cleared */
    WDOG_CNT = 0x20000000;                                    /* SCLK ticks: ~4..5 s */
    WDOG_STAT = 0;
    WDOG_CTL = 0x0002;                                        /* enabled, event NMI */
    wd_armed = 1;
}
void wd_off(void) { WDOG_CTL = 0x0AD0; wd_armed = 0; }        /* power-off flush: no NMI while the OP-1 goes down */
