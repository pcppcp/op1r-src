/* Stock Tombola in short physics steps (seq/tombola.c). */
#ifndef TOMBOLA_H
#define TOMBOLA_H
#define TOMBOLA_STEP 26           /* ticks per advance: a normal frame (23..25 at 30 fps) is one stock advance */
#define TOMBOLA_MAX  102          /* R246's clamp in tombola_player_advance (0x66) */
void tombola_advance(void *player, int ticks);      /* Tombola player vtable 0x01911E04 slot 6 (advance) */
#endif
