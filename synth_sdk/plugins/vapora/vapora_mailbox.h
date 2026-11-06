#ifndef VAPORA_MAILBOX_H
#define VAPORA_MAILBOX_H
#include "vapora_dsp.h"
typedef struct {
    volatile uint32_t sequence;
    volatile int32_t slot, selector;
    volatile ps_snapshot snapshot;
} ps_mailbox;
extern ps_mailbox ps_mail[PS_VOICES];
/* GUI-only hold indication, keyed by note serial so voice reuse cannot sustain
 * an earlier impact. Written by audio callbacks, read atomically by the GUI. */
extern volatile uint32_t ps_held_serial[PS_VOICES];
/* Single-core Blackfin: aligned volatile accesses + compiler ordering.
 * Audio can preempt GUI, GUI never preempts audio publication. No retries,
 * locks, waiting or GUI writes to the mailbox. Not a portable SMP protocol. */
#define PS_BARRIER() __asm__ volatile ("" ::: "memory")
static inline void ps_copy_snapshot(ps_snapshot *dst, const volatile ps_snapshot *src)
{
    dst->serial=src->serial; dst->pitch=src->pitch; dst->level=src->level;
    for(int i=0;i<PS_TRACE;i++)dst->trace[i]=src->trace[i];
    for(int i=0;i<4;i++)dst->primary[i]=src->primary[i];
}
static inline int ps_read_mail(int i,int slot,int selector,ps_snapshot *s,uint32_t *seq)
{
    const ps_mailbox *m=&ps_mail[i];
    uint32_t a=m->sequence;
    if((a&1)||a==*seq)return 0;
    PS_BARRIER();
    int ms=m->slot, me=m->selector;
    ps_copy_snapshot(s,&m->snapshot);
    PS_BARRIER();
    if(a!=m->sequence||ms!=slot||me!=selector)return 0;
    *seq=a;
    return 1;
}
#endif
