#include "debounce.h"

static void release(key_debounce_t *d, unsigned code, key_emit_t emit)
{
    d->releasing[code] = d->down[code] = 0;
    --d->pending;
    emit((int)code, 0);
}

void key_debounce_poll(key_debounce_t *d, uint32_t now, key_emit_t emit)
{
    /* Emit matured releases oldest first, preserving modifier/key ordering when
     * several deadlines expire while the key thread is busy. Unsigned subtraction
     * handles CYCLES wrap; only pending releases have a live timestamp. */
    while (d->pending) {
        unsigned oldest = KEY_DEBOUNCE_KEYS;
        uint32_t age = 0;
        for (unsigned i = 0; i < KEY_DEBOUNCE_KEYS; ++i) {
            uint32_t elapsed = now - d->released_at[i];
            if (d->releasing[i] && (!d->interval_cycles || elapsed >= d->release_window[i]) &&
                (oldest == KEY_DEBOUNCE_KEYS || elapsed > age)) {
                oldest = i;
                age = elapsed;
            }
        }
        if (oldest == KEY_DEBOUNCE_KEYS) break;
        release(d, oldest, emit);
    }
}

void key_debounce_set_ms(key_debounce_t *d, unsigned ms, uint32_t now, key_emit_t emit)
{
    if (ms > KEY_DEBOUNCE_MAX_MS) ms = KEY_DEBOUNCE_MAX_MS;
    d->interval_cycles = ms * KEY_CYCLES_PER_MS;
    /* Turning OFF flushes deferred releases, but preserves keys still held.
     * Other changes affect only future releases, not an in-flight deadline. */
    if (!ms) key_debounce_poll(d, now, emit);
}

void key_debounce_event(key_debounce_t *d, int code, int pressed, uint32_t now, key_emit_t emit)
{
    if ((unsigned)code >= KEY_DEBOUNCE_KEYS) return;
    key_debounce_poll(d, now, emit);
    unsigned k = (unsigned)code;
    if (!d->interval_cycles) {
        d->down[k] = !!pressed;                    /* track holds for later re-enabling */
        emit(code, pressed);                       /* OFF also passes duplicate edges */
        return;
    }
    if (pressed) {
        if (d->releasing[k]) {
            d->releasing[k] = 0;                   /* contact bounced closed again */
            --d->pending;
        }
        if (!d->down[k]) {
            d->down[k] = 1;
            emit(code, 1);                         /* no added attack latency */
        }
    } else if (d->down[k] && !d->releasing[k]) {
        d->released_at[k] = now;
        d->release_window[k] = d->interval_cycles;
        d->releasing[k] = 1;
        ++d->pending;
    }
}
