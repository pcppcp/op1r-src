/* Physical key debounce. Zero-initialise once; all calls run in the key thread. */
#ifndef OP1_KEY_DEBOUNCE_H
#define OP1_KEY_DEBOUNCE_H

#include <stdint.h>

#define KEY_DEBOUNCE_KEYS 0x3F                 /* 0..0x3E; 0x3F is mixer automation */
#define KEY_DEBOUNCE_CYCLES (5u * 400000u)     /* 5 ms at the OP-1's 400 MHz */
#define KEY_SYNTHETIC 0x80                    /* low queue byte; removed before dispatch */

typedef struct {
    uint32_t released_at[KEY_DEBOUNCE_KEYS];
    uint8_t down[KEY_DEBOUNCE_KEYS];
    uint8_t releasing[KEY_DEBOUNCE_KEYS];
    unsigned pending;
} key_debounce_t;

typedef void (*key_emit_t)(int code, int pressed);

void key_debounce_event(key_debounce_t *d, int code, int pressed, uint32_t now, key_emit_t emit);
void key_debounce_poll(key_debounce_t *d, uint32_t now, key_emit_t emit);

/* Firmware entry points, called from hook.S. */
void key_input_event(int code, int pressed);
void key_input_wait(int semaphore, uint32_t timeout);

#endif
