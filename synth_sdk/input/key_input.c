/* R246 key-queue adapter. No ISR or GUI thread accesses the debounce state. */
#include "debounce.h"
#include "../midi/mx.h"

#ifdef OP1_HOST
extern uint32_t key_host_cycles(void);
extern int key_host_queue_empty(void);
extern void key_host_wait(int semaphore, uint32_t timeout);
#define NOW() key_host_cycles()
#define QUEUE_EMPTY() key_host_queue_empty()
#define WAIT(s, t) key_host_wait((s), (t))
#else
static uint32_t key_cycles(void)
{
    uint32_t c;
    __asm__ volatile ("%0 = CYCLES;" : "=d"(c));
    return c;
}
#define NOW() key_cycles()
#define QUEUE_EMPTY() (*(volatile uint32_t *)0x01DD5244 == *(volatile uint32_t *)0x01DD5248)
#define WAIT(s, t) (((int (*)(int, uint32_t))0xFFA0B6E8)((s), (t)))
#endif

extern void key_event_hook(int code, int pressed); /* existing custom + stock dispatch */
static key_debounce_t keys;

void key_input_event(int code, int pressed)
{
    if ((unsigned)code <= 0xFF && (code & KEY_SYNTHETIC)) {
        code &= ~KEY_SYNTHETIC;
        if (code < KEY_DEBOUNCE_KEYS) {
            key_debounce_poll(&keys, NOW(), key_event_hook);
            key_event_hook(code, pressed);
        }
    } else if ((unsigned)code < KEY_DEBOUNCE_KEYS) {
        key_debounce_event(&keys, code, pressed, NOW(), key_event_hook);
    } else if (code == MX_KEY) {
        key_event_hook(code, pressed);             /* MX_KEY: never debounce automation */
    }
}

void key_input_wait(int semaphore, uint32_t timeout)
{
    /* Called at the top of the stock key-thread loop, after the previous queue
     * drain. Process already queued edges before expiring releases. A finite
     * wait ensures the final release is delivered even if no more keys arrive.
     * VDK kNoTimeoutError is bit 31; a plain timeout would invoke the error handler.
     * Once settled, retain the stock indefinite wait (timeout == 0). */
    if (QUEUE_EMPTY()) key_debounce_poll(&keys, NOW(), key_event_hook);
    WAIT(semaphore, keys.pending ? 0x80000001u : timeout);
}
