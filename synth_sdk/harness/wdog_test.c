/* Host test for wdog.c: the power-off hook (wd_shutdown, CALL @0x019F0B9E) switches the hang detector off and still
 * does the stock call; later kicks, the flush's own wd_off and a crash screen leave it off. */
#include <assert.h>
#include <stdio.h>
#include "../wdog.c"

void wd_nmi(void) {}

int main(void)
{
    wd_arm();
    assert(wd_armed && wd_host_ctl == 0x0002 && wd_host_cnt == 0x20000000);
    wd_host_stat = 7; wd_kick(); assert(wd_host_stat == 0);           /* armed: a kick reloads */
    wd_host_set = 0x11;
    assert(wd_shutdown(0x0193E85E) == 0x11 && wd_host_set == 0x0193E85E);   /* the stock setter still runs */
    assert(!wd_armed && wd_host_ctl == 0x0AD0);                        /* off before the audio stops */
    wd_host_stat = 7; wd_kick(); assert(wd_host_stat == 7);           /* no kicks any more, none needed */
    wd_off(); assert(!wd_armed && wd_host_ctl == 0x0AD0);              /* tape_service(1) turns it off again: harmless */
    wd_host_cnt = 0; wd_crash(); assert(wd_host_ctl == 0x0AD0 && wd_host_cnt == 0);   /* a crash then: not re-armed */
    assert(wd_shutdown(5) == 0x0193E85E && wd_host_ctl == 0x0AD0);     /* off already (no detector armed yet): same */
    wd_host_rec[0] = 0x48414E47u; wd_host_rec[1] = 0x01A2FAC4; wd_boot();   /* the record of a hang: taken once */
    assert(wd_have && wd_last[1] == 0x01A2FAC4 && wd_host_rec[0] == 0);
    printf("ok\n");
    return 0;
}
