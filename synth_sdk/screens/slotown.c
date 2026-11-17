/* Screen slot owners (slotown.h). Callers run on different threads (key thread, main loop): interrupts off around
 * each install / restore so the slot and the owners' prev links change together. */
#include "slotown.h"
#include <stdint.h>
#ifdef OP1_HOST
static uint32_t irq_off(void) { return 0; }
static void irq_on(uint32_t m) { (void)m; }
#else
static uint32_t irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m) : : "memory"); return m; }
static void irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m) : "memory"); }
#endif
static so_owner *so_all;

void so_install(so_owner *o, void **tab, int id)
{
    const uint32_t m = irq_off();
    if (!o->in) {
        so_owner *p = so_all;
        while (p && p != o) p = p->next;
        if (!p) { o->next = so_all; so_all = o; }
        o->prev = tab[id]; o->id = id; o->in = 1; tab[id] = o->obj;
    }
    irq_on(m);
}
void so_restore(so_owner *o, void **tab)
{
    const uint32_t m = irq_off();
    if (o->in) {
        o->in = 0;
        if (tab[o->id] == o->obj) tab[o->id] = o->prev;
        else for (so_owner *p = so_all; p; p = p->next)        /* covered by another owner: it gets what we covered */
            if (p != o && p->in && p->id == o->id && p->prev == o->obj) { p->prev = o->prev; break; }
    }
    irq_on(m);
}
