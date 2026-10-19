/* cc -std=c99 -Wall -Wextra -Werror -DOP1_HOST harness/debounce_test.c
 *    input/debounce.c input/key_input.c -o harness/build/debounce_test */
#include "../input/debounce.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MS(n) ((uint32_t)(n) * 400000u)
static struct { int code, down; } events[256];
static unsigned count;
static uint32_t clock_now, waited;
static int queue_empty = 1, waited_sem;

void key_event_hook(int code, int pressed)
{
    assert(count < sizeof events / sizeof events[0]);
    events[count].code = code;
    events[count++].down = pressed;
}
uint32_t key_host_cycles(void) { return clock_now; }
int key_host_queue_empty(void) { return queue_empty; }
void key_host_wait(int semaphore, uint32_t timeout) { waited_sem = semaphore; waited = timeout; }

static void expect(unsigned i, int code, int down)
{
    assert(i < count && events[i].code == code && events[i].down == down);
}
static void edge(key_debounce_t *d, int code, int down, uint32_t now)
{
    key_debounce_event(d, code, down, now, key_event_hook);
}
static void poll(key_debounce_t *d, uint32_t now) { key_debounce_poll(d, now, key_event_hook); }

static void bounce(void)
{
    key_debounce_t d = {0}; count = 0;
    edge(&d, 39, 0, 0);                         /* stray release at startup */
    assert(count == 0);
    edge(&d, 39, 1, 0);
    assert(count == 1); expect(0, 39, 1);        /* immediate first attack, even at time zero */
    edge(&d, 39, 1, MS(1));                     /* repeated down */
    edge(&d, 39, 0, MS(2));
    edge(&d, 39, 1, MS(3));
    edge(&d, 39, 0, MS(4));
    edge(&d, 39, 1, MS(5));
    poll(&d, MS(30));
    assert(count == 1 && d.pending == 0);        /* press chatter never retriggers */
    edge(&d, 39, 0, MS(100));
    edge(&d, 39, 1, MS(102));
    edge(&d, 39, 0, MS(104));
    edge(&d, 39, 0, MS(108));                    /* duplicate up must not move the deadline */
    poll(&d, MS(109) - 1);
    assert(count == 1);
    poll(&d, MS(109));
    assert(count == 2 && d.pending == 0); expect(1, 39, 0);
    poll(&d, MS(200));
    assert(count == 2);
}

static void short_and_repeated_notes(void)
{
    key_debounce_t d = {0}; count = 0;
    edge(&d, 13, 1, 0);
    edge(&d, 13, 0, MS(1));                     /* very short real tap still gets an off */
    poll(&d, MS(6));
    assert(count == 2); expect(1, 13, 0);
    edge(&d, 13, 1, MS(10));
    edge(&d, 13, 0, MS(11));
    edge(&d, 13, 1, MS(16));                    /* second stroke at the threshold, without a poll */
    assert(count == 5); expect(3, 13, 0); expect(4, 13, 1);
    edge(&d, 13, 0, MS(17));
    poll(&d, MS(22));
    assert(count == 6 && d.pending == 0);
}

static void independent_keys(void)
{
    key_debounce_t d = {0}; count = 0;
    edge(&d, 38, 1, 0);                        /* Shift */
    edge(&d, 13, 1, MS(1));
    edge(&d, 38, 0, MS(2));
    edge(&d, 13, 0, MS(3));
    edge(&d, 13, 1, MS(4));                    /* one key's bounce doesn't reset Shift */
    edge(&d, 14, 1, MS(7));
    assert(count == 4); expect(2, 38, 0); expect(3, 14, 1);
    edge(&d, 14, 0, MS(8));
    edge(&d, 13, 0, MS(9));
    poll(&d, MS(20));
    assert(count == 6); expect(4, 14, 0); expect(5, 13, 0); /* release order, not key-code order */
    assert(d.pending == 0);
    count = 0;
    for (int k = 0; k < KEY_DEBOUNCE_KEYS; ++k) edge(&d, k, 1, MS(30));
    for (int k = 0; k < KEY_DEBOUNCE_KEYS; ++k) edge(&d, k, 0, MS(31));
    poll(&d, MS(36));
    assert(count == 2 * KEY_DEBOUNCE_KEYS && d.pending == 0);
    for (int k = 0; k < KEY_DEBOUNCE_KEYS; ++k) {
        expect((unsigned)k, k, 1);
        expect(KEY_DEBOUNCE_KEYS + (unsigned)k, k, 0);
    }
}

static void clock_wrap_and_bounds(void)
{
    key_debounce_t d = {0}; count = 0;
    uint32_t t = UINT32_MAX - MS(2);
    edge(&d, 62, 1, t - MS(20));
    edge(&d, 62, 0, t);
    poll(&d, t + MS(5) - 1);
    assert(count == 1);
    poll(&d, t + MS(5));
    assert(count == 2); expect(1, 62, 0);
    edge(&d, 0, 1, MS(10));
    edge(&d, 0, 0, MS(9));                      /* hold spanning almost a full counter period */
    poll(&d, MS(14));
    assert(count == 4); expect(3, 0, 0);
    edge(&d, -1, 1, MS(20));
    edge(&d, KEY_DEBOUNCE_KEYS, 1, MS(20));
    edge(&d, 256, 1, MS(20));
    assert(count == 4 && d.pending == 0);
}

static void firmware_adapter(void)
{
    count = 0; clock_now = 0;
    key_input_wait(7, 0);
    assert(waited_sem == 7 && waited == 0);     /* no periodic wakeups when idle */
    key_input_event(39, 1);
    clock_now = MS(1);
    key_input_event(39, 0);
    key_input_wait(7, 0);
    assert(waited == 0x80000001u && count == 1);
    clock_now = MS(6);
    queue_empty = 0;
    key_input_wait(7, 0);
    assert(waited == 0x80000001u && count == 1); /* drain queued edges first */
    queue_empty = 1;
    key_input_wait(7, 0);
    assert(waited == 0 && count == 2); expect(1, 39, 0); /* final off, no further key events */

    clock_now = MS(20);
    key_input_event(39, 1);
    for (int i = 0; i < 5; ++i) {             /* sequencer pulses bypass even on a physically held key */
        key_input_event(39 | KEY_SYNTHETIC, 1);
        key_input_event(39 | KEY_SYNTHETIC, 0);
    }
    assert(count == 13);
    for (unsigned i = 3; i < 13; i += 2) { expect(i, 39, 1); expect(i + 1, 39, 0); }
    key_input_event(39, 1);
    assert(count == 13);                       /* synthetic releases didn't reset physical state */
    key_input_event(39, 0);
    clock_now = MS(25);
    key_input_event(2 | KEY_SYNTHETIC, 1);
    assert(count == 15); expect(13, 39, 0); expect(14, 2, 1);
    key_input_event(2 | KEY_SYNTHETIC, 0);
    key_input_event(0x3F, 1);
    key_input_event(0x3F, 1);
    assert(count == 18); expect(16, 0x3F, 1); expect(17, 0x3F, 1);
    key_input_event(-1, 1);
    key_input_event(0x40, 1);
    key_input_event(0xFF, 1);
    key_input_event(0x100, 1);
    assert(count == 18);
    key_input_wait(7, 0);
    assert(waited == 0);
}

int main(void)
{
    bounce();
    short_and_repeated_notes();
    independent_keys();
    clock_wrap_and_bounds();
    firmware_adapter();
    puts("debounce: all tests passed");
    return 0;
}
