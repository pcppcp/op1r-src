/* Physical key debounce. Use KEY_DEBOUNCE_INIT; all calls run in the key thread. */
#ifndef OP1_KEY_DEBOUNCE_H
#define OP1_KEY_DEBOUNCE_H

#include <stdint.h>

#define KEY_DEBOUNCE_KEYS 0x3F                 /* 0..0x3E; 0x3F is mixer automation */
#define KEY_DEBOUNCE_DEFAULT_MS 5u
#define KEY_DEBOUNCE_MAX_MS 30u
#define KEY_CYCLES_PER_MS 400000u             /* OP-1 CPU: 400 MHz */
#define KEY_DEBOUNCE_INIT { .interval_cycles = KEY_DEBOUNCE_DEFAULT_MS * KEY_CYCLES_PER_MS }
#define KEY_SYNTHETIC 0x80                    /* low queue byte; removed before dispatch */

typedef struct {
    uint32_t released_at[KEY_DEBOUNCE_KEYS];
    uint32_t release_window[KEY_DEBOUNCE_KEYS]; /* interval captured at each release */
    volatile uint32_t interval_cycles;         /* GUI reads setting; zero: unfiltered passthrough */
    uint8_t down[KEY_DEBOUNCE_KEYS];
    uint8_t releasing[KEY_DEBOUNCE_KEYS];
    unsigned pending;
} key_debounce_t;

typedef void (*key_emit_t)(int code, int pressed);

void key_debounce_event(key_debounce_t *d, int code, int pressed, uint32_t now, key_emit_t emit);
void key_debounce_poll(key_debounce_t *d, uint32_t now, key_emit_t emit);
void key_debounce_set_ms(key_debounce_t *d, unsigned ms, uint32_t now, key_emit_t emit);

/* Firmware entry points, called from hook.S. */
void key_input_event(int code, int pressed);
void key_input_wait(int semaphore, uint32_t timeout);
unsigned key_input_debounce_ms(void);
void key_input_set_debounce_ms(unsigned ms);

#endif
