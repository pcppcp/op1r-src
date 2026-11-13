/* Safe mode: help held at power-on -> this boot loads no plugin files and neither reads nor writes op1r's own data
 * (op1rsettings: defaults; tape markers; the MIDI sequencer file); help + metronome also clears op1rsettings. A note
 * on every page says so (screens/overlay.c). For data that stops the boot (a plugin, a full flash with a table to
 * create, a setting): the OP-1 starts, the user fixes it, the next normal boot is as before.
 * The decision is made at the first op1r code of the boot (boottime.c bt_db_open, CALL @0x019F02E0). R246's gpio init
 * (0x019AE5A8, CALL @0x019F0000) has read every register of the key expanders by then (MCP23017 at TWI 0x22..0x26,
 * as in crash.c) into its mirror at 0xFF907864: 25 bytes per address (0x20 + i), registers from +3 (callback
 * 0x019AE070; the key interrupt updates it). Key code c sits on expander 0x23 + c / 16, pin c % 16 (GPIOA 0..7,
 * GPIOB 8..15, active low): help = 0x00, metronome = 0x01. */
#pragma GCC optimize ("Os", "no-tree-loop-distribute-patterns")
#include "op1_synth.h"

#ifndef OP1_HOST
volatile uint8_t op1r_safe_boot;                    /* SB_ON | SB_RESET, set once before anything reads it */
#endif

/* k: GPIOA, GPIOB of 0x23..0x26. Only the key pins count (crash.c key_mask); exactly help, or help + metronome,
 * must be down: a failed read (all 0) or other keys held is a normal boot */
int sb_decide(const uint8_t *k)
{
    static const uint8_t mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F, 0x00 };
    uint8_t other = 0;
    for (int i = 1; i < 8; i++) other |= (uint8_t)(~k[i] & mask[i]);
    uint8_t a = (uint8_t)~k[0];
    if (other || (a & ~3) || !(a & 1)) return 0;
    return a & 2 ? SB_ON | SB_RESET : SB_ON;
}

#ifndef OP1_HOST
#define GPIO_MIRROR(dev, reg) (*(volatile uint8_t *)(0xFF907864u + ((dev) - 0x20) * 25 + 3 + (reg)))
#define GPIO_INIT  (*(volatile uint8_t *)0x01001D64)   /* gpio init ran (else it asserts) */
#define GPIO_EXP   (*(volatile uint8_t *)0x01001D65)   /* expanders 0x22..0x26 present (else R246 reads 0x20 only) */
#define DB_HANDLE  ((void *(*)(void))0x0196DD44)
void sb_boot(void)                                  /* boot thread, before db2::open */
{
    if (!GPIO_INIT || !GPIO_EXP) return;
    uint8_t k[8];
    for (int i = 0; i < 8; i++) k[i] = GPIO_MIRROR(0x23 + i / 2, 0x12 + (i & 1));
    op1r_safe_boot = (uint8_t)sb_decide(k);
}
void sb_after_db(void)                              /* boot thread, after db2::open */
{
    if (op1r_safe_boot & SB_RESET) op1_db_write(DB_HANDLE(), "DELETE FROM op1rsettings");   /* (no assert: no table) */
}
#endif
